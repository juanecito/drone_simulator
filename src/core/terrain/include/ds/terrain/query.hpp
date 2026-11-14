// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <vector>

#include "ds/terrain/chunk.hpp"
#include "ds/terrain/height_source.hpp"

/// @file
/// @brief Height and normal queries matching the rendered LOD-0 surface (FR-TER-07).
/// @ingroup terrain

namespace ds::terrain {

/// @brief A unit vector in the world frame.
struct Normal {
    double x = 0.0;  ///< East.
    double y = 1.0;  ///< Up.
    double z = 0.0;  ///< South.
};

/// @brief Answers height queries for physics, item placement and the UI.
/// @details Queries are answered from the height source directly (they never wait for chunk
/// meshes, FR-TER-05) and are interpolated on the same triangles as the level-0 mesh, so
/// they match the rendered finest surface (to float rounding, well under 1 cm). Positions
/// outside a bounded terrain are clamped onto its border. Thread-safe.
class TerrainQuery {
public:
    /// @brief Queries @p source on the level-0 lattice of @p layout; @p originAltitudeM is the
    /// altitude above mean sea level of world height 0 (the geographic origin's altitude).
    TerrainQuery(std::shared_ptr<const HeightSource> source, const ChunkLayout& layout,
                 double originAltitudeM);

    /// @brief World height of the surface at (@p x, @p z), metres.
    [[nodiscard]] double height(double x, double z) const;
    /// @brief Height above mean sea level at (@p x, @p z), metres.
    [[nodiscard]] double heightAmsl(double x, double z) const {
        return originAltitude_ + height(x, z);
    }
    /// @brief Upward unit normal of the surface triangle at (@p x, @p z).
    [[nodiscard]] Normal normal(double x, double z) const;
    /// @brief The source.
    [[nodiscard]] const std::shared_ptr<const HeightSource>& source() const noexcept {
        return source_;
    }

private:
    struct Cell {
        double h00, h10, h01, h11;  // Corners (col, row): (0,0), (1,0), (0,1), (1,1).
        double fx, fz;              // Position inside the cell, 0–1.
    };
    [[nodiscard]] Cell cell(double x, double z) const;

    std::shared_ptr<const HeightSource> source_;
    double spacing_;
    double originAltitude_;
};

/// @brief Samples a `width × height` grid of heights centred on (@p centerX, @p centerZ)
/// covering @p sizeM metres along its longer side, row-major from the north-west corner (for
/// the 2D preview in the Terrain tab).
[[nodiscard]] std::vector<float> sampleHeightMap(const HeightSource& source, double centerX,
                                                 double centerZ, double sizeM, int width,
                                                 int height);

}  // namespace ds::terrain
