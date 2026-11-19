// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Simulation adapter, Simulation tab, Vehicle tab and Settings › Controls (FR-LOOP-03/04a,
// FR-PHY-15, FR-IN-05, FR-WIND-01, FR-GFX-05, FR-GFX-14). Runs on the offscreen platform; the
// loop runs synchronously (no thread) unless a test starts it.

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QImage>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "ds/app/asset_paths.hpp"
#include "ds/app/controls_panel.hpp"
#include "ds/app/environment_panel.hpp"
#include "ds/app/hud_painter.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/app/simulation_panel.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/app/vehicle_panel.hpp"
#include "ds/input/keyboard.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/ground_profile.hpp"
#include "ds/storage/input_profile_store.hpp"
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
    EXPECT_EQ(builtIn,
              (QStringList{"camera-250g", "fpv-5in-racer", "survey-1500g", "rover-4wd-3kg"}));
    ASSERT_EQ(sim_->profiles(physics::VehicleType::Ground).size(), 1);
    EXPECT_EQ(sim_->profiles(physics::VehicleType::Ground).front().key, "rover-4wd-3kg");
    EXPECT_EQ(sim_->vehicleType(), physics::VehicleType::Aerial);
    EXPECT_EQ(sim_->aerialProfile()->key, SimulationController::kDefaultProfile);

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
    EXPECT_EQ(sim_->aerialProfile()->key, "fpv-5in-racer");
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
    physics::AerialProfile p = *sim_->aerialProfile();
    p.frame.massKg *= 1.2;
    const double heavy = p.frame.massKg;
    ASSERT_TRUE(sim_->applyProfile(p).has_value());
    EXPECT_TRUE(sim_->profileModified());
    EXPECT_EQ(sim_->aerialProfile()->key, SimulationController::kDefaultProfile);

    p.frame.massKg = -1.0;  // Invalid: refused, nothing changes.
    EXPECT_FALSE(sim_->applyProfile(p).has_value());

    auto key = sim_->saveProfileAs(QStringLiteral("Heavy Camera"));
    ASSERT_TRUE(key.has_value()) << key.error().message;
    EXPECT_EQ(*key, QStringLiteral("user.heavy-camera"));
    EXPECT_FALSE(sim_->profileModified());
    EXPECT_EQ(sim_->aerialProfile()->name, "Heavy Camera");
    EXPECT_FALSE(sim_->aerialProfile()->reference.has_value());

    // A new controller lists and loads it.
    settings_->flush();
    sim_.reset();
    sim_ = makeSimulation();
    EXPECT_EQ(sim_->aerialProfile()->key, "user.heavy-camera");
    auto loaded = sim_->profileFor(*key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_NEAR(std::get<physics::AerialProfile>(*loaded).frame.massKg, heavy, 1e-9);

    EXPECT_FALSE(sim_->deleteProfile(QStringLiteral("camera-250g")).has_value());
    ASSERT_TRUE(sim_->deleteProfile(*key).has_value());
    EXPECT_EQ(sim_->aerialProfile()->key, SimulationController::kDefaultProfile);
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
    const double expected = std::clamp(14.0 * sim_->aerialProfile()->frame.armLengthM, 1.5, 6.0);
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
    EXPECT_DOUBLE_EQ(mass->value(), sim_->aerialProfile()->frame.massKg);
    auto* apply = vehicle.findChild<QPushButton*>(QStringLiteral("ApplyProfileButton"));
    ASSERT_NE(apply, nullptr);
    EXPECT_FALSE(apply->isEnabled());
    mass->setValue(mass->value() + 0.05);
    EXPECT_TRUE(apply->isEnabled());
    apply->click();
    EXPECT_TRUE(sim_->profileModified());
    EXPECT_NEAR(sim_->aerialProfile()->frame.massKg, mass->value(), 1e-9);

    auto* combo = vehicle.findChild<QComboBox*>(QStringLiteral("ProfileCombo"));
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(combo->findData(QStringLiteral("survey-1500g")));
    emit combo->activated(combo->currentIndex());
    EXPECT_EQ(sim_->aerialProfile()->key, "survey-1500g");
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

// --- Ground drone (M6 step 2) ---------------------------------------------------------------

TEST_F(SimulationPanelsTest, VehicleTypeSwitchesAndIsRemembered) {
    ASSERT_TRUE(sim_->selectProfile(QStringLiteral("survey-1500g")).has_value());
    ASSERT_TRUE(sim_->selectVehicleType(physics::VehicleType::Ground).has_value());
    EXPECT_EQ(sim_->vehicleType(), physics::VehicleType::Ground);
    ASSERT_NE(sim_->groundProfile(), nullptr);
    EXPECT_EQ(sim_->aerialProfile(), nullptr);
    EXPECT_EQ(sim_->profileKey(), QStringLiteral("rover-4wd-3kg"));
    EXPECT_EQ(sim_->vehicleControls(), input::VehicleControls::Ground);
    run(0.1);
    EXPECT_EQ(sim_->snapshot().telemetry.type, physics::VehicleType::Ground);
    EXPECT_EQ(sim_->snapshot().profileKey, "rover-4wd-3kg");

    // Both the type and each type's profile are remembered.
    settings_->flush();
    sim_.reset();
    sim_ = makeSimulation();
    EXPECT_EQ(sim_->vehicleType(), physics::VehicleType::Ground);
    EXPECT_EQ(sim_->profileKey(), QStringLiteral("rover-4wd-3kg"));
    ASSERT_TRUE(sim_->selectVehicleType(physics::VehicleType::Aerial).has_value());
    EXPECT_EQ(sim_->profileKey(), QStringLiteral("survey-1500g"));

    // A profile of the other type cannot be applied as an edit of this one.
    EXPECT_FALSE(sim_->applyProfile(physics::GroundProfile{}).has_value());
}

TEST_F(SimulationPanelsTest, KeyboardDrivesTheRoverAndIsRecorded) {
    ASSERT_TRUE(sim_->selectVehicleType(physics::VehicleType::Ground).has_value());
    run(1.0);  // Settle on the suspension.
    const glm::dvec3 start = sim_->snapshot().body.position;
    EXPECT_FALSE(sim_->snapshot().telemetry.armed);

    // The rover is drawn: body, wheels and lamps.
    sim_->setCameraMode(graphics::CameraMode::Chase);
    sim_->updateFrame(0.016, 16.0 / 9.0);
    EXPECT_EQ(sim_->drawItems().size(), 1U + 4U + 4U);

    sim_->keyboardAction(input::KeyboardAction::ToggleArm);  // Enter.
    run(0.2);
    ASSERT_TRUE(sim_->snapshot().telemetry.armed);
    sim_->setFlightKeys(static_cast<std::uint32_t>(input::FlightKey::ThrottleUp));
    run(2.5);
    EXPECT_NEAR(sim_->snapshot().controls.drive, 1.0, 1e-9);
    EXPECT_GT(sim_->snapshot().telemetry.ground.odometrySpeedMs, 0.8);
    const glm::dvec3 moved = sim_->snapshot().body.position - start;
    EXPECT_GT(std::hypot(moved.x, moved.z), 1.5);
    EXPECT_EQ(sim_->hud().mode.text, "Manual");

    // Space held brakes (the drive key released springs back to neutral).
    sim_->setFlightKeys(static_cast<std::uint32_t>(input::FlightKey::Brake));
    run(1.5);
    EXPECT_TRUE(sim_->snapshot().controls.brake);
    EXPECT_NEAR(sim_->snapshot().controls.drive, 0.0, 1e-9);
    EXPECT_LT(std::abs(sim_->snapshot().telemetry.ground.speedMs), 0.1);
    sim_->setFlightKeys(0);

    // Keys 3 and 4 select Speed control.
    sim_->keyboardAction(input::KeyboardAction::Mode3);
    run(0.1);
    EXPECT_EQ(sim_->snapshot().telemetry.ground.mode, physics::DriveMode::SpeedControl);

    sim_->reset();
    run(0.1);
    auto sessions =
        storage_.service
            ->submit([](storage::DbConnection& c) { return storage::SessionStore(c).recent(10); })
            .get();
    ASSERT_TRUE(sessions.has_value());
    ASSERT_EQ(sessions->size(), 1U);
    EXPECT_EQ(sessions->front().vehicleType, "ground");
    EXPECT_EQ(sessions->front().vehicleProfileKey, "rover-4wd-3kg");
    EXPECT_GT(sessions->front().distanceM, 1.5);
}

TEST(HudPainter, PaintsTheGroundGauges) {
    // FR-GFX-20: the load-transfer bar and the wheel-slip diagram are drawn under the
    // attitude column; one wheel spinning (red), one off the ground (grey).
    sim::WorldSnapshot s;
    s.hasVehicle = true;
    s.telemetry.type = physics::VehicleType::Ground;
    s.telemetry.armed = true;
    physics::GroundTelemetry& g = s.telemetry.ground;
    g.wheelCount = 4;
    g.contact = {true, true, true, false};
    g.slip = {0.05, 0.3, 0.8, 0.0};
    g.maxSlip = 0.8;
    g.loadTransfer = 0.7;
    const sim::HudModel hud = sim::buildHud(s, geo::LocalFrame({40.4, -3.7, 650.0}), {});
    ASSERT_TRUE(hud.ground.has_value());

    QImage image(1280, 720, QImage::Format_RGB32);
    image.fill(QColor(70, 90, 120));
    {
        QPainter painter(&image);
        app::paintHud(painter, image.rect(), hud, {});
    }
    if (const char* dir = std::getenv("DS_TEST_IMAGE_DIR")) {
        image.save(QString::fromUtf8(dir) + QStringLiteral("/ground_hud.png"));
    }
    // Strongly red pixels (the spinning wheel, the red end of the bar and the warning) and
    // amber ones exist in the left half.
    int red = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width() / 2; ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.red() > 200 && c.green() < 90 && c.blue() < 90) {
                ++red;
            }
        }
    }
    EXPECT_GT(red, 40);
}

