// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/missions/mission_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <utility>

#include "ds/astro/time.hpp"
#include "ds/common/sha256.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/terrain/procedural_source.hpp"

namespace ds::missions {
namespace {

using common::Json;
using items::ItemType;
using physics::VehicleType;
namespace fs = std::filesystem;

constexpr double kDefaultMaxAglM = 120.0;
constexpr double kDefaultVlosDistanceM = 500.0;
constexpr double kDefaultVlosHiddenS = 5.0;
constexpr double kDefaultGraceS = 3.0;
using weather::kFogMinHumidityPct;
using weather::kSnowMaxTemperatureC;

// ----- small JSON accessors (the schema has already checked the types) ----------------

std::optional<double> optNumber(const Json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number()) {
        return j[key].get<double>();
    }
    return std::nullopt;
}

double number(const Json& j, const char* key, double fallback) {
    return optNumber(j, key).value_or(fallback);
}

std::string text(const Json& j, const char* key, std::string fallback = {}) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return fallback;
}

bool flag(const Json& j, const char* key, bool fallback) {
    if (j.is_object() && j.contains(key) && j[key].is_boolean()) {
        return j[key].get<bool>();
    }
    return fallback;
}

std::vector<std::string> strings(const Json& j, const char* key) {
    std::vector<std::string> out;
    if (j.is_object() && j.contains(key) && j[key].is_array()) {
        for (const Json& v : j[key]) {
            if (v.is_string()) {
                out.push_back(v.get<std::string>());
            }
        }
    }
    return out;
}

std::uint64_t unsignedValue(const Json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number_integer()) {
        const auto v = j[key].get<std::int64_t>();
        return v < 0 ? 0 : static_cast<std::uint64_t>(v);
    }
    return 0;
}

std::string_view vehicleWord(VehicleType t) {
    return t == VehicleType::Aerial ? "aerial" : "ground";
}

// ----- the checker ----------------------------------------------------------------------

class Checker {
public:
    Checker(const ValidationContext& context, fs::path missionDir, MissionLoadResult& result)
        : ctx_(context), missionDir_(std::move(missionDir)), result_(result) {}

    void run(const Json& root) {
        if (!root.is_object()) {
            error("", "a mission must be a JSON object");
            return;
        }
        // A newer schema gets a clear message instead of a list of unknown fields.
        if (root.contains("schemaVersion") && root["schemaVersion"].is_number_integer()) {
            const auto version = root["schemaVersion"].get<std::int64_t>();
            if (version > kMissionSchemaVersion) {
                error("schemaVersion",
                      std::format("version {} is newer than this application supports ({}); "
                                  "update the application",
                                  version, kMissionSchemaVersion));
                return;
            }
        }
        if (ctx_.schema == nullptr) {
            error("", "the mission schema is not available");
            return;
        }
        for (const common::SchemaIssue& issue : ctx_.schema->validate(root, 50)) {
            error(common::pointerToDotted(issue.pointer), issue.message);
        }
        if (!result_.errors.empty()) {
            return;  // Semantic checks need a structurally valid document.
        }
        MissionDefinition m;
        parseHeader(root, m);
        parseTerrain(root["terrain"], m);
        parseEnvironment(root["environment"], m);
        parseVehicle(root["vehicle"], m);
        if (auto p = position(root["spawn"], "spawn")) {
            m.spawn = *p;
        }
        m.spawnHeadingDeg = number(root["spawn"], "headingDeg", 0.0);
        parseRules(root.value("rules", Json::object()), m);
        parseItems(root.value("items", Json::array()), m);
        parseObjectives(root, m);
        parseFailures(root.value("failures", Json::array()), m);
        parseScoring(root.value("scoring", Json::object()), m);
        checkPlacement(m);
        checkThresholds(m);
        checkWeather(m);
        if (result_.errors.empty()) {
            result_.definition = std::move(m);
        }
    }

private:
    void error(std::string path, std::string message) {
        result_.errors.push_back({path.empty() ? "(root)" : std::move(path), std::move(message)});
    }
    void warning(std::string path, std::string message) {
        result_.warnings.push_back({path.empty() ? "(root)" : std::move(path), std::move(message)});
    }

    // ----- parsing -----------------------------------------------------------------

    std::optional<MissionPosition> position(const Json& j, const std::string& path) {
        const int forms = static_cast<int>(j.contains("geo")) + static_cast<int>(j.contains("utm")) +
                          static_cast<int>(j.contains("local"));
        if (forms != 1) {
            error(path, forms == 0 ? "needs a position: one of geo, utm or local"
                                   : "give only one of geo, utm or local");
            return std::nullopt;
        }
        MissionPosition p;
        p.altitudeRef = text(j, "altRef", "AGL") == "AMSL" ? items::AltitudeRef::Amsl
                                                           : items::AltitudeRef::Agl;
        if (j.contains("geo")) {
            p.kind = MissionPosition::Kind::Geo;
            p.latitudeDeg = j["geo"][0].get<double>();
            p.longitudeDeg = j["geo"][1].get<double>();
            p.altitudeM = j["geo"][2].get<double>();
        } else if (j.contains("utm")) {
            const Json& u = j["utm"];
            const std::string zone = u["zone"].get<std::string>();
            p.kind = MissionPosition::Kind::Utm;
            p.utm.zone = std::stoi(zone.substr(0, zone.size() - 1));
            p.utm.band = zone.back();
            p.utm.north = p.utm.band >= 'N';
            p.utm.easting = u["easting"].get<double>();
            p.utm.northing = u["northing"].get<double>();
            p.altitudeM = u["alt"].get<double>();
            if (!geo::fromUtm(p.utm)) {
                error(path + ".utm", "is not a valid UTM coordinate");
            }
        } else {
            p.kind = MissionPosition::Kind::Local;
            p.x = j["local"][0].get<double>();
            p.altitudeM = j["local"][1].get<double>();
            p.z = j["local"][2].get<double>();
        }
        return p;
    }

