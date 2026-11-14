// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/common/triple_buffer.hpp"
#include "ds/input/control.hpp"
#include "ds/sim/fixed_step.hpp"
#include "ds/sim/world.hpp"

/// @file
/// @brief The main simulation loop (SPECIFICATION §4.1): fixed timestep on its own thread,
/// states Stopped / Running / Paused / SingleStep, time scale, triple-buffered snapshots and
/// an event queue for the GUI.
/// @ingroup sim

namespace ds::sim {

/// @brief Supplies the controls of each tick (called on the simulation thread).
using InputFunction = std::function<input::ControlInputs(double dt)>;

/// @brief Runs a World at a fixed rate in real time.
/// @details All World access happens on the simulation thread; the GUI changes the world
/// through post(). Snapshots are taken with acquireSnapshot()/snapshot() from one consumer
/// thread (the GUI thread). Without start() (or with runTicks()) the loop is fully
/// synchronous, for headless runs and tests (FR-LOOP-08).
class SimulationLoop {
public:
    /// @brief Takes ownership of @p world; @p input supplies the controls.
    SimulationLoop(std::unique_ptr<World> world, InputFunction input);
    ~SimulationLoop();
    SimulationLoop(const SimulationLoop&) = delete;
    SimulationLoop& operator=(const SimulationLoop&) = delete;
    SimulationLoop(SimulationLoop&&) = delete;
    SimulationLoop& operator=(SimulationLoop&&) = delete;

    /// @brief Starts the thread (if needed) and runs.
    void start();
    /// @brief Pauses (the clock stops, FR-LOOP-04a).
    void pause();
    /// @brief Continues after pause().
    void resume();
    /// @brief Runs exactly one tick, then pauses.
    void step();
    /// @brief Stops ticking (the thread stays alive; start() continues).
    void stop();
    /// @brief Ends the thread (destructor).
    void shutdown();
    /// @brief Current state.
    [[nodiscard]] RunState state() const noexcept { return state_.load(); }

    /// @brief Tick rate, 240–2000 Hz (FR-LOOP-01).
    void setTickRate(double hz);
    /// @brief Time scale 0.1–4 (FR-LOOP-04); rejected while a mission runs.
    common::Result<void> setTimeScale(double scale);
    /// @brief Locks the time scale to 1 while a mission runs (FR-LOOP-04).
    void setMissionActive(bool active);

    /// @brief Runs @p command on the simulation thread before the next tick (or at once when
    /// the thread is not running), then publishes a snapshot.
    void post(std::function<void(World&)> command);
    /// @brief Synchronous: runs @p count ticks on the calling thread (thread not running).
    void runTicks(int count);

    /// @brief Takes the latest snapshot (consumer thread). True if it changed.
    bool acquireSnapshot();
    /// @brief The snapshot taken by the last acquireSnapshot().
    [[nodiscard]] const WorldSnapshot& snapshot() const noexcept { return snapshots_.readBuffer(); }
    /// @brief Events since the last call (any thread).
    [[nodiscard]] std::vector<TimedEvent> takeEvents();

    /// @brief Direct access for synchronous use (thread not running).
    [[nodiscard]] World& world() noexcept { return *world_; }

private:
    void run();
    void tickOnce(double dt);
    void runCommands();
    void publish();

    std::unique_ptr<World> world_;
    InputFunction input_;
    FixedStepClock clock_;
    std::atomic<RunState> state_{RunState::Stopped};
    std::atomic<bool> missionActive_{false};
    std::atomic<bool> quit_{false};
    std::thread thread_;
    std::mutex commandMutex_;
    std::condition_variable wake_;
    std::vector<std::function<void(World&)>> commands_;
    std::vector<std::function<void(FixedStepClock&)>> clockCommands_;
    common::TripleBuffer<WorldSnapshot> snapshots_;
    std::mutex eventMutex_;
    std::vector<TimedEvent> events_;
    double tickCostMs_ = 0.0;
};

}  // namespace ds::sim
