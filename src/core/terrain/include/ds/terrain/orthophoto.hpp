// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "ds/geo/coordinates.hpp"
#include "ds/terrain/terrain_error.hpp"

/// @file
/// @brief Orthophoto files draped over real terrain (§4.5.5, FR-TER-I01 … I09): validation,
/// mosaic, reprojection into the scene's frame and image tiles by level of detail.
/// @ingroup terrain

namespace ds::terrain {

/// @brief Texels per side of an orthophoto tile.
inline constexpr int kOrthoTilePx = 256;
/// @brief Side of a level-0 orthophoto tile, metres (0.25 m texels).
/// @details Tiles share the terrain's chunk lattice: a chunk of level L (256 m · 2^L) holds
/// kOrthoTilesPerChunk × kOrthoTilesPerChunk tiles of level L.
inline constexpr double kOrthoTileSizeM = 64.0;
/// @brief Orthophoto tiles per side of a terrain chunk of the same level.
inline constexpr int kOrthoTilesPerChunk = 4;

/// @brief Address of an orthophoto tile: level and position on that level's grid.
/// @details Tile (level, ix, iz) covers x ∈ [ix·S, (ix+1)·S], z ∈ [iz·S, (iz+1)·S] with
/// S = orthoTileSize(level).
struct OrthoTileKey {
    int level = 0;        ///< Level of detail (0 = finest).
    std::int64_t ix = 0;  ///< Column (east).
    std::int64_t iz = 0;  ///< Row (south).

    /// @brief Ordering.
    auto operator<=>(const OrthoTileKey&) const = default;
    /// @brief The tile one level up that contains this one.
    [[nodiscard]] OrthoTileKey parent() const noexcept { return {level + 1, ix >> 1, iz >> 1}; }
};

/// @brief Hash for OrthoTileKey.
struct OrthoTileKeyHash {
    /// @brief Hash value.
    std::size_t operator()(const OrthoTileKey& key) const noexcept;
};

/// @brief Side of a tile of @p level, metres.
[[nodiscard]] double orthoTileSize(int level) noexcept;

/// @brief Colour adjustments of the imagery (FR-TER-I02, S).
struct OrthoColor {
    double brightness = 0.0;  ///< Added to every channel, −1 … 1.
    double contrast = 1.0;    ///< Scale around mid-grey, 0 … 4.
    double gamma = 1.0;       ///< Output = input^(1/gamma), 0.1 … 10.

    /// @brief True if the image is used as it is.
    [[nodiscard]] bool isIdentity() const noexcept {
        return brightness == 0.0 && contrast == 1.0 && gamma == 1.0;
    }
    /// @brief Equality.
    bool operator==(const OrthoColor&) const = default;
};

/// @brief An error message if @p color is out of range, else "".
[[nodiscard]] std::string validateOrthoColor(const OrthoColor& color);

/// @brief How to open orthophotos.
struct OrthoOptions {
    std::string crs;                     ///< CRS for files without one, e.g. "EPSG:25830".
    std::vector<int> bands;              ///< 3 (RGB) or 4 (RGBA) 1-based bands; empty ⇒ automatic.
    OrthoColor color;                    ///< Colour adjustments.
    int maxDimensionPx = 200'000;        ///< Largest image (or mosaic) side (FR-TER-I08).
    int tilePx = kOrthoTilePx;           ///< Texels per tile side (a power of two, 16 … 1024).
    double tileSizeM = kOrthoTileSizeM;  ///< Side of a level-0 tile.
};

/// @brief What the Terrain tab shows about the imagery (FR-TER-I10).
struct OrthoMetadata {
    std::string format;              ///< Driver(s), e.g. "GTiff".
    std::string crs;                 ///< Name of the CRS.
    std::string crsCode;             ///< "EPSG:25830" when known.
    std::string bands;               ///< "RGB", "RGBA", "grey", "bands 4, 3, 2", …
    int fileCount = 0;               ///< Files in the mosaic.
    int widthPx = 0;                 ///< Image (mosaic) width.
    int heightPx = 0;                ///< Image (mosaic) height.
    double resolutionM = 0.0;        ///< Ground size of a pixel.
    bool hasOverviews = false;       ///< Every file has internal overviews.
    double minX = 0.0;               ///< Coverage in the world frame (bounding box): west.
    double minZ = 0.0;               ///< North (smaller z).
    double maxX = 0.0;               ///< East.
    double maxZ = 0.0;               ///< South.
    std::vector<std::string> notes;  ///< Advice, e.g. about overviews.

