// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Elevation-file terrain in the application (M4 step 3): the Terrain tab's file source, the
// saved scene, and a mission flown on elevation files from its mission folder (FR-TER-F02,
// F03, F09, F10, F11).

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

#include "ds/app/asset_paths.hpp"
#include "ds/app/main_window.hpp"
#include "ds/app/missions_controller.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/app/terrain_panel.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/common/json.hpp"
#include "ds/common/sha256.hpp"
#include "ds/storage/mission_store.hpp"
#include "ds/terrain/terrain_files.hpp"

#include "ortho_fixtures.hpp"
#include "raster_fixtures.hpp"

namespace ds::app {
namespace {

namespace fs = std::filesystem;

class FileTerrainTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home_.isValid());
        auto storage = openUserStorage(std::nullopt, homeDir());
        ASSERT_TRUE(storage.has_value());
        storage_ = std::move(*storage);
        settings_ = std::make_unique<SettingsController>(*storage_.service);
        fs::create_directories(homeDir() / "missions");
        fs::create_directories(homeDir() / "library");
        missions_ = std::make_unique<MissionsController>(
            MissionFolders{.assetDir = findAssetDirectory(),
                           .userDir = homeDir() / "missions",
                           .libraryDir = homeDir() / "library"},
            storage_.service.get());
    }

    void TearDown() override {
        window_.reset();
        missions_.reset();
        settings_.reset();
    }

    [[nodiscard]] fs::path homeDir() const { return fs::path(home_.path().toStdString()); }

    void openWindow() {
        window_ = std::make_unique<MainWindow>(settings_.get(), missions_.get());
        window_->show();
    }

    bool waitFor(const std::function<bool()>& condition, int timeoutMs = 5000) {
        return QTest::qWaitFor(
            [&] {
                window_->simulation()->updateFrame(0.016, 1.6);
                return condition();
            },
            timeoutMs);
    }

    // 1 m imagery over the middle 800 m of the 2 km DEM tile.
    static test::ImageSpec orthoSpec() {
        test::ImageSpec image;
        image.width = 800;
        image.height = 800;
        image.geoTransform = {test::kE0 + 600.0, 1.0, 0.0, test::kN0 - 600.0, 0.0, -1.0};
        return image;
    }

    // A copy of "first-flight" flown on the planar UTM tile in its mission folder.
    void writeDemMission(const std::string& sha256 = {}, const std::string& orthoSha256 = {}) {
        const fs::path dir = homeDir() / "missions" / "dem-hop";
        fs::create_directories(dir / "dem");
        test::writeRaster(dir / "dem" / "valley.tif", test::planarUtmSpec());
        std::ifstream in(findAssetDirectory() / "default-missions" / "first-flight.mission.json");
        common::Json m = common::Json::parse(in);
        m["id"] = "dem-hop";
        m["title"] = "DEM hop";
        common::Json file = {{"path", "dem/valley.tif"}};
        if (!sha256.empty()) {
            file["sha256"] = sha256;
        }
        m["terrain"] = {{"source", "file"},
                        {"format", "raster"},
                        {"elevationFiles", common::Json::array({file})},
                        {"verticalDatum", "EGM2008"},
                        {"attribution", "Test DEM, CC BY 4.0"},
                        {"extent", {{"widthM", 1500}, {"lengthM", 1500}}}};
        if (!orthoSha256.empty()) {
            fs::create_directories(dir / "ortho");
            test::writeImage(dir / "ortho" / "a.tif", orthoSpec());
            common::Json image = {{"path", "ortho/a.tif"}};
            if (orthoSha256 != "-") {
                image["sha256"] = orthoSha256;
            }
            m["terrain"]["orthophotoFiles"] = common::Json::array({image});
            m["terrain"]["orthophotoAttribution"] = "Test imagery, CC0";
        }
        std::ofstream(dir / "dem-hop.mission.json") << m.dump(2);
        missions_->reload();
    }

    QTemporaryDir home_;
    UserStorage storage_;
    std::unique_ptr<SettingsController> settings_;
    std::unique_ptr<MissionsController> missions_;
    std::unique_ptr<MainWindow> window_;
};

