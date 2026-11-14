// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup items Items
/// @brief Items manager: entity registry, spatial index and triggers. (SPECIFICATION §4.8).

/// @file
/// @brief Module identification for `ds_items`.
/// @ingroup items

namespace ds::items {

/// @brief Returns the name of this Core module ("items").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::items
