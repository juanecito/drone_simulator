// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Online imagery in the application (M4 step 8) against the mock services of
// tests/support/online_fixtures.hpp and imagery_fixtures.hpp: the Terrain tab's imagery
// source over online elevation, the saved scene, a mission with an imagery provider, an
// imagery provider without its key, and imagery providers in Settings › Data providers
// (FR-TER-O10, FR-TER-O13, FR-NET-11, FR-GEO-02b).

#include "ds/terrain/online_imagery.hpp"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "ds/app/asset_paths.hpp"
#include "ds/app/data_providers_panel.hpp"
#include "ds/app/data_services.hpp"
#include "ds/app/main_window.hpp"
#include "ds/app/missions_controller.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/app/terrain_panel.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/common/json.hpp"

#include "imagery_fixtures.hpp"
#include "online_fixtures.hpp"

namespace ds::app {
namespace {

namespace fs = std::filesystem;

constexpr double kLat = 41.5934;
constexpr double kLon = 1.8376;

class OnlineImageryTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home_.isValid());
        auto storage = openUserStorage(std::nullopt, homeDir());
        ASSERT_TRUE(storage.has_value());
        storage_ = std::move(*storage);
        settings_ = std::make_unique<SettingsController>(*storage_.service);
        fs::create_directories(homeDir() / "providers");
        std::ofstream(homeDir() / "providers" / "mockdem.json") << dem_.providerJson();
        std::ofstream(homeDir() / "providers" / "mockimg.json") << imagery_.providerJson();
        // The same service behind a key.
        common::Json keyed = common::Json::parse(imagery_.providerJson("mockimg-key"));
        keyed["name"] = "Mock Imagery (key)";
        keyed["requiresKey"] = true;
        keyed["apiKey"] = {{"in", "query"}, {"name", "key"}};
        std::ofstream(homeDir() / "providers" / "mockimg-key.json") << keyed.dump(2);
        auto data = DataServices::create({.dataDirectory = homeDir() / "data",
                                          .providerPresets = homeDir() / "providers",
                                          .userStorage = storage_.service.get(),
                                          .keysFromEnvironment = false});
        ASSERT_TRUE(data.has_value()) << data.error().message;
        data_ = std::move(*data);
        data_->setApiKey("mockdem", "secret");
        data_->setConfirmDownload([](QWidget*, const DownloadRequest&) { return true; });
    }

    void TearDown() override {
        window_.reset();
        missions_.reset();
        data_.reset();
        settings_.reset();
    }

    [[nodiscard]] fs::path homeDir() const { return fs::path(home_.path().toStdString()); }

    static TerrainConfig onlineConfig() {
        TerrainConfig c;
        c.kind = TerrainKind::Online;
        c.online.provider = "mockdem";
        c.origin = {kLat, kLon, 0.0};
        c.extent = terrain::Extent{2000.0, 2000.0};
        return c;
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

    // A copy of "first-flight" on the mock elevation, with @p imagery.
    void writeMission(const std::string& imagery) {
        const fs::path dir = homeDir() / "missions" / "photo-hop";
        fs::create_directories(dir);
        std::ifstream in(findAssetDirectory() / "default-missions" / "first-flight.mission.json");
        common::Json m = common::Json::parse(in);
        m["id"] = "photo-hop";
        m["title"] = "Photo hop";
        m["terrain"] = {{"source", "online"},
                        {"elevationProvider", "mockdem"},
                        {"dataset", "TEST30"},
                        {"imageryProvider", imagery},
                        {"origin", {{"geo", {kLat, kLon, 0}}, {"altRef", "AGL"}}},
                        {"extent", {{"widthM", 2000}, {"lengthM", 2000}}}};
        std::ofstream(dir / "photo-hop.mission.json") << m.dump(2);
        if (!missions_) {
            missions_ = std::make_unique<MissionsController>(
                MissionFolders{.assetDir = findAssetDirectory(),
                               .userDir = homeDir() / "missions",
                               .libraryDir = {}},
                storage_.service.get(), data_.get());
        }
        missions_->reload();
    }

    test::MockDemServer dem_;
    test::MockTileServer imagery_;
    QTemporaryDir home_;
    UserStorage storage_;
    std::unique_ptr<SettingsController> settings_;
    std::unique_ptr<DataServices> data_;
    std::unique_ptr<MissionsController> missions_;
    std::unique_ptr<MainWindow> window_;
};

