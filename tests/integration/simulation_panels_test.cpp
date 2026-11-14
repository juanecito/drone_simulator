// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Simulation adapter, Simulation tab, Vehicle tab and Settings › Controls (FR-LOOP-03/04a,
// FR-PHY-15, FR-IN-05, FR-WIND-01, FR-GFX-05, FR-GFX-14). Runs on the offscreen platform; the
// loop runs synchronously (no thread) unless a test starts it.

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonObject>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

#include "ds/app/asset_paths.hpp"
#include "ds/app/controls_panel.hpp"
#include "ds/app/environment_panel.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/app/simulation_panel.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/app/vehicle_panel.hpp"
#include "ds/input/keyboard.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/storage/session_store.hpp"
#include "ds/storage/storage_service.hpp"
#include "ds/storage/vehicle_profile_store.hpp"

namespace {

using namespace ds;
using app::SimulationController;
namespace fs = std::filesystem;

constexpr int kTickRate = 500;  // Default tick rate.

class SimulationPanelsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home_.isValid());
        auto storage = app::openUserStorage(std::nullopt, fs::path(home_.path().toStdString()));
        ASSERT_TRUE(storage.has_value());
        storage_ = std::move(*storage);
        settings_ = std::make_unique<app::SettingsController>(*storage_.service);
        scene_ = std::make_unique<app::SceneController>(settings_.get(), app::findAssetDirectory());
        sim_ = makeSimulation();
    }

    void TearDown() override {
        sim_.reset();
        scene_.reset();
        settings_.reset();
    }

    std::unique_ptr<SimulationController> makeSimulation() {
        return std::make_unique<SimulationController>(*scene_, settings_.get(),
                                                      app::findAssetDirectory(), false);
    }

    // Runs @p seconds of simulated time synchronously, then refreshes the frame.
    void run(double seconds) {
        sim_->loop().runTicks(static_cast<int>(seconds * kTickRate));
        sim_->updateFrame(0.016, 16.0 / 9.0);
    }

    double groundUnderVehicle() const {
        const auto& p = sim_->snapshot().body.position;
        return scene_->query().height(p.x, p.z);
    }

    QTemporaryDir home_;
    app::UserStorage storage_;
    std::unique_ptr<app::SettingsController> settings_;
    std::unique_ptr<app::SceneController> scene_;
    std::unique_ptr<SimulationController> sim_;
};

TEST_F(SimulationPanelsTest, BuiltInProfilesAreStoredAndSelectable) {
    QStringList builtIn;
    for (const auto& p : sim_->profiles()) {
        if (p.builtIn) {
            builtIn << p.key;
        }
    }
    EXPECT_EQ(builtIn, (QStringList{"camera-250g", "fpv-5in-racer", "survey-1500g"}));
    EXPECT_EQ(sim_->profile().key, SimulationController::kDefaultProfile);

    // FR-DB-09: the shipped profiles are in the user's database.
    auto rows = storage_.service
                    ->submit([](storage::DbConnection& c) {
                        return storage::VehicleProfileStore(c).list("aerial");
                    })
                    .get();
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 3U);

    ASSERT_TRUE(sim_->selectProfile(QStringLiteral("fpv-5in-racer")).has_value());
    sim_->updateFrame(0.016, 1.5);
    EXPECT_EQ(sim_->snapshot().profileKey, "fpv-5in-racer");
    EXPECT_FALSE(sim_->selectProfile(QStringLiteral("no-such-profile")).has_value());

    // The choice is remembered.
    settings_->flush();
    sim_.reset();
    sim_ = makeSimulation();
    EXPECT_EQ(sim_->profile().key, "fpv-5in-racer");
}

TEST_F(SimulationPanelsTest, KeyboardFlightTakesOffAndIsRecorded) {
    run(0.5);
    const double start = sim_->snapshot().body.position.y;
    EXPECT_NEAR(sim_->snapshot().aglM, 0.0, 0.05);  // Resting on the terrain.
    EXPECT_NEAR(start - groundUnderVehicle(), 0.0, 0.5);

    sim_->keyboardAction(input::KeyboardAction::ToggleArm);
    run(0.2);
    EXPECT_TRUE(sim_->snapshot().telemetry.armed);
    sim_->setFlightKeys(static_cast<std::uint32_t>(input::FlightKey::ThrottleUp));
    run(1.6);  // Sticky throttle: ~0.8 when released.
    sim_->setFlightKeys(0);
    run(2.0);
    EXPECT_GT(sim_->snapshot().body.position.y, start + 2.0);
    EXPECT_GT(sim_->snapshot().stats.flightTimeS, 3.0);
    EXPECT_GT(sim_->snapshot().controls.throttle, 0.5);

    // Reset: back on the ground, disarmed; the flight goes to the sessions table.
    sim_->reset();
    run(0.1);
    EXPECT_FALSE(sim_->snapshot().telemetry.armed);
    EXPECT_NEAR(sim_->snapshot().body.position.y, start, 0.1);
    auto sessions =
        storage_.service
            ->submit([](storage::DbConnection& c) { return storage::SessionStore(c).recent(10); })
            .get();
    ASSERT_TRUE(sessions.has_value());
    ASSERT_EQ(sessions->size(), 1U);
    EXPECT_EQ(sessions->front().vehicleProfileKey, SimulationController::kDefaultProfile);
    EXPECT_GT(sessions->front().maxAltitudeAglM, 2.0);
    EXPECT_GT(sessions->front().durationS, 3.0);
}

