// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Online imagery (M4 step 8, FR-TER-O10, FR-GEO-02b): imagery provider configuration, tile
// URLs for XYZ, TMS, WMTS and WMS, tile fetching through the cache, and imagery tiles
// reprojected from Web Mercator into the scene frame, against the mock tile service of
// tests/support/imagery_fixtures.hpp.

#include "ds/terrain/online_imagery.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <numbers>
#include <string>
#include <thread>

#include "ds/common/worker_pool.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/geo/web_mercator.hpp"
#include "ds/net/data_client.hpp"
#include "ds/net/disk_cache.hpp"
#include "ds/net/http_client.hpp"
#include "ds/net/provider.hpp"
#include "ds/terrain/chunk.hpp"
#include "ds/terrain/ortho_streamer.hpp"

#include "imagery_fixtures.hpp"
#include "raster_fixtures.hpp"

namespace {

using namespace ds;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr double kLat = 40.4168;
constexpr double kLon = -3.7038;

net::NetResult<net::Payload> fetchTile(net::DataClient& client, const net::ProviderConfig& p,
                                       const geo::TileId& tile) {
    std::promise<net::NetResult<net::Payload>> promise;
    auto future = promise.get_future();
    client.fetch(p, net::TileRequest{"", tile, false}, 0.0,
                 [&](net::NetResult<net::Payload> r) { promise.set_value(std::move(r)); });
    EXPECT_EQ(future.wait_for(10s), std::future_status::ready);
    return future.get();
}

// Builds @p key until it is complete (the service tiles arrive in the background).
terrain::OrthoBuild buildComplete(const terrain::OnlineImagerySource& source,
                                  const terrain::OrthoTileKey& key) {
    terrain::OrthoBuild b;
    for (int i = 0; i < 500; ++i) {
        b = source.build(key, 0.0);
        if (b.complete) {
            return b;
        }
        std::this_thread::sleep_for(10ms);
    }
    ADD_FAILURE() << "tile never completed";
    return b;
}

// --- Provider configuration ---------------------------------------------------------------

TEST(ImageryProvider, ShippedPresetsAreValid) {
    auto presets = net::loadProviderPresets(fs::path(DS_TEST_ASSETS_DIR) / "providers");
    ASSERT_TRUE(presets) << presets.error().message;
    int imagery = 0;
    for (const net::ProviderConfig& p : *presets) {
        if (!p.isImagery()) {
            continue;
        }
        ++imagery;
        SCOPED_TRACE(p.key);
        EXPECT_TRUE(net::isTileProtocol(p.protocol));
        EXPECT_EQ(p.crs, "EPSG:3857");
        EXPECT_FALSE(p.requiresKey);  // Open national services: no key to ship (§4.6).
        EXPECT_FALSE(p.attribution.empty());
        EXPECT_FALSE(p.licence.empty());
        ASSERT_FALSE(p.datasets.empty());
        for (const net::DatasetConfig& d : p.datasets) {
            ASSERT_TRUE(d.maxZoom.has_value());
            EXPECT_TRUE(d.coverage.has_value());
            EXPECT_FALSE(d.attribution.empty());
        }
        auto again = net::parseProvider(net::toJson(p));
        ASSERT_TRUE(again) << again.error().message;
        EXPECT_EQ(net::toJson(*again), net::toJson(p));
    }
    EXPECT_EQ(imagery, 5);  // PNOA, SWISSIMAGE, USGS, OpenStreetMap, Géoplateforme.
}

TEST(ImageryProvider, OpenStreetMapPresetFollowsTheTileUsagePolicy) {
    auto presets = net::loadProviderPresets(fs::path(DS_TEST_ASSETS_DIR) / "providers");
    ASSERT_TRUE(presets) << presets.error().message;
    const auto it = std::ranges::find(*presets, "osm-standard", &net::ProviderConfig::key);
    ASSERT_NE(it, presets->end());
    const net::ProviderConfig& osm = *it;
    // operations.osmfoundation.org/policies/tiles: HTTPS on tile.openstreetmap.org only, cache
    // for at least 7 days without cache headers, no offline use, modest request rates.
    EXPECT_EQ(osm.urlTemplate, "https://tile.openstreetmap.org/{z}/{x}/{y}.png");
    EXPECT_FALSE(osm.insecureHttp);
    EXPECT_TRUE(osm.allowCache);
    EXPECT_GE(osm.maxAgeDays.value_or(0), 7);
    EXPECT_FALSE(osm.serveStale);
    EXPECT_LE(osm.maxConcurrent, 2);
    EXPECT_NE(osm.attribution.find("© OpenStreetMap contributors"), std::string::npos);
    auto url = net::DataClient::cacheKey(osm, osm.datasets.at(0),
                                         geo::TileId{.x = 2128, .y = 1459, .z = 12});
    ASSERT_TRUE(url) << url.error().message;
    EXPECT_EQ(*url, "https://tile.openstreetmap.org/12/2128/1459.png");
}

TEST(ImageryProvider, GeoplateformePresetAsksForWebMercatorTiles) {
    auto presets = net::loadProviderPresets(fs::path(DS_TEST_ASSETS_DIR) / "providers");
    ASSERT_TRUE(presets) << presets.error().message;
    const auto it = std::ranges::find(*presets, "geoplateforme-imagery", &net::ProviderConfig::key);
    ASSERT_NE(it, presets->end());
    const net::ProviderConfig& gp = *it;
    const geo::TileId tile{.x = 17009, .y = 11693, .z = 15};  // Chamonix.
    auto photo = net::DataClient::cacheKey(gp, *gp.dataset("ORTHOIMAGERY.ORTHOPHOTOS"), tile);
    ASSERT_TRUE(photo) << photo.error().message;
    EXPECT_EQ(*photo,
              "https://data.geopf.fr/wmts?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&"
              "LAYER=ORTHOIMAGERY.ORTHOPHOTOS&STYLE=normal&FORMAT=image/jpeg&"
              "TILEMATRIXSET=PM_0_19&TILEMATRIX=15&TILEROW=11693&TILECOL=17009");
    const net::DatasetConfig* plan = gp.dataset("GEOGRAPHICALGRIDSYSTEMS.PLANIGNV2");
    ASSERT_NE(plan, nullptr);
    auto map = net::DataClient::cacheKey(gp, *plan, tile);
    ASSERT_TRUE(map) << map.error().message;
    EXPECT_NE(map->find("&FORMAT=image/png&"), std::string::npos) << *map;
    for (const net::DatasetConfig& d : gp.datasets) {
        EXPECT_EQ(d.maxZoom, 19);  // PM_0_19.
    }
}

TEST(ImageryProvider, ValidationNamesTheProblem) {
    test::MockTileServer server;
    common::Json good = net::toJson(server.provider());
    ASSERT_TRUE(net::parseProvider(good)) << net::parseProvider(good).error().message;
    const auto fails = [&](auto change, std::string_view expected) {
        common::Json j = good;
        change(j);
        auto r = net::parseProvider(j);
        ASSERT_FALSE(r) << j.dump();
        EXPECT_NE(r.error().message.find(expected), std::string::npos) << r.error().message;
    };
    fails([](common::Json& j) { j["protocol"] = "rest_bbox"; }, "imagery providers use");
    fails([](common::Json& j) { j["crs"] = "EPSG:4326"; }, "EPSG:3857");
    fails([](common::Json& j) { j["urlTemplate"] = "http://x.org/{z}/{x}.png"; }, "{z}, {x}");
    fails(
        [](common::Json& j) {
            j["protocol"] = "wms";
            j["urlTemplate"] = "http://x.org/wms?{z}{x}{y}";
        },
        "{bbox}");
    fails([](common::Json& j) { j["tileSizePx"] = 300; }, "power of two");
    fails([](common::Json& j) { j["maxSizePx"] = 512; }, "is for bounding-box providers");
    fails([](common::Json& j) { j["datasets"][0].erase("maxZoom"); }, "maxZoom");
    fails(
        [](common::Json& j) { j["datasets"][0]["minZoom"] = 12, j["datasets"][0]["maxZoom"] = 10; },
        "less than minZoom");
    // Elevation providers keep bounding boxes and refuse tile placeholders.
    fails(
        [](common::Json& j) {
            j["kind"] = "elevation";
            j["crs"] = "EPSG:4326";
        },
        "elevation providers use rest_bbox");
}

// --- Tile URLs and payloads -----------------------------------------------------------------

TEST(ImageryProvider, TileUrlsForEachProtocol) {
    net::ProviderConfig p = test::MockTileServer().provider();
    p.insecureHttp = false;
    const net::DatasetConfig& d = p.datasets.front();
    const geo::TileId tile{3, 1, 2};
    p.urlTemplate = "https://t.org/{z}/{x}/{y}.png";
    EXPECT_EQ(*net::DataClient::cacheKey(p, d, tile), "https://t.org/2/3/1.png");
    p.protocol = "tms";  // Rows counted from the south: 2^2 − 1 − 1.
    EXPECT_EQ(*net::DataClient::cacheKey(p, d, tile), "https://t.org/2/3/2.png");
    p.protocol = "wmts";
    p.urlTemplate =
        "https://t.org/wmts?Layer={dataset}&TileMatrixSet=GoogleMapsCompatible&TileMatrix={z}&"
        "TileRow={y}&TileCol={x}";
    EXPECT_EQ(*net::DataClient::cacheKey(p, d, tile),
              "https://t.org/wmts?Layer=photo&TileMatrixSet=GoogleMapsCompatible&TileMatrix=2&"
              "TileRow=1&TileCol=3");
    p.protocol = "wms";
    p.urlTemplate = "https://t.org/wms?BBOX={bbox}&WIDTH={width}&HEIGHT={height}&CRS={crs}";
    const auto wms = net::DataClient::cacheKey(p, d, geo::TileId{0, 0, 1});
    ASSERT_TRUE(wms) << wms.error().message;
    EXPECT_EQ(*wms,
              "https://t.org/wms?BBOX=-20037508.343%2C0.000%2C0.000%2C20037508.343&WIDTH=256&"
              "HEIGHT=256&CRS=EPSG%3A3857");
    // Beyond the dataset's zoom range or the grid: refused before any request.
    EXPECT_FALSE(net::DataClient::cacheKey(p, d, geo::TileId{0, 0, 20}));
    EXPECT_FALSE(net::DataClient::cacheKey(p, d, geo::TileId{4, 0, 2}));

    EXPECT_TRUE(net::checkImagePayload(std::string("\xFF\xD8\xFF\xE0", 4)));
    EXPECT_TRUE(net::checkImagePayload(std::string("\x89PNG\r\n\x1A\n....", 12)));
    EXPECT_TRUE(net::checkImagePayload(std::string("RIFF\x10\x00\x00\x00WEBPVP8 ", 16)));
    const auto html = net::checkImagePayload("<html>Service unavailable</html>");
    ASSERT_FALSE(html);
    EXPECT_NE(html.error().message.find("Service unavailable"), std::string::npos);
    EXPECT_FALSE(net::checkImagePayload(""));
}

TEST(ImageryProvider, TilesAreFetchedThroughTheCache) {
    test::MockTileServer server(test::MockTileServer::Coverage{-4.0, 40.0, -3.0, 41.0});
    test::TempDir dir("imagery_cache");
    net::InMemoryCacheIndex index;
    net::DiskCache cache(dir.path(), index, 64U << 20U);
    net::HttpClient http;
    net::DataClient client(http, &cache, nullptr);
    const net::ProviderConfig p = server.provider();
    const auto tile = geo::tileAt(kLat, kLon, 15);
    ASSERT_TRUE(tile);
    auto first = fetchTile(client, p, *tile);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_FALSE(first->fromCache);
    EXPECT_TRUE(terrain::checkImageryTile(first->body, 256));
    auto second = fetchTile(client, p, *tile);
    ASSERT_TRUE(second);
    EXPECT_TRUE(second->fromCache);
    EXPECT_EQ(server.requestCount(), 1U);
    // Outside the service's coverage: 404.
    const auto far = fetchTile(client, p, *geo::tileAt(10.0, 10.0, 15));
    ASSERT_FALSE(far);
    EXPECT_EQ(far.error().httpStatus, 404);
    // Imagery is streamed, never downloaded by area or asked for as a bounding box.
    EXPECT_FALSE(client.estimateDownload(p, "", net::GeoRect::around(kLat, kLon, 1, 1)));
    std::promise<net::NetResult<net::Payload>> bbox;
    client.fetch(p, net::BBoxRequest{"", net::GeoRect::around(kLat, kLon, 1, 1)}, 0.0,
                 [&](net::NetResult<net::Payload> r) { bbox.set_value(std::move(r)); });
    EXPECT_FALSE(bbox.get_future().get());
    // A tile of the wrong size is not a usable tile.
    EXPECT_FALSE(
        terrain::checkImageryTile(test::pngTile(128, [](int, int, int) { return 0; }), 256));
}

// --- Imagery tiles ----------------------------------------------------------------------------

class OnlineImageryTest : public ::testing::Test {
protected:
    test::MockTileServer server_{test::MockTileServer::Coverage{-3.7045, 40.0, -3.0, 41.0}};
    test::TempDir dir_{"online_imagery"};
    net::InMemoryCacheIndex index_;
    net::DiskCache cache_{dir_.path(), index_, 256U << 20U};
    net::HttpClient http_;
    net::DataClient client_{http_, &cache_, nullptr};
    geo::Geodetic origin_{kLat, kLon, 650.0};

