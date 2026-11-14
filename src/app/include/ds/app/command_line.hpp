// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "ds/common/error.hpp"
#include "ds/common/log.hpp"

/// @file
/// @brief Command-line options of the application (FR-X-03, subset implemented so far).
/// @ingroup app

namespace ds::app {

/// @brief Options recognised on the command line.
struct CommandLine {
    bool showHelp = false;               ///< `-h`, `--help`.
    bool showVersion = false;            ///< `-v`, `--version`.
    bool glDebug = false;                ///< `--gl-debug`: request an OpenGL debug context.
    std::optional<log::Level> logLevel;  ///< `--log-level <trace|debug|info|warn|error|off>`.
    /// `--db <path>`: database file to use instead of the per-user default (FR-X-03).
    std::optional<std::filesystem::path> databaseFile;
};

/// @brief How strictly unknown arguments are treated.
enum class ParseMode {
    /// Unknown arguments are ignored. Used before `QApplication` exists, because Qt's own
    /// options (`-platform`, `-style`, …) are still in the list.
    Lenient,
    /// Unknown options and positional arguments are errors. Used on
    /// `QCoreApplication::arguments()`, from which Qt has already removed its options.
    Strict
};

/// @brief Parses @p args (including the program name at index 0).
/// @return The options, or an error with a user-readable message.
[[nodiscard]] common::Result<CommandLine> parseCommandLine(std::span<const std::string> args,
                                                           ParseMode mode);

/// @brief Text printed by `--help`.
[[nodiscard]] std::string helpText();

}  // namespace ds::app
