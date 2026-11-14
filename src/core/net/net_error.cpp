// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/net/net_error.hpp"

namespace ds::net {

std::string_view toString(NetErrorCode code) noexcept {
    switch (code) {
        case NetErrorCode::InvalidRequest: return "invalid-request";
        case NetErrorCode::InsecureUrl: return "insecure-url";
        case NetErrorCode::MissingKey: return "missing-key";
        case NetErrorCode::Offline: return "offline";
        case NetErrorCode::QuotaExceeded: return "quota-exceeded";
        case NetErrorCode::CircuitOpen: return "circuit-open";
        case NetErrorCode::Cancelled: return "cancelled";
        case NetErrorCode::Timeout: return "timeout";
        case NetErrorCode::Connection: return "connection";
        case NetErrorCode::TooManyRedirects: return "too-many-redirects";
        case NetErrorCode::TooLarge: return "too-large";
        case NetErrorCode::ContentType: return "content-type";
        case NetErrorCode::HttpStatus: return "http-status";
        case NetErrorCode::InvalidPayload: return "invalid-payload";
        case NetErrorCode::Cache: return "cache";
    }
    return "unknown";
}

}  // namespace ds::net
