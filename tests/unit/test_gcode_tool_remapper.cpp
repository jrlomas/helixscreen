// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gcode_tool_remapper.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "../catch_amalgamated.hpp"

static std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// Split into physical lines (drop the trailing empty element from a final newline).
static std::vector<std::string> to_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        lines.push_back(line);
    }
    return lines;
}

TEST_CASE("U1 remap rewrites all three command families", "[remap][gcode]") {
    std::map<int, int> remap = {{1, 2}}; // logical tool 1 -> physical head 2
    std::string in = slurp("assets/test_gcodes/u1_4color_ring.gcode");
    REQUIRE(!in.empty()); // fixture found
    std::string out = helix::GcodeToolRemapper::apply_to_string(in, remap);

    // body Tn: no bare "T1" line remains; T0 lines untouched
    CHECK(out.find("\nT1\n") == std::string::npos);
    CHECK(out.find("\nT0\n") != std::string::npos);

    // prestart family: every EXECUTABLE SM_PRINT_* command line has been rewritten
    // away from EXTRUDER=1. As with the temp family, a global substring scan is NOT
    // a valid contract -- the fixture's "; machine_start_gcode = ..." comment line
    // embeds the literal "SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1" inside a comment,
    // and a conservative remapper must not touch comments. So we assert per command
    // line (lines that actually START with SM_PRINT_*, not comment text).
    bool saw_auto_feed_0 = false;
    for (const auto& line : to_lines(out)) {
        if (line.rfind("SM_PRINT_AUTO_FEED EXTRUDER=", 0) == 0) {
            CHECK(line.rfind("SM_PRINT_AUTO_FEED EXTRUDER=1", 0) != 0);
        }
        if (line.rfind("SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=", 0) == 0) {
            CHECK(line.rfind("SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1", 0) != 0);
        }
        if (line.rfind("SM_PRINT_FLOW_CALIBRATE EXTRUDER=", 0) == 0) {
            CHECK(line.rfind("SM_PRINT_FLOW_CALIBRATE EXTRUDER=1", 0) != 0);
        }
        if (line.rfind("SM_PRINT_AUTO_FEED EXTRUDER=0", 0) == 0) {
            saw_auto_feed_0 = true; // head 0 untouched
        }
    }
    CHECK(saw_auto_feed_0);

    // temp family: no M104/M109 COMMAND line retains a "T1" tool token.
    // NOTE: a global out.find(" T1 ") scan is NOT a valid contract here -- the
    // captured fixture embeds tool tokens inside comments (e.g. the
    // "; machine_start_gcode = ... S0 T1 A0 ..." escaped block on one physical
    // line, and "M104 S220 T1 ; preheat T1 time: 30s" where the comment text
    // also contains "T1"). A conservative remapper must NOT rewrite comments,
    // so we assert the precise behavioral truth: the executable portion of every
    // M104/M109 line carries the remapped head, not the original.
    for (const auto& line : to_lines(out)) {
        if (line.rfind("M104", 0) != 0 && line.rfind("M109", 0) != 0) {
            continue;
        }
        std::string code = line.substr(0, line.find(';')); // strip trailing comment
        // strip trailing whitespace so " T1" at EOL is caught regardless of \r/spaces
        while (!code.empty() && std::isspace(static_cast<unsigned char>(code.back()))) {
            code.pop_back();
        }
        CHECK(code.find(" T1 ") == std::string::npos);
        if (code.size() >= 3) {
            CHECK(code.substr(code.size() - 3) != " T1"); // no tool token at end of command
        }
    }
    // and the remap target landed on at least one temp line
    CHECK(out.find("M109 S220 T2") != std::string::npos);
}

TEST_CASE("remap is collision-safe for a swap", "[remap][gcode]") {
    // each line mapped from its ORIGINAL index in a single pass: 1<->2 swap must not chain
    std::string in = "T1\nT2\nSM_PRINT_AUTO_FEED EXTRUDER=1\nSM_PRINT_AUTO_FEED EXTRUDER=2\n";
    std::map<int, int> remap = {{1, 2}, {2, 1}};
    std::string out = helix::GcodeToolRemapper::apply_to_string(in, remap);
    CHECK(out == "T2\nT1\nSM_PRINT_AUTO_FEED EXTRUDER=2\nSM_PRINT_AUTO_FEED EXTRUDER=1\n");
}

TEST_CASE("unmapped indices and unrelated lines are untouched", "[remap][gcode]") {
    std::string in = "T0\nT3\nG1 X10 Y10\nM104 S200 T0\n";
    std::map<int, int> remap = {{1, 2}}; // nothing matches
    CHECK(helix::GcodeToolRemapper::apply_to_string(in, remap) == in);
}

TEST_CASE("temp token remapped in all positions", "[remap][gcode]") {
    std::map<int, int> remap = {{1, 2}};
    // token mid-line, token at EOL, token before comment
    std::string in = "M104 T1 S140\n"
                     "M109 S220 T1\n"
                     "M104 S70 T1 ; set nozzle temperature ;cooldown\n";
    std::string out = helix::GcodeToolRemapper::apply_to_string(in, remap);
    CHECK(out == "M104 T2 S140\n"
                 "M109 S220 T2\n"
                 "M104 S70 T2 ; set nozzle temperature ;cooldown\n");
}

TEST_CASE("comment lines containing tool tokens are not rewritten", "[remap][gcode]") {
    std::map<int, int> remap = {{1, 2}};
    std::string in = "; Change Tool1 -> Tool0\n"
                     "; machine_start_gcode = ...M104 S0 T1 A0...\n"
                     "G28\n";
    // none of these are bare-Tn / SM_PRINT_ / M10x command lines -> unchanged
    CHECK(helix::GcodeToolRemapper::apply_to_string(in, remap) == in);
}

