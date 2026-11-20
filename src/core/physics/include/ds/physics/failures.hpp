// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "ds/common/random.hpp"
#include "ds/physics/frames.hpp"

/// @file
/// @brief Failure injection (FR-PHY-15): GPS loss and drift, compass interference, barometer
/// drift, motor failure or degradation, a battery cell fault and RC link loss. Missions
/// schedule them (FR-MIS-07); in free mode the Simulation tab injects them.
/// @ingroup physics

namespace ds::physics {

/// @brief Kinds of failure (the mission file's `failures[].type`).
enum class FailureType {
    GpsDrift,      ///< The GPS position walks away from the truth (jamming, multipath).
    GpsLoss,       ///< No GPS fix.
    MotorFailure,  ///< One motor loses power (efficiency 0 = dead).
    BatteryDrop,   ///< A cell fault takes a share of the capacity at once.
    CompassError,  ///< Magnetic interference: the compass heading is off.
    RcLoss,        ///< The RC link is lost (the failsafe acts).
    BaroDrift,     ///< The barometric altitude walks away from the truth.
};

/// @brief All failure types, in declaration order.
inline constexpr std::array<FailureType, 7> kFailureTypes{
    FailureType::GpsDrift,    FailureType::GpsLoss,      FailureType::MotorFailure,
    FailureType::BatteryDrop, FailureType::CompassError, FailureType::RcLoss,
    FailureType::BaroDrift};

/// @brief Mission name ("gpsDrift", …).
[[nodiscard]] std::string_view failureTypeName(FailureType type) noexcept;
/// @brief Parses a mission name.
[[nodiscard]] std::optional<FailureType> parseFailureType(std::string_view name) noexcept;
/// @brief Display name ("GPS drift", …).
[[nodiscard]] std::string_view failureTitle(FailureType type) noexcept;

/// @brief One failure and its parameters (the mission file's names; unused ones are ignored).
struct Failure {
    FailureType type = FailureType::GpsLoss;  ///< Kind.
    std::optional<double> durationS;          ///< How long it lasts (empty = until reset).
    double magnitudeM = 20.0;                 ///< GPS / baro drift: largest offset, m.
    double driftMs = 0.5;                     ///< GPS / baro drift: rate, m/s.
    int motor = 1;  ///< Motor failure: motor (1-based; a ground drone's wheel).
    /// Motor failure: share left, 0 (dead) … 1 — of the thrust for a rotor, of the drive
    /// voltage for a wheel motor.
    double efficiency = 0.0;
    double fraction = 0.3;    ///< Battery drop: share of the rated capacity lost.
    double offsetDeg = 30.0;  ///< Compass error: heading offset, degrees.
    /// @brief Equality.
    bool operator==(const Failure&) const = default;
};

/// @brief A failure of @p type with its default parameters (baro drift: 0.2 m/s up to 15 m).
[[nodiscard]] Failure defaultFailure(FailureType type) noexcept;

/// @brief What the active failures do to the sensors.
struct SensorFaults {
    bool gpsLost = false;           ///< No fix.
    Vec3 gpsOffset{0.0};            ///< Added to the GPS position, world frame.
    double compassOffsetRad = 0.0;  ///< Added to the compass heading.
    double baroOffsetM = 0.0;       ///< Added to the barometric altitude.
    /// @brief Equality.
    bool operator==(const SensorFaults&) const = default;
};

/// @brief Most motors (or driven wheels) a failure can address.
inline constexpr std::size_t kMaxFailureMotors = 8;

/// @brief What the active failures do to a vehicle (Vehicle::setFaults()).
struct VehicleFaults {
    SensorFaults sensors;  ///< Sensor errors.
    /// Share left per motor (index 0 = motor 1; Failure::efficiency); 1 = healthy.
    std::array<double, kMaxFailureMotors> motorEfficiency{1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    /// @brief Equality.
    bool operator==(const VehicleFaults&) const = default;
};

/// @brief The active failures and their timing. Deterministic for a seed (drift directions).
class FailureInjector {
public:
    /// @brief An active failure.
    struct Active {
        Failure failure;                ///< What and its parameters.
        double elapsedS = 0.0;          ///< Time since it started.
        Vec3 direction{1.0, 0.0, 0.0};  ///< GPS drift direction (horizontal unit vector).
    };

    /// @brief Injector whose drift directions come from @p seed.
    explicit FailureInjector(std::uint64_t seed = 0);

    /// @brief Starts @p failure now. A battery drop acts once (takeBatteryDrops()).
    void inject(const Failure& failure);
    /// @brief Ends every failure (vehicle reset).
    void clear();
    /// @brief Advances the clocks by @p dt; failures past their duration end and are returned.
    std::vector<Failure> advance(double dt);

    /// @brief Combined effect on the vehicle now.
    [[nodiscard]] VehicleFaults vehicleFaults() const;
    /// @brief True while an RC-loss failure is active.
    [[nodiscard]] bool rcLost() const noexcept;
    /// @brief Battery drops (fractions of the capacity) injected since the last call.
    [[nodiscard]] std::vector<double> takeBatteryDrops();
    /// @brief The active failures (battery drops are not kept).
    [[nodiscard]] const std::vector<Active>& active() const noexcept { return active_; }

private:
    std::uint64_t seed_;
    common::GaussianRandom random_;
    std::vector<Active> active_;
    std::vector<double> batteryDrops_;
};

}  // namespace ds::physics
