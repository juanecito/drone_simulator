// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Scene adapter, Terrain tab and Environment tab (FR-TER-04a, FR-TER-09, FR-TER-10, FR-UI-06,
// FR-AST-07). Runs on the offscreen platform; nothing here needs OpenGL.

#include <QComboBox>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTabWidget>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

#include "ds/app/asset_paths.hpp"
#include "ds/app/environment_panel.hpp"
#include "ds/app/main_window.hpp"
#include "ds/app/scene_config.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/terrain_panel.hpp"
#include "ds/app/user_storage.hpp"

namespace {

using namespace ds;
using app::EnvironmentPanel;
using app::SceneController;
using app::SettingsController;
using app::TerrainConfig;
using app::TerrainPanel;
namespace fs = std::filesystem;

template <typename T>
T* child(QWidget& parent, const char* name) {
    T* widget = parent.findChild<T*>(QLatin1StringView(name));
    EXPECT_NE(widget, nullptr) << name;
    return widget;
}

void saveImage(QWidget& widget, const char* name) {
    if (const char* dir = std::getenv("DS_TEST_IMAGE_DIR")) {
        widget.grab().save(QString::fromUtf8(dir) + QLatin1Char('/') + QLatin1StringView(name));
    }
}

class ScenePanelsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home_.isValid());
        auto storage = app::openUserStorage(std::nullopt, fs::path(home_.path().toStdString()));
        ASSERT_TRUE(storage.has_value());
        storage_ = std::move(*storage);
        settings_ = std::make_unique<SettingsController>(*storage_.service);
        scene_ = std::make_unique<SceneController>(settings_.get(), app::findAssetDirectory());
    }

    QTemporaryDir home_;
    app::UserStorage storage_;
    std::unique_ptr<SettingsController> settings_;
    std::unique_ptr<SceneController> scene_;
};

TEST(SceneConfig, TerrainJsonRoundTrip) {
    TerrainConfig config = app::builtInPresets().at(3).config;  // Island: bounded, water.
    config.params.seed = 18'446'744'073'709'551'557ULL;         // Needs all 64 bits.
    config.params.flatZones.push_back({.x = 10.0, .z = -20.0, .radiusM = 30.0, .blendM = 15.0});
    const auto back = app::terrainFromJson(app::terrainToJson(config));
    ASSERT_TRUE(back.has_value()) << back.error().message;
    EXPECT_EQ(*back, config);
}

TEST(SceneConfig, RejectsInvalidTerrain) {
    QJsonObject json = app::terrainToJson(app::builtInPresets().front().config);
    QJsonObject params = json.value(QStringLiteral("params")).toObject();
    params.insert(QStringLiteral("persistence"), 3.0);
    json.insert(QStringLiteral("params"), params);
    EXPECT_FALSE(app::terrainFromJson(json).has_value());
    json = app::terrainToJson(app::builtInPresets().front().config);
    json.insert(QStringLiteral("extent"),
                QJsonObject{{QStringLiteral("widthM"), 10.0}, {QStringLiteral("lengthM"), 1000.0}});
    EXPECT_FALSE(app::terrainFromJson(json).has_value()) << "below the 256 m minimum";
    json = app::terrainToJson(app::builtInPresets().front().config);
    json.insert(QStringLiteral("source"), QStringLiteral("online"));
    EXPECT_FALSE(app::terrainFromJson(json).has_value());
}