    /// @brief True if the coverage box overlaps the rectangle [x0, x1] × [z0, z1].
    [[nodiscard]] bool overlaps(double x0, double z0, double x1, double z1) const noexcept {
        return x0 <= maxX && x1 >= minX && z0 <= maxZ && z1 >= minZ;
    }
};

/// @brief An RGBA8 image tile with its mipmaps, ready for the GPU.
/// @details Colours are sRGB with *premultiplied* alpha (so filtering and mipmaps blend
/// edges correctly). Alpha is 0 where there is no imagery (outside the coverage, NoData,
/// transparent pixels) and ramps up over two texels inside its border (FR-TER-I04).
struct OrthoTile {
    OrthoTileKey key;                ///< Address.
    int sizePx = kOrthoTilePx;       ///< Texels per side of mip level 0.
    std::vector<std::uint8_t> rgba;  ///< Mip levels 0, 1, … down to 1 × 1, packed.
    /// Unique number of this image in the process (a rebuilt tile gets a new one, so GPU
    /// caches know to upload it again).
    std::uint64_t serial = 0;

    /// @brief Number of mip levels.
    [[nodiscard]] int mipCount() const noexcept;
    /// @brief Byte offset of mip level @p mip in @ref rgba.
    [[nodiscard]] std::size_t mipOffset(int mip) const noexcept;
    /// @brief Bytes of a tile of @p sizePx with all its mip levels.
    [[nodiscard]] static std::size_t bytesFor(int sizePx) noexcept;
    /// @brief Memory use, bytes.
    [[nodiscard]] std::size_t byteSize() const noexcept { return sizeof(*this) + rgba.size(); }
};

/// @brief Outcome of building one tile.
struct OrthoBuild {
    /// The image, or nullptr if the tile has no imagery (so far).
    std::shared_ptr<const OrthoTile> tile;
    /// False while data for the tile is still arriving (online imagery): build it again once
    /// OrthoImagery::revision() has moved.
    bool complete = true;
};

/// @brief Imagery draped over the terrain, cut into tiles on the chunk lattice: orthophoto
/// files (OrthophotoSource) or an online tile service (OnlineImagerySource).
class OrthoImagery {
public:
    virtual ~OrthoImagery() = default;
    OrthoImagery(const OrthoImagery&) = delete;
    OrthoImagery& operator=(const OrthoImagery&) = delete;
    OrthoImagery(OrthoImagery&&) = delete;
    OrthoImagery& operator=(OrthoImagery&&) = delete;

    /// @brief Metadata of the imagery.
    [[nodiscard]] const OrthoMetadata& metadata() const noexcept { return metadata_; }
    /// @brief The options it was opened with.
    [[nodiscard]] const OrthoOptions& options() const noexcept { return options_; }
    /// @brief One line for the log.
    [[nodiscard]] const std::string& describe() const noexcept { return description_; }
    /// @brief False if tile @p key certainly holds no imagery (outside the coverage box).
    [[nodiscard]] bool mayCover(const OrthoTileKey& key) const noexcept;
    /// @brief Builds tile @p key (any thread). @p priority orders the downloads it starts
    /// (lower first), if any.
    [[nodiscard]] virtual OrthoBuild build(const OrthoTileKey& key, double priority) const = 0;
    /// @brief Raised whenever data arrives that incomplete builds were waiting for.
    [[nodiscard]] virtual std::uint64_t revision() const noexcept { return 0; }

protected:
    OrthoImagery() = default;

