// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// The built-in real-terrain missions (M4 step 9, SPECIFICATION §8 "File terrain missions"):
// their data is not shipped but fetched into the geodata library by
// scripts/data/fetch_mission_data.py, so
// - the data manifest must cover every library file the missions use;
// - with stand-in files of the same names, formats and CRSs (GeoTIFF tiles, a COG and a JPEG
//   orthophoto with a world file) the missions are available offline,
//   are flown on that data with their items where their coordinates say, and record the data
//   hash;
// - with the real data (DS_MISSION_LIBRARY=<library fetched by the script>) the pads are on
//   level ground and the beacons above it. CI has no such library and skips that test.

#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <numbers>
#include <regex>
#include <set>
#include <string>

#include "ds/app/asset_paths.hpp"
#include "ds/app/data_services.hpp"
#include "ds/app/main_window.hpp"
#include "ds/app/mission_dialogs.hpp"
#include "ds/app/mission_setup.hpp"
#include "ds/app/mission_validation.hpp"
#include "ds/app/missions_controller.hpp"
#include "ds/app/scene_config.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/common/json.hpp"
#include "ds/common/math.hpp"
#include "ds/missions/catalog.hpp"
#include "ds/missions/mission_loader.hpp"
#include "ds/missions/mission_runner.hpp"
#include "ds/missions/mission_world.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/frames.hpp"
#include "ds/sim/mission_session.hpp"
#include "ds/sim/world.hpp"
#include "ds/storage/mission_store.hpp"
#include "ds/terrain/online_imagery.hpp"
#include "ds/terrain/orthophoto.hpp"
#include "ds/terrain/query.hpp"
#include "ds/terrain/terrain_files.hpp"

#include "ortho_fixtures.hpp"
#include "raster_fixtures.hpp"

