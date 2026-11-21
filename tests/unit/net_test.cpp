// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ds/common/log.hpp"
#include "ds/common/sha256.hpp"
#include "ds/net/block_planner.hpp"
#include "ds/net/data_client.hpp"
#include "ds/net/disk_cache.hpp"
#include "ds/net/http_client.hpp"
#include "ds/net/provider.hpp"
#include "ds/net/quota.hpp"
#include "ds/net/url_template.hpp"

#include "mock_http_server.hpp"

namespace {

using namespace ds::net;
using ds::test::MockHttpServer;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

const std::string kTiff = std::string("II*\0", 4) + std::string(60, 'x');

/// Waits for a callback-based result.
template <typename T>
struct Waiter {
    std::promise<T> promise;
    std::future<T> future = promise.get_future();
    auto callback() {
        return [this](T value) { promise.set_value(std::move(value)); };
    }
    T get() {
        if (future.wait_for(10s) != std::future_status::ready) {
            ADD_FAILURE() << "timed out waiting for a callback";
            std::terminate();
        }
        return future.get();
    }
};

fs::path tempDir(std::string_view name) {
    const fs::path dir = fs::temp_directory_path() / std::format("ds_net_{}_{}", name, ::getpid());
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

HttpClientOptions fastOptions() {
    HttpClientOptions o;
    o.retry =
        RetryPolicy{.maxAttempts = 3, .baseDelay = 5ms, .maxDelay = 20ms, .maxRetryAfter = 2000ms};
    o.totalTimeout = 2000ms;
    return o;
}

ProviderConfig testProvider(const MockHttpServer& server) {
    ProviderConfig p;
    p.key = "mock";
    p.name = "Mock DEM";
    p.urlTemplate = server.url(
        "/API/globaldem?demtype={dataset}&south={south}&north={north}&west={west}&east={east}&"
        "outputFormat={format}");
    p.format = "GTiff";
    p.datasets.push_back(
        DatasetConfig{.id = "COP30", .name = "COP30", .resolutionM = 30.0, .maxAreaKm2 = 450000.0});
    p.defaultDataset = "COP30";
    p.requiresKey = true;
    p.apiKey = ApiKeyPlacement{ApiKeyPlacement::Location::Query, "API_Key"};
    p.attribution = "Mock";
    p.insecureHttp = true;
    p.dailyCallLimit = 50;
    p.contentTypes = {"image/tiff", "application/octet-stream"};
    p.quotaErrorPatterns = {"limit exceeded"};
    return p;
}

// --- SHA-256 -------------------------------------------------------------------------------------

TEST(Sha256, FipsVectors) {
    EXPECT_EQ(ds::common::sha256Hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(ds::common::sha256Hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(ds::common::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    ds::common::Sha256 million;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) {
        million.update(chunk);
    }
    EXPECT_EQ(ds::common::Sha256::toHex(million.finish()),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, IncrementalEqualsOneShotAndFile) {
    std::string data;
    for (int i = 0; i < 5000; ++i) {
        data += static_cast<char>(i * 7);
    }
    ds::common::Sha256 h;
    for (std::size_t i = 0; i < data.size(); i += 37) {
        h.update(std::string_view(data).substr(i, 37));
    }
    EXPECT_EQ(ds::common::Sha256::toHex(h.finish()), ds::common::sha256Hex(data));
    const fs::path dir = tempDir("sha");
    {
        std::ofstream(dir / "f.bin", std::ios::binary) << data;
    }
    auto fileHash = ds::common::sha256File(dir / "f.bin");
    ASSERT_TRUE(fileHash) << fileHash.error().message;
    EXPECT_EQ(*fileHash, ds::common::sha256Hex(data));
    EXPECT_FALSE(ds::common::sha256File(dir / "missing.bin"));
    EXPECT_TRUE(ds::common::isSha256Hex(*fileHash));
    EXPECT_FALSE(ds::common::isSha256Hex("abc"));
    fs::remove_all(dir);
}

// --- URL templates and redaction (FR-TER-O02, FR-NET-07) -----------------------------------------

TEST(UrlTemplate, ExpandsAndEncodes) {
    auto url = expandUrlTemplate("https://h/x?d={dataset}&s={south}&c={crs}",
                                 {{"dataset", "A B"}, {"south", "-1.5"}, {"crs", "EPSG:4326"}});
    ASSERT_TRUE(url) << url.error().message;
    EXPECT_EQ(*url, "https://h/x?d=A%20B&s=-1.5&c=EPSG%3A4326");
    EXPECT_EQ(*expandUrlTemplate("https://h/{{literal}}", {}), "https://h/{literal}");
}

TEST(UrlTemplate, RejectsBadTemplates) {
    EXPECT_FALSE(expandUrlTemplate("https://h/{nope}", {}));
    EXPECT_FALSE(expandUrlTemplate("https://h/{dataset}", {}));  // No value.
    EXPECT_FALSE(expandUrlTemplate("https://h/{dataset", {{"dataset", "x"}}));
    EXPECT_FALSE(expandUrlTemplate("ftp://h/x", {}));
    EXPECT_FALSE(expandUrlTemplate("https://h/x}", {}));
}

TEST(UrlTemplate, AppendQueryAndRedact) {
    EXPECT_EQ(appendQueryParameter("https://h/x", "k", "v w"), "https://h/x?k=v%20w");
    EXPECT_EQ(appendQueryParameter("https://h/x?a=1", "k", "v"), "https://h/x?a=1&k=v");
    const std::vector<std::string> secrets{"s3cr+t"};
    EXPECT_EQ(redact("u?API_Key=s3cr%2Bt and s3cr+t", secrets), "u?API_Key=*** and ***");
    EXPECT_EQ(redact("nothing", {}), "nothing");
}

// --- Providers (FR-TER-O02) ---------------------------------------------------------------------

TEST(Provider, ShippedOpenTopographyPresetIsValid) {
    auto presets = loadProviderPresets(fs::path(DS_TEST_ASSETS_DIR) / "providers");
    ASSERT_TRUE(presets) << presets.error().message;
    // OpenTopography and the Géoplateforme, plus the imagery presets (online_imagery_test.cpp).
    const auto it = std::ranges::find(*presets, "opentopography", &ProviderConfig::key);
    ASSERT_NE(it, presets->end());
    EXPECT_EQ(std::ranges::count(*presets, "elevation", &ProviderConfig::kind), 2);
    const ProviderConfig& ot = *it;
    EXPECT_EQ(ot.key, "opentopography");
    EXPECT_TRUE(ot.requiresKey);
    EXPECT_EQ(ot.apiKey.name, "API_Key");
    EXPECT_EQ(ot.dailyCallLimit, 50);  // Non-academic keys (FR-TER-O17).
    EXPECT_EQ(ot.defaultDataset, "COP30");
    const DatasetConfig* cop30 = ot.dataset("COP30");
    ASSERT_NE(cop30, nullptr);
    EXPECT_EQ(cop30->verticalDatum, "EGM2008");  // FR-TER-O18.
    EXPECT_EQ(cop30->maxAreaKm2, 450000.0);
    EXPECT_FALSE(cop30->ellipsoidal);
    ASSERT_NE(ot.dataset("SRTMGL1_E"), nullptr);
    EXPECT_TRUE(ot.dataset("SRTMGL1_E")->ellipsoidal);
    // The USGS request format is not verified yet and must say so.
    ASSERT_NE(ot.dataset("USGS10m"), nullptr);
    EXPECT_FALSE(ot.dataset("USGS10m")->verified);
    EXPECT_NE(ot.templateFor(*ot.dataset("USGS10m")).find("/API/usgsdem"), std::string::npos);
    // The template never contains the key itself.
    EXPECT_EQ(ot.urlTemplate.find("API_Key"), std::string::npos);
    // Round trip through toJson().
    auto again = parseProvider(toJson(ot));
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(toJson(*again), toJson(ot));
}

TEST(Provider, ShippedGeoplateformePresetAsksTheWmsForGeoTiffs) {
    auto presets = loadProviderPresets(fs::path(DS_TEST_ASSETS_DIR) / "providers");
    ASSERT_TRUE(presets) << presets.error().message;
    const auto it = std::ranges::find(*presets, "geoplateforme-elevation", &ProviderConfig::key);
    ASSERT_NE(it, presets->end());
    const ProviderConfig& gp = *it;
    EXPECT_FALSE(gp.requiresKey);   // A public endpoint (cartes.gouv.fr/cgu).
    EXPECT_EQ(gp.maxSizePx, 5000);  // The server's MaxWidth / MaxHeight are 5010.
    EXPECT_LE(gp.rateLimitRps.value_or(1000.0), 40.0);  // 40 requests/s per IP.
    EXPECT_EQ(gp.defaultDataset, "ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES");
    ASSERT_EQ(gp.datasets.size(), 3U);
    for (const DatasetConfig& d : gp.datasets) {
        SCOPED_TRACE(d.id);
        EXPECT_EQ(d.verticalDatum, "NGF-IGN69");
        ASSERT_TRUE(d.coverage.has_value());
        EXPECT_TRUE(d.coverage->contains(GeoRect::around(45.92, 6.87, 10, 10)));  // Chamonix.
        EXPECT_FALSE(d.coverage->contains(GeoRect::around(40.4, -3.7, 10, 10)));  // Madrid.
        EXPECT_FALSE(d.attribution.empty());
        // A streamed block fits in one image.
        const ResponseSize block =
            responseSizePx(blockRect(blockAt(45.92, 6.87, gp.blockSizeKm), gp.blockSizeKm), d);
        EXPECT_LE(std::max(block.width, block.height), *gp.maxSizePx);
    }
    // WMS 1.3.0 boxes in EPSG:4326 are latitude first; the size follows the resolution.
    const GeoRect area{6.85, 45.90, 6.88, 45.92};
    auto url = DataClient::cacheKey(gp, *gp.dataset(gp.defaultDataset), area);
    ASSERT_TRUE(url) << url.error().message;
    EXPECT_TRUE(
        url->starts_with("https://data.geopf.fr/wms-r?SERVICE=WMS&VERSION=1.3.0&"
                         "REQUEST=GetMap&LAYERS=ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES&"))
        << *url;
    EXPECT_NE(url->find("&CRS=EPSG:4326&BBOX=45.9000000,6.8500000,45.9200000,6.8800000&"),
              std::string::npos)
        << *url;
    const ResponseSize size = responseSizePx(area, *gp.dataset(gp.defaultDataset));
    EXPECT_NE(url->find(std::format("&WIDTH={}&HEIGHT={}&", size.width, size.height)),
              std::string::npos)
        << *url;
    EXPECT_NEAR(static_cast<double>(size.width), area.widthKm() * 200.0, 1.0);  // 5 m.
    EXPECT_TRUE(url->ends_with("&FORMAT=image%2Fgeotiff")) << *url;             // Accepted encoded.
    auto again = parseProvider(toJson(gp));
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(toJson(*again), toJson(gp));
}

TEST(Provider, ResponseSizeLimitMustHoldAStreamedBlock) {
    auto json = toJson(ProviderConfig{.key = "x",
                                      .name = "X",
                                      .urlTemplate = "https://h/{dataset}",
                                      .datasets = {DatasetConfig{.id = "A", .resolutionM = 1}},
                                      .attribution = "a"});
    json["maxSizePx"] = 5000;
    json["blockSizeKm"] = 4;  // 4000 px and the margin.
    auto ok = parseProvider(json);
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_EQ(ok->maxSizePx, 5000);
    EXPECT_TRUE(ok->serveStale);
    json["blockSizeKm"] = 5;
    auto r = parseProvider(json);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("blockSizeKm"), std::string::npos) << r.error().message;
    EXPECT_NE(r.error().message.find("more than maxSizePx (5000)"), std::string::npos)
        << r.error().message;
}

TEST(Provider, ErrorsCarryJsonPaths) {
    auto json = toJson(ProviderConfig{.key = "x",
                                      .name = "X",
                                      .urlTemplate = "https://h/{dataset}",
                                      .datasets = {DatasetConfig{.id = "A", .resolutionM = 30}},
                                      .attribution = "a"});
    ASSERT_TRUE(parseProvider(json)) << parseProvider(json).error().message;

    auto bad = json;
    bad["datasets"][0]["resolutionM"] = -1;
    bad["unknownThing"] = 1;
    bad["x-comment"] = "fine";
    auto r = parseProvider(bad);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("/datasets/0/resolutionM"), std::string::npos);
    EXPECT_NE(r.error().message.find("/unknownThing: unknown property"), std::string::npos);
    EXPECT_EQ(r.error().message.find("x-comment"), std::string::npos);

    auto insecure = json;
    insecure["urlTemplate"] = "http://h/{dataset}";
    EXPECT_FALSE(parseProvider(insecure));
    insecure["insecureHttp"] = true;
    EXPECT_TRUE(parseProvider(insecure));

    auto keyless = json;
    keyless["requiresKey"] = true;
    keyless.erase("apiKey");
    EXPECT_FALSE(parseProvider(keyless));

    auto newer = json;
    newer["schemaVersion"] = kProviderSchemaVersion + 1;
    EXPECT_FALSE(parseProvider(newer));
}

// --- Block planning (FR-TER-O16) ----------------------------------------------------------------

TEST(BlockPlanner, GeoRectArea) {
    const GeoRect oneDeg{0, 0, 1, 1};
    EXPECT_NEAR(oneDeg.areaKm2(), 12364.0, 5.0);  // 1° × 1° at the equator.
    EXPECT_NEAR(GeoRect::around(40, -3, 10, 20).widthKm(), 10.0, 0.05);
    EXPECT_NEAR(GeoRect::around(40, -3, 10, 20).heightKm(), 20.0, 1e-9);
    EXPECT_TRUE(oneDeg.expandedKm(1).contains(oneDeg));
    EXPECT_FALSE(GeoRect({1, 0, 0, 1}).valid());
}

TEST(BlockPlanner, OneRequestWhenWithinLimits) {
    ProviderConfig p;
    p.name = "P";
    const DatasetConfig d{.id = "COP30", .name = "COP30", .resolutionM = 30, .maxAreaKm2 = 450000};
    auto plan = planExtentRequests(GeoRect::around(40.4, -3.7, 4, 4).expandedKm(1), p, d);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(plan->size(), 1U);
}

TEST(BlockPlanner, SplitsByResponseSizeIntoFewestPieces) {
    ProviderConfig p;
    p.name = "P";
    p.maxResponseBytes = 64ULL << 20U;
    const DatasetConfig d{.id = "COP30", .name = "COP30", .resolutionM = 30, .maxAreaKm2 = 450000};
    const GeoRect area = GeoRect::around(40.4, -3.7, 200, 200);  // 44 Mpx × 4 B ≈ 178 MB.
    auto plan = planExtentRequests(area, p, d);
    ASSERT_TRUE(plan) << plan.error().message;
    // ≈ 180 MB in total against a limit of 0.9 × 64 MiB ≈ 60.4 MB per request: 3 pieces fit,
    // 2 would not, so the plan must have exactly 3.
    EXPECT_EQ(plan->size(), 3U);
    const double limit = 0.9 * static_cast<double>(p.maxResponseBytes);
    EXPECT_GT(static_cast<double>(estimateResponseBytes(area, d)) / 2.0, limit);
    double total = 0;
    for (const GeoRect& r : *plan) {
        EXPECT_LE(estimateResponseBytes(r, d), p.maxResponseBytes);
        EXPECT_TRUE(area.contains(r));
        total += r.areaKm2();
    }
    EXPECT_NEAR(total, area.areaKm2(), 1e-6 * area.areaKm2());
}

TEST(BlockPlanner, RespectsAreaLimitAndCoverage) {
    ProviderConfig p;
    p.name = "P";
    p.maxResponseBytes = 1ULL << 40U;
    const DatasetConfig d{.id = "U",
                          .name = "USGS 10m",
                          .resolutionM = 10,
                          .maxAreaKm2 = 25000,
                          .coverage = GeoRect{-125, 24, -66, 50}};
    auto plan = planExtentRequests(GeoRect::around(39, -105, 300, 300), p, d);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_GE(plan->size(), 4U);
    for (const GeoRect& r : *plan) {
        EXPECT_LE(r.areaKm2(), 25000.0);
    }
    auto outside = planExtentRequests(GeoRect::around(40, -3, 10, 10), p, d);
    ASSERT_FALSE(outside);
    EXPECT_NE(outside.error().message.find("outside the coverage"), std::string::npos);
}

TEST(BlockPlanner, SplitsToTheLargestImageTheServerMakes) {
    ProviderConfig p;
    p.name = "P";
    p.maxResponseBytes = 1ULL << 40U;
    p.maxSizePx = 5000;
    const DatasetConfig d{.id = "L", .name = "LiDAR 1 m", .resolutionM = 1};
    // 12 × 2 km at 1 m: one piece by area and bytes, but 12 000 px wide; three are 4000 px.
    const GeoRect area = GeoRect::around(45.9, 6.87, 12, 2);
    auto plan = planExtentRequests(area, p, d);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(plan->size(), 3U);
    for (const GeoRect& r : *plan) {
        const ResponseSize size = responseSizePx(r, d);
        EXPECT_LE(size.width, 5000);
        EXPECT_LE(size.height, 5000);
    }
}

TEST(BlockPlanner, BlocksAroundAreStableAndNearestFirst) {
    const auto a = blocksAround(40.40, -3.70, 30, 20);
    const auto b = blocksAround(40.41, -3.69, 30, 20);
    ASSERT_FALSE(a.empty());
    EXPECT_LE(a.front().distanceKm, 20.0);
    for (std::size_t i = 1; i < a.size(); ++i) {
        EXPECT_LE(a[i - 1].distanceKm, a[i].distanceKm);
    }
    // Fixed grid: the same block keys appear with identical rectangles from a nearby point.
    for (const Block& x : a) {
        for (const Block& y : b) {
            if (x.key == y.key) {
                EXPECT_EQ(x.rect, y.rect);
            }
        }
        EXPECT_NEAR(x.rect.heightKm(), 20.0, 1e-6);
        EXPECT_NEAR(x.rect.widthKm(), 20.0, 0.5);
    }
    // Wraps around the antimeridian.
    const auto wrap = blocksAround(0, 179.95, 25, 20);
    EXPECT_TRUE(std::ranges::any_of(wrap, [](const Block& k) { return k.rect.west < -170; }));
}

// --- Quota (FR-TER-O17) -------------------------------------------------------------------------

TEST(Quota, RollingWindow) {
    InMemoryUsageLedger ledger;
    const std::int64_t t0 = 1'800'000'000;
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(ledger.record(ProviderCall{"ot", t0 + (i * 60), "COP30", {}, 200, 10}));
    }
    ASSERT_TRUE(ledger.record(ProviderCall{"other", t0, "", {}, 200, 1}));
    auto s = quotaStatus(ledger, "ot", 50, t0 + 3600);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->used, 50);
    EXPECT_TRUE(s->exhausted());
    ASSERT_TRUE(s->nextFreeAtUnixS);
    EXPECT_EQ(*s->nextFreeAtUnixS, t0 + kQuotaWindowS);
    // Exactly 24 h after the first call it has left the window.
    auto later = quotaStatus(ledger, "ot", 50, t0 + kQuotaWindowS);
    ASSERT_TRUE(later);
    EXPECT_EQ(later->used, 49);
    EXPECT_EQ(later->remaining, 1);
    EXPECT_EQ(quotaNotice(*later, 1), "uses 1 of your 1 remaining calls today");
    EXPECT_EQ(quotaNotice(*later, 3), "needs 3 calls but only 1 of your 50 daily calls remain");
}

// --- Disk cache (FR-NET-04, FR-NET-05) ----------------------------------------------------------

TEST(DiskCache, StoreReadEvictPin) {
    const fs::path dir = tempDir("cache");
    InMemoryCacheIndex index;
    DiskCache cache(dir, index, 250);
    const std::string a(100, 'a'), b(100, 'b'), c(100, 'c');
    ASSERT_TRUE(cache.put("p", "ka", a, {"image/tiff", "\"e1\"", "", std::nullopt}, 10));
    ASSERT_TRUE(cache.put("p", "kb", b, {}, 20));
    ASSERT_TRUE(cache.setPinned("p", "ka", true).value());
    // Private permissions (NFR-SEC-01).
    EXPECT_EQ(fs::status(dir / "p").permissions() & fs::perms::all, fs::perms::owner_all);
    auto got = cache.get("p", "ka", 30);
    ASSERT_TRUE(got && *got);
    EXPECT_EQ((*got)->body, a);
    EXPECT_EQ((*got)->entry.etag, "\"e1\"");
    EXPECT_EQ((*got)->entry.contentHash, ds::common::sha256Hex(a));
    EXPECT_EQ(fs::status(dir / (*got)->entry.relativePath).permissions() & fs::perms::all,
              fs::perms::owner_read | fs::perms::owner_write);
    // Third entry exceeds 250 bytes: the LRU unpinned entry (kb) goes, the pinned one stays.
    ASSERT_TRUE(cache.put("p", "kc", c, {}, 40));
    EXPECT_FALSE(cache.get("p", "kb", 50).value());
    EXPECT_TRUE(cache.get("p", "ka", 50).value());
    EXPECT_TRUE(cache.get("p", "kc", 50).value());
    EXPECT_EQ(cache.totalBytes().value(), 200U);
    // Unpinned entries obey a smaller limit; pinned ones survive.
    ASSERT_TRUE(cache.setLimit(50));
    EXPECT_TRUE(cache.get("p", "ka", 60).value());
    EXPECT_FALSE(cache.get("p", "kc", 60).value());
    ASSERT_TRUE(cache.clear("p", false));
    EXPECT_TRUE(cache.get("p", "ka", 70).value());
    ASSERT_TRUE(cache.clear("p", true));
    EXPECT_FALSE(cache.get("p", "ka", 70).value());
    fs::remove_all(dir);
}

TEST(DiskCache, UsagePerProviderAndReleasingPins) {
    const fs::path dir = tempDir("cache_usage");
    InMemoryCacheIndex index;
    DiskCache cache(dir, index, 1000);
    ASSERT_TRUE(cache.put("b", "k1", std::string(100, 'x'), {}, 10));
    ASSERT_TRUE(cache.put("a", "k2", std::string(200, 'y'), {}, 20));
    ASSERT_TRUE(cache.put("a", "k3", std::string(300, 'z'), {}, 30));
    ASSERT_TRUE(cache.setPinned("a", "k3", true).value());
    auto usage = cache.usage();
    ASSERT_TRUE(usage);
    ASSERT_EQ(usage->size(), 2U);
    EXPECT_EQ((*usage)[0].provider, "a");
    EXPECT_EQ((*usage)[0].entries, 2);
    EXPECT_EQ((*usage)[0].bytes, 500U);
    EXPECT_EQ((*usage)[0].pinnedEntries, 1);
    EXPECT_EQ((*usage)[0].pinnedBytes, 300U);
    EXPECT_EQ((*usage)[1].bytes, 100U);
    EXPECT_EQ(cache.limit(), 1000U);

    // Over a 250-byte limit only the pinned 300 bytes stay; released, they go too.
    ASSERT_TRUE(cache.setLimit(250));
    EXPECT_TRUE(cache.get("a", "k3", 40).value());
    EXPECT_EQ(cache.totalBytes().value(), 300U);
    auto released = cache.releasePinned("a");
    ASSERT_TRUE(released);
    EXPECT_EQ(*released, 1);
    EXPECT_EQ(cache.totalBytes().value(), 0U);
    EXPECT_TRUE(cache.usage()->empty());
    fs::remove_all(dir);
}

TEST(DiskCache, ExpiryAndDamagedFiles) {
    const fs::path dir = tempDir("cache2");
    InMemoryCacheIndex index;
    DiskCache cache(dir, index, 1 << 20);
    auto e = cache.put("p", "k", "payload", {.expiresAtUnixS = 100}, 10);
    ASSERT_TRUE(e);
    EXPECT_FALSE(cache.get("p", "k", 50).value()->stale);
    EXPECT_TRUE(cache.get("p", "k", 100).value()->stale);
    ASSERT_TRUE(cache.revalidated("p", "k", 500, 120));
    EXPECT_FALSE(cache.get("p", "k", 130).value()->stale);
    // A damaged file is never served; its entry is dropped.
    std::ofstream(dir / e->relativePath, std::ios::binary | std::ios::trunc) << "tampered";
    EXPECT_FALSE(cache.get("p", "k", 140).value());
    EXPECT_FALSE(index.find("p", "k").value());
    EXPECT_FALSE(cache.put("../evil", "k", "x", {}, 1));
    fs::remove_all(dir);
}

// --- HTTP client (FR-NET-01 … FR-NET-03) ---------------------------------------------------------

TEST(HttpClient, GetsBodyHeadersAndUserAgent) {
    MockHttpServer server([](const MockHttpServer::Request&) {
        return MockHttpServer::Response{200,
                                        {{"Content-Type", "image/tiff; charset=binary"},
                                         {"ETag", "\"v1\""},
                                         {"Cache-Control", "max-age=60"}},
                                        "hello"};
    });
    HttpClient client(fastOptions());
    Waiter<NetResult<HttpResponse>> w;
    client.submit(
        HttpRequest{
            .url = server.url("/x"), .allowInsecureHttp = true, .contentTypes = {"image/tiff"}},
        w.callback());
    auto r = w.get();
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->body, "hello");
    EXPECT_EQ(r->contentType, "image/tiff");
    EXPECT_EQ(r->etag, "\"v1\"");
    EXPECT_EQ(r->maxAgeS, 60);
    ASSERT_EQ(server.requests().size(), 1U);
    EXPECT_TRUE(server.requests()[0].headers["user-agent"].starts_with("DroneSimulator/"));
}

