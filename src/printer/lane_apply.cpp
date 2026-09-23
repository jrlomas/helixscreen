// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_apply.h"

#include "filament_slot_override.h"

namespace helix::ams {

SlotStatus narrow_status(SlotStatus backend_status, bool present) {
    if (!present) {
        return SlotStatus::EMPTY;
    }
    if (backend_status == SlotStatus::EMPTY || backend_status == SlotStatus::UNKNOWN) {
        return SlotStatus::AVAILABLE;
    }
    return backend_status;
}

void apply_resolved(SlotInfo& slot, const ResolvedLane& resolved) {
    // A field no source observed is not written. The backend built this struct
    // out of what its firmware actually says, and several backends report
    // values the lane model has no producer for at all - Snapmaker's
    // print_task_config material and brand, a tool changer's tool name, a
    // weight the consumption tracker keeps. Writing an unobserved field would
    // replace those with blanks nobody stated.
    if (resolved.present.has_value()) {
        slot.status = narrow_status(slot.status, *resolved.present);
    }

    if (resolved.color_rgb.has_value())
        slot.color_rgb = *resolved.color_rgb;
    if (resolved.color_name.has_value())
        slot.color_name = *resolved.color_name;
    if (resolved.material.has_value())
        slot.material = *resolved.material;
    if (resolved.brand.has_value())
        slot.brand = *resolved.brand;
    if (resolved.spool_name.has_value())
        slot.spool_name = *resolved.spool_name;
    if (resolved.catalog_id.has_value())
        slot.catalog_id = *resolved.catalog_id;
    if (resolved.product_name.has_value())
        slot.product_name = *resolved.product_name;
    if (resolved.spoolman_id.has_value())
        slot.spoolman_id = *resolved.spoolman_id;
    if (resolved.spoolman_vendor_id.has_value())
        slot.spoolman_vendor_id = *resolved.spoolman_vendor_id;
    if (resolved.remaining_weight_g.has_value())
        slot.remaining_weight_g = *resolved.remaining_weight_g;
    if (resolved.total_weight_g.has_value())
        slot.total_weight_g = *resolved.total_weight_g;
}

void copy_resolver_owned_identity(SlotInfo& dst, const SlotInfo& src) {
    dst.color_rgb = src.color_rgb;
    dst.color_name = src.color_name;
    dst.material = src.material;
    dst.brand = src.brand;
    dst.spool_name = src.spool_name;
    dst.catalog_id = src.catalog_id;
    dst.product_name = src.product_name;
    dst.spoolman_id = src.spoolman_id;
    dst.spoolman_vendor_id = src.spoolman_vendor_id;
    dst.remaining_weight_g = src.remaining_weight_g;
    dst.total_weight_g = src.total_weight_g;
}

void clear_lane_only_identity(SlotInfo& slot, const FilamentSlotOverride* ovr) {
    // A field the override record carries is restated from it, not kept from
    // the struct: the struct's copy may be a record that has since been
    // dropped, and the override is the surviving statement of that value.
    if (ovr != nullptr && !ovr->brand.empty()) {
        slot.brand = ovr->brand;
    } else {
        slot.brand.clear();
    }
    if (ovr != nullptr && !ovr->spool_name.empty()) {
        slot.spool_name = ovr->spool_name;
    } else {
        slot.spool_name.clear();
    }
    if (ovr != nullptr && !ovr->catalog_id.empty()) {
        slot.catalog_id = ovr->catalog_id;
    } else {
        slot.catalog_id.clear();
    }
    if (ovr != nullptr && !ovr->product_name.empty()) {
        slot.product_name = ovr->product_name;
    } else {
        slot.product_name.clear();
    }
    if (ovr != nullptr && ovr->spoolman_vendor_id > 0) {
        slot.spoolman_vendor_id = ovr->spoolman_vendor_id;
    } else {
        slot.spoolman_vendor_id = 0;
    }
}

ResolvedLane resolved_lane(LaneId lane) {
    return resolve(lane_sources(lane));
}

} // namespace helix::ams
