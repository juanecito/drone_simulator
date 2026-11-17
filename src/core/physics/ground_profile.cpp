// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/ground_profile.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "ds/common/math.hpp"

namespace ds::physics {
namespace {

using common::Json;

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define DS_NUMBER(ptr, group, label, unit, lo, hi, dec, member)  \
    GroundParamDescriptor {                                      \
        ptr, group, label, unit, lo, hi, dec, ParamKind::Number, \
            [](const GroundProfile& p) { return p.member; },     \
            [](GroundProfile& p, double v) { p.member = v; }     \
    }
#define DS_INTEGER(ptr, group, label, unit, lo, hi, member)                                 \
    GroundParamDescriptor {                                                                 \
        ptr, group, label, unit, lo, hi, 0, ParamKind::Integer,                             \
            [](const GroundProfile& p) { return static_cast<double>(p.member); },           \
            [](GroundProfile& p, double v) { p.member = static_cast<int>(std::lround(v)); } \
    }
// NOLINTEND(cppcoreguidelines-macro-usage)

const std::vector<GroundParamDescriptor>& descriptors() {
    static const std::vector<GroundParamDescriptor> kList = {
        // Chassis
        DS_NUMBER("/chassis/massKg", "Chassis", "All-up mass", "kg", 0.3, 50.0, 2, chassis.massKg),
        DS_NUMBER("/chassis/cogHeightM", "Chassis", "Centre of mass height", "m", 0.02, 1.0, 3,
                  chassis.cogHeightM),
        DS_NUMBER("/chassis/lengthM", "Chassis", "Body length", "m", 0.05, 2.0, 3, chassis.lengthM),
        DS_NUMBER("/chassis/widthM", "Chassis", "Body width", "m", 0.05, 2.0, 3, chassis.widthM),
        DS_NUMBER("/chassis/heightM", "Chassis", "Body height", "m", 0.02, 1.5, 3, chassis.heightM),
        DS_NUMBER("/chassis/clearanceM", "Chassis", "Ground clearance", "m", 0.005, 0.5, 3,
                  chassis.clearanceM),
        DS_NUMBER("/chassis/inertiaXxKgM2", "Chassis", "Roll inertia", "kg·m²", 1e-4, 20.0, 4,
                  chassis.inertiaXxKgM2),
        DS_NUMBER("/chassis/inertiaYyKgM2", "Chassis", "Pitch inertia", "kg·m²", 1e-4, 20.0, 4,
                  chassis.inertiaYyKgM2),
        DS_NUMBER("/chassis/inertiaZzKgM2", "Chassis", "Yaw inertia", "kg·m²", 1e-4, 20.0, 4,
                  chassis.inertiaZzKgM2),
        DS_NUMBER("/chassis/dragAreaForwardM2", "Chassis", "Drag area forward (C_d·A)", "m²", 0.0,
                  2.0, 3, chassis.dragAreaForwardM2),
        DS_NUMBER("/chassis/dragAreaSideM2", "Chassis", "Drag area sideways (C_d·A)", "m²", 0.0,
                  2.0, 3, chassis.dragAreaSideM2),
        DS_NUMBER("/chassis/dragAreaVerticalM2", "Chassis", "Drag area vertical (C_d·A)", "m²", 0.0,
                  4.0, 3, chassis.dragAreaVerticalM2),
        DS_NUMBER("/chassis/crashSpeedMs", "Chassis", "Crash impact speed", "m/s", 0.5, 30.0, 1,
                  chassis.crashSpeedMs),
        // Wheels
        DS_NUMBER("/wheels/wheelbaseM", "Wheels", "Wheelbase", "m", 0.05, 2.0, 3,
                  wheels.wheelbaseM),
        DS_NUMBER("/wheels/trackWidthM", "Wheels", "Track width", "m", 0.05, 2.0, 3,
                  wheels.trackWidthM),
        DS_NUMBER("/wheels/radiusM", "Wheels", "Wheel radius", "m", 0.01, 0.5, 3, wheels.radiusM),
        DS_NUMBER("/wheels/widthM", "Wheels", "Wheel width", "m", 0.005, 0.5, 3, wheels.widthM),
        DS_NUMBER("/wheels/inertiaKgM2", "Wheels", "Wheel inertia", "kg·m²", 1e-6, 1.0, 6,
                  wheels.inertiaKgM2),
        // Suspension
        DS_NUMBER("/suspension/stiffnessNPerM", "Suspension", "Spring rate (per wheel)", "N/m",
                  10.0, 1e6, 0, suspension.stiffnessNPerM),
        DS_NUMBER("/suspension/dampingNsPerM", "Suspension", "Damper rate (per wheel)", "N·s/m",
                  0.0, 1e5, 1, suspension.dampingNsPerM),
        DS_NUMBER("/suspension/travelM", "Suspension", "Travel", "m", 0.002, 0.5, 3,
                  suspension.travelM),
        // Tyres
        DS_NUMBER("/tyre/gripScale", "Tyres", "Grip (relative)", "", 0.2, 2.0, 2, tyre.gripScale),
        DS_NUMBER("/tyre/stiffnessB", "Tyres", "Slip stiffness B", "", 1.0, 40.0, 1,
                  tyre.stiffnessB),
        DS_NUMBER("/tyre/shapeC", "Tyres", "Shape factor C", "", 1.0, 2.0, 2, tyre.shapeC),
        DS_NUMBER("/tyre/rollingResistanceScale", "Tyres", "Rolling resistance (relative)", "", 0.2,
                  5.0, 2, tyre.rollingResistanceScale),
        // Drivetrain
        DS_NUMBER("/drivetrain/motorKv", "Drivetrain", "Motor KV", "rpm/V", 10.0, 20000.0, 0,
                  drivetrain.motorKv),
        DS_NUMBER("/drivetrain/motorResistanceOhm", "Drivetrain", "Motor winding resistance", "Ω",
                  0.005, 50.0, 3, drivetrain.motorResistanceOhm),
        DS_NUMBER("/drivetrain/motorNoLoadCurrentA", "Drivetrain", "Motor no-load current", "A",
                  0.0, 20.0, 2, drivetrain.motorNoLoadCurrentA),
        DS_NUMBER("/drivetrain/motorInertiaKgM2", "Drivetrain", "Motor armature inertia", "kg·m²",
                  1e-9, 1e-2, 8, drivetrain.motorInertiaKgM2),
        DS_NUMBER("/drivetrain/gearRatio", "Drivetrain", "Gear ratio", ":1", 1.0, 500.0, 1,
                  drivetrain.gearRatio),
        DS_NUMBER("/drivetrain/gearEfficiency", "Drivetrain", "Gearbox efficiency", "", 0.2, 1.0, 2,
                  drivetrain.gearEfficiency),
        DS_NUMBER("/drivetrain/maxCurrentA", "Drivetrain", "ESC current limit (per motor)", "A",
                  0.1, 300.0, 1, drivetrain.maxCurrentA),
        DS_NUMBER("/drivetrain/brakeTorqueNm", "Drivetrain", "Brake torque (per wheel)", "N·m", 0.0,
                  100.0, 2, drivetrain.brakeTorqueNm),
        // Battery
        DS_INTEGER("/battery/cellsInSeries", "Battery", "Cells in series", "S", 1, 14,
                   battery.cellsInSeries),
        DS_NUMBER("/battery/capacityAh", "Battery", "Capacity", "Ah", 0.1, 50.0, 3,
                  battery.capacityAh),
        DS_NUMBER("/battery/cellResistanceOhm", "Battery", "Internal resistance per cell", "Ω",
                  0.0005, 0.2, 4, battery.cellResistanceOhm),
        DS_NUMBER("/battery/maxDischargeC", "Battery", "Rated discharge", "C", 1.0, 200.0, 0,
                  battery.maxDischargeC),
        DS_NUMBER("/battery/massKg", "Battery", "Pack mass", "kg", 0.005, 10.0, 3, battery.massKg),
        DS_NUMBER("/battery/heatCapacityJPerK", "Battery", "Heat capacity", "J/K", 1.0, 20000.0, 0,
                  battery.heatCapacityJPerK),
        DS_NUMBER("/battery/coolingWPerK", "Battery", "Cooling", "W/K", 0.01, 100.0, 2,
                  battery.coolingWPerK),
        DS_NUMBER("/battery/lowCellVoltage", "Battery", "Low warning (per cell)", "V", 2.5, 4.2, 2,
                  battery.lowCellVoltage),
        DS_NUMBER("/battery/criticalCellVoltage", "Battery", "Critical warning (per cell)", "V",
                  2.5, 4.2, 2, battery.criticalCellVoltage),
        DS_NUMBER("/battery/cutoffCellVoltage", "Battery", "Cut-off (per cell)", "V", 2.0, 4.0, 2,
                  battery.cutoffCellVoltage),
        DS_NUMBER("/electrical/avionicsCurrentA", "Battery", "Electronics current", "A", 0.0, 20.0,
                  2, electrical.avionicsCurrentA),
        DS_NUMBER("/electrical/escEfficiency", "Battery", "ESC efficiency", "", 0.5, 1.0, 2,
                  electrical.escEfficiency),
        // Sensors
        DS_NUMBER("/sensors/gyroNoiseDps", "Sensors", "Gyro noise", "°/s", 0.0, 5.0, 3,
                  sensors.gyroNoiseDps),
        DS_NUMBER("/sensors/gyroBiasDps", "Sensors", "Gyro bias", "°/s", 0.0, 10.0, 3,
                  sensors.gyroBiasDps),
        DS_NUMBER("/sensors/gyroBiasWalkDps", "Sensors", "Gyro bias walk", "°/s/√s", 0.0, 1.0, 4,
                  sensors.gyroBiasWalkDps),
        DS_NUMBER("/sensors/accelNoiseMs2", "Sensors", "Accelerometer noise", "m/s²", 0.0, 5.0, 3,
                  sensors.accelNoiseMs2),
        DS_NUMBER("/sensors/accelBiasMs2", "Sensors", "Accelerometer bias", "m/s²", 0.0, 2.0, 3,
                  sensors.accelBiasMs2),
        DS_NUMBER("/sensors/vibrationMs2", "Sensors", "Vibration at full speed", "m/s²", 0.0, 50.0,
                  2, sensors.vibrationMs2),
        DS_NUMBER("/sensors/baroNoiseM", "Sensors", "Barometer noise", "m", 0.0, 5.0, 2,
                  sensors.baroNoiseM),
        DS_NUMBER("/sensors/baroRateHz", "Sensors", "Barometer rate", "Hz", 1.0, 200.0, 0,
                  sensors.baroRateHz),
        DS_NUMBER("/sensors/gpsRateHz", "Sensors", "GPS rate", "Hz", 1.0, 20.0, 0,
                  sensors.gpsRateHz),
        DS_NUMBER("/sensors/gpsHorizontalNoiseM", "Sensors", "GPS horizontal error", "m", 0.0, 20.0,
                  2, sensors.gpsHorizontalNoiseM),
        DS_NUMBER("/sensors/gpsVerticalNoiseM", "Sensors", "GPS vertical error", "m", 0.0, 30.0, 2,
                  sensors.gpsVerticalNoiseM),
        DS_NUMBER("/sensors/gpsVelocityNoiseMs", "Sensors", "GPS velocity noise", "m/s", 0.0, 2.0,
                  3, sensors.gpsVelocityNoiseMs),
        DS_NUMBER("/sensors/gpsAcquisitionS", "Sensors", "GPS time to first fix", "s", 0.0, 300.0,
                  0, sensors.gpsAcquisitionS),
        DS_NUMBER("/sensors/magNoiseDeg", "Sensors", "Compass noise", "°", 0.0, 10.0, 2,
                  sensors.magNoiseDeg),
        DS_INTEGER("/encoders/countsPerRev", "Sensors", "Wheel encoder counts per turn", "", 1,
                   100000, encoders.countsPerRev),
        // Drive controller
        DS_NUMBER("/controller/steeringGain", "Drive controller", "Steering gain", "", 0.1, 1.0, 2,
                  controller.steeringGain),
        DS_NUMBER("/controller/throttleExpo", "Drive controller", "Throttle expo", "", 0.0, 1.0, 2,
                  controller.throttleExpo),
        DS_NUMBER("/controller/cruiseSpeedMs", "Drive controller",
                  "Speed control: full-stick speed", "m/s", 0.1, 30.0, 2, controller.cruiseSpeedMs),
        DS_NUMBER("/controller/speedP", "Drive controller", "Speed control P", "1/(m/s)", 0.0, 10.0,
                  3, controller.speedP),
        DS_NUMBER("/controller/speedI", "Drive controller", "Speed control I", "1/m", 0.0, 20.0, 3,
                  controller.speedI),
        DS_NUMBER("/controller/failsafeDelayS", "Drive controller", "Failsafe delay", "s", 0.1,
                  10.0, 1, controller.failsafeDelayS),
        // Limits
        DS_NUMBER("/limits/wadingDepthM", "Limits", "Wading depth", "m", 0.0, 2.0, 3,
                  limits.wadingDepthM),
        DS_NUMBER("/limits/stuckDetectS", "Limits", "Stuck detection time", "s", 1.0, 120.0, 1,
                  limits.stuckDetectS),
        // Camera
        DS_NUMBER("/camera/uptiltDeg", "FPV camera", "Camera uptilt", "°", -30.0, 60.0, 0,
                  camera.uptiltDeg),
        DS_NUMBER("/camera/fovDeg", "FPV camera", "Horizontal field of view", "°", 40.0, 170.0, 0,
                  camera.fovDeg),
    };
    return kList;
}

#undef DS_NUMBER
#undef DS_INTEGER

const Json* findPointer(const Json& root, std::string_view pointer) {
    try {
        const Json::json_pointer p{std::string(pointer)};
        return root.contains(p) ? &root.at(p) : nullptr;
    } catch (const Json::exception&) {
        return nullptr;
    }
}

}  // namespace

std::string_view driveLayoutName(DriveLayout layout) noexcept {
    return layout == DriveLayout::SkidSteer6 ? "skid-steer-6wd" : "skid-steer-4wd";
}

int GroundProfile::wheelCount() const noexcept { return layout == DriveLayout::SkidSteer6 ? 6 : 4; }

std::vector<Vec3> GroundProfile::wheelHubsAtRest() const {
    const int perSide = wheelCount() / 2;
    const double z = chassis.cogHeightM - wheels.radiusM;
    std::vector<Vec3> hubs;
    for (const double y : {-0.5 * wheels.trackWidthM, 0.5 * wheels.trackWidthM}) {
        for (int i = 0; i < perSide; ++i) {
            const double x = wheels.wheelbaseM *
                             (0.5 - static_cast<double>(i) / static_cast<double>(perSide - 1));
            hubs.emplace_back(x, y, z);
        }
    }
    return hubs;
}

bool GroundProfile::isRightWheel(int index) const noexcept { return index >= wheelCount() / 2; }

double GroundProfile::staticCompressionM() const noexcept {
    return chassis.massKg * kGravity /
           (static_cast<double>(wheelCount()) * suspension.stiffnessNPerM);
}

std::span<const GroundParamDescriptor> groundParameters() { return descriptors(); }

common::Result<GroundProfile> groundProfileFromJson(const Json& json) {
    if (!json.is_object()) {
        return common::makeError("vehicle profile: the document must be a JSON object");
    }
    common::JsonReader root(json);
    GroundProfile p;
    root.integer("schemaVersion", 1, 1);
    (void)root.choice("vehicleType", {"ground"});
    p.key = root.string("key");
    p.name = root.string("name");
    p.description = root.string("description", "");
    if (root.has("layout")) {
        p.layout = root.choice("layout", {"skid-steer-4wd", "skid-steer-6wd"}) == "skid-steer-6wd"
                       ? DriveLayout::SkidSteer6
                       : DriveLayout::SkidSteer4;
    }

    // Numeric parameters: absent ones keep their defaults (forward compatibility).
    for (const GroundParamDescriptor& d : descriptors()) {
        const Json* value = findPointer(json, d.pointer);
        if (value == nullptr) {
            continue;
        }
        const bool typeOk =
            d.kind == ParamKind::Integer ? value->is_number_integer() : value->is_number();
        const double v = typeOk ? value->get<double>() : 0.0;
        if (!typeOk || !std::isfinite(v) || v < d.min || v > d.max) {
            root.fail(d.pointer.substr(1),
                      std::format("must be {} between {} and {}",
                                  d.kind == ParamKind::Integer ? "an integer" : "a number", d.min,
                                  d.max));
            continue;
        }
        d.set(p, v);
    }
    if (const Json* chem = findPointer(json, "/battery/chemistry"); chem != nullptr) {
        if (chem->is_string() && *chem == "lipo") {
            p.battery.chemistry = Chemistry::LiPo;
        } else if (chem->is_string() && *chem == "liion") {
            p.battery.chemistry = Chemistry::LiIon;
        } else {
            root.fail("battery/chemistry", "must be one of: lipo, liion");
        }
    }

    // Cross-field checks.
    const auto& b = p.battery;
    if (!(b.cutoffCellVoltage < b.criticalCellVoltage &&
          b.criticalCellVoltage < b.lowCellVoltage)) {
        root.fail("battery", "cell voltages must satisfy cut-off < critical < low");
    }
    const ChassisParams& c = p.chassis;
    if (!(c.clearanceM < c.cogHeightM && c.cogHeightM < c.clearanceM + c.heightM)) {
        root.fail("chassis/cogHeightM",
                  "the centre of mass must lie inside the body "
                  "(ground clearance < height < clearance + body height)");
    }
    if (p.staticCompressionM() > 0.9 * p.suspension.travelM) {
        root.fail("suspension/stiffnessNPerM",
                  std::format("the springs are too soft: {:.3f} m of compression under the "
                              "vehicle's weight for {:.3f} m of travel",
                              p.staticCompressionM(), p.suspension.travelM));
    }
    if (p.wheels.widthM >= p.wheels.trackWidthM) {
        root.fail("wheels/widthM", "wheels wider than the track width");
    }

    if (!root.ok()) {
        std::string text = root.errorText();
        for (std::size_t pos = text.find("~1"); pos != std::string::npos;
             pos = text.find("~1", pos)) {
            text.replace(pos, 2, "/");
        }
        return common::makeError("vehicle profile: " + text);
    }
    return p;
}

common::Result<GroundProfile> loadGroundProfile(const std::filesystem::path& file) {
    auto json = common::readJsonFile(file, std::size_t{1} << 20U);
    if (!json) {
        return std::unexpected(json.error());
    }
    auto profile = groundProfileFromJson(*json);
    if (!profile) {
        return common::makeError(std::format("{}: {}", file.string(), profile.error().message));
    }
    return profile;
}

Json groundProfileToJson(const GroundProfile& p) {
    Json j = Json::object();
    j["schemaVersion"] = 1;
    j["vehicleType"] = "ground";
    j["layout"] = driveLayoutName(p.layout);
    j["key"] = p.key;
    j["name"] = p.name;
    j["description"] = p.description;
    for (const GroundParamDescriptor& d : descriptors()) {
        const Json::json_pointer ptr{std::string(d.pointer)};
        if (d.kind == ParamKind::Integer) {
            j[ptr] = static_cast<int>(std::lround(d.get(p)));
        } else {
            j[ptr] = d.get(p);
        }
    }
    j["battery"]["chemistry"] = chemistryName(p.battery.chemistry);
    return j;
}

// ---------------------------------------------------------------------------------------------
// Derived figures (steady-state solutions of the drivetrain equations the simulation uses).
// ---------------------------------------------------------------------------------------------
namespace {

constexpr double kSeaLevelDensity = 1.225;

double kvRad(const DrivetrainParams& d) { return d.motorKv * common::kTwoPi / 60.0; }

// Torque at one wheel turning at @p wheelOmega with @p volts across the motor (full command).
double wheelTorque(const DrivetrainParams& d, double wheelOmega, double volts) {
    const double motorOmega = d.gearRatio * wheelOmega;
    const double current = std::clamp((volts - motorOmega / kvRad(d)) / d.motorResistanceOhm,
                                      -d.maxCurrentA, d.maxCurrentA);
    const double motorTorque = (current - d.motorNoLoadCurrentA) / kvRad(d);
    return d.gearRatio * d.gearEfficiency * motorTorque;
}

double resistanceForce(const GroundProfile& p, SurfaceMaterial material, double speed) {
    const double crr =
        surfaceProperties(material).rollingResistance * p.tyre.rollingResistanceScale;
    return crr * p.chassis.massKg * kGravity +
           0.5 * kSeaLevelDensity * p.chassis.dragAreaForwardM2 * speed * speed;
}

double packVoltage(const GroundProfile& p, double soc) {
    return p.battery.cellsInSeries * openCircuitCellVoltage(p.battery.chemistry, soc);
}

// Steady speed on level ground of @p material at full command (bisection on the wheel speed).
double topSpeed(const GroundProfile& p, SurfaceMaterial material, double volts) {
    const double n = p.wheelCount();
    const double r = p.wheels.radiusM;
    double lo = 0.0;
    double hi = volts * kvRad(p.drivetrain) / p.drivetrain.gearRatio;  // No-load wheel speed.
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        const double drive = n * wheelTorque(p.drivetrain, mid, volts) / r;
        (drive > resistanceForce(p, material, mid * r) ? lo : hi) = mid;
    }
    return lo * r;
}

// Battery current to hold @p speed on level ground of @p material.
double cruiseCurrent(const GroundProfile& p, SurfaceMaterial material, double speed, double volts) {
    const DrivetrainParams& d = p.drivetrain;
    const double n = p.wheelCount();
    const double r = p.wheels.radiusM;
    const double torque = resistanceForce(p, material, speed) * r / n;
    const double motorCurrent =
        torque / (d.gearRatio * d.gearEfficiency) * kvRad(d) + d.motorNoLoadCurrentA;
    const double motorVolts =
        motorCurrent * d.motorResistanceOhm + d.gearRatio * (speed / r) / kvRad(d);
    const double duty = std::clamp(motorVolts / volts, 0.0, 1.0);
    return n * duty * motorCurrent / p.electrical.escEfficiency + p.electrical.avionicsCurrentA;
}

}  // namespace

