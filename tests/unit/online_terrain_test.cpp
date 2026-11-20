// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Online elevation terrain (M4 step 4): the block grid, in-memory payloads, chunk rebuilds
// when data arrives, bounded areas from the cache and block streaming, against the mock
// provider of tests/support/online_fixtures.hpp.

#include "ds/terrain/online_terrain.hpp"

#include <QJsonObject>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>

#include "ds/app/data_services.hpp"
#include "ds/app/mission_setup.hpp"
#include "ds/app/scene_config.hpp"
#include "ds/common/worker_pool.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/net/block_planner.hpp"
#include "ds/net/data_client.hpp"
#include "ds/net/disk_cache.hpp"
#include "ds/net/http_client.hpp"
#include "ds/net/quota.hpp"
#include "ds/terrain/raster_source.hpp"
#include "ds/terrain/streamer.hpp"

#include "online_fixtures.hpp"
#include "raster_fixtures.hpp"

namespace {

using namespace ds;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

// Montserrat-like origin in the mock surface's area.
constexpr geo::LatLon kOrigin{41.5934, 1.8376};

bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 10s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// Expected height of the mock surface at world (x, z) for a frame at @p origin.
double expected(const geo::LocalFrame& frame, double x, double z) {
    const geo::Geodetic g = frame.toGeodetic(geo::Enu{x, -z, 0.0});
    return test::demHeight(g.latitudeDeg, g.longitudeDeg) - frame.origin().heightM;
}

// Network stack of the tests: mock provider, cache in a temporary folder, in-memory ledger.
struct Online {
    test::MockDemServer server;
    test::TempDir dir{"online_terrain"};
    net::InMemoryCacheIndex index;
    net::DiskCache cache{dir.path() / "cache", index, std::uint64_t{1} << 30U};
    net::InMemoryUsageLedger ledger;
    net::HttpClient http;
    net::DataClient client{http, &cache, &ledger};
    net::ProviderConfig provider = server.provider();

    Online() { client.setApiKey(provider.key, "secret-key"); }

    bool download(const net::GeoRect& area) {
        std::atomic<bool> done{false};
        bool ok = false;
        auto job = client.downloadArea(provider, "", area, {},
                                       [&](net::NetResult<std::vector<std::string>> r) {
                                           ok = r.has_value();
                                           done.store(true);
                                       });
        return job && waitUntil([&] { return done.load(); }) && ok;
    }

