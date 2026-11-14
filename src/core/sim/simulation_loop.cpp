// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/sim/simulation_loop.hpp"

#include <chrono>
#include <utility>

namespace ds::sim {
namespace {

constexpr auto kIdlePeriod = std::chrono::milliseconds(1);

}  // namespace

SimulationLoop::SimulationLoop(std::unique_ptr<World> world, InputFunction input)
    : world_(std::move(world)), input_(std::move(input)) {
    world_->events().subscribe([this](const Event& e) {
        const std::scoped_lock lock(eventMutex_);
        if (events_.size() < 1000) {
            events_.push_back({world_->time(), e});
        }
    });
    publish();
}

SimulationLoop::~SimulationLoop() { shutdown(); }

void SimulationLoop::shutdown() {
    quit_ = true;
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SimulationLoop::start() {
    state_ = RunState::Running;
    if (!thread_.joinable()) {
        quit_ = false;
        thread_ = std::thread([this] { run(); });
    }
    wake_.notify_all();
}

void SimulationLoop::pause() {
    if (state_ != RunState::Stopped) {
        state_ = RunState::Paused;
    }
}

void SimulationLoop::resume() {
    if (state_ == RunState::Paused) {
        state_ = RunState::Running;
        wake_.notify_all();
    }
}

void SimulationLoop::step() {
    if (!thread_.joinable()) {
        runTicks(1);
        state_ = RunState::Paused;
        return;
    }
    state_ = RunState::SingleStep;
    wake_.notify_all();
}

void SimulationLoop::stop() { state_ = RunState::Stopped; }

void SimulationLoop::setTickRate(double hz) {
    const std::scoped_lock lock(commandMutex_);
    if (thread_.joinable()) {
        clockCommands_.push_back([hz](FixedStepClock& c) { c.setRate(hz); });
    } else {
        clock_.setRate(hz);
    }
}

common::Result<void> SimulationLoop::setTimeScale(double scale) {
    if (missionActive_.load() && scale != 1.0) {
        return common::makeError("the time scale is locked to 1× during missions");
    }
    const std::scoped_lock lock(commandMutex_);
    if (thread_.joinable()) {
        clockCommands_.push_back([scale](FixedStepClock& c) { c.setTimeScale(scale); });
    } else {
        clock_.setTimeScale(scale);
    }
    return {};
}

void SimulationLoop::setMissionActive(bool active) {
    missionActive_ = active;
    if (active) {
        (void)setTimeScale(1.0);
    }
}

void SimulationLoop::post(std::function<void(World&)> command) {
    if (!thread_.joinable()) {
        command(*world_);
        publish();
        return;
    }
    {
        const std::scoped_lock lock(commandMutex_);
        commands_.push_back(std::move(command));
    }
    wake_.notify_all();
}

void SimulationLoop::runCommands() {
    std::vector<std::function<void(World&)>> commands;
    std::vector<std::function<void(FixedStepClock&)>> clockCommands;
    {
        const std::scoped_lock lock(commandMutex_);
        commands.swap(commands_);
        clockCommands.swap(clockCommands_);
    }
    for (auto& c : clockCommands) {
        c(clock_);
    }
    for (auto& c : commands) {
        c(*world_);
    }
    if (!commands.empty()) {
        publish();
    }
}

void SimulationLoop::tickOnce(double dt) {
    const auto begin = std::chrono::steady_clock::now();
    world_->tick(input_ ? input_(dt) : input::ControlInputs{}, dt);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    tickCostMs_ += 0.002 * (ms - tickCostMs_);  // Running average over ~500 ticks.
}

void SimulationLoop::runTicks(int count) {
    runCommands();
    for (int i = 0; i < count; ++i) {
        tickOnce(clock_.step());
    }
    publish();
}

void SimulationLoop::publish() {
    WorldSnapshot& s = snapshots_.writeBuffer();
    world_->fillSnapshot(s);
    s.state = state_.load();
    s.timeScale = clock_.timeScale();
    s.tickRateHz = clock_.rate();
    s.tickCostMs = tickCostMs_;
    s.publishedAt = std::chrono::steady_clock::now();
    snapshots_.publish();
}

bool SimulationLoop::acquireSnapshot() { return snapshots_.update(); }

std::vector<TimedEvent> SimulationLoop::takeEvents() {
    const std::scoped_lock lock(eventMutex_);
    return std::exchange(events_, {});
}

void SimulationLoop::run() {
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    RunState previous = state_.load();
    while (!quit_.load()) {
        runCommands();
        const auto now = Clock::now();
        const double real = std::chrono::duration<double>(now - last).count();
        last = now;
        const RunState state = state_.load();
        if (state == RunState::Running) {
            if (previous != RunState::Running) {
                clock_.reset();  // No catch-up of the paused time.
            } else {
                const int steps = clock_.advance(real);
                for (int i = 0; i < steps; ++i) {
                    tickOnce(clock_.step());
                }
                if (steps > 0) {
                    publish();
                }
            }
            previous = state;
            std::this_thread::sleep_for(kIdlePeriod);
        } else if (state == RunState::SingleStep) {
            tickOnce(clock_.step());
            state_ = RunState::Paused;
            publish();
            previous = RunState::Paused;
        } else {
            if (state != previous) {
                publish();  // Show the new state.
            }
            previous = state;
            std::unique_lock lock(commandMutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return quit_.load() || !commands_.empty() || !clockCommands_.empty() ||
                       state_.load() == RunState::Running || state_.load() == RunState::SingleStep;
            });
        }
    }
}

}  // namespace ds::sim