TEST(SceneConfig, EnvironmentJsonRoundTripAndClamping) {
    app::EnvironmentConfig env;
    env.time = *astro::parseUtc("2026-08-12T21:40:00Z");
    env.timeZone = "Europe/Madrid";
    env.animate = true;
    env.timeRate = 60.0;
    env.exposure = {
        .mode = graphics::ExposureMode::Manual, .manualEv100 = -3.0, .compensationEv = 1.0};
    env.visibilityM = 800.0;
    EXPECT_EQ(app::environmentFromJson(app::environmentToJson(env), {}), env);

    QJsonObject bad = app::environmentToJson(env);
    bad.insert(QStringLiteral("visibilityM"), 1.0);
    bad.insert(QStringLiteral("timeZone"), QStringLiteral("Mars/Olympus_Mons"));
    const auto read = app::environmentFromJson(bad, {});
    EXPECT_EQ(read.visibilityM, app::kMinVisibilityM);
    EXPECT_EQ(read.timeZone, "UTC");
}

TEST(SceneConfig, BuiltInPresetsAreValid) {
    for (const auto& preset : app::builtInPresets()) {
        EXPECT_TRUE(preset.key.starts_with("builtin."));
        EXPECT_TRUE(terrain::validate(preset.config.params).has_value()) << preset.key;
        const auto back = app::terrainFromJson(app::terrainToJson(preset.config));
        EXPECT_TRUE(back.has_value()) << preset.key;
    }
}

TEST_F(ScenePanelsTest, TerrainAndEnvironmentPersistAcrossRestart) {
    TerrainConfig config = app::builtInPresets().at(1).config;
    config.params.seed = 99;
    ASSERT_TRUE(scene_->setTerrain(config).has_value());
    app::EnvironmentConfig env = scene_->environment();
    env.time = *astro::parseUtc("2026-03-20T06:00:00Z");
    env.visibilityM = 5000.0;
    scene_->setEnvironment(env);
    scene_.reset();
    settings_->flush();

    SceneController again(settings_.get(), app::findAssetDirectory());
    EXPECT_EQ(again.terrain(), config);
    EXPECT_EQ(again.environment().time, env.time);
    EXPECT_DOUBLE_EQ(again.environment().visibilityM, 5000.0);
}

TEST_F(ScenePanelsTest, CameraStaysAboveTerrainAndInsideExtent) {
    scene_->camera().position = {30'000.0, -500.0, 30'000.0};
    TerrainConfig island = app::builtInPresets().at(3).config;
    ASSERT_TRUE(scene_->setTerrain(island).has_value());
    const auto& pos = scene_->camera().position;
    EXPECT_LE(std::abs(pos.x), 5000.0);
    EXPECT_LE(std::abs(pos.z), 5000.0);
    EXPECT_GT(pos.y, scene_->query().height(pos.x, pos.z));
}

TEST_F(ScenePanelsTest, AnimatedTimeAdvances) {
    app::EnvironmentConfig env = scene_->environment();
    env.time = *astro::parseUtc("2026-06-21T12:00:00Z");
    env.animate = true;
    env.timeRate = 600.0;
    scene_->setEnvironment(env);
    scene_->advance(1.0);
    EXPECT_EQ(scene_->environment().time - env.time, std::chrono::minutes(10));
}

