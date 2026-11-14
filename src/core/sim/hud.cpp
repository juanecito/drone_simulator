// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/sim/hud.hpp"

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
    h.throttle.text = std::format("THR {:.0f}%", 100.0 * t.throttle);
    h.mode.text = std::string(physics::flightModeName(t.mode));
    if (t.failsafe) {
        h.mode.text = "FAILSAFE";
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
            h.warnings.push_back({"BATTERY CRITICAL — LAND", HudLevel::Warning});
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
    const auto timer = static_cast<int>(t.armedTimeS);
    h.timer.text = std::format("{:02d}:{:02d}", timer / 60, timer % 60);
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
        h.warnings.push_back({"CRASHED — PRESS R TO RESET", HudLevel::Warning});
    }
    if (t.failsafe) {
        h.warnings.push_back({"RC LINK LOST — LANDING", HudLevel::Warning});
    }
    if (h.altitudeAgl.level == HudLevel::Warning) {
        h.warnings.push_back({"ABOVE 120 m AGL", HudLevel::Warning});
    }
    return h;
}

}  // namespace ds::sim
