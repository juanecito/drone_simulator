// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

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
enum class HudLevel { Normal, Caution, Warning };

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
};

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
    HudItem timer;                  ///< Flight timer.
    HudItem clockLocal;             ///< Local time.
    HudItem clockUtc;               ///< UTC.
    HudItem lights;                 ///< Lights indicator.
    std::vector<HudItem> warnings;  ///< Flashing warnings.
    double rollRad = 0.0;           ///< Artificial horizon.
    double pitchRad = 0.0;          ///< Artificial horizon.
    double headingDeg = 0.0;        ///< Compass.
    /// Direction the wind blows towards, relative to the drone's nose (radians, clockwise).
    std::optional<double> windArrowRad;
};

/// @brief AGL thresholds of the flight-rule indicator (FR-GFX-14).
inline constexpr double kAglCautionM = 100.0;
/// @brief AGL red threshold (120 m rule).
inline constexpr double kAglWarningM = 120.0;

/// @brief Builds the HUD for @p snapshot; @p frame converts world positions to geodetic.
[[nodiscard]] HudModel buildHud(const WorldSnapshot& snapshot, const geo::LocalFrame& frame,
                                const HudOptions& options);

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

/// @brief Groups digits by three with a thin space: 4474567.8 → "4 474 567.8".
[[nodiscard]] std::string groupDigits(double value, int decimals);

}  // namespace ds::sim
