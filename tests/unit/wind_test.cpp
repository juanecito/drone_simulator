// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Unit tests of the wind levels (FR-ENV-03) and the wind model (FR-PHY-07): table file,
// calm air, long-term mean, height profile, Dryden intensity, gusts and determinism.

#include "ds/physics/wind.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include "ds/common/json.hpp"
#include "ds/weather/wind_levels.hpp"

namespace {

using namespace ds;
using physics::Vec3;
using physics::WindModel;
using physics::WindParameters;
using weather::WindLevelTable;

const std::filesystem::path kLevelsFile =
    std::filesystem::path(DS_TEST_ASSETS_DIR) / "environment" / "wind_levels.json";

// --- Levels ----------------------------------------------------------------------------------

TEST(WindLevels, AssetFileMatchesBuiltInTable) {
    const auto table = WindLevelTable::load(kLevelsFile);
    ASSERT_TRUE(table.has_value()) << table.error().message;
    EXPECT_EQ(*table, WindLevelTable::builtIn());
}

TEST(WindLevels, TableIsOrderedAndCalmIsZero) {
    const WindLevelTable& t = WindLevelTable::builtIn();
    EXPECT_EQ(t.level(0).meanSpeedMs, 0.0);
    EXPECT_EQ(t.level(0).gustPeakMs, 0.0);
    EXPECT_EQ(t.level(0).turbulenceScale, 0.0);
    for (int i = 1; i < WindLevelTable::kLevelCount; ++i) {
        EXPECT_EQ(t.level(i).level, i);
        EXPECT_GT(t.level(i).meanSpeedMs, t.level(i - 1).meanSpeedMs);
        EXPECT_GE(t.level(i).gustPeakMs, t.level(i).meanSpeedMs);
        EXPECT_FALSE(t.level(i).name.empty());
    }
    EXPECT_EQ(t.level(-3).level, 0);   // Clamped.
    EXPECT_EQ(t.level(42).level, 10);  // Clamped.
    EXPECT_EQ(t.nearestLevel(6.5), 4);
    EXPECT_EQ(t.nearestLevel(0.2), 0);
    EXPECT_EQ(t.highestLevelAtMost(9.4), 5);
    EXPECT_EQ(t.highestLevelAtMost(9.3), 4);
    EXPECT_EQ(t.highestLevelWithGustsAtMost(13.0), 5);
    EXPECT_EQ(t.highestLevelWithGustsAtMost(12.9), 4);
}

TEST(WindLevels, RejectsInvalidFiles) {
    EXPECT_FALSE(WindLevelTable::fromJson(common::Json::array()).has_value());
    auto file = common::readJsonFile(kLevelsFile);
    ASSERT_TRUE(file.has_value());
    common::Json json = *file;
    json["levels"].erase(json["levels"].begin() + 3);  // Ten levels only.
    EXPECT_FALSE(WindLevelTable::fromJson(json).has_value());
    json = *common::readJsonFile(kLevelsFile);
    json["levels"][4]["meanSpeedMs"] = -1.0;
    EXPECT_FALSE(WindLevelTable::fromJson(json).has_value());
    EXPECT_FALSE(WindLevelTable::load("/nonexistent/wind_levels.json").has_value());
}

// --- Model -----------------------------------------------------------------------------------

WindModel modelForLevel(int level, std::uint64_t seed, double fromDeg = 270.0) {
    const weather::WindSettings settings{level, fromDeg, false};
    return {WindParameters::fromLevel(WindLevelTable::builtIn(), settings, seed), seed};
}

TEST(WindModel, CalmIsExactlyZero) {
    WindModel m = modelForLevel(0, 5);
    for (int i = 0; i < 10000; ++i) {
        m.update(0.01, 1.0 + i * 0.01, Vec3(5.0, 0.0, 0.0));
        ASSERT_EQ(m.velocity(), Vec3(0.0));
    }
}

TEST(WindModel, DirectionAndLogProfile) {
    // From the west (270°): blows towards the east (+x).
    WindModel m = modelForLevel(3, 1);
    m.update(0.0, 10.0, Vec3(0.0));
    EXPECT_GT(m.meanVelocity().x, 0.0);
    EXPECT_NEAR(m.meanVelocity().z, 0.0, 1e-9);
    // From the north (0°): blows towards the south (+z).
    WindModel north = modelForLevel(3, 1, 0.0);
    north.update(0.0, 10.0, Vec3(0.0));
    EXPECT_GT(north.meanVelocity().z, 0.0);

    EXPECT_DOUBLE_EQ(physics::logProfileSpeed(8.0, 10.0, 0.03), 8.0);
    EXPECT_NEAR(physics::logProfileSpeed(8.0, 2.0, 0.03),
                8.0 * std::log(2.0 / 0.03) / std::log(10.0 / 0.03), 1e-12);
    EXPECT_GT(physics::logProfileSpeed(8.0, 50.0, 0.03), 8.0);
    EXPECT_DOUBLE_EQ(physics::logProfileSpeed(8.0, 0.01, 0.03), 0.0);
    // Rougher ground slows the wind near the surface.
    EXPECT_LT(physics::logProfileSpeed(8.0, 2.0, 0.5), physics::logProfileSpeed(8.0, 2.0, 0.03));
}

TEST(WindModel, SixtySecondMeanMatchesLevel) {
    // FR-ENV-03: the 60 s mean at 10 m is the table value within ±5 % (ensemble of sessions,
    // gusts and turbulence included).
    for (const int level : {2, 4, 6}) {
        const double expected = WindLevelTable::builtIn().level(level).meanSpeedMs;
        double sum = 0.0;
        int n = 0;
        for (std::uint64_t seed = 1; seed <= 40; ++seed) {
            WindModel m = modelForLevel(level, seed);
            for (int i = 0; i < 6000; ++i) {
                m.update(0.01, 10.0, Vec3(0.0));
                sum += m.velocity().x;  // Along the mean direction (from the west).
                ++n;
            }
        }
        EXPECT_NEAR(sum / n, expected, 0.05 * expected) << "level " << level;
    }
}

TEST(WindModel, DrydenIntensityMatchesMilSpec) {
    WindParameters p;
    p.meanSpeed10mMs = 10.0;
    p.turbulenceScale = 1.0;
    p.fromDeg = 270.0;
    WindModel m(p, 99);
    // MIL-F-8785C low altitude at h = 10 m (32.8 ft): σ_w = 0.1·W20, σ_u = σ_w/(0.177 +
    // 0.000823·h)^0.4.
    const double w20 = physics::logProfileSpeed(10.0, 6.096, p.roughnessM);
    const double sigmaW = 0.1 * w20;
    const double hFt = 10.0 / 0.3048;
    const double sigmaU = sigmaW / std::pow(0.177 + 0.000823 * hFt, 0.4);
    double su = 0.0;
    double suu = 0.0;
    double sv = 0.0;
    double svv = 0.0;
    double sw = 0.0;
    double sww = 0.0;
    const int n = 400000;
    for (int i = 0; i < n; ++i) {
        m.update(0.01, 10.0, Vec3(0.0));
        const Vec3 t = m.velocity() - m.meanVelocity();
        su += t.x;
        suu += t.x * t.x;
        sv += t.z;
        svv += t.z * t.z;
        sw += t.y;
        sww += t.y * t.y;
    }
    const auto sd = [n](double s, double ss) { return std::sqrt(ss / n - (s / n) * (s / n)); };
    EXPECT_NEAR(sd(su, suu), sigmaU, 0.12 * sigmaU);
    EXPECT_NEAR(sd(sv, svv), sigmaU, 0.12 * sigmaU);
    EXPECT_NEAR(sd(sw, sww), sigmaW, 0.12 * sigmaW);
}

TEST(WindModel, GustsReachThePeakAndAreDeterministic) {
    const double peak = WindLevelTable::builtIn().level(5).gustPeakMs;
    WindModel a = modelForLevel(5, 11);
    WindModel b = modelForLevel(5, 11);
    WindModel c = modelForLevel(5, 12);
    bool sawGust = false;
    bool differs = false;
    double maxSpeed = 0.0;
    for (int i = 0; i < 30000; ++i) {
        a.update(0.01, 10.0, Vec3(0.0));
        b.update(0.01, 10.0, Vec3(0.0));
        c.update(0.01, 10.0, Vec3(0.0));
        ASSERT_EQ(a.velocity(), b.velocity());
        differs = differs || a.velocity() != c.velocity();
        sawGust = sawGust || a.inGust();
        maxSpeed = std::max(maxSpeed, glm::length(a.velocity()));
    }
    EXPECT_TRUE(sawGust);
    EXPECT_TRUE(differs);
    EXPECT_GT(maxSpeed, 0.85 * peak);
    EXPECT_LT(maxSpeed, 1.6 * peak);
}

TEST(WindModel, RandomDirectionDependsOnSeedAndWanderIsBounded) {
    const weather::WindSettings random{4, 270.0, true};
    const auto p1 = WindParameters::fromLevel(WindLevelTable::builtIn(), random, 1);
    const auto p1b = WindParameters::fromLevel(WindLevelTable::builtIn(), random, 1);
    EXPECT_EQ(p1.fromDeg, p1b.fromDeg);
    int distinct = 0;
    for (std::uint64_t s = 2; s < 10; ++s) {
        distinct +=
            WindParameters::fromLevel(WindLevelTable::builtIn(), random, s).fromDeg != p1.fromDeg;
    }
    EXPECT_GE(distinct, 6);

    WindModel m = modelForLevel(6, 3, 90.0);
    const double swing = WindLevelTable::builtIn().level(6).directionSwingDeg;
    for (int i = 0; i < 60000; ++i) {
        m.update(0.01, 10.0, Vec3(0.0));
        double d = m.currentFromDeg() - 90.0;
        ASSERT_LE(std::abs(d), swing + 1e-9);
    }
}

}  // namespace