TEST(HttpClient, RejectsInsecureOfflineContentTypeAndSize) {
    MockHttpServer server([](const MockHttpServer::Request&) {
        return MockHttpServer::Response{
            200, {{"Content-Type", "text/html"}}, std::string(5000, 'x')};
    });
    HttpClient client(fastOptions());
    {
        Waiter<NetResult<HttpResponse>> w;
        client.submit(HttpRequest{.url = server.url("/x")}, w.callback());
        EXPECT_EQ(w.get().error().code, NetErrorCode::InsecureUrl);
    }
    {
        Waiter<NetResult<HttpResponse>> w;
        client.submit(
            HttpRequest{
                .url = server.url("/x"), .allowInsecureHttp = true, .contentTypes = {"image/tiff"}},
            w.callback());
        EXPECT_EQ(w.get().error().code, NetErrorCode::ContentType);
    }
    {
        Waiter<NetResult<HttpResponse>> w;
        client.submit(
            HttpRequest{.url = server.url("/x"), .allowInsecureHttp = true, .maxBytes = 1000},
            w.callback());
        EXPECT_EQ(w.get().error().code, NetErrorCode::TooLarge);
    }
    client.setOffline(true);
    {
        Waiter<NetResult<HttpResponse>> w;
        client.submit(HttpRequest{.url = server.url("/x"), .allowInsecureHttp = true},
                      w.callback());
        EXPECT_EQ(w.get().error().code, NetErrorCode::Offline);
    }
    EXPECT_EQ(server.requestCount(), 2U);  // Insecure and offline never reached the server.
}

