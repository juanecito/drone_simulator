// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ds/net/geo_rect.hpp"
#include "ds/net/net_error.hpp"
#include "ds/net/provider.hpp"

/// @file
/// @brief Block fetching for bounding-box APIs (FR-TER-O16): as few, large requests as the
/// provider's limits allow, because API calls are the scarce resource.
/// @ingroup net

namespace ds::net {

/// @brief Width and height in pixels of the response for @p area at the dataset's native
/// resolution (the `{width}` and `{height}` of a bounding-box request).
struct ResponseSize {
    long width = 0;   ///< Pixels west to east.
    long height = 0;  ///< Pixels south to north.
};
/// @brief The ResponseSize of @p area.
[[nodiscard]] ResponseSize responseSizePx(const GeoRect& area,
                                          const DatasetConfig& dataset) noexcept;

/// @brief Estimated size of the response for @p area at the dataset's native resolution.
[[nodiscard]] std::uint64_t estimateResponseBytes(const GeoRect& area,
                                                  const DatasetConfig& dataset) noexcept;

/// @brief Splits @p area into the fewest equal pieces that each respect the dataset's area limit
/// and the provider's response-size limits (bytes, and maxSizePx a side).
/// @details Pieces form a grid of columns × rows; among the grids with the fewest pieces the one
/// whose pieces are closest to square (in km) is chosen. Returns InvalidRequest for an invalid
/// rectangle and for an area outside the dataset's coverage (FR-TER-O08).
[[nodiscard]] NetResult<std::vector<GeoRect>> planExtentRequests(const GeoRect& area,
                                                                 const ProviderConfig& provider,
                                                                 const DatasetConfig& dataset);

/// @brief Index of a block of the fixed streaming grid.
struct BlockKey {
    std::int32_t row = 0;                              ///< Latitude band.
    std::int32_t col = 0;                              ///< Block within the band.
    bool operator==(const BlockKey&) const = default;  ///< Equality.
};

/// @brief A block of the streaming grid.
struct Block {
    BlockKey key;       ///< Grid index.
    GeoRect rect;       ///< Geographic extent.
    double distanceKm;  ///< Distance from the query point to the block centre.
};

/// @brief Blocks of a fixed geographic grid of about @p blockSizeKm × @p blockSizeKm whose
/// centre is within @p radiusKm of (@p latDeg, @p lonDeg), nearest first (FR-TER-O16).
/// @details The grid is fixed (independent of the query point) so blocks are reused from the
/// cache between flights. Latitude bands have equal height; each band is divided into a whole
/// number of blocks of nearly @p blockSizeKm width at the band's middle latitude.
[[nodiscard]] std::vector<Block> blocksAround(double latDeg, double lonDeg, double radiusKm,
                                              double blockSizeKm);

/// @brief The block of the grid of blocksAround() that contains (@p latDeg, @p lonDeg).
[[nodiscard]] BlockKey blockAt(double latDeg, double lonDeg, double blockSizeKm) noexcept;

/// @brief Geographic extent of block @p key of the grid of blocksAround().
[[nodiscard]] GeoRect blockRect(const BlockKey& key, double blockSizeKm) noexcept;

/// @brief Hash of a BlockKey (for unordered containers).
struct BlockKeyHash {
    /// @brief Hash value.
    std::size_t operator()(const BlockKey& k) const noexcept {
        return (static_cast<std::size_t>(static_cast<std::uint32_t>(k.row)) << 32U) ^
               static_cast<std::uint32_t>(k.col);
    }
};

}  // namespace ds::net
