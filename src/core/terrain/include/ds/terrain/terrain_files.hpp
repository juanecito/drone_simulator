// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/json.hpp"
#include "ds/terrain/height_source.hpp"
#include "ds/terrain/orthophoto.hpp"
#include "ds/terrain/terrain_error.hpp"

/// @file
/// @brief Elevation and orthophoto files declared in a mission's `terrain` object
/// (§4.5.4, §4.5.5): parsing, path rules, size limits and integrity. Reading the files
/// (GDAL / PDAL) is done by the raster, lidar and orthophoto sources.
/// @ingroup terrain

namespace ds::terrain {

/// @brief Kind of the elevation files.
enum class ElevationFormat {
    Raster,      ///< COG, GeoTIFF, `.hgt`, `.asc`, `.img` (FR-TER-F01).
    PointCloud,  ///< LAS / LAZ (FR-TER-L01).
};

/// @brief One file reference: `path` plus optional `sha256`.
struct FileRef {
    std::string path;      ///< As written in the mission (relative or `library:<name>`).
    std::string sha256;    ///< Expected SHA-256 (lower case), empty if not given.
    std::string jsonPath;  ///< JSON pointer of the entry, for error messages.
};

/// @brief Interpolation used to rasterize lidar ground points (FR-TER-L03).
enum class LidarInterpolation { Tin, Idw };

/// @brief Mission terrain with `"source": "file"` (FR-TER-F02, FR-TER-I02).
struct FileTerrainSpec {
    ElevationFormat format = ElevationFormat::Raster;  ///< `format`: raster | las | laz.
    std::vector<FileRef> elevationFiles;               ///< At least one.
    std::string crs;                     ///< Required if the files carry no CRS (FR-TER-F05).
    std::string verticalDatum;           ///< e.g. EGM2008 (FR-TER-F06).
    std::string attribution;             ///< Shown like provider attribution (FR-TER-F10).
    std::optional<double> originLatDeg;  ///< Geographic origin (FR-TER-09).
    std::optional<double> originLonDeg;  ///< Geographic origin.
    std::optional<double> originAltM;    ///< Origin altitude.
    std::string originAltRef = "AGL";    ///< `AGL` or `AMSL`.
    std::optional<Extent> extent;        ///< Sub-area (FR-TER-F07).
    int band = 1;                        ///< Raster band (FR-TER-C01).
    // LAS/LAZ (FR-TER-L02 … L04)
    std::optional<double> resolutionM;  ///< Grid resolution; empty ⇒ automatic.
    LidarInterpolation interpolation = LidarInterpolation::Tin;  ///< Hole/grid interpolation.
    bool classifyGround = false;  ///< Run SMRF when no ground class exists.
    // Orthophotos (FR-TER-I01, I02)
    std::vector<FileRef> orthophotoFiles;  ///< Optional imagery.
    std::string orthophotoCrs;             ///< If the images carry no CRS.
    std::string orthophotoAttribution;     ///< Imagery attribution.
    std::vector<int> orthophotoBands;      ///< e.g. {1, 2, 3}; empty ⇒ automatic.
    OrthoColor orthophotoColor;            ///< `orthophotoColor` adjustments (S).
    // Online imagery instead of orthophoto files (FR-TER-O10).
    std::string imageryProvider;  ///< Imagery provider key; empty ⇒ none.
    std::string imageryDataset;   ///< Its layer; empty ⇒ the provider's default.
    /// Canonical paths of the elevation files, in order: filled by the mission loader when
    /// every file was found (not part of the JSON).
    std::vector<std::filesystem::path> resolvedElevationFiles;
    /// Canonical paths of the orthophotos, in order: filled by the mission loader when every
    /// one was found; empty otherwise, which disables the imagery (FR-TER-I08). At launch only
    /// files that pass their sha256 check are kept.
    std::vector<std::filesystem::path> resolvedOrthophotoFiles;
};

/// @brief Parses a mission `terrain` object with `"source": "file"`.
/// @param json The `terrain` object.
/// @param jsonPath Its JSON pointer (e.g. "/terrain"), prefixed to error messages.
/// @details Unknown properties are errors (except `x-…`). `surface` other than `"dtm"` is
/// NotSupported ("DSM is not supported in v1.0", FR-TER-L02); `interpolation: "max"` is only
/// meaningful for DSM and rejected likewise.
[[nodiscard]] TerrainResult<FileTerrainSpec> parseFileTerrain(
    const common::Json& json, std::string_view jsonPath = "/terrain");

/// @brief Folders where terrain files may be.
struct TerrainFileRoots {
    std::filesystem::path missionDir;  ///< Folder of the `.mission.json` (relative paths).
    std::filesystem::path libraryDir;  ///< `$HOME/.drone_simulation/library` (`library:`).
};

/// @brief Resolves @p reference by the rules of FR-TER-F03.
/// @details Relative paths are resolved in `missionDir`; `library:<name>` in `libraryDir`.
/// Absolute paths, backslashes, `..` segments, and paths whose real location (after following
/// symlinks) leaves the folder are PathRejected; a missing or non-regular file is FileNotFound.
/// @return The canonical path of the file.
[[nodiscard]] TerrainResult<std::filesystem::path> resolveTerrainPath(
    std::string_view reference, const TerrainFileRoots& roots);

/// @brief True for a folder reference: a path ending in `/` (such as `library:my-lidar/`),
/// which stands for every file of the right kind directly in that folder.
[[nodiscard]] bool isFolderReference(std::string_view reference) noexcept;

/// @brief The files a reference stands for: @p ref itself (unchecked) for a file, or, for a
/// folder reference, the regular files directly in the folder whose names @p accept takes,
/// sorted by name, each as `<folder reference><name>` with the entry's JSON path.
/// @details The folder follows the path rules of resolveTerrainPath(); files whose real
/// location leaves it are skipped. A missing folder, or one without such files, is
/// FileNotFound.
[[nodiscard]] TerrainResult<std::vector<FileRef>> expandFileRef(
    const FileRef& ref, const TerrainFileRoots& roots,
    const std::function<bool(const std::filesystem::path&)>& accept);

/// @brief Elevation format implied by a file name, or nullopt if unsupported (FR-TER-F01).
[[nodiscard]] std::optional<ElevationFormat> elevationFormatOf(const std::filesystem::path& file);

/// @brief True for orthophoto file names supported by FR-TER-I01 (`.tif`, `.tiff`, `.jp2`,
/// `.png`, `.jpg`, `.jpeg`).
[[nodiscard]] bool isOrthophotoFile(const std::filesystem::path& file);

/// @brief Size limits (FR-TER-F08, FR-TER-I08).
struct TerrainFileLimits {
    std::uint64_t maxRasterBytes = std::uint64_t{4} << 30U;       ///< 4 GB per terrain.
    std::uint64_t maxPointCloudBytes = std::uint64_t{20} << 30U;  ///< 20 GB per terrain.
    std::uint64_t maxOrthophotoBytes = std::uint64_t{50} << 30U;  ///< 50 GB per mission.
};

/// @brief A resolved and verified file.
struct ResolvedFile {
    std::filesystem::path path;   ///< Canonical path.
    std::uint64_t sizeBytes = 0;  ///< File size.
    std::string sha256;           ///< Computed hash.
};

/// @brief Result of prepareFileTerrain().
struct PreparedFileTerrain {
    std::vector<ResolvedFile> elevation;    ///< Elevation files, in mission order.
    std::vector<ResolvedFile> orthophotos;  ///< Usable orthophotos (empty if disabled).
    std::vector<std::string> warnings;      ///< e.g. why orthophotos were disabled.
    std::string terrainDataHash;            ///< FR-TER-F09 (orthophotos excluded, FR-TER-I09).
};

/// @brief Remembers file hashes by path, size and modification time, so a file is hashed
/// once per session (thread-safe).
class FileHashCache {
public:
    /// @brief The cached hash of @p file, if its size and time still match.
    [[nodiscard]] std::optional<std::string> find(const std::filesystem::path& file) const;
    /// @brief Stores @p sha256 for @p file as it is now.
    void store(const std::filesystem::path& file, std::string sha256);

private:
    struct Entry {
        std::uint64_t size = 0;
        std::filesystem::file_time_type time;
        std::string sha256;
    };
    mutable std::mutex mutex_;
    std::map<std::filesystem::path, Entry> entries_;
};

/// @brief Optional hooks of prepareFileTerrain().
struct PrepareHooks {
    /// Called after each file with the bytes hashed so far and the total (any thread).
    std::function<void(std::uint64_t done, std::uint64_t total)> progress;
    FileHashCache* cache = nullptr;  ///< Hashes known from earlier calls.
};

/// @brief Resolves, checks and hashes every file of @p spec.
/// @details Elevation problems are errors naming the JSON path. Orthophoto problems only
/// disable the imagery and add a warning (FR-TER-I08). Hashing reads every file once and can be
/// cancelled with @p stop.
[[nodiscard]] TerrainResult<PreparedFileTerrain> prepareFileTerrain(
    const FileTerrainSpec& spec, const TerrainFileRoots& roots,
    const TerrainFileLimits& limits = {}, std::stop_token stop = {},
    const PrepareHooks& hooks = {});

/// @brief Combined hash of the elevation inputs and the settings that change the heights
/// (FR-TER-F09): SHA-256 over the file hashes in order and a canonical text of the settings.
[[nodiscard]] std::string terrainDataHash(const std::vector<std::string>& fileHashes,
                                          const FileTerrainSpec& spec);

}  // namespace ds::terrain
