#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_esp32_xml_bindings.py: a name XML binds must be
# registered by a file the ESP32 firmware compiles, in a branch it compiles.

load helpers

GATE="scripts/check_esp32_xml_bindings.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    FW="$ROOT/firmware/helixscreen-esp32/components/helixapp"
    mkdir -p "$ROOT/src" "$ROOT/include" "$ROOT/ui_xml" "$FW"
    cat > "$FW/CMakeLists.txt" <<'CMAKE'
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HELIX_HAS_PLUGINS=0)
CMAKE
    echo "src/kept.cpp" > "$FW/app_srcs.txt"
    cat > "$ROOT/ui_xml/demo.xml" <<'XML'
<component><view>
  <x callback="on_row"><bind_flag_if_eq subject="row_available" flag="hidden" ref_value="0"/></x>
</view></component>
XML
}

run_gate() {
    run python3 "$GATE" --repo-root "$ROOT" --list
}

@test "a callback and subject registered only by an excluded file are reported" {
    cat > "$ROOT/src/excluded.cpp" <<'CPP'
void reg() {
    lv_xml_register_event_cb(nullptr, "on_row", on_row);
    lv_xml_register_subject(nullptr, "row_available", &row_available_);
}
CPP
    run_gate
    contains "callback:on_row" "$output"
    contains "subject:row_available" "$output"
}

@test "registrations in a listed file or a firmware stub stay silent" {
    cat > "$ROOT/src/kept.cpp" <<'CPP'
void reg() { lv_xml_register_event_cb(nullptr, "on_row", on_row); }
CPP
    cat > "$FW/stub.cpp" <<'CPP'
void reg() { lv_xml_register_subject(nullptr, "row_available", &row_available_); }
CPP
    run_gate
    [ "$status" -eq 0 ]
    lacks "on_row" "$output"
    lacks "row_available" "$output"
}

@test "a registration in a branch the firmware compiles out is reported" {
    cat > "$ROOT/src/kept.cpp" <<'CPP'
void reg() {
#if HELIX_HAS_PLUGINS
    lv_xml_register_event_cb(nullptr, "on_row", on_row);
#endif
    lv_xml_register_subject(nullptr, "row_available", &row_available_);
}
CPP
    run_gate
    contains "callback:on_row" "$output"
    lacks "row_available" "$output"
    sed -i 's/#endif/#else\n    lv_xml_register_event_cb(nullptr, "on_row", on_noop);\n#endif/' "$ROOT/src/kept.cpp"
    run_gate
    lacks "on_row" "$output"
}

@test "a name nothing registers anywhere is not this gate's finding" {
    run_gate
    lacks "on_row" "$output"
    lacks "row_available" "$output"
}

@test "the baseline fails a new finding and passes accepted debt" {
    cat > "$ROOT/src/excluded.cpp" <<'CPP'
void reg() { lv_xml_register_event_cb(nullptr, "on_row", on_row); }
CPP
    echo "callback:on_row" > "$ROOT/baseline.txt"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 0 ]
    echo 'void more() { lv_xml_register_subject(nullptr, "row_available", &s); }' >> "$ROOT/src/excluded.cpp"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 1 ]
    contains "subject:row_available" "$output"
}

@test "this tree holds its baseline" {
    run python3 "$GATE" --baseline scripts/esp32_xml_binding_baseline.txt
    [ "$status" -eq 0 ]
}
