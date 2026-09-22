// src/printer/snapmaker_exceptions.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_exceptions.h"

#include <array>
#include <cctype>

namespace helix::snapmaker {
namespace {

/// One fault we have wording for. Matched on (id, index, code); level is not
/// part of the identity, because the firmware may raise the same fault at a
/// different level depending on what it decides to do about it.
struct KnownException {
    int id;
    int index;
    int code;
    const char* message;
};

constexpr std::array<KnownException, 2> kKnown{{
    {530, 0, 11,
     "Remove the PEI sheet from the bed, then start again: probing through the sheet "
     "gives wrong results"},
    {531, 0, 16, "That setting cannot be changed while a print is running"},
}};

/// Is `s[at..at+3]` four digits?
bool four_digits(const std::string& s, size_t at) {
    if (at + 4 > s.size()) {
        return false;
    }
    for (size_t i = at; i < at + 4; ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i])) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<ExceptionCode> decode_exception_code(const std::string& text) {
    // Scan for NNNN-NNNN-NNNN-NNNN. Requiring all four groups is what keeps the
    // three-part basic form from decoding as a shifted four-part one.
    for (size_t i = 0; i + 19 <= text.size(); ++i) {
        if (!four_digits(text, i) || text[i + 4] != '-' || !four_digits(text, i + 5) ||
            text[i + 9] != '-' || !four_digits(text, i + 10) || text[i + 14] != '-' ||
            !four_digits(text, i + 15)) {
            continue;
        }
        // A fifth group means this is not the shape we think it is.
        if (i + 19 < text.size() && text[i + 19] == '-') {
            continue;
        }
        // Each field is exactly four validated digits, so the parses below are
        // bounded to 0..9999 and cannot throw.
        ExceptionCode c;
        c.level = std::stoi(text.substr(i, 4));
        c.id = std::stoi(text.substr(i + 5, 4));
        c.index = std::stoi(text.substr(i + 10, 4));
        c.code = std::stoi(text.substr(i + 15, 4));
        return c;
    }
    return std::nullopt;
}

std::string_view exception_message(const ExceptionCode& c) {
    for (const auto& k : kKnown) {
        if (k.id == c.id && k.index == c.index && k.code == c.code) {
            return k.message;
        }
    }
    return {};
}

ExceptionSeverity severity_of(int level) {
    switch (level) {
    case 1:
        return ExceptionSeverity::Informational;
    case 2:
        return ExceptionSeverity::Pause;
    default:
        return ExceptionSeverity::Cancel;
    }
}

} // namespace helix::snapmaker