TEST_F(SimulationPanelsTest, EnvironmentChangesReachTheWorld) {
    app::EnvironmentConfig env = scene_->environment();
    env.wind.level = 4;
    env.wind.fromDeg = 90.0;
    env.air.temperatureC = 32.0;
    scene_->setEnvironment(env);
    run(0.1);
    EXPECT_EQ(sim_->snapshot().windLevel, 4);
    EXPECT_GT(sim_->snapshot().windMeanSpeedMs, 0.0);
    EXPECT_GT(sim_->snapshot().windGustMs, sim_->snapshot().windMeanSpeedMs);

    // The setting is saved with the environment.
    const auto back = app::environmentFromJson(app::environmentToJson(scene_->environment()), {});
    EXPECT_EQ(back.wind.level, 4);
    EXPECT_DOUBLE_EQ(back.air.temperatureC, 32.0);

    // Out-of-range values from a damaged settings row are clamped.
    QJsonObject bad = app::environmentToJson(env);
    bad[QStringLiteral("windLevel")] = 42;
    bad[QStringLiteral("qnhHpa")] = 5.0;
    bad[QStringLiteral("windFromDeg")] = -90.0;
    const auto clamped = app::environmentFromJson(bad, {});
    EXPECT_EQ(clamped.wind.level, 10);
    EXPECT_DOUBLE_EQ(clamped.air.pressureHpa, app::kMinQnhHpa);
    EXPECT_DOUBLE_EQ(clamped.wind.fromDeg, 270.0);
}

