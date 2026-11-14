// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <format>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

/// @file
/// @brief Minimal thread-safe levelled logger (FR-X-01). File output and rotation come later.
/// @ingroup common

namespace ds::log {

/// @brief Log severity, from most to least verbose.
enum class Level : int {
    Trace = 0,  ///< Very detailed diagnostics.
    Debug,      ///< Developer diagnostics.
    Info,       ///< Normal operational messages (default threshold).
    Warn,       ///< Something unexpected that the application recovered from.
    Error,      ///< An operation failed.
    Off         ///< Disables all output when used as threshold.
};

/// @brief Receives every message at or above the current threshold.
using Sink = std::function<void(Level, std::string_view)>;

/// @brief Sets the minimum level that is written. Thread-safe.
void setLevel(Level threshold) noexcept;

/// @brief Returns the current minimum level.
[[nodiscard]] Level level() noexcept;

/// @brief Returns true if a message of @p severity would be written.
[[nodiscard]] bool isEnabled(Level severity) noexcept;

/// @brief Replaces the output sink; pass an empty Sink to restore the default (stderr).
void setSink(Sink sink);

/// @brief Writes @p message at @p severity if it passes the threshold. Thread-safe.
void write(Level severity, std::string_view message);

/// @brief Lower-case name of a level ("trace", "debug", "info", "warn", "error", "off").
[[nodiscard]] std::string_view toString(Level severity) noexcept;

/// @brief Parses a level name as produced by toString(); returns nullopt for unknown names.
[[nodiscard]] std::optional<Level> parseLevel(std::string_view name) noexcept;

/// @brief Formats and writes a message at @p severity (formatting is skipped when disabled).
template <typename... Args>
void log(Level severity, std::format_string<Args...> fmt, Args&&... args) {
    if (isEnabled(severity)) {
        write(severity, std::format(fmt, std::forward<Args>(args)...));
    }
}

/// @brief Formats and writes a Trace message.
template <typename... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    log(Level::Trace, fmt, std::forward<Args>(args)...);
}

/// @brief Formats and writes a Debug message.
template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    log(Level::Debug, fmt, std::forward<Args>(args)...);
}

/// @brief Formats and writes an Info message.
template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    log(Level::Info, fmt, std::forward<Args>(args)...);
}

/// @brief Formats and writes a Warn message.
template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    log(Level::Warn, fmt, std::forward<Args>(args)...);
}

/// @brief Formats and writes an Error message.
template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    log(Level::Error, fmt, std::forward<Args>(args)...);
}

}  // namespace ds::log
