// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>

#include "ds/terrain/terrain_error.hpp"
#include "ds/terrain/terrain_files.hpp"

/// @file
/// @brief LAS / LAZ lidar point clouds turned into a bare-earth elevation model (DTM) written
/// as a Cloud-Optimised GeoTIFF (FR-TER-L01 … L06), read with LASzip.
/// @ingroup terrain

namespace ds::terrain {

/// @brief Largest empty area filled by interpolation (FR-TER-L03); larger gaps stay NoData.
inline constexpr double kLidarMaxHoleM = 50.0;
/// @brief Largest DTM grid, in cells (32 M cells ≈ 200 MB while rasterizing).
inline constexpr std::uint64_t kLidarMaxCells = 32'000'000;

/// @brief What the header of a LAS / LAZ file says.
struct LidarHeader {
    std::filesystem::path file;  ///< The file.
    std::string version;         ///< e.g. "1.4".
    int pointFormat = 0;         ///< Point data record format (0 … 10).
    bool compressed = false;     ///< LAZ.
    std::uint64_t points = 0;    ///< Number of points.
    double minX = 0.0;           ///< Bounds, in the file's CRS units.
    double minY = 0.0;           ///< Bounds.
    double maxX = 0.0;           ///< Bounds.
    double maxY = 0.0;           ///< Bounds.
    double minZ = 0.0;           ///< Bounds, in the file's vertical units.
    double maxZ = 0.0;           ///< Bounds.
    /// CRS from the GeoTIFF keys (`EPSG:<code>`) or the WKT record; empty if there is none.
    std::string crs;
    /// Vertical units to metres from the GeoTIFF keys (feet, US survey feet); nullopt ⇒ not
    /// stated (the horizontal units are used).
    std::optional<double> verticalToMetres;
};

/// @brief Rasterization settings (FR-TER-L02 … L04).
struct LidarOptions {
    /// Cell size in metres (at least 0.25); empty ⇒ automatic: about two point spacings,
    /// between 0.5 and 20 m, coarser if the grid would exceed @ref maxCells.
    std::optional<double> resolutionM;
    /// How empty cells up to @ref maxHoleM across are filled.
    LidarInterpolation interpolation = LidarInterpolation::Tin;
    /// Height of a cell from its ground points.
    LidarCellHeight cellHeight = LidarCellHeight::Mean;
    /// CRS of files without one (e.g. "EPSG:25831").
    std::string crs;
    /// Requested automatic ground classification (SMRF, FR-TER-L04 S): not available yet, so
    /// an unclassified cloud fails either way, with a message saying so.
    bool classifyGround = false;
    double maxHoleM = kLidarMaxHoleM;         ///< Largest filled gap.
    std::uint64_t maxCells = kLidarMaxCells;  ///< Largest grid.
};

/// @brief What a rasterization produced (also stored in the COG's metadata).
struct LidarSummary {
    std::uint64_t points = 0;        ///< Points read.
    std::uint64_t groundPoints = 0;  ///< Ground (class 2) and water (class 9) points used.
    std::uint64_t dropped = 0;       ///< Withheld and noise (classes 7, 18) points.
    double resolutionM = 0.0;        ///< Cell size.
    int widthPx = 0;                 ///< Grid columns.
    int heightPx = 0;                ///< Grid rows.
    std::uint64_t filledCells = 0;   ///< Empty cells filled by TIN / IDW.
    std::uint64_t noDataCells = 0;   ///< Cells left without a height.
    std::string crs;                 ///< CRS of the grid.
    std::string interpolation;       ///< "tin" or "idw".
    std::string cellHeight = "mean";  ///< "mean" or "lowest".
};

/// @brief Fraction done, 0 … 1 (any thread).
using LidarProgress = std::function<void(double fraction)>;

/// @brief Reads the header of @p file.
/// @return FileNotFound, UnsupportedFormat (not LAS / LAZ) or CorruptData on failure.
[[nodiscard]] TerrainResult<LidarHeader> readLidarHeader(const std::filesystem::path& file);

/// @brief Rasterizes @p files (one or more tiles of one CRS, FR-TER-F04) into a DTM COG at
/// @p output.
/// @details Points are streamed (never the whole cloud in memory). Withheld points and noise
/// (classes 7, 18) are dropped; ground (2) and water (9) points give the surface. A cell
/// holds the mean (or the lowest, LidarOptions::cellHeight) height of its ground points; empty
/// cells inside gaps up to LidarOptions::maxHoleM across are filled by TIN (Delaunay triangulation
/// of the gap's rim) or IDW; larger gaps and areas outside the data stay NoData. Heights are
/// written in metres. The COG is written next to @p output and renamed when complete, so a
/// cancelled or failed run leaves no file.
/// @return The summary, or MissingCrs, MixedCrs, NotSupported (geographic CRS, no ground
/// points), TooLarge, Cancelled, CorruptData, FileNotFound.
[[nodiscard]] TerrainResult<LidarSummary> rasterizeLidar(
    std::span<const std::filesystem::path> files, const LidarOptions& options,
    const std::filesystem::path& output, std::stop_token stop = {},
    const LidarProgress& progress = {});

/// @brief Name of the cached DTM of @p files with @p options: a SHA-256 of the files' paths,
/// sizes and modification times and of the options that change the result.
/// @details A changed file gives a new key, so a stale DTM is never used; the full content
/// hash (FR-TER-F09) is not needed to find the cache entry.
/// @return FileNotFound if a file is missing.
[[nodiscard]] TerrainResult<std::string> lidarDemKey(std::span<const std::filesystem::path> files,
                                                     const LidarOptions& options);

/// @brief The summary stored in a DTM written by rasterizeLidar().
[[nodiscard]] TerrainResult<LidarSummary> readLidarSummary(const std::filesystem::path& cog);

}  // namespace ds::terrain
