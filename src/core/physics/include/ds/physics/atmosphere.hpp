// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "ds/weather/wind_levels.hpp"

/// @file
/// @brief ISA-based atmosphere adjusted by the weather (FR-PHY-05).
/// @ingroup physics

namespace ds::physics {

/// @brief Air at one altitude.
struct AirState {
    double temperatureK = 288.15;  ///< Static temperature, K.
    double pressurePa = 101325.0;  ///< Static pressure, Pa.
    double densityKgM3 = 1.225;    ///< Density of the (moist) air, kg/m³.
};

/// @brief International Standard Atmosphere (troposphere) constants.
struct Isa {
    static constexpr double kSeaLevelTemperatureK = 288.15;  ///< T0.
    static constexpr double kSeaLevelPressurePa = 101325.0;  ///< p0.
    static constexpr double kSeaLevelDensity = 1.225;        ///< ρ0, kg/m³.
    static constexpr double kLapseRateKPerM = 0.0065;        ///< L.
    static constexpr double kGasConstantDryAir = 287.05287;  ///< R_d, J/(kg·K).
    static constexpr double kGasConstantVapour = 461.495;    ///< R_v, J/(kg·K).
    /// g0 / (R_d · L): exponent of the barometric formula.
    static constexpr double kPressureExponent = 9.80665 / (287.05287 * 0.0065);
};

/// @brief Dry ISA air at @p altitudeAmslM (no weather).
[[nodiscard]] AirState isaAir(double altitudeAmslM) noexcept;

/// @brief Saturation vapour pressure over water at @p temperatureC (Buck 1996), Pa.
[[nodiscard]] double saturationVapourPressurePa(double temperatureC) noexcept;

/// @brief Air at @p altitudeAmslM for the weather @p conditions measured at the ground
/// altitude @p groundAmslM.
/// @details Temperature falls with the ISA lapse rate from the ground temperature; pressure
/// follows the ISA barometric formula from QNH (so a barometric altimeter set to QNH reads
/// the true altitude in an ISA temperature profile); density includes water vapour (moist air
/// is lighter, so hot and humid days reduce thrust).
[[nodiscard]] AirState airAt(double altitudeAmslM, const weather::AirConditions& conditions,
                             double groundAmslM) noexcept;

/// @brief Pressure altitude above the @p qnhHpa reference for the static pressure
/// @p pressurePa (what a barometric altimeter set to QNH shows), metres.
[[nodiscard]] double pressureAltitudeM(double pressurePa, double qnhHpa) noexcept;

}  // namespace ds::physics