TEST_F(OnlineImageryTest, TerrainTabDrapesProviderImageryOverOnlineElevation) {
    {
        SceneController scene(settings_.get(), findAssetDirectory(), data_.get());
        TerrainPanel panel(scene);
        panel.setFormConfig(onlineConfig());
        auto* source = panel.findChild<QComboBox*>(QStringLiteral("OrthoSourceCombo"));
        ASSERT_NE(source, nullptr);
        EXPECT_EQ(source->currentIndex(), 0);  // Orthophoto files.
        const int i = source->findData(QStringLiteral("mockimg"));
        ASSERT_GT(i, 0);
        source->setCurrentIndex(i);
        auto* layer = panel.findChild<QComboBox*>(QStringLiteral("OrthoLayerCombo"));
        EXPECT_EQ(layer->currentData(), QStringLiteral("photo"));
        EXPECT_TRUE(panel.findChild<QWidget*>(QStringLiteral("OrthoList"))->isHidden());
        const TerrainConfig form = panel.formConfig();
        EXPECT_EQ(form.ortho.provider, "mockimg");
        EXPECT_TRUE(form.ortho.files.empty());
        ASSERT_TRUE(panel.regenerate())
            << panel.findChild<QLabel*>(QStringLiteral("StatusLabel"))->text().toStdString();

        const auto* online =
            dynamic_cast<const terrain::OnlineImagerySource*>(scene.orthophotos().get());
        ASSERT_NE(online, nullptr) << scene.orthoWarning();
        ASSERT_NE(scene.orthoStreamer(), nullptr);
        // FR-TER-O13: the elevation and the imagery are credited.
        EXPECT_NE(scene.attribution().find("Test DEM"), std::string::npos) << scene.attribution();
        EXPECT_NE(scene.attribution().find("Mock imagery provider; Test imagery © nobody"),
                  std::string::npos)
            << scene.attribution();
        const QString info = panel.findChild<QLabel*>(QStringLiteral("OrthoInfoLabel"))->text();
        EXPECT_TRUE(info.contains(QStringLiteral("Mock Imagery · Test photo (XYZ tiles)")))
            << info.toStdString();

        // A tile over the origin: built from the service once its tiles arrive.
        const terrain::OrthoTileKey key{0, 0, 0};
        terrain::OrthoBuild b = online->build(key, 0.0);
        for (int n = 0; n < 500 && !b.complete; ++n) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            b = online->build(key, 0.0);
        }
        ASSERT_TRUE(b.complete);
        ASSERT_NE(b.tile, nullptr);
        EXPECT_GT(imagery_.requestCount(), 0U);
    }
    // Saved with the scene.
    SceneController again(settings_.get(), findAssetDirectory(), data_.get());
    EXPECT_EQ(again.terrain().ortho.provider, "mockimg");
    EXPECT_NE(again.orthophotos(), nullptr);
}

TEST_F(OnlineImageryTest, MissionImageryIsStreamedAndAMissingKeyOnlyDisablesIt) {
    writeMission("mockimg");
    const MissionRow* row = missions_->find("photo-hop");
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->status, MissionStatus::Available) << row->statusDetail;
    window_ = std::make_unique<MainWindow>(settings_.get(), missions_.get(), data_.get());
    window_->show();
    SimulationController& sim = *window_->simulation();
    SceneController& scene = *window_->scene();
    auto launched = window_->launchMission("photo-hop");
    ASSERT_TRUE(launched.has_value()) << launched.error().message;
    // Imagery is streamed while flying, not downloaded before PreFlight (no score effect).
    EXPECT_EQ(imagery_.requestCount(), 0U);
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
    EXPECT_NE(dynamic_cast<const terrain::OnlineImagerySource*>(scene.orthophotos().get()), nullptr)
        << scene.orthoWarning();
    sim.abortMission();
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().result.has_value(); }));
    window_->endMission();

    // A provider behind a key with no key set: the mission flies without imagery.
    writeMission("mockimg-key");
    ASSERT_EQ(missions_->find("photo-hop")->status, MissionStatus::Available);
    launched = window_->launchMission("photo-hop");
    ASSERT_TRUE(launched.has_value()) << launched.error().message;
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
    EXPECT_EQ(scene.orthophotos(), nullptr);
    EXPECT_TRUE(window_->statusBar()->currentMessage().contains(QStringLiteral("API key")))
        << window_->statusBar()->currentMessage().toStdString();
    sim.abortMission();
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().result.has_value(); }));
    window_->endMission();

    // An unknown imagery provider is a mistake in the mission.
    writeMission("nobody");
    EXPECT_EQ(missions_->find("photo-hop")->status, MissionStatus::Invalid);
}

TEST_F(OnlineImageryTest, ImageryProvidersInTheSettings) {
    // The editor makes imagery providers with tile protocols and zoom levels.
    net::ProviderConfig blank;
    blank.key = "my-photos";
    blank.name = "My photos";
    blank.format = "GTiff";
    blank.datasets.push_back(net::DatasetConfig{.id = "a", .name = "A", .resolutionM = 30.0});
    blank.defaultDataset = "a";
    blank.attribution = "Me";
    ProviderDialog dialog(blank, true);
    auto* kind = dialog.findChild<QComboBox*>(QStringLiteral("ProviderKindCombo"));
    kind->setCurrentIndex(1);
    auto* protocol = dialog.findChild<QComboBox*>(QStringLiteral("ProtocolCombo"));
    protocol->setCurrentIndex(protocol->findData(QStringLiteral("wmts")));
    dialog.findChild<QLineEdit*>(QStringLiteral("UrlTemplateEdit"))
        ->setText(QStringLiteral(
            "https://example.org/wmts?Layer={dataset}&TileMatrix={z}&TileRow={y}&TileCol={x}"));
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("DatasetsTable"));
    table->item(0, 2)->setText(QStringLiteral("3-19"));
    EXPECT_TRUE(dialog.validate().isEmpty()) << dialog.validate().toStdString();
    const net::ProviderConfig made = dialog.config();
    EXPECT_TRUE(made.isImagery());
    EXPECT_EQ(made.protocol, "wmts");
    EXPECT_EQ(made.crs, "EPSG:3857");
    EXPECT_EQ(made.datasets[0].minZoom, 3);
    EXPECT_EQ(made.datasets[0].maxZoom, 19);
    table->item(0, 2)->setText(QStringLiteral("deep"));
    EXPECT_FALSE(dialog.validate().isEmpty());

    // Test connection fetches and decodes one tile (FR-NET-11).
    const net::ProviderConfig* p = data_->provider("mockimg");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(data_->imageryProviders().size(), 2U);
    std::optional<ConnectionTest> result;
    data_->testConnection(*p, "", [&](const ConnectionTest& t) { result = t; });
    ASSERT_TRUE(QTest::qWaitFor([&] { return result.has_value(); }, 10000));
    EXPECT_TRUE(result->ok) << result->message;
    EXPECT_NE(result->message.find("256 × 256 px"), std::string::npos) << result->message;
}

}  // namespace
}  // namespace ds::app