GroundStats computeGroundStats(const GroundProfile& p) {
    GroundStats s;
    const DrivetrainParams& d = p.drivetrain;
    const double n = p.wheelCount();
    const double r = p.wheels.radiusM;
    const double m = p.chassis.massKg;
    // Loaded pack voltage at 90 % charge with every motor near its current limit.
    const double sag = n * d.maxCurrentA * 0.5 * p.battery.cellResistanceOhm *
                       static_cast<double>(p.battery.cellsInSeries);
    const double volts = std::max(0.1, packVoltage(p, 0.9) - sag);

    s.topSpeedMs = topSpeed(p, SurfaceMaterial::Paved, volts);
    s.stallTorqueNm = wheelTorque(d, 0.0, volts);

    // 0 → 90 % of the top speed, with the wheels' and motors' rotating inertia and the
    // traction limit.
    {
        const double wheelInertia =
            p.wheels.inertiaKgM2 + d.motorInertiaKgM2 * d.gearRatio * d.gearRatio;
        const double effectiveMass = m + n * wheelInertia / (r * r);
        const double grip = surfaceProperties(SurfaceMaterial::Paved).peakFriction *
                            p.tyre.gripScale * m * kGravity;
        double v = 0.0;
        double t = 0.0;
        constexpr double kDt = 0.001;
        while (v < 0.9 * s.topSpeedMs && t < 60.0) {
            const double drive = std::min(n * wheelTorque(d, v / r, volts) / r, grip);
            v += kDt * (drive - resistanceForce(p, SurfaceMaterial::Paved, v)) / effectiveMass;
            t += kDt;
        }
        s.accelerationTimeS = t;
    }

    // Steepest climb per material: wheel slip (tan θ = μ) or motor torque at a crawl
    // (sin θ + C_rr·cos θ = n·T / (m·g·r)), whichever comes first.
    const double crawlTorque = wheelTorque(d, 0.1 / r, volts);
    for (const SurfaceMaterial material : allSurfaceMaterials()) {
        const SurfaceProperties sp = surfaceProperties(material);
        const double mu = sp.peakFriction * p.tyre.gripScale;
        const double crr = sp.rollingResistance * p.tyre.rollingResistanceScale;
        const double traction = std::atan(mu);
        const double k = n * crawlTorque / (m * kGravity * r);
        const double ratio = k / std::sqrt(1.0 + crr * crr);
        const double torque =
            ratio >= 1.0 ? common::kPi / 2.0 : std::max(0.0, std::asin(ratio) - std::atan(crr));
        s.slopes.push_back(
            {material, common::radToDeg(std::min(traction, torque)), traction <= torque});
    }
    s.rolloverSideDeg =
        common::radToDeg(std::atan(0.5 * p.wheels.trackWidthM / p.chassis.cogHeightM));
    s.rolloverPitchDeg =
        common::radToDeg(std::atan(0.5 * p.wheels.wheelbaseM / p.chassis.cogHeightM));

    // Range at 70 % of the top speed on grass, using 85 % of the capacity.
    s.cruiseSpeedMs = 0.7 * topSpeed(p, SurfaceMaterial::Grass, volts);
    s.cruiseCurrentA =
        cruiseCurrent(p, SurfaceMaterial::Grass, s.cruiseSpeedMs, packVoltage(p, 0.6));
    if (s.cruiseCurrentA > 0.0) {
        const double hours = 0.85 * p.battery.capacityAh / s.cruiseCurrentA;
        s.enduranceMin = hours * 60.0;
        s.rangeKm = s.cruiseSpeedMs * hours * 3.6;
    }
    return s;
}

}  // namespace ds::physics
