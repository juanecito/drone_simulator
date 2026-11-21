// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/net/provider.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>

#include "ds/net/url_template.hpp"

namespace ds::net {
namespace {

// Streamed blocks are fetched with a few pixels of margin, and blocks of a latitude band are
// slightly wider than blockSizeKm.
constexpr double kBlockPxHeadroom = 1.1;

using common::Json;
using common::JsonReader;

void rejectUnknown(JsonReader& r, std::initializer_list<std::string_view> known) {
    if (!r.value().is_object()) {
        return;
    }
    for (const auto& [name, value] : r.value().items()) {
        if (name.starts_with("x-") || name.starts_with("$")) {
            continue;
        }
        if (std::ranges::find(known, std::string_view(name)) == known.end()) {
            r.fail(name, "unknown property");
        }
    }
}

bool isNull(const JsonReader& r, std::string_view key) {
    return r.has(key) && r.value().at(std::string(key)).is_null();
}

std::optional<double> optionalNumber(JsonReader& r, std::string_view key, double min, double max) {
    if (!r.has(key) || isNull(r, key)) {
        return std::nullopt;
    }
    return r.number(key, min, max);
}

std::optional<int> optionalInteger(JsonReader& r, std::string_view key, int min, int max) {
    if (!r.has(key) || isNull(r, key)) {
        return std::nullopt;
    }
    return r.integer(key, min, max);
}

std::vector<std::string> stringList(JsonReader& r, std::string_view key) {
    std::vector<std::string> out;
    const std::size_t n = r.optionalArray(key);
    for (std::size_t i = 0; i < n; ++i) {
        JsonReader e = r.element(key, i);
        if (!e.value().is_string() || e.value().get<std::string>().empty()) {
            e.fail("", "must be a non-empty string");
            continue;
        }
        out.push_back(e.value().get<std::string>());
    }
    return out;
}

std::optional<GeoRect> readRect(JsonReader& r, std::string_view key) {
    auto o = r.optionalObject(key);
    if (!o) {
        return std::nullopt;
    }
    rejectUnknown(*o, {"west", "south", "east", "north"});
    GeoRect rect{o->number("west", -180, 180), o->number("south", -90, 90),
                 o->number("east", -180, 180), o->number("north", -90, 90)};
    if (o->ok() && !rect.valid()) {
        o->fail("", "west must be less than east and south less than north");
    }
    return rect;
}

void checkTemplate(JsonReader& r, std::string_view key, const std::string& text, bool insecureHttp,
                   std::string_view protocol) {
    std::map<std::string, std::string, std::less<>> dummy;
    for (const std::string_view name : urlPlaceholders()) {
        dummy.emplace(std::string(name), "0");
    }
    if (auto expanded = expandUrlTemplate(text, dummy); !expanded) {
        r.fail(key, expanded.error().message);
    } else if (text.starts_with("http://") && !insecureHttp) {
        r.fail(key, "plain http:// requires \"insecureHttp\": true");
    } else if (protocol == "wms" && text.find("{bbox}") == std::string::npos) {
        r.fail(key, "a WMS template needs {bbox} (and usually {width} and {height})");
    } else if (isTileProtocol(protocol) && protocol != "wms" &&
               (text.find("{z}") == std::string::npos || text.find("{x}") == std::string::npos ||
                text.find("{y}") == std::string::npos)) {
        r.fail(key, "a tile template needs {z}, {x} and {y}");
    } else if (!isTileProtocol(protocol) &&
               (text.find("{z}") != std::string::npos || text.find("{x}") != std::string::npos ||
                text.find("{y}") != std::string::npos)) {
        r.fail(key, "{z}, {x} and {y} are for tile protocols");
    }
}

DatasetConfig readDataset(JsonReader& d, bool insecureHttp, std::string_view protocol,
                          bool imagery) {
    rejectUnknown(d, {"id", "name", "urlTemplate", "resolutionM", "maxAreaKm2", "verticalDatum",
                      "ellipsoidal", "coverage", "bytesPerSample", "attribution", "verified",
                      "minZoom", "maxZoom"});
    DatasetConfig ds;
    ds.id = d.string("id");
    ds.name = d.string("name", ds.id);
    ds.urlTemplate = d.string("urlTemplate", "");
    if (!ds.urlTemplate.empty()) {
        checkTemplate(d, "urlTemplate", ds.urlTemplate, insecureHttp, protocol);
    }
    if (imagery) {
        ds.minZoom = d.integer("minZoom", 0, kMaxImageryZoom, 0);
        ds.maxZoom = d.integer("maxZoom", 0, kMaxImageryZoom);
        if (d.ok() && *ds.maxZoom < ds.minZoom) {
            d.fail("maxZoom", "must not be less than minZoom");
        }
        // Ground pixel at the equator of the finest zoom (256-px tiles).
        const double finest = 156543.03392804097 / std::ldexp(1.0, ds.maxZoom.value_or(0));
        ds.resolutionM = d.number("resolutionM", 0.01, 10000.0, std::max(finest, 0.01));
    } else {
        if (d.has("minZoom") || d.has("maxZoom")) {
            d.fail(d.has("minZoom") ? "minZoom" : "maxZoom", "is for imagery datasets");
        }
        ds.resolutionM = d.number("resolutionM", 0.01, 10000.0);
    }
    ds.maxAreaKm2 = optionalNumber(d, "maxAreaKm2", 0.001, 1.0e9);
    ds.verticalDatum = d.string("verticalDatum", "");
    ds.ellipsoidal = d.boolean("ellipsoidal", false);
    ds.coverage = readRect(d, "coverage");
    ds.bytesPerSample = d.integer("bytesPerSample", 1, 8, 4);
    ds.attribution = d.string("attribution", "");
    ds.verified = d.boolean("verified", true);
    return ds;
}

}  // namespace

bool isTileProtocol(std::string_view protocol) noexcept {
    return protocol == "xyz" || protocol == "tms" || protocol == "wmts" || protocol == "wms";
}

const DatasetConfig* ProviderConfig::dataset(std::string_view id) const noexcept {
    const auto it = std::ranges::find(datasets, id, &DatasetConfig::id);
    return it == datasets.end() ? nullptr : &*it;
}

const std::string& ProviderConfig::templateFor(const DatasetConfig& d) const noexcept {
    return d.urlTemplate.empty() ? urlTemplate : d.urlTemplate;
}

common::Result<ProviderConfig> parseProvider(const Json& json) {
    if (!json.is_object()) {
        return common::makeError("provider: must be a JSON object");
    }
    JsonReader r(json);
    rejectUnknown(r, {"schemaVersion",  "key",
                      "name",           "kind",
                      "protocol",       "urlTemplate",
                      "format",         "crs",
                      "datasets",       "defaultDataset",
                      "requiresKey",    "apiKey",
                      "attribution",    "licence",
                      "termsUrl",       "allowCache",
                      "maxAgeDays",     "rateLimitRps",
                      "dailyCallLimit", "maxConcurrent",
                      "insecureHttp",   "maxResponseBytes",
                      "contentTypes",   "quotaErrorPatterns",
                      "blockSizeKm",    "origin",
                      "tileSizePx",     "maxSizePx",
                      "serveStale"});
    const int version = r.integer("schemaVersion", 1, 1000);
    if (r.ok() && version > kProviderSchemaVersion) {
        r.fail("schemaVersion",
               std::format("version {} is newer than this application supports ({})", version,
                           kProviderSchemaVersion));
    }
    ProviderConfig p;
    p.key = r.string("key");
    if (!p.key.empty() && !std::ranges::all_of(p.key, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                   c == '.';
        })) {
        r.fail("key", "may only contain a–z, 0–9, '-', '_' and '.'");
    }
    p.name = r.string("name");
    p.kind = r.choice("kind", {"elevation", "imagery"});
    p.protocol = r.choice("protocol", {"rest_bbox", "xyz", "tms", "wmts", "wms"});
    const bool imagery = p.kind == "imagery";
    if (r.ok() && imagery != isTileProtocol(p.protocol)) {
        r.fail("protocol", imagery ? "imagery providers use xyz, tms, wmts or wms"
                                   : "elevation providers use rest_bbox for now");
    }
    p.insecureHttp = r.boolean("insecureHttp", false);
    p.urlTemplate = r.string("urlTemplate", "");
    if (!p.urlTemplate.empty()) {
        checkTemplate(r, "urlTemplate", p.urlTemplate, p.insecureHttp, p.protocol);
    }
    p.format = r.string("format", "");
    p.crs = r.string("crs", imagery ? "EPSG:3857" : "EPSG:4326");
    if (imagery && p.crs != "EPSG:3857") {
        r.fail("crs", "imagery providers must use Web Mercator (EPSG:3857) tiles for now");
    } else if (!imagery && p.crs != "EPSG:4326") {
        r.fail("crs", "bounding-box providers must use EPSG:4326 for now");
    }
    p.tileSizePx = r.integer("tileSizePx", 64, 1024, 256);
    if (r.ok() && (p.tileSizePx & (p.tileSizePx - 1)) != 0) {
        r.fail("tileSizePx", "must be a power of two (256 or 512)");
    }
    const std::size_t n = r.array("datasets");
    std::set<std::string> ids;
    for (std::size_t i = 0; i < n; ++i) {
        JsonReader d = r.element("datasets", i);
        DatasetConfig ds = readDataset(d, p.insecureHttp, p.protocol, imagery);
        if (!ds.id.empty() && !ids.insert(ds.id).second) {
            d.fail("id", std::format("duplicate dataset id \"{}\"", ds.id));
        }
        if (ds.urlTemplate.empty() && p.urlTemplate.empty()) {
            d.fail("urlTemplate", "required when the provider has no urlTemplate");
        }
        p.datasets.push_back(std::move(ds));
    }
    if (r.ok() && p.datasets.empty()) {
        r.fail("datasets", "must list at least one dataset");
    }
    p.defaultDataset = r.string("defaultDataset", p.datasets.empty() ? "" : p.datasets[0].id);
    if (!p.datasets.empty() && p.dataset(p.defaultDataset) == nullptr) {
        r.fail("defaultDataset", std::format("unknown dataset \"{}\"", p.defaultDataset));
    }
    p.requiresKey = r.boolean("requiresKey", false);
    if (auto k = r.optionalObject("apiKey")) {
        rejectUnknown(*k, {"in", "name"});
        p.apiKey.location = k->choice("in", {"query", "header"}) == "header"
                                ? ApiKeyPlacement::Location::Header
                                : ApiKeyPlacement::Location::Query;
        p.apiKey.name = k->string("name");
    } else if (p.requiresKey) {
        r.fail("apiKey", "is required when requiresKey is true");
    }
    p.attribution = r.string("attribution");
    p.licence = r.string("licence", "");
    p.termsUrl = r.string("termsUrl", "");
    p.allowCache = r.boolean("allowCache", true);
    p.maxAgeDays = optionalInteger(r, "maxAgeDays", 1, 36500);
    p.rateLimitRps = optionalNumber(r, "rateLimitRps", 0.001, 1000.0);
    p.dailyCallLimit = optionalInteger(r, "dailyCallLimit", 1, 10'000'000);
    p.maxConcurrent = r.integer("maxConcurrent", 1, 16, 2);
    p.maxResponseBytes = static_cast<std::uint64_t>(
        r.number("maxResponseBytes", 1024.0, 4.0 * 1024 * 1024 * 1024, 64.0 * 1024 * 1024));
    p.contentTypes = stringList(r, "contentTypes");
    p.quotaErrorPatterns = stringList(r, "quotaErrorPatterns");
    p.blockSizeKm = r.number("blockSizeKm", 1.0, 500.0, 20.0);
    p.serveStale = r.boolean("serveStale", true);
    p.maxSizePx = optionalInteger(r, "maxSizePx", 64, 1'000'000);
    if (imagery && p.maxSizePx) {
        r.fail("maxSizePx", "is for bounding-box providers");
    } else if (r.ok() && p.maxSizePx) {
        // A streamed block, with its margin, must fit in one response.
        for (const DatasetConfig& d : p.datasets) {
            const double px = p.blockSizeKm * 1000.0 / d.resolutionM * kBlockPxHeadroom;
            if (px > *p.maxSizePx) {
                r.fail("blockSizeKm",
                       std::format("blocks of {:g} km need about {:.0f} px at the {:g} m of "
                                   "\"{}\", more than maxSizePx ({})",
                                   p.blockSizeKm, px, d.resolutionM, d.id, *p.maxSizePx));
                break;
            }
        }
    }
    p.origin = r.has("origin") ? r.choice("origin", {"builtin", "user"}) : "builtin";
    if (!r.ok()) {
        return common::makeError(r.errorText());
    }
    return p;
}