    terrain::OnlineTerrainRequest request(std::optional<terrain::Extent> extent) const {
        terrain::OnlineTerrainRequest r;
        r.provider = provider;
        r.origin = kOrigin;
        r.extent = extent;
        return r;
    }
};

TEST(BlockGrid, BlockAtMatchesTheBlocksAround) {
    for (const double lat : {-60.0, -0.001, 0.0, 41.5934, 66.6}) {
        for (const double lon : {-179.99, -3.7, 0.0, 1.8376, 179.99}) {
            const net::BlockKey key = net::blockAt(lat, lon, 20.0);
            const net::GeoRect rect = net::blockRect(key, 20.0);
            EXPECT_TRUE(lat >= rect.south && lat <= rect.north && lon >= rect.west &&
                        lon <= rect.east)
                << lat << ", " << lon;
            const auto around = net::blocksAround(lat, lon, 30.0, 20.0);
            EXPECT_TRUE(std::ranges::any_of(
                around, [&](const net::Block& b) { return b.key == key && b.rect == rect; }))
                << lat << ", " << lon;
        }
    }
}

TEST(RasterMemory, OpensPayloadsAndPiecesOfALargerSurface) {
    const net::GeoRect area =
        net::GeoRect::around(kOrigin.latitudeDeg, kOrigin.longitudeDeg, 3.0, 3.0);
    const std::array<std::string, 1> payload{
        test::demGeoTiff(area.west, area.south, area.east, area.north)};
    terrain::RasterOptions options;
    options.origin = kOrigin;
    auto raster = terrain::RasterSource::openMemory(payload, options, "Mock DEM TEST30");
    ASSERT_TRUE(raster.has_value()) << raster.error().message;
    EXPECT_EQ((*raster)->metadata().crsCode, "EPSG:4326");
    EXPECT_NE((*raster)->describe().find("Mock DEM TEST30"), std::string::npos);
    const geo::LocalFrame frame((*raster)->metadata().origin);
    EXPECT_NEAR((*raster)->height(0.0, 0.0), 0.0, 1e-6);
    // A plane in degrees, bilinear between pixel centres: errors come from the waves only.
    EXPECT_NEAR((*raster)->height(400.0, -300.0), expected(frame, 400.0, -300.0), 0.5);

    // Unbounded piece: no extent, the origin may lie outside it given its altitude.
    terrain::RasterOptions piece;
    piece.origin = geo::LatLon{kOrigin.latitudeDeg - 0.1, kOrigin.longitudeDeg};
    piece.unbounded = true;
    EXPECT_EQ(terrain::RasterSource::openMemory(payload, piece, "piece").error().code,
              terrain::TerrainErrorCode::NotCovered);
    piece.originAltitudeM = 500.0;
    auto block = terrain::RasterSource::openMemory(payload, piece, "piece");
    ASSERT_TRUE(block.has_value()) << block.error().message;
    EXPECT_FALSE((*block)->extent().has_value());

    const std::array<std::string, 1> junk{"<html>quota</html>"};
    EXPECT_FALSE(terrain::RasterSource::openMemory(junk, options, "junk").has_value());
}

// A flat source whose eastern half rises when "data arrives".
class ArrivingSource final : public terrain::HeightSource {
public:
    [[nodiscard]] double height(double x, double) const override {
        return arrived_.load() && x > 0.0 ? 100.0 : 0.0;
    }
    [[nodiscard]] std::uint64_t revision() const noexcept override {
        return arrived_.load() ? 1 : 0;
    }
    [[nodiscard]] std::vector<terrain::ChangedArea> changesSince(std::uint64_t r) const override {
        if (r >= 1 || !arrived_.load()) {
            return {};
        }
        return {terrain::ChangedArea{1, 0.0, -1e6, 1e6, 1e6}};
    }
    [[nodiscard]] std::string describe() const override { return "arriving"; }
    void arrive() { arrived_.store(true); }

private:
    std::atomic<bool> arrived_{false};
};

TEST(StreamerChanges, MeshesOverChangedDataAreRebuilt) {
    common::WorkerPool pool(2);
    auto source = std::make_shared<ArrivingSource>();
    terrain::ChunkLayout layout{.gridSize = 33};
    terrain::StreamerSettings settings;
    settings.selection.viewRadiusM = 2000.0;
    terrain::TerrainStreamer streamer(source, layout, pool, settings);
    auto settle = [&] {
        for (int i = 0; i < 400; ++i) {
            pool.waitIdle();
            (void)streamer.update(0.0, 50.0, 0.0);
            const auto s = streamer.stats();
            if (s.inFlight == 0 && s.stale == 0 && s.cachedChunks > 0) {
                return true;
            }
        }
        const auto s = streamer.stats();
        ADD_FAILURE() << "cached " << s.cachedChunks << " in flight " << s.inFlight << " stale "
                      << s.stale;
        return false;
    };
    ASSERT_TRUE(settle());
    const std::size_t before = streamer.stats().generated;
    EXPECT_EQ(streamer.stats().stale, 0U);
    auto maxHeight = [&] {
        float h = 0.0F;
        for (const auto& mesh : streamer.update(0.0, 50.0, 0.0)) {
            h = std::max(h, mesh->maxHeight);
        }
        return h;
    };
    EXPECT_FLOAT_EQ(maxHeight(), 0.0F);

    source->arrive();
    (void)streamer.update(0.0, 50.0, 0.0);
    EXPECT_GT(streamer.stats().stale, 0U);
    ASSERT_TRUE(settle());
    EXPECT_GT(streamer.stats().generated, before);
    EXPECT_FLOAT_EQ(maxHeight(), 100.0F);
}

TEST(OnlineTerrain, BoundedAreaComesFromTheCacheOnly) {
    Online online;
    const auto request = online.request(terrain::Extent{3000.0, 2000.0});
    // Not downloaded: no request is made to open it.
    auto missing = terrain::openOnlineTerrain(online.client, request);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, terrain::TerrainErrorCode::NotCached);
    EXPECT_EQ(online.server.requestCount(), 0U);

