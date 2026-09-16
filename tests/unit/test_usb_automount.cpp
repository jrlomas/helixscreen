// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_automount.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include "usb_backend_linux.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

using helix::usb::UsbAutomount;
using TimePoint = std::chrono::steady_clock::time_point;

constexpr auto kGrace = std::chrono::milliseconds(3000);
const TimePoint kT0{std::chrono::seconds(1000)};

std::string call(const std::string& dev, const std::string& mnt, const std::string& fs,
                 const std::string& opts) {
    return dev + "|" + mnt + "|" + fs + "|" + opts;
}

// Records every syscall the automounter makes so assertions run against the
// decision layer, not against a real mount table.
struct FakeMountOps : helix::usb::MountOps {
    bool root = true;
    std::vector<std::string> candidates; // what a sysfs scan would return
    std::set<std::string> mounted;       // devices listed in the mount table
    std::set<std::string> present;       // devices sysfs still shows
    std::set<std::string> busy;          // mount points whose plain umount is EBUSY
    bool dir_result = true;

    // Which (fs, options) pairs this kernel accepts. Default: any vfat.
    std::function<bool(const std::string&, const std::string&)> accepts =
        [](const std::string& fs, const std::string&) { return fs == "vfat"; };

    std::vector<std::string> mount_calls;   // "dev|mnt|fs|opts"
    std::vector<std::string> unmount_calls; // "mnt|plain" or "mnt|lazy"

    bool is_root() override {
        return root;
    }
    std::vector<std::string> mounted_devices() override {
        return {mounted.begin(), mounted.end()};
    }
    std::vector<std::string> removable_block_devices() override {
        return candidates;
    }
    bool device_present(const std::string& device) override {
        return present.count(device) > 0;
    }
    bool ensure_mount_point_dir(const std::string&) override {
        return dir_result;
    }

    bool mount(const std::string& dev, const std::string& mnt, const std::string& fs,
               const std::string& opts) override {
        mount_calls.push_back(call(dev, mnt, fs, opts));
        return accepts(fs, opts);
    }

    bool unmount(const std::string& mnt, bool lazy) override {
        // Every attempt is recorded, including ones that fail with EBUSY, so
        // assertions can see the fallback sequence.
        unmount_calls.push_back(mnt + "|" + (lazy ? "lazy" : "plain"));
        return !(!lazy && busy.count(mnt) > 0); // EBUSY on plain umount
    }
};

struct AutomountFixture {
    FakeMountOps* ops = new FakeMountOps();
    std::unique_ptr<UsbAutomount> am;

    explicit AutomountFixture(std::chrono::milliseconds grace = std::chrono::milliseconds(100))
        : am(std::make_unique<UsbAutomount>(std::unique_ptr<helix::usb::MountOps>(ops), grace)) {}

    void expect_mounted(const std::string& dev) {
        // Mirror what a successful mount(2) does to the mount table so later
        // polls see the device as mounted.
        ops->mounted.insert(dev);
    }
};

size_t first_pos_holding(const std::vector<std::string>& entries, const std::string& needle) {
    auto it = std::find_if(entries.begin(), entries.end(), [&needle](const std::string& e) {
        return e.find(needle) != std::string::npos;
    });
    REQUIRE(it != entries.end());
    return static_cast<size_t>(it - entries.begin());
}

} // namespace

TEST_CASE("UsbAutomount option ladder prefers long names and kernel defaults", "[usb_automount]") {
    const auto ladder = helix::usb::automount_ladder();
    REQUIRE_FALSE(ladder.empty());

    // Read-only always: a stick yanked mid-write must not corrupt its FAT.
    for (const auto& attempt : ladder) {
        CAPTURE(attempt.fs_type, attempt.options);
        REQUIRE(attempt.options.compare(0, 3, "ro,") == 0);
    }

    // First attempt is plain kernel defaults.
    REQUIRE(ladder.front().fs_type == "vfat");
    REQUIRE(ladder.front().options == "ro,noatime");

    // Within vfat: plain, then utf8, then iso8859-1, codepage=437 last.
    const auto vfat_opts = [&ladder] {
        std::vector<std::string> opts;
        for (const auto& a : ladder) {
            if (a.fs_type == "vfat") {
                opts.push_back(a.options);
            }
        }
        return opts;
    }();
    REQUIRE(vfat_opts.size() == 4);
    REQUIRE(first_pos_holding(vfat_opts, "ro,noatime,iocharset=utf8") <
            first_pos_holding(vfat_opts, "ro,noatime,iocharset=iso8859-1"));
    REQUIRE(first_pos_holding(vfat_opts, "iocharset=iso8859-1") <
            first_pos_holding(vfat_opts, "codepage=437"));
}