Json toJson(const ProviderConfig& p) {
    Json j;
    j["schemaVersion"] = kProviderSchemaVersion;
    j["key"] = p.key;
    j["name"] = p.name;
    j["kind"] = p.kind;
    j["protocol"] = p.protocol;
    if (!p.urlTemplate.empty()) {
        j["urlTemplate"] = p.urlTemplate;
    }
    if (!p.format.empty()) {
        j["format"] = p.format;
    }
    j["crs"] = p.crs;
    if (p.isImagery()) {
        j["tileSizePx"] = p.tileSizePx;
    }
    j["datasets"] = Json::array();
    for (const DatasetConfig& d : p.datasets) {
        Json dj;
        dj["id"] = d.id;
        dj["name"] = d.name;
        if (!d.urlTemplate.empty()) {
            dj["urlTemplate"] = d.urlTemplate;
        }
        dj["resolutionM"] = d.resolutionM;
        if (d.maxAreaKm2) {
            dj["maxAreaKm2"] = *d.maxAreaKm2;
        }
        if (!d.verticalDatum.empty()) {
            dj["verticalDatum"] = d.verticalDatum;
        }
        dj["ellipsoidal"] = d.ellipsoidal;
        if (d.coverage) {
            dj["coverage"] = {{"west", d.coverage->west},
                              {"south", d.coverage->south},
                              {"east", d.coverage->east},
                              {"north", d.coverage->north}};
        }
        dj["bytesPerSample"] = d.bytesPerSample;
        if (!d.attribution.empty()) {
            dj["attribution"] = d.attribution;
        }
        dj["verified"] = d.verified;
        if (p.isImagery()) {
            dj["minZoom"] = d.minZoom;
            if (d.maxZoom) {
                dj["maxZoom"] = *d.maxZoom;
            }
        }
        j["datasets"].push_back(std::move(dj));
    }
    if (!p.defaultDataset.empty()) {
        j["defaultDataset"] = p.defaultDataset;
    }
    j["requiresKey"] = p.requiresKey;
    if (!p.apiKey.name.empty()) {
        j["apiKey"] = {
            {"in", p.apiKey.location == ApiKeyPlacement::Location::Header ? "header" : "query"},
            {"name", p.apiKey.name}};
    }
    j["attribution"] = p.attribution;
    if (!p.licence.empty()) {
        j["licence"] = p.licence;
    }
    if (!p.termsUrl.empty()) {
        j["termsUrl"] = p.termsUrl;
    }
    j["allowCache"] = p.allowCache;
    j["maxAgeDays"] = p.maxAgeDays ? Json(*p.maxAgeDays) : Json(nullptr);
    j["rateLimitRps"] = p.rateLimitRps ? Json(*p.rateLimitRps) : Json(nullptr);
    j["dailyCallLimit"] = p.dailyCallLimit ? Json(*p.dailyCallLimit) : Json(nullptr);
    j["maxConcurrent"] = p.maxConcurrent;
    j["insecureHttp"] = p.insecureHttp;
    j["maxResponseBytes"] = p.maxResponseBytes;
    j["contentTypes"] = p.contentTypes;
    j["quotaErrorPatterns"] = p.quotaErrorPatterns;
    j["blockSizeKm"] = p.blockSizeKm;
    j["serveStale"] = p.serveStale;
    if (p.maxSizePx) {
        j["maxSizePx"] = *p.maxSizePx;
    }
    j["origin"] = p.origin;
    return j;
}

common::Result<std::vector<ProviderConfig>> loadProviderPresets(
    const std::filesystem::path& directory) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    if (ec) {
        return common::makeError(std::format("cannot read provider presets in \"{}\": {}",
                                             directory.string(), ec.message()));
    }
    std::vector<ProviderConfig> providers;
    for (const auto& file : files) {
        auto json = common::readJsonFile(file, std::size_t{1} << 20U);
        if (!json) {
            return common::makeError(
                std::format("{}: {}", file.filename().string(), json.error().message));
        }
        auto provider = parseProvider(*json);
        if (!provider) {
            return common::makeError(
                std::format("{}: {}", file.filename().string(), provider.error().message));
        }
        provider->origin = "builtin";
        providers.push_back(std::move(*provider));
    }
    std::ranges::sort(providers, {}, &ProviderConfig::key);
    if (const auto dup = std::ranges::adjacent_find(providers, {}, &ProviderConfig::key);
        dup != providers.end()) {
        return common::makeError(std::format("duplicate provider key \"{}\"", dup->key));
    }
    return providers;
}

}  // namespace ds::net
