// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/failures.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ds/common/math.hpp"

namespace ds::physics {

std::string_view failureTypeName(FailureType type) noexcept {
    switch (type) {
        case FailureType::GpsDrift: return "gpsDrift";
        case FailureType::GpsLoss: return "gpsLoss";
        case FailureType::MotorFailure: return "motorFailure";
        case FailureType::BatteryDrop: return "batteryDrop";
        case FailureType::CompassError: return "compassError";
        case FailureType::RcLoss: return "rcLoss";
        case FailureType::BaroDrift: return "baroDrift";
    }
    return "";
}

std::optional<FailureType> parseFailureType(std::string_view name) noexcept {
    for (const FailureType t : kFailureTypes) {
        if (failureTypeName(t) == name) {
            return t;
        }
    }
    return std::nullopt;
}

std::string_view failureTitle(FailureType type) noexcept {
    switch (type) {
        case FailureType::GpsDrift: return "GPS drift";
        case FailureType::GpsLoss: return "GPS loss";
        case FailureType::MotorFailure: return "Motor failure";
        case FailureType::BatteryDrop: return "Battery cell fault";
        case FailureType::CompassError: return "Compass interference";
        case FailureType::RcLoss: return "RC link loss";
        case FailureType::BaroDrift: return "Barometer drift";
    }
    return "";
}

Failure defaultFailure(FailureType type) noexcept {
    Failure f;
    f.type = type;
    if (type == FailureType::BaroDrift) {
        f.driftMs = 0.2;
        f.magnitudeM = 15.0;
    }
    return f;
}

FailureInjector::FailureInjector(std::uint64_t seed)
    : seed_(seed), random_(common::mix64(seed ^ 0x4641494CULL)) {}  // "FAIL"

void FailureInjector::inject(const Failure& failure) {
    if (failure.type == FailureType::BatteryDrop) {
        batteryDrops_.push_back(std::clamp(failure.fraction, 0.0, 1.0));
        return;
    }
    Active a;
    a.failure = failure;
    const double angle = random_.uniform() * common::kTwoPi;
    a.direction = Vec3(std::cos(angle), 0.0, std::sin(angle));
    active_.push_back(a);
}

void FailureInjector::clear() {
    active_.clear();
    batteryDrops_.clear();
    random_ = common::GaussianRandom(common::mix64(seed_ ^ 0x4641494CULL));
}

std::vector<Failure> FailureInjector::advance(double dt) {
    std::vector<Failure> ended;
    for (Active& a : active_) {
        a.elapsedS += dt;
        if (a.failure.durationS && a.elapsedS >= *a.failure.durationS) {
            ended.push_back(a.failure);
        }
    }
    std::erase_if(active_, [](const Active& a) {
        return a.failure.durationS && a.elapsedS >= *a.failure.durationS;
    });
    return ended;
}

VehicleFaults FailureInjector::vehicleFaults() const {
    VehicleFaults v;
    for (const Active& a : active_) {
        const Failure& f = a.failure;
        const double drift = std::min(f.magnitudeM, std::max(0.0, f.driftMs) * a.elapsedS);
        switch (f.type) {
            case FailureType::GpsDrift: v.sensors.gpsOffset += drift * a.direction; break;
            case FailureType::GpsLoss: v.sensors.gpsLost = true; break;
            case FailureType::MotorFailure:
                if (f.motor >= 1 && f.motor <= static_cast<int>(kMaxFailureMotors)) {
                    v.motorEfficiency[static_cast<std::size_t>(f.motor - 1)] *=
                        std::clamp(f.efficiency, 0.0, 1.0);
                }
                break;
            case FailureType::CompassError:
                v.sensors.compassOffsetRad += common::degToRad(f.offsetDeg);
                break;
            case FailureType::BaroDrift: v.sensors.baroOffsetM += drift; break;
            case FailureType::BatteryDrop:
            case FailureType::RcLoss: break;
        }
    }
    return v;
}

bool FailureInjector::rcLost() const noexcept {
    return std::ranges::any_of(
        active_, [](const Active& a) { return a.failure.type == FailureType::RcLoss; });
}

std::vector<double> FailureInjector::takeBatteryDrops() { return std::exchange(batteryDrops_, {}); }

}  // namespace ds::physics
