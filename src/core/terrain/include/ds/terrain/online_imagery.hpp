// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ds/geo/coordinates.hpp"
#include "ds/geo/web_mercator.hpp"
#include "ds/net/data_client.hpp"
#include "ds/net/provider.hpp"
#include "ds/terrain/orthophoto.hpp"
#include "ds/terrain/terrain_error.hpp"

/// @file
/// @brief Online imagery from tile services (FR-TER-O10): XYZ, TMS, WMTS (Web-Mercator tile
/// matrix set) and WMS, reprojected and draped like orthophoto files.
/// @ingroup terrain

namespace ds::terrain {

/// @brief What the imagery comes from.
struct OnlineImageryRequest {
    net::ProviderConfig provider;  ///< Imagery provider (tile protocol).
    std::string dataset;           ///< Dataset (layer) id; empty ⇒ the provider's default.
    OrthoColor color;              ///< Colour adjustments.
};

/// @brief Tuning of the online imagery.
struct OnlineImageryOptions {
    /// Decoded service tiles kept in memory (the compressed ones stay in the disk cache).
    std::size_t memoryBytes = std::size_t{96} << 20U;
    /// Wait before a tile that failed (network, offline, quota) is asked for again.
    std::chrono::seconds retryAfter{60};
    /// Service tiles requested at once; further tiles wait for these.
    int maxPending = 64;
    /// Service tiles one orthophoto tile may need; beyond that (a level far coarser than the
    /// service's coarsest zoom) the tile shows the materials.
    int maxTilesPerBuild = 36;
};

/// @brief State of the online imagery, for the UI.
struct OnlineImageryStatus {
    std::size_t memoryTiles = 0;   ///< Decoded service tiles in memory.
    std::size_t pending = 0;       ///< Service tiles being fetched.
    std::uint64_t downloaded = 0;  ///< Service tiles received from the network.
    std::uint64_t fromCache = 0;   ///< Service tiles read from the disk cache.
    std::uint64_t missing = 0;     ///< Tiles the service does not have (404, empty, corrupt).
    std::uint64_t failed = 0;      ///< Failed fetches (retried later).
    std::string lastError;         ///< Latest failure ("" if none), e.g. a missing API key.
};

/// @brief Hash for geo::TileId.
struct TileIdHash {
    /// @brief Hash value.
    std::size_t operator()(const geo::TileId& tile) const noexcept;
};

/// @brief Imagery of an online tile service over the terrain (FR-TER-O10).
/// @details Each orthophoto tile is built from the service tiles of the zoom level whose
/// pixels are just finer than its texels (within the dataset's zoom range), reprojected from
/// Web Mercator into the scene's local frame through the same 17 × 17 lattice as the files.
/// Service tiles come from memory, else from the disk cache, else they are fetched in the
/// background through the DataClient (cache, keys, quota and offline rules); a build that
/// still waits for some is incomplete and the streamer repeats it once revision() moves.
/// Tiles the service does not have (404, empty or undecodable answers) show the terrain
/// materials. Payloads are decoded in memory with GDAL and must be square tiles of the
/// provider's size (FR-TER-O15). Thread-safe.
class OnlineImagerySource final : public OrthoImagery,
                                  public std::enable_shared_from_this<OnlineImagerySource> {
public:
    /// @brief Opens the imagery of @p request in the frame of the scene origin @p origin
    /// (latitude, longitude, AMSL altitude of world height 0). @p client must outlive the
    /// source. Nothing is fetched until tiles are built.
    /// @return The source, or ProviderError (not an imagery provider, unknown dataset) or
    /// InvalidSpec (colour, tile layout, origin beyond Web Mercator).
    [[nodiscard]] static TerrainResult<std::shared_ptr<OnlineImagerySource>> open(
        net::DataClient& client, const OnlineImageryRequest& request, const geo::Geodetic& origin,
        const OnlineImageryOptions& options = {}, const OrthoOptions& tiles = {});

    /// @brief Cancels the fetches still running.
    ~OnlineImagerySource() override;
    OnlineImagerySource(const OnlineImagerySource&) = delete;
    OnlineImagerySource& operator=(const OnlineImagerySource&) = delete;
    OnlineImagerySource(OnlineImagerySource&&) = delete;
    OnlineImagerySource& operator=(OnlineImagerySource&&) = delete;

    [[nodiscard]] OrthoBuild build(const OrthoTileKey& key, double priority) const override;
    [[nodiscard]] std::uint64_t revision() const noexcept override;

    /// @brief Zoom level of the service tiles used for orthophoto tiles of @p level.
    [[nodiscard]] int zoomFor(int level) const noexcept;
    /// @brief The provider.
    [[nodiscard]] const net::ProviderConfig& provider() const noexcept { return provider_; }
    /// @brief The dataset (layer) in use.
    [[nodiscard]] const net::DatasetConfig& dataset() const noexcept { return dataset_; }
    /// @brief Current state.
    [[nodiscard]] OnlineImageryStatus status() const;

private:
    struct Private {};

public:
    /// @brief Use open().
    OnlineImagerySource(Private, net::DataClient& client, net::ProviderConfig provider,
                        net::DatasetConfig dataset, const OnlineImageryOptions& options);

private:
    using Clock = std::chrono::steady_clock;
    // A decoded service tile: RGBA8, size × size.
    struct Image {
        int size = 0;
        std::vector<std::uint8_t> rgba;
    };
    enum class TileState { Ready, Missing, Waiting };
    struct Lookup {
        TileState state = TileState::Missing;
        std::shared_ptr<const Image> image;
    };

    // The tile from memory, the disk cache or (started here) the network.
    [[nodiscard]] Lookup acquire(const geo::TileId& tile, double priority) const;
    [[nodiscard]] std::shared_ptr<const Image> decode(const std::string& body,
                                                      std::string& error) const;
    [[nodiscard]] static std::shared_ptr<const Image> decodeTile(const std::string& body,
                                                                 int tileSizePx,
                                                                 std::string& error);
    friend TerrainResult<void> checkImageryTile(const std::string& body, int tileSizePx);
    void remember(const geo::TileId& tile, std::shared_ptr<const Image> image) const;
    void arrived(const geo::TileId& tile, net::NetResult<net::Payload> result) const;

    net::DataClient* client_;
    net::ProviderConfig provider_;
    net::DatasetConfig dataset_;
    OnlineImageryOptions imageryOptions_;
    geo::LocalFrame frame_{geo::Geodetic{}};
    std::array<int, 32> zoom_{};
    std::array<std::uint8_t, 256> lut_{};
    bool adjust_ = false;

    mutable std::mutex mutex_;
    mutable std::list<geo::TileId> lru_;  // Most recently used first.
    struct Cached {
        std::shared_ptr<const Image> image;
        std::list<geo::TileId>::iterator position;
    };
    mutable std::unordered_map<geo::TileId, Cached, TileIdHash> memory_;
    mutable std::size_t memoryBytes_ = 0;
    mutable std::unordered_map<geo::TileId, net::RequestId, TileIdHash> pending_;
    mutable std::unordered_map<geo::TileId, bool, TileIdHash> missing_;
    mutable std::unordered_map<geo::TileId, Clock::time_point, TileIdHash> failed_;
    mutable OnlineImageryStatus counters_;
    mutable std::atomic<std::uint64_t> revision_{0};
};

/// @brief Decodes @p body as a service tile of @p tileSizePx square (JPEG, PNG or WebP;
/// grey, palette, RGB or RGBA): CorruptData with the reason if it is not one (*Test
/// connection*, FR-NET-11).
[[nodiscard]] TerrainResult<void> checkImageryTile(const std::string& body, int tileSizePx);

}  // namespace ds::terrain