TEST_CASE("UsbAutomount ladder orders filesystem families", "[usb_automount]") {
    const auto ladder = helix::usb::automount_ladder();

    auto first_index_of_fs = [&ladder](const std::string& fs) {
        auto it =
            std::find_if(ladder.begin(), ladder.end(),
                         [&fs](const helix::usb::MountAttempt& a) { return a.fs_type == fs; });
        REQUIRE(it != ladder.end());
        return static_cast<size_t>(it - ladder.begin());
    };

    REQUIRE(first_index_of_fs("vfat") < first_index_of_fs("msdos"));
    REQUIRE(first_index_of_fs("exfat") < first_index_of_fs("msdos"));
    REQUIRE(first_index_of_fs("ntfs3") < first_index_of_fs("msdos"));
}

TEST_CASE("UsbAutomount mount point sits inside the is_usb_mount prefixes", "[usb_automount]") {
    REQUIRE(helix::usb::automount_mount_point("/dev/sda1") == "/mnt/usb/sda1");
    REQUIRE(helix::usb::automount_mount_point("/dev/sda") == "/mnt/usb/sda");

    // The mount the automounter creates must be surfaced by the backend's
    // existing detection, not by a parallel path: one shared prefix
    // predicate, so a prefix change on either side reddens this test.
    REQUIRE(UsbBackendLinux::is_usb_mount_point(helix::usb::automount_mount_point("/dev/sda1")));
    REQUIRE(UsbBackendLinux::is_usb_mount_point(helix::usb::automount_mount_point("/dev/sda")));
    // is_usb_mount's sysfs half needs a /sys/block node no host has, so the
    // full classification is pinned by the [usb_backend][linux] tests.
}

TEST_CASE("UsbAutomount waits out the grace period before mounting", "[usb_automount]") {
    AutomountFixture fx{kGrace};
    fx.ops->candidates = {"/dev/sda1"};
    fx.ops->present = {"/dev/sda1"};

    fx.am->poll(kT0); // first sighting
    REQUIRE(fx.ops->mount_calls.empty());

    fx.am->poll(kT0 + std::chrono::milliseconds(1500)); // inside grace
    REQUIRE(fx.ops->mount_calls.empty());

    // A primary mounter winning the race cancels our intent entirely.
    fx.expect_mounted("/dev/sda1");
    fx.am->poll(kT0 + kGrace + std::chrono::milliseconds(100));
    REQUIRE(fx.ops->mount_calls.empty());
    REQUIRE(fx.am->our_mount_count() == 0);
}

TEST_CASE("UsbAutomount mounts an unmounted stick read-only after grace", "[usb_automount]") {
    AutomountFixture fx{kGrace};
    fx.ops->candidates = {"/dev/sda1"};
    fx.ops->present = {"/dev/sda1"};

    fx.am->poll(kT0);
    fx.am->poll(kT0 + kGrace + std::chrono::milliseconds(100));

    REQUIRE(fx.ops->mount_calls.size() == 1);
    REQUIRE(fx.ops->mount_calls[0] == call("/dev/sda1", "/mnt/usb/sda1", "vfat", "ro,noatime"));
    REQUIRE(fx.am->our_mount_count() == 1);
}

TEST_CASE("UsbAutomount never touches a device mounted anywhere else", "[usb_automount]") {
    AutomountFixture fx{kGrace};

    SECTION("mounted at someone else's mount point") {
        fx.ops->candidates = {"/dev/sda1", "/dev/sdb1"};
        fx.ops->present = {"/dev/sda1", "/dev/sdb1"};
        // udisks2 got sda1; only sdb1 is ours to mount.
        fx.ops->mounted = {"/dev/sda1"};

        fx.am->poll(kT0);
        fx.am->poll(kT0 + kGrace + std::chrono::milliseconds(100));

        for (const auto& c : fx.ops->mount_calls) {
            CAPTURE(c);
            REQUIRE(c.find("/dev/sda1|") != 0);
        }
        REQUIRE(fx.ops->mount_calls.size() == 1);
        REQUIRE(fx.ops->mount_calls[0].find("/dev/sdb1|/mnt/usb/sdb1|") == 0);
    }

    SECTION("mounted outside the usb prefixes") {
        // The check must read the whole table, not just our own mount points.
        fx.ops->candidates = {"/dev/sda1"};
        fx.ops->present = {"/dev/sda1"};
        fx.ops->mounted = {"/dev/sda1"}; // wherever it is mounted, it is mounted

        fx.am->poll(kT0);
        fx.am->poll(kT0 + std::chrono::minutes(1));
        REQUIRE(fx.ops->mount_calls.empty());
    }
}

