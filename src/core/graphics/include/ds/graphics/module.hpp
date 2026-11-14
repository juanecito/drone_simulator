// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup graphics Graphics
/// @brief OpenGL 4.5 graphics engine. (SPECIFICATION §4.4).

/// @file
/// @brief Module identification for `ds_graphics`.
/// @ingroup graphics

namespace ds::graphics {

/// @brief Returns the name of this Core module ("graphics").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::graphics
