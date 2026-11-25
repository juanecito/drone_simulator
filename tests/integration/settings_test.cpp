// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Start-up storage, the settings adapter and the Settings tab (FR-DB-01/02, FR-UI-03, FR-UI-06).

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <memory>

#include "ds/app/main_window.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/settings_panel.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/app/viewport.hpp"
#include "ds/storage/data_location.hpp"
#include "ds/storage/migrations.hpp"
#include "ds/storage/settings_store.hpp"

namespace {

using ds::app::CoordinateDisplay;
using ds::app::MainWindow;
using ds::app::SettingsController;
using ds::app::SettingsPanel;
using ds::app::UnitSystem;
namespace fs = std::filesystem;

fs::path toPath(const QString& path) { return fs::path(path.toStdString()); }

ds::app::UserStorage open(const std::optional<fs::path>& db, const fs::path& home) {
    auto storage = ds::app::openUserStorage(db, home);
    EXPECT_TRUE(storage.has_value()) << (storage ? "" : storage.error().message);
    return std::move(*storage);
}

TEST(UserStorage, CreatesPrivateDataDirectoryAndDatabase) {
    const QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const auto storage = open(std::nullopt, toPath(home.path()));
    EXPECT_TRUE(storage.warning.empty());
    const auto& info = storage.service->info();
    EXPECT_FALSE(info.inMemory);
    const fs::path expected = toPath(home.path()) / ".drone_simulation" / "drone_simulation.db";
    EXPECT_EQ(info.databaseFile, expected);
    EXPECT_TRUE(fs::exists(expected));
    EXPECT_EQ(info.migration.toVersion, ds::storage::latestSchemaVersion());
    const auto perms = fs::status(expected.parent_path()).permissions();
    EXPECT_EQ(perms & fs::perms::all, fs::perms::owner_all);
}

TEST(UserStorage, DatabaseOverrideSkipsTheDataDirectory) {
    const QTemporaryDir home;
    const QTemporaryDir other;
    const fs::path db = toPath(other.path()) / "test.db";
    const auto storage = open(db, toPath(home.path()));
    EXPECT_TRUE(storage.warning.empty());
    EXPECT_EQ(storage.service->info().databaseFile, db);
    EXPECT_FALSE(fs::exists(toPath(home.path()) / ".drone_simulation"));
}

TEST(UserStorage, UnusableHomeFallsBackToMemoryWithWarning) {
    const QTemporaryDir dir;
    const QString notADirectory = dir.filePath(QStringLiteral("home-is-a-file"));
    {
        QFile file(notADirectory);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }
    const auto storage = open(std::nullopt, toPath(notADirectory));
    EXPECT_TRUE(storage.service->info().inMemory);
    EXPECT_FALSE(storage.warning.empty());
    EXPECT_NE(storage.warning.find("will not be saved"), std::string::npos);
}

TEST(UserStorage, UnopenableDatabaseFallsBackToMemory) {
    const QTemporaryDir dir;
    const auto storage = open(toPath(dir.path()), toPath(dir.path()));  // A directory, not a file.
    EXPECT_TRUE(storage.service->info().inMemory);
    EXPECT_FALSE(storage.warning.empty());
}

TEST(SettingsController, DefaultsAndPersistence) {
    const QTemporaryDir home;
    const fs::path db = toPath(home.path()) / "settings.db";
    {
        auto storage = open(db, {});
        SettingsController settings(*storage.service);
        EXPECT_EQ(settings.units(), UnitSystem::Metric);
        EXPECT_EQ(settings.coordinateDisplay(), CoordinateDisplay::Utm);
        EXPECT_FALSE(settings.windowGeometry().has_value());
        EXPECT_TRUE(settings.chaseTopView());
        EXPECT_TRUE(settings.chaseBehindView());

        QSignalSpy unitsSpy(&settings, &SettingsController::unitsChanged);
        settings.setUnits(UnitSystem::Imperial);
        settings.setUnits(UnitSystem::Imperial);  // No change, no signal.
        EXPECT_EQ(unitsSpy.count(), 1);
        settings.setCoordinateDisplay(CoordinateDisplay::UtmAndLatLon);
        settings.setWindowLayout(QByteArray("geo\0metry", 9), QByteArray("state"));
        QSignalSpy insetsSpy(&settings, &SettingsController::chaseInsetsChanged);
        settings.setChaseBehindView(false);
        ASSERT_EQ(insetsSpy.count(), 1);
        EXPECT_EQ(insetsSpy.front(), (QVariantList{true, false}));
    }
    auto storage = open(db, {});
    const SettingsController settings(*storage.service);
    EXPECT_EQ(settings.units(), UnitSystem::Imperial);
    EXPECT_EQ(settings.coordinateDisplay(), CoordinateDisplay::UtmAndLatLon);
    EXPECT_EQ(settings.windowGeometry(), QByteArray("geo\0metry", 9));
    EXPECT_EQ(settings.windowState(), QByteArray("state"));
    EXPECT_TRUE(settings.chaseTopView());
    EXPECT_FALSE(settings.chaseBehindView());
}

TEST(SettingsController, InvalidStoredValuesAreIgnored) {
    const QTemporaryDir home;
    auto storage = open(toPath(home.path()) / "bad.db", {});
    storage.service
        ->submit([](ds::storage::DbConnection& c) {
            ds::storage::SettingsStore store(c);
            ASSERT_TRUE(store.setString(ds::app::settings_keys::kUnits, "furlongs"));
            ASSERT_TRUE(store.setInteger(ds::app::settings_keys::kCoordinates, 7));
            ASSERT_TRUE(store.setString(ds::app::settings_keys::kWindowGeometry, "%%%"));
        })
        .get();
    const SettingsController settings(*storage.service);
    EXPECT_EQ(settings.units(), UnitSystem::Metric);
    EXPECT_EQ(settings.coordinateDisplay(), CoordinateDisplay::Utm);
    EXPECT_FALSE(settings.windowGeometry().has_value());
}

TEST(SettingsPanel, ShowsDataLocationAndEditsDisplaySettings) {
    const QTemporaryDir home;
    auto storage = open(std::nullopt, toPath(home.path()));
    SettingsController settings(*storage.service);
    const MainWindow window(&settings);

    auto* panel = window.tabs()->findChild<SettingsPanel*>(QStringLiteral("SettingsPanel"));
    ASSERT_NE(panel, nullptr);
    // The panel sits in a scroll area together with the controls.
    int tab = -1;
    for (int i = 0; i < window.tabs()->count(); ++i) {
        if (window.tabs()->widget(i)->isAncestorOf(panel)) {
            tab = i;
        }
    }
    ASSERT_GE(tab, 0);
    EXPECT_EQ(window.tabs()->tabText(tab), QStringLiteral("Settings"));
    EXPECT_NE(window.tabs()->widget(tab)->findChild<QWidget*>(QStringLiteral("ControlsPanel")),
              nullptr);
    EXPECT_TRUE(panel->databaseLabel()->text().endsWith(
        QStringLiteral(".drone_simulation/drone_simulation.db")));
    const auto* open = panel->findChild<QPushButton*>(QStringLiteral("OpenDataFolderButton"));
    ASSERT_NE(open, nullptr);
    EXPECT_TRUE(open->isEnabled());

    panel->unitsCombo()->setCurrentIndex(
        panel->unitsCombo()->findText(QStringLiteral("Imperial"), Qt::MatchStartsWith));
    EXPECT_EQ(settings.units(), UnitSystem::Imperial);
    settings.setCoordinateDisplay(CoordinateDisplay::UtmAndLatLon);  // Model → view.
    EXPECT_EQ(panel->coordinatesCombo()->currentData().toInt(),
              int(CoordinateDisplay::UtmAndLatLon));

    // Chase insets: the check boxes change the setting, which reaches the 3D view.
    EXPECT_EQ(panel->chaseTopViewCheck(),
              panel->findChild<QCheckBox*>(QStringLiteral("ChaseTopViewCheck")));
    EXPECT_TRUE(panel->chaseTopViewCheck()->isChecked());
    EXPECT_TRUE(panel->chaseBehindViewCheck()->isChecked());
    panel->chaseTopViewCheck()->setChecked(false);
    EXPECT_FALSE(settings.chaseTopView());
    EXPECT_EQ(window.viewport()->chaseInsetsShown(), (std::array{false, true}));
    settings.setChaseBehindView(false);  // Model → view.
    EXPECT_FALSE(panel->chaseBehindViewCheck()->isChecked());
    EXPECT_EQ(window.viewport()->chaseInsetsShown(), (std::array{false, false}));
}

TEST(SettingsPanel, InMemoryDatabaseIsExplained) {
    auto memory = ds::storage::StorageService::start({});
    ASSERT_TRUE(memory.has_value());
    SettingsController settings(**memory);
    const SettingsPanel panel(settings);
    EXPECT_TRUE(panel.databaseLabel()->text().contains(QStringLiteral("nothing is saved")));
    EXPECT_FALSE(
        panel.findChild<QPushButton*>(QStringLiteral("OpenDataFolderButton"))->isEnabled());
}

TEST(MainWindowLayout, GeometryAndDockStateSurviveRestart) {
    const QTemporaryDir home;
    const fs::path db = toPath(home.path()) / "layout.db";
    {
        auto storage = open(db, {});
        SettingsController settings(*storage.service);
        MainWindow window(&settings);
        window.show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
        // Within the offscreen platform's 800×600 screen: restoreGeometry() clamps to it.
        window.resize(700, 500);
        window.panelAction()->trigger();  // Hide the panel.
        ASSERT_FALSE(window.panelDock()->isVisibleTo(&window));
        window.close();
    }
    auto storage = open(db, {});
    SettingsController settings(*storage.service);
    ASSERT_TRUE(settings.windowGeometry().has_value());
    ASSERT_TRUE(settings.windowState().has_value());
    MainWindow window(&settings);
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    EXPECT_EQ(window.size(), QSize(700, 500));
    EXPECT_FALSE(window.panelDock()->isVisibleTo(&window));
    EXPECT_FALSE(window.panelAction()->isChecked());
}

}  // namespace