TEST_F(ScenePanelsTest, TerrainPanelRegeneratesAndReportsErrors) {
    TerrainPanel panel(*scene_);
    panel.resize(400, 900);
    QSignalSpy changed(scene_.get(), &SceneController::terrainChanged);

    child<QLineEdit>(panel, "SeedEdit")->setText(QStringLiteral("12345"));
    child<QComboBox>(panel, "DimensionsCombo")->setCurrentIndex(1);
    child<QDoubleSpinBox>(panel, "WidthSpin")->setValue(4.0);
    child<QDoubleSpinBox>(panel, "LengthSpin")->setValue(6.0);
    child<QPushButton>(panel, "RegenerateButton")->click();
    ASSERT_EQ(changed.size(), 1);
    EXPECT_EQ(scene_->terrain().params.seed, 12345U);
    ASSERT_TRUE(scene_->terrain().extent.has_value());
    EXPECT_DOUBLE_EQ(scene_->terrain().extent->widthM, 4000.0);
    EXPECT_DOUBLE_EQ(scene_->terrain().extent->lengthM, 6000.0);
    EXPECT_TRUE(scene_->terrain().presetKey.empty()) << "edited terrain is no longer the preset";
    EXPECT_EQ(child<QComboBox>(panel, "PresetCombo")->currentIndex(), 0);
    saveImage(panel, "terrain_panel.png");

    // Online and file sources are not selectable before M4.
    auto* source = child<QComboBox>(panel, "SourceCombo");
    EXPECT_EQ(source->count(), 3);
    EXPECT_FALSE(source->model()->flags(source->model()->index(1, 0)).testFlag(Qt::ItemIsEnabled));

    // Random seed changes the seed text.
    const QString before = child<QLineEdit>(panel, "SeedEdit")->text();
    child<QPushButton>(panel, "RandomSeedButton")->click();
    EXPECT_NE(child<QLineEdit>(panel, "SeedEdit")->text(), before);

    // Out-of-range values cannot be entered: the form clamps them to valid ranges.
    TerrainConfig bad = panel.formConfig();
    bad.params.lacunarity = 9.0;
    panel.setFormConfig(bad);
    EXPECT_LE(panel.formConfig().params.lacunarity, 4.0);
    EXPECT_TRUE(terrain::validate(panel.formConfig().params).has_value());
}

TEST_F(ScenePanelsTest, TerrainPresetsCanBeSavedLoadedAndDeleted) {
    TerrainPanel panel(*scene_);
    auto* presets = child<QComboBox>(panel, "PresetCombo");
    const int builtIns = static_cast<int>(app::builtInPresets().size());
    EXPECT_EQ(presets->count(), builtIns + 1);  // Plus "(custom)".

    TerrainConfig config = scene_->terrain();
    config.params.seed = 4242;
    auto key = scene_->savePreset(QStringLiteral("My Hills!"), config);
    ASSERT_TRUE(key.has_value()) << key.error().message;
    EXPECT_EQ(*key, QStringLiteral("user.my-hills"));
    EXPECT_EQ(presets->count(), builtIns + 2);
    auto loaded = scene_->presetConfig(*key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->params.seed, 4242U);

    // Selecting a built-in preset loads it into the form and regenerates.
    const int alpine = presets->findData(QStringLiteral("builtin.alpine"));
    ASSERT_GE(alpine, 0);
    presets->setCurrentIndex(alpine);
    emit presets->activated(alpine);
    EXPECT_EQ(scene_->terrain().presetKey, "builtin.alpine");
    EXPECT_DOUBLE_EQ(scene_->terrain().params.amplitudeM, 1200.0);

    EXPECT_FALSE(scene_->deletePreset(QStringLiteral("builtin.alpine")).has_value());
    ASSERT_TRUE(scene_->deletePreset(*key).has_value());
    EXPECT_EQ(presets->count(), builtIns + 1);
}

TEST_F(ScenePanelsTest, TerrainPreviewShowsWater) {
    const TerrainConfig island = app::builtInPresets().at(3).config;
    const QImage image = TerrainPanel::renderPreview(island, 64);
    ASSERT_EQ(image.size(), QSize(64, 64));
    const QColor corner = image.pixelColor(1, 1);  // The sea around the island.
    EXPECT_GT(corner.blue(), corner.red());
    EXPECT_GT(corner.blue(), corner.green());
}

