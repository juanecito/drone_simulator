// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/common/json.hpp"
#include "ds/net/geo_rect.hpp"

/// @file
/// @brief Data-provider configuration (FR-TER-O02): built-in presets from
/// `assets/providers/*.json` and user-defined providers share this format.
/// @ingroup net

namespace ds::net {

/// @brief Version of the provider JSON format read by parseProvider().
inline constexpr int kProviderSchemaVersion = 1;

/// @brief Highest zoom level an imagery dataset may declare.
inline constexpr int kMaxImageryZoom = 24;

/// @brief True for the tile protocols of imagery providers (FR-TER-O10): `xyz`, `tms`, `wmts`
/// (Web-Mercator "GoogleMapsCompatible" tile matrix set) and `wms` (GetMap per tile).
[[nodiscard]] bool isTileProtocol(std::string_view protocol) noexcept;

/// @brief One dataset offered by a provider (e.g. COP30 from OpenTopography).
struct DatasetConfig {
    std::string id;                    ///< Value of the `{dataset}` placeholder (e.g. "COP30").
    std::string name;                  ///< Display name.
    std::string urlTemplate;           ///< Overrides the provider's template if not empty.
    double resolutionM = 30.0;         ///< Native ground resolution.
    std::optional<double> maxAreaKm2;  ///< Provider's area limit per request.
    std::string verticalDatum;         ///< e.g. "EGM2008"; empty ⇒ unknown.
    bool ellipsoidal = false;          ///< Ellipsoidal heights (opt-in, FR-TER-O18).
    std::optional<GeoRect> coverage;   ///< Covered area; empty ⇒ whole world.
    int bytesPerSample = 4;            ///< For response-size estimates.
    std::string attribution;           ///< Dataset citation (FR-TER-O13).
    bool verified = true;  ///< False if the request format is not verified against the docs.
    // Imagery (tile protocols).
    int minZoom = 0;             ///< Coarsest zoom level served.
    std::optional<int> maxZoom;  ///< Finest zoom level served (required for imagery).
};

/// @brief Where the API key goes in a request (FR-NET-07).
struct ApiKeyPlacement {
    /// @brief Query parameter or HTTP header.
    enum class Location {
        Query,  ///< Appended to the URL query.
        Header  ///< Sent as an HTTP header.
    };
    Location location = Location::Query;  ///< Where it goes.
    std::string name;                     ///< Parameter or header name (e.g. "API_Key").
};

/// @brief A configured data provider.
struct ProviderConfig {
    std::string key;                      ///< Stable unique key (e.g. "opentopography").
    std::string name;                     ///< Display name.
    std::string kind = "elevation";       ///< `elevation` or `imagery`.
    /// `rest_bbox` for elevation (FR-TER-O01); a tile protocol for imagery (isTileProtocol()).
    std::string protocol = "rest_bbox";
    std::string urlTemplate;              ///< Default template; never contains the key itself.
    std::string format;                   ///< Value of `{format}` (e.g. "GTiff").
    /// Value of `{crs}`: EPSG:4326 (bounding boxes) or EPSG:3857 (imagery tiles).
    std::string crs = "EPSG:4326";
    int tileSizePx = 256;                 ///< Imagery tile size (256 or 512).
    std::vector<DatasetConfig> datasets;  ///< Offered datasets (at least one).
    std::string defaultDataset;           ///< Id of the default dataset.
    bool requiresKey = false;             ///< True if requests need an API key.
    ApiKeyPlacement apiKey;               ///< Where the key goes (when requiresKey).
    std::string attribution;              ///< Provider acknowledgement (FR-TER-O13).
    std::string licence;                  ///< Licence / terms summary.
    std::string termsUrl;                 ///< Terms of use.
    bool allowCache = true;               ///< Provider allows caching (FR-NET-05).
    /// Expired cache entries may stand in when the provider cannot be reached (offline mode,
    /// quota used up, network errors). False for services that forbid offline use, such as
    /// the OpenStreetMap tile servers: only entries the server's cache lifetime allows are used.
    bool serveStale = true;
    std::optional<int> maxAgeDays;        ///< Cache lifetime; empty ⇒ unlimited.
    std::optional<double> rateLimitRps;   ///< Max requests per second.
    std::optional<int> dailyCallLimit;    ///< Rolling 24 h call budget (FR-TER-O17).
    int maxConcurrent = 2;                ///< Simultaneous requests (FR-NET-01).
    bool insecureHttp = false;            ///< Plain HTTP allowed (FR-NET-02).
    std::uint64_t maxResponseBytes = std::uint64_t{64} << 20U;  ///< FR-NET-02 (64 MB).
    /// Largest width and height of one bounding-box response in pixels (a WMS server's
    /// MaxWidth / MaxHeight); requests are split to stay within it. Empty ⇒ no limit.
    std::optional<int> maxSizePx;
    std::vector<std::string> contentTypes;                      ///< Allowed response content types.
    std::vector<std::string> quotaErrorPatterns;  ///< Body texts marking a daily-quota error.
    double blockSizeKm = 20.0;       ///< Block size for streaming without extent (FR-TER-O16).
    std::string origin = "builtin";  ///< `builtin` or `user`.

    /// @brief True for an imagery provider (tile protocol).
    [[nodiscard]] bool isImagery() const noexcept { return kind == "imagery"; }
    /// @brief The dataset with @p id, or nullptr.
    [[nodiscard]] const DatasetConfig* dataset(std::string_view id) const noexcept;
    /// @brief URL template used for @p dataset.
    [[nodiscard]] const std::string& templateFor(const DatasetConfig& dataset) const noexcept;
};

/// @brief Reads and validates a provider from JSON (FR-TER-O02).
/// @details Errors carry the JSON path, e.g. `/datasets/2/resolutionM: must be …`. Unknown
/// members are errors, except those starting with `x-` or `$`.
[[nodiscard]] common::Result<ProviderConfig> parseProvider(const common::Json& json);

/// @brief Writes @p provider in the format read by parseProvider().
[[nodiscard]] common::Json toJson(const ProviderConfig& provider);

/// @brief Reads every `*.json` provider preset in @p directory, sorted by key.
/// @details A broken file is reported in the error with its name; presets are shipped with the
/// application, so any error is a packaging bug.
[[nodiscard]] common::Result<std::vector<ProviderConfig>> loadProviderPresets(
    const std::filesystem::path& directory);

}  // namespace ds::net
