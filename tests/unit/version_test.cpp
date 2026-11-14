// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/common/version.hpp"

#include <gtest/gtest.h>

#include <format>

namespace {

TEST(Version, NameIsDroneSimulator) { EXPECT_EQ(ds::common::kApplicationName, "Drone Simulator"); }

TEST(Version, CopyrightNamesAuthorEmailAndYear) {
    EXPECT_EQ(ds::common::kCopyright,
              "Copyright Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com> 2026");
}

TEST(Version, StringMatchesComponents) {
    const auto expected = std::format("{}.{}.{}", ds::common::kVersionMajor,
                                      ds::common::kVersionMinor, ds::common::kVersionPatch);
    EXPECT_EQ(ds::common::kVersion, expected);
}

}  // namespace