    std::shared_ptr<terrain::OnlineImagerySource> open(int maxZoom = 19) {
        auto s = terrain::OnlineImagerySource::open(
            client_, terrain::OnlineImageryRequest{server_.provider("mockimg", maxZoom), "", {}},
            origin_);
        EXPECT_TRUE(s) << s.error().message;
        return s ? *s : nullptr;
    }
};

TEST_F(OnlineImageryTest, ZoomFollowsTheTexelSizeWithinTheDatasetRange) {
    auto source = open(19);
    ASSERT_NE(source, nullptr);
    // 0.25 m texels at 40.4° N: zoom 19 pixels are 0.23 m.
    EXPECT_EQ(source->zoomFor(0), 19);
    EXPECT_EQ(source->zoomFor(1), 18);
    EXPECT_EQ(source->zoomFor(4), 15);
    auto capped = open(17);
    EXPECT_EQ(capped->zoomFor(0), 17);
    EXPECT_EQ(capped->zoomFor(3), 16);
    EXPECT_EQ(capped->metadata().crsCode, "EPSG:3857");
    EXPECT_NEAR(capped->metadata().resolutionM, 0.91, 0.01);
    // Not an imagery provider, or no such layer.
    auto bad = terrain::OnlineImagerySource::open(
        client_, terrain::OnlineImageryRequest{server_.provider(), "nope", {}}, origin_);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, terrain::TerrainErrorCode::ProviderError);
}

