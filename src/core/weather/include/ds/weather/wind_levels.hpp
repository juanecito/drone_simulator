// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <filesystem>
#include <string>

#include "ds/common/error.hpp"
#include "ds/common/json.hpp"

/// @file
/// @brief Wind levels 0–10 (SPECIFICATION §4.2.1, FR-WIND-01/02), read from
/// `assets/environment/wind_levels.json`.
/// @ingroup weather

namespace ds::weather {

/// @brief Parameters of one wind level.
struct WindLevel {
    int level = 0;                   ///< 0 (calm) … 10 (storm).
    std::string name;                ///< Beaufort name, e.g. "Moderate breeze".
    double meanSpeedMs = 0.0;        ///< Mean wind at 10 m above ground, m/s.
    double gustPeakMs = 0.0;         ///< Peak speed reached in gusts, m/s.
    double gustIntervalS = 0.0;      ///< Mean time between gusts, s (0 = no gusts).
    std::string turbulence;          ///< Turbulence description ("none" … "extreme").
    double turbulenceScale = 0.0;    ///< Dryden intensity, 1 = MIL-F-8785C (σ_w = 0.1·W20).
    double directionSwingDeg = 0.0;  ///< Direction wander, ± degrees.
    /// @brief Equality (tests).
    bool operator==(const WindLevel&) const = default;
};

/// @brief The 11 wind levels.
class WindLevelTable {
public:
    /// @brief Number of levels (0–10).
    static constexpr int kLevelCount = 11;

    /// @brief The table of SPECIFICATION §4.2.1 (identical to the shipped data file).
    [[nodiscard]] static const WindLevelTable& builtIn();
    /// @brief Parses the data file format; every level 0–10 must be present exactly once,
    /// level 0 must be calm and speeds must not decrease with the level.
    [[nodiscard]] static common::Result<WindLevelTable> fromJson(const common::Json& json);
    /// @brief Reads `wind_levels.json`.
    [[nodiscard]] static common::Result<WindLevelTable> load(const std::filesystem::path& file);

    /// @brief Level @p level, clamped to 0–10.
    [[nodiscard]] const WindLevel& level(int level) const noexcept;
    /// @brief Parameters at a fractional level (linear between the neighbouring levels; the
    /// name, turbulence text and level number are those of the nearest level). Used while the
    /// weather blends from one level to another.
    [[nodiscard]] WindLevel interpolated(double level) const noexcept;
    /// @brief The level whose mean speed is closest to @p meanSpeedMs (FR-WIND-08).
    [[nodiscard]] int nearestLevel(double meanSpeedMs) const noexcept;
    /// @brief Highest level whose mean speed does not exceed @p speedMs (0 if none).
    [[nodiscard]] int highestLevelAtMost(double speedMs) const noexcept;
    /// @brief Highest level whose gust peak does not exceed @p speedMs (0 if none): the
    /// level a drone that can hold position up to @p speedMs still copes with (FR-WIND-07).
    [[nodiscard]] int highestLevelWithGustsAtMost(double speedMs) const noexcept;
    /// @brief All levels, index = level.
    [[nodiscard]] const std::array<WindLevel, kLevelCount>& levels() const noexcept {
        return levels_;
    }

    /// @brief Equality (tests).
    bool operator==(const WindLevelTable&) const = default;

private:
    std::array<WindLevel, kLevelCount> levels_{};
};

/// @brief Wind as chosen in the Environment tab or a mission (FR-WIND-01).
struct WindSettings {
    int level = 0;                 ///< 0–10.
    double fromDeg = 270.0;        ///< Direction the wind blows from, degrees true.
    bool randomDirection = false;  ///< Pick the direction from the seed instead.
    /// @brief Equality.
    bool operator==(const WindSettings&) const = default;
};

/// @brief Ground-level air conditions (FR-PHY-05; set by the weather generator in M7).
struct AirConditions {
    double temperatureC = 15.0;    ///< Air temperature at the ground, °C.
    double pressureHpa = 1013.25;  ///< QNH (pressure reduced to mean sea level), hPa.
    double humidityPct = 50.0;     ///< Relative humidity, %.
    /// @brief Equality.
    bool operator==(const AirConditions&) const = default;
};

}  // namespace ds::weather
