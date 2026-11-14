// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/terrain/height_source.hpp"
#include "ds/terrain/noise.hpp"

/// @file
/// @brief Procedural (random, seeded) height source (SPECIFICATION §4.5.2).
/// @ingroup terrain

namespace ds::terrain {

/// @brief How a bounded terrain ends at its border (FR-TER-P04).
enum class EdgeMode {
    Cliff,  ///< The terrain stops with a vertical wall.
    Fade,   ///< Heights blend to the base height near the border.
    Water,  ///< Heights sink below the water (or base) level near the border.
};

/// @brief A flattened area, e.g. a landing site or mission area (FR-TER-P03).
struct FlatZone {
    double x = 0.0;         ///< Centre, world x (east), metres.
    double z = 0.0;         ///< Centre, world z (−north), metres.
    double radiusM = 50.0;  ///< Fully flat radius.
    double blendM = 50.0;   ///< Width of the transition ring outside the radius.
    /// Height of the flat area; `std::nullopt` ⇒ the natural height at the centre.
    std::optional<double> heightM;
    /// @brief Equality.
    bool operator==(const FlatZone&) const = default;
};

/// @brief Parameters of the procedural terrain (FR-TER-P01..P04). The defaults give rolling
/// hills with a few ridges.
struct ProceduralParams {
    std::uint64_t seed = 1;        ///< Random seed; the same seed and parameters ⇒ same terrain.
    int octaves = 7;               ///< Number of fBm octaves, 1–16.
    double featureSizeM = 2000.0;  ///< Wavelength of the first octave (= 1 / base frequency).
    double lacunarity = 2.0;       ///< Frequency multiplier per octave, 1.5–4.
    double persistence = 0.5;      ///< Amplitude multiplier per octave, 0.05–0.95.
    double amplitudeM = 250.0;     ///< Height scale of the hills, 0–8000 m.
    double baseHeightM = 0.0;      ///< Height offset relative to the origin altitude.
    double mountains = 0.35;       ///< Ridged-noise mountains, 0 (none) – 1 (strong).
    double warp = 0.0;             ///< Domain warping, 0–1 (twisted, less regular shapes).
    double erosion = 0.3;  ///< Erosion approximation, 0–1 (smoother valleys, sharper peaks).
    std::optional<double> waterLevelM;  ///< Water surface height, if any.
    std::optional<double> snowLineM;    ///< Snow line; default from the height range.
    std::vector<FlatZone> flatZones;    ///< Flattened areas (at most 64).
    EdgeMode edge = EdgeMode::Fade;     ///< Border treatment for bounded terrain.

    /// @brief Equality (used to detect changes).
    bool operator==(const ProceduralParams&) const = default;
};

/// @brief Checks @p params against the documented ranges.
/// @return Nothing, or an error naming the first invalid parameter and its range.
[[nodiscard]] common::Result<void> validate(const ProceduralParams& params);

/// @brief Checks that both sides of @p extent are within [kMinExtentM, kMaxExtentM].
[[nodiscard]] common::Result<void> validate(const Extent& extent);

/// @brief Smallest wavelength generated, metres: the detail limit of height().
inline constexpr double kProceduralDetailLimitM = 4.0;

/// @brief Seeded fractal-noise terrain: fBm hills with an erosion approximation, ridged
/// mountains, optional domain warping, flat zones, water level and edge treatment.
class ProceduralSource final : public HeightSource {
public:
    /// @brief Validates the inputs and builds the source.
    [[nodiscard]] static common::Result<std::shared_ptr<const ProceduralSource>> create(
        const ProceduralParams& params, const std::optional<Extent>& extent = std::nullopt);

    /// @brief Parameters the source was built with.
    [[nodiscard]] const ProceduralParams& params() const noexcept { return params_; }

    [[nodiscard]] double height(double x, double z) const override;
    void sampleGrid(double x0, double z0, double step, int cols, int rows,
                    std::span<double> out) const override;
    [[nodiscard]] std::optional<Extent> extent() const override { return extent_; }
    [[nodiscard]] std::optional<double> waterLevel() const override { return params_.waterLevelM; }
    [[nodiscard]] double snowLine() const override { return snowLine_; }
    [[nodiscard]] std::string describe() const override;

    /// @brief Use create().
    ProceduralSource(const ProceduralParams& params, const std::optional<Extent>& extent);

private:
    static constexpr int kMaxOctaves = 16;

    [[nodiscard]] double natural(double x, double z, double minWavelength) const;
    [[nodiscard]] double shaped(double x, double z, double minWavelength) const;

    ProceduralParams params_;
    std::optional<Extent> extent_;
    SimplexNoise hills_;
    SimplexNoise ridges_;
    SimplexNoise mask_;
    SimplexNoise warpX_;
    SimplexNoise warpZ_;
    std::array<std::array<double, 2>, kMaxOctaves> offsets_{};
    std::vector<double> zoneHeights_;
    double snowLine_ = 0.0;
};

}  // namespace ds::terrain