TEST_F(OnlineImageryTest, PixelsLandAtTheirWebMercatorPositions) {
    auto source = open(19);
    ASSERT_NE(source, nullptr);
    const terrain::OrthoTileKey key{0, 1, -2};
    const terrain::OrthoBuild first = source->build(key, 0.0);
    EXPECT_FALSE(first.complete);  // The service tiles are on their way.
    const terrain::OrthoBuild b = buildComplete(*source, key);
    ASSERT_NE(b.tile, nullptr);
    {
        const auto st = source->status();
        EXPECT_GT(st.downloaded, 0U) << st.fromCache << " cached, " << st.missing << " missing, "
                                     << st.failed << " failed: " << st.lastError;
    }
    // Every texel holds the red / green of the service pixel under its centre (≤ 1 px).
    const geo::LocalFrame frame(origin_);
    const int zoom = source->zoomFor(0);
    const int n = b.tile->sizePx;
    const double side = terrain::orthoTileSize(0);
    int checked = 0;
    for (int j = 3; j < n; j += 17) {
        for (int i = 5; i < n; i += 19) {
            const double x = (static_cast<double>(key.ix) + (i + 0.5) / n) * side;
            const double z = (static_cast<double>(key.iz) + (j + 0.5) / n) * side;
            const geo::Geodetic g = frame.toGeodetic(geo::Enu{x, -z, 0.0});
            const auto p = geo::toTilePoint(g.latitudeDeg, g.longitudeDeg, zoom);
            ASSERT_TRUE(p);
            const double gx = p->x * 256.0 - 0.5;
            const double gy = p->y * 256.0 - 0.5;
            const double ex = std::fmod(gx, 256.0);
            const double ey = std::fmod(gy, 256.0);
            if (ex < 2.0 || ex > 253.0 || ey < 2.0 || ey > 253.0) {
                continue;  // The ramp wraps at tile borders.
            }
            const std::uint8_t* t =
                &b.tile->rgba[(static_cast<std::size_t>(j) * static_cast<std::size_t>(n) +
                               static_cast<std::size_t>(i)) *
                              4U];
            ASSERT_EQ(t[3], 255);
            EXPECT_NEAR(t[0], ex, 1.5) << i << ", " << j;
            EXPECT_NEAR(t[1], ey, 1.5) << i << ", " << j;
            EXPECT_EQ(t[2], 128);
            ++checked;
        }
    }
    EXPECT_GT(checked, 100);
}

