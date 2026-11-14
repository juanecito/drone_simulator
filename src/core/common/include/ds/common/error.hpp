// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <string>
#include <utility>

/// @file
/// @brief Common error type and `Result<T>` alias for recoverable errors (FR-X-02).
/// @ingroup common

namespace ds::common {

/// @brief A recoverable error with a user-readable message.
struct Error {
    std::string message;  ///< Human-readable description of what went wrong.
};

/// @brief Result of an operation that can fail: a value of type @p T or an Error.
template <typename T>
using Result = std::expected<T, Error>;

/// @brief Convenience factory for an unexpected Error.
/// @param message Human-readable description of the failure.
/// @return An `std::unexpected<Error>` usable as the return value of any `Result<T>`.
[[nodiscard]] inline std::unexpected<Error> makeError(std::string message) {
    return std::unexpected<Error>(Error{std::move(message)});
}

}  // namespace ds::common