    void parseHeader(const Json& root, MissionDefinition& m) {
        m.schemaVersion = root["schemaVersion"].get<int>();
        m.id = root["id"].get<std::string>();
        m.title = root["title"].get<std::string>();
        m.briefing = text(root, "briefing");
        m.difficulty = static_cast<int>(number(root, "difficulty", 1));
        m.chapter = text(root, "chapter");
        m.order = static_cast<int>(number(root, "order", 0));
        if (root.contains("unlock")) {
            UnlockSpec u;
            u.missions = strings(root["unlock"], "requires");
            u.minStars = static_cast<int>(number(root["unlock"], "minStars", 1));
            if (std::ranges::find(u.missions, m.id) != u.missions.end()) {
                error("unlock.requires", "a mission cannot require itself");
            }
            m.unlock = std::move(u);
        }
        m.objectiveOrder = text(root, "objectiveOrder", "parallel") == "sequential"
                               ? ObjectiveOrder::Sequential
                               : ObjectiveOrder::Parallel;
    }

    void parseTerrain(const Json& j, MissionDefinition& m) {
        TerrainSpec& t = m.terrain;
        const std::string source = j["source"].get<std::string>();
        if (j.contains("origin")) {
            const Json& o = j["origin"];
            t.origin = geo::LatLon{o["geo"][0].get<double>(), o["geo"][1].get<double>()};
            t.originAltitudeM = o["geo"][2].get<double>();
            t.originAltitudeRef = text(o, "altRef", "AGL") == "AMSL" ? items::AltitudeRef::Amsl
                                                                      : items::AltitudeRef::Agl;
        }
        if (j.contains("extent")) {
            t.extent = terrain::Extent{j["extent"]["widthM"].get<double>(),
                                       j["extent"]["lengthM"].get<double>()};
        }
        if (source == "procedural") {
            t.source = TerrainSource::Procedural;
            t.seed = unsignedValue(j, "seed");
            t.preset = text(j, "preset");
            t.params = j.value("params", Json::object());
            if (!t.preset.empty() && !ctx_.terrainPresets.empty() &&
                !ctx_.terrainPresets.contains(t.preset)) {
                error("terrain.preset", std::format("unknown terrain preset \"{}\"", t.preset));
            }
            checkProceduralParams(t);
        } else if (source == "online") {
            t.source = TerrainSource::Online;
            t.elevationProvider = text(j, "elevationProvider");
            t.dataset = text(j, "dataset");
            t.imageryProvider = text(j, "imageryProvider");
            t.imageryDataset = text(j, "imageryDataset");
            t.maxResolutionM = optNumber(j, "maxResolutionM");
            for (const auto& [key, value] : {std::pair{"elevationProvider", t.elevationProvider},
                                             std::pair{"imageryProvider", t.imageryProvider}}) {
                if (!value.empty() && !ctx_.providers.empty() && !ctx_.providers.contains(value)) {
                    error(std::format("terrain.{}", key),
                          std::format("unknown data provider \"{}\"", value));
                }
            }
        } else {
            t.source = TerrainSource::File;
            auto spec = terrain::parseFileTerrain(j, "/terrain");
            if (!spec) {
                error("terrain", spec.error().message);
                return;
            }
            checkTerrainFiles(*spec);
            if (!spec->imageryProvider.empty() && !ctx_.providers.empty() &&
                !ctx_.providers.contains(spec->imageryProvider)) {
                error("terrain.imageryProvider",
                      std::format("unknown data provider \"{}\"", spec->imageryProvider));
            }
            t.file = std::move(*spec);
        }
    }

    void checkProceduralParams(const TerrainSpec& t) {
        terrain::ProceduralParams p;
        p.seed = t.seed;
        const Json& j = t.params;
        p.octaves = static_cast<int>(number(j, "octaves", p.octaves));
        for (const auto& [key, out] :
             {std::pair{"featureSizeM", &p.featureSizeM}, std::pair{"lacunarity", &p.lacunarity},
              std::pair{"persistence", &p.persistence}, std::pair{"amplitudeM", &p.amplitudeM},
              std::pair{"baseHeightM", &p.baseHeightM}, std::pair{"mountains", &p.mountains},
              std::pair{"warp", &p.warp}, std::pair{"erosion", &p.erosion}}) {
            *out = number(j, key, *out);
        }
        p.waterLevelM = optNumber(j, "waterLevelM");
        p.snowLineM = optNumber(j, "snowLineM");
        if (auto ok = terrain::validate(p); !ok) {
            error("terrain.params", ok.error().message);
        }
    }