TEST_F(FileTerrainTest, TerrainTabLoadsElevationFilesAndShowsTheirMetadata) {
    const fs::path dem = homeDir() / "dem.tif";
    test::writeRaster(dem, test::planarUtmSpec());
    {
        SceneController scene(settings_.get(), findAssetDirectory());
        TerrainPanel panel(scene);
        TerrainConfig config;
        config.kind = TerrainKind::File;
        config.file.files = {dem};
        config.file.attribution = "Test DEM";
        panel.setFormConfig(config);
        auto* source = panel.findChild<QComboBox*>(QStringLiteral("SourceCombo"));
        ASSERT_NE(source, nullptr);
        EXPECT_EQ(source->currentIndex(), 2);
        EXPECT_EQ(panel.findChild<QListWidget*>(QStringLiteral("FileList"))->count(), 1);
        EXPECT_TRUE(panel.findChild<QCheckBox*>(QStringLiteral("AutoOriginCheck"))->isChecked());
        ASSERT_TRUE(panel.regenerate());

        EXPECT_EQ(scene.terrain().kind, TerrainKind::File);
        ASSERT_TRUE(scene.terrainMetadata().has_value());
        EXPECT_EQ(scene.attribution(), "Test DEM");
        EXPECT_NEAR(scene.terrain().origin.heightM,
                    test::planeUtm(test::kE0 + 1000.0, test::kN0 - 1000.0), 1e-3);
        EXPECT_NEAR(scene.query().height(0.0, 0.0), 0.0, 1e-3);
        const QString meta = panel.findChild<QLabel*>(QStringLiteral("MetadataLabel"))->text();
        EXPECT_TRUE(meta.contains(QStringLiteral("EPSG:32630"))) << meta.toStdString();
        EXPECT_TRUE(meta.contains(QStringLiteral("GTiff"))) << meta.toStdString();
        // The form keeps "automatic origin": the next regenerate finds it again.
        EXPECT_FALSE(panel.formConfig().file.origin.has_value());
    }
    // Saved: the next start opens the same files.
    {
        SceneController scene(settings_.get(), findAssetDirectory());
        EXPECT_EQ(scene.terrain().kind, TerrainKind::File);
        EXPECT_TRUE(scene.terrainMetadata().has_value());
    }
    // Files gone: the default terrain, without losing the saved choice.
    fs::remove(dem);
    {
        SceneController scene(settings_.get(), findAssetDirectory());
        EXPECT_EQ(scene.terrain().kind, TerrainKind::Procedural);
        EXPECT_FALSE(scene.terrainMetadata().has_value());
        TerrainPanel panel(scene);
        TerrainConfig config;
        config.kind = TerrainKind::File;
        config.file.files = {dem};
        panel.setFormConfig(config);
        EXPECT_FALSE(panel.regenerate());  // The error is shown in the panel.
        EXPECT_EQ(scene.terrain().kind, TerrainKind::Procedural);
    }
}