// ---------------------------------------------------------------------------
// Streaming form: the production rewrite path. Peak memory is one line, so the
// oracle below is what guarantees a 400MB job gets the same bytes a 4KB one does.
// ---------------------------------------------------------------------------

namespace {
std::string stream_remap(const std::string& in_text, const std::map<int, int>& remap,
                         size_t* changed_out = nullptr) {
    std::istringstream in(in_text);
    std::ostringstream out;
    size_t changed = helix::GcodeToolRemapper::apply_to_stream(in, out, remap);
    if (changed_out != nullptr) {
        *changed_out = changed;
    }
    return out.str();
}
} // namespace

TEST_CASE("streaming a real file on disk rewrites every changed line and nothing else",
          "[remap][gcode][stream]") {
    // Through std::ifstream/std::ofstream, not stringstreams: the print path
    // rewrites file-to-file, and a real file is where binary mode, buffering
    // and the final-line EOF differ from an in-memory stream.
    std::map<int, int> remap = {{1, 2}};
    const std::string src = "assets/test_gcodes/u1_4color_ring.gcode";
    std::string original = slurp(src);
    REQUIRE(!original.empty());

    const auto out_path = std::filesystem::temp_directory_path() / "helix_stream_remap_test.gcode";
    size_t changed = 0;
    {
        std::ifstream in(src, std::ios::binary);
        std::ofstream out(out_path, std::ios::binary);
        REQUIRE(in);
        REQUIRE(out);
        changed = helix::GcodeToolRemapper::apply_to_stream(in, out, remap);
    }
    std::string rewritten = slurp(out_path.string());
    std::error_code ec;
    std::filesystem::remove(out_path, ec);

    // The rewrite is line-for-line, so the file keeps its shape whatever else
    // changed. A count that drifts from the line count means a line was
    // dropped or split.
    CHECK(to_lines(rewritten).size() == to_lines(original).size());
    CHECK(changed > 0);

    // Every executable line that named tool 1 now names tool 2, and every line
    // that named neither is byte-identical to where it started.
    auto orig_lines = to_lines(original);
    auto new_lines = to_lines(rewritten);
    REQUIRE(orig_lines.size() == new_lines.size());
    size_t differing = 0;
    for (size_t i = 0; i < orig_lines.size(); ++i) {
        if (orig_lines[i] != new_lines[i]) {
            ++differing;
            INFO("line " << (i + 1) << ": '" << orig_lines[i] << "' -> '" << new_lines[i] << "'");
            // A changed line only ever moves 1 -> 2; nothing else may move.
            CHECK(orig_lines[i].find('1') != std::string::npos);
            CHECK(new_lines[i].find('2') != std::string::npos);
        }
    }
    CHECK(differing == changed);
    CHECK(new_lines[0] == orig_lines[0]); // the header comment is untouched
}

TEST_CASE("streaming rewrite preserves a missing final newline", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    // A last line carrying no newline must not gain one: the slicer footer this
    // file ends in is parsed by byte offset, and a stray byte moves all of it.
    CHECK(stream_remap("G28\nT1", remap) == "G28\nT2");
    CHECK(stream_remap("G28\nT1\n", remap) == "G28\nT2\n");
    CHECK(stream_remap("T1", remap) == "T2");
    CHECK(stream_remap("", remap).empty());
    CHECK(stream_remap("\n", remap) == "\n");
}

TEST_CASE("streaming rewrite preserves CRLF line endings", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    // The \r belongs to the line, not to the separator. A rewritten line that
    // dropped it would silently change every byte offset after it.
    CHECK(stream_remap("G28\r\nT1\r\nG1 X1\r\n", remap) == "G28\r\nT2\r\nG1 X1\r\n");
}

TEST_CASE("multi-digit tool numbers are remapped, not truncated", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{10, 3}, {2, 11}};
    CHECK(stream_remap("T10\nT2\nT1\n", remap) == "T3\nT11\nT1\n");
    // T1 is a different tool from T10 and must not be caught by a prefix match.
    CHECK(stream_remap("M109 S220 T10\n", remap) == "M109 S220 T3\n");
}

TEST_CASE("streaming rewrite counts only the lines it changed", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    size_t changed = 99;
    stream_remap("T0\nT1\nG1 X1\nM109 S220 T1\n", remap, &changed);
    CHECK(changed == 2);

    // A count of zero is the signal the print path uses to skip the temp copy
    // and print the original, so an identity remap has to reach it.
    changed = 99;
    stream_remap("T0\nT1\nG1 X1\n", {{1, 1}}, &changed);
    CHECK(changed == 0);

    // So does a remap naming a tool the file never uses.
    changed = 99;
    stream_remap("T0\nG1 X1\n", {{4, 5}}, &changed);
    CHECK(changed == 0);
}

TEST_CASE("near-miss tokens are left alone", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    // Each of these is a corrupted print if the match is ever loosened: a
    // parameterised toolchange, a macro whose name starts with T, a temperature
    // line with no tool token, and a filename that contains one.
    CHECK(stream_remap("T1X\n", remap) == "T1X\n");
    CHECK(stream_remap("TOOL\n", remap) == "TOOL\n");
    CHECK(stream_remap("M104 S200\n", remap) == "M104 S200\n");
    CHECK(stream_remap("; printing T1_bracket.gcode\n", remap) == "; printing T1_bracket.gcode\n");
}
