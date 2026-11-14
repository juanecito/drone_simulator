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
    std::string protocol = "rest_bbox";   ///< Only `rest_bbox` is implemented (FR-TER-O01).
    std::string urlTemplate;              ///< Default template; never contains the key itself.
    std::string format;                   ///< Value of `{format}` (e.g. "GTiff").
    std::string crs = "EPSG:4326";        ///< Value of `{crs}` and CRS of the bounding box.
    std::vector<DatasetConfig> datasets;  ///< Offered datasets (at least one).
    std::string defaultDataset;           ///< Id of the default dataset.
    bool requiresKey = false;             ///< True if requests need an API key.
    ApiKeyPlacement apiKey;               ///< Where the key goes (when requiresKey).
    std::string attribution;              ///< Provider acknowledgement (FR-TER-O13).
    std::string licence;                  ///< Licence / terms summary.
    std::string termsUrl;                 ///< Terms of use.
    bool allowCache = true;               ///< Provider allows caching (FR-NET-05).
    std::optional<int> maxAgeDays;        ///< Cache lifetime; empty ⇒ unlimited.
    std::optional<double> rateLimitRps;   ///< Max requests per second.
    std::optional<int> dailyCallLimit;    ///< Rolling 24 h call budget (FR-TER-O17).
    int maxConcurrent = 2;                ///< Simultaneous requests (FR-NET-01).
    bool insecureHttp = false;            ///< Plain HTTP allowed (FR-NET-02).
    std::uint64_t maxResponseBytes = std::uint64_t{64} << 20U;  ///< FR-NET-02 (64 MB).
    std::vector<std::string> contentTypes;                      ///< Allowed response content types.
    std::vector<std::string> quotaErrorPatterns;  ///< Body texts marking a daily-quota error.
    double blockSizeKm = 20.0;       ///< Block size for streaming without extent (FR-TER-O16).
    std::string origin = "builtin";  ///< `builtin` or `user`.

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
