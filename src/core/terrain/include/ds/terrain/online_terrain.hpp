// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "ds/geo/coordinates.hpp"
#include "ds/net/block_planner.hpp"
#include "ds/net/data_client.hpp"
#include "ds/net/geo_rect.hpp"
#include "ds/net/provider.hpp"
#include "ds/terrain/height_source.hpp"
#include "ds/terrain/raster_source.hpp"
#include "ds/terrain/terrain_error.hpp"

/// @file
/// @brief Online elevation terrain from bounding-box providers (§4.5.3, FR-TER-O01 … O18):
/// a bounded area downloaded in as few requests as possible, or blocks streamed around the
/// drone.
/// @ingroup terrain

namespace ds::terrain {

/// @brief Margin fetched around a bounded online terrain, metres (plus three pixels), so the
/// terrain's border is interpolated from data on both sides.
inline constexpr double kOnlineMarginM = 300.0;

/// @brief What an online terrain is made of.
struct OnlineTerrainRequest {
    net::ProviderConfig provider;           ///< Elevation provider.
    std::string dataset;                    ///< Dataset id; empty ⇒ the provider's default.
    geo::LatLon origin;                     ///< Geographic origin (required for online terrain).
    std::optional<double> originAltitudeM;  ///< AMSL of world height 0; empty ⇒ the ground.
    std::optional<Extent> extent;           ///< Bounded terrain; empty ⇒ streaming blocks.
    EdgeMode edge = EdgeMode::Cliff;        ///< Border of a bounded terrain (Cliff or Fade).
};

/// @brief What the metadata view shows about the provider data (FR-TER-O12).
struct OnlineMetadata {
    std::string providerKey;               ///< e.g. "opentopography".
    std::string providerName;              ///< e.g. "OpenTopography".
    std::string protocol;                  ///< e.g. "rest_bbox".
    std::string datasetId;                 ///< e.g. "COP30".
    std::string datasetName;               ///< e.g. "Copernicus GLO-30 (30 m)".
    double resolutionM = 0.0;              ///< Native resolution of the dataset.
    std::string verticalDatum;             ///< Datum of the dataset (FR-TER-O18).
    bool ellipsoidal = false;              ///< Ellipsoidal heights.
    std::optional<net::GeoRect> coverage;  ///< Dataset coverage (empty ⇒ the whole world).
    std::string attribution;  ///< Dataset citation and provider acknowledgement (FR-TER-O13).
    net::GeoRect area;        ///< Area of the data in use (bounded: with its margin).
    int pieces = 0;           ///< Requests (bounded) or blocks (streaming) in use.
    std::uint64_t bytes = 0;  ///< Size of the payloads in use.
    bool streaming = false;   ///< Blocks streamed around the drone (no extent).
};

/// @brief The dataset of @p request, or a ProviderError.
[[nodiscard]] TerrainResult<const net::DatasetConfig*> onlineDataset(
    const OnlineTerrainRequest& request);

/// @brief The area that must be cached before the terrain can be opened (FR-TER-O09): a
/// bounded terrain's extent plus the margin, or the streaming block under the origin (with its
/// margin). Errors for an unknown dataset or an area outside the dataset's coverage
/// (FR-TER-O08).
[[nodiscard]] TerrainResult<net::GeoRect> onlineRequiredArea(const OnlineTerrainRequest& request);

/// @brief Rectangle fetched for streaming block @p key: the block plus three pixels on every
/// side, so bilinear interpolation up to the block's border stays inside its data.
[[nodiscard]] net::GeoRect blockFetchRect(const net::BlockKey& key,
                                          const net::ProviderConfig& provider,
                                          const net::DatasetConfig& dataset) noexcept;

/// @brief Tuning of block streaming.
struct OnlineStreamOptions {
    double radiusKm = 0.0;  ///< Blocks whose centre is this close are fetched; 0 ⇒ block size.
    int maxPending = 2;     ///< Blocks requested at once.
    std::chrono::seconds retryAfter{120};  ///< Wait before a failed block is asked for again.
};

/// @brief State of block streaming, for the UI.
struct OnlineStreamStatus {
    int loadedBlocks = 0;     ///< Blocks in memory.
    int pendingBlocks = 0;    ///< Blocks being fetched.
    int failedBlocks = 0;     ///< Blocks that failed (retried later).
    std::uint64_t bytes = 0;  ///< Payload size of the loaded blocks.
    double minHeightM = 0.0;  ///< Lowest elevation of the loaded blocks, AMSL.
    double maxHeightM = 0.0;  ///< Highest elevation of the loaded blocks, AMSL.
    std::string lastError;    ///< Latest failure ("" if none), e.g. the quota is used up.
};

/// @brief Online elevation without an extent: fixed-grid blocks (FR-TER-O16) fetched ahead of
/// the drone through the DataClient's cache, quota and offline rules (FR-TER-O06).
/// @details The block under the origin must be cached when the source is opened (it fixes the
/// origin altitude). Further blocks arrive in the background; until then their area has the
/// lowest height of the origin block, and physics never waits for the network. Each arrival
/// raises revision() and lists the block's rectangle in changesSince(). Each block is decoded
/// in memory (GDAL `/vsimem/`) and validated like a file (FR-TER-O15). Thread-safe.
class OnlineStreamSource final : public HeightSource,
                                 public std::enable_shared_from_this<OnlineStreamSource> {
public:
    /// @brief Opens the block under the origin from the cache of @p client, which must outlive
    /// the source. NotCached if that block is not downloaded.
    [[nodiscard]] static TerrainResult<std::shared_ptr<OnlineStreamSource>> open(
        net::DataClient& client, const OnlineTerrainRequest& request,
        const OnlineStreamOptions& options = {});

    /// @brief Cancels the fetches still running.
    ~OnlineStreamSource() override;
    OnlineStreamSource(const OnlineStreamSource&) = delete;
    OnlineStreamSource& operator=(const OnlineStreamSource&) = delete;
    OnlineStreamSource(OnlineStreamSource&&) = delete;
    OnlineStreamSource& operator=(OnlineStreamSource&&) = delete;

    [[nodiscard]] double height(double x, double z) const override;
    void sampleGrid(double x0, double z0, double step, int cols, int rows,
                    std::span<double> out) const override;
    [[nodiscard]] std::uint64_t revision() const noexcept override { return revision_.load(); }
    [[nodiscard]] std::vector<ChangedArea> changesSince(std::uint64_t revision) const override;
    [[nodiscard]] std::string describe() const override;

    /// @brief Requests the missing blocks around world (@p x, @p z). Call it regularly from
    /// one thread (each frame is fine: it does nothing until the point moved 250 m or a second
    /// passed).
    void focus(double x, double z);
    /// @brief Current state.
    [[nodiscard]] OnlineStreamStatus status() const;
    /// @brief Raster metadata of the block under the origin (format, CRS, origin).
    [[nodiscard]] const SourceMetadata& originMetadata() const noexcept { return originMeta_; }

private:
    struct Private {};

public:
    /// @brief Use open().
    OnlineStreamSource(Private, net::DataClient& client, OnlineTerrainRequest request,
                       const net::DatasetConfig& dataset, const OnlineStreamOptions& options);

private:
    using Clock = std::chrono::steady_clock;
    struct Loaded {
        std::shared_ptr<const RasterSource> raster;
        std::uint64_t bytes = 0;
    };

    [[nodiscard]] net::BlockKey keyAt(double x, double z) const noexcept;
    [[nodiscard]] const RasterSource* blockAt(const net::BlockKey& key) const;
    // Decodes a payload of block @p key (any thread); nullptr with @p error on failure.
    [[nodiscard]] std::shared_ptr<const RasterSource> decode(const net::BlockKey& key,
                                                             const std::string& body,
                                                             std::string& error) const;
    void arrived(const net::BlockKey& key, net::NetResult<net::Payload> result);
    [[nodiscard]] ChangedArea worldRect(const net::BlockKey& key) const noexcept;

    net::DataClient* client_;
    OnlineTerrainRequest request_;
    net::DatasetConfig dataset_;
    OnlineStreamOptions options_;
    geo::LocalFrame frame_{geo::Geodetic{}};
    double originAltitude_ = 0.0;
    double fallback_ = 0.0;  // World height of areas without data.
    SourceMetadata originMeta_;
    std::atomic<std::uint64_t> revision_{0};

    mutable std::shared_mutex mutex_;
    std::unordered_map<net::BlockKey, Loaded, net::BlockKeyHash> blocks_;
    std::unordered_map<net::BlockKey, net::RequestId, net::BlockKeyHash> pending_;
    std::unordered_map<net::BlockKey, Clock::time_point, net::BlockKeyHash> failed_;
    std::vector<ChangedArea> changes_;
    std::string lastError_;

    // focus() state (one thread).
    std::optional<std::pair<double, double>> lastFocus_;
    Clock::time_point lastFocusTime_{};
};

/// @brief An opened online terrain.
struct OpenedOnlineTerrain {
    std::shared_ptr<const HeightSource> source;  ///< Heights.
    std::shared_ptr<OnlineStreamSource> stream;  ///< Streaming source (no extent), else null.
    SourceMetadata data;                         ///< Raster metadata: CRS, origin, extent, range.
    OnlineMetadata online;                       ///< Provider metadata.
    /// FR-TER-O14: SHA-256 of the provider, dataset and the content hashes of the payloads
    /// (bounded terrain; empty when streaming, whose data depends on the flight).
    std::string dataHash;
};

/// @brief Opens an online terrain from the cache of @p client only — never from the network.
/// @details Bounded: the pieces planned for onlineRequiredArea() (FR-TER-O16), mosaicked;
/// NotCached if any piece is missing. Streaming: OnlineStreamSource::open(). Heights outside
/// −12 000 … 9 000 m are rejected as corrupt data (FR-TER-O15).
[[nodiscard]] TerrainResult<OpenedOnlineTerrain> openOnlineTerrain(
    net::DataClient& client, const OnlineTerrainRequest& request,
    const OnlineStreamOptions& streamOptions = {});

}  // namespace ds::terrain
