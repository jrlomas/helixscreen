// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Components register the first time something names them
// (helix::register_xml_on_first_use), so these tests point the asset root at a
// scratch ui_xml/ tree whose components nothing registers up front.

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/layout_manager_test_access.h"
#include "data_root_resolver.h"
#include "layout_manager.h"
#include "xml_hot_reloader.h"
#include "xml_registration.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

extern "C" {
#include "helix-xml/src/xml/lv_xml.h"
}

namespace fs = std::filesystem;

namespace {

/// Registry membership asked without lv_xml_component_get_scope(), which would load.
bool is_registered(const char* name) {
    struct Probe {
        const char* name;
        bool found;
    } probe{name, false};
    lv_xml_component_foreach(
        [](const char* n, void* ud) {
            auto* p = static_cast<Probe*>(ud);
            if (std::strcmp(n, p->name) == 0)
                p->found = true;
        },
        &probe);
    return probe.found;
}

std::string component_with_label(const std::string& text) {
    return "<component><view extends=\"lv_obj\"><lv_label name=\"probe_label\" text=\"" + text +
           "\"/></view></component>";
}

std::string label_text(lv_obj_t* root) {
    auto* label = lv_obj_find_by_name(root, "probe_label");
    return label ? lv_label_get_text(label) : "<no probe_label>";
}

class LazyXmlFixture : public XMLTestFixture {
  public:
    LazyXmlFixture()
        : saved_root_(helix::asset_root()),
          root_(fs::temp_directory_path() /
                ("helix_lazy_xml_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        fs::create_directories(root_ / "ui_xml" / "components");
        fs::create_directories(root_ / "ui_xml" / "portrait");
        helix::set_asset_root(root_.string());
    }

    ~LazyXmlFixture() override {
        helix::set_asset_root(saved_root_);
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    void write(const std::string& rel, const std::string& content) {
        std::ofstream(root_ / "ui_xml" / rel) << content;
    }

    /// Rewrite with a later mtime than the hot reloader recorded.
    void rewrite(const std::string& rel, const std::string& content) {
        auto path = root_ / "ui_xml" / rel;
        auto before = fs::last_write_time(path);
        write(rel, content);
        fs::last_write_time(path, before + std::chrono::seconds(2));
    }

    std::string xml_dir() const {
        return (root_ / "ui_xml").string();
    }

  private:
    std::string saved_root_;
    fs::path root_;
};

} // namespace

TEST_CASE_METHOD(LazyXmlFixture, "a component registers on its first create, with what it nests",
                 "[xml][lazy]") {
    write("lazy_probe_outer.xml", "<component><view extends=\"lv_obj\">"
                                  "<lazy_probe_inner name=\"nested\"/></view></component>");
    write("components/lazy_probe_inner.xml", component_with_label("inner"));
    REQUIRE_FALSE(is_registered("lazy_probe_outer"));
    REQUIRE_FALSE(is_registered("lazy_probe_inner"));

    auto* outer = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_outer", nullptr));

    REQUIRE(outer != nullptr);
    auto* nested = lv_obj_find_by_name(outer, "nested");
    REQUIRE(nested != nullptr);
    CHECK(label_text(nested) == "inner");
    CHECK(is_registered("lazy_probe_inner"));
}

TEST_CASE_METHOD(LazyXmlFixture, "a name with no XML file still fails to create", "[xml][lazy]") {
    CHECK(lv_xml_create(test_screen(), "lazy_probe_absent", nullptr) == nullptr);
    CHECK_FALSE(is_registered("lazy_probe_absent"));
}

TEST_CASE_METHOD(LazyXmlFixture, "first use registers the active layout variant's copy",
                 "[xml][lazy]") {
    write("lazy_probe_variant.xml", component_with_label("base"));
    write("portrait/lazy_probe_variant.xml", component_with_label("portrait"));
    auto& lm = helix::LayoutManager::instance();
    lm.set_override("portrait");
    lm.init(480, 800);

    auto* obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_variant", nullptr));

    REQUIRE(obj != nullptr);
    CHECK(label_text(obj) == "portrait");
}

TEST_CASE_METHOD(LazyXmlFixture, "a component registered on first use hot-reloads",
                 "[xml][lazy][hotreload]") {
    write("lazy_probe_reload.xml", component_with_label("v1"));
    write("lazy_probe_unopened.xml", component_with_label("v1"));
    REQUIRE(lv_xml_create(test_screen(), "lazy_probe_reload", nullptr) != nullptr);

    helix::XmlHotReloader hr;
    std::vector<std::string> reloaded;
    hr.set_after_reload_callback([&](const std::string& name) { reloaded.push_back(name); });
    hr.start({xml_dir()}, 60000);
    rewrite("lazy_probe_reload.xml", component_with_label("v2"));
    rewrite("lazy_probe_unopened.xml", component_with_label("v2"));
    hr.scan_and_reload();
    hr.stop();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(reloaded.size() == 2);
    auto* reloaded_obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_reload", nullptr));
    REQUIRE(reloaded_obj != nullptr);
    CHECK(label_text(reloaded_obj) == "v2");
    // Saved before anything ever created it: the reload registers the new copy.
    auto* unopened =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_unopened", nullptr));
    REQUIRE(unopened != nullptr);
    CHECK(label_text(unopened) == "v2");
}

TEST_CASE_METHOD(LVGLUITestFixture, "the eager components are registered before first use",
                 "[xml][lazy][ui_integration]") {
    // styles.xml resolves theme tokens at registration, so it registers at boot.
    CHECK(is_registered("styles"));
    // C++ writes responsive consts into these scopes at boot.
    for (const char* name : {"color_picker", "ams_edit_overlay"}) {
        INFO(name);
        REQUIRE(is_registered(name));
        CHECK(lv_xml_get_const(lv_xml_component_get_scope(name), "sv_size") != nullptr);
    }
    // Everything else waits for first use.
    CHECK_FALSE(is_registered("spool_wizard"));
}