namespace ds::app {
namespace {

namespace fs = std::filesystem;

const std::vector<std::string> kMissions{"real-02-oeschinensee", "real-03-sau"};

fs::path sourceDir() { return fs::path(DS_TEST_ASSETS_DIR).parent_path(); }

common::Json readJson(const fs::path& file) {
    std::ifstream in(file);
    return common::Json::parse(in);
}

// The library paths of the built-in missions, by mission id.
std::map<std::string, std::set<std::string>> libraryFilesOfMissions() {
    std::map<std::string, std::set<std::string>> out;
    for (const fs::path& file :
         missions::findMissionFiles(fs::path(DS_TEST_ASSETS_DIR) / "missions")) {
        const common::Json m = readJson(file);
        if (!m.contains("terrain")) {
            continue;
        }
        for (const char* list : {"elevationFiles", "orthophotoFiles"}) {
            for (const auto& ref : m["terrain"].value(list, common::Json::array())) {
                const std::string path = ref["path"].get<std::string>();
                if (path.starts_with("library:")) {
                    out[m["id"].get<std::string>()].insert(path.substr(8));
                }
            }
        }
    }
    return out;
}

TEST(RealTerrainData, TheManifestCoversTheMissionsAndTheirLicences) {
    const common::Json manifest = readJson(sourceDir() / "scripts" / "data" / "mission_data.json");
    EXPECT_EQ(manifest["manifestVersion"], 1);
    auto used = libraryFilesOfMissions();
    for (const std::string& id : kMissions) {
        EXPECT_TRUE(used.contains(id)) << id << " has no library data";
    }
    std::map<std::string, std::set<std::string>> listed;
    const std::regex sha("[0-9a-f]{64}");
    for (const auto& d : manifest["datasets"]) {
        SCOPED_TRACE(d["id"].get<std::string>());
        for (const char* key : {"name", "attribution", "licence", "termsUrl"}) {
            EXPECT_FALSE(d[key].get<std::string>().empty()) << key;
        }
        EXPECT_TRUE(d["termsUrl"].get<std::string>().starts_with("https://"));
        for (const auto& f : d["files"]) {
            const std::string url = f["url"].get<std::string>();
            EXPECT_TRUE(url.starts_with("https://")) << url;
            // Published files are pinned; rendered ones (WMS) are checked as images.
            if (f.contains("sha256")) {
                EXPECT_TRUE(std::regex_match(f["sha256"].get<std::string>(), sha));
                EXPECT_GT(f["bytes"].get<std::int64_t>(), 0);
            } else {
                EXPECT_TRUE(f["contentType"].get<std::string>().starts_with("image/"));
            }
            for (const auto& m : d["missions"]) {
                listed[m.get<std::string>()].insert(f["path"].get<std::string>());
            }
        }
    }
    // Every file a mission uses can be fetched, and nothing else is.
    for (const auto& [id, files] : used) {
        for (const std::string& f : files) {
            EXPECT_TRUE(listed[id].contains(f)) << id << ": " << f << " is not in the manifest";
        }
    }
    for (const auto& [id, files] : listed) {
        EXPECT_TRUE(used.contains(id)) << id << " is not a built-in mission";
        for (const std::string& f : files) {
            EXPECT_TRUE(used[id].contains(f)) << id << ": " << f << " is not used";
        }
    }
}

// Stand-in data: the real file names, formats and CRSs over the missions' areas, with a gentle
// synthetic surface.
void writeStandIns(const fs::path& library) {
    // Oeschinensee: eight 1 km GeoTIFF tiles in LV95 (20 m pixels).
    for (const int e : {2620, 2621, 2622, 2623}) {
        for (const int n : {1149, 1150}) {
            test::RasterSpec s;
            s.width = s.height = 50;
            s.epsg = 2056;
            s.geoTransform = {e * 1000.0, 20.0, 0.0, (n + 1) * 1000.0, 0.0, -20.0};
            s.value = [e, n](int i, int j) {
                const double x = e * 1000.0 + (i + 0.5) * 20.0;
                const double y = (n + 1) * 1000.0 - (j + 0.5) * 20.0;
                return 1600.0 + 0.02 * (x - 2622000.0) + 0.03 * (y - 1150000.0);
            };
            const fs::path file = library / "oeschinensee" /
                                  std::format("swissalti3d_2025_{}-{}_2_2056_5728.tif", e, n);
            fs::create_directories(file.parent_path());
            test::writeRaster(file, s);
        }
    }
    // Sau: a COG in WGS 84 (3″ pixels) and a JPEG orthophoto with a world file in UTM 31N.
    {
        test::RasterSpec s;
        s.width = 240;
        s.height = 120;
        s.epsg = 4326;
        s.geoTransform = {2.3, 1.0 / 1200.0, 0.0, 42.0, 0.0, -1.0 / 1200.0};
        s.value = [](int i, int j) { return 450.0 + 0.5 * i - 0.3 * j; };
        s.driver = "COG";
        const fs::path dem = library / "sau" / "Copernicus_DSM_COG_10_N41_00_E002_00_DEM.tif";
        fs::create_directories(dem.parent_path());
        test::writeRaster(dem, s);
        test::ImageSpec image;
        image.width = image.height = 850;
        image.geoTransform = {448000.0, 4.0, 0.0, 4648700.0, 0.0, -4.0};
        image.epsg = 0;
        image.driver = "JPEG";
        image.worldFile = true;
        test::writeImage(library / "sau" / "pnoa-sau-25831.jpg", image);
    }
}

class RealTerrainMissionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home_.isValid());
        auto storage = openUserStorage(std::nullopt, homeDir());
        ASSERT_TRUE(storage.has_value());
        storage_ = std::move(*storage);
        settings_ = std::make_unique<SettingsController>(*storage_.service);
        // The shipped providers (the swisstopo imagery of two missions), offline: CI never
        // contacts a real provider.
        auto data = DataServices::create({.dataDirectory = homeDir() / "data",
                                          .providerPresets = findAssetDirectory() / "providers",
                                          .userStorage = storage_.service.get(),
                                          .keysFromEnvironment = false});
        ASSERT_TRUE(data.has_value()) << data.error().message;
        data_ = std::move(*data);
        data_->setOffline(true);
        fs::create_directories(homeDir() / "missions");
        fs::create_directories(library());
        missions_ =
            std::make_unique<MissionsController>(MissionFolders{.assetDir = findAssetDirectory(),
                                                                .userDir = homeDir() / "missions",
                                                                .libraryDir = library()},
                                                 storage_.service.get(), data_.get());
        // The missions open after a first star on "Hover" (their unlock rule).
        auto recorded = storage_.service
                            ->submit([](storage::DbConnection& c) {
                                return storage::MissionStore(c).recordAttempt(
                                    {.missionKey = "basics-01-hover",
                                     .startedAt = "2026-11-21T10:00:00Z",
                                     .outcome = "succeeded",
                                     .stars = 1});
                            })
                            .get();
        ASSERT_TRUE(recorded.has_value());
        missions_->reload();
    }

    void TearDown() override {
        window_.reset();
        missions_.reset();
        data_.reset();
        settings_.reset();
    }

    [[nodiscard]] fs::path homeDir() const { return fs::path(home_.path().toStdString()); }
    [[nodiscard]] fs::path library() const { return homeDir() / "library"; }

    bool waitFor(const std::function<bool()>& condition, int timeoutMs = 20000) {
        return QTest::qWaitFor(
            [&] {
                window_->simulation()->updateFrame(0.016, 1.6);
                return condition();
            },
            timeoutMs);
    }

    QTemporaryDir home_;
    UserStorage storage_;
    std::unique_ptr<SettingsController> settings_;
    std::unique_ptr<DataServices> data_;
    std::unique_ptr<MissionsController> missions_;
    std::unique_ptr<MainWindow> window_;
};

