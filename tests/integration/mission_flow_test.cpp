// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// The mission flow in the main window (FR-UI-07, M5): briefing → flight → debrief, attempts
// stored in the user's database, Missions › Import, and the free-mode rules setting.

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "ds/app/asset_paths.hpp"
#include "ds/app/items_controller.hpp"
#include "ds/app/items_panel.hpp"
#include "ds/app/main_window.hpp"
#include "ds/app/mission_dialogs.hpp"
#include "ds/app/missions_controller.hpp"
#include "ds/app/missions_panel.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/common/json.hpp"
#include "ds/storage/mission_store.hpp"

namespace ds::app {
namespace {

namespace fs = std::filesystem;

// Saves @p widget as @p name in $DS_TEST_IMAGE_DIR, if set (visual checks).
void saveGrab(QWidget* widget, const char* name) {
    if (const char* dir = std::getenv("DS_TEST_IMAGE_DIR")) {
        widget->grab().save(QString::fromUtf8(dir) + QLatin1Char('/') + QLatin1String(name));
    }
}

class MissionFlowTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home_.isValid());
        auto storage = openUserStorage(std::nullopt, homeDir());
        ASSERT_TRUE(storage.has_value());
        storage_ = std::move(*storage);
        settings_ = std::make_unique<SettingsController>(*storage_.service);
        missions_ =
            std::make_unique<MissionsController>(MissionFolders{.assetDir = findAssetDirectory(),
                                                                .userDir = homeDir() / "missions",
                                                                .libraryDir = {}},
                                                 storage_.service.get());
        window_ = std::make_unique<MainWindow>(settings_.get(), missions_.get());
        window_->show();
    }

    void TearDown() override {
        window_.reset();
        missions_.reset();
        settings_.reset();
    }

    [[nodiscard]] fs::path homeDir() const { return fs::path(home_.path().toStdString()); }

    // Waits for @p condition, refreshing the simulation frame as the viewport would.
    bool waitFor(const std::function<bool()>& condition, int timeoutMs = 5000) {
        return QTest::qWaitFor(
            [&] {
                window_->simulation()->updateFrame(0.016, 1.6);
                return condition();
            },
            timeoutMs);
    }

    std::vector<storage::MissionAttemptRecord> attempts(const std::string& key) {
        auto list = storage_.service
                        ->submit([key](storage::DbConnection& c) {
                            return storage::MissionStore(c).attempts(key);
                        })
                        .get();
        EXPECT_TRUE(list.has_value());
        return list ? *list : std::vector<storage::MissionAttemptRecord>{};
    }

    QTemporaryDir home_;
    UserStorage storage_;
    std::unique_ptr<SettingsController> settings_;
    std::unique_ptr<MissionsController> missions_;
    std::unique_ptr<MainWindow> window_;
};

TEST_F(MissionFlowTest, BriefingFlyStartsTheMissionAndLocksTheScene) {
    SimulationController& sim = *window_->simulation();
    ASSERT_TRUE(window_->panelDock()->isVisible());
    window_->startMission("first-flight");
    MissionBriefingDialog* briefing = window_->briefingDialog();
    ASSERT_NE(briefing, nullptr);
    EXPECT_TRUE(briefing->text()->toPlainText().contains(QStringLiteral("Climb to 10 m")));
    EXPECT_TRUE(briefing->text()->toPlainText().contains(QStringLiteral("Sunrise")));
    saveGrab(briefing, "briefing.png");
    // A second request while the briefing is open is ignored.
    window_->startMission("basics-01-hover");
    EXPECT_EQ(window_->briefingDialog(), briefing);

    briefing->flyButton()->click();
    ASSERT_TRUE(sim.missionActive());
    EXPECT_EQ(sim.mission()->id, "first-flight");
    EXPECT_FALSE(window_->panelDock()->isVisible());  // FR-UI-04: full view.
    EXPECT_FALSE(window_->missionsPanel()->startButton()->isEnabled());
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
    EXPECT_EQ(sim.missionStatus().state, missions::MissionState::PreFlight);
    EXPECT_EQ(sim.missionStatus().objectives.size(), 3U);

    // Another mission cannot start on top of it.
    EXPECT_FALSE(window_->launchMission("basics-01-hover").has_value());

    window_->endMission();
    EXPECT_FALSE(sim.missionActive());
    EXPECT_TRUE(window_->panelDock()->isVisible());
    EXPECT_TRUE(window_->missionsPanel()->startButton()->isEnabled());
}

