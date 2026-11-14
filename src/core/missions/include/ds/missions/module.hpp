// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup missions Missions
/// @brief Mission files, objectives, flight rules, failures and scoring. (SPECIFICATION §4.9).

/// @file
/// @brief Module identification for `ds_missions`.
/// @ingroup missions

namespace ds::missions {

/// @brief Returns the name of this Core module ("missions").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::missions