    auto area = terrain::onlineRequiredArea(request);
    ASSERT_TRUE(area.has_value());
    ASSERT_TRUE(online.download(*area));
    EXPECT_EQ(online.server.requestCount(), 1U);  // One request for the whole area.

    auto opened = terrain::openOnlineTerrain(online.client, request);
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    EXPECT_FALSE(opened->stream);
    EXPECT_EQ(opened->source->extent(), (terrain::Extent{3000.0, 2000.0}));
    EXPECT_EQ(opened->dataHash.size(), 64U);
    EXPECT_EQ(opened->online.pieces, 1);
    EXPECT_EQ(opened->online.verticalDatum, "EGM2008");
    EXPECT_NE(opened->online.attribution.find("Test DEM"), std::string::npos);
    EXPECT_NEAR(opened->data.origin.heightM,
                test::demHeight(kOrigin.latitudeDeg, kOrigin.longitudeDeg), 1.0);
    const geo::LocalFrame frame(opened->data.origin);
    for (const auto& [x, z] : {std::pair{0.0, 0.0}, {1400.0, 900.0}, {-1450.0, -950.0}}) {
        EXPECT_NEAR(opened->source->height(x, z), expected(frame, x, z), 1.0) << x << ", " << z;
    }

    // Offline (a restart without network): identical terrain from the cache (NFR test plan).
    online.http.setOffline(true);
    auto again = terrain::openOnlineTerrain(online.client, request);
    ASSERT_TRUE(again.has_value()) << again.error().message;
    EXPECT_EQ(again->dataHash, opened->dataHash);
    EXPECT_EQ(again->source->height(700.0, -300.0), opened->source->height(700.0, -300.0));

    // Outside the dataset's coverage.
    auto covered = request;
    covered.provider.datasets[0].coverage = net::GeoRect{-10.0, 35.0, 0.0, 40.0};
    EXPECT_EQ(terrain::onlineRequiredArea(covered).error().code,
              terrain::TerrainErrorCode::NotCovered);
}

TEST(OnlineTerrain, StreamingFetchesBlocksAroundTheDrone) {
    Online online;
    const auto request = online.request(std::nullopt);
    EXPECT_EQ(terrain::openOnlineTerrain(online.client, request).error().code,
              terrain::TerrainErrorCode::NotCached);
    auto area = terrain::onlineRequiredArea(request);  // The block under the origin.
    ASSERT_TRUE(area.has_value());
    ASSERT_TRUE(online.download(*area));
    auto opened = terrain::openOnlineTerrain(online.client, request);
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    ASSERT_TRUE(opened->stream);
    terrain::OnlineStreamSource& stream = *opened->stream;
    EXPECT_FALSE(stream.extent().has_value());
    EXPECT_EQ(stream.revision(), 0U);
    EXPECT_EQ(stream.status().loadedBlocks, 1);
    const geo::LocalFrame frame(stream.originMetadata().origin);
    EXPECT_NEAR(stream.height(0.0, 0.0), 0.0, 1e-6);

    // A point two blocks east: the lowest height of the origin block until its block arrives.
    const double far = 8500.0;
    const double before = stream.height(far, 0.0);
    EXPECT_NEAR(before, stream.originMetadata().minHeightM - frame.origin().heightM, 1e-9);
    stream.focus(far, 0.0);
    ASSERT_TRUE(waitUntil([&] {
        stream.focus(far, 0.0);
        const auto s = stream.status();
        return s.pendingBlocks == 0 && s.loadedBlocks >= 3;
    })) << stream.status().lastError;
    EXPECT_GT(stream.revision(), 0U);
    const auto changes = stream.changesSince(0);
    EXPECT_EQ(changes.size(), stream.revision());
    EXPECT_TRUE(std::ranges::any_of(changes, [&](const terrain::ChangedArea& c) {
        return far >= c.minX && far <= c.maxX && c.minZ <= 0.0 && c.maxZ >= 0.0;
    }));
    EXPECT_NEAR(stream.height(far, 0.0), expected(frame, far, 0.0), 1.0);
    // Grids across block borders agree with single heights.
    std::vector<double> grid(9 * 9);
    stream.sampleGrid(far - 2100.0, -40.0, 2.0, 9, 9, grid);
    EXPECT_DOUBLE_EQ(grid[40], stream.height(far - 2100.0 + 8.0, -40.0 + 8.0));
}

