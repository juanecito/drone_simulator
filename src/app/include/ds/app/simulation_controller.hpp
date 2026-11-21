// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ds/app/scene_config.hpp"
#include "ds/common/error.hpp"
#include "ds/graphics/camera.hpp"
#include "ds/graphics/camera_rig.hpp"
#include "ds/graphics/drone_model.hpp"
#include "ds/graphics/mesh_loader.hpp"
#include "ds/graphics/rover_model.hpp"
#include "ds/graphics/weather_view.hpp"
#include "ds/input/input_selector.hpp"
#include "ds/input/joystick_service.hpp"
#include "ds/missions/mission.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/vehicle_factory.hpp"
#include "ds/sim/hud.hpp"
#include "ds/sim/item_visuals.hpp"
#include "ds/sim/simulation_loop.hpp"
#include "ds/storage/recording_store.hpp"
#include "ds/weather/wind_levels.hpp"

namespace ds::missions {
class MissionRunner;
}  // namespace ds::missions

/// @file
/// @brief Adapter between the GUI and the flight simulation: vehicle profiles, the simulation
/// loop, input devices, vehicle cameras, the drone model and the HUD (SPECIFICATION §4.1,
/// §4.11; FR-UI-06).
/// @ingroup app

namespace ds::app {

class SceneController;
class SettingsController;

/// @brief A vehicle profile as listed in the Vehicle tab.
struct VehicleProfileInfo {
    QString key;           ///< Stable key (built-in key or `user.*`).
    QString name;          ///< Display name.
    bool builtIn = false;  ///< Built-in profiles are read-only.
    physics::VehicleType type = physics::VehicleType::Aerial;  ///< Vehicle type.
};

/// @brief Settings keys used by the simulation.
namespace sim_settings_keys {
inline constexpr const char* kProfile = "sim.vehicleProfile";       ///< Aerial profile key.
inline constexpr const char* kGroundProfile = "sim.groundProfile";  ///< Ground profile key.
inline constexpr const char* kVehicleType = "sim.vehicleType";      ///< "aerial" or "ground".
inline constexpr const char* kCameraMode = "sim.cameraMode";  ///< "fpv", "chase", "los", "free".
/// Joystick profile of the aerial drone: {guid, name} or null.
inline constexpr const char* kJoystick = "input.activeProfile";
/// Joystick profile of the ground drone: {guid, name} or null.
inline constexpr const char* kGroundJoystick = "input.activeGroundProfile";
}  // namespace sim_settings_keys

/// @brief Owns the simulation loop (on its own thread) and everything the GUI needs from it.
/// @details Lives on the GUI thread. The vehicle is an aerial or a ground drone; each type
/// remembers its own profile and joystick mapping. The world is built from the scene's terrain and
/// environment and follows their changes (terrain, wind, air, time). Once the simulation has
/// been started the scene's clock follows the simulated UTC. Finished flights are written to
/// the `sessions` table.
class SimulationController final : public QObject {
    Q_OBJECT

public:
    /// @brief Default profile for new users (gentle, GPS altitude hold).
    static constexpr const char* kDefaultProfile = "camera-250g";
    /// @brief Default ground profile.
    static constexpr const char* kDefaultGroundProfile = "rover-4wd-3kg";

    /// @brief Builds the world at the free camera's ground position, heading the same way.
    /// @param scene Scene (terrain, environment); must outlive this object.
    /// @param settings User settings, or null (no persistence, no user profiles).
    /// @param assetDirectory Holds `vehicles/aerial/*.json`, `vehicles/ground/*.json` and
    /// `environment/wind_levels.json`.
    /// @param joysticks Start the SDL joystick service (off in tests).
    /// @param parent Parent object.
    SimulationController(SceneController& scene, SettingsController* settings,
                         const std::filesystem::path& assetDirectory, bool joysticks = true,
                         QObject* parent = nullptr);
    ~SimulationController() override;

    SimulationController(const SimulationController&) = delete;
    SimulationController& operator=(const SimulationController&) = delete;
    SimulationController(SimulationController&&) = delete;
    SimulationController& operator=(SimulationController&&) = delete;

    // --- Vehicle profiles (Vehicle tab) ---------------------------------------------------