TEST_F(ScenePanelsTest, EnvironmentPanelEditsTime) {
    app::EnvironmentConfig env = scene_->environment();
    env.timeZone = "Europe/Madrid";
    env.time = *astro::parseUtc("2026-06-21T10:00:00Z");  // 12:00 CEST.
    scene_->setEnvironment(env);
    EnvironmentPanel panel(*scene_);
    panel.resize(420, 900);

    auto* slider = child<QSlider>(panel, "TimeSlider");
    EXPECT_EQ(slider->value(), 12 * 60);
    slider->setValue(21 * 60 + 30);  // 21:30 CEST = 19:30 UTC.
    EXPECT_EQ(scene_->environment().time, *astro::parseUtc("2026-06-21T19:30:00Z"));

    auto* edit = child<QDateTimeEdit>(panel, "DateTimeEdit");
    EXPECT_EQ(edit->dateTime().time(), QTime(21, 30));

    // Changing the zone keeps the instant and shows it in the new zone.
    auto* zone = child<QComboBox>(panel, "TimeZoneCombo");
    zone->setCurrentText(QStringLiteral("UTC"));
    emit zone->activated(zone->currentIndex());
    EXPECT_EQ(scene_->environment().timeZone, "UTC");
    EXPECT_EQ(scene_->environment().time, *astro::parseUtc("2026-06-21T19:30:00Z"));
    EXPECT_EQ(edit->dateTime().time(), QTime(19, 30));

    // Read-outs: Madrid area at the June solstice has sunrise and sunset.
    const QString events = child<QLabel>(panel, "EventsLabel")->text();
    EXPECT_TRUE(events.contains(QStringLiteral("Sunrise")));
    EXPECT_FALSE(events.contains(QStringLiteral("polar")));
    EXPECT_FALSE(child<QLabel>(panel, "SunLabel")->text().isEmpty());

    // Now button jumps to the current time.
    child<QPushButton>(panel, "NowButton")->click();
    const auto now = std::chrono::system_clock::now();
    EXPECT_LT(std::chrono::abs(now - scene_->environment().time), std::chrono::minutes(1));
    saveImage(panel, "environment_panel.png");
}

TEST_F(ScenePanelsTest, EnvironmentPanelEditsExposureAndVisibility) {
    EnvironmentPanel panel(*scene_);
    auto* mode = child<QComboBox>(panel, "ExposureModeCombo");
    auto* manual = child<QDoubleSpinBox>(panel, "ManualEvSpin");
    EXPECT_FALSE(manual->isEnabled());
    mode->setCurrentIndex(1);
    emit mode->activated(1);
    EXPECT_EQ(scene_->environment().exposure.mode, graphics::ExposureMode::Manual);
    EXPECT_TRUE(manual->isEnabled());
    manual->setValue(-4.0);
    EXPECT_DOUBLE_EQ(scene_->environment().exposure.manualEv100, -4.0);
    child<QDoubleSpinBox>(panel, "VisibilitySpin")->setValue(0.8);
    EXPECT_DOUBLE_EQ(scene_->environment().visibilityM, 800.0);
}

TEST(EnvironmentPanelText, PhaseNames) {
    EXPECT_EQ(EnvironmentPanel::phaseName(0.0), QStringLiteral("New moon"));
    EXPECT_EQ(EnvironmentPanel::phaseName(45.0), QStringLiteral("Waxing crescent"));
    EXPECT_EQ(EnvironmentPanel::phaseName(90.0), QStringLiteral("First quarter"));
    EXPECT_EQ(EnvironmentPanel::phaseName(180.0), QStringLiteral("Full moon"));
    EXPECT_EQ(EnvironmentPanel::phaseName(270.0), QStringLiteral("Last quarter"));
    EXPECT_EQ(EnvironmentPanel::phaseName(330.0), QStringLiteral("Waning crescent"));
}

TEST(MainWindowTabs, TerrainAndEnvironmentTabsArePanels) {
    app::MainWindow window;
    QTabWidget* tabs = window.tabs();
    bool terrain = false;
    bool environment = false;
    for (int i = 0; i < tabs->count(); ++i) {
        terrain |= qobject_cast<TerrainPanel*>(tabs->widget(i)) != nullptr;
        environment |= qobject_cast<EnvironmentPanel*>(tabs->widget(i)) != nullptr;
    }
    EXPECT_TRUE(terrain);
    EXPECT_TRUE(environment);
    EXPECT_NE(window.scene(), nullptr);
}

}  // namespace
