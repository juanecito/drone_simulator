// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// @brief Error type of real-terrain loading (FR-TER-F08, FR-TER-I08, FR-TER-O15). Terrain
/// files and payloads are untrusted input: every problem becomes a TerrainError, never a crash.
/// @ingroup terrain

namespace ds::terrain {

/// @brief What went wrong.
enum class TerrainErrorCode {
    InvalidSpec,        ///< The mission's terrain JSON is malformed.
    PathRejected,       ///< Absolute path, `..`, or a symlink leaving the allowed folder.
    FileNotFound,       ///< Missing or unreadable file.
    UnsupportedFormat,  ///< File extension / format not supported.
    MixedFormats,       ///< Rasters mixed with point clouds, or format ≠ declared format.
    MissingCrs,         ///< No CRS in the file and none in the mission.
    Sha256Mismatch,     ///< File does not match its declared sha256 (FR-TER-F09).
    TooLarge,           ///< Over the size limits of FR-TER-F08 / FR-TER-I08.
    NotSupported,       ///< Valid but not supported in this version (e.g. DSM).
    Cancelled,          ///< Cancelled by the user.
};

/// @brief A terrain failure; the message names the file and the JSON path where useful.
struct TerrainError {
    TerrainErrorCode code = TerrainErrorCode::InvalidSpec;  ///< Category.
    std::string message;                                    ///< Human-readable description.
};

/// @brief Result of a terrain operation.
template <typename T>
using TerrainResult = std::expected<T, TerrainError>;

/// @brief Convenience factory for a failed TerrainResult.
[[nodiscard]] inline std::unexpected<TerrainError> terrainError(TerrainErrorCode code,
                                                                std::string message) {
    return std::unexpected<TerrainError>(TerrainError{code, std::move(message)});
}

}  // namespace ds::terrain