TEST_F(SimulationPanelsTest, EditedProfilesCanBeSavedAndDeleted) {
    physics::AerialProfile p = sim_->profile();
    p.frame.massKg *= 1.2;
    const double heavy = p.frame.massKg;
    ASSERT_TRUE(sim_->applyProfile(p).has_value());
    EXPECT_TRUE(sim_->profileModified());
    EXPECT_EQ(sim_->profile().key, SimulationController::kDefaultProfile);

    p.frame.massKg = -1.0;  // Invalid: refused, nothing changes.
    EXPECT_FALSE(sim_->applyProfile(p).has_value());

    auto key = sim_->saveProfileAs(QStringLiteral("Heavy Camera"));
    ASSERT_TRUE(key.has_value()) << key.error().message;
    EXPECT_EQ(*key, QStringLiteral("user.heavy-camera"));
    EXPECT_FALSE(sim_->profileModified());
    EXPECT_EQ(sim_->profile().name, "Heavy Camera");
    EXPECT_FALSE(sim_->profile().reference.has_value());

    // A new controller lists and loads it.
    settings_->flush();
    sim_.reset();
    sim_ = makeSimulation();
    EXPECT_EQ(sim_->profile().key, "user.heavy-camera");
    auto loaded = sim_->profileFor(*key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_NEAR(loaded->frame.massKg, heavy, 1e-9);

    EXPECT_FALSE(sim_->deleteProfile(QStringLiteral("camera-250g")).has_value());
    ASSERT_TRUE(sim_->deleteProfile(*key).has_value());
    EXPECT_EQ(sim_->profile().key, SimulationController::kDefaultProfile);
    for (const auto& info : sim_->profiles()) {
        EXPECT_NE(info.key, *key);
    }
}

TEST_F(SimulationPanelsTest, CamerasDrawItemsAndHud) {
    run(0.1);
    EXPECT_FALSE(sim_->vehicleView());  // The free camera until the simulation starts.

    sim_->setCameraMode(graphics::CameraMode::Chase);
    run(0.1);
    EXPECT_GE(sim_->drawItems().size(), 8U);  // Frame, propellers, lights.
    const glm::dvec3 drone = sim_->vehiclePosition();
    const graphics::Camera& chase = sim_->vehicleCamera();
    const double expected = std::clamp(14.0 * sim_->profile().frame.armLengthM, 1.5, 6.0);
    EXPECT_NEAR(glm::distance(chase.position, drone), std::hypot(expected, 0.35 * expected), 0.3);
    EXPECT_GT(chase.position.y, drone.y);
    EXPECT_EQ(sim_->hud().positionUtm.rfind("UTM", 0), 0U) << sim_->hud().positionUtm;
    EXPECT_EQ(sim_->hud().armed.text, "DISARMED");
    EXPECT_EQ(sim_->hud().lights.text, "LIGHTS OFF");

    sim_->keyboardAction(input::KeyboardAction::ToggleLights);
    run(0.1);
    EXPECT_EQ(sim_->hud().lights.text, "LIGHTS ON");

    sim_->setCameraMode(graphics::CameraMode::Fpv);
    run(0.01);
    EXPECT_TRUE(sim_->drawItems().empty());  // Not drawn from its own camera.
    EXPECT_LT(glm::distance(sim_->vehicleCamera().position, sim_->vehiclePosition()), 0.2);

    sim_->setCameraMode(graphics::CameraMode::LineOfSight);
    run(0.01);
    EXPECT_FALSE(sim_->hud().pilotDistance.text.empty());
    sim_->cycleCameraMode();
    EXPECT_EQ(sim_->cameraMode(), graphics::CameraMode::Free);

    // The camera mode is remembered.
    sim_->setCameraMode(graphics::CameraMode::Chase);
    settings_->flush();
    sim_.reset();
    sim_ = makeSimulation();
    EXPECT_EQ(sim_->cameraMode(), graphics::CameraMode::Chase);
}

TEST_F(SimulationPanelsTest, RunningLoopDrivesTheSky) {
    EXPECT_FALSE(scene_->timeFollowsSimulation());
    sim_->start();
    EXPECT_EQ(sim_->cameraMode(), graphics::CameraMode::Chase);  // Leaves the free camera.
    for (int i = 0; i < 50 && sim_->state() != sim::RunState::Running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        sim_->updateFrame(0.01, 1.0);
    }
    EXPECT_EQ(sim_->state(), sim::RunState::Running);
    EXPECT_TRUE(scene_->timeFollowsSimulation());
    const auto t0 = sim_->snapshot().simTimeS;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sim_->updateFrame(0.2, 1.0);
    EXPECT_GT(sim_->snapshot().simTimeS, t0 + 0.05);
    EXPECT_EQ(scene_->environment().time, sim_->snapshot().utc);

    sim_->pause();
    for (int i = 0; i < 50 && sim_->state() != sim::RunState::Paused; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        sim_->updateFrame(0.01, 1.0);
    }
    EXPECT_EQ(sim_->state(), sim::RunState::Paused);
}

TEST_F(SimulationPanelsTest, PanelsEditTheSimulation) {
    app::VehiclePanel vehicle(*sim_);
    auto* mass = vehicle.findChild<QDoubleSpinBox*>(QStringLiteral("Param/frame/massKg"));
    ASSERT_NE(mass, nullptr);
    EXPECT_DOUBLE_EQ(mass->value(), sim_->profile().frame.massKg);
    auto* apply = vehicle.findChild<QPushButton*>(QStringLiteral("ApplyProfileButton"));
    ASSERT_NE(apply, nullptr);
    EXPECT_FALSE(apply->isEnabled());
    mass->setValue(mass->value() + 0.05);
    EXPECT_TRUE(apply->isEnabled());
    apply->click();
    EXPECT_TRUE(sim_->profileModified());
    EXPECT_NEAR(sim_->profile().frame.massKg, mass->value(), 1e-9);

    auto* combo = vehicle.findChild<QComboBox*>(QStringLiteral("ProfileCombo"));
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(combo->findData(QStringLiteral("survey-1500g")));
    emit combo->activated(combo->currentIndex());
    EXPECT_EQ(sim_->profile().key, "survey-1500g");
    EXPECT_FALSE(sim_->profileModified());

    app::SimulationPanel simulation(*sim_);
    auto* camera = simulation.findChild<QComboBox*>(QStringLiteral("CameraCombo"));
    ASSERT_NE(camera, nullptr);
    sim_->setCameraMode(graphics::CameraMode::Fpv);
    EXPECT_EQ(camera->currentIndex(), static_cast<int>(graphics::CameraMode::Fpv));

    app::EnvironmentPanel environment(*scene_);
    auto* level = environment.findChild<QSpinBox*>(QStringLiteral("WindLevelSpin"));
    ASSERT_NE(level, nullptr);
    level->setValue(6);
    EXPECT_EQ(scene_->environment().wind.level, 6);
    run(0.01);
    EXPECT_EQ(sim_->snapshot().windLevel, 6);

    app::ControlsPanel controls(*sim_, settings_.get());
    auto* device = controls.findChild<QComboBox*>(QStringLiteral("DeviceCombo"));
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->count(), 1);  // Keyboard only (no joystick service in tests).
}

}  // namespace
