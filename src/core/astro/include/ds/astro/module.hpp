// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup astro Time & Astronomy
/// @brief Simulation clock ⇄ UTC and sun, moon, star and planet positions. (SPECIFICATION §4.7.1).

/// @file
/// @brief Module identification for `ds_astro`.
/// @ingroup astro

namespace ds::astro {

/// @brief Returns the name of this Core module ("astro").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::astro