TEST_F(SimulationPanelsTest, RoverHeadlightsAndGroundHud) {
    ASSERT_TRUE(sim_->selectVehicleType(physics::VehicleType::Ground).has_value());
    sim_->setCameraMode(graphics::CameraMode::Fpv);
    run(0.5);
    EXPECT_TRUE(sim_->lights().empty());
    ASSERT_TRUE(sim_->hud().ground.has_value());
    EXPECT_EQ(sim_->hud().ground->wheelCount, 4);
    EXPECT_TRUE(sim_->hud().altitudeAgl.text.empty());

    // `L`: two headlight beams, also from the rover's own FPV camera (where the rover itself
    // is not drawn), aimed the way the rover faces.
    sim_->keyboardAction(input::KeyboardAction::ToggleLights);
    run(0.1);
    EXPECT_TRUE(sim_->drawItems().empty());
    ASSERT_EQ(sim_->lights().size(), 2U);
    const glm::dvec3 nose = sim_->vehiclePose().orientation * glm::dvec3(1.0, 0.0, 0.0);
    for (const graphics::SpotLight& l : sim_->lights()) {
        EXPECT_GT(glm::dot(l.direction, nose), 0.99);
    }
    sim_->keyboardAction(input::KeyboardAction::ToggleLights);
    run(0.1);
    EXPECT_TRUE(sim_->lights().empty());

    // The aerial drone has no headlights (yet) and no ground HUD.
    ASSERT_TRUE(sim_->selectVehicleType(physics::VehicleType::Aerial).has_value());
    sim_->keyboardAction(input::KeyboardAction::ToggleLights);
    run(0.1);
    EXPECT_TRUE(sim_->lights().empty());
    EXPECT_FALSE(sim_->hud().ground.has_value());
}