TEST(OnlineTerrain, StreamingStopsAtTheQuotaAndReportsWhy) {
    Online online;
    const auto request = online.request(std::nullopt);
    ASSERT_TRUE(online.download(*terrain::onlineRequiredArea(request)));
    auto opened = terrain::openOnlineTerrain(online.client, request);
    ASSERT_TRUE(opened.has_value());
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(online.ledger.record(
            net::ProviderCall{online.provider.key, now, "TEST30", std::nullopt, 200, 1}));
    }
    const std::size_t calls = online.server.requestCount();
    opened->stream->focus(8500.0, 0.0);
    const auto status = opened->stream->status();
    EXPECT_EQ(status.pendingBlocks, 0);
    EXPECT_GT(status.failedBlocks, 0);
    EXPECT_NE(status.lastError.find("daily limit"), std::string::npos) << status.lastError;
    EXPECT_EQ(online.server.requestCount(), calls);  // Cache only: no call made.
}

// --- Application side ----------------------------------------------------------------------

app::TerrainConfig onlineConfig() {
    app::TerrainConfig c;
    c.kind = app::TerrainKind::Online;
    c.online.provider = "mockdem";
    c.online.dataset = "TEST30";
    c.origin = {kOrigin.latitudeDeg, kOrigin.longitudeDeg, 0.0};
    c.extent = terrain::Extent{3000.0, 2000.0};
    return c;
}

TEST(OnlineApp, ConfigJsonAndPresetRoundTrip) {
    app::TerrainConfig c = onlineConfig();
    c.online.originAltitudeM = 720.0;
    c.online.edge = terrain::EdgeMode::Fade;
    const auto back = app::terrainFromJson(app::terrainToJson(c));
    ASSERT_TRUE(back.has_value()) << back.error().message;
    EXPECT_EQ(*back, c);
    const auto record = app::toPresetRecord(c, "user.montserrat", "Montserrat");
    EXPECT_EQ(record.sourceType, "online");
    const auto preset = app::fromPresetRecord(record);
    ASSERT_TRUE(preset.has_value()) << preset.error().message;
    EXPECT_EQ(preset->online, c.online);

    QJsonObject json = app::terrainToJson(c);
    QJsonObject online = json.value(QStringLiteral("online")).toObject();
    online.insert(QStringLiteral("provider"), QString());
    json.insert(QStringLiteral("online"), online);
    EXPECT_FALSE(app::terrainFromJson(json).has_value());
}

