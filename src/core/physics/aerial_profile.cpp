// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/aerial_profile.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

#include "ds/common/math.hpp"
#include "ds/physics/aerodynamics.hpp"

namespace ds::physics {
namespace {

using common::Json;

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define DS_NUMBER(ptr, group, label, unit, lo, hi, dec, member)  \
    ParamDescriptor {                                            \
        ptr, group, label, unit, lo, hi, dec, ParamKind::Number, \
            [](const AerialProfile& p) { return p.member; },     \
            [](AerialProfile& p, double v) { p.member = v; }     \
    }
#define DS_INTEGER(ptr, group, label, unit, lo, hi, member)                                 \
    ParamDescriptor {                                                                       \
        ptr, group, label, unit, lo, hi, 0, ParamKind::Integer,                             \
            [](const AerialProfile& p) { return static_cast<double>(p.member); },           \
            [](AerialProfile& p, double v) { p.member = static_cast<int>(std::lround(v)); } \
    }
// NOLINTEND(cppcoreguidelines-macro-usage)

const std::vector<ParamDescriptor>& descriptors() {
    static const std::vector<ParamDescriptor> kList = {
        // Frame
        DS_NUMBER("/frame/massKg", "Frame", "All-up weight", "kg", 0.05, 25.0, 3, frame.massKg),
        DS_NUMBER("/frame/armLengthM", "Frame", "Motor distance from centre", "m", 0.03, 1.0, 3,
                  frame.armLengthM),
        DS_NUMBER("/frame/inertiaXxKgM2", "Frame", "Roll inertia", "kg·m²", 1e-6, 5.0, 6,
                  frame.inertiaXxKgM2),
        DS_NUMBER("/frame/inertiaYyKgM2", "Frame", "Pitch inertia", "kg·m²", 1e-6, 5.0, 6,
                  frame.inertiaYyKgM2),
        DS_NUMBER("/frame/inertiaZzKgM2", "Frame", "Yaw inertia", "kg·m²", 1e-6, 5.0, 6,
                  frame.inertiaZzKgM2),
        DS_NUMBER("/frame/dragAreaForwardM2", "Frame", "Drag area forward (C_d·A)", "m²", 0.0, 1.0,
                  4, frame.dragAreaForwardM2),
        DS_NUMBER("/frame/dragAreaSideM2", "Frame", "Drag area sideways (C_d·A)", "m²", 0.0, 1.0, 4,
                  frame.dragAreaSideM2),
        DS_NUMBER("/frame/dragAreaVerticalM2", "Frame", "Drag area vertical (C_d·A)", "m²", 0.0,
                  2.0, 4, frame.dragAreaVerticalM2),
        DS_NUMBER("/frame/propHeightM", "Frame", "Propeller plane above CoG", "m", 0.0, 0.3, 3,
                  frame.propHeightM),
        DS_NUMBER("/frame/gearHeightM", "Frame", "Landing gear below CoG", "m", 0.005, 0.5, 3,
                  frame.gearHeightM),
        DS_NUMBER("/frame/crashSpeedMs", "Frame", "Crash impact speed", "m/s", 0.5, 30.0, 1,
                  frame.crashSpeedMs),
        // Motors
        DS_NUMBER("/motor/kv", "Motors", "KV", "rpm/V", 100.0, 10000.0, 0, motor.kv),
        DS_NUMBER("/motor/resistanceOhm", "Motors", "Winding resistance", "Ω", 0.001, 2.0, 3,
                  motor.resistanceOhm),
        DS_NUMBER("/motor/noLoadCurrentA", "Motors", "No-load current", "A", 0.0, 10.0, 2,
                  motor.noLoadCurrentA),
        DS_NUMBER("/motor/timeConstantS", "Motors", "Spin-up time constant", "s", 0.005, 0.5, 3,
                  motor.timeConstantS),
        DS_NUMBER("/motor/rotorInertiaKgM2", "Motors", "Rotor + propeller inertia", "kg·m²", 1e-8,
                  1e-2, 8, motor.rotorInertiaKgM2),
        DS_NUMBER("/motor/maxCurrentA", "Motors", "Rated peak current", "A", 1.0, 300.0, 0,
                  motor.maxCurrentA),
        DS_NUMBER("/motor/massKg", "Motors", "Motor mass", "kg", 0.001, 2.0, 3, motor.massKg),
        // Propellers
        DS_NUMBER("/propeller/diameterM", "Propellers", "Diameter", "m", 0.03, 1.0, 4,
                  propeller.diameterM),
        DS_NUMBER("/propeller/pitchM", "Propellers", "Pitch", "m", 0.01, 1.0, 4, propeller.pitchM),
        DS_INTEGER("/propeller/blades", "Propellers", "Blades", "", 2, 6, propeller.blades),
        DS_NUMBER("/propeller/thrustCoefficient", "Propellers", "Thrust coefficient C_T", "", 0.01,
                  0.5, 4, propeller.thrustCoefficient),
        DS_NUMBER("/propeller/torqueCoefficient", "Propellers", "Torque coefficient C_Q", "",
                  0.0005, 0.1, 5, propeller.torqueCoefficient),
        DS_NUMBER("/propeller/zeroThrustAdvanceRatio", "Propellers", "Advance ratio of zero thrust",
                  "", 0.1, 2.0, 2, propeller.zeroThrustAdvanceRatio),
        DS_NUMBER("/propeller/rotorDragCoefficient", "Propellers", "Rotor drag coefficient", "",
                  0.0, 3.0, 2, propeller.rotorDragCoefficient),
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
        DS_NUMBER("/electrical/avionicsCurrentA", "Battery", "Avionics current", "A", 0.0, 20.0, 2,
                  electrical.avionicsCurrentA),
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
        // Rates
        DS_NUMBER("/controller/rates/roll/centerDps", "Rates", "Roll centre rate", "°/s", 10.0,
                  2000.0, 0, controller.rates[0].centerDps),
        DS_NUMBER("/controller/rates/roll/maxDps", "Rates", "Roll max rate", "°/s", 10.0, 2000.0, 0,
                  controller.rates[0].maxDps),
        DS_NUMBER("/controller/rates/roll/expo", "Rates", "Roll expo", "", 0.0, 1.0, 2,
                  controller.rates[0].expo),
        DS_NUMBER("/controller/rates/pitch/centerDps", "Rates", "Pitch centre rate", "°/s", 10.0,
                  2000.0, 0, controller.rates[1].centerDps),
        DS_NUMBER("/controller/rates/pitch/maxDps", "Rates", "Pitch max rate", "°/s", 10.0, 2000.0,
                  0, controller.rates[1].maxDps),
        DS_NUMBER("/controller/rates/pitch/expo", "Rates", "Pitch expo", "", 0.0, 1.0, 2,
                  controller.rates[1].expo),
        DS_NUMBER("/controller/rates/yaw/centerDps", "Rates", "Yaw centre rate", "°/s", 10.0,
                  2000.0, 0, controller.rates[2].centerDps),
        DS_NUMBER("/controller/rates/yaw/maxDps", "Rates", "Yaw max rate", "°/s", 10.0, 2000.0, 0,
                  controller.rates[2].maxDps),
        DS_NUMBER("/controller/rates/yaw/expo", "Rates", "Yaw expo", "", 0.0, 1.0, 2,
                  controller.rates[2].expo),
        // PID
        DS_NUMBER("/controller/pid/roll/p", "PID", "Roll P", "", 0.0, 2.0, 4, controller.pid[0].p),
        DS_NUMBER("/controller/pid/roll/i", "PID", "Roll I", "", 0.0, 5.0, 4, controller.pid[0].i),
        DS_NUMBER("/controller/pid/roll/d", "PID", "Roll D", "", 0.0, 0.1, 5, controller.pid[0].d),
        DS_NUMBER("/controller/pid/pitch/p", "PID", "Pitch P", "", 0.0, 2.0, 4,
                  controller.pid[1].p),
        DS_NUMBER("/controller/pid/pitch/i", "PID", "Pitch I", "", 0.0, 5.0, 4,
                  controller.pid[1].i),
        DS_NUMBER("/controller/pid/pitch/d", "PID", "Pitch D", "", 0.0, 0.1, 5,
                  controller.pid[1].d),
        DS_NUMBER("/controller/pid/yaw/p", "PID", "Yaw P", "", 0.0, 2.0, 4, controller.pid[2].p),
        DS_NUMBER("/controller/pid/yaw/i", "PID", "Yaw I", "", 0.0, 5.0, 4, controller.pid[2].i),
        DS_NUMBER("/controller/pid/yaw/d", "PID", "Yaw D", "", 0.0, 0.1, 5, controller.pid[2].d),
        DS_NUMBER("/controller/dTermCutoffHz", "PID", "D-term filter", "Hz", 5.0, 500.0, 0,
                  controller.dTermCutoffHz),
        // Flight modes
        DS_NUMBER("/controller/maxAngleDeg", "Flight modes", "Angle mode tilt limit", "°", 5.0,
                  85.0, 0, controller.maxAngleDeg),
        DS_NUMBER("/controller/levelGain", "Flight modes", "Angle mode level gain", "1/s", 0.5,
                  20.0, 1, controller.levelGain),
        DS_NUMBER("/controller/maxClimbMs", "Flight modes", "Altitude hold max climb", "m/s", 0.5,
                  15.0, 1, controller.maxClimbMs),
        DS_NUMBER("/controller/maxDescentMs", "Flight modes", "Altitude hold max descent", "m/s",
                  0.5, 15.0, 1, controller.maxDescentMs),
        DS_NUMBER("/controller/altitudeGain", "Flight modes", "Altitude hold position gain", "1/s",
                  0.1, 5.0, 2, controller.altitudeGain),
        DS_NUMBER("/controller/climbP", "Flight modes", "Altitude hold climb P", "", 0.0, 1.0, 3,
                  controller.climbP),
        DS_NUMBER("/controller/climbI", "Flight modes", "Altitude hold climb I", "", 0.0, 1.0, 3,
                  controller.climbI),
        DS_NUMBER("/controller/idleThrottle", "Flight modes", "Idle throttle", "", 0.0, 0.2, 3,
                  controller.idleThrottle),
        DS_NUMBER("/controller/failsafeDescentMs", "Flight modes", "Failsafe landing speed", "m/s",
                  0.3, 5.0, 1, controller.failsafeDescentMs),
        DS_NUMBER("/controller/failsafeDelayS", "Flight modes", "Failsafe delay", "s", 0.1, 10.0, 1,
                  controller.failsafeDelayS),
        // Camera
        DS_NUMBER("/camera/uptiltDeg", "FPV camera", "Camera uptilt", "°", 0.0, 60.0, 0,
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

std::array<Vec3, AerialProfile::kMotorCount> AerialProfile::motorPositions() const noexcept {
    const double a = frame.armLengthM / std::sqrt(2.0);
    const double z = -frame.propHeightM;
    return {Vec3(a, a, z), Vec3(-a, a, z), Vec3(-a, -a, z), Vec3(a, -a, z)};
}

std::span<const ParamDescriptor> aerialParameters() { return descriptors(); }

common::Result<AerialProfile> aerialProfileFromJson(const Json& json) {
    common::JsonReader root(json);
    AerialProfile p;
    if (!json.is_object()) {
        return common::makeError("vehicle profile: the document must be a JSON object");
    }
    root.integer("schemaVersion", 1, 1);
    const std::string type = root.choice("vehicleType", {"aerial"});
    (void)type;
    p.key = root.string("key");
    p.name = root.string("name");
    p.description = root.string("description", "");
    p.modelFile = root.string("modelFile", "");
    if (root.has("layout")) {
        root.choice("layout", {"quad-x"});
    }

    // Numeric parameters: absent ones keep their defaults (forward compatibility).
    for (const ParamDescriptor& d : descriptors()) {
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
    // Pointer errors are reported relative to the root: "/motor/kv: …" (strip the
    // escaping JsonReader adds for '/').
    if (const Json* chem = findPointer(json, "/battery/chemistry"); chem != nullptr) {
        if (chem->is_string() && *chem == "lipo") {
            p.battery.chemistry = Chemistry::LiPo;
        } else if (chem->is_string() && *chem == "liion") {
            p.battery.chemistry = Chemistry::LiIon;
        } else {
            root.fail("battery/chemistry", "must be one of: lipo, liion");
        }
    }
    if (const Json* airmode = findPointer(json, "/controller/airmode"); airmode != nullptr) {
        if (airmode->is_boolean()) {
            p.controller.airmode = airmode->get<bool>();
        } else {
            root.fail("controller/airmode", "must be true or false");
        }
    }
    if (auto ref = root.optionalObject("reference")) {
        ReferenceFigures r;
        r.hoverThrottle = ref->number("hoverThrottle", 0.01, 1.0);
        r.thrustToWeight = ref->number("thrustToWeight", 1.0, 30.0);
        r.hoverEnduranceMin = ref->number("hoverEnduranceMin", 0.5, 300.0);
        r.maxLevelSpeedMs = ref->number("maxLevelSpeedMs", 1.0, 100.0);
        r.source = ref->string("source", "");
        p.reference = r;
    }
    // Cross-field checks.
    const auto& b = p.battery;
    if (!(b.cutoffCellVoltage < b.criticalCellVoltage &&
          b.criticalCellVoltage < b.lowCellVoltage)) {
        root.fail("battery", "cell voltages must satisfy cut-off < critical < low");
    }
    if (p.propeller.diameterM > 2.0 * p.frame.armLengthM * 1.05) {
        root.fail("propeller/diameterM", "propellers would overlap (diameter > 2 × arm length)");
    }

    if (!root.ok()) {
        std::string text = root.errorText();
        // JsonReader escapes '/' inside keys as "~1"; show plain JSON pointers instead.
        for (std::size_t pos = text.find("~1"); pos != std::string::npos;
             pos = text.find("~1", pos)) {
            text.replace(pos, 2, "/");
        }
        return common::makeError("vehicle profile: " + text);
    }
    return p;
}

common::Result<AerialProfile> loadAerialProfile(const std::filesystem::path& file) {
    auto json = common::readJsonFile(file, std::size_t{1} << 20U);
    if (!json) {
        return std::unexpected(json.error());
    }
    auto profile = aerialProfileFromJson(*json);
    if (!profile) {
        return common::makeError(std::format("{}: {}", file.string(), profile.error().message));
    }
    return profile;
}

Json aerialProfileToJson(const AerialProfile& p) {
    Json j = Json::object();
    j["schemaVersion"] = 1;
    j["vehicleType"] = "aerial";
    j["layout"] = "quad-x";
    j["key"] = p.key;
    j["name"] = p.name;
    j["description"] = p.description;
    if (!p.modelFile.empty()) {
        j["modelFile"] = p.modelFile;
    }
    for (const ParamDescriptor& d : descriptors()) {
        const Json::json_pointer ptr{std::string(d.pointer)};
        if (d.kind == ParamKind::Integer) {
            j[ptr] = static_cast<int>(std::lround(d.get(p)));
        } else {
            j[ptr] = d.get(p);
        }
    }
    j["battery"]["chemistry"] = chemistryName(p.battery.chemistry);
    j["controller"]["airmode"] = p.controller.airmode;
    if (p.reference) {
        j["reference"] = {{"hoverThrottle", p.reference->hoverThrottle},
                          {"thrustToWeight", p.reference->thrustToWeight},
                          {"hoverEnduranceMin", p.reference->hoverEnduranceMin},
                          {"maxLevelSpeedMs", p.reference->maxLevelSpeedMs},
                          {"source", p.reference->source}};
    }
    return j;
}

// ---------------------------------------------------------------------------------------------
// Derived figures (steady-state solutions of the same equations the simulation integrates).
// ---------------------------------------------------------------------------------------------
namespace {

struct OperatingPoint {
    double duty = 0.0;
    double omega = 0.0;
    double thrustPerRotor = 0.0;
    double batteryCurrent = 0.0;
    double batteryVoltage = 0.0;
};

// Command headroom kept for attitude control when holding position in wind.
constexpr double kWindDutyLimit = 0.85;

// Rotor + battery equilibrium at motor command @p duty (all four motors equal).
OperatingPoint atDuty(const AerialProfile& p, double duty, double axialSpeed, double rho,
                      double soc, bool steadyPolarisation) {
    const double rPack =
        p.battery.cellResistanceOhm * p.battery.cellsInSeries * (steadyPolarisation ? 1.5 : 1.0);
    const double ocv = p.battery.cellsInSeries * openCircuitCellVoltage(p.battery.chemistry, soc);
    OperatingPoint op;
    op.duty = duty;
    double current = p.electrical.avionicsCurrentA;
    for (int i = 0; i < 40; ++i) {
        const double v = std::max(0.0, ocv - current * rPack);
        op.batteryVoltage = v;
        op.omega = steadyStateOmega(p.motor, p.propeller, duty * v, rho, axialSpeed);
        const RotorOutput r = propellerLoads(p.propeller, op.omega, rho, axialSpeed);
        op.thrustPerRotor = r.thrustN;
        const double motor = motorCurrent(p.motor, op.omega, r.torqueNm, 0.0);
        const double next = static_cast<double>(AerialProfile::kMotorCount) * duty * motor /
                                p.electrical.escEfficiency +
                            p.electrical.avionicsCurrentA;
        const double updated = 0.5 * (current + next);  // Damped fixed point.
        const bool converged = std::abs(updated - current) < 1e-6 * (1.0 + current);
        current = updated;
        if (converged) {
            break;
        }
    }
    op.batteryCurrent = current;
    return op;
}

// Smallest duty producing @p thrustPerRotor (bisection); duty > 1 if impossible.
OperatingPoint forThrust(const AerialProfile& p, double thrustPerRotor, double axialSpeed,
                         double rho, double soc) {
    if (atDuty(p, 1.0, axialSpeed, rho, soc, true).thrustPerRotor < thrustPerRotor) {
        OperatingPoint op = atDuty(p, 1.0, axialSpeed, rho, soc, true);
        op.duty = 1.01;
        return op;
    }
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 24; ++i) {  // 6·10⁻⁸ resolution.
        const double mid = 0.5 * (lo + hi);
        if (atDuty(p, mid, axialSpeed, rho, soc, true).thrustPerRotor < thrustPerRotor) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return atDuty(p, hi, axialSpeed, rho, soc, true);
}

// Net forces in the vertical plane for level flight at airspeed @p v with forward tilt @p tilt
// and per-rotor thrust/speed (x = forward horizontal, y = up).
struct PlaneForces {
    double horizontal;
    double vertical;
};

PlaneForces levelForces(const AerialProfile& p, double v, double tilt, double thrustPerRotor,
                        double omega, double rho) {
    // Air velocity relative to the vehicle in the body frame (nose-down tilt θ, flying forward).
    const double c = std::cos(tilt);
    const double s = std::sin(tilt);
    const Vec3 airBody(v * c, 0.0, -v * s);
    const Vec3 drag = bodyDrag(p.frame, airBody, rho) +
                      static_cast<double>(AerialProfile::kMotorCount) *
                          rotorDrag(p.propeller, thrustPerRotor, omega, airBody);
    const double thrust = static_cast<double>(AerialProfile::kMotorCount) * thrustPerRotor;
    // Body x = (c, −s), body z = (−s, −c) in (forward, up); thrust along −z.
    const double fx = thrust * s + drag.x * c - drag.z * s;
    const double fy = thrust * c - drag.x * s - drag.z * c - p.frame.massKg * kGravity;
    return {fx, fy};
}

}  // namespace

WindHoldState windHoldState(const AerialProfile& p, double windMs, double rho, double soc) {
    // Hovering in a wind W is level flight at airspeed W: find the tilt where the horizontal
    // force vanishes with the thrust that balances the weight.
    WindHoldState out;
    const double maxTilt = common::degToRad(p.controller.maxAngleDeg);
    auto solveAtTilt = [&](double tilt, OperatingPoint& op) {
        // Thrust that makes the vertical force zero (fixed point: the thrust changes the rotor
        // speed, which changes rotor drag and the advance ratio).
        double thrust = p.frame.massKg * kGravity /
                        static_cast<double>(AerialProfile::kMotorCount) / std::cos(tilt);
        for (int i = 0; i < 30; ++i) {
            op = forThrust(p, thrust, windMs * std::sin(tilt), rho, soc);
            const PlaneForces f = levelForces(p, windMs, tilt, thrust, op.omega, rho);
            const double step =
                f.vertical / (static_cast<double>(AerialProfile::kMotorCount) * std::cos(tilt));
            thrust = std::max(thrust - step, 0.0);
            if (std::abs(step) < 1e-5 * (1.0 + thrust)) {
                break;
            }
        }
        op = forThrust(p, thrust, windMs * std::sin(tilt), rho, soc);
        return levelForces(p, windMs, tilt, thrust, op.omega, rho).horizontal;
    };
    OperatingPoint op;
    double lo = 0.0;
    double hi = common::degToRad(85.0);
    for (int i = 0; i < 24; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (solveAtTilt(mid, op) < 0.0) {
            lo = mid;  // Drag still wins: tilt more.
        } else {
            hi = mid;
        }
    }
    solveAtTilt(hi, op);
    out.tiltDeg = common::radToDeg(hi);
    out.throttle = op.duty;
    out.currentA = op.batteryCurrent;
    out.powerW = op.batteryCurrent * op.batteryVoltage;
    out.feasible = op.duty <= kWindDutyLimit && hi <= maxTilt;
    return out;
}

AerialStats computeAerialStats(const AerialProfile& p) {
    constexpr double kRho = 1.225;
    AerialStats s;
    const double weight = p.frame.massKg * kGravity;
    const OperatingPoint hover =
        forThrust(p, weight / static_cast<double>(AerialProfile::kMotorCount), 0.0, kRho, 0.95);
    s.hoverThrottle = hover.duty;
    s.hoverCurrentA = hover.batteryCurrent;
    s.hoverPowerW = hover.batteryCurrent * hover.batteryVoltage;
    const OperatingPoint full = atDuty(p, 1.0, 0.0, kRho, 1.0, false);
    s.maxThrustN = static_cast<double>(AerialProfile::kMotorCount) * full.thrustPerRotor;
    s.thrustToWeight = s.maxThrustN / weight;

    // Hover endurance: discharge at the hover load until empty or at the cut-off voltage.
    {
        double consumedAh = 0.0;
        double t = 0.0;
        const double dt = 5.0;
        for (; t < 4.0 * 3600.0; t += dt) {
            const double soc = 1.0 - consumedAh / p.battery.capacityAh;
            if (soc <= 0.0) {
                break;
            }
            const OperatingPoint op = forThrust(
                p, weight / static_cast<double>(AerialProfile::kMotorCount), 0.0, kRho, soc);
            if (op.duty > 1.0 ||
                op.batteryVoltage / p.battery.cellsInSeries <= p.battery.cutoffCellVoltage) {
                break;
            }
            consumedAh += op.batteryCurrent * dt / 3600.0;
        }
        s.hoverEnduranceMin = t / 60.0;
    }

    // Top speed and strongest wind: the airspeed at which holding altitude needs either the
    // tilt limit of the Angle mode or the available command (full command for the speed;
    // 85 % for the wind, keeping headroom for attitude control).
    auto fastest = [&](double dutyLimit) {
        double lo = 0.0;
        double hi = 80.0;
        for (int i = 0; i < 18; ++i) {  // 0.3 mm/s resolution.
            const double mid = 0.5 * (lo + hi);
            const WindHoldState w = windHoldState(p, mid, kRho, 0.9);
            if (w.tiltDeg <= p.controller.maxAngleDeg && w.throttle <= dutyLimit) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return lo;
    };
    s.maxLevelSpeedMs = fastest(1.0);
    s.maxWindMs = fastest(kWindDutyLimit);
    s.maxWindTiltDeg = windHoldState(p, s.maxWindMs, kRho, 0.9).tiltDeg;
    return s;
}

}  // namespace ds::physics
