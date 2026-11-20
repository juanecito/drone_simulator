// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Online elevation terrain in the application (M4 step 4) against the mock provider of
// tests/support/online_fixtures.hpp: the Terrain tab's online source with area download,
// quota and metadata; a restart in offline mode giving identical terrain; block streaming;
// and a mission whose area is downloaded before PreFlight (FR-TER-O01 … O18, FR-MIS-02,
// SPECIFICATION §8 "Online terrain scene").

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

#include "ds/app/asset_paths.hpp"
#include "ds/app/data_services.hpp"
#include "ds/app/main_window.hpp"
#include "ds/app/missions_controller.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/app/terrain_panel.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/common/json.hpp"
#include "ds/storage/mission_store.hpp"

#include "online_fixtures.hpp"

namespace ds::app {
namespace {

namespace fs = std::filesystem;

constexpr double kLat = 41.5934;
constexpr double kLon = 1.8376;

class OnlineTerrainTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home_.isValid());
        auto storage = openUserStorage(std::nullopt, homeDir());
        ASSERT_TRUE(storage.has_value());
        storage_ = std::move(*storage);
        settings_ = std::make_unique<SettingsController>(*storage_.service);
        fs::create_directories(homeDir() / "providers");
        std::ofstream(homeDir() / "providers" / "mockdem.json") << server_.providerJson();
        auto data = DataServices::create({.dataDirectory = homeDir() / "data",
                                          .providerPresets = homeDir() / "providers",
                                          .userStorage = storage_.service.get(),
                                          .keysFromEnvironment = false});
        ASSERT_TRUE(data.has_value()) << data.error().message;
        data_ = std::move(*data);
        data_->setApiKey("mockdem", "secret");
        data_->setConfirmDownload([this](QWidget*, const DownloadRequest& request) {
            asked_.push_back(request);
            return accept_;
        });
    }

    void TearDown() override {
        window_.reset();
        missions_.reset();
        data_.reset();
        settings_.reset();
    }

    [[nodiscard]] fs::path homeDir() const { return fs::path(home_.path().toStdString()); }

    static TerrainConfig onlineConfig(std::optional<terrain::Extent> extent) {
        TerrainConfig c;
        c.kind = TerrainKind::Online;
        c.online.provider = "mockdem";
        c.origin = {kLat, kLon, 0.0};
        c.extent = extent;
        return c;
    }

    // Expected world height of the mock surface at (x, z) in @p scene.
    static double expected(const SceneController& scene, double x, double z) {
        const geo::Geodetic g = scene.frame().toGeodetic(geo::Enu{x, -z, 0.0});
        return test::demHeight(g.latitudeDeg, g.longitudeDeg) - scene.terrain().origin.heightM;
    }

    bool waitFor(const std::function<bool()>& condition, int timeoutMs = 10000) {
        return QTest::qWaitFor(
            [&] {
                if (window_) {
                    window_->simulation()->updateFrame(0.016, 1.6);
                }
                return condition();
            },
            timeoutMs);
    }

    test::MockDemServer server_;
    QTemporaryDir home_;
    UserStorage storage_;
    std::unique_ptr<SettingsController> settings_;
    std::unique_ptr<DataServices> data_;
    std::unique_ptr<MissionsController> missions_;
    std::unique_ptr<MainWindow> window_;
    std::vector<DownloadRequest> asked_;
    bool accept_ = true;
};

