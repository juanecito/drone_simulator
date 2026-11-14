// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/main_window.hpp"

#include <QAction>
#include <QDockWidget>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QSignalSpy>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTest>
#include <QTextDocument>

#include <gtest/gtest.h>

#include "ds/app/viewport.hpp"

namespace {

using ds::app::MainWindow;

TEST(MainWindow, HasViewportAsCentralWidget) {
    const MainWindow window;
    ASSERT_NE(window.viewport(), nullptr);
    EXPECT_EQ(window.centralWidget(), window.viewport());
    EXPECT_EQ(window.windowTitle(), QStringLiteral("Drone Simulator"));
}

TEST(MainWindow, PanelHasTheSpecifiedTabsInOrder) {
    const MainWindow window;
    const QStringList expected{"Missions", "Simulation",  "Vehicle",  "Terrain",
                               "Items",    "Environment", "Settings", "History"};
    ASSERT_EQ(window.tabs()->count(), expected.size());
    for (int i = 0; i < window.tabs()->count(); ++i) {
        EXPECT_EQ(window.tabs()->tabText(i), expected.at(i)) << i;
    }
    EXPECT_EQ(window.panelDock()->widget(), window.tabs());
}

TEST(MainWindow, MenusExist) {
    const MainWindow window;
    QStringList titles;
    for (const QAction* action : window.menuBar()->actions()) {
        titles << action->text().remove(QLatin1Char('&'));
    }
    EXPECT_EQ(titles, (QStringList{"File", "Missions", "Simulation", "Vehicle", "View", "Help"}));
}

TEST(MainWindow, ShortcutsAreF11AndF9) {
    const MainWindow window;
    EXPECT_EQ(window.fullScreenAction()->shortcut(), QKeySequence(Qt::Key_F11));
    EXPECT_EQ(window.panelAction()->shortcut(), QKeySequence(Qt::Key_F9));
}

TEST(MainWindow, PanelActionTogglesDock) {
    MainWindow window;
    window.show();  // The dock's toggle action follows the visibility of a shown window.
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    EXPECT_TRUE(window.panelAction()->isChecked());
    EXPECT_TRUE(window.panelDock()->isVisibleTo(&window));
    window.panelAction()->trigger();
    EXPECT_FALSE(window.panelDock()->isVisibleTo(&window));
    window.panelAction()->trigger();
    EXPECT_TRUE(window.panelDock()->isVisibleTo(&window));
}

TEST(MainWindow, StatusBarShowsPositionStateAndGl) {
    const MainWindow window;
    const auto* position = window.statusBar()->findChild<QLabel*>(QStringLiteral("PositionLabel"));
    ASSERT_NE(position, nullptr);
    EXPECT_TRUE(position->text().contains(QStringLiteral("UTM")));
    EXPECT_TRUE(position->text().contains(QStringLiteral("Elev")));
    EXPECT_NE(window.statusBar()->findChild<QLabel*>(QStringLiteral("StateLabel")), nullptr);
    EXPECT_NE(window.statusBar()->findChild<QLabel*>(QStringLiteral("GlLabel")), nullptr);
}

TEST(MainWindow, AboutShowsCopyrightWithEmail) {
    const QString html = MainWindow::aboutText();
    // The e-mail's angle brackets must be escaped, or rich text would swallow it as a tag.
    EXPECT_TRUE(html.contains(QStringLiteral(
        "Copyright Juan Maria Gomez Lopez &lt;juan-maria.gomez-lopez@tutamail.com&gt; 2026")));
    QTextDocument doc;
    doc.setHtml(html);
    EXPECT_TRUE(doc.toPlainText().contains(QStringLiteral(
        "Copyright Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com> 2026")));
    EXPECT_TRUE(doc.toPlainText().contains(QStringLiteral("MIT License")));
}

// FR-GFX-01: a viewport GL failure reaches main() (which shows the dialog and exits),
// delivered asynchronously so no dialog is opened from inside initializeGL().
TEST(MainWindow, ViewportGlFailureBecomesFatalError) {
    MainWindow window;
    QSignalSpy fatal(&window, &MainWindow::fatalError);
    emit window.viewport()->glFailed(QStringLiteral("no GL"));
    EXPECT_TRUE(fatal.isEmpty());  // Queued, not delivered yet.
    ASSERT_TRUE(fatal.wait(1000));
    EXPECT_EQ(fatal.first().first().toString(), QStringLiteral("no GL"));
    const auto* glLabel = window.statusBar()->findChild<QLabel*>(QStringLiteral("GlLabel"));
    ASSERT_NE(glLabel, nullptr);
    EXPECT_TRUE(glLabel->text().contains(QStringLiteral("unavailable")));
}

}  // namespace
