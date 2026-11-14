// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "ds/astro/module.hpp"
#include "ds/common/module.hpp"
#include "ds/geo/module.hpp"
#include "ds/graphics/module.hpp"
#include "ds/input/module.hpp"
#include "ds/items/module.hpp"
#include "ds/missions/module.hpp"
#include "ds/net/module.hpp"
#include "ds/physics/module.hpp"
#include "ds/sim/module.hpp"
#include "ds/storage/module.hpp"
#include "ds/terrain/module.hpp"
#include "ds/weather/module.hpp"

namespace {

// Every Core module links and reports its own name (milestone M0 skeleton).
TEST(Modules, ReportTheirNames) {
    EXPECT_EQ(ds::common::moduleName(), "common");
    EXPECT_EQ(ds::geo::moduleName(), "geo");
    EXPECT_EQ(ds::astro::moduleName(), "astro");
    EXPECT_EQ(ds::weather::moduleName(), "weather");
    EXPECT_EQ(ds::storage::moduleName(), "storage");
    EXPECT_EQ(ds::net::moduleName(), "net");
    EXPECT_EQ(ds::terrain::moduleName(), "terrain");
    EXPECT_EQ(ds::items::moduleName(), "items");
    EXPECT_EQ(ds::input::moduleName(), "input");
    EXPECT_EQ(ds::physics::moduleName(), "physics");
    EXPECT_EQ(ds::missions::moduleName(), "missions");
    EXPECT_EQ(ds::graphics::moduleName(), "graphics");
    EXPECT_EQ(ds::sim::moduleName(), "sim");
}

}  // namespace
