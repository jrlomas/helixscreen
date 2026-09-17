# Native OpenAMS preview for Raspberry Pi 64-bit

An unofficial test build from the `jrlomas/helixscreen` fork, including the native
OpenAMS backend proposed in [HelixScreen PR #1691](https://github.com/prestonbrown/helixscreen/pull/1691).
No AFC dependency. This is a prerelease, not an official HelixScreen release.

## Supported target

Raspberry Pi / BTT Pi with a **64-bit ARM Linux OS** (`uname -m` reports `aarch64`).
The package includes DRM/KMS and framebuffer display binaries, built against
Debian Bullseye's libraries using HelixScreen's normal Pi toolchain.
It is not for a 32-bit Pi OS, x86 PC, Android, or a printer's embedded screen.

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
2. Download the `.tar.gz` package, `helixscreen-pi.zip`, `install.sh`,
   `BUILD_INFO.txt`, and `SHA256SUMS` from this release into one new directory.
3. In that directory, verify the files:

   ```sh
   sha256sum -c SHA256SUMS
   ```

4. Use the same account/install location as your existing HelixScreen installation:

   ```sh
   sh ./install.sh --update --local "$PWD/helixscreen-pi.zip"
   ```

   `--update` preserves the existing HelixScreen configuration. Do not use `--clean`.
   For a fresh installation, omit `--update`.
5. Connect HelixScreen to the printer's existing Moonraker. The backend discovers
   `oams_manager` automatically. Its UI controls require the updated OpenAMS macros.

The bundled installer and normal update channels still point to upstream
HelixScreen. Always use `--local` for this preview. Installing an official update
can replace it and remove OpenAMS support until the PR is merged and released.

## Verification and limitations

Publication is gated on the installer/shell tests, a complete cross-build of both
display variants, ARM64/OpenAMS binary-content checks, archive checks, and SHA-256
checksums. `BUILD_INFO.txt` records the exact source commit and build run; source
is also available under this release's tag.

This workflow does **not** run the full C++ unit suite or validate a physical
printer. Treat the package as a preview and test discovery/status first, then
supervised load/unload operations on an idle printer.