TEST(HttpClient, RetriesServerErrorsAndHonoursRetryAfter) {
    std::atomic<int> n{0};
    MockHttpServer server([&](const MockHttpServer::Request&) {
        const int i = n++;
        if (i == 0) {
            return MockHttpServer::Response{503, {}, "busy"};
        }
        if (i == 1) {
            return MockHttpServer::Response{429, {{"Retry-After", "1"}}, "slow down"};
        }
        return MockHttpServer::Response{200, {}, "ok"};
    });
    HttpClient client(fastOptions());
    std::atomic<int> attempts{0};
    Waiter<NetResult<HttpResponse>> w;
    const auto start = std::chrono::steady_clock::now();
    client.submit(HttpRequest{.url = server.url("/x"),
                              .allowInsecureHttp = true,
                              .onAttempt = [&](const AttemptInfo&) { ++attempts; }},
                  w.callback());
    auto r = w.get();
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->attempts, 3);
    EXPECT_EQ(attempts.load(), 3);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 950ms);  // Retry-After: 1 s.
}

TEST(HttpClient, QuotaErrorsAndClientErrorsAreNotRetried) {
    MockHttpServer server([](const MockHttpServer::Request& req) {
        if (req.target == "/quota") {
            return MockHttpServer::Response{401, {}, "<error>Rate limit exceeded</error>"};
        }
        return MockHttpServer::Response{404, {}, "missing"};
    });
    HttpClient client(fastOptions());
    auto quotaCheck = [](long, std::string_view body) {
        return body.find("limit exceeded") != std::string_view::npos;
    };
    {
        Waiter<NetResult<HttpResponse>> w;
        client.submit(
            HttpRequest{
                .url = server.url("/quota"), .allowInsecureHttp = true, .isQuotaError = quotaCheck},
            w.callback());
        auto r = w.get();
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, NetErrorCode::QuotaExceeded);
    }
    {
        Waiter<NetResult<HttpResponse>> w;
        client.submit(HttpRequest{.url = server.url("/missing"),
                                  .allowInsecureHttp = true,
                                  .isQuotaError = quotaCheck},
                      w.callback());
        auto r = w.get();
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, NetErrorCode::HttpStatus);
        EXPECT_EQ(r.error().httpStatus, 404);
    }
    EXPECT_EQ(server.requestCount(), 2U);
}