TEST_F(SimulationPanelsTest, VehiclePanelEditsGroundProfiles) {
    app::VehiclePanel vehicle(*sim_);
    auto* type = vehicle.findChild<QComboBox*>(QStringLiteral("VehicleTypeCombo"));
    ASSERT_NE(type, nullptr);
    auto* aerialEditor = vehicle.findChild<QWidget*>(QStringLiteral("AerialEditor"));
    auto* groundEditor = vehicle.findChild<QWidget*>(QStringLiteral("GroundEditor"));
    ASSERT_NE(aerialEditor, nullptr);
    ASSERT_NE(groundEditor, nullptr);
    EXPECT_FALSE(groundEditor->isVisibleTo(&vehicle));

    type->setCurrentIndex(type->findData(static_cast<int>(physics::VehicleType::Ground)));
    emit type->activated(type->currentIndex());
    EXPECT_EQ(sim_->vehicleType(), physics::VehicleType::Ground);
    EXPECT_TRUE(groundEditor->isVisibleTo(&vehicle));
    EXPECT_FALSE(aerialEditor->isVisibleTo(&vehicle));
    auto* combo = vehicle.findChild<QComboBox*>(QStringLiteral("ProfileCombo"));
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 1);
    EXPECT_EQ(combo->itemData(0).toString(), QStringLiteral("rover-4wd-3kg"));
    auto* figures = vehicle.findChild<QLabel*>(QStringLiteral("FiguresLabel"));
    ASSERT_NE(figures, nullptr);
    EXPECT_TRUE(figures->text().contains(QStringLiteral("Top speed")));
    EXPECT_TRUE(figures->text().contains(QStringLiteral("rollover")));

    auto* mass = vehicle.findChild<QDoubleSpinBox*>(QStringLiteral("GroundParam/chassis/massKg"));
    ASSERT_NE(mass, nullptr);
    EXPECT_DOUBLE_EQ(mass->value(), sim_->groundProfile()->chassis.massKg);
    mass->setValue(3.4);
    vehicle.findChild<QPushButton*>(QStringLiteral("ApplyProfileButton"))->click();
    EXPECT_TRUE(sim_->profileModified());
    EXPECT_NEAR(sim_->groundProfile()->chassis.massKg, 3.4, 1e-9);

    // Saved as a ground profile in the user's database.
    auto key = sim_->saveProfileAs(QStringLiteral("Heavy Rover"));
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, QStringLiteral("user.heavy-rover"));
    auto rows = storage_.service
                    ->submit([](storage::DbConnection& c) {
                        return storage::VehicleProfileStore(c).list("ground");
                    })
                    .get();
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 2U);  // The built-in rover and the new one.
    auto loaded = sim_->profileFor(*key);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(std::holds_alternative<physics::GroundProfile>(*loaded));
    EXPECT_NEAR(std::get<physics::GroundProfile>(*loaded).chassis.massKg, 3.4, 1e-9);
    EXPECT_EQ(sim_->profiles(physics::VehicleType::Ground).size(), 2);
    EXPECT_EQ(sim_->profiles(physics::VehicleType::Aerial).size(), 3);

    // Deleting it falls back to the default rover, not to an aerial profile.
    ASSERT_TRUE(sim_->deleteProfile(*key).has_value());
    EXPECT_EQ(sim_->profileKey(), QStringLiteral("rover-4wd-3kg"));
}