    void checkTerrainFiles(terrain::FileTerrainSpec& spec) {
        if (missionDir_.empty()) {
            return;
        }
        const terrain::TerrainFileRoots roots{missionDir_, ctx_.libraryDir};
        std::vector<std::filesystem::path> found;
        std::vector<std::string> notInLibrary;
        for (const terrain::FileRef& f : spec.elevationFiles) {
            if (f.path.starts_with("library:") && ctx_.libraryDir.empty()) {
                continue;
            }
            const auto resolved = terrain::resolveTerrainPath(f.path, roots);
            if (resolved) {
                found.push_back(*resolved);
                continue;
            }
            if (resolved.error().code != terrain::TerrainErrorCode::FileNotFound) {
                error(common::pointerToDotted(f.jsonPath), resolved.error().message);
            } else if (f.path.starts_with("library:")) {
                notInLibrary.push_back(f.path);
            } else {
                result_.unavailable.push_back(
                    std::format("elevation file \"{}\" not found", f.path));
            }
        }
        // Library data is usually fetched as a whole: one line, with where it goes.
        if (!notInLibrary.empty()) {
            result_.unavailable.push_back(std::format(
                "{} not in the geodata library {} (the built-in missions get their data with "
                "fetch_mission_data.py)",
                notInLibrary.size() == 1
                    ? std::format("elevation file \"{}\" is", notInLibrary.front())
                    : std::format("{} elevation files, from \"{}\", are", notInLibrary.size(),
                                  notInLibrary.front()),
                ctx_.libraryDir.string()));
        }
        if (found.size() == spec.elevationFiles.size()) {
            spec.resolvedElevationFiles = std::move(found);
        }
        // Orthophotos: a missing one only disables the imagery (FR-TER-I08); a path that
        // breaks the folder rules is an error like any other.
        std::vector<std::filesystem::path> images;
        for (const terrain::FileRef& f : spec.orthophotoFiles) {
            if (f.path.starts_with("library:") && ctx_.libraryDir.empty()) {
                continue;
            }
            const auto resolved = terrain::resolveTerrainPath(f.path, roots);
            if (resolved) {
                images.push_back(*resolved);
            } else if (resolved.error().code != terrain::TerrainErrorCode::FileNotFound) {
                error(common::pointerToDotted(f.jsonPath), resolved.error().message);
            }
        }
        if (images.size() == spec.orthophotoFiles.size()) {
            spec.resolvedOrthophotoFiles = std::move(images);
        }
    }

    void parseEnvironment(const Json& j, MissionDefinition& m) {
        EnvironmentSpec& e = m.environment;
        const std::string utc = j["utc"].get<std::string>();
        if (auto t = astro::parseUtc(utc); !t) {
            error("environment.utc", t.error().message);
        } else if (!astro::isSupported(*t)) {
            error("environment.utc", "is outside the supported years");
        } else {
            e.utc = *t;
        }
        e.timeZone = text(j, "timeZone");
        if (!e.timeZone.empty() && !astro::toLocalTime(e.utc, e.timeZone)) {
            error("environment.timeZone", std::format("unknown time zone \"{}\"", e.timeZone));
        }
        if (j.contains("lightPollution")) {
            e.lightPollution = j["lightPollution"].get<int>();
        }
        if (j.contains("weather")) {
            const Json& w = j["weather"];
            WeatherSpec& ws = e.weather;
            ws.json = w;
            if (auto settings = weather::WeatherSettings::fromJson(w)) {
                ws.settings = std::move(*settings);
            } else {
                error("environment.weather", settings.error().message);
            }
        }
    }

    void parseVehicle(const Json& j, MissionDefinition& m) {
        VehicleSpec& v = m.vehicle;
        v.type = j["type"].get<std::string>() == "ground" ? VehicleType::Ground : VehicleType::Aerial;
        v.profile = text(j, "profile");
        v.allowProfileChange = flag(j, "allowProfileChange", false);
        if (j.contains("customProfile")) {
            v.customProfile = j["customProfile"];
        }
        const bool custom = !v.customProfile.is_null();
        if (custom == !v.profile.empty()) {
            error("vehicle", custom ? "give either profile or customProfile, not both"
                                    : "needs a profile or a customProfile");
            return;
        }
        if (custom) {
            if (text(v.customProfile, "vehicleType") != vehicleWord(v.type)) {
                error("vehicle.customProfile.vehicleType",
                      std::format("must be \"{}\" like vehicle.type", vehicleWord(v.type)));
            }
            return;
        }
        const auto& known = v.type == VehicleType::Aerial ? ctx_.aerialProfiles : ctx_.groundProfiles;
        const auto& other = v.type == VehicleType::Aerial ? ctx_.groundProfiles : ctx_.aerialProfiles;
        if (other.contains(v.profile)) {
            error("vehicle.profile", std::format("\"{}\" is a {} profile, but the mission is for a "
                                                 "{} vehicle",
                                                 v.profile,
                                                 vehicleWord(v.type == VehicleType::Aerial
                                                                 ? VehicleType::Ground
                                                                 : VehicleType::Aerial),
                                                 vehicleWord(v.type)));
        } else if (!known.empty() && !known.contains(v.profile)) {
            error("vehicle.profile", std::format("unknown vehicle profile \"{}\"", v.profile));
        }
    }