    /// @brief Built-in profiles followed by the user's (sorted by name), both vehicle types.
    [[nodiscard]] const QList<VehicleProfileInfo>& profiles() const noexcept { return profiles_; }
    /// @brief The profiles of vehicle type @p type.
    [[nodiscard]] QList<VehicleProfileInfo> profiles(physics::VehicleType type) const;
    /// @brief Profile being flown (possibly edited and not saved).
    [[nodiscard]] const physics::VehicleProfile& profile() const noexcept { return profile_; }
    /// @brief The flown profile if it is an aerial one, else null.
    [[nodiscard]] const physics::AerialProfile* aerialProfile() const noexcept {
        return std::get_if<physics::AerialProfile>(&profile_);
    }
    /// @brief The flown profile if it is a ground one, else null.
    [[nodiscard]] const physics::GroundProfile* groundProfile() const noexcept {
        return std::get_if<physics::GroundProfile>(&profile_);
    }
    /// @brief Key of the flown profile.
    [[nodiscard]] QString profileKey() const {
        return QString::fromStdString(physics::profileKey(profile_));
    }
    /// @brief Display name of the flown profile.
    [[nodiscard]] QString profileName() const {
        return QString::fromStdString(physics::profileName(profile_));
    }
    /// @brief Type of the vehicle being flown or driven.
    [[nodiscard]] physics::VehicleType vehicleType() const noexcept {
        return physics::profileType(profile_);
    }
    /// @brief True if the flown profile has unsaved edits.
    [[nodiscard]] bool profileModified() const noexcept { return modified_; }
    /// @brief Loads profile @p key (built-in or user, either type).
    [[nodiscard]] common::Result<physics::VehicleProfile> profileFor(const QString& key) const;
    /// @brief Flies profile @p key (resets the vehicle) and remembers the choice.
    common::Result<void> selectProfile(const QString& key);
    /// @brief Switches to vehicle type @p type with the profile last chosen for it (or its
    /// default); nothing happens if it is already the type flown.
    common::Result<void> selectVehicleType(physics::VehicleType type);
    /// @brief Flies an edited copy of the current profile (same type; validated; resets the
    /// vehicle).
    common::Result<void> applyProfile(const physics::VehicleProfile& profile);
    /// @brief Saves the flown profile as user profile @p name; returns its key.
    common::Result<QString> saveProfileAs(const QString& name);
    /// @brief Deletes user profile @p key (the default profile of its type is flown if it was
    /// selected).
    common::Result<void> deleteProfile(const QString& key);
    /// @brief Wind levels (from `wind_levels.json`).
    [[nodiscard]] const weather::WindLevelTable& windTable() const noexcept { return windTable_; }

    // --- Loop (Simulation tab and menu) -------------------------------------------------

    /// @brief Starts or resumes; switches from the free camera to the chase camera.
    void start();
    /// @brief Pauses.
    void pause();
    /// @brief Pauses a running simulation, resumes a paused one, starts a stopped one (`P`).
    void togglePause();
    /// @brief Runs one tick and pauses.
    void step();
    /// @brief Puts the vehicle back at the take-off point, disarmed (`R`).
    void reset();
    /// @brief Moves the take-off point under the free camera and resets.
    void moveHomeToCamera();
    /// @brief Loop state of the last snapshot.
    [[nodiscard]] sim::RunState state() const noexcept { return state_; }
    /// @brief Changes the time scale (0.1–4).
    common::Result<void> setTimeScale(double scale);

    // --- Missions (FR-MIS-08, FR-MIS-09) -------------------------------------------------

    /// @brief Flies @p mission with @p profile: shows the mission's terrain and environment,
    /// places the vehicle at the spawn point and enters PreFlight (the mission starts when the
    /// vehicle is armed). Mission rules apply until endMission(): time scale 1×, no free
    /// camera, no resets, the profile locked, the keyboard only if `allowKeyboard`.
    /// @p missionDir (the mission file's folder) is searched first for item model files.
    common::Result<void> startMission(const missions::MissionDefinition& mission,
                                      const physics::VehicleProfile& profile,
                                      const std::filesystem::path& missionDir = {});
    /// @brief Starts the attempt again from a clean state (vehicle back at the spawn point).
    void retryMission();
    /// @brief Ends the running attempt as Aborted.
    void abortMission();
    /// @brief Leaves the mission: the free-mode scene, vehicle and take-off point come back.
    void endMission();
    /// @brief Items of the free-flight scene (Items tab): drawn and solid outside missions.
    void setFreeItems(const std::vector<items::Item>& freeItems);
    /// @brief True between startMission() and endMission().
    [[nodiscard]] bool missionActive() const noexcept { return mission_.has_value(); }
    /// @brief The mission being flown (valid while missionActive()).
    [[nodiscard]] const missions::MissionDefinition* mission() const noexcept {
        return mission_ ? &*mission_ : nullptr;
    }
    /// @brief Mission part of the latest snapshot.
    [[nodiscard]] const sim::MissionStatus& missionStatus() const noexcept {
        return loop_->snapshot().mission;
    }
    /// @brief True if the keyboard may fly (free mode, or a mission with `allowKeyboard`).
    [[nodiscard]] bool keyboardAllowed() const noexcept;
    /// @brief The next recorded session of a mission is linked to attempt @p attemptId (the
    /// attempt is saved when it ends, its session when the next one starts).
    void linkNextSessionToAttempt(std::int64_t attemptId) { attemptToLink_ = attemptId; }