TEST_F(RealTerrainMissionsTest, WithoutTheirDataTheMissionsSayHowToGetIt) {
    for (const std::string& id : kMissions) {
        const MissionRow* row = missions_->find(id);
        ASSERT_NE(row, nullptr) << id;
        EXPECT_EQ(row->status, MissionStatus::Unavailable) << id;
        EXPECT_NE(row->statusDetail.find("fetch_mission_data.py"), std::string::npos)
            << row->statusDetail;
        EXPECT_NE(row->statusDetail.find(library().string()), std::string::npos)
            << row->statusDetail;
    }
    // Eight missing tiles are one line, not eight.
    const MissionRow* oes = missions_->find("real-02-oeschinensee");
    EXPECT_NE(oes->statusDetail.find("8 elevation files"), std::string::npos) << oes->statusDetail;
}

TEST_F(RealTerrainMissionsTest, OnTheirDataTheMissionsAreFlownWhereTheirCoordinatesSay) {
    writeStandIns(library());
    missions_->reload();
    window_ = std::make_unique<MainWindow>(settings_.get(), missions_.get(), data_.get());
    window_->show();
    SimulationController& sim = *window_->simulation();
    SceneController& scene = *window_->scene();
    for (const std::string& id : kMissions) {
        SCOPED_TRACE(id);
        const MissionRow* row = missions_->find(id);
        ASSERT_NE(row, nullptr);
        ASSERT_EQ(row->status, MissionStatus::Available) << row->statusDetail;
        // A copy: the list is reloaded when the attempt is stored.
        const missions::MissionDefinition def = *missions_->entry(*row).load.definition;
        auto launched = window_->launchMission(id);
        ASSERT_TRUE(launched.has_value()) << launched.error().message;
        ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
        EXPECT_EQ(scene.terrain().kind, TerrainKind::File);
        ASSERT_TRUE(def.terrain.extent.has_value());
        EXPECT_EQ(scene.source()->extent(), def.terrain.extent);
        EXPECT_NE(scene.attribution().find(def.terrain.file->attribution), std::string::npos)
            << scene.attribution();
        // Imagery: swisstopo's provider (offline, so streamed from the cache only) or the
        // orthophoto file.
        if (def.terrain.file->imageryProvider.empty()) {
            EXPECT_NE(dynamic_cast<const terrain::OrthophotoSource*>(scene.orthophotos().get()),
                      nullptr)
                << scene.orthoWarning();
        } else {
            EXPECT_NE(dynamic_cast<const terrain::OnlineImagerySource*>(scene.orthophotos().get()),
                      nullptr)
                << scene.orthoWarning();
            EXPECT_NE(scene.attribution().find("swisstopo"), std::string::npos);
        }
        // The vehicle stands on the ground the renderer draws, at the spawn coordinates.
        const terrain::HeightSource& ground = *scene.source();
        EXPECT_NEAR(sim.home().y, ground.height(sim.home().x, sim.home().z), 0.1);
        const geo::Geodetic spawn =
            scene.frame().toGeodetic(geo::Enu{sim.home().x, -sim.home().z, 0.0});
        ASSERT_EQ(def.spawn.kind, missions::MissionPosition::Kind::Geo);
        EXPECT_NEAR(spawn.latitudeDeg, def.spawn.latitudeDeg, 1e-6);
        EXPECT_NEAR(spawn.longitudeDeg, def.spawn.longitudeDeg, 1e-6);
        // Items by latitude / longitude land there, at their height above the ground.
        auto map = missionMapData(def, data_.get());
        ASSERT_TRUE(map.has_value()) << map.error().message;
        for (const items::Item& item : map->items) {
            SCOPED_TRACE(item.key);
            const auto spec = std::ranges::find(def.items, item.key, &missions::ItemSpec::id);
            ASSERT_NE(spec, def.items.end());
            const missions::MissionPosition& at = spec->position;
            ASSERT_EQ(at.kind, missions::MissionPosition::Kind::Geo);
            const glm::dvec3 p = item.transform.position;
            const geo::Geodetic g = scene.frame().toGeodetic(geo::Enu{p.x, -p.z, 0.0});
            EXPECT_NEAR(g.latitudeDeg, at.latitudeDeg, 1e-6);  // ≈ 0.1 m.
            EXPECT_NEAR(g.longitudeDeg, at.longitudeDeg, 1e-6);
            EXPECT_NEAR(p.y - ground.height(p.x, p.z), at.altitudeM, 0.05);
        }

        sim.abortMission();
        ASSERT_TRUE(waitFor([&] { return sim.missionStatus().result.has_value(); }));
        auto stored = storage_.service
                          ->submit([&id](storage::DbConnection& c) {
                              return storage::MissionStore(c).attempts(id);
                          })
                          .get();
        ASSERT_TRUE(stored.has_value());
        ASSERT_EQ(stored->size(), 1U);
        auto prepared = terrain::prepareFileTerrain(*def.terrain.file, {{}, library()});
        ASSERT_TRUE(prepared.has_value()) << prepared.error().message;
        EXPECT_EQ((*stored)[0].terrainDataHash, prepared->terrainDataHash);  // FR-TER-F09.
        window_->endMission();
    }
}