TEST(HttpClient, CircuitBreakerOpensAfterRepeatedFailures) {
    MockHttpServer server(
        [](const MockHttpServer::Request&) { return MockHttpServer::Response{500, {}, "down"}; });
    HttpClientOptions o = fastOptions();
    o.retry.maxAttempts = 1;
    o.breaker = CircuitBreakerPolicy{.failureThreshold = 3, .cooldown = 60s};
    HttpClient client(o);
    for (int i = 0; i < 3; ++i) {
        Waiter<NetResult<HttpResponse>> w;
        client.submit(HttpRequest{.url = server.url("/x"), .group = "g", .allowInsecureHttp = true},
                      w.callback());
        EXPECT_EQ(w.get().error().code, NetErrorCode::HttpStatus);
    }
    EXPECT_TRUE(client.circuitOpen("g"));
    Waiter<NetResult<HttpResponse>> w;
    client.submit(HttpRequest{.url = server.url("/x"), .group = "g", .allowInsecureHttp = true},
                  w.callback());
    EXPECT_EQ(w.get().error().code, NetErrorCode::CircuitOpen);
    EXPECT_EQ(server.requestCount(), 3U);
    EXPECT_FALSE(client.circuitOpen("other"));
}

TEST(HttpClient, CancelAndPriorityOrder) {
    std::mutex m;
    std::vector<std::string> order;
    MockHttpServer server([&](const MockHttpServer::Request& req) {
        {
            const std::scoped_lock lock(m);
            order.push_back(req.target);
        }
        return MockHttpServer::Response{200, {}, "ok", req.target == "/slow" ? 1500ms : 0ms};
    });
    HttpClient client(fastOptions());
    client.setGroupLimits("g", 1, std::nullopt);  // One at a time ⇒ queue order is observable.
    Waiter<NetResult<HttpResponse>> slow, low, high;
    const RequestId slowId = client.submit(
        HttpRequest{.url = server.url("/slow"), .group = "g", .allowInsecureHttp = true},
        slow.callback());
    std::this_thread::sleep_for(100ms);  // /slow is running.
    client.submit(
        HttpRequest{
            .url = server.url("/low"), .group = "g", .priority = 5, .allowInsecureHttp = true},
        low.callback());
    client.submit(
        HttpRequest{
            .url = server.url("/high"), .group = "g", .priority = 1, .allowInsecureHttp = true},
        high.callback());
    client.cancel(slowId);
    EXPECT_EQ(slow.get().error().code, NetErrorCode::Cancelled);
    EXPECT_TRUE(high.get());
    EXPECT_TRUE(low.get());
    const std::scoped_lock lock(m);
    ASSERT_EQ(order.size(), 3U);
    EXPECT_EQ(order[1], "/high");
    EXPECT_EQ(order[2], "/low");
}

