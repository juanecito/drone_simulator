// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/command_line.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

using ds::app::parseCommandLine;
using ds::app::ParseMode;
using ds::log::Level;

auto parse(std::vector<std::string> args, ParseMode mode = ParseMode::Strict) {
    args.insert(args.begin(), "drone_simulator");
    return parseCommandLine(args, mode);
}

TEST(CommandLine, NoArgumentsGivesDefaults) {
    const auto options = parse({});
    ASSERT_TRUE(options.has_value());
    EXPECT_FALSE(options->showHelp);
    EXPECT_FALSE(options->showVersion);
    EXPECT_FALSE(options->glDebug);
    EXPECT_FALSE(options->logLevel.has_value());
}

TEST(CommandLine, RecognisesFlags) {
    const auto options = parse({"--help", "-v", "--gl-debug"});
    ASSERT_TRUE(options.has_value());
    EXPECT_TRUE(options->showHelp);
    EXPECT_TRUE(options->showVersion);
    EXPECT_TRUE(options->glDebug);
}

TEST(CommandLine, LogLevelAsSeparateValue) {
    const auto options = parse({"--log-level", "debug"});
    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(options->logLevel, Level::Debug);
}

TEST(CommandLine, LogLevelWithEquals) {
    const auto options = parse({"--log-level=warn"});
    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(options->logLevel, Level::Warn);
}

TEST(CommandLine, InvalidLogLevelIsAnErrorInBothModes) {
    EXPECT_FALSE(parse({"--log-level", "loud"}).has_value());
    EXPECT_FALSE(parse({"--log-level=loud"}, ParseMode::Lenient).has_value());
}

TEST(CommandLine, MissingLogLevelValueIsAnError) {
    const auto options = parse({"--log-level"});
    ASSERT_FALSE(options.has_value());
    EXPECT_NE(options.error().message.find("needs a value"), std::string::npos);
}

TEST(CommandLine, StrictRejectsUnknownOptionsAndArguments) {
    EXPECT_FALSE(parse({"--frobnicate"}).has_value());
    EXPECT_FALSE(parse({"file.txt"}).has_value());
    EXPECT_FALSE(parse({"--log-levelx=info"}).has_value());
}

TEST(CommandLine, DatabasePath) {
    EXPECT_FALSE(parse({})->databaseFile.has_value());
    EXPECT_EQ(parse({"--db", "/tmp/a b.db"})->databaseFile, std::filesystem::path("/tmp/a b.db"));
    EXPECT_EQ(parse({"--db=test.db"})->databaseFile, std::filesystem::path("test.db"));
    EXPECT_EQ(parse({"--db", "x.db"}, ParseMode::Lenient)->databaseFile,
              std::filesystem::path("x.db"));
    EXPECT_FALSE(parse({"--db"}).has_value());
    EXPECT_FALSE(parse({"--db="}).has_value());
}

TEST(CommandLine, FlagsDoNotTakeValues) {
    EXPECT_FALSE(parse({"--gl-debug=yes"}).has_value());
    EXPECT_TRUE(parse({"--style=fusion"}, ParseMode::Lenient).has_value());
}

TEST(CommandLine, LenientIgnoresQtOptions) {
    const auto options = parse({"-platform", "offscreen", "--gl-debug"}, ParseMode::Lenient);
    ASSERT_TRUE(options.has_value());
    EXPECT_TRUE(options->glDebug);
}

TEST(CommandLine, HelpTextListsOptions) {
    const std::string text = ds::app::helpText();
    for (const char* option : {"--help", "--version", "--log-level", "--gl-debug", "--db"}) {
        EXPECT_NE(text.find(option), std::string::npos) << option;
    }
}

}  // namespace
