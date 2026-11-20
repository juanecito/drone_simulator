// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "ds/terrain/terrain_error.hpp"

#include <gdal.h>

/// @file
/// @brief Private GDAL helpers of core/terrain: one-time set-up (only the drivers of
/// FR-TER-F01 / FR-TER-I01, no network, no side-car files written), dataset handles and error
/// text. GDAL types never leave core/terrain's sources.

namespace ds::terrain::gdal {

/// @brief Drivers allowed for elevation rasters (FR-TER-F01).
inline constexpr std::array<const char*, 5> kElevationDrivers{"GTiff", "SRTMHGT", "AAIGrid", "HFA",
                                                              nullptr};

/// @brief Registers the allowed GDAL drivers once and configures GDAL (thread-safe).
/// @details Every other driver is deregistered, so GDAL's network drivers (WMS, HTTP…) are
/// gone; `.aux.xml` side-car files are never written into mission folders; GDAL errors go to
/// the log at debug level (callers turn them into TerrainErrors).
void ensureInitialised();

/// @brief Closes a dataset.
struct DatasetCloser {
    /// @brief GDALClose.
    void operator()(void* dataset) const noexcept;
};
/// @brief Owning dataset handle.
using DatasetPtr = std::unique_ptr<void, DatasetCloser>;

/// @brief Opens @p path read-only as a raster with one of @p drivers (null-terminated list).
/// @return The dataset, or nullptr (lastError() says why).
[[nodiscard]] DatasetPtr openRaster(const std::string& path, std::span<const char* const> drivers);

/// @brief The calling thread's last GDAL error message ("" if none).
[[nodiscard]] std::string lastError();

/// @brief Clears the calling thread's last GDAL error.
void clearError();

/// @brief A file name in GDAL's in-memory file system, unique in the process.
[[nodiscard]] std::string uniqueMemPath(std::string_view stem, std::string_view extension);

}  // namespace ds::terrain::gdal
