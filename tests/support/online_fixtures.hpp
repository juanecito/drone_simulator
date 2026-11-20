// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// A mock bounding-box elevation provider for the online-terrain tests (SPECIFICATION §9: CI
// never contacts real providers). It answers OpenTopography-style requests with a GeoTIFF
// of an analytic surface, on a global grid of 1 arc-second pixels, for any bounding box.

#pragma once

#include <atomic>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/net/provider.hpp"

#include "mock_http_server.hpp"
#include <cpl_conv.h>
#include <cpl_vsi.h>
#include <gdal.h>
#include <ogr_srs_api.h>

namespace ds::test {

// Pixel size of the mock dataset: 1 arc-second (≈ 31 m north–south).
inline constexpr double kDemPixelDeg = 1.0 / 3600.0;

// Smooth analytic surface, metres AMSL: a slope plus gentle waves.
inline double demHeight(double latDeg, double lonDeg) {
    return 800.0 + 3000.0 * (latDeg - 41.5) + 1500.0 * (lonDeg - 1.8) +
           40.0 * std::sin(latDeg * 400.0) * std::cos(lonDeg * 300.0);
}

// GeoTIFF bytes covering @p west … @p north with pixels of the global grid.
inline std::string demGeoTiff(double west, double south, double east, double north) {
    GDALAllRegister();
    const double i0 = std::floor(west / kDemPixelDeg);
    const double i1 = std::ceil(east / kDemPixelDeg);
    const double j0 = std::floor(south / kDemPixelDeg);
    const double j1 = std::ceil(north / kDemPixelDeg);
    const int width = static_cast<int>(i1 - i0);
    const int height = static_cast<int>(j1 - j0);
    GDALDatasetH mem =
        GDALCreate(GDALGetDriverByName("MEM"), "", width, height, 1, GDT_Float32, nullptr);
    double gt[6] = {i0 * kDemPixelDeg, kDemPixelDeg, 0.0, j1 * kDemPixelDeg, 0.0, -kDemPixelDeg};
    GDALSetGeoTransform(mem, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    GDALSetSpatialRef(mem, srs);
    OSRDestroySpatialReference(srs);
    std::vector<float> values(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int j = 0; j < height; ++j) {
        const double lat = (j1 - j - 0.5) * kDemPixelDeg;
        for (int i = 0; i < width; ++i) {
            const double lon = (i0 + i + 0.5) * kDemPixelDeg;
            values[static_cast<std::size_t>(j) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(i)] = static_cast<float>(demHeight(lat, lon));
        }
    }
    if (GDALRasterIO(GDALGetRasterBand(mem, 1), GF_Write, 0, 0, width, height, values.data(), width,
                     height, GDT_Float32, 0, 0) != CE_None) {
        GDALClose(mem);
        return {};
    }
    static std::atomic<int> counter{0};
    const std::string name = std::format("/vsimem/mock_dem_{}.tif", counter++);
    GDALDatasetH tif = GDALCreateCopy(GDALGetDriverByName("GTiff"), name.c_str(), mem, FALSE,
                                      nullptr, nullptr, nullptr);
    GDALClose(tif);
    GDALClose(mem);
    vsi_l_offset size = 0;
    GByte* data = VSIGetMemFileBuffer(name.c_str(), &size, FALSE);
    std::string out(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size));
    VSIUnlink(name.c_str());
    return out;
}

// Value of query parameter @p name in @p target, if any.
inline std::optional<double> queryNumber(std::string_view target, std::string_view name) {
    const std::string key = std::string(name) + "=";
    std::size_t at = target.find("?" + key);
    if (at == std::string_view::npos) {
        at = target.find("&" + key);
    }
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view rest = target.substr(at + 1 + key.size());
    const std::string_view text = rest.substr(0, rest.find('&'));
    double value = 0.0;  // from_chars: independent of the locale the Qt application sets.
    if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

// The mock provider: counts its requests; can fail on demand.
class MockDemServer {
public:
    MockDemServer()
        : server_([this](const MockHttpServer::Request& request) { return answer(request); }) {}

    // Provider config pointing at this server (quota 50 calls / 24 h, key required).
    [[nodiscard]] net::ProviderConfig provider(const std::string& key = "mockdem") const {
        net::ProviderConfig p;
        p.key = key;
        p.name = "Mock DEM";
        p.urlTemplate = server_.url(
            "/API/globaldem?demtype={dataset}&south={south}&north={north}&west={west}&"
            "east={east}&outputFormat={format}");
        p.format = "GTiff";
        p.datasets.push_back(net::DatasetConfig{.id = "TEST30",
                                                .name = "Test DEM (30 m)",
                                                .resolutionM = 30.0,
                                                .maxAreaKm2 = 450000.0,
                                                .verticalDatum = "EGM2008",
                                                .attribution = "Test DEM © nobody"});
        p.defaultDataset = "TEST30";
        p.requiresKey = true;
        p.apiKey = net::ApiKeyPlacement{net::ApiKeyPlacement::Location::Query, "API_Key"};
        p.attribution = "Mock DEM provider";
        p.insecureHttp = true;
        p.dailyCallLimit = 50;
        p.maxConcurrent = 2;
        p.contentTypes = {"image/tiff"};
        p.quotaErrorPatterns = {"limit exceeded"};
        p.blockSizeKm = 4.0;  // Small blocks keep the payloads small.
        return p;
    }

    // Provider JSON (format of assets/providers/*.json).
    [[nodiscard]] std::string providerJson(const std::string& key = "mockdem") const {
        common::Json json = net::toJson(provider(key));
        json["schemaVersion"] = net::kProviderSchemaVersion;
        return json.dump(2);
    }

    [[nodiscard]] std::size_t requestCount() const { return server_.requestCount(); }
    [[nodiscard]] std::vector<MockHttpServer::Request> requests() const {
        return server_.requests();
    }
    // Every later request fails with 500.
    void setBroken(bool broken) { broken_.store(broken); }

private:
    MockHttpServer::Response answer(const MockHttpServer::Request& request) {
        if (broken_.load()) {
            return {.status = 500, .headers = {}, .body = "server error", .delay = {}};
        }
        const auto s = queryNumber(request.target, "south");
        const auto n = queryNumber(request.target, "north");
        const auto w = queryNumber(request.target, "west");
        const auto e = queryNumber(request.target, "east");
        if (!s || !n || !w || !e || request.target.find("API_Key=") == std::string::npos) {
            return {.status = 400, .headers = {}, .body = "bad request", .delay = {}};
        }
        return {.status = 200,
                .headers = {{"Content-Type", "image/tiff"}},
                .body = demGeoTiff(*w, *s, *e, *n),
                .delay = {}};
    }

    std::atomic<bool> broken_{false};
    MockHttpServer server_;
};

}  // namespace ds::test