// "Fit to the data": the terrain covers the elevation files, and the Terrain tab shows their
// size, from which a custom size starts.
TEST_F(FileTerrainTest, TerrainTabFitsTheTerrainToTheData) {
    const fs::path dem = homeDir() / "wide.tif";
    test::RasterSpec wide = test::planarUtmSpec();
    wide.width = 300;  // 3 × 1.5 km at 10 m.
    wide.height = 150;
    test::writeRaster(dem, wide);
    SceneController scene(settings_.get(), findAssetDirectory());
    TerrainPanel panel(scene);
    auto* dimensions = panel.findChild<QComboBox*>(QStringLiteral("DimensionsCombo"));
    auto* width = panel.findChild<QDoubleSpinBox*>(QStringLiteral("WidthSpin"));
    auto* length = panel.findChild<QDoubleSpinBox*>(QStringLiteral("LengthSpin"));
    auto* source = panel.findChild<QComboBox*>(QStringLiteral("SourceCombo"));
    ASSERT_NE(dimensions, nullptr);
    const auto* items = qobject_cast<QStandardItemModel*>(dimensions->model());
    ASSERT_NE(items, nullptr);
    EXPECT_FALSE(items->item(2)->isEnabled());  // Procedural terrain has no data to fit.

    TerrainConfig config;
    config.kind = TerrainKind::File;
    config.file.files = {dem};
    panel.setFormConfig(config);
    EXPECT_EQ(dimensions->currentIndex(), 2);
    EXPECT_TRUE(items->item(2)->isEnabled());
    EXPECT_FALSE(items->item(0)->isEnabled());  // Files are never unlimited.
    EXPECT_FALSE(width->isEnabled());
    ASSERT_TRUE(panel.regenerate());
    EXPECT_FALSE(panel.formConfig().extent.has_value());
    const auto extent = scene.source()->extent();
    ASSERT_TRUE(extent.has_value());
    // Nearly all of it: a true-north rectangle inside a UTM grid tile (whose north is a little
    // rotated), short of its outer half-pixels.
    EXPECT_GT(extent->widthM, 0.98 * 3000.0);
    EXPECT_LE(extent->widthM, 3000.0);
    EXPECT_GT(extent->lengthM, 0.98 * 1500.0);
    EXPECT_LE(extent->lengthM, 1500.0);
    EXPECT_NEAR(width->value(), extent->widthM / 1000.0, 1e-3);
    EXPECT_NEAR(length->value(), extent->lengthM / 1000.0, 1e-3);

    // Custom starts from the data's size; a part of it is fine.
    dimensions->setCurrentIndex(1);
    EXPECT_TRUE(width->isEnabled());
    EXPECT_NEAR(width->value(), extent->widthM / 1000.0, 1e-3);
    width->setValue(2.0);
    ASSERT_TRUE(panel.regenerate());
    const auto part = scene.source()->extent();
    ASSERT_TRUE(part.has_value());
    EXPECT_DOUBLE_EQ(part->widthM, 2000.0);
    EXPECT_NEAR(part->lengthM, extent->lengthM, 1.0);  // Shown to the metre.
    // More than the data: refused, pointing at "Fit to the data".
    width->setValue(5.0);
    EXPECT_FALSE(panel.regenerate());
    const QString status = panel.findChild<QLabel*>(QStringLiteral("StatusLabel"))->text();
    EXPECT_TRUE(status.contains(QStringLiteral("Fit to the data"))) << status.toStdString();

    // Back to procedural terrain from "Fit to the data": unlimited.
    dimensions->setCurrentIndex(2);
    source->setCurrentIndex(0);
    EXPECT_EQ(dimensions->currentIndex(), 0);
    EXPECT_FALSE(items->item(2)->isEnabled());
}

TEST_F(FileTerrainTest, MissionOnElevationFilesIsFlownAndRecordsItsDataHash) {
    writeDemMission();
    const MissionRow* row = missions_->find("dem-hop");
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->status, MissionStatus::Available) << row->statusDetail;
    openWindow();
    SimulationController& sim = *window_->simulation();
    SceneController& scene = *window_->scene();
    auto launched = window_->launchMission("dem-hop");
    ASSERT_TRUE(launched.has_value()) << launched.error().message;
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
    EXPECT_EQ(scene.terrain().kind, TerrainKind::File);
    EXPECT_EQ(scene.attribution(), "Test DEM, CC BY 4.0");
    EXPECT_EQ(scene.source()->extent(), (terrain::Extent{1500.0, 1500.0}));
    // The origin is the centre of the data, the spawn on the ground there.
    EXPECT_NEAR(scene.terrain().origin.heightM,
                test::planeUtm(test::kE0 + 1000.0, test::kN0 - 1000.0), 1e-3);
    EXPECT_NEAR(sim.home().y, 0.0, 0.1);

    sim.abortMission();
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().result.has_value(); }));
    auto stored = storage_.service
                      ->submit([](storage::DbConnection& c) {
                          return storage::MissionStore(c).attempts("dem-hop");
                      })
                      .get();
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->size(), 1U);
    // FR-TER-F09: the attempt records the hash of the elevation data and its settings.
    const auto& def = *missions_->entry(*missions_->find("dem-hop")).load.definition;
    auto prepared = terrain::prepareFileTerrain(
        *def.terrain.file, {homeDir() / "missions" / "dem-hop", homeDir() / "library"});
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ((*stored)[0].terrainDataHash, prepared->terrainDataHash);
    EXPECT_EQ((*stored)[0].terrainDataHash.size(), 64U);

    window_->endMission();
    EXPECT_EQ(scene.terrain().kind, TerrainKind::Procedural);  // Free mode again.
    EXPECT_TRUE(scene.attribution().empty());
}