// The real-data tests: DS_MISSION_LIBRARY=<library filled by fetch_mission_data.py>.
const char* realLibrary() {
    const char* real = std::getenv("DS_MISSION_LIBRARY");
    return real != nullptr && *real != '\0' ? real : nullptr;
}
TEST_F(RealTerrainMissionsTest, OnTheRealDataPadsAreLevelAndBeaconsClearOfTheGround) {
    const char* real = realLibrary();
    if (real == nullptr) {
        GTEST_SKIP() << "set DS_MISSION_LIBRARY to a library filled by fetch_mission_data.py";
    }
    auto validation = MissionValidation::create(findAssetDirectory(), real);
    ASSERT_TRUE(validation.has_value());
    for (const fs::path& file : missions::findMissionFiles(findAssetDirectory() / "missions")) {
        auto result = missions::loadMissionFile(file, (*validation)->context);
        ASSERT_TRUE(result.valid()) << result.report();
        const missions::MissionDefinition& m = *result.definition;
        if (std::ranges::find(kMissions, m.id) == kMissions.end()) {
            continue;
        }
        SCOPED_TRACE(m.id);
        ASSERT_TRUE(result.unavailable.empty()) << result.report();
        auto config = missionTerrain(m);
        ASSERT_TRUE(config.has_value()) << config.error().message;
        auto map = missionMapData(m, data_.get());
        ASSERT_TRUE(map.has_value()) << map.error().message;
        const terrain::HeightSource& ground = *map->terrain;
        const auto inside = [&](const glm::dvec3& p, double margin) {
            return std::abs(p.x) <= map->extent->widthM / 2 - margin &&
                   std::abs(p.z) <= map->extent->lengthM / 2 - margin;
        };
        EXPECT_TRUE(inside(map->spawn, 100.0)) << "within the HUD's edge warning";
        for (const items::Item& item : map->items) {
            SCOPED_TRACE(item.key);
            const glm::dvec3 p = item.transform.position;
            EXPECT_TRUE(inside(p, 100.0)) << "within the HUD's edge warning";
            const double h = ground.height(p.x, p.z);
            if (item.type == items::ItemType::LandingPad) {
                for (int k = 0; k < 8; ++k) {
                    const double a = k * std::numbers::pi / 4.0;
                    EXPECT_NEAR(ground.height(p.x + 2.0 * std::cos(a), p.z + 2.0 * std::sin(a)), h,
                                0.25);
                }
            } else {
                // Clear of the ground under the whole beacon.
                for (int k = 0; k < 8; ++k) {
                    const double a = k * std::numbers::pi / 4.0;
                    const double r = item.shape.radiusM;
                    EXPECT_GT(p.y - ground.height(p.x + r * std::cos(a), p.z + r * std::sin(a)),
                              5.0);
                }
            }
        }
    }
}