TEST(OnlineApp, MissionTerrainUsesTheOriginAndADefaultArea) {
    missions::MissionDefinition m;
    m.id = "online";
    m.terrain.source = missions::TerrainSource::Online;
    m.terrain.elevationProvider = "mockdem";
    m.terrain.origin = kOrigin;
    auto config = app::missionTerrain(m);
    ASSERT_TRUE(config.has_value()) << config.error().message;
    EXPECT_EQ(config->kind, app::TerrainKind::Online);
    EXPECT_EQ(config->online.provider, "mockdem");
    EXPECT_TRUE(config->online.dataset.empty());               // The provider's default.
    EXPECT_FALSE(config->online.originAltitudeM.has_value());  // AGL: the ground at the origin.
    EXPECT_EQ(config->extent, (terrain::Extent{app::kDefaultOnlineMissionExtentM,
                                               app::kDefaultOnlineMissionExtentM}));
    m.terrain.originAltitudeRef = items::AltitudeRef::Amsl;
    m.terrain.originAltitudeM = 750.0;
    m.terrain.extent = terrain::Extent{2000.0, 1500.0};
    config = app::missionTerrain(m);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->online.originAltitudeM, 750.0);
    EXPECT_EQ(config->extent, (terrain::Extent{2000.0, 1500.0}));
}

TEST(OnlineApp, DataServicesDecideWhetherOnlineMissionsArePlayable) {
    test::MockDemServer server;
    test::TempDir dir("online_app");
    std::filesystem::create_directories(dir / "providers");
    std::ofstream(dir / "providers" / "mockdem.json") << server.providerJson();
    EXPECT_EQ(app::DataServices::keyVariable("open-topo"), "DS_API_KEY_OPEN_TOPO");
    ::setenv("DS_API_KEY_MOCKDEM", "from-environment", 1);
    auto data = app::DataServices::create(
        {.dataDirectory = dir / "data", .providerPresets = dir / "providers"});
    ::unsetenv("DS_API_KEY_MOCKDEM");
    ASSERT_TRUE(data.has_value()) << data.error().message;
    app::DataServices& d = **data;
    ASSERT_EQ(d.elevationProviders().size(), 1U);
    EXPECT_TRUE(d.hasApiKey("mockdem"));  // From the environment.

    missions::MissionDefinition m;
    m.id = "online";
    m.terrain.source = missions::TerrainSource::Online;
    m.terrain.elevationProvider = "mockdem";
    m.terrain.origin = kOrigin;
    m.terrain.extent = terrain::Extent{2000.0, 2000.0};
    EXPECT_FALSE(app::unplayableReason(m, &d).has_value());  // Downloaded at launch.
    d.setApiKey("mockdem", "");
    EXPECT_NE(app::unplayableReason(m, &d).value_or("").find("API key"), std::string::npos);
    m.terrain.elevationProvider = "nobody";
    EXPECT_NE(app::unplayableReason(m, &d).value_or("").find("unknown elevation provider"),
              std::string::npos);

    // Not downloaded: buildTerrain never fetches.
    auto built = app::buildTerrain(onlineConfig(), &d);
    ASSERT_FALSE(built.has_value());
    EXPECT_NE(built.error().message.find("not downloaded"), std::string::npos);
    EXPECT_EQ(server.requestCount(), 0U);
    EXPECT_FALSE(app::buildTerrain(onlineConfig()).has_value());  // No data services.

    // Declined: nothing is downloaded.
    d.setApiKey("mockdem", "secret");
    int asked = 0;
    d.setConfirmDownload([&](QWidget*, const app::DownloadRequest& r) {
        ++asked;
        EXPECT_EQ(r.estimate.uncachedRequests, 1);
        EXPECT_NE(app::downloadQuestion(r).find("uses 1 of your 50 remaining calls today"),
                  std::string::npos)
            << app::downloadQuestion(r);
        return false;
    });
    auto request = app::onlineRequest(onlineConfig(), d);
    ASSERT_TRUE(request.has_value());
    auto ensured = app::ensureOnlineData(nullptr, d, *request);
    ASSERT_TRUE(ensured.has_value()) << ensured.error().message;
    EXPECT_FALSE(*ensured);
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(server.requestCount(), 0U);
}

}  // namespace