TEST_CASE("UsbAutomount stays disarmed without root", "[usb_automount]") {
    // Arming is decided at construction against the ops' answer, so root must
    // be false before the automount is built.
    auto ops = std::make_unique<FakeMountOps>();
    ops->root = false;
    FakeMountOps* view = ops.get();
    UsbAutomount am(std::move(ops), kGrace);
    REQUIRE_FALSE(am.armed());

    view->candidates = {"/dev/sda1"};
    view->present = {"/dev/sda1"};

    am.poll(kT0);
    am.poll(kT0 + std::chrono::minutes(1));
    REQUIRE(view->mount_calls.empty());

    am.unmount_all();
    REQUIRE(view->unmount_calls.empty());
}

TEST_CASE("UsbAutomount caches the winning option combination", "[usb_automount]") {
    AutomountFixture fx{kGrace};
    // A kernel whose FAT defaults cannot mount plain vfat but accepts utf8.
    fx.ops->accepts = [](const std::string& fs, const std::string& opts) {
        return fs == "vfat" && opts.find("iocharset=utf8") != std::string::npos;
    };

    fx.ops->candidates = {"/dev/sda1"};
    fx.ops->present = {"/dev/sda1"};
    fx.am->poll(kT0);
    fx.am->poll(kT0 + kGrace + std::chrono::milliseconds(100));

    // Ladder order until the first success: plain vfat, then utf8.
    REQUIRE(fx.ops->mount_calls.size() == 2);
    REQUIRE(fx.ops->mount_calls[0] == call("/dev/sda1", "/mnt/usb/sda1", "vfat", "ro,noatime"));
    REQUIRE(fx.ops->mount_calls[1] ==
            call("/dev/sda1", "/mnt/usb/sda1", "vfat", "ro,noatime,iocharset=utf8"));
    fx.expect_mounted("/dev/sda1");

    // A second stick on the same kernel starts from the cached combination.
    fx.ops->candidates = {"/dev/sda1", "/dev/sdb1"};
    fx.ops->present.insert("/dev/sdb1");
    const auto t1 = kT0 + std::chrono::seconds(10);
    fx.am->poll(t1); // sdb1 first sighting
    fx.am->poll(t1 + kGrace + std::chrono::milliseconds(100));

    const auto sdb_calls =
        std::count_if(fx.ops->mount_calls.begin(), fx.ops->mount_calls.end(),
                      [](const std::string& c) { return c.find("/dev/sdb1|") == 0; });
    REQUIRE(sdb_calls == 1);
    REQUIRE(fx.ops->mount_calls.back() ==
            call("/dev/sdb1", "/mnt/usb/sdb1", "vfat", "ro,noatime,iocharset=utf8"));
}

TEST_CASE("UsbAutomount backs off after every option combination fails", "[usb_automount]") {
    AutomountFixture fx{kGrace};
    fx.ops->accepts = [](const std::string&, const std::string&) { return false; };
    fx.ops->candidates = {"/dev/sda1"};
    fx.ops->present = {"/dev/sda1"};

    fx.am->poll(kT0);
    fx.am->poll(kT0 + kGrace + std::chrono::milliseconds(100));
    const auto ladder_size = helix::usb::automount_ladder().size();
    REQUIRE(fx.ops->mount_calls.size() == ladder_size);
    REQUIRE(fx.am->our_mount_count() == 0);

    // Immediately after failure: no retry storm.
    fx.am->poll(kT0 + kGrace + std::chrono::seconds(2));
    REQUIRE(fx.ops->mount_calls.size() == ladder_size);

    // After the cooldown the full ladder runs again.
    fx.am->poll(kT0 + kGrace + std::chrono::seconds(31));
    REQUIRE(fx.ops->mount_calls.size() == 2 * ladder_size);
}

TEST_CASE("UsbAutomount unmounts its own stale mount when the device vanishes", "[usb_automount]") {
    AutomountFixture fx{kGrace};
    fx.ops->candidates = {"/dev/sda1"};
    fx.ops->present = {"/dev/sda1"};
    fx.am->poll(kT0);
    fx.am->poll(kT0 + kGrace + std::chrono::milliseconds(100));
    REQUIRE(fx.am->our_mount_count() == 1);
    fx.expect_mounted("/dev/sda1");

    // Stick yanked: sysfs node gone, but the mount table entry lingers.
    fx.ops->present.clear();
    fx.ops->candidates.clear();

    fx.am->poll(kT0 + std::chrono::seconds(30));
    REQUIRE(fx.ops->unmount_calls.size() == 1);
    REQUIRE(fx.ops->unmount_calls[0] == "/mnt/usb/sda1|plain");
    REQUIRE(fx.am->our_mount_count() == 0);
}