    void parseRules(const Json& j, MissionDefinition& m) {
        RulesSpec& r = m.rules;
        const bool aerial = m.vehicle.type == VehicleType::Aerial;
        r.allowKeyboard = flag(j, "allowKeyboard", false);
        r.showMarkers = flag(j, "showMarkers", true);
        if (j.contains("maxAltitudeAglM")) {
            if (!aerial) {
                error("rules.maxAltitudeAglM", "altitude limits do not apply to ground vehicles");
            }
            r.maxAltitudeAglM = optNumber(j, "maxAltitudeAglM");
        }
        if (!aerial) {
            r.maxAltitudeAglM.reset();
        }
        if (j.contains("vlos")) {
            const Json& v = j["vlos"];
            r.vlos.enabled = flag(v, "enabled", true);
            r.vlos.maxDistanceM = number(v, "maxDistanceM", kDefaultVlosDistanceM);
            r.vlos.maxHiddenS = number(v, "maxHiddenS", kDefaultVlosHiddenS);
        }
        r.graceS = number(j, "graceS", kDefaultGraceS);
        r.timeLimitS = optNumber(j, "timeLimitS");
        r.minBatteryPct = number(j, "minBatteryPct", 0.0);
        r.maxStuckS = optNumber(j, "maxStuckS");
        r.maxCollisionSpeedMs = optNumber(j, "maxCollisionSpeedMs");
        if (aerial) {
            for (const char* key : {"maxStuckS", "maxCollisionSpeedMs"}) {
                if (j.contains(key)) {
                    error(std::format("rules.{}", key), "only applies to ground vehicles");
                }
            }
        }
        r.requireLightsOn = flag(j, "requireLightsOn", false);
        r.daylightOnly = flag(j, "daylightOnly", false);
        const std::string endAt = text(j, "endAt");
        r.endAt = endAt == "sunset" ? EndAt::Sunset : endAt == "sunrise" ? EndAt::Sunrise : EndAt::Never;

        // Overrides of the default flight rules (FR-MIS-06c).
        auto& s = r.specialRules;
        if (aerial && j.contains("maxAltitudeAglM")) {
            if (!r.maxAltitudeAglM) {
                s.emplace_back("No height limit (the 120 m AGL rule is lifted)");
            } else if (*r.maxAltitudeAglM != kDefaultMaxAglM) {
                s.push_back(std::format("Height limit {:g} m AGL (instead of 120 m)",
                                        *r.maxAltitudeAglM));
            }
        }
        if (!r.vlos.enabled) {
            s.emplace_back("Beyond visual line of sight: VLOS is not required");
        } else {
            if (r.vlos.maxDistanceM != kDefaultVlosDistanceM) {
                s.push_back(std::format("VLOS distance {:g} m (instead of 500 m)", r.vlos.maxDistanceM));
            }
            if (r.vlos.maxHiddenS != kDefaultVlosHiddenS) {
                s.push_back(std::format("Drone may be hidden for {:g} s (instead of 5 s)",
                                        r.vlos.maxHiddenS));
            }
        }
        if (r.graceS != kDefaultGraceS) {
            s.push_back(std::format("Rule violations fail the mission after {:g} s (instead of 3 s)",
                                    r.graceS));
        }
        if (r.allowKeyboard) {
            s.emplace_back("Keyboard flying allowed");
        }
        if (r.requireLightsOn) {
            s.emplace_back("Vehicle lights must stay on");
        }
        if (r.daylightOnly) {
            s.emplace_back("Daylight and civil twilight only");
        }
        if (r.endAt != EndAt::Never) {
            s.push_back(std::format("The mission ends at {}",
                                    r.endAt == EndAt::Sunset ? "sunset" : "sunrise"));
        }
    }

    // Properties allowed per item type (besides type, id, name, position and heading).
    static bool propertyAllowed(ItemType t, std::string_view key) {
        using enum ItemType;
        static const std::map<std::string_view, std::vector<ItemType>> kAllowed{
            {"shape", {Obstacle}},
            {"radiusM",
             {Obstacle, Waypoint, LandingPad, PickupZone, DropZone, InspectionTarget, SurveyArea,
              NoFlyZone, Geofence}},
            {"widthM", {Gate}},
            {"heightM", {Obstacle, Gate, PickupZone, DropZone, ParkingZone, Ramp, Bridge}},
            {"sizeM", {Obstacle, PickupZone, DropZone, ParkingZone, Ramp, Bridge, Payload}},
            {"polygon", {SurveyArea, NoFlyZone, Geofence}},
            {"bottomM", {NoFlyZone, Geofence, SurveyArea}},
            {"topM", {NoFlyZone, Geofence, SurveyArea}},
            {"massKg", {Payload, Obstacle}},
            {"restitution", {Obstacle}},
            {"friction", {Obstacle}},
            {"order", {Gate, Waypoint}},
            {"rampLengthM", {Bridge}},
            {"kerbs", {Bridge}},
            {"model", {Obstacle}},
        };
        const auto it = kAllowed.find(key);
        return it == kAllowed.end() || std::ranges::find(it->second, t) != it->second.end();
    }

