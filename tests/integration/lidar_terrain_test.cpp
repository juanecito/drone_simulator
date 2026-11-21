// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// LAS / LAZ terrain in the application (M4 step 6): the Terrain tab's point-cloud settings,
// the DTM made once and cached (FR-TER-L05), its metadata (FR-TER-L06), and a mission flown
// on a point cloud from its mission folder.

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

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
#include "ds/terrain/terrain_files.hpp"

#include "lidar_fixtures.hpp"

namespace ds::app {
namespace {

namespace fs = std::filesystem;

constexpr double kX0 = 400000.0;
constexpr double kY0 = 4600000.0;
constexpr double kSideM = 400.0;

int demFiles(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        n += e.path().extension() == ".tif" ? 1 : 0;
    }
    return n;
}

class LidarTerrainTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home_.isValid());
        auto storage = openUserStorage(std::nullopt, homeDir());
        ASSERT_TRUE(storage.has_value());
        storage_ = std::move(*storage);
        settings_ = std::make_unique<SettingsController>(*storage_.service);
        fs::create_directories(homeDir() / "providers");
        auto data = DataServices::create({.dataDirectory = homeDir() / "data",
                                          .providerPresets = homeDir() / "providers",
                                          .userStorage = storage_.service.get(),
                                          .keysFromEnvironment = false});
        ASSERT_TRUE(data.has_value()) << data.error().message;
        data_ = std::move(*data);
        fs::create_directories(homeDir() / "missions");
        fs::create_directories(homeDir() / "library");
        missions_ = std::make_unique<MissionsController>(
            MissionFolders{.assetDir = findAssetDirectory(),
                           .userDir = homeDir() / "missions",
                           .libraryDir = homeDir() / "library"},
            storage_.service.get(), data_.get());
    }

    void TearDown() override {
        window_.reset();
        missions_.reset();
        data_.reset();
        settings_.reset();
    }

    [[nodiscard]] fs::path homeDir() const { return fs::path(home_.path().toStdString()); }

    static void writeCloud(const fs::path& file, bool classified = true) {
        auto points = test::lidarPoints(kX0, kY0, kSideM, kSideM, 2.0);
        if (!classified) {
            for (auto& p : points) {
                p.classification = 1;
            }
        }
        fs::create_directories(file.parent_path());
        std::string why;
        ASSERT_TRUE(
            test::writeLidar(file, points, {.compressed = file.extension() == ".laz"}, &why))
            << why;
    }

    // A copy of "first-flight" flown on a LAZ point cloud in its mission folder.
    void writeLidarMission(bool classified = true) {
        const fs::path dir = homeDir() / "missions" / "lidar-hop";
        writeCloud(dir / "pc" / "site.laz", classified);
        std::ifstream in(findAssetDirectory() / "default-missions" / "first-flight.mission.json");
        common::Json m = common::Json::parse(in);
        m["id"] = "lidar-hop";
        m["title"] = "Lidar hop";
        m["terrain"] = {{"source", "file"},
                        {"format", "laz"},
                        {"elevationFiles", common::Json::array({{{"path", "pc/site.laz"}}})},
                        {"resolutionM", 2.0},
                        {"interpolation", "idw"},
                        {"attribution", "Test lidar, CC BY 4.0"},
                        {"extent", {{"widthM", 300}, {"lengthM", 300}}}};
        std::ofstream(dir / "lidar-hop.mission.json") << m.dump(2);
        missions_->reload();
    }

    bool waitFor(const std::function<bool()>& condition, int timeoutMs = 5000) {
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

TEST_F(LidarTerrainTest, TerrainTabMakesTheDtmOnceAndShowsWhatItCameFrom) {
    const fs::path las = homeDir() / "site.las";
    writeCloud(las);
    const fs::path demDir = data_->demDirectory();
    {
        SceneController scene(settings_.get(), findAssetDirectory(), data_.get());
        TerrainPanel panel(scene);
        TerrainConfig config;
        config.kind = TerrainKind::File;
        config.file.files = {las};
        config.file.resolutionM = 2.0;
        panel.setFormConfig(config);
        // Point-cloud settings instead of the raster band.
        auto* lidarBox = panel.findChild<QWidget*>(QStringLiteral("LidarBox"));
        ASSERT_NE(lidarBox, nullptr);
        EXPECT_FALSE(lidarBox->isHidden());
        EXPECT_FALSE(panel.findChild<QCheckBox*>(QStringLiteral("LidarAutoCheck"))->isChecked());
        EXPECT_DOUBLE_EQ(
            panel.findChild<QDoubleSpinBox*>(QStringLiteral("LidarResolutionSpin"))->value(), 2.0);
        EXPECT_EQ(panel.formConfig().file.resolutionM, 2.0);
        auto* cell = panel.findChild<QComboBox*>(QStringLiteral("LidarCellCombo"));
        ASSERT_NE(cell, nullptr);
        EXPECT_EQ(panel.formConfig().file.cellHeight, terrain::LidarCellHeight::Mean);
        cell->setCurrentIndex(cell->findData(static_cast<int>(terrain::LidarCellHeight::Lowest)));
        EXPECT_EQ(panel.formConfig().file.cellHeight, terrain::LidarCellHeight::Lowest);
        cell->setCurrentIndex(cell->findData(static_cast<int>(terrain::LidarCellHeight::Mean)));

        ASSERT_TRUE(panel.regenerate());
        EXPECT_EQ(demFiles(demDir), 1);
        ASSERT_TRUE(scene.terrainMetadata().has_value());
        EXPECT_NEAR(scene.terrainMetadata()->resolutionM, 2.0, 0.01);
        EXPECT_NEAR(scene.query().height(0.0, 0.0), 0.0, 1e-3);
        EXPECT_NEAR(scene.terrain().origin.heightM,
                    test::lidarSurface(kX0 + kSideM / 2, kY0 + kSideM / 2), 0.6);
        const QString meta = panel.findChild<QLabel*>(QStringLiteral("MetadataLabel"))->text();
        EXPECT_TRUE(meta.contains(QStringLiteral("LAS point cloud"))) << meta.toStdString();
        EXPECT_TRUE(meta.contains(QStringLiteral("ground"))) << meta.toStdString();
        EXPECT_TRUE(meta.contains(QStringLiteral("EPSG:25831"))) << meta.toStdString();

        // The cached DTM is used again: no new file.
        const auto made = fs::last_write_time(fs::directory_iterator(demDir)->path());
        ASSERT_TRUE(panel.regenerate());
        EXPECT_EQ(demFiles(demDir), 1);
        EXPECT_EQ(fs::last_write_time(fs::directory_iterator(demDir)->path()), made);

        // Another cell size is another DTM.
        config.file.resolutionM = 4.0;
        panel.setFormConfig(config);
        ASSERT_TRUE(panel.regenerate());
        EXPECT_EQ(demFiles(demDir), 2);
        EXPECT_NEAR(scene.terrainMetadata()->resolutionM, 4.0, 0.01);
    }
    // Saved: the next start opens the cached DTM.
    {
        SceneController scene(settings_.get(), findAssetDirectory(), data_.get());
        EXPECT_EQ(scene.terrain().kind, TerrainKind::File);
        EXPECT_TRUE(scene.terrainMetadata().has_value());
    }
    // The DTMs are part of the cache: counted, and cleared with it.
    bool counted = false;
    for (const net::CacheUsage& c : data_->cacheUsage()) {
        if (c.provider == DataServices::kLidarDemCache) {
            counted = true;
            EXPECT_EQ(c.entries, 2);
            EXPECT_GT(c.bytes, 0U);
        }
    }
    EXPECT_TRUE(counted);
    ASSERT_TRUE(data_->clearCache(false));
    EXPECT_EQ(demFiles(demDir), 0);
}

TEST_F(LidarTerrainTest, TerrainTabShowsWhyACloudCannotBeUsed) {
    const fs::path raw = homeDir() / "raw.las";
    writeCloud(raw, false);
    SceneController scene(settings_.get(), findAssetDirectory(), data_.get());
    TerrainPanel panel(scene);
    TerrainConfig config;
    config.kind = TerrainKind::File;
    config.file.files = {raw};
    panel.setFormConfig(config);
    EXPECT_FALSE(panel.regenerate());
    const QString status = panel.findChild<QLabel*>(QStringLiteral("StatusLabel"))->text();
    EXPECT_TRUE(status.contains(QStringLiteral("no ground points"))) << status.toStdString();
    EXPECT_EQ(scene.terrain().kind, TerrainKind::Procedural);
    EXPECT_EQ(demFiles(data_->demDirectory()), 0);
}

TEST_F(LidarTerrainTest, MissionOnAPointCloudIsFlownOnItsDtm) {
    writeLidarMission();
    const MissionRow* row = missions_->find("lidar-hop");
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->status, MissionStatus::Available) << row->statusDetail;
    window_ = std::make_unique<MainWindow>(settings_.get(), missions_.get(), data_.get());
    window_->show();
    SimulationController& sim = *window_->simulation();
    SceneController& scene = *window_->scene();
    auto launched = window_->launchMission("lidar-hop");
    ASSERT_TRUE(launched.has_value()) << launched.error().message;
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
    EXPECT_EQ(demFiles(data_->demDirectory()), 1);
    EXPECT_EQ(scene.terrain().kind, TerrainKind::File);
    EXPECT_EQ(scene.terrain().file.interpolation, terrain::LidarInterpolation::Idw);
    EXPECT_EQ(scene.attribution(), "Test lidar, CC BY 4.0");
    EXPECT_EQ(scene.source()->extent(), (terrain::Extent{300.0, 300.0}));
    EXPECT_NEAR(sim.home().y, 0.0, 0.1);

    sim.abortMission();
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().result.has_value(); }));
    auto stored = storage_.service
                      ->submit([](storage::DbConnection& c) {
                          return storage::MissionStore(c).attempts("lidar-hop");
                      })
                      .get();
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->size(), 1U);
    // FR-TER-F09: the hash of the point cloud and its rasterization settings.
    const auto& def = *missions_->entry(*missions_->find("lidar-hop")).load.definition;
    auto prepared = terrain::prepareFileTerrain(
        *def.terrain.file, {homeDir() / "missions" / "lidar-hop", homeDir() / "library"});
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ((*stored)[0].terrainDataHash, prepared->terrainDataHash);
    window_->endMission();
}

TEST_F(LidarTerrainTest, AnUnclassifiedCloudStopsTheMissionWithAClearMessage) {
    writeLidarMission(false);
    window_ = std::make_unique<MainWindow>(settings_.get(), missions_.get(), data_.get());
    auto launched = window_->launchMission("lidar-hop");
    ASSERT_FALSE(launched.has_value());
    EXPECT_NE(launched.error().message.find("no ground points"), std::string::npos)
        << launched.error().message;
    EXPECT_FALSE(window_->simulation()->missionActive());
}

TEST_F(LidarTerrainTest, WithoutTheDataCacheTheMissionIsUnavailable) {
    writeLidarMission();
    MissionsController offline(MissionFolders{.assetDir = findAssetDirectory(),
                                              .userDir = homeDir() / "missions",
                                              .libraryDir = homeDir() / "library"},
                               storage_.service.get());
    const MissionRow* row = offline.find("lidar-hop");
    ASSERT_NE(row, nullptr);
    EXPECT_NE(row->status, MissionStatus::Available);
    EXPECT_NE(row->statusDetail.find("data cache"), std::string::npos) << row->statusDetail;
}

}  // namespace
}  // namespace ds::app