TEST(HttpClient, TimeoutAndUnreachable) {
    MockHttpServer server([](const MockHttpServer::Request&) {
        return MockHttpServer::Response{200, {}, "late", 1000ms};
    });
    HttpClientOptions o = fastOptions();
    o.totalTimeout = 200ms;
    o.retry.maxAttempts = 1;
    HttpClient client(o);
    Waiter<NetResult<HttpResponse>> w;
    client.submit(HttpRequest{.url = server.url("/x"), .allowInsecureHttp = true}, w.callback());
    EXPECT_EQ(w.get().error().code, NetErrorCode::Timeout);

    Waiter<NetResult<HttpResponse>> w2;
    client.submit(HttpRequest{.url = "http://127.0.0.1:1/x", .allowInsecureHttp = true},
                  w2.callback());
    EXPECT_EQ(w2.get().error().code, NetErrorCode::Connection);
    EXPECT_FALSE(client.networkReachable());
}

// --- DataClient (§4.6) --------------------------------------------------------------------------

struct DataClientFixture : ::testing::Test {
    std::atomic<int> served{0};
    std::string etag = "\"t1\"";
    MockHttpServer server{[this](const MockHttpServer::Request& req) {
        ++served;
        if (req.headers.contains("if-none-match") && req.headers.at("if-none-match") == etag) {
            return MockHttpServer::Response{304, {}, ""};
        }
        if (req.target.find("API_Key=good") == std::string::npos) {
            return MockHttpServer::Response{401, {}, "<error>Invalid API key</error>"};
        }
        if (req.target.find("demtype=EMPTY") != std::string::npos) {
            return MockHttpServer::Response{
                200, {{"Content-Type", "image/tiff"}}, "Error: no data. API_Key=good"};
        }
        return MockHttpServer::Response{
            200, {{"Content-Type", "image/tiff"}, {"ETag", etag}}, kTiff};
    }};
    fs::path dir = tempDir("data");
    InMemoryCacheIndex index;
    DiskCache cache{dir, index, 1 << 20};
    InMemoryUsageLedger ledger;
    std::int64_t now = 1'800'000'000;
    HttpClient http{fastOptions()};
    DataClient client{http, &cache, &ledger, [this] { return now; }};
    ProviderConfig provider = testProvider(server);
    GeoRect area = GeoRect::around(40.4, -3.7, 4, 4);

