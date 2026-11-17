// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// The built-in mission pack (assets/missions, M5): every mission is valid and, when playable
// in this build, sits sensibly on its own terrain — pads on level ground, beacons and gates
// above the hills, everything inside the terrain area.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <set>
#include <string>
#include <vector>

#include "ds/app/mission_dialogs.hpp"
#include "ds/app/mission_setup.hpp"
#include "ds/app/mission_validation.hpp"
#include "ds/missions/mission_loader.hpp"

namespace ds::app {
namespace {

std::filesystem::path assets() { return DS_TEST_ASSETS_DIR; }

std::vector<std::filesystem::path> packFiles() {
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(assets() / "missions")) {
        if (e.path().filename().string().ends_with(".mission.json")) {
            files.push_back(e.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

TEST(MissionPack, EveryMissionIsValidAndCoversTheObjectiveTypes) {
    auto validation = MissionValidation::create(assets(), {});
    ASSERT_TRUE(validation.has_value());
    const auto files = packFiles();
    ASSERT_GE(files.size(), 10U);
    std::set<missions::ObjectiveType> types;
    std::set<std::string> ids;
    std::set<std::string> chapters;
    int playable = 0;
    for (const auto& file : files) {
        const auto result = missions::loadMissionFile(file, (*validation)->context);
        ASSERT_TRUE(result.valid()) << file << "\n" << result.report();
        EXPECT_TRUE(result.warnings.empty()) << file << "\n" << result.report();
        const missions::MissionDefinition& m = *result.definition;
        EXPECT_TRUE(ids.insert(m.id).second) << "duplicate id " << m.id;
        EXPECT_FALSE(m.briefing.empty()) << m.id;
        chapters.insert(m.chapter);
        for (const auto& o : m.objectives) {
            types.insert(o.type);
        }
        if (!unplayableReason(m)) {
            ++playable;
        }
    }
    EXPECT_GE(playable, 10);
    EXPECT_GE(chapters.size(), 3U);
    // Every aerial objective type is in the pack (park is for ground vehicles, M6).
    for (const auto type : {missions::ObjectiveType::ReachAltitude,
                            missions::ObjectiveType::ReachWaypoint, missions::ObjectiveType::Hover,
                            missions::ObjectiveType::Land, missions::ObjectiveType::GateCourse,
                            missions::ObjectiveType::Deliver, missions::ObjectiveType::Inspect,
                            missions::ObjectiveType::Survey, missions::ObjectiveType::ReturnHome}) {
        EXPECT_TRUE(types.contains(type)) << missions::objectiveTypeName(type);
    }
}

TEST(MissionPack, PlayableMissionsSitSensiblyOnTheirTerrain) {
    auto validation = MissionValidation::create(assets(), {});
    ASSERT_TRUE(validation.has_value());
    for (const auto& file : packFiles()) {
        const auto result = missions::loadMissionFile(file, (*validation)->context);
        ASSERT_TRUE(result.valid()) << file;
        const missions::MissionDefinition& m = *result.definition;
        if (unplayableReason(m)) {
            continue;
        }
        SCOPED_TRACE(m.id);
        auto map = missionMapData(m);
        ASSERT_TRUE(map.has_value()) << map.error().message;
        const terrain::HeightSource& ground = *map->terrain;
        const auto inside = [&](const glm::dvec3& p) {
            return !map->extent || map->extent->contains(p.x, p.z);
        };
        EXPECT_TRUE(inside(map->spawn));
        for (const items::Item& item : map->items) {
            SCOPED_TRACE(item.key);
            const glm::dvec3 p = item.transform.position;
            EXPECT_TRUE(inside(p));
            const double h = ground.height(p.x, p.z);
            switch (item.type) {
                case items::ItemType::LandingPad: {
                    // Level: the ground under the pad's rim within 25 cm of its centre.
                    for (int k = 0; k < 8; ++k) {
                        const double a = k * std::numbers::pi / 4.0;
                        const double r = 2.0;
                        EXPECT_NEAR(ground.height(p.x + r * std::cos(a), p.z + r * std::sin(a)), h,
                                    0.25);
                    }
                    break;
                }
                case items::ItemType::Waypoint:
                case items::ItemType::Gate:
                    EXPECT_GT(p.y - h, 2.0) << "too close to the ground";
                    break;
                default: break;
            }
        }
    }
}

}  // namespace
}  // namespace ds::app
