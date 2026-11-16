// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/command_line.hpp"

#include <filesystem>
#include <format>
#include <optional>
#include <string_view>

#include "ds/common/version.hpp"

namespace ds::app {
namespace {

common::Result<log::Level> parseLevelValue(std::string_view value) {
    if (const auto level = log::parseLevel(value)) {
        return *level;
    }
    return common::makeError(std::format(
        "invalid log level '{}' (expected trace, debug, info, warn, error or off)", value));
}

}  // namespace

common::Result<CommandLine> parseCommandLine(std::span<const std::string> args, ParseMode mode) {
    CommandLine options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];

        // Options with a value accept both "--name value" and "--name=value".
        std::optional<std::string_view> inlineValue;
        std::string_view name = arg;
        if (arg.starts_with("--")) {
            if (const auto eq = arg.find('='); eq != std::string_view::npos) {
                name = arg.substr(0, eq);
                inlineValue = arg.substr(eq + 1);
            }
        }
        const auto takeValue = [&]() -> common::Result<std::string_view> {
            if (inlineValue) {
                return *inlineValue;
            }
            if (i + 1 >= args.size()) {
                return common::makeError(std::format("option '{}' needs a value", name));
            }
            return std::string_view(args[++i]);
        };

        if (name == "--log-level") {
            const auto value = takeValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            auto level = parseLevelValue(*value);
            if (!level) {
                return std::unexpected(level.error());
            }
            options.logLevel = *level;
        } else if (name == "--db") {
            const auto value = takeValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            if (value->empty()) {
                return common::makeError("option '--db' needs a non-empty path");
            }
            options.databaseFile = std::filesystem::path(std::string(*value));
        } else if (name == "--validate-mission") {
            const auto value = takeValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            if (value->empty()) {
                return common::makeError("option '--validate-mission' needs a file");
            }
            options.validateMissions.emplace_back(std::string(*value));
        } else if (inlineValue && mode == ParseMode::Strict) {
            return common::makeError(std::format("option '{}' does not take a value", name));
        } else if (inlineValue) {
            continue;  // Lenient: possibly a Qt option.
        } else if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
        } else if (arg == "-v" || arg == "--version") {
            options.showVersion = true;
        } else if (arg == "--gl-debug") {
            options.glDebug = true;
        } else if (mode == ParseMode::Strict) {
            if (arg.starts_with('-')) {
                return common::makeError(std::format("unknown option '{}'", arg));
            }
            return common::makeError(std::format("unexpected argument '{}'", arg));
        }
    }
    return options;
}

std::string helpText() {
    return std::format(
        "Usage: drone_simulator [options]\n"
        "\n"
        "{} {} — realistic 3D drone simulator.\n"
        "\n"
        "Options:\n"
        "  -h, --help               Show this help and exit.\n"
        "  -v, --version            Show the version and exit.\n"
        "      --log-level <level>  Log threshold: trace, debug, info (default), warn, error, "
        "off.\n"
        "      --gl-debug           Request an OpenGL debug context and log driver messages\n"
        "                           (always on in Debug builds).\n"
        "      --db <path>          Use this database file instead of\n"
        "                           ~/.drone_simulation/drone_simulation.db (for testing).\n"
        "      --validate-mission <file>\n"
        "                           Validate a mission file (.json), print its errors and\n"
        "                           exit (0 = valid, 1 = invalid). May be repeated.\n"
        "\n"
        "Standard Qt options (for example -platform, -style) are also accepted.\n",
        common::kApplicationName, common::kVersion);
}

}  // namespace ds::app
