// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// @brief Error type of the network module (FR-X-02). Messages never contain API keys
/// (FR-NET-07): URLs are redacted before they reach an error.
/// @ingroup net

namespace ds::net {

/// @brief What went wrong with a request.
enum class NetErrorCode {
    InvalidRequest,    ///< Bad URL, template or provider configuration.
    InsecureUrl,       ///< Plain HTTP to a provider not marked insecure (FR-NET-02).
    MissingKey,        ///< The provider needs an API key and none is set (FR-NET-07).
    Offline,           ///< Offline mode and the data is not cached (FR-NET-06).
    QuotaExceeded,     ///< Daily call budget used up; never retried (FR-TER-O17).
    CircuitOpen,       ///< Too many recent failures for this provider (FR-NET-03).
    Cancelled,         ///< Cancelled by the caller.
    Timeout,           ///< Connect or total timeout (FR-NET-02).
    Connection,        ///< DNS, connect, TLS or transfer failure.
    TooManyRedirects,  ///< More than the allowed redirects (FR-NET-02).
    TooLarge,          ///< Response larger than the provider's limit (FR-NET-02).
    ContentType,       ///< Response content type not allowed for the provider (FR-NET-02).
    HttpStatus,        ///< Non-success HTTP status (see NetError::httpStatus).
    InvalidPayload,    ///< Payload failed validation (FR-TER-O15).
    Cache,             ///< Disk cache or cache index failure.
};

/// @brief A network failure.
struct NetError {
    NetErrorCode code = NetErrorCode::Connection;  ///< Category.
    std::string message;                           ///< Human-readable, key-free description.
    long httpStatus = 0;                           ///< HTTP status if one was received.
};

/// @brief Result of a network operation.
template <typename T>
using NetResult = std::expected<T, NetError>;

/// @brief Convenience factory for a failed NetResult.
[[nodiscard]] inline std::unexpected<NetError> netError(NetErrorCode code, std::string message,
                                                        long httpStatus = 0) {
    return std::unexpected<NetError>(NetError{code, std::move(message), httpStatus});
}

/// @brief Name of an error code, e.g. "quota-exceeded".
[[nodiscard]] std::string_view toString(NetErrorCode code) noexcept;

}  // namespace ds::net
