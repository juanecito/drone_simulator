// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup geo Geo-referencing
/// @brief WGS84 / ECEF / local ENU, UTM and Web-Mercator conversions. (SPECIFICATION §4.7).

/// @file
/// @brief Module identification for `ds_geo`.
/// @ingroup geo

namespace ds::geo {

/// @brief Returns the name of this Core module ("geo").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::geo
