// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup weather Weather
/// @brief Seeded weather generator: clouds, precipitation, fog, wind and timeline. (SPECIFICATION
/// §4.2.3).

/// @file
/// @brief Module identification for `ds_weather`.
/// @ingroup weather

namespace ds::weather {

/// @brief Returns the name of this Core module ("weather").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::weather
