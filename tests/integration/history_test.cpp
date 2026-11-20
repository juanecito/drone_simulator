// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// History tab, telemetry recording and replays (M7 step 7: FR-X-05, FR-X-06, FR-DB-07): a
// keyboard flight with a failure and a wind change is saved with its telemetry, listed,
// plotted and replayed exactly. Runs on the offscreen platform; the loop runs synchronously.

#include <QComboBox>
#include <QCoreApplication>
#include <QImage>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "ds/app/asset_paths.hpp"
#include "ds/app/history_panel.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/app/telemetry_plot.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/input/keyboard.hpp"
#include "ds/storage/recording_store.hpp"
#include "ds/storage/session_store.hpp"
#include "ds/storage/storage_service.hpp"

namespace {

using namespace ds;
using app::HistoryPanel;
using app::SimulationController;
namespace fs = std::filesystem;

constexpr int kTickRate = 500;

void saveImage(QWidget& widget, const char* name) {
    if (const char* dir = std::getenv("DS_TEST_IMAGE_DIR")) {
        widget.grab().save(QString::fromLocal8Bit(dir) + QLatin1Char('/') +
                           QString::fromLatin1(name));
    }
}

class HistoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home_.isValid());
        auto storage = app::openUserStorage(std::nullopt, fs::path(home_.path().toStdString()));
        ASSERT_TRUE(storage.has_value());
        storage_ = std::move(*storage);
        settings_ = std::make_unique<app::SettingsController>(*storage_.service);
        scene_ = std::make_unique<app::SceneController>(settings_.get(), app::findAssetDirectory());
        sim_ = std::make_unique<SimulationController>(*scene_, settings_.get(),
                                                      app::findAssetDirectory(), false);
    }

    void TearDown() override {
        sim_.reset();
        scene_.reset();
        settings_.reset();
    }

    void run(double seconds) {
        sim_->loop().runTicks(static_cast<int>(seconds * kTickRate));
        sim_->updateFrame(0.016, 16.0 / 9.0);
    }

    // Waits for the storage thread, then lets the controller and the panel see the results.
    void settle(const HistoryPanel* panel = nullptr) {
        for (int i = 0; i < 500; ++i) {
            (void)storage_.service->submit([](storage::DbConnection&) { return 0; }).get();
            sim_->updateFrame(0.0, 16.0 / 9.0);
            QCoreApplication::processEvents();
            if (panel == nullptr || !panel->busy()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL() << "the history panel never finished loading";
    }

    QTemporaryDir home_;
    app::UserStorage storage_;
    std::unique_ptr<app::SettingsController> settings_;
    std::unique_ptr<app::SceneController> scene_;
    std::unique_ptr<SimulationController> sim_;
};

TEST_F(HistoryTest, FlightsAreRecordedListedPlottedAndReplayedExactly) {
    const app::TerrainConfig terrain = scene_->terrain();
    const glm::dvec3 home = sim_->snapshot().body.position;
    run(0.3);
    sim_->keyboardAction(input::KeyboardAction::ToggleArm);
    run(0.2);
    ASSERT_TRUE(sim_->snapshot().telemetry.armed);
    sim_->setFlightKeys(static_cast<std::uint32_t>(input::FlightKey::ThrottleUp) |
                        static_cast<std::uint32_t>(input::FlightKey::PitchForward));
    run(1.6);
    sim_->setFlightKeys(static_cast<std::uint32_t>(input::FlightKey::RollRight));
    physics::Failure gps = physics::defaultFailure(physics::FailureType::GpsDrift);
    gps.durationS = 1.0;
    sim_->injectFailure(gps);
    app::EnvironmentConfig env = scene_->environment();
    env.wind.level = 3;
    scene_->setEnvironment(env);
    run(1.5);
    sim_->setFlightKeys(0);
    run(1.0);
    const sim::WorldSnapshot live = sim_->snapshot();
    EXPECT_GT(live.aglM, 2.0);

    // The reset ends the session: it is saved with its telemetry and replay.
    qint64 recorded = -1;
    QObject::connect(sim_.get(), &SimulationController::sessionRecorded,
                     [&recorded](qint64 id) { recorded = id; });
    sim_->reset();
    run(0.1);
    settle();
    ASSERT_GT(recorded, 0);

    HistoryPanel panel(storage_.service.get(), *sim_);
    panel.resize(700, 900);
    settle(&panel);
    ASSERT_EQ(panel.sessions().size(), 1U);
    const storage::SessionRecord& session = panel.sessions().front();
    EXPECT_EQ(session.id, recorded);
    EXPECT_TRUE(session.hasReplay);
    EXPECT_TRUE(session.missionKey.empty());
    EXPECT_GT(session.maxAltitudeAglM, 2.0);
    auto* table = panel.findChild<QTableWidget*>(QStringLiteral("SessionsTable"));
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->rowCount(), 1);
    EXPECT_EQ(table->item(0, 2)->text(), QStringLiteral("free flight"));
    EXPECT_EQ(table->item(0, 9)->text(), QStringLiteral("yes"));

    // Selecting it plots its 20 Hz telemetry.
    auto* replayButton = panel.findChild<QPushButton*>(QStringLiteral("ReplayButton"));
    auto* stopButton = panel.findChild<QPushButton*>(QStringLiteral("StopReplayButton"));
    ASSERT_NE(replayButton, nullptr);
    ASSERT_NE(stopButton, nullptr);
    EXPECT_FALSE(replayButton->isEnabled());
    ASSERT_TRUE(panel.selectSession(recorded));
    settle(&panel);
    auto* plot = panel.findChild<app::TelemetryPlot*>(QStringLiteral("TelemetryPlot"));
    ASSERT_NE(plot, nullptr);
    EXPECT_NEAR(static_cast<double>(plot->sampleCount()), session.durationS * 20.0, 2.0);
    EXPECT_TRUE(replayButton->isEnabled());
    EXPECT_FALSE(stopButton->isEnabled());
    saveImage(panel, "history_panel.png");
    auto* quantity = panel.findChild<QComboBox*>(QStringLiteral("PlotQuantityCombo"));
    ASSERT_NE(quantity, nullptr);
    quantity->setCurrentIndex(static_cast<int>(app::PlotQuantity::Track));
    EXPECT_EQ(plot->quantity(), app::PlotQuantity::Track);
    saveImage(*plot, "history_track.png");

    // Replay: the same flight, tick for tick.
    std::optional<storage::ReplayRecord> requested;
    QObject::connect(&panel, &HistoryPanel::replayRequested,
                     [&requested](const storage::ReplayRecord& r) { requested = r; });
    replayButton->click();
    settle(&panel);
    ASSERT_TRUE(requested.has_value());
    EXPECT_EQ(requested->sessionId, recorded);
    // Paused first: startReplay() resumes it instead of starting the loop thread, and the
    // test keeps ticking synchronously.
    sim_->loop().step();
    ASSERT_TRUE(sim_->startReplay(*requested).has_value());
    QCoreApplication::processEvents();
    EXPECT_TRUE(sim_->replayActive());
    EXPECT_TRUE(stopButton->isEnabled());
    EXPECT_FALSE(sim_->selectVehicleType(physics::VehicleType::Ground).has_value());
    sim_->loop().runTicks(static_cast<int>(requested->ticks));
    sim_->updateFrame(0.016, 16.0 / 9.0);
    const sim::WorldSnapshot& replayed = sim_->snapshot();
    EXPECT_TRUE(replayed.replay.active);
    EXPECT_FALSE(replayed.replay.finished);
    EXPECT_NEAR(replayed.replay.timeS, requested->durationS, 1e-9);
    EXPECT_EQ(replayed.body.position, live.body.position);
    EXPECT_EQ(replayed.body.orientation, live.body.orientation);
    EXPECT_EQ(replayed.stats.distanceM, live.stats.distanceM);
    EXPECT_EQ(sim_->hud().replay.text.rfind("REPLAY ", 0), 0U);
    sim_->loop().runTicks(10);  // Past the end the world holds still.
    sim_->updateFrame(0.016, 16.0 / 9.0);
    EXPECT_TRUE(sim_->snapshot().replay.finished);
    EXPECT_EQ(sim_->snapshot().body.position, live.body.position);

    // Stop: free flight comes back on the same terrain; the replay was not recorded.
    stopButton->click();
    run(0.1);
    EXPECT_FALSE(sim_->replayActive());
    EXPECT_FALSE(sim_->snapshot().replay.active);
    EXPECT_EQ(scene_->terrain(), terrain);
    EXPECT_NEAR(sim_->snapshot().body.position.x, home.x, 0.01);
    EXPECT_NEAR(sim_->snapshot().body.position.z, home.z, 0.01);
    settle();
    const auto sessions =
        storage_.service
            ->submit([](storage::DbConnection& c) { return storage::SessionStore(c).recent(); })
            .get();
    ASSERT_TRUE(sessions.has_value());
    EXPECT_EQ(sessions->size(), 1U);

    // Delete: the session goes with its telemetry and replay.
    panel.removeSession(recorded);
    settle(&panel);
    EXPECT_TRUE(panel.sessions().empty());
    const auto replay = storage_.service
                            ->submit([recorded](storage::DbConnection& c) {
                                return storage::RecordingStore(c).replay(recorded);
                            })
                            .get();
    ASSERT_TRUE(replay.has_value());
    EXPECT_FALSE(replay->has_value());
}

TEST_F(HistoryTest, NoDatabaseNoHistory) {
    HistoryPanel panel(nullptr, *sim_);
    EXPECT_FALSE(panel.busy());
    EXPECT_EQ(panel.findChild<QTableWidget*>(QStringLiteral("SessionsTable")), nullptr);
}

TEST(TelemetryPlot, NiceTicks) {
    EXPECT_EQ(app::niceTicks(0.0, 10.0, 5), (std::vector<double>{0, 2, 4, 6, 8, 10}));
    EXPECT_EQ(app::niceTicks(-3.0, 7.0, 5), (std::vector<double>{-2, 0, 2, 4, 6}));
    EXPECT_EQ(app::niceTicks(-3.0, 7.0, 4), (std::vector<double>{0, 5}));
    EXPECT_TRUE(app::niceTicks(1.0, 1.0).empty());
    const auto small = app::niceTicks(7.9, 8.4, 5);
    ASSERT_FALSE(small.empty());
    EXPECT_NEAR(small[1] - small[0], 0.1, 1e-12);
}

}  // namespace