TEST_F(SimulationPanelsTest, JoystickMappingsAreKeptPerVehicleType) {
    auto aerial = input::radioMapping("AETR");
    ASSERT_TRUE(aerial.has_value());
    aerial->deviceGuid = "guid-1";
    aerial->name = "Radio aerial";
    input::InputMapping ground = input::groundGamepadMapping(true);
    ground.deviceGuid = "guid-1";
    ground.name = "Pad ground";
    for (const input::InputMapping& m : {*aerial, ground}) {
        const storage::InputProfileRecord record{
            .deviceGuid = m.deviceGuid,
            .vehicleType = input::vehicleControlsName(m.vehicle),
            .name = m.name,
            .mappingJson = m.toJson().dump()};
        ASSERT_TRUE(storage_.service
                        ->submit([record](storage::DbConnection& c) {
                            return storage::InputProfileStore(c).save(record);
                        })
                        .get()
                        .has_value());
    }

    // A ground mapping is refused for the aerial drone.
    EXPECT_FALSE(sim_->setJoystickMapping(ground).has_value());
    ASSERT_TRUE(sim_->setJoystickMapping(*aerial).has_value());
    ASSERT_TRUE(sim_->selectVehicleType(physics::VehicleType::Ground).has_value());
    EXPECT_FALSE(sim_->joystickMapping().has_value());  // Keyboard for the rover so far.
    ASSERT_TRUE(sim_->setJoystickMapping(ground).has_value());
    EXPECT_EQ(sim_->joystickMapping()->name, "Pad ground");
    EXPECT_EQ(sim_->joystickMapping(physics::VehicleType::Aerial)->name, "Radio aerial");

    // Both are restored after a restart.
    settings_->flush();
    sim_.reset();
    sim_ = makeSimulation();
    ASSERT_TRUE(sim_->joystickMapping().has_value());
    EXPECT_EQ(sim_->joystickMapping()->vehicle, input::VehicleControls::Ground);
    EXPECT_EQ(sim_->joystickMapping()->name, "Pad ground");
    ASSERT_TRUE(sim_->joystickMapping(physics::VehicleType::Aerial).has_value());
    EXPECT_EQ(sim_->joystickMapping(physics::VehicleType::Aerial)->name, "Radio aerial");

    // The Controls panel follows the vehicle type.
    app::ControlsPanel controls(*sim_, settings_.get());
    auto* label = controls.findChild<QLabel*>(QStringLiteral("ControlsVehicleLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->text().startsWith(QStringLiteral("Ground")));
    ASSERT_TRUE(sim_->selectVehicleType(physics::VehicleType::Aerial).has_value());
    EXPECT_TRUE(label->text().startsWith(QStringLiteral("Aerial")));
}

}  // namespace
