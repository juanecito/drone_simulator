// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/input/joystick_service.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <map>

#include "ds/common/log.hpp"

#include <SDL3/SDL.h>

namespace ds::input {
namespace {

constexpr auto kPollPeriod = std::chrono::milliseconds(2);

void sdlLog(void* /*user*/, int /*category*/, SDL_LogPriority priority, const char* message) {
    switch (priority) {
        case SDL_LOG_PRIORITY_ERROR:
        case SDL_LOG_PRIORITY_CRITICAL: log::error("SDL: {}", message); break;
        case SDL_LOG_PRIORITY_WARN: log::warn("SDL: {}", message); break;
        default: log::debug("SDL: {}", message); break;
    }
}

std::string guidText(SDL_GUID guid) {
    char buffer[33] = {};
    SDL_GUIDToString(guid, buffer, sizeof(buffer));
    return buffer;
}

double normaliseAxis(Sint16 value) {
    return std::clamp(static_cast<double>(value) / 32767.0, -1.0, 1.0);
}

}  // namespace

struct JoystickService::Impl {
    struct Open {
        SDL_Joystick* joystick = nullptr;
        SDL_Gamepad* gamepad = nullptr;
        DeviceInfo info;
        bool isVirtual = false;
    };
    std::map<SDL_JoystickID, Open> open;

    void add(SDL_JoystickID id) {
        if (open.contains(id)) {
            return;
        }
        Open o;
        if (SDL_IsGamepad(id)) {
            o.gamepad = SDL_OpenGamepad(id);
            o.joystick = o.gamepad != nullptr ? SDL_GetGamepadJoystick(o.gamepad) : nullptr;
        }
        if (o.joystick == nullptr) {
            o.gamepad = nullptr;
            o.joystick = SDL_OpenJoystick(id);
        }
        if (o.joystick == nullptr) {
            log::warn("Cannot open joystick {}: {}", id, SDL_GetError());
            return;
        }
        const char* name = SDL_GetJoystickName(o.joystick);
        o.info.name = name != nullptr ? name : "Joystick";
        o.info.guid = guidText(SDL_GetJoystickGUID(o.joystick));
        o.info.gamepad = o.gamepad != nullptr;
        o.info.axes =
            o.gamepad != nullptr ? SDL_GAMEPAD_AXIS_COUNT : SDL_GetNumJoystickAxes(o.joystick);
        o.info.buttons =
            o.gamepad != nullptr ? SDL_GAMEPAD_BUTTON_COUNT : SDL_GetNumJoystickButtons(o.joystick);
        log::info("Input device connected: {} ({} axes, {} buttons{})", o.info.name, o.info.axes,
                  o.info.buttons, o.info.gamepad ? ", gamepad" : "");
        open.emplace(id, o);
    }

    void remove(SDL_JoystickID id) {
        const auto it = open.find(id);
        if (it == open.end()) {
            return;
        }
        log::info("Input device disconnected: {}", it->second.info.name);
        if (it->second.gamepad != nullptr) {
            SDL_CloseGamepad(it->second.gamepad);
        } else {
            SDL_CloseJoystick(it->second.joystick);
        }
        open.erase(it);
    }

    Open* byGuid(std::string_view guid) {
        for (auto& [id, o] : open) {
            if (o.info.guid == guid) {
                return &o;
            }
        }
        return nullptr;
    }

