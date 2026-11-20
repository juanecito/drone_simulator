// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/astro/time.hpp"
#include "ds/common/json.hpp"
#include "ds/geo/utm.hpp"
#include "ds/items/item.hpp"
#include "ds/physics/vehicle.hpp"
#include "ds/terrain/height_source.hpp"
#include "ds/terrain/terrain_files.hpp"
#include "ds/weather/weather.hpp"

/// @file
/// @brief In-memory form of a mission file (§4.9.1). Produced by the loader from a
/// `.mission.json` that passed the schema and the semantic checks; the JSON file stays the
/// single source of truth.
/// @ingroup missions

namespace ds::missions {

/// @brief Mission schema version this build reads and writes.
inline constexpr int kMissionSchemaVersion = 1;
/// @brief Largest accepted mission file (§4.9.1).
inline constexpr std::size_t kMaxMissionFileBytes = std::size_t{5} << 20U;

/// @brief A position as written in the mission (FR-MIS-03).
struct MissionPosition {
    /// @brief How the position is given.
    enum class Kind {
        Geo,    ///< `geo: [lat, lon, alt]`.
        Utm,    ///< `utm: {zone, easting, northing, alt}`.
        Local,  ///< `local: [x, y, z]` metres from the terrain origin (x east, y up, z south).
    };
    Kind kind = Kind::Local;                            ///< Form.
    double latitudeDeg = 0.0;                           ///< Geo.
    double longitudeDeg = 0.0;                          ///< Geo.
    geo::UtmCoordinate utm;                             ///< Utm.
    double x = 0.0;                                     ///< Local east.
    double z = 0.0;                                     ///< Local south.
    double altitudeM = 0.0;                             ///< Altitude above `altitudeRef`.
    items::AltitudeRef altitudeRef = items::AltitudeRef::Agl;  ///< `altRef` (default AGL).
};

/// @brief Terrain source of a mission (FR-MIS-02).
enum class TerrainSource { Procedural, Online, File };

/// @brief The mission's `terrain` object.
struct TerrainSpec {
    TerrainSource source = TerrainSource::Procedural;  ///< Source.
    // Procedural.
    std::uint64_t seed = 0;                             ///< Noise seed.
    std::string preset;                                 ///< Built-in preset ("rolling-hills").
    common::Json params = common::Json::object();       ///< Parameter overrides.
    // Online.
    std::string elevationProvider;                      ///< Provider key.
    std::string dataset;                                ///< Provider dataset ("COP30").
    std::string imageryProvider;                        ///< Empty if none.
    std::optional<double> maxResolutionM;               ///< Finest resolution to fetch.
    // File.
    std::optional<terrain::FileTerrainSpec> file;       ///< Elevation / orthophoto files.
    // All sources.
    std::optional<geo::LatLon> origin;                  ///< Geographic origin, if given.
    double originAltitudeM = 0.0;                       ///< Origin altitude.
    items::AltitudeRef originAltitudeRef = items::AltitudeRef::Agl;  ///< Its reference.
    std::optional<terrain::Extent> extent;              ///< Bounded terrain (FR-TER-04a).
};

/// @brief `environment.weather` (§4.2.3): the weather generator settings.
struct WeatherSpec {
    weather::WeatherSettings settings;           ///< Generator settings.
    common::Json json = common::Json::object();  ///< The whole object (plausibility checks).
};

/// @brief `environment`.
struct EnvironmentSpec {
    astro::UtcTime utc{};            ///< Mission start time.
    std::string timeZone;            ///< IANA time zone for display ("" ⇒ mean solar time).
    std::optional<int> lightPollution;  ///< Bortle-like 1–9.
    WeatherSpec weather;             ///< Weather.
};

/// @brief `vehicle`.
struct VehicleSpec {
    physics::VehicleType type = physics::VehicleType::Aerial;  ///< FR-MIS-03a.
    std::string profile;                       ///< Built-in profile key ("" if embedded).
    common::Json customProfile;                ///< Embedded profile (null if none).
    bool allowProfileChange = false;           ///< Pilot may pick another profile.
};

/// @brief VLOS rule (FR-MIS-06b).
struct VlosRule {
    bool enabled = true;          ///< Checked at all.
    double maxDistanceM = 500.0;  ///< Horizontal pilot–drone distance limit.
    double maxHiddenS = 5.0;      ///< Longest time hidden behind terrain or items.
};

/// @brief When the mission ends by itself (FR-MIS-06a).
enum class EndAt { Never, Sunset, Sunrise };

/// @brief `rules` (FR-MIS-06 … FR-MIS-09).
struct RulesSpec {
    bool allowKeyboard = false;                   ///< Keyboard flying allowed.
    bool showMarkers = true;                      ///< Draw item markers / guidance.
    std::optional<double> maxAltitudeAglM = 120.0;  ///< Height limit (aerial); nullopt ⇒ none.
    VlosRule vlos;                                ///< Visual line of sight.
    double graceS = 3.0;                          ///< Time a rule may be broken before failing.
    std::optional<double> timeLimitS;             ///< Mission time limit.
    double minBatteryPct = 0.0;                   ///< Fail below this charge (0 ⇒ only empty).
    std::optional<double> maxStuckS;              ///< Ground: longest time stuck (default 30 s).
    std::optional<double> maxCollisionSpeedMs;    ///< Ground: fail above this impact speed.
    bool requireLightsOn = false;                 ///< Vehicle lights must be on.
    bool daylightOnly = false;                    ///< No flight after civil dusk / before dawn.
    EndAt endAt = EndAt::Never;                   ///< Ends at sunset / sunrise.
    /// Overrides of the default flight rules, for the briefing's *Special rules* (FR-MIS-06c).
    std::vector<std::string> specialRules;
};

/// @brief One item of the mission.
struct ItemSpec {
    items::ItemType type = items::ItemType::Obstacle;  ///< Type.
    std::string id;                                    ///< Unique id ("pad").
    std::string name;                                  ///< Display name ("" ⇒ id).
    MissionPosition position;                          ///< Where.
    double headingDeg = 0.0;                           ///< Orientation.
    common::Json properties = common::Json::object();  ///< Type-specific properties.
};

/// @brief Objective types (FR-MIS-04, FR-MIS-04a).
enum class ObjectiveType {
    ReachAltitude, ReachWaypoint, Hover, Land, GateCourse, Deliver, Inspect, Survey, ReturnHome,
    Park,
};

/// @brief JSON name ("reachAltitude", …).
[[nodiscard]] std::string_view objectiveTypeName(ObjectiveType type) noexcept;
/// @brief Type for a JSON name.
[[nodiscard]] std::optional<ObjectiveType> parseObjectiveType(std::string_view name) noexcept;
/// @brief True if objectives of @p type are allowed for @p vehicle (FR-MIS-04a).
[[nodiscard]] bool objectiveAllowed(ObjectiveType type, physics::VehicleType vehicle) noexcept;

/// @brief One objective. Parameters that do not apply to the type stay empty.
struct ObjectiveSpec {
    std::string id;                        ///< Unique id.
    ObjectiveType type = ObjectiveType::ReachAltitude;  ///< Type.
    std::string title;                     ///< Display text ("" ⇒ generated).
    std::vector<std::string> after;        ///< Objectives that must be completed first.
    bool optional = false;                 ///< Not needed for success; scores a bonus.
    double bonus = 0.0;                    ///< Bonus points of an optional objective.
    std::string target;                    ///< Item (waypoint, pad, zone, target, area).
    std::vector<std::string> gates;        ///< gateCourse: gates in order.
    int laps = 1;                          ///< gateCourse laps.
    bool strictOrder = false;              ///< gateCourse: a wrong gate fails the objective.
    std::string pickup;                    ///< deliver: pickup zone.
    std::string drop;                      ///< deliver: drop zone.
    double cargoKg = 0.0;                  ///< deliver: cargo mass.
    std::optional<MissionPosition> position;  ///< reachWaypoint / hover without an item.
    std::optional<double> altitudeAglM;    ///< reachAltitude / hover target height.
    std::optional<double> minAltitudeAglM;  ///< Altitude band (waypoint, survey).
    std::optional<double> maxAltitudeAglM;  ///< Altitude band.
    std::optional<double> radiusM;         ///< reachWaypoint / returnHome radius.
    std::optional<double> toleranceM;      ///< hover position / reachAltitude height tolerance.
    std::optional<double> altitudeToleranceM;  ///< hover height tolerance.
    std::optional<double> holdS;           ///< Time to hold the condition.
    std::optional<double> durationS;       ///< hover / inspect duration.
    std::optional<double> maxSpeedMs;      ///< Speed limit while in the target.
    std::optional<double> maxErrorM;       ///< land / park position error.
    std::optional<double> maxTouchdownSpeedMs;  ///< land: hard-landing limit.
    std::optional<double> maxHeadingErrorDeg;   ///< land / park heading tolerance.
    std::optional<double> maxDropSpeedMs;  ///< deliver: release speed limit.
    std::optional<double> maxDistanceM;    ///< inspect: camera distance.
    std::optional<double> maxAngleDeg;     ///< inspect: off-axis angle.
    std::optional<double> coveragePct;     ///< survey coverage needed.
    std::optional<double> swathWidthM;     ///< survey footprint width.
};

/// @brief Order of the objectives (FR-MIS-05).
enum class ObjectiveOrder {
    Parallel,    ///< Only `after` constrains the order.
    Sequential,  ///< Each objective also waits for the required objectives listed before it.
};

/// @brief A scheduled failure injection (FR-MIS-07).
struct FailureSpec {
    double atS = 0.0;                                  ///< Mission time.
    std::string type;                                  ///< gpsDrift, motorFailure, …
    common::Json params = common::Json::object();      ///< Type-specific values.
};

/// @brief One star threshold (FR-MIS-10). All given conditions must hold.
struct StarThreshold {
    std::optional<double> maxTimeS;       ///< Mission time.
    std::optional<double> maxErrorM;      ///< Worst objective position error.
    std::optional<double> minBatteryPct;  ///< Charge at the end.
    std::optional<double> minScore;       ///< Total score.
};

/// @brief `scoring`.
struct ScoringSpec {
    /// Thresholds for the 2nd and 3rd star (success alone gives one star).
    std::vector<StarThreshold> stars;
};

/// @brief `unlock`: campaign rule (FR-MIS-12).
struct UnlockSpec {
    std::vector<std::string> missions;  ///< `requires`: missions that need `minStars`.
    int minStars = 1;                   ///< Stars needed on each.
};

/// @brief A complete mission.
struct MissionDefinition {
    int schemaVersion = kMissionSchemaVersion;  ///< `schemaVersion`.
    std::string id;                    ///< Stable key.
    std::string title;                 ///< Title.
    std::string briefing;              ///< Briefing text.
    int difficulty = 1;                ///< 1–5.
    std::string chapter;               ///< Campaign chapter ("" ⇒ none).
    int order = 0;                     ///< Position in the chapter.
    std::optional<UnlockSpec> unlock;  ///< Explicit unlock rule.
    TerrainSpec terrain;               ///< Terrain.
    EnvironmentSpec environment;       ///< Time and weather.
    VehicleSpec vehicle;               ///< Vehicle.
    MissionPosition spawn;             ///< Take-off / start point.
    double spawnHeadingDeg = 0.0;      ///< Initial heading.
    RulesSpec rules;                   ///< Rules.
    std::vector<ItemSpec> items;       ///< Items.
    ObjectiveOrder objectiveOrder = ObjectiveOrder::Parallel;  ///< Order.
    std::vector<ObjectiveSpec> objectives;  ///< At least one.
    std::vector<FailureSpec> failures;      ///< Failure injections.
    ScoringSpec scoring;                    ///< Star thresholds.

    /// @brief The item with id @p id, or nullptr.
    [[nodiscard]] const ItemSpec* item(std::string_view itemId) const noexcept;
    /// @brief The objective with id @p id, or nullptr.
    [[nodiscard]] const ObjectiveSpec* objective(std::string_view objectiveId) const noexcept;
};

/// @brief Display text of objective @p o of @p mission: its `title`, or a generated one such as
/// "Land on Home pad" (items by their name when they have one).
[[nodiscard]] std::string objectiveTitle(const MissionDefinition& mission, const ObjectiveSpec& o);

}  // namespace ds::missions