TEST_F(OnlineImageryTest, MissingTilesShowTheMaterialsAndBadTilesAreSkipped) {
    auto source = open(19);
    ASSERT_NE(source, nullptr);
    // The service's coverage starts with the zoom-19 column holding 3.7045° W, some 60 … 120 m
    // west of the origin.
    const auto edgeTile = geo::tileAt(kLat, -3.7045, source->zoomFor(0));
    ASSERT_TRUE(edgeTile);
    const double edgeX = geo::LocalFrame(origin_)
                             .toEnu(geo::Geodetic{kLat, geo::tileBounds(*edgeTile)->west, 0.0})
                             .east;
    ASSERT_LT(edgeX, -40.0);
    const double inTile = edgeX - 64.0 * std::floor(edgeX / 64.0);
    ASSERT_GT(inTile, 2.0);  // The edge crosses the tile, away from its sides.
    ASSERT_LT(inTile, 62.0);
    const terrain::OrthoTileKey west{0, static_cast<std::int64_t>(std::floor(edgeX / 64.0)) - 2, 0};
    const terrain::OrthoBuild none = buildComplete(*source, west);
    EXPECT_EQ(none.tile, nullptr);  // Only 404s there.
    EXPECT_GT(source->status().missing, 0U);
    // The tile across the edge: imagery east of it, transparent west of it.
    const terrain::OrthoTileKey across{0, static_cast<std::int64_t>(std::floor(edgeX / 64.0)), 0};
    const terrain::OrthoBuild part = buildComplete(*source, across);
    ASSERT_NE(part.tile, nullptr);
    const int n = part.tile->sizePx;
    EXPECT_EQ(part.tile->rgba[3], 0);                                            // West edge.
    EXPECT_EQ(part.tile->rgba[(static_cast<std::size_t>(n) - 1) * 4 + 3], 255);  // East edge.

    // A service tile of the wrong size is left out, with the reason.
    const auto bad = geo::tileAt(kLat - 0.01, kLon + 0.02, 19);
    server_.breakTile(bad->z, bad->x, bad->y);
    const geo::Enu at = geo::LocalFrame(origin_).toEnu(
        geo::Geodetic{0.5 * (geo::tileBounds(*bad)->south + geo::tileBounds(*bad)->north),
                      0.5 * (geo::tileBounds(*bad)->west + geo::tileBounds(*bad)->east), 0.0});
    const terrain::OrthoTileKey over{0, static_cast<std::int64_t>(std::floor(at.east / 64.0)),
                                     static_cast<std::int64_t>(std::floor(-at.north / 64.0))};
    static_cast<void>(buildComplete(*source, over));
    EXPECT_NE(source->status().lastError.find("128 × 128"), std::string::npos)
        << source->status().lastError;
}