    static RawJoystickState read(const Open& o) {
        RawJoystickState s;
        s.connected = true;
        s.axes.resize(static_cast<std::size_t>(std::max(o.info.axes, 0)));
        s.buttons.resize(static_cast<std::size_t>(std::max(o.info.buttons, 0)));
        for (std::size_t i = 0; i < s.axes.size(); ++i) {
            const int a = static_cast<int>(i);
            s.axes[i] =
                normaliseAxis(o.gamepad != nullptr
                                  ? SDL_GetGamepadAxis(o.gamepad, static_cast<SDL_GamepadAxis>(a))
                                  : SDL_GetJoystickAxis(o.joystick, a));
        }
        for (std::size_t i = 0; i < s.buttons.size(); ++i) {
            const int b = static_cast<int>(i);
            s.buttons[i] = o.gamepad != nullptr
                               ? SDL_GetGamepadButton(o.gamepad, static_cast<SDL_GamepadButton>(b))
                               : SDL_GetJoystickButton(o.joystick, b);
        }
        return s;
    }
};

JoystickService::JoystickService() : impl_(std::make_unique<Impl>()) {}

JoystickService::~JoystickService() { stop(); }

common::Result<void> JoystickService::start() {
    if (running_.load()) {
        return {};
    }
    stop_ = false;
    std::promise<common::Result<void>> started;
    auto future = started.get_future();
    thread_ = std::thread([this, p = std::move(started)]() mutable { run(std::move(p)); });
    auto result = future.get();
    if (!result) {
        thread_.join();
    }
    return result;
}

void JoystickService::stop() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void JoystickService::post(std::function<void(Impl&)> command) {
    const std::scoped_lock lock(mutex_);
    commands_.push_back(std::move(command));
}

void JoystickService::run(std::promise<common::Result<void>> started) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetLogOutputFunction(&sdlLog, nullptr);
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        started.set_value(common::makeError(std::string("Joystick support is unavailable (SDL: ") +
                                            SDL_GetError() + ")"));
        return;
    }
    running_ = true;
    started.set_value({});

    auto publish = [this] {
        std::vector<DeviceInfo> devices;
        std::vector<RawJoystickState> states;
        for (const auto& [id, o] : impl_->open) {
            devices.push_back(o.info);
            states.push_back(Impl::read(o));
        }
        const std::scoped_lock lock(mutex_);
        const bool changed = devices != devices_;
        devices_ = std::move(devices);
        states_ = std::move(states);
        if (changed) {
            ++version_;
        }
    };

    // Devices already present.
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&count); ids != nullptr) {
        for (int i = 0; i < count; ++i) {
            impl_->add(ids[i]);
        }
        SDL_free(ids);
    }
    publish();

    auto next = std::chrono::steady_clock::now();
    while (!stop_.load()) {
        std::vector<std::function<void(Impl&)>> commands;
        {
            const std::scoped_lock lock(mutex_);
            commands.swap(commands_);
        }
        for (auto& c : commands) {
            c(*impl_);
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_JOYSTICK_ADDED) {
                impl_->add(event.jdevice.which);
            } else if (event.type == SDL_EVENT_JOYSTICK_REMOVED) {
                impl_->remove(event.jdevice.which);
            }
        }
        SDL_UpdateJoysticks();
        publish();
        next += kPollPeriod;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) {
            next = now;
        }
        std::this_thread::sleep_until(next);
    }

    for (auto it = impl_->open.begin(); it != impl_->open.end();) {
        const SDL_JoystickID id = it->first;
        const bool isVirtual = it->second.isVirtual;
        ++it;
        impl_->remove(id);
        if (isVirtual) {
            SDL_DetachVirtualJoystick(id);
        }
    }
    {
        const std::scoped_lock lock(mutex_);
        devices_.clear();
        states_.clear();
        ++version_;
    }
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);
    SDL_Quit();
    SDL_SetLogOutputFunction(nullptr, nullptr);
    running_ = false;
}

std::vector<DeviceInfo> JoystickService::devices() const {
    const std::scoped_lock lock(mutex_);
    return devices_;
}

RawJoystickState JoystickService::state(std::string_view guid) const {
    const std::scoped_lock lock(mutex_);
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].guid == guid) {
            return states_[i];
        }
    }
    return {};
}

common::Result<std::string> JoystickService::attachVirtualDevice(const std::string& name, int axes,
                                                                 int buttons) {
    if (!running_.load()) {
        return common::makeError("the joystick service is not running");
    }
    auto promise = std::make_shared<std::promise<common::Result<std::string>>>();
    auto future = promise->get_future();
    post([promise, name, axes, buttons](Impl& impl) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = static_cast<Uint16>(axes);
        desc.nbuttons = static_cast<Uint16>(buttons);
        desc.name = name.c_str();
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        if (id == 0) {
            promise->set_value(common::makeError(std::string("SDL: ") + SDL_GetError()));
            return;
        }
        // Open it as a plain joystick so the raw axes keep their order.
        Impl::Open o;
        o.joystick = SDL_OpenJoystick(id);
        if (o.joystick == nullptr) {
            SDL_DetachVirtualJoystick(id);
            promise->set_value(common::makeError(std::string("SDL: ") + SDL_GetError()));
            return;
        }
        o.isVirtual = true;
        o.info.name = name;
        o.info.guid = guidText(SDL_GetJoystickGUID(o.joystick)) + "-" + std::to_string(id);
        o.info.axes = axes;
        o.info.buttons = buttons;
        impl.open.emplace(id, o);
        promise->set_value(o.info.guid);
    });
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        return common::makeError("timed out attaching a virtual joystick");
    }
    auto guid = future.get();
    if (guid) {
        // Wait until the device appears in the published list.
        for (int i = 0; i < 500 && state(*guid).axes.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    return guid;
}

void JoystickService::setVirtualAxis(const std::string& guid, int axis, double value) {
    post([guid, axis, value](Impl& impl) {
        if (Impl::Open* o = impl.byGuid(guid); o != nullptr && o->isVirtual) {
            SDL_SetJoystickVirtualAxis(
                o->joystick, axis,
                static_cast<Sint16>(std::lround(std::clamp(value, -1.0, 1.0) * 32767.0)));
        }
    });
}

void JoystickService::setVirtualButton(const std::string& guid, int button, bool pressed) {
    post([guid, button, pressed](Impl& impl) {
        if (Impl::Open* o = impl.byGuid(guid); o != nullptr && o->isVirtual) {
            SDL_SetJoystickVirtualButton(o->joystick, button, pressed);
        }
    });
}

void JoystickService::detachVirtualDevice(const std::string& guid) {
    post([guid](Impl& impl) {
        for (auto& [id, o] : impl.open) {
            if (o.info.guid == guid && o.isVirtual) {
                const SDL_JoystickID joystickId = id;
                impl.remove(joystickId);
                SDL_DetachVirtualJoystick(joystickId);
                return;
            }
        }
    });
}

}  // namespace ds::input
