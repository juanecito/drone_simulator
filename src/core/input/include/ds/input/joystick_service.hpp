// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/input/mapping.hpp"

/// @file
/// @brief RC transmitters and gamepads through SDL3 (FR-IN-01), with hot-plug.
/// @details SDL is used only inside this module (SPECIFICATION §3.1): the service owns a
/// thread that initialises SDL's joystick subsystem, polls the devices every 2 ms and
/// publishes their raw state. Gamepads recognised by SDL are reported in SDL's standard
/// layout (axes: left X, left Y, right X, right Y, left trigger, right trigger); other
/// devices (radios in joystick mode) report their raw axes.
/// @ingroup input

namespace ds::input {

/// @brief A connected device.
struct DeviceInfo {
    std::string guid;      ///< SDL GUID as text (stable across sessions for a device model).
    std::string name;      ///< Name reported by the device.
    int axes = 0;          ///< Number of axes.
    int buttons = 0;       ///< Number of buttons.
    bool gamepad = false;  ///< Recognised as a gamepad (standard layout).
    /// @brief Equality.
    bool operator==(const DeviceInfo&) const = default;
};

/// @brief Background service owning SDL's joystick subsystem.
class JoystickService {
public:
    JoystickService();
    ~JoystickService();
    JoystickService(const JoystickService&) = delete;
    JoystickService& operator=(const JoystickService&) = delete;
    JoystickService(JoystickService&&) = delete;
    JoystickService& operator=(JoystickService&&) = delete;

    /// @brief Starts the thread and initialises SDL; fails if SDL cannot be initialised.
    common::Result<void> start();
    /// @brief Stops the thread and shuts SDL down (also done by the destructor).
    void stop();
    /// @brief True while running.
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    /// @brief Connected devices.
    [[nodiscard]] std::vector<DeviceInfo> devices() const;
    /// @brief Incremented whenever a device is connected or disconnected.
    [[nodiscard]] std::uint64_t devicesVersion() const noexcept { return version_.load(); }
    /// @brief Latest state of the first device with @p guid (connected = false if absent).
    [[nodiscard]] RawJoystickState state(std::string_view guid) const;

    /// @brief Adds a virtual device (tests, demos). Returns its GUID.
    common::Result<std::string> attachVirtualDevice(const std::string& name, int axes, int buttons);
    /// @brief Moves an axis (−1 … 1) of a virtual device.
    void setVirtualAxis(const std::string& guid, int axis, double value);
    /// @brief Presses or releases a button of a virtual device.
    void setVirtualButton(const std::string& guid, int button, bool pressed);
    /// @brief Removes a virtual device.
    void detachVirtualDevice(const std::string& guid);

private:
    struct Impl;
    void run(std::promise<common::Result<void>> started);
    void post(std::function<void(Impl&)> command);

    std::unique_ptr<Impl> impl_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> version_{0};
    mutable std::mutex mutex_;
    std::vector<DeviceInfo> devices_;
    std::vector<RawJoystickState> states_;
    std::vector<std::function<void(Impl&)>> commands_;
};

}  // namespace ds::input
