// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// @defgroup terrain Terrain
/// @brief Height sources, chunked level-of-detail meshes, streaming and height queries
/// (SPECIFICATION §4.5).

/// @file
/// @brief Abstract height source (FR-TER-01) and terrain extent (FR-TER-04a).
/// @ingroup terrain

namespace ds::terrain {

/// @brief Smallest allowed side of a bounded terrain, metres (FR-TER-04a).
inline constexpr double kMinExtentM = 256.0;
/// @brief Largest allowed side of a bounded terrain, metres (FR-TER-04a).
inline constexpr double kMaxExtentM = 200'000.0;

/// @brief Optional terrain dimensions: a rectangle centred on the geographic origin.
/// @details In world coordinates it spans x ∈ [−width/2, width/2] (east) and
/// z ∈ [−length/2, length/2] (z = −north).
struct Extent {
    double widthM = 0.0;   ///< East–west size.
    double lengthM = 0.0;  ///< North–south size.

    /// @brief True if (@p x, @p z) lies inside the rectangle (borders included).
    [[nodiscard]] bool contains(double x, double z) const noexcept {
        return x >= -0.5 * widthM && x <= 0.5 * widthM && z >= -0.5 * lengthM && z <= 0.5 * lengthM;
    }
    /// @brief Equality.
    bool operator==(const Extent&) const = default;
};

/// @brief A world rectangle whose heights changed (data that arrived later, e.g. an online
/// elevation block).
struct ChangedArea {
    std::uint64_t revision = 0;  ///< Source revision that brought the change.
    double minX = 0.0;           ///< West edge.
    double minZ = 0.0;           ///< North edge (smaller z).
    double maxX = 0.0;           ///< East edge.
    double maxZ = 0.0;           ///< South edge.
};

/// @brief Source of terrain heights in the world frame (x = east, z = −north, metres from the
/// geographic origin; heights in metres relative to the origin altitude).
/// @details Implementations are safe to call from several threads at once (chunk workers and
/// physics). Heights must be deterministic (FR-TER-03). Most sources are immutable after
/// construction; a source whose data arrives later (online blocks) raises revision() and lists
/// the changed rectangles in changesSince(), so meshes built earlier can be rebuilt. Heights
/// outside the changed rectangles never change.
class HeightSource {
public:
    HeightSource() = default;
    virtual ~HeightSource() = default;
    HeightSource(const HeightSource&) = delete;
    HeightSource& operator=(const HeightSource&) = delete;
    HeightSource(HeightSource&&) = delete;
    HeightSource& operator=(HeightSource&&) = delete;

    /// @brief Full-detail height at world (@p x, @p z).
    [[nodiscard]] virtual double height(double x, double z) const = 0;

    /// @brief Heights of a regular grid, row-major: `out[row·cols + col]` is the height at
    /// (@p x0 + col·@p step, @p z0 + row·@p step).
    /// @details Implementations may omit features smaller than about 2·@p step (they would
    /// not be visible at that spacing); with the smallest spacing used for meshes they must
    /// equal height() exactly. The default calls height() for every point.
    virtual void sampleGrid(double x0, double z0, double step, int cols, int rows,
                            std::span<double> out) const;

    /// @brief Terrain bounds, or `std::nullopt` for unbounded terrain.
    [[nodiscard]] virtual std::optional<Extent> extent() const { return std::nullopt; }

    /// @brief Water surface height, if the terrain has water.
    [[nodiscard]] virtual std::optional<double> waterLevel() const { return std::nullopt; }

    /// @brief Height above which the ground is snow-covered (material classification).
    [[nodiscard]] virtual double snowLine() const { return 1.0e9; }

    /// @brief Revision of the data, 0 for sources that never change.
    [[nodiscard]] virtual std::uint64_t revision() const noexcept { return 0; }

    /// @brief Rectangles changed by the revisions after @p revision, oldest first.
    [[nodiscard]] virtual std::vector<ChangedArea> changesSince(std::uint64_t revision) const {
        (void)revision;
        return {};
    }

    /// @brief Short human-readable description (logs, UI).
    [[nodiscard]] virtual std::string describe() const = 0;
};

}  // namespace ds::terrain
