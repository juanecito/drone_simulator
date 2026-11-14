// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup net Network
/// @brief HTTP client, data providers, quota accounting and the disk cache. (SPECIFICATION §4.6).

/// @file
/// @brief Module identification for `ds_net`.
/// @ingroup net

namespace ds::net {

/// @brief Returns the name of this Core module ("net").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::net