TEST_CASE("UsbAutomount falls back to a lazy unmount when the mount is busy", "[usb_automount]") {
    AutomountFixture fx{kGrace};
    fx.ops->candidates = {"/dev/sda1"};
    fx.ops->present = {"/dev/sda1"};
    fx.am->poll(kT0);
    fx.am->poll(kT0 + kGrace + std::chrono::milliseconds(100));
    fx.expect_mounted("/dev/sda1");
    fx.ops->busy = {"/mnt/usb/sda1"};

    fx.ops->present.clear();
    fx.ops->candidates.clear();
    fx.am->poll(kT0 + std::chrono::seconds(30));

    REQUIRE(fx.ops->unmount_calls.size() == 2);
    REQUIRE(fx.ops->unmount_calls[0] == "/mnt/usb/sda1|plain");
    REQUIRE(fx.ops->unmount_calls[1] == "/mnt/usb/sda1|lazy");
}

TEST_CASE("UsbAutomount unmounts only mounts it created", "[usb_automount]") {
    AutomountFixture fx{kGrace};
    fx.ops->candidates = {"/dev/sda1", "/dev/sdb1"};
    fx.ops->present = {"/dev/sda1", "/dev/sdb1"};
    fx.ops->mounted = {"/dev/sdb1"}; // someone else's mount
    fx.am->poll(kT0);
    fx.am->poll(kT0 + kGrace + std::chrono::milliseconds(100));
    REQUIRE(fx.am->our_mount_count() == 1); // only sda1

    SECTION("both devices vanish: only ours is unmounted") {
        fx.ops->present.clear();
        fx.ops->candidates.clear();
        fx.am->poll(kT0 + std::chrono::seconds(30));
        REQUIRE(fx.ops->unmount_calls.size() == 1);
        REQUIRE(fx.ops->unmount_calls[0] == "/mnt/usb/sda1|plain");
    }

    SECTION("clean shutdown: only ours is unmounted") {
        fx.am->unmount_all();
        REQUIRE(fx.ops->unmount_calls.size() == 1);
        REQUIRE(fx.ops->unmount_calls[0] == "/mnt/usb/sda1|plain");
        REQUIRE(fx.am->our_mount_count() == 0);
    }
}

TEST_CASE("UsbAutomount forgets a mount unmounted from outside", "[usb_automount]") {
    AutomountFixture fx{kGrace};
    fx.ops->candidates = {"/dev/sda1"};
    fx.ops->present = {"/dev/sda1"};
    fx.am->poll(kT0);
    fx.am->poll(kT0 + kGrace + std::chrono::milliseconds(100));
    REQUIRE(fx.am->our_mount_count() == 1);
    fx.expect_mounted("/dev/sda1");

    // An operator unmounts it over ssh: sysfs still shows the device, the
    // mount table no longer lists it.
    fx.ops->mounted.clear();

    fx.am->poll(kT0 + std::chrono::seconds(30));
    REQUIRE(fx.am->our_mount_count() == 0);
    REQUIRE(fx.ops->unmount_calls.empty()); // nothing of ours to unmount

    // The device stays a candidate, so it is mounted again - but through a
    // fresh grace period that started at the reap poll above, never instantly
    // off the stale first sighting.
    const auto before = fx.ops->mount_calls.size();
    fx.am->poll(kT0 + std::chrono::seconds(31)); // 1s after the reap: inside grace
    REQUIRE(fx.ops->mount_calls.size() == before);

    fx.am->poll(kT0 + std::chrono::seconds(34)); // fresh grace elapsed
    REQUIRE(fx.ops->mount_calls.size() == before + 1);
}

TEST_CASE("The test binary pins the automounter off before any test runs", "[usb_automount]") {
    // A root run of this binary (CI containers run as root) with a stick
    // attached must never issue real mount(2) calls against the host's
    // drives. test_main's startup constructor forces HELIX_USB_AUTOMOUNT=0
    // and create() refuses to arm under it, so this holds with or without a
    // fixture and whatever euid runs the suite.
    //
    // Deliberately fixture-less: the pin must hold for tests that construct
    // UsbBackendLinux directly.
    const char* pinned = ::getenv("HELIX_USB_AUTOMOUNT");
    REQUIRE(pinned != nullptr);
    REQUIRE(std::string(pinned) == "0");
    REQUIRE(helix::usb::UsbAutomount::create() == nullptr);
}

#endif // __linux__ && !__ANDROID__