    void parseItems(const Json& list, MissionDefinition& m) {
        std::map<std::string, std::size_t> seen;
        int pilots = 0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Json& j = list[i];
            const std::string path = std::format("items[{}]", i);
            ItemSpec item;
            item.type = *items::parseItemType(j["type"].get<std::string>());
            item.id = j["id"].get<std::string>();
            item.name = text(j, "name", item.id);
            item.headingDeg = number(j, "headingDeg", 0.0);
            if (const auto [it, inserted] = seen.emplace(item.id, i); !inserted) {
                error(path + ".id", std::format("duplicate item id \"{}\" (also items[{}])",
                                                item.id, it->second));
            }
            for (const auto& [key, value] : j.items()) {
                if (!key.starts_with("x-") && !propertyAllowed(item.type, key)) {
                    error(std::format("{}.{}", path, key),
                          std::format("does not apply to a {}", items::itemTypeName(item.type)));
                }
                if (key != "type" && key != "id" && key != "name" && key != "geo" &&
                    key != "utm" && key != "local" && key != "altRef" && key != "headingDeg") {
                    item.properties[key] = value;
                }
            }
            if (const auto model = item.properties.find("model");
                model != item.properties.end() && model->is_string() &&
                std::ranges::any_of(
                    std::filesystem::path(model->get<std::string>()),
                    [](const std::filesystem::path& part) { return part == ".."; })) {
                error(path + ".model", "must not leave the model folders (\"..\")");
            }
            if (item.type == ItemType::PilotPosition && ++pilots > 1) {
                error(path, "only one pilot position is allowed");
            }
            if (auto p = position(j, path)) {
                item.position = *p;
            }
            m.items.push_back(std::move(item));
        }
    }

    void requireItem(const MissionDefinition& m, const std::string& path, const std::string& id,
                     std::initializer_list<ItemType> types) {
        const ItemSpec* item = m.item(id);
        if (item == nullptr) {
            error(path, std::format("unknown item \"{}\"", id));
            return;
        }
        if (std::ranges::find(types, item->type) == types.end()) {
            std::string names;
            for (const ItemType t : types) {
                names += names.empty() ? "" : " or ";
                names += items::itemTypeName(t);
            }
            error(path, std::format("item \"{}\" is a {}, expected a {}", id,
                                    items::itemTypeName(item->type), names));
        }
    }

    void parseObjectives(const Json& root, MissionDefinition& m) {
        const Json& list = root["objectives"];
        std::map<std::string, std::size_t> seen;
        const bool aerial = m.vehicle.type == VehicleType::Aerial;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Json& j = list[i];
            const std::string path = std::format("objectives[{}]", i);
            ObjectiveSpec o;
            o.id = j["id"].get<std::string>();
            o.type = *parseObjectiveType(j["type"].get<std::string>());
            o.title = text(j, "title");
            o.after = strings(j, "after");
            o.optional = flag(j, "optional", false);
            o.bonus = number(j, "bonus", o.optional ? 100.0 : 0.0);
            o.target = text(j, "target");
            o.gates = strings(j, "gates");
            o.laps = static_cast<int>(number(j, "laps", 1));
            o.strictOrder = flag(j, "strictOrder", false);
            o.pickup = text(j, "pickup");
            o.drop = text(j, "drop");
            o.cargoKg = number(j, "cargoKg", 0.0);
            o.altitudeAglM = optNumber(j, "altitudeAglM");
            o.minAltitudeAglM = optNumber(j, "minAltitudeAglM");
            o.maxAltitudeAglM = optNumber(j, "maxAltitudeAglM");
            o.radiusM = optNumber(j, "radiusM");
            o.toleranceM = optNumber(j, "toleranceM");
            o.altitudeToleranceM = optNumber(j, "altitudeToleranceM");
            o.holdS = optNumber(j, "holdS");
            o.durationS = optNumber(j, "durationS");
            o.maxSpeedMs = optNumber(j, "maxSpeedMs");
            o.maxErrorM = optNumber(j, "maxErrorM");
            o.maxTouchdownSpeedMs = optNumber(j, "maxTouchdownSpeedMs");
            o.maxHeadingErrorDeg = optNumber(j, "maxHeadingErrorDeg");
            o.maxDropSpeedMs = optNumber(j, "maxDropSpeedMs");
            o.maxDistanceM = optNumber(j, "maxDistanceM");
            o.maxAngleDeg = optNumber(j, "maxAngleDeg");
            o.coveragePct = optNumber(j, "coveragePct");
            o.swathWidthM = optNumber(j, "swathWidthM");
            if (j.contains("position")) {
                o.position = position(j["position"], path + ".position");
            }
            if (const auto [it, inserted] = seen.emplace(o.id, i); !inserted) {
                error(path + ".id", std::format("duplicate objective id \"{}\" (also objectives[{}])",
                                                o.id, it->second));
            }
            if (!objectiveAllowed(o.type, m.vehicle.type)) {
                error(path + ".type",
                      std::format("\"{}\" is not available for {} vehicles",
                                  objectiveTypeName(o.type), vehicleWord(m.vehicle.type)));
            }
            checkObjectiveReferences(m, o, path, aerial);
            m.objectives.push_back(std::move(o));
        }
        // Dependencies: known, not self, no cycles.
        for (std::size_t i = 0; i < m.objectives.size(); ++i) {
            const ObjectiveSpec& o = m.objectives[i];
            for (std::size_t k = 0; k < o.after.size(); ++k) {
                const std::string path = std::format("objectives[{}].after[{}]", i, k);
                const ObjectiveSpec* dep = m.objective(o.after[k]);
                if (o.after[k] == o.id) {
                    error(path, "an objective cannot wait for itself");
                } else if (dep == nullptr) {
                    error(path, std::format("unknown objective \"{}\"", o.after[k]));
                } else if (dep->optional && !o.optional) {
                    warning(path, std::format("required objective waits for optional objective "
                                              "\"{}\", which makes it required",
                                              dep->id));
                }
            }
        }
        if (result_.errors.empty()) {
            checkCycles(m);
        }
        if (std::ranges::none_of(m.objectives, [](const ObjectiveSpec& o) { return !o.optional; })) {
            error("objectives", "at least one objective must be required (not optional)");
        }
    }

    void checkObjectiveReferences(const MissionDefinition& m, const ObjectiveSpec& o,
                                  const std::string& path, bool aerial) {
        using enum ItemType;
        const auto targetOrPosition = [&](bool required) {
            const bool both = !o.target.empty() && o.position.has_value();
            if (both) {
                error(path, "give either target or position, not both");
            } else if (required && o.target.empty() && !o.position) {
                error(path, "needs a target item or a position");
            }
        };
        switch (o.type) {
            case ObjectiveType::ReachWaypoint:
                targetOrPosition(true);
                if (!o.target.empty()) {
                    requireItem(m, path + ".target", o.target, {Waypoint});
                }
                if (!aerial && (o.minAltitudeAglM || o.maxAltitudeAglM)) {
                    error(path, "altitude bands do not apply to ground vehicles");
                }
                break;
            case ObjectiveType::Hover:
                targetOrPosition(true);
                if (!o.target.empty() && m.item(o.target) == nullptr) {
                    error(path + ".target", std::format("unknown item \"{}\"", o.target));
                }
                break;
            case ObjectiveType::Land:
                requireItem(m, path + ".target", o.target, {LandingPad});
                break;
            case ObjectiveType::GateCourse:
                for (std::size_t g = 0; g < o.gates.size(); ++g) {
                    requireItem(m, std::format("{}.gates[{}]", path, g), o.gates[g], {Gate});
                }
                break;
            case ObjectiveType::Deliver:
                requireItem(m, path + ".pickup", o.pickup, {PickupZone});
                requireItem(m, path + ".drop", o.drop, {DropZone, LandingPad, ParkingZone});
                break;
            case ObjectiveType::Inspect:
                requireItem(m, path + ".target", o.target, {InspectionTarget});
                break;
            case ObjectiveType::Survey:
                requireItem(m, path + ".target", o.target, {SurveyArea});
                if (!aerial && (o.minAltitudeAglM || o.maxAltitudeAglM)) {
                    error(path, "altitude bands do not apply to ground vehicles");
                }
                break;
            case ObjectiveType::Park:
                requireItem(m, path + ".target", o.target, {ParkingZone});
                break;
            case ObjectiveType::ReachAltitude:
            case ObjectiveType::ReturnHome: break;
        }
        if (o.minAltitudeAglM && o.maxAltitudeAglM && *o.minAltitudeAglM >= *o.maxAltitudeAglM) {
            error(path + ".minAltitudeAglM", "must be below maxAltitudeAglM");
        }
    }

    void checkCycles(const MissionDefinition& m) {
        enum class Mark { None, Visiting, Done };
        std::map<std::string, Mark> marks;
        std::function<bool(const ObjectiveSpec&)> visit = [&](const ObjectiveSpec& o) {
            Mark& mark = marks[o.id];
            if (mark == Mark::Visiting) {
                return false;
            }
            if (mark == Mark::Done) {
                return true;
            }
            mark = Mark::Visiting;
            for (const std::string& dep : o.after) {
                if (const ObjectiveSpec* d = m.objective(dep); d != nullptr && !visit(*d)) {
                    return false;
                }
            }
            marks[o.id] = Mark::Done;
            return true;
        };
        for (std::size_t i = 0; i < m.objectives.size(); ++i) {
            if (!visit(m.objectives[i])) {
                error(std::format("objectives[{}].after", i),
                      std::format("objective \"{}\" is part of a dependency cycle",
                                  m.objectives[i].id));
                return;
            }
        }
    }

    void parseFailures(const Json& list, MissionDefinition& m) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Json& j = list[i];
            FailureSpec f;
            f.atS = j["atS"].get<double>();
            f.type = j["type"].get<std::string>();
            for (const auto& [key, value] : j.items()) {
                if (key != "atS" && key != "type") {
                    f.params[key] = value;
                }
            }
            if (f.type == "motorFailure" && m.vehicle.type == VehicleType::Aerial &&
                number(j, "motor", 1) > physics::AerialProfile::kMotorCount) {
                error(std::format("failures[{}].motor", i),
                      std::format("an aerial drone has {} motors",
                                  physics::AerialProfile::kMotorCount));
            }
            if (m.rules.timeLimitS && f.atS > *m.rules.timeLimitS) {
                warning(std::format("failures[{}].atS", i),
                        "is after the time limit, so it never happens");
            }
            m.failures.push_back(std::move(f));
        }
    }

    void parseScoring(const Json& j, MissionDefinition& m) {
        if (!j.contains("stars")) {
            return;
        }
        const Json& list = j["stars"];
        for (std::size_t i = 0; i < list.size(); ++i) {
            StarThreshold s;
            s.maxTimeS = optNumber(list[i], "maxTimeS");
            s.maxErrorM = optNumber(list[i], "maxErrorM");
            s.minBatteryPct = optNumber(list[i], "minBatteryPct");
            s.minScore = optNumber(list[i], "minScore");
            m.scoring.stars.push_back(s);
        }
    }

    // ----- semantic checks ---------------------------------------------------------

    void checkPlacement(const MissionDefinition& m) {
        const TerrainSpec& t = m.terrain;
        std::optional<geo::LatLon> origin = t.origin;
        if (!origin && t.file && t.file->originLatDeg && t.file->originLonDeg) {
            origin = geo::LatLon{*t.file->originLatDeg, *t.file->originLonDeg};
        }
        const bool originFromFiles = !origin && t.source == TerrainSource::File;
        const auto where = [&](const MissionPosition& p,
                               const std::string& path) -> std::optional<items::Vec2> {
            if (p.kind != MissionPosition::Kind::Local && !origin) {
                if (!originFromFiles) {
                    error(path, "geographic positions need terrain.origin");
                }
                return std::nullopt;  // Placed once the files' origin is known.
            }
            auto xz = horizontalPosition(p, origin);
            if (xz && t.extent && !t.extent->contains(xz->x, xz->y)) {
                error(path, std::format("is outside the terrain extent ({:g} × {:g} m around the "
                                        "origin): {:.0f} m east, {:.0f} m north",
                                        t.extent->widthM, t.extent->lengthM, xz->x + 0.0, 0.0 - xz->y));
            }
            return xz;
        };
        const auto spawn = where(m.spawn, "spawn");
        for (std::size_t i = 0; i < m.items.size(); ++i) {
            const ItemSpec& item = m.items[i];
            const std::string path = std::format("items[{}]", i);
            const auto xz = where(item.position, path);
            if (!xz || !spawn) {
                continue;
            }
            if (item.type == ItemType::NoFlyZone || item.type == ItemType::Geofence) {
                items::Item probe;
                probe.type = item.type;
                probe.shape = items::defaultShape(item.type, item.properties);
                probe.transform.position = items::Vec3(xz->x, 0.0, xz->y);
                probe.transform.rotation = items::headingRotation(item.headingDeg);
                const double y = std::clamp(0.0, probe.shape.bottomM, probe.shape.topM);
                const bool inside = probe.contains(items::Vec3(spawn->x, y, spawn->y));
                if (item.type == ItemType::NoFlyZone && inside) {
                    error(path, std::format("no-fly zone \"{}\" contains the spawn point", item.id));
                } else if (item.type == ItemType::Geofence && !inside) {
                    error(path, std::format("geofence \"{}\" does not contain the spawn point",
                                            item.id));
                }
            }
        }
    }

    void checkThresholds(const MissionDefinition& m) {
        const auto& limit = m.rules.maxAltitudeAglM;
        const auto above = [&](double h) { return limit && h > *limit; };
        for (std::size_t i = 0; i < m.objectives.size(); ++i) {
            const ObjectiveSpec& o = m.objectives[i];
            const std::string path = std::format("objectives[{}]", i);
            if (o.altitudeAglM && above(*o.altitudeAglM)) {
                error(path + ".altitudeAglM",
                      std::format("{:g} m cannot be reached: the height limit is {:g} m AGL",
                                  *o.altitudeAglM, *limit));
            }
            if (o.minAltitudeAglM && above(*o.minAltitudeAglM)) {
                error(path + ".minAltitudeAglM",
                      std::format("{:g} m cannot be reached: the height limit is {:g} m AGL",
                                  *o.minAltitudeAglM, *limit));
            }
            if (o.type == ObjectiveType::Land && o.maxErrorM) {
                if (const ItemSpec* pad = m.item(o.target)) {
                    const double radius = items::defaultShape(pad->type, pad->properties).radiusM;
                    if (*o.maxErrorM > radius) {
                        warning(path + ".maxErrorM",
                                std::format("is larger than the pad radius ({:g} m)", radius));
                    }
                }
            }
        }
        for (std::size_t i = 0; i < m.items.size(); ++i) {
            const ItemSpec& item = m.items[i];
            const bool flown = item.type == ItemType::Waypoint || item.type == ItemType::Gate ||
                               item.type == ItemType::InspectionTarget;
            if (flown && item.position.altitudeRef == items::AltitudeRef::Agl &&
                above(item.position.altitudeM)) {
                error(std::format("items[{}]", i),
                      std::format("\"{}\" is at {:g} m AGL, above the {:g} m height limit", item.id,
                                  item.position.altitudeM, *limit));
            }
        }
        const auto& stars = m.scoring.stars;
        for (std::size_t i = 0; i < stars.size(); ++i) {
            const std::string path = std::format("scoring.stars[{}]", i);
            if (stars[i].maxTimeS && m.rules.timeLimitS && *stars[i].maxTimeS > *m.rules.timeLimitS) {
                warning(path + ".maxTimeS", "is longer than the time limit, so it never matters");
            }
            if (stars[i].minBatteryPct && *stars[i].minBatteryPct < m.rules.minBatteryPct) {
                warning(path + ".minBatteryPct", "is below rules.minBatteryPct, so it never matters");
            }
            if (stars[i].maxErrorM &&
                std::ranges::none_of(m.objectives, [](const ObjectiveSpec& o) {
                    return o.type == ObjectiveType::Land || o.type == ObjectiveType::Park ||
                           o.type == ObjectiveType::Hover || o.type == ObjectiveType::ReachWaypoint ||
                           o.type == ObjectiveType::Deliver || o.type == ObjectiveType::ReturnHome;
                })) {
                warning(path + ".maxErrorM", "no objective measures a position error");
            }
        }
        if (stars.size() == 2 && stars[0].maxTimeS && stars[1].maxTimeS &&
            *stars[1].maxTimeS > *stars[0].maxTimeS) {
            warning("scoring.stars[1].maxTimeS", "the 3rd star is easier than the 2nd");
        }
    }

    void checkWeather(const MissionDefinition& m) {
        const Json& w = m.environment.weather.json;
        if (!w.is_object() || w.empty()) {
            return;
        }
        checkConditions(w, w, "environment.weather");
        if (w.contains("timeline")) {
            double last = -1.0;
            for (std::size_t i = 0; i < w["timeline"].size(); ++i) {
                const Json& entry = w["timeline"][i];
                const std::string path = std::format("environment.weather.timeline[{}]", i);
                const double at = entry["atS"].get<double>();
                if (at <= last) {
                    error(path + ".atS", "timeline entries must be in increasing time order");
                }
                last = at;
                checkConditions(entry, w, path);
            }
        }
    }

    // Plausibility (FR-WX-05). @p base supplies values the timeline entry does not change.
    void checkConditions(const Json& w, const Json& base, const std::string& path) {
        const auto value = [&](const char* key) {
            return optNumber(w, key).or_else([&] { return optNumber(base, key); });
        };
        const std::string preset = text(w, "preset", text(base, "preset"));
        const auto temperature = value("temperatureC");
        const auto humidity = value("humidityPct");
        const bool snow = preset == "light-snow" || preset == "heavy-snow" ||
                          (w.contains("precipitation") &&
                           text(w["precipitation"], "type") == "snow");
        if (snow && temperature && *temperature > kSnowMaxTemperatureC) {
            error(path, std::format("snow at {:g} °C is not possible (at most {:g} °C)",
                                    *temperature, kSnowMaxTemperatureC));
        }
        const bool fog = preset == "fog" || preset == "mist";
        if (fog && humidity && *humidity < kFogMinHumidityPct) {
            error(path, std::format("{} needs a humidity of at least {:g} % (got {:g} %)", preset,
                                    kFogMinHumidityPct, *humidity));
        }
        const Json& windObj = w.contains("wind") ? w["wind"] : base.value("wind", Json::object());
        if (preset == "thunderstorm" && number(windObj, "level", 0) < 2) {
            warning(path, "a thunderstorm with calm wind is unusual (wind level below 2)");
        }
        const Json& clouds = w.contains("clouds") ? w["clouds"] : base.value("clouds", Json::array());
        double lowestBase = 1e9;
        for (std::size_t i = 0; i < clouds.size(); ++i) {
            const double b = clouds[i]["baseM"].get<double>();
            lowestBase = std::min(lowestBase, b);
            if (const auto top = optNumber(clouds[i], "topM"); top && *top <= b) {
                error(std::format("{}.clouds[{}].topM", path, i), "must be above baseM");
            }
        }
        const Json& fogObj = w.contains("fog") ? w["fog"] : base.value("fog", Json::object());
        if (const auto fogTop = optNumber(fogObj, "topM"); fogTop && *fogTop > lowestBase) {
            error(path, std::format("cloud base {:g} m is below the fog top {:g} m", lowestBase,
                                    *fogTop));
        }
    }

    const ValidationContext& ctx_;
    fs::path missionDir_;
    MissionLoadResult& result_;
};

}  // namespace

