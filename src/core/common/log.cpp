// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/common/log.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <print>

namespace ds::log {
namespace {

std::atomic<Level> gThreshold{Level::Info};
std::mutex gSinkMutex;
Sink gSink;  // Empty ⇒ default stderr sink. Guarded by gSinkMutex.

// Local time zone, or nullptr (UTC) if the time-zone database is unavailable.
const std::chrono::time_zone* localZone() noexcept {
    static const std::chrono::time_zone* const zone = []() noexcept {
        try {
            return std::chrono::current_zone();
        } catch (...) {
            return static_cast<const std::chrono::time_zone*>(nullptr);
        }
    }();
    return zone;
}

void writeToStderr(Level severity, std::string_view message) {
    const auto now =
        std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    if (const auto* zone = localZone(); zone != nullptr) {
        std::println(stderr, "[{:%T}] [{}] {}", zone->to_local(now), toString(severity), message);
    } else {
        std::println(stderr, "[{:%T}Z] [{}] {}", now, toString(severity), message);
    }
}

constexpr std::array<std::string_view, 6> kNames{"trace", "debug", "info", "warn", "error", "off"};

}  // namespace

void setLevel(Level threshold) noexcept { gThreshold.store(threshold, std::memory_order_relaxed); }

Level level() noexcept { return gThreshold.load(std::memory_order_relaxed); }

bool isEnabled(Level severity) noexcept { return severity != Level::Off && severity >= level(); }

void setSink(Sink sink) {
    const std::scoped_lock lock(gSinkMutex);
    gSink = std::move(sink);
}

void write(Level severity, std::string_view message) {
    if (!isEnabled(severity)) {
        return;
    }
    const std::scoped_lock lock(gSinkMutex);
    if (gSink) {
        gSink(severity, message);
    } else {
        writeToStderr(severity, message);
    }
}

std::string_view toString(Level severity) noexcept {
    const auto index = static_cast<std::size_t>(severity);
    return index < kNames.size() ? kNames[index] : std::string_view{"?"};
}

std::optional<Level> parseLevel(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (kNames[i] == name) {
            return static_cast<Level>(i);
        }
    }
    return std::nullopt;
}

}  // namespace ds::log
