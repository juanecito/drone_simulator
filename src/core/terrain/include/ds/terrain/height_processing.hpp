// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

/// @file
/// @brief How elevation data (files, lidar DTMs, online providers) is interpolated between
/// its cells and filtered before it becomes the terrain surface (FR-TER-C03).
/// @ingroup terrain

namespace ds::terrain {

/// @brief Interpolation between the cells of an elevation raster.
enum class HeightInterpolation {
    Bilinear,  ///< Straight lines between cell centres: exact at the cells, creased slopes.
    Bicubic,   ///< Catmull-Rom cubic: exact at the cells, smooth slopes; may overshoot cliffs.
    BSpline,   ///< Cubic B-spline: the smoothest surface; slightly lowers peaks and ridges.
    Nearest,   ///< Each cell is a flat step: shows the raw cells.
};

/// @brief Largest radius of the spike filter, metres.
inline constexpr double kMaxDespikeRadiusM = 10.0;
/// @brief Largest radius of the smoothing filter, metres.
inline constexpr double kMaxSmoothRadiusM = 100.0;

/// @brief Interpolation and filters applied to elevation data, in this order: NoData voids
/// are filled, spikes removed, the cells smoothed, then interpolated.
struct HeightProcessing {
    HeightInterpolation interpolation = HeightInterpolation::Bilinear;  ///< Between cells.
    /// Median filter radius, metres (0 = off): each cell becomes the median of the cells
    /// within this distance, which removes isolated spikes and pits (lidar noise, DEM
    /// artefacts) and keeps slopes and edges.
    double despikeRadiusM = 0.0;
    /// Gaussian smoothing radius, metres (0 = off): the kernel reaches this far (σ = half
    /// of it). It removes noise and the terracing of integer-metre data, and softens detail.
    double smoothRadiusM = 0.0;

    /// @brief True if a filter is on (the cells change, not only the interpolation).
    [[nodiscard]] bool filtered() const noexcept {
        return despikeRadiusM > 0.0 || smoothRadiusM > 0.0;
    }
    /// @brief How far from a point the filters look, metres.
    [[nodiscard]] double reachM() const noexcept { return despikeRadiusM + smoothRadiusM; }
    /// @brief Equality.
    bool operator==(const HeightProcessing&) const = default;
};

/// @brief Stable name of @p interpolation: "bilinear", "bicubic", "bspline" or "nearest".
[[nodiscard]] std::string_view interpolationName(HeightInterpolation interpolation) noexcept;
/// @brief The interpolation called @p name, or nullopt.
[[nodiscard]] std::optional<HeightInterpolation> interpolationFromName(
    std::string_view name) noexcept;
/// @brief Why @p processing is invalid ("" if it is valid): radii must be 0 … the limits.
[[nodiscard]] std::string validateProcessing(const HeightProcessing& processing);
/// @brief Short description for the metadata view, e.g. "bicubic, spikes 2 m, smoothing
/// 10 m"; "bilinear" for the default.
[[nodiscard]] std::string describeProcessing(const HeightProcessing& processing);

}  // namespace ds::terrain