std::string MissionIssue::text() const { return std::format("{}: {}", path, message); }

std::string MissionLoadResult::report() const {
    std::string out;
    const std::string name = file.empty() ? std::string("mission") : file.string();
    for (const MissionIssue& e : errors) {
        out += std::format("{}: error: {}\n", name, e.text());
    }
    for (const MissionIssue& w : warnings) {
        out += std::format("{}: warning: {}\n", name, w.text());
    }
    for (const std::string& u : unavailable) {
        out += std::format("{}: unavailable: {}\n", name, u);
    }
    return out;
}

std::optional<items::Vec2> horizontalPosition(const MissionPosition& p,
                                              const std::optional<geo::LatLon>& origin,
                                              double originAltitudeM) {
    switch (p.kind) {
        case MissionPosition::Kind::Local: return items::Vec2(p.x, p.z);
        case MissionPosition::Kind::Geo:
        case MissionPosition::Kind::Utm: {
            if (!origin) {
                return std::nullopt;
            }
            geo::LatLon ll{p.latitudeDeg, p.longitudeDeg};
            if (p.kind == MissionPosition::Kind::Utm) {
                const auto converted = geo::fromUtm(p.utm);
                if (!converted) {
                    return std::nullopt;
                }
                ll = *converted;
            }
            const geo::LocalFrame frame(
                geo::Geodetic{origin->latitudeDeg, origin->longitudeDeg, originAltitudeM});
            const geo::Enu enu =
                frame.toEnu(geo::Geodetic{ll.latitudeDeg, ll.longitudeDeg, originAltitudeM});
            return items::Vec2(enu.east, -enu.north);
        }
    }
    return std::nullopt;
}

