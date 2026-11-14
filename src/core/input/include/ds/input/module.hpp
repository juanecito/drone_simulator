// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup input Input
/// @brief RC transmitter / gamepad input and action mapping. (SPECIFICATION §4.3).

/// @file
/// @brief Module identification for `ds_input`.
/// @ingroup input

namespace ds::input {

/// @brief Returns the name of this Core module ("input").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::input
