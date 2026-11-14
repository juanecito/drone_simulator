// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/atmosphere.hpp"

#include <algorithm>
#include <cmath>

namespace ds::physics {
namespace {

constexpr double kCelsiusToKelvin = 273.15;
// Keep the formulas inside the troposphere and away from singular values.
constexpr double kMinAltitude = -500.0;
constexpr double kMaxAltitude = 11000.0;

double pressureFromQnh(double altitudeM, double qnhPa) noexcept {
    const double h = std::clamp(altitudeM, kMinAltitude, kMaxAltitude);
    return qnhPa * std::pow(1.0 - Isa::kLapseRateKPerM * h / Isa::kSeaLevelTemperatureK,
                            Isa::kPressureExponent);
}

}  // namespace

AirState isaAir(double altitudeAmslM) noexcept {
    const double h = std::clamp(altitudeAmslM, kMinAltitude, kMaxAltitude);
    AirState air;
    air.temperatureK = Isa::kSeaLevelTemperatureK - Isa::kLapseRateKPerM * h;
    air.pressurePa = pressureFromQnh(h, Isa::kSeaLevelPressurePa);
    air.densityKgM3 = air.pressurePa / (Isa::kGasConstantDryAir * air.temperatureK);
    return air;
}

double saturationVapourPressurePa(double temperatureC) noexcept {
    const double t = std::clamp(temperatureC, -80.0, 80.0);
    return 611.21 * std::exp((18.678 - t / 234.5) * (t / (257.14 + t)));
}

AirState airAt(double altitudeAmslM, const weather::AirConditions& conditions,
               double groundAmslM) noexcept {
    const double h = std::clamp(altitudeAmslM, kMinAltitude, kMaxAltitude);
    AirState air;
    air.temperatureK =
        std::max(150.0, conditions.temperatureC + kCelsiusToKelvin -
                            Isa::kLapseRateKPerM *
                                (h - std::clamp(groundAmslM, kMinAltitude, kMaxAltitude)));
    air.pressurePa = pressureFromQnh(h, conditions.pressureHpa * 100.0);
    const double vapour = std::clamp(conditions.humidityPct, 0.0, 100.0) / 100.0 *
                          saturationVapourPressurePa(air.temperatureK - kCelsiusToKelvin);
    const double dry = std::max(0.0, air.pressurePa - vapour);
    air.densityKgM3 = dry / (Isa::kGasConstantDryAir * air.temperatureK) +
                      vapour / (Isa::kGasConstantVapour * air.temperatureK);
    return air;
}

double pressureAltitudeM(double pressurePa, double qnhHpa) noexcept {
    const double ratio = std::max(pressurePa, 1.0) / (qnhHpa * 100.0);
    return Isa::kSeaLevelTemperatureK / Isa::kLapseRateKPerM *
           (1.0 - std::pow(ratio, 1.0 / Isa::kPressureExponent));
}

}  // namespace ds::physics