TEST_F(FileTerrainTest, MissionOrthophotosAreDrapedAndABrokenOneOnlyDisablesThem) {
    // FR-TER-I02, I08, I09: the imagery comes with the mission; a damaged file only disables
    // it, and the terrain data hash ignores it.
    writeDemMission({}, "-");
    openWindow();
    SceneController& scene = *window_->scene();
    SimulationController& sim = *window_->simulation();
    auto launched = window_->launchMission("dem-hop");
    ASSERT_TRUE(launched.has_value()) << launched.error().message;
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
    ASSERT_NE(scene.orthophotos(), nullptr) << scene.orthoWarning();
    EXPECT_NE(scene.orthoStreamer(), nullptr);
    EXPECT_EQ(scene.attribution(), "Test DEM, CC BY 4.0 · Test imagery, CC0");
    EXPECT_NEAR(scene.orthophotos()->metadata().minX, -404.0, 4.0);
    sim.abortMission();
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().result.has_value(); }));
    window_->endMission();
    EXPECT_EQ(scene.orthophotos(), nullptr);  // Free mode: procedural terrain.

    writeDemMission({}, std::string(64, 'b'));  // Not the image's sha256.
    launched = window_->launchMission("dem-hop");
    ASSERT_TRUE(launched.has_value()) << launched.error().message;
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
    EXPECT_EQ(scene.terrain().kind, TerrainKind::File);
    EXPECT_EQ(scene.orthophotos(), nullptr);
    EXPECT_TRUE(window_->statusBar()->currentMessage().contains(QStringLiteral("Orthophotos")))
        << window_->statusBar()->currentMessage().toStdString();
    sim.abortMission();
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().result.has_value(); }));
    window_->endMission();
}

TEST_F(FileTerrainTest, TerrainTabDrapesOrthophotos) {
    const fs::path dem = homeDir() / "dem.tif";
    const fs::path image = homeDir() / "ortho.tif";
    test::writeRaster(dem, test::planarUtmSpec());
    test::writeImage(image, orthoSpec());
    SceneController scene(settings_.get(), findAssetDirectory());
    TerrainPanel panel(scene);
    TerrainConfig config;
    config.kind = TerrainKind::File;
    config.file.files = {dem};
    config.ortho.files = {image};
    config.ortho.attribution = "Test imagery";
    config.ortho.color.gamma = 1.2;
    panel.setFormConfig(config);
    auto* box = panel.findChild<QWidget*>(QStringLiteral("OrthoBox"));
    ASSERT_NE(box, nullptr);
    EXPECT_FALSE(box->isHidden());
    EXPECT_EQ(panel.findChild<QListWidget*>(QStringLiteral("OrthoList"))->count(), 1);
    EXPECT_EQ(panel.formConfig().ortho, config.ortho);
    ASSERT_TRUE(panel.regenerate());
    ASSERT_NE(scene.orthophotos(), nullptr);
    EXPECT_EQ(scene.attribution(), "Test imagery");
    const QString info = panel.findChild<QLabel*>(QStringLiteral("OrthoInfoLabel"))->text();
    EXPECT_TRUE(info.contains(QStringLiteral("Covers 0.8"))) << info.toStdString();
    EXPECT_TRUE(info.contains(QStringLiteral("RGB"))) << info.toStdString();
    // Procedural terrain: the group is hidden and the imagery is not used.
    auto* source = panel.findChild<QComboBox*>(QStringLiteral("SourceCombo"));
    source->setCurrentIndex(0);
    EXPECT_TRUE(box->isHidden());
    EXPECT_TRUE(panel.formConfig().ortho.files.empty());
}