    // --- Recording and replays (FR-X-05, FR-X-06; M7 step 7) -----------------------------

    /// @brief Plays the recorded session @p replay: the scene, vehicle, weather and seeds of
    /// its start come back and the world runs on the recorded controls (pause, single step
    /// and time scale work as usual). A session that flew a mission needs @p mission, the
    /// mission it flew (@p missionDir: its folder, for item models). Refused during a mission
    /// and for sessions that cannot be replayed. Free flight comes back with stopReplay().
    common::Result<void> startReplay(const storage::ReplayRecord& replay,
                                     const missions::MissionDefinition* mission = nullptr,
                                     const std::filesystem::path& missionDir = {});
    /// @brief Ends the replay: the free-flight scene, vehicle and take-off point come back.
    void stopReplay();
    /// @brief True between startReplay() and stopReplay().
    [[nodiscard]] bool replayActive() const noexcept { return replay_; }
    /// @brief Position in the replay (latest snapshot).
    [[nodiscard]] const sim::ReplayStatus& replayStatus() const noexcept {
        return loop_->snapshot().replay;
    }
    /// @brief Saves the sessions the simulation finished recording (called every frame; the
    /// writes run on the storage thread).
    void saveRecordings();

    // --- Cameras --------------------------------------------------------------------------

    /// @brief Current camera mode.
    [[nodiscard]] graphics::CameraMode cameraMode() const noexcept { return cameraMode_; }
    /// @brief Selects the camera mode (saved).
    void setCameraMode(graphics::CameraMode mode);
    /// @brief Next camera mode (`C`).
    void cycleCameraMode();
    /// @brief True when the view follows the vehicle (not the free camera).
    [[nodiscard]] bool vehicleView() const noexcept {
        return cameraMode_ != graphics::CameraMode::Free;
    }

    // --- Input ----------------------------------------------------------------------------

    /// @brief The joystick service, or null if SDL is not available.
    [[nodiscard]] input::JoystickService* joysticks() noexcept { return joysticks_.get(); }
    /// @brief Flight keys held (input::FlightKey bits).
    void setFlightKeys(std::uint32_t keys);
    /// @brief One-shot keyboard action (arm, modes, lights).
    void keyboardAction(input::KeyboardAction action);
    /// @brief Failure injection in free mode (FR-PHY-15); ignored during a mission, which
    /// schedules its own.
    void injectFailure(const physics::Failure& failure);
    /// @brief Ends every injected failure.
    void clearFailures();
    /// @brief Flies the current vehicle type with @p mapping (a joystick mapping of that
    /// type), or with the keyboard if empty; remembered per vehicle type.
    common::Result<void> setJoystickMapping(std::optional<input::InputMapping> mapping);
    /// @brief The joystick mapping of the current vehicle type, if any.
    [[nodiscard]] const std::optional<input::InputMapping>& joystickMapping() const noexcept {
        return mappings_[typeIndex(vehicleType())];
    }
    /// @brief The joystick mapping remembered for vehicle type @p type, if any.
    [[nodiscard]] const std::optional<input::InputMapping>& joystickMapping(
        physics::VehicleType type) const noexcept {
        return mappings_[typeIndex(type)];
    }
    /// @brief Controls of the current vehicle type (aerial sticks or ground drive).
    [[nodiscard]] input::VehicleControls vehicleControls() const noexcept {
        return vehicleType() == physics::VehicleType::Ground ? input::VehicleControls::Ground
                                                             : input::VehicleControls::Aerial;
    }
    /// @brief Device of the last tick.
    [[nodiscard]] input::InputSource activeInputSource() const noexcept;

    // --- Per frame (viewport) -----------------------------------------------------------