    ~DataClientFixture() override { fs::remove_all(dir); }

    NetResult<Payload> fetch(const BBoxRequest& req) {
        Waiter<NetResult<Payload>> w;
        client.fetch(provider, req, 0, w.callback());
        return w.get();
    }
};

TEST_F(DataClientFixture, MissingKeyNeverCallsTheProvider) {
    auto r = fetch({"", area});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, NetErrorCode::MissingKey);
    EXPECT_EQ(served.load(), 0);
}

TEST_F(DataClientFixture, FetchCachesRecordsAndNeverLogsTheKey) {
    std::mutex m;
    std::string logged;
    ds::log::setLevel(ds::log::Level::Trace);
    ds::log::setSink([&](ds::log::Level, std::string_view msg) {
        const std::scoped_lock lock(m);
        logged += msg;
        logged += '\n';
    });
    client.setApiKey("mock", "good");
    auto first = fetch({"COP30", area});
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_FALSE(first->fromCache);
    EXPECT_EQ(first->body, kTiff);
    EXPECT_EQ(first->contentHash, ds::common::sha256Hex(kTiff));
    EXPECT_EQ(first->cacheKey.find("good"), std::string::npos);
    auto second = fetch({"COP30", area});
    ASSERT_TRUE(second);
    EXPECT_TRUE(second->fromCache);
    EXPECT_EQ(served.load(), 1);
    ASSERT_EQ(ledger.calls().size(), 1U);
    EXPECT_EQ(ledger.calls()[0].dataset, "COP30");
    EXPECT_EQ(ledger.calls()[0].httpStatus, 200);

    client.setApiKey("mock", "wrong-key-123");
    auto bad = fetch({"COP30", GeoRect::around(41, -3.7, 4, 4)});
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, NetErrorCode::HttpStatus);
    ds::log::setSink({});
    ds::log::setLevel(ds::log::Level::Info);
    EXPECT_EQ(logged.find("wrong-key-123"), std::string::npos) << logged;
    EXPECT_EQ(logged.find("API_Key=good"), std::string::npos) << logged;
    EXPECT_NE(logged.find("API_Key=***"), std::string::npos) << logged;
    EXPECT_EQ(bad.error().message.find("wrong-key-123"), std::string::npos);
}

