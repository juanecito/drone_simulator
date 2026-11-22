// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <thread>

#include "ds/common/worker_pool.hpp"
#include "ds/terrain/chunk.hpp"
#include "ds/terrain/chunk_selection.hpp"
#include "ds/terrain/noise.hpp"
#include "ds/terrain/procedural_source.hpp"
#include "ds/terrain/query.hpp"
#include "ds/terrain/streamer.hpp"

namespace {

using namespace ds::terrain;

std::shared_ptr<const ProceduralSource> makeSource(const ProceduralParams& params = {},
                                                   std::optional<Extent> extent = std::nullopt) {
    auto source = ProceduralSource::create(params, extent);
    EXPECT_TRUE(source.has_value()) << source.error().message;
    return *source;
}

// --- Noise ------------------------------------------------------------------------------------

TEST(SimplexNoise, DeterministicBoundedAndSeedDependent) {
    const SimplexNoise a(42);
    const SimplexNoise b(42);
    const SimplexNoise c(43);
    double maxAbs = 0.0;
    int differences = 0;
    for (int i = 0; i < 2000; ++i) {
        const double x = i * 0.37 - 300.0;
        const double y = i * 0.13 + 50.0;
        EXPECT_EQ(a.value(x, y), b.value(x, y));
        maxAbs = std::max(maxAbs, std::abs(a.value(x, y)));
        differences += a.value(x, y) != c.value(x, y) ? 1 : 0;
    }
    EXPECT_LE(maxAbs, 1.05);
    EXPECT_GT(maxAbs, 0.6);
    EXPECT_GT(differences, 1900);
}

TEST(SimplexNoise, AnalyticGradientMatchesFiniteDifferences) {
    const SimplexNoise noise(7);
    constexpr double h = 1e-6;
    for (int i = 0; i < 200; ++i) {
        const double x = i * 0.731 - 40.0;
        const double y = i * 0.417 + 3.0;
        const NoiseSample s = noise.sample(x, y);
        EXPECT_NEAR(s.dx, (noise.value(x + h, y) - noise.value(x - h, y)) / (2 * h), 1e-4);
        EXPECT_NEAR(s.dy, (noise.value(x, y + h) - noise.value(x, y - h)) / (2 * h), 1e-4);
    }
}

TEST(SimplexNoise, PreciseFarFromOrigin) {
    const SimplexNoise noise(3);
    // Same relative offset far away still gives a smooth, finite field.
    const double base = 1.0e6;
    const double v0 = noise.value(base, base);
    const double v1 = noise.value(base + 1e-3, base);
    EXPECT_TRUE(std::isfinite(v0));
    EXPECT_NEAR(v0, v1, 0.01);
}

// --- Parameters (FR-TER-P01) --------------------------------------------------------------------

TEST(ProceduralParams, Validation) {
    EXPECT_TRUE(validate(ProceduralParams{}).has_value());
    const auto bad = [](auto mutate) {
        ProceduralParams p;
        mutate(p);
        return !validate(p).has_value();
    };
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.octaves = 0; }));
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.octaves = 17; }));
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.featureSizeM = 10.0; }));
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.lacunarity = 1.0; }));
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.persistence = 1.0; }));
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.amplitudeM = -1.0; }));
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.amplitudeM = std::nan(""); }));
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.mountains = 1.5; }));
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.waterLevelM = 9000.0; }));
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.flatZones.push_back({.radiusM = 0.0}); }));
    EXPECT_TRUE(bad([](ProceduralParams& p) { p.flatZones.resize(65); }));
    const auto error = validate(ProceduralParams{.lacunarity = 9.0});
    ASSERT_FALSE(error.has_value());
    EXPECT_NE(error.error().message.find("lacunarity"), std::string::npos);

    EXPECT_TRUE(validate(Extent{256.0, 200'000.0}).has_value());
    EXPECT_FALSE(validate(Extent{100.0, 1000.0}).has_value());
    EXPECT_FALSE(validate(Extent{1000.0, 300'000.0}).has_value());
    EXPECT_FALSE(ProceduralSource::create({}, Extent{10.0, 10.0}).has_value());
}

// --- Determinism (FR-TER-03) ---------------------------------------------------------------------

TEST(ProceduralSource, SameSeedSameHeightsDifferentSeedDifferent) {
    const auto a = makeSource({.seed = 123});
    const auto b = makeSource({.seed = 123});
    const auto c = makeSource({.seed = 124});
    int differences = 0;
    for (int i = 0; i < 500; ++i) {
        const double x = i * 37.1 - 5000.0;
        const double z = i * 11.7 - 2000.0;
        EXPECT_EQ(a->height(x, z), b->height(x, z));
        differences += std::abs(a->height(x, z) - c->height(x, z)) > 0.01 ? 1 : 0;
    }
    EXPECT_GT(differences, 450);
}

TEST(ProceduralSource, HeightsFollowAmplitudeAndBase) {
    const auto source = makeSource({.amplitudeM = 200.0, .baseHeightM = 500.0, .mountains = 0.0});
    double lo = 1e9;
    double hi = -1e9;
    for (int i = 0; i < 4000; ++i) {
        const double h = source->height(i * 97.0 - 2e5, (i % 63) * 311.0);
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    EXPECT_GE(lo, 500.0 - 200.0);
    EXPECT_LE(hi, 500.0 + 200.0);
    EXPECT_GT(hi - lo, 60.0);  // Not flat.
}

TEST(ProceduralSource, MountainsRaiseTheRelief) {
    const auto hills = makeSource({.seed = 5, .mountains = 0.0});
    const auto alps = makeSource({.seed = 5, .mountains = 1.0});
    double hillMax = -1e9;
    double alpMax = -1e9;
    for (int i = 0; i < 3000; ++i) {
        const double x = (i % 60) * 500.0;
        const double z = (i / 60) * 500.0;
        hillMax = std::max(hillMax, hills->height(x, z));
        alpMax = std::max(alpMax, alps->height(x, z));
    }
    EXPECT_GT(alpMax, hillMax + 100.0);
}

TEST(ProceduralSource, FlatZoneIsFlat) {
    ProceduralParams params{.seed = 9, .amplitudeM = 400.0};
    params.flatZones.push_back({.x = 1000.0, .z = -500.0, .radiusM = 80.0, .blendM = 60.0});
    params.flatZones.push_back(
        {.x = -3000.0, .z = 0.0, .radiusM = 50.0, .blendM = 0.0, .heightM = 123.0});
    const auto source = makeSource(params);
    const auto natural = makeSource({.seed = 9, .amplitudeM = 400.0});
    const double centre = natural->height(1000.0, -500.0);
    for (double r = 0.0; r <= 80.0; r += 10.0) {
        EXPECT_NEAR(source->height(1000.0 + r, -500.0), centre, 1e-9);
        EXPECT_NEAR(source->height(1000.0, -500.0 - r), centre, 1e-9);
    }
    EXPECT_DOUBLE_EQ(source->height(-3000.0, 20.0), 123.0);
    // Far away the zones have no effect.
    EXPECT_DOUBLE_EQ(source->height(5000.0, 5000.0), natural->height(5000.0, 5000.0));
}

TEST(ProceduralSource, BoundedEdgesFadeAndClamp) {
    const Extent extent{4000.0, 2000.0};
    ProceduralParams params{.seed = 2, .amplitudeM = 300.0, .baseHeightM = 50.0};
    params.edge = EdgeMode::Fade;
    const auto fade = makeSource(params, extent);
    EXPECT_NEAR(fade->height(2000.0, 0.0), 50.0, 1e-9);  // On the border: base height.
    EXPECT_NEAR(fade->height(5000.0, 0.0), 50.0, 1e-9);  // Outside: clamped to the border.
    EXPECT_NEAR(fade->height(0.0, -1000.0), 50.0, 1e-9);

    params.edge = EdgeMode::Water;
    params.waterLevelM = 20.0;
    const auto water = makeSource(params, extent);
    EXPECT_LT(water->height(2000.0, 300.0), 20.0);

    params.edge = EdgeMode::Cliff;
    const auto cliff = makeSource(params, extent);
    const auto unbounded = makeSource(params);
    EXPECT_DOUBLE_EQ(cliff->height(2000.0, 300.0), unbounded->height(2000.0, 300.0));
    EXPECT_DOUBLE_EQ(cliff->height(2600.0, 300.0), unbounded->height(2000.0, 300.0));
    ASSERT_TRUE(cliff->extent().has_value());
    EXPECT_EQ(*cliff->extent(), extent);
}

TEST(ProceduralSource, LevelZeroGridEqualsHeight) {
    const auto source = makeSource({.seed = 77, .octaves = 12, .featureSizeM = 300.0});
    const ChunkLayout layout;
    std::vector<double> grid(9);
    const double step = layout.spacing(0);
    source->sampleGrid(1234.0, -876.0, step, 3, 3, grid);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_EQ(grid[static_cast<std::size_t>(r * 3 + c)],
                      source->height(1234.0 + c * step, -876.0 + r * step));
        }
    }
}

// --- Chunks (FR-TER-04, FR-TER-06, FR-TER-08) ----------------------------------------------------

TEST(ChunkLayout, FinerSpacingsAddLevelsNearTheCameraOnly) {
    EXPECT_EQ(layoutForSpacing(kDefaultMeshSpacingM), ChunkLayout{});
    const double root = ChunkLayout{}.size(ChunkLayout{}.lodCount - 1);
    for (const double spacing : kMeshSpacingsM) {
        const ChunkLayout layout = layoutForSpacing(spacing);
        EXPECT_TRUE(isMeshSpacing(spacing));
        EXPECT_TRUE(layout.isValid()) << spacing;
        EXPECT_DOUBLE_EQ(layout.spacing(0), spacing);
        EXPECT_EQ(layout.gridSize, ChunkLayout{}.gridSize);
        EXPECT_DOUBLE_EQ(layout.size(layout.lodCount - 1), root) << spacing;  // Same reach.
    }
    EXPECT_EQ(layoutForSpacing(0.5).lodCount, 10);
    for (const double bad : {0.0, 0.3, 3.0, 8.0, -1.0}) {
        EXPECT_FALSE(isMeshSpacing(bad));
        EXPECT_EQ(layoutForSpacing(bad), ChunkLayout{});
    }

    // Low over the ground each halving draws one ring of small chunks more; high up the
    // finer levels are not reached and nothing changes.
    const auto drawn = [](double spacing, double y) {
        const auto ready = [](const ChunkId&) {
            return std::optional<std::pair<float, float>>{{0.0F, 0.0F}};
        };
        return selectChunks(layoutForSpacing(spacing), std::nullopt, 0.0, y, 0.0, {}, ready)
            .render.size();
    };
    const std::size_t low = drawn(2.0, 30.0);
    EXPECT_GT(drawn(0.5, 30.0), low);
    EXPECT_LT(static_cast<double>(drawn(0.5, 30.0)), 1.35 * static_cast<double>(low));
    EXPECT_EQ(drawn(0.5, 1000.0), drawn(2.0, 1000.0));
}

TEST(ChunkLayout, Geometry) {
    const ChunkLayout layout;
    EXPECT_TRUE(layout.isValid());
    EXPECT_DOUBLE_EQ(layout.size(0), 256.0);
    EXPECT_DOUBLE_EQ(layout.spacing(0), 2.0);
    EXPECT_DOUBLE_EQ(layout.size(3), 2048.0);
    EXPECT_GE(layout.lodCount, 5);
    EXPECT_FALSE((ChunkLayout{.gridSize = 100}).isValid());
    EXPECT_FALSE((ChunkLayout{.lodCount = 4}).isValid());

    const ChunkId id{2, -3, 5};
    EXPECT_EQ(id.parent(), (ChunkId{3, -2, 2}));
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(id.child(i).parent(), id);
    }
    const ChunkBounds b = chunkBounds(layout, id);
    EXPECT_DOUBLE_EQ(b.minX, -3 * 1024.0);
    EXPECT_DOUBLE_EQ(b.maxZ, 6 * 1024.0);
}

TEST(ChunkIndices, ValidAndCounterClockwise) {
    constexpr int n = 17;
    const auto indices = buildChunkIndices(n);
    EXPECT_EQ(indices.size(), static_cast<std::size_t>((n - 1) * (n - 1) * 6 + 4 * (n - 1) * 6));
    const std::size_t vertexCount = n * n + 4 * n;
    for (const auto i : indices) {
        EXPECT_LT(i, vertexCount);
    }
    // Grid triangles face up: positions (u, h=0, v), so the normal's y must be > 0.
    for (std::size_t t = 0; t < static_cast<std::size_t>((n - 1) * (n - 1) * 2); ++t) {
        const auto pos = [&](std::uint16_t i) { return std::pair<double, double>(i % n, i / n); };
        const auto [ax, az] = pos(indices[3 * t]);
        const auto [bx, bz] = pos(indices[3 * t + 1]);
        const auto [cx, cz] = pos(indices[3 * t + 2]);
        // y of (b − a) × (c − a) with y = 0 everywhere: (bz−az)(cx−ax) − (bx−ax)(cz−az).
        EXPECT_GT((bz - az) * (cx - ax) - (bx - ax) * (cz - az), 0.0) << t;
    }
}

TEST(ChunkIndices, SkirtsFaceOutward) {
    constexpr int n = 9;
    const auto indices = buildChunkIndices(n);
    const std::size_t gridTris = (n - 1) * (n - 1) * 2;
    // Positions: grid vertex (col, 0, row); skirt vertex below its grid vertex (y = −1).
    const auto position = [](std::uint16_t i) {
        struct P {
            double x, y, z;
        };
        if (i < n * n) {
            return P{static_cast<double>(i % n), 0.0, static_cast<double>(i / n)};
        }
        const int e = (i - n * n) / n;
        const int k = (i - n * n) % n;
        switch (e) {
            case 0: return P{static_cast<double>(k), -1.0, 0.0};
            case 1: return P{static_cast<double>(k), -1.0, n - 1.0};
            case 2: return P{0.0, -1.0, static_cast<double>(k)};
            default: return P{n - 1.0, -1.0, static_cast<double>(k)};
        }
    };
    const double mid = (n - 1) / 2.0;
    for (std::size_t t = gridTris; t < indices.size() / 3; ++t) {
        const auto a = position(indices[3 * t]);
        const auto b = position(indices[3 * t + 1]);
        const auto c = position(indices[3 * t + 2]);
        const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        const double nx = uy * vz - uz * vy;
        const double nz = ux * vy - uy * vx;
        const double cxm = (a.x + b.x + c.x) / 3.0 - mid;
        const double czm = (a.z + b.z + c.z) / 3.0 - mid;
        EXPECT_GT(nx * cxm + nz * czm, 0.0) << "triangle " << t;
    }
}

TEST(ChunkMesh, BordersOfNeighboursMatch) {
    const auto source = makeSource({.seed = 31});
    const ChunkLayout layout;
    const ChunkMesh left = buildChunkMesh(*source, layout, {0, 4, -2});
    const ChunkMesh right = buildChunkMesh(*source, layout, {0, 5, -2});
    const ChunkMesh below = buildChunkMesh(*source, layout, {0, 4, -1});
    const int n = layout.gridSize;
    for (int i = 0; i < n; ++i) {
        const auto at = [n](const ChunkMesh& m, int col, int row) {
            return m.vertices[static_cast<std::size_t>(row * n + col)];
        };
        EXPECT_EQ(at(left, n - 1, i).height, at(right, 0, i).height);
        EXPECT_EQ(at(left, n - 1, i).normal, at(right, 0, i).normal);
        EXPECT_EQ(at(left, i, n - 1).height, at(below, i, 0).height);
    }
    // Coarser level shares the lattice: its vertices equal level-0 heights there up to the
    // omitted fine detail (small compared with the amplitude).
    const ChunkMesh coarse = buildChunkMesh(*source, layout, {1, 2, -1});
    EXPECT_NEAR(coarse.vertices[0].height, left.vertices[0].height, 5.0);
    EXPECT_LE(left.minHeight, left.maxHeight);
    for (std::size_t i = static_cast<std::size_t>(n * n); i < left.vertices.size(); ++i) {
        EXPECT_LT(left.vertices[i].height, left.maxHeight);  // Skirts hang below.
    }
}

TEST(ChunkMesh, NormalsAndMaterials) {
    double x = 0.0, y = 0.0, z = 0.0;
    decodeNormal(encodeNormal(0.0, 1.0, 0.0), x, y, z);
    EXPECT_NEAR(y, 1.0, 1e-9);
    decodeNormal(encodeNormal(0.6, 0.0, -0.8), x, y, z);
    EXPECT_NEAR(x, 0.6, 1e-4);
    EXPECT_NEAR(z, -0.8, 1e-4);
    decodeNormal(encodeNormal(0.3, -0.9, 0.316), x, y, z);
    EXPECT_LT(y, -0.85);

    const auto weights = [](std::uint32_t m) {
        return std::array<int, 4>{static_cast<int>(m & 255U), static_cast<int>((m >> 8U) & 255U),
                                  static_cast<int>((m >> 16U) & 255U), static_cast<int>(m >> 24U)};
    };
    const auto sum = [&](std::uint32_t m) {
        const auto w = weights(m);
        return w[0] + w[1] + w[2] + w[3];
    };
    const auto flatGrass = weights(classifyMaterial(100.0, 1.0, std::nullopt, 2000.0));
    EXPECT_EQ(flatGrass[0], 255);
    const auto cliff = weights(classifyMaterial(100.0, 0.5, std::nullopt, 2000.0));
    EXPECT_GT(cliff[1], 200);
    const auto beach = weights(classifyMaterial(10.5, 1.0, 10.0, 2000.0));
    EXPECT_GT(beach[2], 200);
    const auto peak = weights(classifyMaterial(2500.0, 0.98, std::nullopt, 2000.0));
    EXPECT_GT(peak[3], 200);
    for (double h = -10.0; h < 3000.0; h += 97.0) {
        for (double ny = 0.0; ny <= 1.0; ny += 0.1) {
            EXPECT_EQ(sum(classifyMaterial(h, ny, 5.0, 1500.0)), 255);
        }
    }
}

TEST(ChunkMesh, BoundedTerrainClampsAndWalls) {
    const Extent extent{1000.0, 1000.0};
    const auto source = makeSource({.seed = 4, .edge = EdgeMode::Cliff}, extent);
    const ChunkLayout layout;
    EXPECT_TRUE(chunkInExtent(layout, {0, 1, 1}, extent));
    EXPECT_FALSE(chunkInExtent(layout, {0, 2, 0}, extent));  // x ∈ [512, 768] > 500
    EXPECT_FALSE(chunkInExtent(layout, {0, -3, 0}, extent));
    const ChunkMesh edge = buildChunkMesh(*source, layout, {0, 1, 0});  // x ∈ [256, 512]
    const int n = layout.gridSize;
    // East skirt (edge 3) is the wall: well below the chunk.
    const auto& wall = edge.vertices[static_cast<std::size_t>(n * n + 3 * n)];
    EXPECT_LE(wall.height, edge.minHeight - 199.0F);
}

// --- Selection (FR-TER-04/06) --------------------------------------------------------------------

TEST(ChunkSelection, RequestsRootsFirstThenRefinesNearCamera) {
    const ChunkLayout layout;
    std::set<ChunkId> ready;
    const auto readiness = [&](const ChunkId& id) -> std::optional<std::pair<float, float>> {
        if (ready.contains(id)) {
            return std::pair{0.0F, 10.0F};
        }
        return std::nullopt;
    };
    const SelectionSettings settings{.splitFactor = 2.0, .viewRadiusM = 20'000.0};
    auto sel = selectChunks(layout, std::nullopt, 100.0, 50.0, 100.0, settings, readiness);
    EXPECT_TRUE(sel.render.empty());
    ASSERT_FALSE(sel.wanted.empty());
    for (const auto& [id, priority] : sel.wanted) {
        EXPECT_EQ(id.lod, layout.lodCount - 1);
    }
    // Make everything ready level by level, as the streamer would.
    for (int round = 0; round < 20 && !sel.wanted.empty(); ++round) {
        for (const auto& [id, priority] : sel.wanted) {
            ready.insert(id);
        }
        sel = selectChunks(layout, std::nullopt, 100.0, 50.0, 100.0, settings, readiness);
    }
    EXPECT_TRUE(sel.wanted.empty());
    // The chunk under the camera is level 0; distant ones are coarse.
    const auto under = std::ranges::find_if(sel.render, [&](const ChunkId& id) {
        const auto b = chunkBounds(layout, id);
        return b.minX <= 100.0 && b.maxX >= 100.0 && b.minZ <= 100.0 && b.maxZ >= 100.0;
    });
    ASSERT_NE(under, sel.render.end());
    EXPECT_EQ(under->lod, 0);
    int coarse = 0;
    for (const auto& id : sel.render) {
        coarse += id.lod >= 4 ? 1 : 0;
    }
    EXPECT_GT(coarse, 0);
    EXPECT_LT(sel.render.size(), 600U);

    // No overlaps: no rendered chunk lies inside another rendered chunk.
    const std::set<ChunkId> rendered(sel.render.begin(), sel.render.end());
    EXPECT_EQ(rendered.size(), sel.render.size());
    for (const auto& id : sel.render) {
        for (ChunkId up = id; up.lod < layout.lodCount - 1;) {
            up = up.parent();
            EXPECT_FALSE(rendered.contains(up)) << "overlap at level " << up.lod;
        }
    }
}

TEST(ChunkSelection, BoundedExtentNeverRequestsOutside) {
    const ChunkLayout layout;
    const Extent extent{3000.0, 2000.0};
    std::set<ChunkId> ready;
    const auto readiness = [&](const ChunkId& id) -> std::optional<std::pair<float, float>> {
        if (ready.contains(id)) {
            return std::pair{0.0F, 0.0F};
        }
        return std::nullopt;
    };
    for (int round = 0; round < 20; ++round) {
        const auto sel = selectChunks(layout, extent, 0.0, 10.0, 0.0, {}, readiness);
        for (const auto& [id, p] : sel.wanted) {
            EXPECT_TRUE(chunkInExtent(layout, id, extent))
                << id.lod << " " << id.ix << " " << id.iz;
            ready.insert(id);
        }
        for (const auto& id : sel.render) {
            EXPECT_TRUE(chunkInExtent(layout, id, extent));
        }
    }
}

TEST(ChunkSelection, FarFromOrigin) {
    const ChunkLayout layout;
    std::set<ChunkId> ready;
    const auto readiness = [&](const ChunkId& id) -> std::optional<std::pair<float, float>> {
        if (ready.contains(id)) {
            return std::pair{0.0F, 0.0F};
        }
        return std::nullopt;
    };
    const double far = 3.7e6;  // 3 700 km east, 2 900 km south.
    ChunkSelection sel;
    for (int round = 0; round < 20; ++round) {
        sel = selectChunks(layout, std::nullopt, far, 30.0, 0.78 * far, {}, readiness);
        for (const auto& [id, p] : sel.wanted) {
            ready.insert(id);
        }
    }
    const auto under = std::ranges::find_if(sel.render, [&](const ChunkId& id) {
        const auto b = chunkBounds(layout, id);
        return b.minX <= far && b.maxX >= far && b.minZ <= 0.78 * far && b.maxZ >= 0.78 * far;
    });
    ASSERT_NE(under, sel.render.end());
    EXPECT_EQ(under->lod, 0);
    // A mesh there is still precise: vertex spacing is exact relative to the chunk origin.
    const auto source = makeSource({.seed = 8});
    const ChunkMesh mesh = buildChunkMesh(*source, layout, *under);
    EXPECT_FLOAT_EQ(mesh.vertices[1].height, static_cast<float>(source->height(
                                                 mesh.originX + layout.spacing(0), mesh.originZ)));
}

// --- Streaming (FR-TER-05) -----------------------------------------------------------------------

TEST(TerrainStreamer, GeneratesAsynchronouslyAndCaches) {
    ds::common::WorkerPool pool(4);
    TerrainStreamer streamer(makeSource({.seed = 11}), ChunkLayout{}, pool,
                             {.selection = {.viewRadiusM = 8000.0}});
    auto meshes = streamer.update(0.0, 100.0, 0.0);
    EXPECT_TRUE(meshes.empty());  // Nothing ready yet; update() did not block.
    for (int i = 0; i < 400 && (streamer.stats().inFlight > 0 || meshes.empty()); ++i) {
        pool.waitIdle();
        meshes = streamer.update(0.0, 100.0, 0.0);
    }
    pool.waitIdle();
    meshes = streamer.update(0.0, 100.0, 0.0);
    ASSERT_FALSE(meshes.empty());
    EXPECT_EQ(streamer.stats().inFlight, 0U);
    EXPECT_TRUE(std::ranges::any_of(meshes, [](const auto& m) { return m->id.lod == 0; }));
    const auto generated = streamer.stats().generated;
    for (int i = 0; i < 3; ++i) {
        meshes = streamer.update(0.0, 100.0, 0.0);
    }
    EXPECT_EQ(streamer.stats().generated, generated);  // Cached, not regenerated.
}

TEST(TerrainStreamer, RespectsCacheLimit) {
    ds::common::WorkerPool pool(4);
    TerrainStreamer streamer(makeSource({.seed = 12}), ChunkLayout{.gridSize = 33}, pool,
                             {.selection = {.viewRadiusM = 3000.0}, .cacheBytes = 400'000});
    for (int step = 0; step < 30; ++step) {
        const double x = step * 2000.0;
        for (int i = 0; i < 10; ++i) {
            pool.waitIdle();
            static_cast<void>(streamer.update(x, 50.0, 0.0));
        }
        const auto stats = streamer.stats();
        // Chunks in use this frame may exceed the limit; old ones must be gone.
        EXPECT_LT(stats.cachedChunks, 200U);
    }
}

TEST(TerrainStreamer, DestructionWithQueuedWorkIsSafe) {
    ds::common::WorkerPool pool(2);
    {
        TerrainStreamer streamer(makeSource(), ChunkLayout{}, pool);
        static_cast<void>(streamer.update(0.0, 0.0, 0.0));
    }
    pool.waitIdle();
    SUCCEED();
}

// --- Queries (FR-TER-07) -------------------------------------------------------------------------

TEST(TerrainQuery, MatchesLevelZeroMeshWithin1cm) {
    const auto source = makeSource({.seed = 21, .amplitudeM = 600.0, .mountains = 0.8});
    const ChunkLayout layout;
    const TerrainQuery query(source, layout, 1000.0);
    const ChunkMesh mesh = buildChunkMesh(*source, layout, {0, -2, 3});
    const int n = layout.gridSize;
    const double s = layout.spacing(0);
    double worst = 0.0;
    for (int row = 0; row + 1 < n; row += 7) {
        for (int col = 0; col + 1 < n; col += 5) {
            const auto h = [&](int c, int r) {
                return static_cast<double>(
                    mesh.vertices[static_cast<std::size_t>(r * n + c)].height);
            };
            for (const auto& [fx, fz] :
                 {std::pair{0.25, 0.75}, {0.8, 0.1}, {0.5, 0.5}, {0.0, 0.0}}) {
                // Mesh interpolation with the same diagonal split.
                const double meshH = fz >= fx ? h(col, row) + fz * (h(col, row + 1) - h(col, row)) +
                                                    fx * (h(col + 1, row + 1) - h(col, row + 1))
                                              : h(col, row) + fx * (h(col + 1, row) - h(col, row)) +
                                                    fz * (h(col + 1, row + 1) - h(col + 1, row));
                const double x = mesh.originX + (col + fx) * s;
                const double z = mesh.originZ + (row + fz) * s;
                worst = std::max(worst, std::abs(query.height(x, z) - meshH));
            }
        }
    }
    EXPECT_LT(worst, 0.01);
    EXPECT_DOUBLE_EQ(query.heightAmsl(10.0, 20.0), 1000.0 + query.height(10.0, 20.0));
}

TEST(TerrainQuery, NormalAgreesWithSlope) {
    const auto source = makeSource({.seed = 22, .amplitudeM = 500.0});
    const TerrainQuery query(source, ChunkLayout{}, 0.0);
    for (int i = 0; i < 50; ++i) {
        const double x = i * 13.3 + 0.4;
        const double z = i * 7.9 + 0.3;
        const Normal n = query.normal(x, z);
        EXPECT_NEAR(n.x * n.x + n.y * n.y + n.z * n.z, 1.0, 1e-12);
        EXPECT_GT(n.y, 0.0);
        // Small step inside the same triangle: dh ≈ −(nx/ny)·dx.
        const double dh = query.height(x + 0.01, z) - query.height(x, z);
        EXPECT_NEAR(dh, -n.x / n.y * 0.01, 1e-6);
    }
}

TEST(TerrainQuery, HeightMapPreview) {
    const auto source = makeSource({.seed = 23});
    const auto map = sampleHeightMap(*source, 0.0, 0.0, 10'000.0, 64, 32);
    ASSERT_EQ(map.size(), 64U * 32U);
    // The preview uses the same detail-limited sampling as a mesh with that spacing.
    const double step = 10'000.0 / 64;
    std::vector<double> corner(1);
    source->sampleGrid(-0.5 * step * 63, -0.5 * step * 31, step, 1, 1, corner);
    EXPECT_FLOAT_EQ(map[0], static_cast<float>(corner[0]));
    EXPECT_NEAR(map[0], source->height(-0.5 * step * 63, -0.5 * step * 31), 30.0);
}

// --- Performance (NFR-PERF-03) -------------------------------------------------------------------

TEST(TerrainPerformance, ChunkGenerationTime) {
    const auto source = makeSource({.seed = 99, .mountains = 0.6, .warp = 0.5});
    const ChunkLayout layout;
    const auto start = std::chrono::steady_clock::now();
    constexpr int kChunks = 6;
    for (int i = 0; i < kChunks; ++i) {
        static_cast<void>(buildChunkMesh(*source, layout, {0, i, 0}));
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count() /
        kChunks;
    std::printf("Chunk 129x129 (LOD 0): %.2f ms\n", ms);
    RecordProperty("chunk_ms", std::to_string(ms));
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    EXPECT_LE(ms, 10.0);  // NFR-PERF-03 (optimised builds only).
#endif
}

}  // namespace
