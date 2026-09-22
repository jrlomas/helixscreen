// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_fault_codes.h"

#include "printer_discovery.h"
#include "snapmaker_exceptions.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>

namespace helix::faultcodes {
namespace {

/// One firmware that reports faults as structured codes embedded in error
/// text. Everything vendor-specific lives in `classify_event`; this table
/// only carries the capability question and what to subscribe.
struct Provider {
    /// Status object whose presence marks the capability.
    const char* detect_object;
    /// Status objects carrying standing faults, for the subscription builder.
    std::vector<std::string> status_objects;
    /// Source tag for events this provider classifies.
    ErrorSource source;
    /// Read one error line (prefix already stripped) into an event, or
    /// nullopt when the line carries no code this firmware owns.
    std::optional<ErrorEvent> (*classify_event)(const Provider& p, const std::string& text);
};

ErrorSeverity severity_of_event(const snapmaker::ExceptionSeverity& s) {
    switch (s) {
    case snapmaker::ExceptionSeverity::Informational:
        return ErrorSeverity::INFO;
    case snapmaker::ExceptionSeverity::Pause:
        return ErrorSeverity::WARNING;
    case snapmaker::ExceptionSeverity::Cancel:
        return ErrorSeverity::CRITICAL;
    }
    return ErrorSeverity::CRITICAL;
}

/// The vendor's translation of `level-id-index-code` text into an event.
std::optional<ErrorEvent> classify_snapmaker(const Provider& p, const std::string& text) {
    const auto code = snapmaker::decode_exception_code(text);
    if (!code) {
        return std::nullopt;
    }
    ErrorEvent e;
    e.source = p.source;
    e.severity = severity_of_event(snapmaker::severity_of(code->level));
    e.code =
        fmt::format("{:04d}-{:04d}-{:04d}-{:04d}", code->level, code->id, code->index, code->code);
    e.raw_detail = text;
    // A fault we have no wording for still classifies, carrying the
    // firmware's own sentence rather than a fabricated one.
    if (const auto known = snapmaker::exception_message(*code); !known.empty()) {
        e.detail = std::string(known);
    } else {
        e.detail = text;
    }
    return e;
}

/// The provider table. Adding a firmware with the same capability is one row
/// here and no call-site change.
const std::array<Provider, 1> kProviders{{
    {"exception_manager", {"exception_manager"}, ErrorSource::SNAPMAKER, classify_snapmaker},
}};

const Provider* provider_for(const PrinterDiscovery& hw) {
    const auto& objects = hw.printer_objects();
    for (const auto& p : kProviders) {
        if (std::find(objects.begin(), objects.end(), p.detect_object) != objects.end()) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

bool firmware_reports_fault_codes(const PrinterDiscovery& hw) {
    return provider_for(hw) != nullptr;
}

std::vector<std::string> required_status_objects(const PrinterDiscovery& hw) {
    const Provider* p = provider_for(hw);
    return p ? p->status_objects : std::vector<std::string>{};
}

std::optional<ErrorEvent> classify(const PrinterDiscovery& hw, const std::string& line) {
    const Provider* p = provider_for(hw);
    if (!p) {
        return std::nullopt;
    }
    const GcodeErrorLine parsed = parse_gcode_error_line(line);
    if (parsed.prefix == GcodeErrorPrefix::None) {
        return std::nullopt;
    }
    return p->classify_event(*p, parsed.text);
}

} // namespace helix::faultcodes