TEST_F(OnlineTerrainTest, TerrainTabDownloadsTheAreaAndARestartOfflineGivesTheSameTerrain) {
    EXPECT_TRUE(fs::exists(homeDir() / "data" / "cache.db"));
    std::array<double, 4> heights{};
    std::string hash;
    {
        SceneController scene(settings_.get(), findAssetDirectory(), data_.get());
        TerrainPanel panel(scene);
        auto* source = panel.findChild<QComboBox*>(QStringLiteral("SourceCombo"));
        ASSERT_NE(source, nullptr);
        panel.setFormConfig(onlineConfig(terrain::Extent{3000.0, 2000.0}));
        EXPECT_EQ(source->currentIndex(), 1);
        EXPECT_EQ(panel.findChild<QComboBox*>(QStringLiteral("ProviderCombo"))->currentText(),
                  QStringLiteral("Mock DEM"));
        EXPECT_EQ(panel.findChild<QComboBox*>(QStringLiteral("DatasetCombo"))->currentData(),
                  QStringLiteral("TEST30"));
        EXPECT_TRUE(panel.findChild<QLabel*>(QStringLiteral("QuotaLabel"))
                        ->text()
                        .contains(QStringLiteral("50 of 50 calls left")));

        ASSERT_TRUE(panel.regenerate())
            << panel.findChild<QLabel*>(QStringLiteral("StatusLabel"))->text().toStdString();
        ASSERT_EQ(asked_.size(), 1U);  // FR-TER-O17: asked before spending a call.
        EXPECT_EQ(asked_[0].estimate.uncachedRequests, 1);
        EXPECT_EQ(server_.requestCount(), 1U);  // FR-TER-O16: one request for the area.
        EXPECT_EQ(scene.terrain().kind, TerrainKind::Online);
        ASSERT_TRUE(scene.onlineMetadata().has_value());
        EXPECT_FALSE(scene.onlineStatus().has_value());  // Bounded: no streaming.
        EXPECT_EQ(scene.source()->extent(), (terrain::Extent{3000.0, 2000.0}));
        EXPECT_NE(scene.attribution().find("Test DEM"), std::string::npos);
        EXPECT_EQ(scene.terrainDataHash().size(), 64U);
        EXPECT_NEAR(scene.query().height(0.0, 0.0), 0.0, 0.05);
        EXPECT_NEAR(scene.terrain().origin.heightM, test::demHeight(kLat, kLon), 1.0);
        EXPECT_NEAR(scene.query().height(1200.0, -700.0), expected(scene, 1200.0, -700.0), 1.5);
        const QString meta = panel.findChild<QLabel*>(QStringLiteral("MetadataLabel"))->text();
        EXPECT_TRUE(meta.contains(QStringLiteral("Provider: Mock DEM"))) << meta.toStdString();
        EXPECT_TRUE(meta.contains(QStringLiteral("EGM2008"))) << meta.toStdString();
        EXPECT_TRUE(meta.contains(QStringLiteral("Cached area"))) << meta.toStdString();
        EXPECT_TRUE(panel.findChild<QLabel*>(QStringLiteral("QuotaLabel"))
                        ->text()
                        .contains(QStringLiteral("49 of 50 calls left")));

        // Cached now: regenerating asks nothing and calls nobody.
        ASSERT_TRUE(panel.regenerate());
        EXPECT_EQ(asked_.size(), 1U);
        EXPECT_EQ(server_.requestCount(), 1U);

        // A declined download changes nothing.
        accept_ = false;
        panel.setFormConfig(onlineConfig(terrain::Extent{6000.0, 6000.0}));
        EXPECT_FALSE(panel.regenerate());
        EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("StatusLabel"))->text(),
                  QStringLiteral("Download cancelled."));
        EXPECT_EQ(scene.source()->extent(), (terrain::Extent{3000.0, 2000.0}));
        EXPECT_EQ(server_.requestCount(), 1U);

        heights = {scene.query().height(0.0, 0.0), scene.query().height(900.0, 400.0),
                   scene.query().height(-1400.0, -950.0), scene.query().height(10.0, 990.0)};
        hash = scene.terrainDataHash();
    }

    // Restart without network: the saved online terrain opens from the cache, identical.
    data_->http().setOffline(true);
    SceneController scene(settings_.get(), findAssetDirectory(), data_.get());
    ASSERT_EQ(scene.terrain().kind, TerrainKind::Online);
    EXPECT_EQ(scene.terrainDataHash(), hash);
    EXPECT_EQ(scene.query().height(0.0, 0.0), heights[0]);
    EXPECT_EQ(scene.query().height(900.0, 400.0), heights[1]);
    EXPECT_EQ(scene.query().height(-1400.0, -950.0), heights[2]);
    EXPECT_EQ(scene.query().height(10.0, 990.0), heights[3]);
    EXPECT_EQ(server_.requestCount(), 1U);
}

