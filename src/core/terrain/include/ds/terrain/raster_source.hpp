// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ds/geo/coordinates.hpp"
#include "ds/terrain/height_source.hpp"
#include "ds/terrain/procedural_source.hpp"
#include "ds/terrain/terrain_error.hpp"

/// @file
/// @brief Elevation rasters (COG, GeoTIFF, `.hgt`, `.asc`, `.img`) as a height source
/// (§4.5.4, FR-TER-F01 … F08, FR-TER-C01/C02).
/// @ingroup terrain

namespace ds::terrain {

/// @brief How to open elevation rasters.
struct RasterOptions {
    int band = 1;               ///< Elevation band, 1-based (FR-TER-C01).
    std::string crs;            ///< CRS for files without one, e.g. "EPSG:25830" (FR-TER-F05).
    std::string verticalDatum;  ///< Overrides the file's vertical datum name (FR-TER-F06).
    /// Geographic origin; empty ⇒ the centre of the data (FR-TER-09).
    std::optional<geo::LatLon> origin;
    /// Altitude above mean sea level of world height 0; empty ⇒ the ground height at the
    /// origin (an AGL origin).
    std::optional<double> originAltitudeM;
    /// Sub-area, centred on the origin; it must lie inside the data (FR-TER-F07). Empty ⇒ the
    /// largest origin-centred rectangle the data covers (at most kMaxExtentM a side).
    std::optional<Extent> extent;
    /// Grid spacings up to this read the full-resolution data, so the finest mesh level and
    /// height queries see the same heights; coarser spacings read reduced resolution
    /// (overviews, or averages built while reading). Default: the default level-0 spacing.
    double fullDetailStepM = 2.0;
    int maxDimensionPx = 40'000;  ///< Largest raster (or mosaic) side (FR-TER-F08).
    /// Border treatment (FR-TER-O08): Cliff clamps the data at the edge (closed by a wall),
    /// Fade blends heights down to the lowest elevation of the data within the outer
    /// kFadeBandM (at most a tenth of the shorter side). Water is not supported.
    EdgeMode edge = EdgeMode::Cliff;
    /// Width of the fade band, metres.
    static constexpr double kFadeBandM = 500.0;
};

/// @brief What the metadata view shows about elevation data (FR-TER-O12).
struct SourceMetadata {
    std::string format;              ///< Driver(s), e.g. "GTiff".
    std::string crs;                 ///< Name of the horizontal CRS.
    std::string crsCode;             ///< "EPSG:32630" when known.
    std::string verticalDatum;       ///< e.g. "EGM2008", "" if unknown.
    std::string verticalUnit = "m";  ///< Unit in the file ("m", "ft", "US ft").
    int fileCount = 0;               ///< Files in the mosaic.
    int widthPx = 0;                 ///< Raster (mosaic) width.
    int heightPx = 0;                ///< Raster (mosaic) height.
    double resolutionM = 0.0;        ///< Ground size of a pixel (mean of x and y).
    bool hasOverviews = false;       ///< Internal overviews present.
    double minHeightM = 0.0;         ///< Lowest elevation, AMSL (from a reduced read).
    double maxHeightM = 0.0;         ///< Highest elevation, AMSL.
    double noDataPercent = 0.0;      ///< Share of NoData pixels (from a reduced read).
    geo::Geodetic origin{};          ///< Origin used; heightM is AMSL.
    Extent extent;                   ///< Terrain bounds in the world frame.
    /// Advice for the user, e.g. convert a large file without overviews to COG.
    std::vector<std::string> notes;
};

/// @brief One or more elevation rasters as a HeightSource, reprojected into the scene's local
/// frame (x east, z south, heights relative to the origin altitude).
/// @details Several files are mosaicked with a GDAL VRT (FR-TER-F04); they must share one CRS.
/// Every query reads only the part of the file it needs (FR-TER-C02) through a process-wide
/// GDAL block cache of 256 MB. Grids up to RasterOptions::fullDetailStepM read full-resolution
/// pixels, so they equal height() bit for bit; coarser grids read data averaged over aligned
/// blocks (from overviews when present), which agree between windows to float rounding.
/// Heights are bilinear between pixel centres. NoData pixels are filled
/// from valid pixels within 8 pixels (inverse-distance weights), otherwise with the lowest
/// elevation of the data (FR-TER-C01). Read errors in a damaged file count as NoData and are
/// counted (readErrors()). Thread-safe; each thread reads through its own GDAL handle.
class RasterSource final : public HeightSource {
public:
    /// @brief Opens and validates @p files (already resolved and checked by
    /// prepareFileTerrain()).
    [[nodiscard]] static TerrainResult<std::shared_ptr<RasterSource>> open(
        std::span<const std::filesystem::path> files, const RasterOptions& options = {});

    ~RasterSource() override;
    RasterSource(const RasterSource&) = delete;
    RasterSource& operator=(const RasterSource&) = delete;
    RasterSource(RasterSource&&) = delete;
    RasterSource& operator=(RasterSource&&) = delete;

    [[nodiscard]] double height(double x, double z) const override;
    void sampleGrid(double x0, double z0, double step, int cols, int rows,
                    std::span<double> out) const override;
    [[nodiscard]] std::optional<Extent> extent() const override { return metadata_.extent; }
    [[nodiscard]] std::string describe() const override;

    /// @brief Metadata of the data and the scene placement.
    [[nodiscard]] const SourceMetadata& metadata() const noexcept { return metadata_; }
    /// @brief Height above mean sea level at full resolution (no origin offset).
    [[nodiscard]] double heightAmsl(double x, double z) const;
    /// @brief Reads that failed since opening (damaged file areas, shown as gaps).
    [[nodiscard]] std::uint64_t readErrors() const noexcept { return readErrors_.load(); }

private:
    struct Reader;
    struct Layout;
    class Lease;
    RasterSource();

    [[nodiscard]] Lease acquire() const;
    void release(std::unique_ptr<Reader> reader) const;
    [[nodiscard]] TerrainResult<std::unique_ptr<Reader>> makeReader() const;
    // Heights AMSL of world points (xs[i], zs[i]) read with decimation factor @p d.
    void sampleAmsl(Reader& reader, std::span<const double> xs, std::span<const double> zs, int d,
                    std::span<double> out) const;
    [[nodiscard]] int decimationFor(double step) const noexcept;
    // Applies the edge treatment to a height AMSL at world (x, z).
    [[nodiscard]] double edged(double amsl, double x, double z) const noexcept;

    std::unique_ptr<Layout> layout_;
    SourceMetadata metadata_;
    std::string description_;
    double originAltitude_ = 0.0;
    double fallbackAmsl_ = 0.0;
    double fadeBandM_ = 0.0;  // 0 ⇒ no fading.
    mutable std::mutex readersMutex_;
    mutable std::vector<std::unique_ptr<Reader>> readers_;
    mutable std::atomic<std::uint64_t> readErrors_{0};
};

}  // namespace ds::terrain