MissionLoadResult validateMission(const common::Json& json, const ValidationContext& context,
                                  const std::filesystem::path& missionDir) {
    MissionLoadResult result;
    try {
        Checker(context, missionDir, result).run(json);
    } catch (const std::exception& e) {
        // Defensive: the schema should make every access above safe.
        result.definition.reset();
        result.errors.push_back({"(root)", std::format("cannot read the mission: {}", e.what())});
    }
    return result;
}

MissionLoadResult loadMissionFile(const std::filesystem::path& file,
                                  const ValidationContext& context) {
    MissionLoadResult result;
    result.file = file;
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) {
        result.errors.push_back({"(root)", std::format("cannot read the file: {}", ec.message())});
        return result;
    }
    if (size > kMaxMissionFileBytes) {
        result.errors.push_back(
            {"(root)", std::format("the file is too large ({} bytes, limit {})", size,
                                   kMaxMissionFileBytes)});
        return result;
    }
    std::ifstream in(file, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    result.sha256 = common::sha256Hex(bytes);
    auto json = common::parseJson(bytes);
    if (!json) {
        result.errors.push_back({"(root)", json.error().message});
        return result;
    }
    MissionLoadResult checked = validateMission(*json, context, file.parent_path());
    checked.file = file;
    checked.sha256 = std::move(result.sha256);
    return checked;
}

void addBuiltInKeys(ValidationContext& context, const std::filesystem::path& assetDir) {
    const auto scan = [](const fs::path& dir, const char* member, std::set<std::string>& out) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            if (auto j = common::readJsonFile(entry.path(), std::size_t{1} << 20U);
                j && j->is_object() && j->contains(member) && (*j)[member].is_string()) {
                out.insert((*j)[member].get<std::string>());
            }
        }
    };
    scan(assetDir / "vehicles" / "aerial", "key", context.aerialProfiles);
    scan(assetDir / "vehicles" / "ground", "key", context.groundProfiles);
    scan(assetDir / "providers", "key", context.providers);
}

}  // namespace ds::missions
