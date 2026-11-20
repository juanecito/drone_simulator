// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/sim/hud.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "ds/common/math.hpp"
#include "ds/geo/magnetic.hpp"
#include "ds/geo/utm.hpp"

namespace ds::sim {
namespace {

constexpr double kFeet = 0.3048;

std::string length(double metres, bool imperial, int decimals = 1) {
    return imperial ? std::format("{:.0f} ft", metres / kFeet)
                    : std::format("{:.{}f} m", metres, decimals);
}

std::string speedText(double ms, bool imperial) {
    return imperial ? std::format("{:.0f} mph", ms * 2.236936)
                    : std::format("{:.0f} km/h", ms * 3.6);
}

std::string dms(double degrees, char positive, char negative) {
    const double a = std::abs(degrees);
    const int d = static_cast<int>(a);
    const double m = (a - d) * 60.0;
    return std::format("{}°{:02d}'{:05.2f}\"{}", d, static_cast<int>(m),
                       (m - static_cast<int>(m)) * 60.0, degrees >= 0.0 ? positive : negative);
}

std::string clock(std::chrono::local_time<std::chrono::milliseconds> t) {
    const auto s = std::chrono::floor<std::chrono::seconds>(t);
    return std::format("{:%H:%M:%S}", s);
}

}  // namespace

std::string groupDigits(double value, int decimals) {
    std::string text = std::format("{:.{}f}", std::abs(value), decimals);
    const std::size_t dot = text.find('.');
    std::size_t end = dot == std::string::npos ? text.size() : dot;
    std::string grouped;
    std::string integer = text.substr(0, end);
    for (std::size_t i = 0; i < integer.size(); ++i) {
        if (i > 0 && (integer.size() - i) % 3 == 0) {
            grouped += ' ';
        }
        grouped += integer[i];
    }
    if (dot != std::string::npos) {
        grouped += text.substr(dot);
    }
    return value < 0.0 ? "-" + grouped : grouped;
}

PositionText formatPosition(const geo::Geodetic& p, double amslM, std::optional<double> aglM,
                            bool imperial, bool latLon) {
    PositionText t;
    if (auto utm = geo::toUtm(p.latitudeDeg, p.longitudeDeg)) {
        t.utm = std::format("UTM {}{}  {} E  {} N", utm->zone, utm->band,
                            groupDigits(utm->easting, 1), groupDigits(utm->northing, 1));
    } else {
        t.utm = "UTM —  (outside the UTM range)";
    }
    t.elevation = std::format("Elev {} AMSL", length(amslM, imperial));
    if (aglM) {
        t.elevation += std::format("   AGL {}", length(*aglM, imperial));
    }
    if (latLon) {
        t.latLon = std::format("{:.6f}°  {:.6f}°   {}  {}", p.latitudeDeg, p.longitudeDeg,
                               dms(p.latitudeDeg, 'N', 'S'), dms(p.longitudeDeg, 'E', 'W'));
    }
    return t;
}

HudCompass buildCompass(const geo::Geodetic& position, double trueHeadingDeg, astro::UtcTime utc) {
    HudCompass c;
    const geo::MagneticField field = geo::magneticField(position, geo::decimalYear(utc));
    c.trueHeadingDeg = common::normalizeDegrees(trueHeadingDeg);
    c.declinationDeg = field.declinationDeg;
    c.magneticHeadingDeg = common::normalizeDegrees(trueHeadingDeg - field.declinationDeg);
    // Rounded first so that 359.6° shows as 000°, not 360°.
    const auto degrees = [](double d) { return (std::lround(d) % 360 + 360) % 360; };
    c.heading.text = std::format("HDG {:03d}°T  {:03d}°M", degrees(c.trueHeadingDeg),
                                 degrees(c.magneticHeadingDeg));
    c.declination.text = std::format("DECL {:.1f}°{}", std::abs(field.declinationDeg),
                                     field.declinationDeg >= 0.0 ? 'E' : 'W');
    switch (geo::compassQuality(field)) {
        case geo::CompassQuality::Unreliable:
            c.declination = {"MAG UNRELIABLE", HudLevel::Warning};
            break;
        case geo::CompassQuality::Caution:
            c.declination.text += "  weak field";
            c.declination.level = HudLevel::Caution;
            break;
        case geo::CompassQuality::Good: break;
    }
    if (!field.dateValid && c.declination.level != HudLevel::Warning) {
        c.declination.text +=
            std::format("  ({} {:.0f}–{:.0f})", geo::magneticModelName(),
                        geo::magneticModelStartYear(), geo::magneticModelEndYear());
        c.declination.level = HudLevel::Caution;
    }
    return c;
}

namespace {

HudLevel slipLevel(double slip) {
    return slip >= kSlipWarning   ? HudLevel::Warning
           : slip >= kSlipCaution ? HudLevel::Caution
                                  : HudLevel::Normal;
}

// Ground drone read-outs and warnings (FR-GFX-20, FR-GND-07/08/09).
void buildGroundHud(const WorldSnapshot& s, const HudOptions& o, HudModel& h) {
    const physics::VehicleTelemetry& t = s.telemetry;
    const physics::GroundTelemetry& g = t.ground;
    HudGround ground;
    ground.pitch.text = std::format("PITCH {:+.0f}°", common::radToDeg(s.attitude.pitch));
    ground.roll.text = std::format("ROLL {:+.0f}°", common::radToDeg(s.attitude.roll));
    const double transfer = std::abs(g.loadTransfer);
    ground.loadTransfer = std::clamp(g.loadTransfer, -1.0, 1.0);
    ground.stability = g.rolledOver || transfer >= kRolloverWarning ? HudLevel::Warning
                       : transfer >= kRolloverCaution               ? HudLevel::Caution
                                                                    : HudLevel::Normal;
    ground.roll.level = ground.stability;
    ground.slope.text = std::format("SLOPE {:.0f}° {}", g.slopeDeg,
                                    physics::surfaceDescription(g.surface, g.cover));
    ground.slip.text = std::format("SLIP {:.0f}%", 100.0 * g.maxSlip);
    ground.slip.level = slipLevel(g.maxSlip);
    ground.odometer.text = "ODO " + length(g.odometerM, o.imperial, 0);
    ground.wheelCount = std::clamp(g.wheelCount, 0, physics::kMaxWheels);
    for (std::size_t i = 0; i < static_cast<std::size_t>(ground.wheelCount); ++i) {
        ground.wheelSlip[i] = g.slip[i];
        ground.wheelContact[i] = g.contact[i];
    }
    // Forward speed from the truth, signed: reversing shows "−".
    h.speed.text = (g.speedMs < -0.05 ? "R " : "") + speedText(std::abs(g.speedMs), o.imperial);
    if (g.mode == physics::DriveMode::SpeedControl && t.armed && !t.failsafe) {
        h.mode.text += " " + speedText(g.cruiseSetpointMs, o.imperial);
    }
    // The altitude items belong to aerial drones.
    h.altitudeAgl = {};
    h.altitudeAmsl = {};
    h.verticalSpeed = {};

    if (g.rolledOver) {
        h.warnings.push_back({s.mission.active ? "ROLLED OVER" : "ROLLED OVER — PRESS R TO RESET",
                              HudLevel::Warning});
    } else if (ground.stability == HudLevel::Warning) {
        h.warnings.push_back({"ROLLOVER RISK", HudLevel::Warning});
    } else if (ground.stability == HudLevel::Caution) {
        h.warnings.push_back({"ROLLOVER RISK", HudLevel::Caution});
    }
    if (g.inWater) {
        h.warnings.push_back({"IN WATER", HudLevel::Warning});
    } else if (g.waterDepthM > 0.0) {
        h.warnings.push_back(
            {std::format("WATER {:.0f} cm", 100.0 * g.waterDepthM), HudLevel::Caution});
    }
    if (g.stuckS >= 2.0) {
        h.warnings.push_back({"STUCK", HudLevel::Caution});
    }
    h.ground = ground;
}

}  // namespace

HudItem weatherItem(const WorldSnapshot& s, bool imperial) {
    const weather::WeatherConditions& c = s.weather;
    std::string text = "WX";
    // The lowest layer that shows (METAR reports every layer; one fits the OSD).
    for (const weather::CloudLayer& l : c.clouds) {
        if (const std::string_view code = weather::coverCode(l.coverOktas); !code.empty()) {
            text += std::format(" {} {}", code, length(l.baseM, imperial, 0));
            if (l.type == weather::CloudType::Cumulonimbus) {
                text += " CB";
            }
            break;
        }
    }
    if (text == "WX") {
        text += " SKC";
    }
    if (const std::string present = weather::presentWeatherCode(c); !present.empty()) {
        text += " " + present;
    }
    const double v = s.visibilityM;
    const std::string vis =
        imperial ? std::format("{:.1f} mi", v / 1609.344)
                 : (v < 5000.0 ? std::format("{:.0f} m", v) : std::format("{:.0f} km", v / 1000.0));
    text += " VIS " + vis;
    text += imperial ? std::format(" {:.0f} °F", c.air.temperatureC * 1.8 + 32.0)
                     : std::format(" {:.0f} °C", c.air.temperatureC);
    HudItem item{std::move(text)};
    const bool wet =
        c.precipitation.type != weather::PrecipitationType::None && c.precipitation.rateMmH > 0.0;
    if (v < kVisibilityWarningM || c.thunderstorm) {
        item.level = HudLevel::Warning;
    } else if (v < kVisibilityCautionM || wet) {
        item.level = HudLevel::Caution;
    }
    return item;
}

HudModel buildHud(const WorldSnapshot& s, const geo::LocalFrame& frame, const HudOptions& o) {
    HudModel h;
    const physics::VehicleTelemetry& t = s.telemetry;
    const physics::Vec3& pos = s.body.position;

    // Position panel: ground truth (FR-GFX-14a).
    const geo::Geodetic geodetic =
        frame.toGeodetic(geo::toEnu(geo::WorldPosition{pos.x, pos.y, pos.z}));
    const bool aerial = t.type == physics::VehicleType::Aerial;
    const PositionText p =
        formatPosition(geodetic, s.amslM(), aerial ? std::optional<double>(s.aglM) : std::nullopt,
                       o.imperial, o.showLatLon);
    h.positionUtm = p.utm;
    h.positionElevation = p.elevation;
    h.positionLatLon = p.latLon;

    h.altitudeAgl.text = "AGL " + length(s.aglM, o.imperial);
    h.altitudeAgl.level = s.aglM >= kAglWarningM   ? HudLevel::Warning
                          : s.aglM >= kAglCautionM ? HudLevel::Caution
                                                   : HudLevel::Normal;
    // OSD values come from the flight controller's sensors, as on a real OSD.
    h.altitudeAmsl.text = "ALT " + length(t.estimate.altitudeM, o.imperial, 0);
    const double ground = std::hypot(s.body.velocity.x, s.body.velocity.z);
    h.speed.text = speedText(ground, o.imperial);
    h.verticalSpeed.text = std::format("{:+.1f} m/s", t.estimate.verticalSpeedMs);
    if (aerial) {
        h.throttle.text = std::format("THR {:.0f}%", 100.0 * t.throttle);
        h.mode.text = std::string(physics::flightModeName(t.mode));
        if (t.rthStage) {
            h.mode.text += " " + std::string(physics::rthStageName(*t.rthStage));
            h.mode.level = HudLevel::Caution;
        }
    } else {
        // Ground: the drive command (the full ground HUD is FR-GFX-20). Parked, the holding
        // brake is always on: only a brake applied while armed is shown.
        h.throttle.text = t.armed && t.ground.braking
                              ? std::string("BRAKE")
                              : std::format("DRV {:+.0f}%", 100.0 * s.controls.drive);
        h.mode.text = std::string(physics::driveModeName(t.ground.mode));
    }
    if (t.failsafe) {
        h.mode.text = t.rthStage ? "FAILSAFE RTH " + std::string(physics::rthStageName(*t.rthStage))
                                 : "FAILSAFE";
        h.mode.level = HudLevel::Warning;
    }
    if (t.crashed) {
        h.armed = {"CRASHED", HudLevel::Warning};
    } else if (t.armed) {
        h.armed = {"ARMED", HudLevel::Caution};
    } else {
        h.armed = {"DISARMED", HudLevel::Normal};
    }
    const int cells = std::max(t.battery.cells, 1);
    h.battery.text =
        std::format("{:.1f} V  {:.2f} V/cell  {:.0f}%  {:.1f} A", t.battery.voltage,
                    t.battery.voltage / cells, 100.0 * t.battery.stateOfCharge, t.battery.current);
    switch (t.battery.warning) {
        case physics::BatteryWarning::None: break;
        case physics::BatteryWarning::Low:
            h.battery.level = HudLevel::Caution;
            h.warnings.push_back({"BATTERY LOW", HudLevel::Caution});
            break;
        case physics::BatteryWarning::Critical:
            h.battery.level = HudLevel::Warning;
            h.warnings.push_back(
                {aerial ? "BATTERY CRITICAL — LAND" : "BATTERY CRITICAL", HudLevel::Warning});
            break;
        case physics::BatteryWarning::Depleted:
            h.battery.level = HudLevel::Warning;
            h.warnings.push_back({"BATTERY EMPTY", HudLevel::Warning});
            break;
    }
    if (t.gps.fix) {
        h.gps.text = std::format("GPS {} sats  HDOP {:.1f}", t.gps.satellites, t.gps.hdop);
    } else {
        h.gps = {std::format("GPS no fix ({} sats)", t.gps.satellites), HudLevel::Caution};
    }
    if (t.estimate.positionValid) {
        const physics::Vec3 d = s.home - t.estimate.position;
        const double distance = std::hypot(d.x, d.z);
        const double bearing = common::normalizeDegrees(common::radToDeg(std::atan2(d.x, -d.z)));
        h.home.text = std::format("HOME {} {:03.0f}°", length(distance, o.imperial, 0), bearing);
    } else {
        h.home = {"HOME —", HudLevel::Caution};
    }
    if (o.pilotPosition) {
        const double d = glm::length(pos - *o.pilotPosition);
        h.pilotDistance.text = "PILOT " + length(d, o.imperial, 0);
    }
    h.headingDeg = common::normalizeDegrees(common::radToDeg(s.attitude.yaw));
    h.compass = buildCompass(geodetic, h.headingDeg, s.utc);
    h.rollRad = s.attitude.roll;
    h.pitchRad = s.attitude.pitch;
    if (o.showWind) {
        h.wind.text = s.windLevel == 0
                          ? "WIND 0 calm"
                          : std::format("WIND {} {:03.0f}° {} (gusts {})", s.windLevel,
                                        s.windFromDeg, speedText(s.windMeanSpeedMs, o.imperial),
                                        speedText(s.windGustMs, o.imperial));
        if (s.windLevel > 0) {
            // The wind blows towards fromDeg + 180°; relative to the nose, clockwise.
            h.windArrowRad =
                common::wrapRadiansPi(common::degToRad(s.windFromDeg + 180.0) - s.attitude.yaw);
        }
    }
    h.weather = weatherItem(s, o.imperial);
    const auto timer = static_cast<int>(t.armedTimeS);
    h.timer.text = std::format("{:02d}:{:02d}", timer / 60, timer % 60);
    h.replay = replayItem(s.replay);
    if (auto local = astro::toLocalTime(s.utc, o.timeZone)) {
        h.clockLocal.text = std::format("{} {}", clock(local->time), local->abbreviation);
    }
    h.clockUtc.text =
        clock(std::chrono::local_time<std::chrono::milliseconds>(s.utc.time_since_epoch())) +
        " UTC";
    h.lights.text = s.controls.lights ? "LIGHTS ON" : "LIGHTS OFF";

    if (t.armBlock != physics::ArmBlock::None && !t.armed) {
        h.warnings.push_back(
            {"CANNOT ARM: " + std::string(physics::armBlockText(t.armBlock)), HudLevel::Caution});
    }
    if (t.crashed) {
        h.warnings.push_back({s.mission.active ? "CRASHED" : "CRASHED — PRESS R TO RESET",
                              HudLevel::Warning});
    }
    if (t.failsafe) {
        h.warnings.push_back({!aerial      ? "RC LINK LOST — STOPPED"
                              : t.rthStage ? "RC LINK LOST — RETURNING HOME"
                                           : "RC LINK LOST — LANDING",
                              HudLevel::Warning});
    }
    if (aerial && t.armed && t.gpsModeUnavailable) {
        h.warnings.push_back(
            {s.controls.returnToHome ? "NO GPS — RTH UNAVAILABLE" : "NO GPS — ALTITUDE HOLD ONLY",
             HudLevel::Caution});
    }
    if (!aerial) {
        buildGroundHud(s, o, h);
    }
    // Flight rules (FR-MIS-06b, FR-MIS-06d): broken ones in red, close ones in amber.
    bool heightRule = false;
    for (const missions::RuleWarning& w : s.ruleWarnings) {
        heightRule = heightRule || w.rule == missions::FlightRule::Height;
        h.warnings.push_back({w.message, w.broken ? HudLevel::Warning : HudLevel::Caution});
    }
    if (aerial && h.altitudeAgl.level == HudLevel::Warning && !heightRule) {
        h.warnings.push_back({"ABOVE 120 m AGL", HudLevel::Warning});
    }
    if (o.terrainExtent) {
        const double edge = std::min(0.5 * o.terrainExtent->widthM - std::abs(pos.x),
                                     0.5 * o.terrainExtent->lengthM - std::abs(pos.z));
        if (edge < kTerrainEdgeCautionM) {
            h.warnings.push_back(
                {std::format("TERRAIN EDGE {}", length(std::max(edge, 0.0), o.imperial, 0)),
                 edge < kTerrainEdgeWarningM ? HudLevel::Warning : HudLevel::Caution});
        }
    }
    buildMissionHud(s.mission, pos, o, h);
    return h;
}

std::string missionClock(double seconds) {
    const double t = std::max(seconds, 0.0);
    const auto tenths = static_cast<long long>(std::floor(t * 10.0));
    return std::format("{}:{:02d}.{}", tenths / 600, static_cast<int>((tenths / 10) % 60),
                       static_cast<int>(tenths % 10));
}

void buildMissionHud(const MissionStatus& m, const physics::Vec3& vehicle, const HudOptions& o,
                     HudModel& h) {
    h.mission.reset();
    h.markers.clear();
    if (!m.active) {
        return;
    }
    using missions::MissionState;
    using missions::ObjectiveState;
    HudMission panel;
    panel.title = m.title;
    switch (m.state) {
        case MissionState::Briefing: panel.state = {"BRIEFING", HudLevel::Normal}; break;
        case MissionState::PreFlight:
            panel.state = {m.preflightProblems.empty() ? "PRE-FLIGHT — ARM TO START"
                                                       : "PRE-FLIGHT — NOT READY",
                           HudLevel::Caution};
            break;
        case MissionState::InProgress: panel.state = {"IN PROGRESS", HudLevel::Normal}; break;
        case MissionState::Succeeded:
        case MissionState::Debrief:
            panel.state = m.result && m.result->outcome != MissionState::Succeeded
                              ? HudItem{"MISSION ENDED", HudLevel::Warning}
                              : HudItem{"MISSION SUCCEEDED", HudLevel::Good};
            break;
        case MissionState::Failed:
            panel.state = {"MISSION FAILED" + (m.result && !m.result->failureReason.empty()
                                                   ? ": " + m.result->failureReason
                                                   : std::string()),
                           HudLevel::Warning};
            break;
        case MissionState::Aborted: panel.state = {"MISSION ABORTED", HudLevel::Warning}; break;
    }
    panel.timer.text = "T " + missionClock(m.elapsedS);
    if (m.timeLimitS) {
        panel.timer.text += " / " + missionClock(*m.timeLimitS);
        const double left = *m.timeLimitS - m.elapsedS;
        if (m.state == MissionState::InProgress && left < 30.0) {
            panel.timer.level = left < 10.0 ? HudLevel::Warning : HudLevel::Caution;
        }
    }
    for (const missions::ObjectiveStatus& obj : m.objectives) {
        HudItem line;
        std::string text = obj.title;
        if (obj.optional) {
            text += " (bonus)";
        }
        switch (obj.state) {
            case ObjectiveState::Completed:
                line = {"✓ " + text, HudLevel::Good};
                break;
            case ObjectiveState::Failed:
                line = {"✗ " + text + (obj.detail.empty() ? "" : " — " + obj.detail),
                        HudLevel::Warning};
                break;
            case ObjectiveState::Locked: line = {"· " + text, HudLevel::Muted}; break;
            case ObjectiveState::Active: {
                std::string extra = obj.detail;
                if (obj.targetPosition) {
                    const double d = glm::length(*obj.targetPosition - vehicle);
                    extra += (extra.empty() ? "" : ", ") + length(d, o.imperial, 0);
                }
                line = {"▸ " + text + (extra.empty() ? "" : " — " + extra), HudLevel::Normal};
                if (o.showMarkers && obj.targetPosition && m.state == MissionState::InProgress) {
                    const std::string name =
                        obj.targetName.empty() ? obj.title : obj.targetName;
                    h.markers.push_back(
                        {std::format("{} {}", name,
                                     length(glm::length(*obj.targetPosition - vehicle),
                                            o.imperial, 0)),
                         *obj.targetPosition});
                }
                break;
            }
        }
        panel.objectives.push_back(std::move(line));
    }
    for (const std::string& p : m.preflightProblems) {
        panel.notes.push_back({"Not ready: " + p, HudLevel::Caution});
    }
    if (m.payloadKg > 0.0) {
        panel.notes.push_back({std::format("Cargo {:.2f} kg", m.payloadKg), HudLevel::Normal});
    }
    h.mission = std::move(panel);
}

HudItem replayItem(const ReplayStatus& replay) {
    if (!replay.active) {
        return {};
    }
    const auto mmss = [](double seconds) {
        const auto t = static_cast<long long>(std::max(0.0, std::floor(seconds)));
        return std::format("{}:{:02d}", t / 60, t % 60);
    };
    if (replay.finished) {
        return {std::format("REPLAY ENDED {}", mmss(replay.durationS)), HudLevel::Caution};
    }
    return {std::format("REPLAY {} / {}", mmss(replay.timeS), mmss(replay.durationS)),
            HudLevel::Normal};
}

}  // namespace ds::sim