// A pilot on the sticks in altitude-hold mode that follows the terrain: 25 m over the highest
// ground of the next 120 m, under 80 % of the height limit over the lowest ground of the next
// 100 m, creeping forward while it has to climb or descend; over the target it goes to the
// target's height.
class TerrainPilot {
public:
    TerrainPilot(sim::World& world, std::function<double(double, double)> ground,
                 double heightLimitM)
        : world_(&world), ground_(std::move(ground)), limit_(heightLimitM) {}

    // Towards @p target; @p land: descend onto it once above it.
    input::ControlInputs controls(const glm::dvec3& target, bool land) const {
        const physics::RigidBodyState& b = world_->vehicle().bodyState();
        input::ControlInputs c;
        c.arm = std::exchange(primed_, true);
        c.flightMode = 2;
        c.lights = true;
        glm::dvec2 to(target.x - b.position.x, target.z - b.position.z);
        const double distance = glm::length(to);
        const glm::dvec2 dir = distance > 1e-3 ? to / distance : glm::dvec2(0.0);
        const double below = ground_(b.position.x, b.position.z);
        double ahead = below;   // Highest ground of the next 120 m.
        double lowest = below;  // Lowest of the next 100 m: the height limit there.
        for (double d = 5.0; d <= std::min(distance, 120.0); d += 5.0) {
            const double g = ground_(b.position.x + dir.x * d, b.position.z + dir.y * d);
            ahead = std::max(ahead, g);
            lowest = d <= 100.0 ? std::min(lowest, g) : lowest;
        }
        double want = distance < 40.0 ? target.y : std::max(target.y, ahead + 25.0);
        want = std::min(want, lowest + 0.8 * limit_);  // Below the HUD's height warning.
        const bool creep = std::abs(want - b.position.y) > 8.0 || ahead + 10.0 > b.position.y;
        double climb = std::clamp(0.8 * (want - b.position.y), -3.0, 4.0);
        double speed = std::min(creep ? 3.0 : 8.0, 0.6 * distance);
        if (land && distance < 0.4) {
            const double agl = b.position.y - world_->vehicle().restHeightM() - below;
            climb = -std::clamp(0.5 * agl, 0.3, 1.5);
            speed = 0.6 * distance;
        }
        const glm::dvec2 err = dir * speed - glm::dvec2(b.velocity.x, b.velocity.z);
        const double yaw = physics::eulerFromOrientation(b.orientation).yaw;
        const glm::dvec2 forward(std::sin(yaw), -std::cos(yaw));
        const glm::dvec2 right(std::cos(yaw), std::sin(yaw));
        c.pitch = std::clamp(0.12 * glm::dot(err, forward), -0.5, 0.5);
        c.roll = std::clamp(0.12 * glm::dot(err, right), -0.5, 0.5);
        c.throttle = world_->vehicle().armed() ? stick(climb) : 0.5;
        return c;
    }

private:
    [[nodiscard]] double stick(double climbMs) const {
        constexpr double kDeadband = 0.1;
        const double maxClimb = world_->aerialVehicle()->profile().controller.maxClimbMs;
        const double f = std::clamp(climbMs / maxClimb, -1.0, 1.0);
        if (std::abs(f) < 1e-3) {
            return 0.5;
        }
        return 0.5 + std::copysign(0.5 * kDeadband + std::abs(f) * (0.5 - 0.5 * kDeadband), f);
    }

    sim::World* world_;
    std::function<double(double, double)> ground_;
    double limit_;
    mutable bool primed_ = false;
};

