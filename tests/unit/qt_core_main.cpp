// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <QCoreApplication>

#include <gtest/gtest.h>

// Storage tests need a QCoreApplication so Qt can load the QSQLITE driver plugin (§8.1).
int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    const QCoreApplication app(argc, argv);
    return RUN_ALL_TESTS();
}