TEST_F(DataClientFixture, NoCacheRequestsAlwaysAndSessionStatsCount) {
    EXPECT_FALSE(client.stats("mock").hitRate().has_value());
    client.setApiKey("mock", "good");
    ASSERT_TRUE(fetch({"COP30", area}));
    ASSERT_TRUE(fetch({"COP30", area}));  // From the cache.
    EXPECT_EQ(served.load(), 1);
    // Test connection: neither read nor written to the cache.
    auto direct = fetch({.dataset = "COP30", .area = area, .noCache = true});
    ASSERT_TRUE(direct) << direct.error().message;
    EXPECT_FALSE(direct->fromCache);
    EXPECT_EQ(served.load(), 2);
    const GeoRect other = GeoRect::around(42.0, -3.7, 4, 4);
    ASSERT_TRUE(fetch({.dataset = "COP30", .area = other, .noCache = true}));
    EXPECT_FALSE(client.cached(provider, {"COP30", other}).value().has_value());

    client.setApiKey("mock", "bad");
    EXPECT_FALSE(fetch({"COP30", GeoRect::around(43.0, -3.7, 4, 4)}));
    const ClientStats s = client.stats("mock");
    EXPECT_EQ(s.cacheHits, 1U);
    EXPECT_EQ(s.requests, 4U);
    EXPECT_EQ(s.failures, 1U);
    EXPECT_DOUBLE_EQ(s.hitRate().value(), 0.2);
    EXPECT_EQ(client.stats("other").requests, 0U);
}

TEST_F(DataClientFixture, InvalidPayloadIsRejectedAndKeyRedacted) {
    provider.datasets.push_back(DatasetConfig{.id = "EMPTY", .name = "E", .resolutionM = 30});
    client.setApiKey("mock", "good");
    auto r = fetch({"EMPTY", area});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, NetErrorCode::InvalidPayload);
    EXPECT_NE(r.error().message.find("expected a GeoTIFF"), std::string::npos);
    EXPECT_EQ(r.error().message.find("good"), std::string::npos) << r.error().message;
}

