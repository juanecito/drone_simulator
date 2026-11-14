// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup physics Physics
/// @brief Vehicle models (aerial and ground), atmosphere, wind, battery, sensors, controllers and
/// collision. (SPECIFICATION §4.2).

/// @file
/// @brief Module identification for `ds_physics`.
/// @ingroup physics

namespace ds::physics {

/// @brief Returns the name of this Core module ("physics").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::physics
