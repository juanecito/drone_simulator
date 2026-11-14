// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/weather/wind_levels.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace ds::weather {

const WindLevelTable& WindLevelTable::builtIn() {
    static const WindLevelTable kTable = [] {
        WindLevelTable t;
        t.levels_ = {{
            {0, "Calm", 0.0, 0.0, 0.0, "none", 0.0, 0.0},
            {1, "Light air", 1.0, 1.5, 30.0, "very light", 0.5, 30.0},
            {2, "Light breeze", 2.5, 3.5, 28.0, "light", 0.7, 25.0},
            {3, "Gentle breeze", 4.5, 6.5, 25.0, "light", 0.8, 20.0},
            {4, "Moderate breeze", 6.7, 9.5, 23.0, "moderate", 1.0, 20.0},
            {5, "Fresh breeze", 9.4, 13.0, 20.0, "moderate", 1.0, 15.0},
            {6, "Strong breeze", 12.3, 17.0, 18.0, "moderate-severe", 1.15, 15.0},
            {7, "Near gale", 15.5, 21.0, 15.0, "severe", 1.3, 15.0},
            {8, "Gale", 19.0, 26.0, 13.0, "severe", 1.3, 10.0},
            {9, "Strong gale", 22.6, 31.0, 10.0, "severe", 1.3, 10.0},
            {10, "Storm", 26.5, 36.0, 8.0, "extreme", 1.5, 10.0},
        }};
        return t;
    }();
    return kTable;
}

common::Result<WindLevelTable> WindLevelTable::fromJson(const common::Json& json) {
    common::JsonReader root(json);
    const int schema = root.integer("schemaVersion", 1, 1);
    (void)schema;
    const std::size_t count = root.array("levels");
    WindLevelTable table;
    std::array<bool, kLevelCount> seen{};
    for (std::size_t i = 0; i < count; ++i) {
        common::JsonReader e = root.element("levels", i);
        WindLevel l;
        l.level = e.integer("level", 0, kLevelCount - 1);
        l.name = e.string("name");
        l.meanSpeedMs = e.number("meanSpeedMs", 0.0, 100.0);
        l.gustPeakMs = e.number("gustPeakMs", 0.0, 150.0);
        l.gustIntervalS = e.number("gustIntervalS", 0.0, 3600.0);
        l.turbulence = e.string("turbulence");
        l.turbulenceScale = e.number("turbulenceScale", 0.0, 5.0);
        l.directionSwingDeg = e.number("directionSwingDeg", 0.0, 180.0);
        if (l.gustPeakMs < l.meanSpeedMs) {
            e.fail("gustPeakMs", "must not be below meanSpeedMs");
        }
        const auto index = static_cast<std::size_t>(l.level);
        if (seen.at(index)) {
            e.fail("level", "appears more than once");
        }
        seen.at(index) = true;
        table.levels_.at(index) = l;
    }
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (!seen.at(i)) {
            root.fail("levels", std::format("level {} is missing", i));
        }
    }
    const WindLevel& calm = table.levels_[0];
    if (seen[0] && (calm.meanSpeedMs != 0.0 || calm.gustPeakMs != 0.0 ||
                    calm.turbulenceScale != 0.0 || calm.directionSwingDeg != 0.0)) {
        root.fail("levels", "level 0 must be calm (no wind, gusts or turbulence)");
    }
    for (std::size_t i = 1; i < table.levels_.size(); ++i) {
        if (table.levels_.at(i).meanSpeedMs < table.levels_.at(i - 1).meanSpeedMs) {
            root.fail("levels", "mean speeds must not decrease with the level");
            break;
        }
    }
    if (!root.ok()) {
        return common::makeError("wind levels: " + root.errorText());
    }
    return table;
}

common::Result<WindLevelTable> WindLevelTable::load(const std::filesystem::path& file) {
    auto json = common::readJsonFile(file, std::size_t{1} << 20U);
    if (!json) {
        return std::unexpected(json.error());
    }
    return fromJson(*json);
}

const WindLevel& WindLevelTable::level(int level) const noexcept {
    return levels_[static_cast<std::size_t>(std::clamp(level, 0, kLevelCount - 1))];
}

int WindLevelTable::nearestLevel(double meanSpeedMs) const noexcept {
    int best = 0;
    double bestDiff = 1e300;
    for (const auto& l : levels_) {
        const double diff = std::abs(l.meanSpeedMs - meanSpeedMs);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = l.level;
        }
    }
    return best;
}

int WindLevelTable::highestLevelAtMost(double speedMs) const noexcept {
    int best = 0;
    for (const auto& l : levels_) {
        if (l.meanSpeedMs <= speedMs) {
            best = std::max(best, l.level);
        }
    }
    return best;
}

int WindLevelTable::highestLevelWithGustsAtMost(double speedMs) const noexcept {
    int best = 0;
    for (const auto& l : levels_) {
        if (l.gustPeakMs <= speedMs) {
            best = std::max(best, l.level);
        }
    }
    return best;
}

}  // namespace ds::weather