TEST_F(DataClientFixture, OfflineAndQuotaFallBackToCache) {
    client.setApiKey("mock", "good");
    ASSERT_TRUE(fetch({"COP30", area}));
    http.setOffline(true);
    auto cached = fetch({"COP30", area});
    ASSERT_TRUE(cached);
    EXPECT_TRUE(cached->fromCache);
    auto uncached = fetch({"COP30", GeoRect::around(42, -3.7, 4, 4)});
    ASSERT_FALSE(uncached);
    EXPECT_EQ(uncached.error().code, NetErrorCode::Offline);
    http.setOffline(false);

    for (int i = 0; i < 49; ++i) {
        ASSERT_TRUE(ledger.record(ProviderCall{"mock", now - 10, "COP30", {}, 200, 1}));
    }
    auto q = client.quota(provider);
    ASSERT_TRUE(q && *q);
    EXPECT_TRUE((*q)->exhausted());
    const int before = served.load();
    auto over = fetch({"COP30", GeoRect::around(43, -3.7, 4, 4)});
    ASSERT_FALSE(over);
    EXPECT_EQ(over.error().code, NetErrorCode::QuotaExceeded);
    EXPECT_EQ(served.load(), before);     // Never sent.
    EXPECT_TRUE(fetch({"COP30", area}));  // Cached data still works.
}

TEST_F(DataClientFixture, StaleEntriesAreRevalidated) {
    provider.maxAgeDays = 1;
    client.setApiKey("mock", "good");
    ASSERT_TRUE(fetch({"COP30", area}));
    now += 2 * 86400;
    auto r = fetch({"COP30", area});
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(r->fromCache);
    EXPECT_FALSE(r->stale);
    EXPECT_EQ(served.load(), 2);
    const auto reqs = server.requests();
    EXPECT_EQ(reqs.back().headers.at("if-none-match"), etag);
    EXPECT_TRUE(fetch({"COP30", area})->fromCache);  // Fresh again.
    EXPECT_EQ(served.load(), 2);
}

TEST_F(DataClientFixture, ProvidersWithoutOfflineUseNeverServeExpiredEntries) {
    provider.maxAgeDays = 1;
    provider.serveStale = false;  // e.g. the OpenStreetMap tile servers.
    client.setApiKey("mock", "good");
    ASSERT_TRUE(fetch({"COP30", area}));
    http.setOffline(true);
    auto fresh = fetch({"COP30", area});  // Within the cache lifetime.
    ASSERT_TRUE(fresh) << fresh.error().message;
    EXPECT_TRUE(fresh->fromCache);
    now += 2 * 86400;
    auto expired = fetch({"COP30", area});
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, NetErrorCode::Offline);
    EXPECT_FALSE(client.cached(provider, {"COP30", area}).value().has_value());
    http.setOffline(false);
    // Online, the expired entry is revalidated as usual.
    auto again = fetch({"COP30", area});
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_TRUE(again->fromCache);
    EXPECT_EQ(served.load(), 2);
}

TEST_F(DataClientFixture, RequestsLargerThanTheServerMakesAreRefused) {
    provider.maxSizePx = 100;  // 4 km at 30 m is 134 px.
    client.setApiKey("mock", "good");
    auto r = fetch({"COP30", area});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, NetErrorCode::InvalidRequest);
    EXPECT_NE(r.error().message.find("more than Mock DEM serves at once (100 px)"),
              std::string::npos)
        << r.error().message;
    EXPECT_EQ(served.load(), 0);
}

TEST_F(DataClientFixture, DownloadAreaPlansPinsAndChecksBudget) {
    client.setApiKey("mock", "good");
    provider.maxResponseBytes = 4 << 20;  // Forces several requests.
    const GeoRect big = GeoRect::around(40.4, -3.7, 60, 60);
    auto estimate = client.estimateDownload(provider, "", big);
    ASSERT_TRUE(estimate) << estimate.error().message;
    const auto pieces = static_cast<int>(estimate->requests.size());
    EXPECT_GT(pieces, 1);
    EXPECT_EQ(estimate->uncachedRequests, pieces);
    ASSERT_TRUE(estimate->quota);
    EXPECT_EQ(estimate->quota->remaining, 50);

    std::atomic<int> progressCalls{0};
    Waiter<NetResult<std::vector<std::string>>> done;
    auto job = client.downloadArea(
        provider, "", big, [&](const DownloadProgress&) { ++progressCalls; }, done.callback());
    ASSERT_TRUE(job) << job.error().message;
    auto keys = done.get();
    ASSERT_TRUE(keys) << keys.error().message;
    EXPECT_EQ(static_cast<int>(keys->size()), pieces);
    EXPECT_EQ(progressCalls.load(), pieces);
    EXPECT_TRUE((*job)->finished());
    for (const std::string& k : *keys) {
        auto e = index.find("mock", k);
        ASSERT_TRUE(e && *e);
        EXPECT_TRUE((*e)->pinned);
    }
    auto again = client.estimateDownload(provider, "", big);
    EXPECT_EQ(again->uncachedRequests, 0);

    // Not enough calls left for a new area: refused before any request.
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(ledger.record(ProviderCall{"mock", now, "COP30", {}, 200, 1}));
    }
    auto refused = client.downloadArea(provider, "", GeoRect::around(45, 5, 60, 60), {},
                                       [](NetResult<std::vector<std::string>>) {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, NetErrorCode::QuotaExceeded);
}

}  // namespace
