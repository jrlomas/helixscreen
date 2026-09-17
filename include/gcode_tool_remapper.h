// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <iosfwd>
#include <map>
#include <string>
namespace helix {

class GcodeToolRemapper {
  public:
    // remap: logical tool index -> physical head index.
    //
    // Rewrites all three command families. Every line is transformed from its
    // OWN original text, so a swap (1<->2) does not chain and no line needs to
    // see any other. That is what lets the same rule run over a stream.
    //
    // Peak memory is one line, whatever the file's size, and the byte-for-byte
    // contract is the same as apply_to_string(): unmatched lines pass through
    // untouched and a final line with no trailing newline keeps it that way.
    // Returns the number of lines that changed.
    static size_t apply_to_stream(std::istream& in, std::ostream& out,
                                  const std::map<int, int>& remap);

    // The whole-file form of apply_to_stream(), for callers that already hold
    // the content. A file being printed goes through the stream instead.
    static std::string apply_to_string(const std::string& gcode, const std::map<int, int>& remap);
};

} // namespace helix
