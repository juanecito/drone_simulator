// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Missions tab: list, grouping, campaign locks, progress, details and reload (§4.11, FR-MIS-12).

#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTreeWidget>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "ds/app/main_window.hpp"
#include "ds/app/missions_controller.hpp"
#include "ds/app/missions_panel.hpp"
#include "ds/common/json.hpp"
#include "ds/storage/mission_store.hpp"
#include "ds/storage/storage_service.hpp"
#include "test_assets.hpp"

namespace {

namespace fs = std::filesystem;
using ds::app::MissionsController;
using ds::app::MissionsPanel;
using ds::app::MissionStatus;
using ds::common::Json;

const fs::path kAssets = DS_TEST_ASSETS_DIR;

Json defaultMission() {
    std::ifstream in(kAssets / "default-missions" / "first-flight.mission.json");
    return Json::parse(in);
}

void writeMission(const fs::path& file, const char* id, const char* title, const char* chapter,
                  int order) {
    Json m = defaultMission();
    m["id"] = id;
    m["title"] = title;
    if (*chapter != '\0') {
        m["chapter"] = chapter;
    }
    m["order"] = order;
    std::ofstream(file) << m.dump(2);
}

struct Fixture {
    QTemporaryDir home;
    std::unique_ptr<ds::storage::StorageService> storage;
    std::unique_ptr<MissionsController> missions;

    Fixture() {
        auto s = ds::storage::StorageService::start({.databaseFile = std::nullopt});
        EXPECT_TRUE(s.has_value());
        storage = std::move(*s);
        missions = std::make_unique<MissionsController>(
            ds::app::MissionFolders{
                .assetDir = ds::test::assetsWithoutMissionPack(home.path().toStdString()),
                .userDir = userDir(),
                .libraryDir = {}},
            storage.get());
    }
    [[nodiscard]] fs::path userDir() const {
        return fs::path(home.path().toStdString()) / "missions";
    }
};

QTreeWidgetItem* findItem(QTreeWidget* tree, const QString& title) {
    const auto found = tree->findItems(title, Qt::MatchExactly | Qt::MatchRecursive);
    return found.isEmpty() ? nullptr : found.front();
}

TEST(MissionsPanel, ListsTheDefaultMissionWithItsDetails) {
    Fixture f;
    MissionsPanel panel(*f.missions);
    auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("MissionTree"));
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("MissionSummary"))->text(),
              QStringLiteral("1 mission, 1 available"));
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("Free missions"));
    QTreeWidgetItem* item = findItem(tree, QStringLiteral("First flight"));
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->text(1), QStringLiteral("Aerial"));
    EXPECT_EQ(item->text(2), QStringLiteral("—"));
    EXPECT_EQ(item->text(4), QStringLiteral("Available"));

    EXPECT_EQ(panel.selectedMission(), "first-flight");  // Selected by default.
    const QString details =
        panel.findChild<QTextBrowser*>(QStringLiteral("MissionDetails"))->toPlainText();
    for (const char* text :
         {"First flight", "Climb to 10 m", "Fly to the beacon", "Land on the home pad",
          "Procedural, flatlands", "Europe/Madrid", "Wind 270° level 1 (4 km/h, gusts 5)",
          "clear sky", "camera-250g", "Height limit 120 m", "No time limit",
          "Keyboard flying allowed", "time ≤ 1:00.0", "first-flight.mission.json"}) {
        EXPECT_TRUE(details.contains(QString::fromUtf8(text))) << text;
    }
    EXPECT_TRUE(panel.findChild<QPushButton*>(QStringLiteral("StartMission"))->isEnabled());
    EXPECT_TRUE(panel.findChild<QPushButton*>(QStringLiteral("OpenMissionsFolder"))->isEnabled());
}