TEST_F(OnlineTerrainTest, StreamingOnlineTerrainLoadsBlocksAroundTheCamera) {
    SceneController scene(settings_.get(), findAssetDirectory(), data_.get());
    TerrainPanel panel(scene);
    panel.setFormConfig(onlineConfig(std::nullopt));
    ASSERT_TRUE(panel.regenerate());
    ASSERT_EQ(asked_.size(), 1U);  // The block under the origin.
    ASSERT_TRUE(scene.onlineStatus().has_value());
    EXPECT_FALSE(scene.source()->extent().has_value());
    EXPECT_EQ(scene.onlineStatus()->loadedBlocks, 1);

    // Flying east: the blocks ahead arrive in the background and the chunks are rebuilt.
    const double far = 8500.0;
    const double before = scene.query().height(far, 0.0);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            scene.focusTerrain(far, 0.0);
            (void)scene.streamer().update(far, 1500.0, 0.0);
            const auto s = scene.onlineStatus();
            return s && s->pendingBlocks == 0 && s->loadedBlocks >= 3;
        },
        10000))
        << scene.onlineStatus()->lastError;
    EXPECT_GT(scene.source()->revision(), 0U);
    EXPECT_NE(scene.query().height(far, 0.0), before);
    EXPECT_NEAR(scene.query().height(far, 0.0), expected(scene, far, 0.0), 1.5);
    EXPECT_GT(server_.requestCount(), 1U);
    EXPECT_EQ(asked_.size(), 1U);  // Streaming does not ask again (FR-TER-O16).
}

TEST_F(OnlineTerrainTest, OnlineMissionDownloadsItsAreaBeforePreFlight) {
    // A copy of "first-flight" on the mock provider's terrain.
    const fs::path dir = homeDir() / "missions" / "online-hop";
    fs::create_directories(dir);
    std::ifstream in(findAssetDirectory() / "default-missions" / "first-flight.mission.json");
    common::Json m = common::Json::parse(in);
    m["id"] = "online-hop";
    m["title"] = "Online hop";
    m["terrain"] = {{"source", "online"},
                    {"elevationProvider", "mockdem"},
                    {"dataset", "TEST30"},
                    {"origin", {{"geo", {kLat, kLon, 0}}, {"altRef", "AGL"}}},
                    {"extent", {{"widthM", 2000}, {"lengthM", 2000}}}};
    std::ofstream(dir / "online-hop.mission.json") << m.dump(2);

    missions_ = std::make_unique<MissionsController>(
        MissionFolders{
            .assetDir = findAssetDirectory(), .userDir = homeDir() / "missions", .libraryDir = {}},
        storage_.service.get(), data_.get());
    data_->setApiKey("mockdem", "");
    missions_->reload();
    const MissionRow* row = missions_->find("online-hop");
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->status, MissionStatus::Unavailable)  // No key and nothing cached.
        << row->statusDetail;
    EXPECT_NE(row->statusDetail.find("API key"), std::string::npos) << row->statusDetail;
    data_->setApiKey("mockdem", "secret");
    missions_->reload();
    row = missions_->find("online-hop");
    ASSERT_EQ(row->status, MissionStatus::Available) << row->statusDetail;

    window_ = std::make_unique<MainWindow>(settings_.get(), missions_.get(), data_.get());
    window_->show();
    SimulationController& sim = *window_->simulation();
    SceneController& scene = *window_->scene();

    // Declined: the mission does not start.
    accept_ = false;
    EXPECT_FALSE(window_->launchMission("online-hop").has_value());
    EXPECT_FALSE(sim.missionActive());
    EXPECT_EQ(server_.requestCount(), 0U);

    accept_ = true;
    auto launched = window_->launchMission("online-hop");
    ASSERT_TRUE(launched.has_value()) << launched.error().message;
    EXPECT_EQ(server_.requestCount(), 1U);
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
    EXPECT_EQ(scene.terrain().kind, TerrainKind::Online);
    EXPECT_EQ(scene.source()->extent(), (terrain::Extent{2000.0, 2000.0}));
    EXPECT_NEAR(sim.home().y, 0.0, 0.1);  // Spawn on the ground at the origin.
    const std::string hash = scene.terrainDataHash();
    ASSERT_EQ(hash.size(), 64U);

    sim.abortMission();
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().result.has_value(); }));
    auto stored = storage_.service
                      ->submit([](storage::DbConnection& c) {
                          return storage::MissionStore(c).attempts("online-hop");
                      })
                      .get();
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->size(), 1U);
    EXPECT_EQ((*stored)[0].terrainDataHash, hash);  // FR-TER-O14.
    window_->endMission();

    // Cached and pinned: playable without the key, launched without another call.
    data_->setApiKey("mockdem", "");
    missions_->reload();
    ASSERT_EQ(missions_->find("online-hop")->status, MissionStatus::Available);
    ASSERT_TRUE(window_->launchMission("online-hop").has_value());
    EXPECT_EQ(server_.requestCount(), 1U);
    EXPECT_EQ(asked_.size(), 2U);  // Only the two downloads were asked about.
    window_->endMission();
}

}  // namespace
}  // namespace ds::app
