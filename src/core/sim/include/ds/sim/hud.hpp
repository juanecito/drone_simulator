// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "ds/geo/coordinates.hpp"
#include "ds/sim/world_snapshot.hpp"

/// @file
/// @brief Content of the position panel and the OSD (FR-GFX-14, FR-GFX-14a), computed from a
/// snapshot without any GUI code; the app only paints it.
/// @ingroup sim

namespace ds::sim {

/// @brief Colour class of an OSD value.
enum class HudLevel {
    Normal,   ///< White.
    Caution,  ///< Amber.
    Warning,  ///< Red.
    Good,     ///< Green (objective completed, mission succeeded).
    Muted,    ///< Grey (objective not available yet).
};

/// @brief One OSD value.
struct HudItem {
    std::string text;                   ///< Formatted text.
    HudLevel level = HudLevel::Normal;  ///< Colour.
};

/// @brief Display options.
struct HudOptions {
    bool imperial = false;         ///< Feet and mph instead of metres and km/h.
    bool showLatLon = false;       ///< Add latitude/longitude to the position panel.
    bool showWind = true;          ///< Missions may hide the wind indicator.
    std::string timeZone = "UTC";  ///< Zone of the local clock.
    /// Pilot position for line-of-sight flying (distance to pilot), if known.
    std::optional<physics::Vec3> pilotPosition;
    bool showMarkers = true;       ///< Guidance markers for the mission's targets.
};

/// @brief Compass rose (FR-GFX-14b): true and magnetic north and the vehicle's heading.
struct HudCompass {
    double trueHeadingDeg = 0.0;      ///< Heading from geographic (true) north, 0…360.
    double magneticHeadingDeg = 0.0;  ///< Heading from magnetic north, 0…360.
    double declinationDeg = 0.0;      ///< Magnetic north relative to true north, + east (WMM).
    HudItem heading;                  ///< "HDG 123°T  121°M".
    /// "DECL 2.1°W"; caution outside the WMM validity period or in a weak horizontal field,
    /// warning ("MAG UNRELIABLE") near the magnetic poles.
    HudItem declination;
};

/// @brief Mission panel (FR-GFX-14, FR-MIS-04): title, state, timer and objectives.
struct HudMission {
    std::string title;                ///< Mission title.
    HudItem state;                    ///< "PRE-FLIGHT — ARM TO START", "SUCCEEDED", …
    HudItem timer;                    ///< "T 1:23.4 / 5:00".
    std::vector<HudItem> objectives;  ///< One line per objective ("✓ Climb to 10 m").
    std::vector<HudItem> notes;       ///< Pre-flight problems, cargo carried.
};

/// @brief A guidance marker at a world position (the current objective's target), drawn
/// where it projects on screen (`rules.showMarkers`).
struct HudMarker {
    std::string label;         ///< "Beacon 45 m".
    glm::dvec3 position{0.0};  ///< World position.
};

/// @brief Ground drone part of the OSD (FR-GFX-20): attitude with the rollover warning, slope,
/// wheel slip and odometry.
struct HudGround {
    HudItem pitch;     ///< "PITCH +4°".
    HudItem roll;      ///< "ROLL −3°"; amber/red as the load transfer nears the tipping point.
    HudItem slope;     ///< "SLOPE 12° grass".
    HudItem slip;      ///< "SLIP 8%"; amber past the tyres' peak grip, red when spinning.
    HudItem odometer;  ///< "ODO 123 m" (wheel encoders).
    /// Lateral load transfer (N_right − N_left) / ΣN: ±1 = one side's wheels lifting.
    double loadTransfer = 0.0;
    HudLevel stability = HudLevel::Normal;  ///< Level of the load transfer.
    int wheelCount = 0;                     ///< Wheels (left side first, front to rear).
    std::array<double, physics::kMaxWheels> wheelSlip{};   ///< Combined slip per wheel.
    std::array<bool, physics::kMaxWheels> wheelContact{};  ///< Wheel on the ground.
};

/// @brief Load transfer above which the rollover warning is amber.
inline constexpr double kRolloverCaution = 0.6;
/// @brief Load transfer above which it is red (the inner wheels are about to lift).
inline constexpr double kRolloverWarning = 0.85;
/// @brief Combined slip above which a tyre is past its peak grip (amber).
inline constexpr double kSlipCaution = 0.2;
/// @brief Combined slip of a spinning or locked tyre (red).
inline constexpr double kSlipWarning = 0.5;

/// @brief Everything drawn over the 3D view.
struct HudModel {
    std::string positionUtm;        ///< "UTM 30T  440 123.4 E  4 474 567.8 N".
    std::string positionElevation;  ///< "Elev 712.4 m AMSL   AGL 35.2 m".
    std::string positionLatLon;     ///< Latitude/longitude (empty unless enabled).
    HudItem altitudeAgl;            ///< AGL (amber ≥ 100 m, red ≥ 120 m).
    HudItem altitudeAmsl;           ///< AMSL.
    HudItem speed;                  ///< Horizontal speed.
    HudItem verticalSpeed;          ///< Climb rate.
    HudItem throttle;               ///< Throttle %.
    HudItem mode;                   ///< Flight mode.
    HudItem armed;                  ///< ARMED / DISARMED / CRASHED.
    HudItem battery;                ///< Voltage, %, current.
    HudItem gps;                    ///< Satellites and HDOP.
    HudItem home;                   ///< Distance and bearing to home.
    HudItem pilotDistance;          ///< Distance to the pilot (empty without a pilot position).
    HudItem wind;                   ///< Wind level and speed (empty when hidden).
    /// Weather (FR-WX-10): METAR-style sky cover of the lowest layer, present weather,
    /// visibility at the vehicle and the temperature, e.g. "WX BKN 900 m -RA VIS 6.0 km 12 °C".
    /// Amber under 5 km visibility or with precipitation, red under 1 km or in a thunderstorm.
    HudItem weather;
    HudItem timer;                  ///< Flight timer.
    HudItem clockLocal;             ///< Local time.
    HudItem clockUtc;               ///< UTC.
    HudItem lights;                 ///< Lights indicator.
    std::vector<HudItem> warnings;  ///< Flashing warnings.
    double rollRad = 0.0;           ///< Artificial horizon.
    double pitchRad = 0.0;          ///< Artificial horizon.
    double headingDeg = 0.0;        ///< True heading (same as compass.trueHeadingDeg).
    HudCompass compass;             ///< Compass rose.
    /// Direction the wind blows towards, relative to the drone's nose (radians, clockwise).
    std::optional<double> windArrowRad;
    std::optional<HudMission> mission;  ///< Mission panel, while a mission is flown.
    /// Ground drone read-outs (empty for aerial drones, whose altitude items are used instead).
    std::optional<HudGround> ground;
    std::vector<HudMarker> markers;     ///< Guidance markers.
};

/// @brief Visibility under which the HUD weather is amber, m.
inline constexpr double kVisibilityCautionM = 5000.0;
/// @brief Visibility under which the HUD weather is red, m.
inline constexpr double kVisibilityWarningM = 1000.0;

/// @brief The HUD weather item for @p snapshot (part of buildHud()).
[[nodiscard]] HudItem weatherItem(const WorldSnapshot& snapshot, bool imperial);

/// @brief AGL thresholds of the flight-rule indicator (FR-GFX-14).
inline constexpr double kAglCautionM = 100.0;
/// @brief AGL red threshold (120 m rule).
inline constexpr double kAglWarningM = 120.0;

/// @brief Builds the HUD for @p snapshot; @p frame converts world positions to geodetic.
/// @details The declination comes from the World Magnetic Model at the vehicle's position and
/// the snapshot's UTC (FR-GEO-05).
[[nodiscard]] HudModel buildHud(const WorldSnapshot& snapshot, const geo::LocalFrame& frame,
                                const HudOptions& options);

/// @brief Mission panel and guidance markers for @p mission, seen from @p vehiclePosition.
/// Leaves @p hud.mission empty when no mission is active.
void buildMissionHud(const MissionStatus& mission, const physics::Vec3& vehiclePosition,
                     const HudOptions& options, HudModel& hud);

/// @brief "1:23.4" (minutes, seconds and tenths).
[[nodiscard]] std::string missionClock(double seconds);

/// @brief Position panel lines only (also used by the status bar, FR-UI-08a):
/// UTM, elevation/AGL, lat/lon. @p aglM is omitted for ground vehicles (empty optional).
struct PositionText {
    std::string utm;        ///< UTM line.
    std::string elevation;  ///< Elevation line.
    std::string latLon;     ///< Lat/lon line (empty unless requested).
};
/// @brief Formats a position (FR-GFX-14a, NFR-REAL-07).
[[nodiscard]] PositionText formatPosition(const geo::Geodetic& position, double amslM,
                                          std::optional<double> aglM, bool imperial, bool latLon);

/// @brief Compass content for a vehicle at @p position heading @p trueHeadingDeg at
/// @p utc (FR-GFX-14b).
[[nodiscard]] HudCompass buildCompass(const geo::Geodetic& position, double trueHeadingDeg,
                                      astro::UtcTime utc);

/// @brief Groups digits by three with a thin space: 4474567.8 → "4 474 567.8".
[[nodiscard]] std::string groupDigits(double value, int decimals);

}  // namespace ds::sim