    /// @brief Takes the latest snapshot, handles its events and prepares the camera, the draw
    /// items and the HUD for a frame @p dt seconds after the previous one.
    void updateFrame(double dt, double aspect);
    /// @brief Latest snapshot.
    [[nodiscard]] const sim::WorldSnapshot& snapshot() const noexcept { return loop_->snapshot(); }
    /// @brief Vehicle camera of the frame (valid when vehicleView()).
    [[nodiscard]] const graphics::Camera& vehicleCamera() const noexcept { return camera_; }
    /// @brief Cameras of the two chase insets for insets of @p aspect: the top view (north
    /// up) and the behind view (rigidly behind the nose). Meaningful in the Chase mode.
    [[nodiscard]] std::array<graphics::Camera, 2> chaseInsetCameras(double aspect) const {
        return {rig_.topView(pose_, aspect), rig_.behindView(pose_, aspect)};
    }
    /// @brief Objects to draw (the vehicle, except from its own FPV camera).
    [[nodiscard]] std::span<const graphics::DrawItem> drawItems() const noexcept { return items_; }
    /// @brief Local lights of the frame (the vehicle's headlights).
    [[nodiscard]] std::span<const graphics::SpotLight> lights() const noexcept { return lights_; }
    /// @brief HUD of the frame.
    [[nodiscard]] const sim::HudModel& hud() const noexcept { return hud_; }
    /// @brief Vehicle pose of the frame (extrapolated between ticks).
    [[nodiscard]] const graphics::VehiclePose& vehiclePose() const noexcept { return pose_; }
    /// @brief Vehicle position of the frame (extrapolated between ticks).
    [[nodiscard]] const glm::dvec3& vehiclePosition() const noexcept { return pose_.position; }
    /// @brief Take-off point.
    [[nodiscard]] const glm::dvec3& home() const noexcept { return home_; }
    /// @brief Weather to draw this frame (FR-WX-09): the snapshot's, its clock extrapolated
    /// like the pose; cloud and fog heights are measured from the ground at the take-off
    /// point.
    [[nodiscard]] const graphics::WeatherView& weatherView() const noexcept { return weather_; }
    /// @brief Velocity of the vehicle camera (FPV and Chase follow the vehicle; the others
    /// stand still), world frame.
    [[nodiscard]] glm::dvec3 cameraVelocity() const noexcept;
    /// @brief Wetness of the FPV camera's lens 0–1 (raindrops, FR-WX-09): it gets wet in rain,
    /// drizzle or sleet and dries in a minute or two after; 0 for the other cameras.
    [[nodiscard]] double lensWetness() const noexcept {
        return cameraMode_ == graphics::CameraMode::Fpv ? lensWetness_ : 0.0;
    }
    /// @brief The loop (tests).
    [[nodiscard]] sim::SimulationLoop& loop() noexcept { return *loop_; }

signals:
    /// @brief The loop state changed.
    void stateChanged();
    /// @brief The profile list changed.
    void profilesChanged();
    /// @brief Another profile (or an edited one) is flown.
    void profileChanged();
    /// @brief The camera mode changed.
    void cameraModeChanged();
    /// @brief A simulation event happened (crash, arming refused, …); @p text describes it.
    void simulationEvent(const QString& text);
    /// @brief About four times a second, for telemetry read-outs.
    void telemetryUpdated();
    /// @brief A mission was started or ended (missionActive() changed).
    void missionActiveChanged();
    /// @brief The mission's state changed (PreFlight → InProgress, …).
    void missionStateChanged();
    /// @brief The current attempt ended (succeeded, failed or aborted); missionStatus().result
    /// holds the result. Emitted once per attempt.
    void missionFinished();
    /// @brief A replay started or ended (replayActive() changed).
    void replayActiveChanged();
    /// @brief A flight session was saved (History tab); @p sessionId is its row.
    void sessionRecorded(qint64 sessionId);

private:
    void loadWindTable(const std::filesystem::path& assets);
    void loadWeatherPresets(const std::filesystem::path& assets);
    /// Replaces the procedural body by the profile's model file, if it has one.
    void applyModelFile();
    void loadBuiltInProfiles(const std::filesystem::path& assets);
    void syncBuiltInProfiles();
    void reloadProfiles();
    void restoreSelections();
    void restoreJoystick(physics::VehicleType type);
    void createLoop();
    void setFlownProfile(const physics::VehicleProfile& profile, bool modified);
    void activateJoystick();
    void buildModel();
    void appendVehicleItems(const sim::WorldSnapshot& s);
    [[nodiscard]] graphics::RoverVisualState roverState(const sim::WorldSnapshot& s) const;
    [[nodiscard]] static std::size_t typeIndex(physics::VehicleType type) noexcept {
        return type == physics::VehicleType::Ground ? 1 : 0;
    }
    void onTerrainChanged();
    void onEnvironmentChanged();
    void handleEvents();
    void saveRecording(sim::FlightRecording recording);
    void updateSceneTag();
    void saveFreeState();
    void restoreFreeState();
    [[nodiscard]] common::Result<std::unique_ptr<missions::MissionRunner>> enterMissionScene(
        const missions::MissionDefinition& mission, const std::filesystem::path& missionDir);
    void updatePilotEye();
    // Height the pilot stands at: the terrain, or the launch platform's top over it.
    [[nodiscard]] double standingHeight(double x, double z) const;
    void applyFreeFlightRules();
    void refuseInMission(const QString& what);
    [[nodiscard]] sim::GroundFunction groundFunction() const;
    [[nodiscard]] graphics::FpvCameraParams fpvParams() const;