TEST(MissionsPanel, CampaignLocksProgressAndInvalidFiles) {
    Fixture f;
    writeMission(f.userDir() / "b1.json", "b1", "Basics one", "basics", 1);
    writeMission(f.userDir() / "b2.json", "b2", "Basics two", "basics", 2);
    std::ofstream(f.userDir() / "broken.mission.json") << R"({"schemaVersion": 1, "id": "broken"})";
    f.missions->reload();
    MissionsPanel panel(*f.missions);
    auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("MissionTree"));
    ASSERT_EQ(tree->topLevelItemCount(), 3);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("Basics"));
    EXPECT_EQ(tree->topLevelItem(1)->text(0), QStringLiteral("Free missions"));
    EXPECT_EQ(tree->topLevelItem(2)->text(0), QStringLiteral("Invalid missions"));
    EXPECT_EQ(findItem(tree, QStringLiteral("Basics one"))->text(4), QStringLiteral("Available"));
    QTreeWidgetItem* locked = findItem(tree, QStringLiteral("Basics two"));
    EXPECT_EQ(locked->text(4), QStringLiteral("Locked"));
    EXPECT_TRUE(locked->toolTip(0).contains(QStringLiteral("1 star on Basics one")));
    EXPECT_EQ(findItem(tree, QStringLiteral("broken"))->text(4), QStringLiteral("Invalid"));
    EXPECT_EQ(panel.selectedMission(), "b1");  // First playable mission.

    auto* details = panel.findChild<QTextBrowser*>(QStringLiteral("MissionDetails"));
    ASSERT_TRUE(panel.selectMission("b2"));
    EXPECT_TRUE(details->toPlainText().contains(QStringLiteral("Locked: needs 1 star on Basics one.")));
    ASSERT_TRUE(panel.selectMission("broken"));
    EXPECT_TRUE(details->toPlainText().contains(QStringLiteral("has errors")));
    EXPECT_TRUE(details->toPlainText().contains(QStringLiteral("missing required property")));

    // One star on b1 unlocks b2; results appear in the list and the details.
    ds::storage::MissionAttemptRecord attempt;
    attempt.missionKey = "b1";
    attempt.startedAt = "2026-11-16T10:00:00Z";
    attempt.outcome = "succeeded";
    attempt.stars = 1;
    attempt.score = 1200.0;
    attempt.durationS = 75.25;
    ASSERT_TRUE(f.storage
                    ->submit([attempt](ds::storage::DbConnection& c) {
                        return ds::storage::MissionStore(c).recordAttempt(attempt);
                    })
                    .get());
    QSignalSpy changed(f.missions.get(), &MissionsController::missionsChanged);
    panel.findChild<QPushButton*>(QStringLiteral("ReloadMissions"))->click();
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(panel.selectedMission(), "broken");  // Selection kept.
    EXPECT_EQ(findItem(tree, QStringLiteral("Basics two"))->text(4), QStringLiteral("Available"));
    QTreeWidgetItem* b1 = findItem(tree, QStringLiteral("Basics one"));
    EXPECT_EQ(b1->text(2), QString::fromUtf8("★☆☆"));
    EXPECT_EQ(b1->text(3), QStringLiteral("1:15.3"));
    ASSERT_TRUE(panel.selectMission("b1"));
    EXPECT_TRUE(details->toPlainText().contains(QStringLiteral("1 attempt, best ★☆☆, fastest 1:15.3")));
}

TEST(MissionsPanel, ReloadPicksUpNewFiles) {
    Fixture f;
    MissionsPanel panel(*f.missions);
    auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("MissionTree"));
    writeMission(f.userDir() / "extra.json", "extra", "Extra mission", "", 0);
    EXPECT_EQ(findItem(tree, QStringLiteral("Extra mission")), nullptr);
    f.missions->reload();
    EXPECT_NE(findItem(tree, QStringLiteral("Extra mission")), nullptr);
    EXPECT_EQ(f.missions->missions().size(), 2U);
}

TEST(MissionsPanel, FormatsStarsAndTimes) {
    EXPECT_EQ(ds::app::starsText(0), QStringLiteral("—"));
    EXPECT_EQ(ds::app::starsText(3), QString::fromUtf8("★★★"));
    EXPECT_EQ(ds::app::durationText(5.04), QStringLiteral("0:05.0"));
    EXPECT_EQ(ds::app::durationText(59.96), QStringLiteral("1:00.0"));
    EXPECT_EQ(ds::app::durationText(600.0), QStringLiteral("10:00.0"));
    EXPECT_EQ(ds::app::chapterTitle("first-steps"), QStringLiteral("First steps"));
}

TEST(MissionsPanel, MainWindowShowsTheMissionsTab) {
    Fixture f;
    const ds::app::MainWindow window(nullptr, f.missions.get());
    EXPECT_EQ(window.missions(), f.missions.get());
    auto* panel = qobject_cast<MissionsPanel*>(window.tabs()->widget(0));
    ASSERT_NE(panel, nullptr);
    EXPECT_EQ(panel->selectedMission(), "first-flight");

    // Without a controller the window lists the built-in missions only.
    const ds::app::MainWindow plain;
    ASSERT_NE(plain.missions(), nullptr);
    EXPECT_TRUE(plain.missions()->folders().userDir.empty());
    for (const auto& row : plain.missions()->missions()) {
        EXPECT_EQ(plain.missions()->entry(row).origin, ds::missions::MissionOrigin::BuiltIn);
    }
    EXPECT_NE(qobject_cast<MissionsPanel*>(plain.tabs()->widget(0)), nullptr);
}

}  // namespace