TEST_F(MissionFlowTest, AbortedAttemptIsStoredAndDebriefed) {
    SimulationController& sim = *window_->simulation();
    ASSERT_TRUE(window_->launchMission("first-flight").has_value());
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
    sim.abortMission();
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().result.has_value(); }));
    EXPECT_EQ(sim.missionStatus().result->outcome, missions::MissionState::Aborted);

    // Stored at once (FR-MIS-12); the debrief follows after a short delay.
    auto stored = attempts("first-flight");
    ASSERT_EQ(stored.size(), 1U);
    EXPECT_EQ(stored[0].outcome, "aborted");
    ASSERT_TRUE(waitFor([&] { return window_->debriefDialog() != nullptr; },
                        MainWindow::kDebriefDelayMs + 3000));
    MissionDebriefDialog* debrief = window_->debriefDialog();
    saveGrab(debrief, "debrief.png");
    const QString text = debrief->text()->toPlainText();
    EXPECT_TRUE(text.contains(QStringLiteral("Mission aborted"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("battery 100 %"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("score 0"))) << text.toStdString();
    ASSERT_NE(debrief->button(MissionDebriefDialog::Choice::Next), nullptr);

    // Retry: a new attempt from the start.
    debrief->button(MissionDebriefDialog::Choice::Retry)->click();
    ASSERT_TRUE(waitFor([&] {
        return sim.missionStatus().attempt == 1 && !sim.missionStatus().result.has_value();
    }));
    EXPECT_TRUE(sim.missionActive());
    EXPECT_EQ(sim.missionStatus().state, missions::MissionState::PreFlight);

    // Abort again, then back to free flight from the debrief.
    sim.abortMission();
    ASSERT_TRUE(waitFor([&] { return window_->debriefDialog() != nullptr; },
                        MainWindow::kDebriefDelayMs + 3000));
    EXPECT_EQ(attempts("first-flight").size(), 2U);
    window_->debriefDialog()->button(MissionDebriefDialog::Choice::Back)->click();
    ASSERT_TRUE(waitFor([&] { return !sim.missionActive(); }));
    EXPECT_TRUE(window_->panelDock()->isVisible());
}

TEST_F(MissionFlowTest, RetryFromTheMenuKeepsTheMissionWhileTheDebriefIsOpen) {
    SimulationController& sim = *window_->simulation();
    ASSERT_TRUE(window_->launchMission("first-flight").has_value());
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().active; }));
    sim.abortMission();
    ASSERT_TRUE(waitFor([&] { return window_->debriefDialog() != nullptr; },
                        MainWindow::kDebriefDelayMs + 3000));
    QAction* retry = nullptr;
    for (QAction* a : window_->findChildren<QAction*>()) {
        if (a->text() == QStringLiteral("Re&try Mission")) {
            retry = a;
        }
    }
    ASSERT_NE(retry, nullptr);
    retry->trigger();
    EXPECT_EQ(window_->debriefDialog(), nullptr);
    ASSERT_TRUE(waitFor([&] { return sim.missionStatus().attempt == 1; }));
    EXPECT_TRUE(sim.missionActive());  // Closing the debrief is not "Back to free flight".
}

// Radius of a tree's crown (its largest foliage sphere).
double crownRadius(const items::Item& tree) {
    double r = 0.0;
    for (const items::SolidPart& part : tree.shape.parts) {
        if (part.material == items::Material::Foliage) {
            r = std::max(r, part.radiusM);
        }
    }
    return r;
}

