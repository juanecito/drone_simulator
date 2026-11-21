// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// A mock XYZ imagery service for the online-imagery tests (SPECIFICATION §9: CI never
// contacts real providers). Tile z/x/y is a 256-px PNG whose pixel at global position
// (gx, gy) of zoom z has red = gx mod 256, green = gy mod 256 and blue = 128, so a test can
// tell where a texel was sampled from. Tiles outside a coverage box answer 404.

#pragma once

#include <atomic>
#include <charconv>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "ds/geo/web_mercator.hpp"
#include "ds/net/provider.hpp"

#include "mock_http_server.hpp"
#include <cpl_vsi.h>
#include <gdal.h>

namespace ds::test {

// PNG bytes of a @p size × @p size RGB image from @p value(i, j, band).
template <typename F>
inline std::string pngTile(int size, F value) {
    GDALAllRegister();
    GDALDatasetH mem = GDALCreate(GDALGetDriverByName("MEM"), "", size, size, 3, GDT_Byte, nullptr);
    std::vector<std::uint8_t> band(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
    for (int b = 0; b < 3; ++b) {
        for (int j = 0; j < size; ++j) {
            for (int i = 0; i < size; ++i) {
                band[static_cast<std::size_t>(j) * static_cast<std::size_t>(size) +
                     static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value(i, j, b));
            }
        }
        if (GDALRasterIO(GDALGetRasterBand(mem, b + 1), GF_Write, 0, 0, size, size, band.data(),
                         size, size, GDT_Byte, 0, 0) != CE_None) {
            GDALClose(mem);
            return {};
        }
    }
    static std::atomic<int> counter{0};
    const std::string name = std::format("/vsimem/mock_tile_{}.png", counter++);
    GDALDatasetH png = GDALCreateCopy(GDALGetDriverByName("PNG"), name.c_str(), mem, FALSE, nullptr,
                                      nullptr, nullptr);
    GDALClose(png);
    GDALClose(mem);
    vsi_l_offset size2 = 0;
    GByte* data = VSIGetMemFileBuffer(name.c_str(), &size2, FALSE);
    std::string out(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size2));
    VSIUnlink(name.c_str());
    return out;
}

// The mock imagery service: XYZ tiles at /tiles/{z}/{x}/{y}.png.
class MockTileServer {
public:
    struct Coverage {
        double west;
        double south;
        double east;
        double north;
    };

    MockTileServer() : MockTileServer(Coverage{-180.0, -85.0, 180.0, 85.0}) {}
    explicit MockTileServer(Coverage coverage)
        : coverage_(coverage),
          server_([this](const MockHttpServer::Request& request) { return answer(request); }) {}

    // Provider config pointing at this server (no key, zoom 0 … @p maxZoom).
    [[nodiscard]] net::ProviderConfig provider(const std::string& key = "mockimg",
                                               int maxZoom = 19) const {
        net::ProviderConfig p;
        p.key = key;
        p.name = "Mock Imagery";
        p.kind = "imagery";
        p.protocol = "xyz";
        p.crs = "EPSG:3857";
        p.urlTemplate = server_.url("/tiles/{z}/{x}/{y}.png");
        p.format = "image/png";
        p.tileSizePx = 256;
        net::DatasetConfig d{.id = "photo", .name = "Test photo"};
        d.minZoom = 0;
        d.maxZoom = maxZoom;
        d.resolutionM = 156543.03392804097 / static_cast<double>(1 << maxZoom);
        d.attribution = "Test imagery © nobody";
        p.datasets.push_back(d);
        p.defaultDataset = "photo";
        p.attribution = "Mock imagery provider";
        p.insecureHttp = true;
        p.maxConcurrent = 4;
        p.contentTypes = {"image/png"};
        return p;
    }

    // Provider JSON (format of assets/providers/*.json).
    [[nodiscard]] std::string providerJson(const std::string& key = "mockimg") const {
        common::Json json = net::toJson(provider(key));
        json["schemaVersion"] = net::kProviderSchemaVersion;
        return json.dump(2);
    }

    [[nodiscard]] std::size_t requestCount() const { return server_.requestCount(); }
    // Tile z/x/y answers a 128-px image instead of a 256-px one.
    void breakTile(int z, std::int64_t x, std::int64_t y) {
        const std::scoped_lock lock(mutex_);
        broken_.insert({z, x, y});
    }
    // Every later request fails with 500.
    void setFailing(bool failing) { failing_.store(failing); }

private:
    static std::optional<std::int64_t> number(std::string_view text) {
        std::int64_t v = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), v).ec != std::errc{}) {
            return std::nullopt;
        }
        return v;
    }

    MockHttpServer::Response answer(const MockHttpServer::Request& request) {
        if (failing_.load()) {
            return {.status = 500, .headers = {}, .body = "server error", .delay = {}};
        }
        // /tiles/{z}/{x}/{y}.png
        std::string_view t = request.target;
        if (!t.starts_with("/tiles/") || !t.ends_with(".png")) {
            return {.status = 400, .headers = {}, .body = "bad request", .delay = {}};
        }
        t = t.substr(7, t.size() - 11);
        const auto s1 = t.find('/');
        const auto s2 = t.find('/', s1 + 1);
        const auto z = number(t.substr(0, s1));
        const auto x = number(t.substr(s1 + 1, s2 - s1 - 1));
        const auto y = number(t.substr(s2 + 1));
        if (!z || !x || !y) {
            return {.status = 400, .headers = {}, .body = "bad request", .delay = {}};
        }
        const geo::TileId tile{*x, *y, static_cast<int>(*z)};
        const auto bounds = geo::tileBounds(tile);
        if (!bounds || bounds->east <= coverage_.west || bounds->west >= coverage_.east ||
            bounds->north <= coverage_.south || bounds->south >= coverage_.north) {
            return {.status = 404, .headers = {}, .body = "no tile", .delay = {}};
        }
        {
            const std::scoped_lock lock(mutex_);
            if (broken_.contains({tile.z, tile.x, tile.y})) {
                return {.status = 200,
                        .headers = {{"Content-Type", "image/png"}},
                        .body = pngTile(128, [](int, int, int) { return 50; }),
                        .delay = {}};
            }
        }
        const std::int64_t gx0 = tile.x * 256;
        const std::int64_t gy0 = tile.y * 256;
        return {.status = 200,
                .headers = {{"Content-Type", "image/png"}},
                .body = pngTile(256,
                                [&](int i, int j, int b) {
                                    return b == 0   ? static_cast<int>((gx0 + i) % 256)
                                           : b == 1 ? static_cast<int>((gy0 + j) % 256)
                                                    : 128;
                                }),
                .delay = {}};
    }

    Coverage coverage_;
    std::atomic<bool> failing_{false};
    std::mutex mutex_;
    std::set<std::tuple<int, std::int64_t, std::int64_t>> broken_;
    MockHttpServer server_;
};

}  // namespace ds::test
