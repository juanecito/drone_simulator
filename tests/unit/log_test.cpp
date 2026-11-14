// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/common/log.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

using ds::log::Level;

// Captures log output and restores the default sink and threshold afterwards.
class LogTest : public ::testing::Test {
protected:
    void SetUp() override {
        previous_ = ds::log::level();
        ds::log::setSink([this](Level level, std::string_view message) {
            records_.emplace_back(level, std::string(message));
        });
    }
    void TearDown() override {
        ds::log::setSink({});
        ds::log::setLevel(previous_);
    }

    std::vector<std::pair<Level, std::string>> records_;

private:
    Level previous_ = Level::Info;
};

TEST_F(LogTest, FormatsArguments) {
    ds::log::setLevel(Level::Trace);
    ds::log::info("value {} of {}", 3, "four");
    ASSERT_EQ(records_.size(), 1U);
    EXPECT_EQ(records_[0].first, Level::Info);
    EXPECT_EQ(records_[0].second, "value 3 of four");
}

TEST_F(LogTest, FiltersBelowThreshold) {
    ds::log::setLevel(Level::Warn);
    ds::log::debug("hidden");
    ds::log::info("hidden");
    ds::log::warn("shown");
    ds::log::error("shown too");
    ASSERT_EQ(records_.size(), 2U);
    EXPECT_EQ(records_[0].first, Level::Warn);
    EXPECT_EQ(records_[1].first, Level::Error);
}

TEST_F(LogTest, OffDisablesEverything) {
    ds::log::setLevel(Level::Off);
    ds::log::error("nothing");
    EXPECT_TRUE(records_.empty());
    EXPECT_FALSE(ds::log::isEnabled(Level::Error));
}

TEST_F(LogTest, OffIsNeverAMessageLevel) {
    ds::log::setLevel(Level::Trace);
    ds::log::write(Level::Off, "never");
    EXPECT_TRUE(records_.empty());
}

TEST(LogLevel, NamesRoundTrip) {
    for (const Level level :
         {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Off}) {
        const auto parsed = ds::log::parseLevel(ds::log::toString(level));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, level);
    }
}

TEST(LogLevel, RejectsUnknownNames) {
    EXPECT_FALSE(ds::log::parseLevel("loud").has_value());
    EXPECT_FALSE(ds::log::parseLevel("INFO").has_value());
    EXPECT_FALSE(ds::log::parseLevel("").has_value());
}

}  // namespace
