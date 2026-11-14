// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup sim Simulation
/// @brief Main event loop, world orchestration and snapshots. (SPECIFICATION §4.1).

/// @file
/// @brief Module identification for `ds_sim`.
/// @ingroup sim

namespace ds::sim {

/// @brief Returns the name of this Core module ("sim").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::sim