    SceneController* scene_;
    SettingsController* settings_;
    weather::WindLevelTable windTable_ = weather::WindLevelTable::builtIn();
    weather::WeatherPresetTable weatherPresets_ = weather::WeatherPresetTable::builtIn();
    std::vector<physics::VehicleProfile> builtIn_;
    QList<VehicleProfileInfo> profiles_;
    physics::VehicleProfile profile_;
    bool modified_ = false;
    std::unique_ptr<graphics::DroneModel> model_;
    std::unique_ptr<graphics::RoverModel> rover_;
    /// Model files (FR-GFX-12): `~/.drone_simulation/models`, then `<assets>/models`.
    std::unique_ptr<graphics::ModelLibrary> models_;

    std::unique_ptr<input::JoystickService> joysticks_;
    std::unique_ptr<input::InputSelector> selector_;
    std::array<std::optional<input::InputMapping>, 2> mappings_;  // Aerial, ground.
    std::unique_ptr<sim::SimulationLoop> loop_;
    sim::RunState state_ = sim::RunState::Stopped;
    bool started_ = false;

    glm::dvec3 home_{0.0};
    double homeHeadingRad_ = 0.0;
    EnvironmentConfig lastEnvironment_;

    void updateLens();
    graphics::CameraMode cameraMode_ = graphics::CameraMode::Free;
    graphics::WeatherView weather_;
    std::optional<glm::dvec2> weatherGroundAt_;  // Where weatherGroundY_ was sampled.
    double weatherGroundY_ = 0.0;
    double lensWetness_ = 0.0;
    std::optional<double> lensTimeS_;  // Weather time of the last lens update.
    graphics::CameraRig rig_;
    graphics::VehiclePose pose_;
    graphics::Camera camera_;
    std::vector<graphics::DrawItem> items_;
    std::vector<graphics::SpotLight> lights_;
    sim::HudModel hud_;
    double telemetryClock_ = 0.0;

    // Mission.
    std::optional<missions::MissionDefinition> mission_;
    std::unique_ptr<sim::ItemVisuals> itemVisuals_;
    // Launch platform under the take-off position (sim::LaunchPlatform), its mesh and size.
    std::shared_ptr<const graphics::Mesh> platformMesh_;
    std::optional<sim::LaunchPlatform> platformShown_;
    std::optional<sim::LaunchPlatform> pilotPlatform_;  // The pilot eye was placed with it.
    std::unique_ptr<sim::ItemVisuals> freeVisuals_;
    std::vector<std::string> highlighted_;
    glm::dvec3 missionSpawn_{0.0};
    double missionHeadingRad_ = 0.0;
    glm::dvec3 missionPilot_{0.0};
    physics::VehicleProfile freeProfile_;
    bool freeProfileModified_ = false;
    glm::dvec3 freeHome_{0.0};
    double freeHeadingRad_ = 0.0;
    std::optional<missions::MissionState> lastMissionState_;
    std::uint64_t reportedAttempt_ = ~std::uint64_t{0};

    std::uint64_t lastResetCount_ = 0;

    // Recording and replays.
    std::vector<items::Item> freeItems_;
    std::optional<std::int64_t> attemptToLink_;
    bool replay_ = false;
    bool replayMission_ = false;  // The replayed session flew a mission.
    double freeTickRateHz_ = 0.0;
    std::vector<std::future<std::optional<std::int64_t>>> pendingSaves_;
};

}  // namespace ds::app