// Each tour is flown to the end by TerrainPilot in calm air: winnable within its time limit
// and height limit, without touching the ground (SPECIFICATION §8 "Headless missions").
TEST_F(RealTerrainMissionsTest, OnTheRealDataEachTourCanBeFlownToTheEnd) {
    const char* real = realLibrary();
    if (real == nullptr) {
        GTEST_SKIP() << "set DS_MISSION_LIBRARY to a library filled by fetch_mission_data.py";
    }
    auto validation = MissionValidation::create(findAssetDirectory(), real);
    ASSERT_TRUE(validation.has_value());
    auto profile = physics::loadAerialProfile(findAssetDirectory() / "vehicles" / "aerial" /
                                              "camera-250g.json");
    ASSERT_TRUE(profile.has_value());
    for (const fs::path& file : missions::findMissionFiles(findAssetDirectory() / "missions")) {
        auto result = missions::loadMissionFile(file, (*validation)->context);
        ASSERT_TRUE(result.valid()) << result.report();
        const missions::MissionDefinition& m = *result.definition;
        if (std::ranges::find(kMissions, m.id) == kMissions.end()) {
            continue;
        }
        SCOPED_TRACE(m.id);
        auto config = missionTerrain(m);
        ASSERT_TRUE(config.has_value()) << config.error().message;
        auto built = buildTerrain(*config, data_.get());
        ASSERT_TRUE(built.has_value()) << built.error().message;
        auto query = std::make_shared<const terrain::TerrainQuery>(
            built->source, terrain::ChunkLayout{}, built->origin.heightM);
        const auto height = [query](double x, double z) { return query->height(x, z); };

        missions::MissionPlacement placement;
        placement.origin = geo::LatLon{built->origin.latitudeDeg, built->origin.longitudeDeg};
        placement.ground = height;
        placement.originAltitudeM = built->origin.heightM;
        auto runner = missions::MissionRunner::create(m, placement);
        ASSERT_TRUE(runner.has_value()) << runner.error().message;
        sim::WorldConfig c;
        c.profile = *profile;
        c.seed = 7;
        c.ground = [query](double x, double z) {
            physics::GroundSample g;
            g.heightM = query->height(x, z);
            const terrain::Normal n = query->normal(x, z);
            g.normal = physics::Vec3(n.x, n.y, n.z);
            return g;
        };
        c.originAltitudeM = built->origin.heightM;
        c.startPosition = (*runner)->spawnPosition();
        c.startHeadingRad = common::degToRad((*runner)->spawnHeadingDeg());
        c.startUtc = m.environment.utc;
        c.geoOrigin = built->origin;
        sim::World world(std::move(c));
        world.setMission(std::make_unique<sim::MissionSession>(std::move(*runner)));
        const auto status = [&] {
            sim::WorldSnapshot snap;
            world.fillSnapshot(snap);
            return snap.mission;
        };
        const TerrainPilot pilot(world, height, m.rules.maxAltitudeAglM.value_or(500.0));
        constexpr double kDt = 1.0 / 250.0;
        const auto fly = [&](const glm::dvec3& target, bool land, const auto& done) {
            for (double t = 0.0; t < 900.0; t += kDt) {
                world.tick(pilot.controls(target, land), kDt);
                if (done() || status().result.has_value()) {
                    return;
                }
            }
        };
        const glm::dvec3 pad = world.vehicle().bodyState().position;
        fly(pad, false, [&] { return status().state == missions::MissionState::InProgress; });
        for (std::size_t i = 0; i < m.objectives.size() && !status().result; ++i) {
            const auto target = status().objectives.at(i).targetPosition;
            ASSERT_TRUE(target.has_value());
            const bool land = m.objectives[i].type == missions::ObjectiveType::Land;
            fly(*target, land, [&] {
                return status().objectives.at(i).state == missions::ObjectiveState::Completed;
            });
        }
        const sim::MissionStatus s = status();
        ASSERT_TRUE(s.result.has_value()) << "not finished";
        EXPECT_EQ(s.result->outcome, missions::MissionState::Succeeded) << s.result->failureReason;
        EXPECT_EQ(s.result->collisions, 0);
        EXPECT_EQ(s.result->ruleWarnings, 0);
        std::cout << m.id << ": " << s.result->durationS << " s of " << *m.rules.timeLimitS
                  << " s, " << s.result->stars << " star(s), battery " << s.result->batteryPct
                  << " %\n";
    }
}

}  // namespace
}  // namespace ds::app