TEST_F(MissionFlowTest, ItemsTabAddsEditsAndSavesSceneItems) {
    auto* panel = window_->findChild<ItemsPanel*>(QStringLiteral("ItemsPanel"));
    ASSERT_NE(panel, nullptr);
    ItemsController& items = *window_->items();
    auto* tree = panel->findChild<QTreeWidget*>(QStringLiteral("ItemTree"));
    ASSERT_EQ(tree->topLevelItemCount(), 0);

    // Palette: a gate and a tree in front of the drone.
    const auto paletteIndex = [](const char* label) {
        for (std::size_t i = 0; i < itemPalette().size(); ++i) {
            if (itemPalette()[i].label == label) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    panel->addFromPalette(paletteIndex("Gate"));
    panel->addFromPalette(paletteIndex("Gate"));
    panel->addFromPalette(paletteIndex("Tree"));
    ASSERT_EQ(items.registry().size(), 3U);
    EXPECT_EQ(tree->topLevelItemCount(), 3);
    const auto selected = panel->selectedItem();
    ASSERT_TRUE(selected.has_value());
    const items::Item* treeItem = items.registry().get(*selected);
    EXPECT_EQ(treeItem->type, items::ItemType::Obstacle);
    EXPECT_TRUE(treeItem->solid);
    EXPECT_EQ(treeItem->name, "Tree 1");
    // 15 m north of the drone (heading 0), on the ground.
    const glm::dvec3 drone = window_->simulation()->snapshot().body.position;
    EXPECT_NEAR(treeItem->transform.position.z, drone.z - 15.0, 0.5);
    EXPECT_NEAR(treeItem->transform.position.y,
                window_->scene()->query().height(treeItem->transform.position.x,
                                                 treeItem->transform.position.z),
                1e-6);

    // Property editor: rename, move 10 m east, 4 m radius.
    panel->findChild<QLineEdit*>(QStringLiteral("ItemName"))->setText(QStringLiteral("Big oak"));
    auto* east = panel->findChild<QDoubleSpinBox*>(QStringLiteral("ItemEast"));
    east->setValue(east->value() + 10.0);
    panel->findChild<QDoubleSpinBox*>(QStringLiteral("ItemRadius"))->setValue(4.0);
    panel->applyEdits();
    treeItem = items.registry().get(*selected);
    EXPECT_EQ(treeItem->name, "Big oak");
    EXPECT_DOUBLE_EQ(crownRadius(*treeItem), 4.0);
    EXPECT_NEAR(treeItem->transform.position.x, drone.x + 10.0, 0.5);
    ASSERT_TRUE(items.undo());
    EXPECT_EQ(items.registry().get(*selected)->name, "Tree 1");
    ASSERT_TRUE(items.redo());

    // Gate order: the second gate first.
    auto order = items.gateOrder();
    ASSERT_EQ(order.size(), 2U);
    ASSERT_TRUE(items.moveGate(order[1], -1));
    EXPECT_EQ(items.gateOrder().front(), order[1]);
    EXPECT_EQ(panel->findChild<QListWidget*>(QStringLiteral("GateOrder"))->item(0)->text(),
              QStringLiteral("1. Gate 2"));

    // Duplicate and delete.
    auto copy = items.duplicate(*selected);
    ASSERT_TRUE(copy.has_value());
    EXPECT_EQ(items.registry().get(*copy)->name, "Big oak (copy)");
    ASSERT_TRUE(items.remove(*copy));
    window_->tabs()->setCurrentWidget(panel);
    saveGrab(window_->panelDock(), "items_tab.png");
    EXPECT_EQ(items.registry().size(), 3U);

    // Saved with the scene: a new controller on the same settings loads them back.
    const ItemsController reloaded(*window_->scene(), settings_.get());
    EXPECT_EQ(reloaded.registry().size(), 3U);
    ASSERT_NE(reloaded.registry().find(treeItem->key), nullptr);
    EXPECT_EQ(reloaded.registry().find(treeItem->key)->name, "Big oak");
    EXPECT_DOUBLE_EQ(crownRadius(*reloaded.registry().find(treeItem->key)), 4.0);
}

TEST_F(MissionFlowTest, ItemsTabAddsHousesAnimalsAndPeopleWithAMass) {
    auto* panel = window_->findChild<ItemsPanel*>(QStringLiteral("ItemsPanel"));
    ASSERT_NE(panel, nullptr);
    ItemsController& items = *window_->items();
    const auto add = [&](const std::string& label) -> const items::Item* {
        const auto& palette = itemPalette();
        const auto it = std::ranges::find(palette, label, &ItemTemplate::label);
        EXPECT_NE(it, palette.end()) << label;
        panel->addFromPalette(static_cast<int>(it - palette.begin()));
        const auto id = panel->selectedItem();
        return id ? items.registry().get(*id) : nullptr;
    };
    auto* mass = panel->findChild<QDoubleSpinBox*>(QStringLiteral("ItemMass"));
    auto* shape = panel->findChild<QComboBox*>(QStringLiteral("ItemShape"));
    auto* height = panel->findChild<QDoubleSpinBox*>(QStringLiteral("ItemHeight"));
    ASSERT_NE(mass, nullptr);
    for (const auto& [label, kind, kg] :
         {std::tuple{"House", "house", 140000.0}, std::tuple{"Cow", "cow", 650.0},
          std::tuple{"Man", "man", 75.0}}) {
        const items::Item* added = add(label);
        ASSERT_NE(added, nullptr) << label;
        EXPECT_TRUE(added->solid);
        EXPECT_EQ(added->properties.value("shape", ""), kind);
        EXPECT_FALSE(added->shape.parts.empty()) << label;
        EXPECT_NEAR(mass->value(), kg, 0.05) << label;
        EXPECT_FALSE(mass->isHidden());
    }

    // The man (selected): taller, and heavier by hand.
    height->setValue(1.9);
    mass->setValue(95.0);
    panel->applyEdits();
    const auto man = panel->selectedItem();
    ASSERT_TRUE(man.has_value());
    EXPECT_DOUBLE_EQ(items.registry().get(*man)->properties.value("heightM", 0.0), 1.9);
    EXPECT_DOUBLE_EQ(items::massKg(*items.registry().get(*man)), 95.0);
    // Another figure: its typical size and mass; the hand-set mass is dropped.
    shape->setCurrentText(QStringLiteral("cow"));
    EXPECT_NEAR(mass->value(), 650.0, 0.05);
    EXPECT_DOUBLE_EQ(height->value(), 1.5);
    panel->applyEdits();
    const items::Item* cow = items.registry().get(*man);
    EXPECT_EQ(cow->properties.value("shape", ""), "cow");
    EXPECT_FALSE(cow->properties.contains("massKg"));
    EXPECT_NEAR(items::massKg(*cow), 650.0, 1e-6);
}

TEST_F(MissionFlowTest, ItemsTabShowsTheMissionsItemsReadOnly) {
    auto* panel = window_->findChild<ItemsPanel*>(QStringLiteral("ItemsPanel"));
    ASSERT_NE(panel, nullptr);
    panel->addFromPalette(0);
    ASSERT_TRUE(window_->launchMission("racing-01-gate-run").has_value());
    auto* tree = panel->findChild<QTreeWidget*>(QStringLiteral("ItemTree"));
    EXPECT_EQ(tree->topLevelItemCount(), 6);  // Pad and five gates.
    EXPECT_FALSE(panel->findChild<QToolButton*>(QStringLiteral("AddItemButton"))->isEnabled());
    EXPECT_FALSE(panel->findChild<QLineEdit*>(QStringLiteral("ItemName"))->isEnabled());
    EXPECT_FALSE(panel->selectedItem().has_value());
    window_->endMission();
    EXPECT_EQ(tree->topLevelItemCount(), 1);  // The scene's own item again.
}

TEST_F(MissionFlowTest, UnavailableMissionsDoNotStart) {
    // Real terrain needs the elevation sources of M4.
    window_->startMission("real-01-montserrat");
    EXPECT_EQ(window_->briefingDialog(), nullptr);
    EXPECT_FALSE(window_->launchMission("real-01-montserrat").has_value());
    EXPECT_FALSE(window_->launchMission("no-such-mission").has_value());
    EXPECT_FALSE(window_->simulation()->missionActive());
}

TEST_F(MissionFlowTest, ImportCopiesTheMissionAndSelectsIt) {
    std::ifstream in(findAssetDirectory() / "default-missions" / "first-flight.mission.json");
    common::Json m = common::Json::parse(in);
    m["id"] = "imported-hop";
    m["title"] = "Imported hop";
    const fs::path source = homeDir() / "hop.mission.json";
    std::ofstream(source) << m.dump(2);

    window_->importMission(QString::fromStdString(source.string()));
    EXPECT_TRUE(fs::exists(homeDir() / "missions" / "hop.mission.json"));
    EXPECT_EQ(window_->missionsPanel()->selectedMission(), "imported-hop");
    EXPECT_NE(missions_->find("imported-hop"), nullptr);
}

TEST_F(MissionFlowTest, FreeModeRulesSettingIsSaved) {
    auto* check = window_->findChild<QCheckBox*>(QStringLiteral("EnforceRulesCheck"));
    ASSERT_NE(check, nullptr);
    EXPECT_FALSE(check->isChecked());
    EXPECT_FALSE(settings_->enforceFreeFlightRules());
    check->click();
    EXPECT_TRUE(settings_->enforceFreeFlightRules());
}

}  // namespace
}  // namespace ds::app
