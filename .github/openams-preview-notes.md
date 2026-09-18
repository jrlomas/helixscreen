# Native OpenAMS preview for Raspberry Pi and Linux x86-64

An unofficial test build from the `jrlomas/helixscreen` fork, including the native
OpenAMS backend proposed in [HelixScreen PR #1691](https://github.com/prestonbrown/helixscreen/pull/1691).
No AFC dependency. This is a prerelease, not an official HelixScreen release.

## Changes in this preview

- Separate the four-spool routes near the HUB, including space for the wider glow.
- Stronger blue selection/glow for neutral filament, with a contrasting edge so
  black filament stays visible without changing its actual color.
- Continuous loaded paths through the drop/bend and from HUB to toolhead,
  removing doubled outlines and disconnected-looking joins.
- Correct OpenAMS error highlighting: healthy idle/loaded/loading/unloading
  states no longer appear as a nozzle fault. Reported faults remain highlighted.

The production panel was rendered locally in both dark and light modes using
disconnected OpenAMS test data. The local native build and 46 focused geometry,
rendering, and OpenAMS tests passed, plus both visual fixtures. This is not a
full C++ suite or hardware validation; local full-suite rebuilding was constrained
by available disk space. No Klipper configuration or printer commands were changed.

## Choose your package

| System | `uname -m` | Package key | ZIP package |
| --- | --- | --- | --- |
| Raspberry Pi / BTT Pi, 64-bit OS | `aarch64` | `pi` | `helixscreen-pi.zip` |
| Raspberry Pi, 32-bit ARM OS | `armv7l` | `pi32` | `helixscreen-pi32.zip` |
| Linux PC / mini PC, 64-bit Intel or AMD | `x86_64` | `x86` | `helixscreen-x86.zip` |

Choose for the installed OS, not just the CPU's capabilities. The ARM32 build
requires ARMv7 hard-float; it is not for the original Pi 1 / Pi Zero's ARMv6 CPU.
Each package includes DRM/KMS and framebuffer display binaries, built against
Debian Bullseye's libraries using HelixScreen's normal platform toolchain.
The x86 package is for a Linux touchscreen host, not a Windows/macOS application
or an SDL desktop-window build. These packages are not for Android or a printer's
embedded screen.

## Prepare OpenAMS

Update `OpenAMSOrg/klipper_openams` to master containing commit
[`c2dfe828473d423cf377e3dde440c808dd6c5eeb`](https://github.com/OpenAMSOrg/klipper_openams/commit/c2dfe828473d423cf377e3dde440c808dd6c5eeb)
or newer. Follow the
[existing-installation upgrade instructions](https://github.com/OpenAMSOrg/klipper_openams/blob/master/docs/UI_API.md#upgrading-existing-installations)
to merge the UI macro additions while keeping your calibrated settings and custom
printer routines. The OpenAMS installer does not overwrite existing macros.
Restart Klipper while the printer is idle after reviewing these changes.

## Install on an existing HelixScreen host

1. Back up your HelixScreen configuration and keep your previous installer/package
   available for rollback. Do this while the printer is idle.
2. Download the `.tar.gz` and `.zip` for your package key, `install.sh`,
   `BUILD_INFO-KEY.txt`, and `SHA256SUMS-KEY` from this release into one new
   directory, replacing `KEY` with `pi`, `pi32`, or `x86`.
3. In that directory, verify the files (64-bit Pi example):

   ```sh
   sha256sum -c SHA256SUMS-pi
   ```

4. Use the same account/install location as your existing HelixScreen installation:

   ```sh
   sh ./install.sh --update --local "$PWD/helixscreen-pi.zip"
   ```

   For Pi 32-bit or x86-64, use `helixscreen-pi32.zip` or `helixscreen-x86.zip`.
   `--update` preserves the existing HelixScreen configuration. Do not use `--clean`.
   For a fresh installation, omit `--update`.
5. Connect HelixScreen to the printer's existing Moonraker. The backend discovers
   `oams_manager` automatically. Its UI controls require the updated OpenAMS macros.

The bundled installer and normal update channels still point to upstream
HelixScreen. Always use `--local` for this preview. Installing an official update
can replace it and remove OpenAMS support until the PR is merged and released.

## Verification and limitations

Publication is gated on the installer/shell tests, a complete cross-build of both
display variants on all three architectures, architecture/OpenAMS binary-content
checks, archive checks, and SHA-256 checksums. `BUILD_INFO-KEY.txt` records the exact
source commit and build run; source
is also available under this release's tag.

This workflow does **not** run the full C++ unit suite or validate a physical
printer. Treat the package as a preview and test discovery/status first, then
supervised load/unload operations on an idle printer.
