#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

setup() {
    load helpers
    REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
}

@test "SDL fractional zoom preserves layout, resize behavior, mouse clicks and normalized touch" {
    command -v sdl2-config >/dev/null || skip "native SDL2 development files unavailable"
    local cc="${CC:-cc}" cxx="${CXX:-c++}"
    command -v "$cc" >/dev/null || skip "C compiler unavailable"
    command -v "$cxx" >/dev/null || skip "C++ compiler unavailable"
    local objects_dir="$REPO_ROOT/build/obj/lvgl"
    local font="$REPO_ROOT/build/obj/assets/fonts/noto_sans_14.o"
    [ -r "$objects_dir/src/drivers/sdl/lv_sdl_window.o" ] && [ -r "$font" ] \
        || skip "native application objects unavailable (build the SDL app first)"

    cd "$REPO_ROOT"
    local -a flags objects
    flags=(-D_GNU_SOURCE -DHELIX_DISPLAY_SDL -DHELIX_MAX_FONT_TIER=6 -DLV_CONF_INCLUDE_SIMPLE
           -I. -Iinclude -isystem lib -isystem lib/lvgl -isystem lib/lvgl/src
           -isystem lib/spdlog/include)
    local object
    while IFS= read -r -d '' object; do
        case "$object" in
            */lv_sdl_window.o|*/lv_sdl_mouse.o) continue ;;
        esac
        objects+=("$object")
    done < <(find "$objects_dir" -name '*.o' -print0)

    # Compile the changed drivers directly, so the regression cannot silently
    # use cached pre-fix objects. Everything else comes from the native build.
    local driver
    for driver in window mouse; do
        run "$cc" -std=c11 -O0 -g0 -fexceptions "${flags[@]}" $(sdl2-config --cflags) \
            -c "lib/lvgl/src/drivers/sdl/lv_sdl_${driver}.c" \
            -o "$BATS_TEST_TMPDIR/sdl_${driver}.o"
        [ "$status" -eq 0 ] || { echo "$output"; return 1; }
    done
    run "$cxx" -std=c++17 -O0 -g0 -DFMT_HEADER_ONLY "${flags[@]}" $(sdl2-config --cflags) \
        tests/standalone/sdl_display_scaling.cpp "$BATS_TEST_TMPDIR/sdl_window.o" \
        "$BATS_TEST_TMPDIR/sdl_mouse.o" "${objects[@]}" "$font" \
        $(sdl2-config --libs) -lm -lpthread -o "$BATS_TEST_TMPDIR/sdl_display_scaling"
    [ "$status" -eq 0 ] || { echo "$output"; return 1; }
    run "$BATS_TEST_TMPDIR/sdl_display_scaling"
    echo "$output"
    [ "$status" -eq 0 ]
}
