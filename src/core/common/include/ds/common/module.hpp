// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup common Common
/// @brief Math, logging, configuration, events, error types and JSON helpers shared by all modules.
/// (SPECIFICATION §3.2).

/// @file
/// @brief Module identification for `ds_common`.
/// @ingroup common

namespace ds::common {

/// @brief Returns the name of this Core module ("common").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::common