    OrthoOptions options_;     ///< Tile layout and colour adjustments.
    OrthoMetadata metadata_;   ///< Filled when opened.
    std::string description_;  ///< Filled when opened.
};

/// @brief One or more orthophoto files as a mosaic in the scene's local frame.
/// @details Files must share one CRS and band layout; several are mosaicked with an in-memory
/// GDAL VRT. Tiles are reprojected per texel (through a 17 × 17 lattice of exact
/// transformations, bilinear in between), so they line up with the elevation, which uses the
/// same frame. Each tile reads only the window it needs, at the overview level nearest its
/// texel size. Thread-safe; each thread reads through its own GDAL handle.
class OrthophotoSource final : public OrthoImagery {
public:
    /// @brief Opens and validates @p files (resolved and checked by prepareFileTerrain()) in
    /// the frame of the scene origin @p origin (latitude, longitude, AMSL altitude of world
    /// height 0).
    /// @return The source, or FileNotFound, UnsupportedFormat (format, 16-bit data, band
    /// layout), MissingCrs, MixedCrs, CorruptData (no georeferencing) or TooLarge.
    [[nodiscard]] static TerrainResult<std::shared_ptr<OrthophotoSource>> open(
        std::span<const std::filesystem::path> files, const OrthoOptions& options,
        const geo::Geodetic& origin);

    ~OrthophotoSource() override;
    OrthophotoSource(const OrthophotoSource&) = delete;
    OrthophotoSource& operator=(const OrthophotoSource&) = delete;
    OrthophotoSource(OrthophotoSource&&) = delete;
    OrthophotoSource& operator=(OrthophotoSource&&) = delete;

    /// @brief The image of tile @p key, or nullptr if it holds no imagery (any thread).
    [[nodiscard]] std::shared_ptr<const OrthoTile> tile(const OrthoTileKey& key) const;
    /// @brief tile(), always complete.
    [[nodiscard]] OrthoBuild build(const OrthoTileKey& key, double priority) const override;
    /// @brief Reads that failed since opening (damaged areas, shown without imagery).
    [[nodiscard]] std::uint64_t readErrors() const noexcept { return readErrors_.load(); }

private:
    struct Layout;
    struct Reader;
    OrthophotoSource();
    [[nodiscard]] TerrainResult<std::unique_ptr<Reader>> makeReader() const;
    [[nodiscard]] std::unique_ptr<Reader> acquire() const;
    void release(std::unique_ptr<Reader> reader) const;

    std::unique_ptr<Layout> layout_;
    mutable std::mutex readersMutex_;
    mutable std::vector<std::unique_ptr<Reader>> readers_;
    mutable std::atomic<std::uint64_t> readErrors_{0};
};

/// @brief Largest side of an image without overviews that is read directly; larger ones are
/// converted to a cached COG first (FR-TER-I06).
inline constexpr int kOrthoDirectMaxPx = 4096;

/// @brief True if @p file is a large image without internal overviews (or a format that
/// cannot be read by window efficiently), so a cached COG copy should be used. False for
/// unreadable files (open() reports them).
[[nodiscard]] bool orthoNeedsCache(const std::filesystem::path& file,
                                   int directMaxPx = kOrthoDirectMaxPx);

/// @brief Name of the cached COG of @p file: a SHA-256 of its path, size and modification
/// time. @return FileNotFound if the file is missing.
[[nodiscard]] TerrainResult<std::string> orthoCacheKey(const std::filesystem::path& file);

/// @brief Fraction done, 0 … 1 (any thread).
using OrthoProgress = std::function<void(double fraction)>;

/// @brief Writes @p file as a tiled COG with overviews to @p output (written beside it and
/// renamed when complete, so a cancelled or failed copy leaves nothing).
/// @return Cancelled, UnsupportedFormat or CorruptData on failure.
[[nodiscard]] TerrainResult<void> buildOrthoCog(const std::filesystem::path& file,
                                                const std::filesystem::path& output,
                                                std::stop_token stop = {},
                                                const OrthoProgress& progress = {});

}  // namespace ds::terrain
