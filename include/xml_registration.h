// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file xml_registration.h
 * @brief XML component registration: a short eager list, the rest on first use
 *
 * @threading Main thread only; must complete before UI creation
 * @note Fonts and images are registered via AssetManager::register_fonts/images()
 */

#pragma once

namespace helix {

/**
 * @brief Register XML components from ui_xml/ directory
 *
 * Registers the components that must exist before their first use and
 * installs the loader for the rest.
 * Must be called after AssetManager initialization and theme init.
 */
void register_xml_components();

/**
 * @brief Register each XML component the first time anything names it
 *
 * Installs the engine's component loader: a tag, extends= base, lv_xml_create()
 * or lv_xml_component_get_scope() naming an unregistered component registers
 * ui_xml/<name>.xml or ui_xml/components/<name>.xml, layout variant applied.
 * register_xml_components() calls it; a test fixture that skips that calls it
 * directly.
 */
void register_xml_on_first_use();

/**
 * @brief Deinitialize XML-related subjects
 *
 * Must be called during shutdown before lv_deinit().
 * Called by StaticPanelRegistry.
 */
void deinit_xml_subjects();

} // namespace helix