TEST_F(OnlineImageryTest, OfflineUsesTheCacheAndFailuresAreRetried) {
    const terrain::OrthoTileKey key{2, 0, 0};
    {
        auto source = open(19);
        ASSERT_NE(buildComplete(*source, key).tile, nullptr);
    }
    const std::size_t requests = server_.requestCount();
    http_.setOffline(true);
    auto offline = open(19);
    const terrain::OrthoBuild cached = offline->build(key, 0.0);
    EXPECT_TRUE(cached.complete);  // Straight from the disk cache.
    ASSERT_NE(cached.tile, nullptr);
    EXPECT_GT(offline->status().fromCache, 0U);
    EXPECT_EQ(server_.requestCount(), requests);
    // Elsewhere nothing is cached: the build waits, with the reason.
    const terrain::OrthoBuild elsewhere = offline->build({2, 3, 3}, 0.0);
    EXPECT_FALSE(elsewhere.complete);
    EXPECT_NE(offline->status().lastError.find("offline"), std::string::npos)
        << offline->status().lastError;
    EXPECT_GT(offline->status().failed, 0U);
}

TEST_F(OnlineImageryTest, TheStreamerRebuildsTilesAsTheirDataArrives) {
    auto source = open(19);
    common::WorkerPool pool(2);
    terrain::ChunkLayout layout;
    terrain::OrthoStreamer streamer(source, layout, pool);
    auto mesh = std::make_shared<terrain::ChunkMesh>();
    mesh->id = terrain::ChunkId{0, 0, 0};
    const std::vector<std::shared_ptr<const terrain::ChunkMesh>> chunks{mesh};
    const auto keys = terrain::OrthoStreamer::tilesOf(mesh->id);
    bool ready = false;
    for (int frame = 0; frame < 1000 && !ready; ++frame) {
        streamer.update(chunks, 0.0, 0.0);
        ready = std::ranges::all_of(keys,
                                    [&](const terrain::OrthoTileKey& k) {
                                        return streamer.find(k) == terrain::OrthoTileState::Ready;
                                    }) &&
                streamer.stats().waitingTiles == 0;
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(ready);
    std::shared_ptr<const terrain::OrthoTile> tile;
    ASSERT_EQ(streamer.find(keys[5], &tile), terrain::OrthoTileState::Ready);
    EXPECT_EQ(tile->rgba[3], 255);
    // Built more than once: the first builds waited for the service tiles.
    EXPECT_GT(streamer.stats().built, keys.size());
}

}  // namespace