TEST_F(FileTerrainTest, TerrainTabInterpolatesAndSmoothsTheElevation) {
    const fs::path dem = homeDir() / "dem.tif";
    test::RasterSpec spec = test::planarUtmSpec();
    spec.value = [](int i, int j) { return i == 100 && j == 100 ? 900.0 : 500.0; };  // A spike.
    test::writeRaster(dem, spec);
    SceneController scene(settings_.get(), findAssetDirectory());
    TerrainPanel panel(scene);
    TerrainConfig config;
    config.kind = TerrainKind::File;
    config.file.files = {dem};
    panel.setFormConfig(config);
    auto* box = panel.findChild<QWidget*>(QStringLiteral("ProcessingBox"));
    ASSERT_NE(box, nullptr);
    EXPECT_FALSE(box->isHidden());
    auto* interpolation = panel.findChild<QComboBox*>(QStringLiteral("InterpolationCombo"));
    auto* despike = panel.findChild<QDoubleSpinBox*>(QStringLiteral("DespikeSpin"));
    auto* smooth = panel.findChild<QDoubleSpinBox*>(QStringLiteral("SmoothSpin"));
    ASSERT_TRUE(interpolation && despike && smooth);
    EXPECT_EQ(panel.formConfig().processing, terrain::HeightProcessing{});
    ASSERT_TRUE(panel.regenerate());
    EXPECT_GT(scene.terrain().origin.heightM, 550.0);  // The spike touches the origin.

    interpolation->setCurrentIndex(
        interpolation->findData(static_cast<int>(terrain::HeightInterpolation::Bicubic)));
    despike->setValue(10.0);
    smooth->setValue(20.0);
    const terrain::HeightProcessing wanted{.interpolation = terrain::HeightInterpolation::Bicubic,
                                           .despikeRadiusM = 10.0,
                                           .smoothRadiusM = 20.0};
    EXPECT_EQ(panel.formConfig().processing, wanted);
    ASSERT_TRUE(panel.regenerate());
    EXPECT_EQ(scene.terrain().processing, wanted);
    EXPECT_NEAR(scene.terrain().origin.heightM, 500.0, 1e-6);
    EXPECT_NEAR(scene.query().height(5.0, 5.0), 0.0, 1e-6);
    const QString meta = panel.findChild<QLabel*>(QStringLiteral("MetadataLabel"))->text();
    EXPECT_TRUE(meta.contains(QStringLiteral("bicubic, spikes removed within 10 m")))
        << meta.toStdString();

    // Loading a configuration shows its processing; procedural terrain hides the group.
    TerrainConfig other = config;
    other.processing.interpolation = terrain::HeightInterpolation::Nearest;
    panel.setFormConfig(other);
    EXPECT_EQ(interpolation->currentData().toInt(),
              static_cast<int>(terrain::HeightInterpolation::Nearest));
    EXPECT_EQ(despike->value(), 0.0);
    EXPECT_EQ(despike->text(), QStringLiteral("Off"));
    panel.findChild<QComboBox*>(QStringLiteral("SourceCombo"))->setCurrentIndex(0);
    EXPECT_TRUE(box->isHidden());
    EXPECT_EQ(panel.formConfig().processing, terrain::HeightProcessing{});
}

TEST_F(FileTerrainTest, ADamagedElevationFileStopsTheMission) {
    writeDemMission(std::string(64, 'a'));  // Not the file's sha256.
    openWindow();
    auto launched = window_->launchMission("dem-hop");
    ASSERT_FALSE(launched.has_value());
    EXPECT_NE(launched.error().message.find("sha256"), std::string::npos)
        << launched.error().message;
    EXPECT_FALSE(window_->simulation()->missionActive());
    EXPECT_EQ(window_->scene()->terrain().kind, TerrainKind::Procedural);
}

TEST_F(FileTerrainTest, MissingElevationFilesMakeTheMissionUnavailable) {
    writeDemMission();
    fs::remove(homeDir() / "missions" / "dem-hop" / "dem" / "valley.tif");
    missions_->reload();
    const MissionRow* row = missions_->find("dem-hop");
    ASSERT_NE(row, nullptr);
    EXPECT_NE(row->status, MissionStatus::Available);
    EXPECT_NE(row->statusDetail.find("not found"), std::string::npos) << row->statusDetail;
}

}  // namespace
}  // namespace ds::app
