// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/storage_error.hpp"

namespace ds::storage {

std::string_view toString(StorageErrorCode code) noexcept {
    switch (code) {
        case StorageErrorCode::DriverUnavailable: return "driver-unavailable";
        case StorageErrorCode::CannotOpen: return "cannot-open";
        case StorageErrorCode::DataDirectory: return "data-directory";
        case StorageErrorCode::Query: return "query";
        case StorageErrorCode::Transaction: return "transaction";
        case StorageErrorCode::Migration: return "migration";
        case StorageErrorCode::NewerSchema: return "newer-schema";
        case StorageErrorCode::InvalidValue: return "invalid-value";
        case StorageErrorCode::Stopped: return "stopped";
    }
    return "unknown";
}

}  // namespace ds::storage
