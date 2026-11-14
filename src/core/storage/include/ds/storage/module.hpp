// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup storage Storage
/// @brief Per-user SQLite persistence through Qt SQL, migrations and the cache index.
/// (SPECIFICATION §4.10).

/// @file
/// @brief Module identification for `ds_storage`.
/// @ingroup storage

namespace ds::storage {

/// @brief Returns the name of this Core module ("storage").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::storage
