// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/terrain_files.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>

#include "ds/common/sha256.hpp"

namespace ds::terrain {
namespace {

namespace fs = std::filesystem;
using common::Json;
using common::JsonReader;

std::string lowerExtension(const fs::path& file) {
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

void rejectUnknown(JsonReader& r, std::initializer_list<std::string_view> known) {
    if (!r.value().is_object()) {
        return;
    }
    for (const auto& [name, value] : r.value().items()) {
        if (!name.starts_with("x-") &&
            std::ranges::find(known, std::string_view(name)) == known.end()) {
            r.fail(name, "unknown property");
        }
    }
}

std::vector<FileRef> readFileList(JsonReader& r, std::string_view key, bool required) {
    std::vector<FileRef> out;
    const std::size_t n = required ? r.array(key) : r.optionalArray(key);
    if (required && r.has(key) && n == 0) {
        r.fail(key, "must list at least one file");
    }
    for (std::size_t i = 0; i < n; ++i) {
        JsonReader e = r.element(key, i);
        if (!e.value().is_object()) {
            e.fail("", "must be an object { \"path\": …, \"sha256\": … }");
            continue;
        }
        rejectUnknown(e, {"path", "sha256"});
        FileRef f;
        f.path = e.string("path");
        f.sha256 = e.string("sha256", "");
        f.jsonPath = e.path();
        if (!f.sha256.empty()) {
            if (!common::isSha256Hex(f.sha256)) {
                e.fail("sha256", "must be 64 hexadecimal characters");
            }
            std::ranges::transform(f.sha256, f.sha256.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        }
        out.push_back(std::move(f));
    }
    return out;
}

bool isInside(const fs::path& base, const fs::path& candidate) {
    auto b = base.begin();
    auto c = candidate.begin();
    for (; b != base.end(); ++b, ++c) {
        if (c == candidate.end() || *b != *c) {
            return false;
        }
    }
    return c != candidate.end();  // Strictly inside.
}

TerrainResult<ResolvedFile> resolveAndCheck(const FileRef& ref, const TerrainFileRoots& roots,
                                            const std::stop_token& stop) {
    auto path = resolveTerrainPath(ref.path, roots);
    if (!path) {
        return terrainError(path.error().code,
                            std::format("{}: {}", ref.jsonPath, path.error().message));
    }
    std::error_code ec;
    ResolvedFile file{*path, fs::file_size(*path, ec), {}};
    if (ec) {
        return terrainError(TerrainErrorCode::FileNotFound,
                            std::format("{}: cannot read \"{}\"", ref.jsonPath, ref.path));
    }
    auto hash = common::sha256File(*path, stop);
    if (!hash) {
        if (stop.stop_requested()) {
            return terrainError(TerrainErrorCode::Cancelled, "cancelled");
        }
        return terrainError(TerrainErrorCode::FileNotFound,
                            std::format("{}: {}", ref.jsonPath, hash.error().message));
    }
    file.sha256 = std::move(*hash);
    if (!ref.sha256.empty() && ref.sha256 != file.sha256) {
        return terrainError(TerrainErrorCode::Sha256Mismatch,
                            std::format("{}: \"{}\" does not match its sha256 (file changed or "
                                        "damaged)",
                                        ref.jsonPath, ref.path));
    }
    return file;
}

}  // namespace

TerrainResult<FileTerrainSpec> parseFileTerrain(const Json& json, std::string_view jsonPath) {
    if (!json.is_object()) {
        return terrainError(TerrainErrorCode::InvalidSpec,
                            std::format("{}: must be an object", jsonPath));
    }
    JsonReader r(json, std::string(jsonPath));
    rejectUnknown(
        r, {"source", "format", "elevationFiles", "crs", "verticalDatum", "attribution", "origin",
            "extent", "band", "surface", "resolutionM", "interpolation", "classifyGround",
            "orthophotoFiles", "orthophotoCrs", "orthophotoAttribution", "orthophotoBands"});
    FileTerrainSpec spec;
    if (r.choice("source", {"file"}) != "file") {
        return terrainError(TerrainErrorCode::InvalidSpec, r.errorText());
    }
    const std::string format = r.choice("format", {"raster", "las", "laz"});
    spec.format = format == "raster" ? ElevationFormat::Raster : ElevationFormat::PointCloud;
    spec.elevationFiles = readFileList(r, "elevationFiles", true);
    spec.crs = r.string("crs", "");
    spec.verticalDatum = r.string("verticalDatum", "");
    spec.attribution = r.string("attribution", "");
    if (auto o = r.optionalObject("origin")) {
        rejectUnknown(*o, {"geo", "altRef"});
        if (o->array("geo") != 3) {
            o->fail("geo", "must be [latitude, longitude, altitude]");
        } else {
            spec.originLatDeg = o->element("geo", 0).asNumber(-90.0, 90.0);
            spec.originLonDeg = o->element("geo", 1).asNumber(-180.0, 180.0);
            spec.originAltM = o->element("geo", 2).asNumber(-500.0, 9000.0);
        }
        spec.originAltRef = o->has("altRef") ? o->choice("altRef", {"AGL", "AMSL"}) : "AGL";
    }
    if (auto e = r.optionalObject("extent")) {
        rejectUnknown(*e, {"widthM", "lengthM"});
        spec.extent = Extent{e->number("widthM", kMinExtentM, kMaxExtentM),
                             e->number("lengthM", kMinExtentM, kMaxExtentM)};
    }
    const bool raster = spec.format == ElevationFormat::Raster;
    spec.band = r.integer("band", 1, 1000, 1);
    if (!raster && r.has("band")) {
        r.fail("band", "only applies to raster files");
    }
    std::optional<TerrainError> notSupported;
    if (r.has("surface")) {
        const std::string surface = r.choice("surface", {"dtm", "dsm"});
        if (surface == "dsm") {
            notSupported = TerrainError{
                TerrainErrorCode::NotSupported,
                std::format("{}/surface: \"dsm\" is not supported in v1.0 (only \"dtm\")",
                            jsonPath)};
        } else if (raster) {
            r.fail("surface", "only applies to LAS/LAZ files");
        }
    }
    if (r.has("resolutionM")) {
        spec.resolutionM = r.number("resolutionM", 0.25, 100.0);
        if (raster) {
            r.fail("resolutionM", "only applies to LAS/LAZ files");
        }
    }
    if (r.has("interpolation")) {
        const std::string interp = r.choice("interpolation", {"tin", "idw", "max"});
        if (interp == "max") {
            notSupported = TerrainError{
                TerrainErrorCode::NotSupported,
                std::format("{}/interpolation: \"max\" builds a DSM, which is not supported in "
                            "v1.0",
                            jsonPath)};
        }
        spec.interpolation = interp == "idw" ? LidarInterpolation::Idw : LidarInterpolation::Tin;
        if (raster) {
            r.fail("interpolation", "only applies to LAS/LAZ files");
        }
    }
    spec.classifyGround = r.boolean("classifyGround", false);
    if (raster && r.has("classifyGround")) {
        r.fail("classifyGround", "only applies to LAS/LAZ files");
    }
    spec.orthophotoFiles = readFileList(r, "orthophotoFiles", false);
    spec.orthophotoCrs = r.string("orthophotoCrs", "");
    spec.orthophotoAttribution = r.string("orthophotoAttribution", "");
    const std::size_t bands = r.optionalArray("orthophotoBands");
    for (std::size_t i = 0; i < bands; ++i) {
        spec.orthophotoBands.push_back(
            static_cast<int>(r.element("orthophotoBands", i).asNumber(1, 1000)));
    }
    if (bands != 0 && bands != 3 && bands != 4) {
        r.fail("orthophotoBands", "must list 3 (RGB) or 4 (RGBA) bands");
    }
    if (!r.ok()) {
        return terrainError(TerrainErrorCode::InvalidSpec, r.errorText());
    }
    if (notSupported) {
        return std::unexpected(std::move(*notSupported));
    }
    // Every file must match the declared format (FR-TER-F04).
    for (const FileRef& f : spec.elevationFiles) {
        const std::string_view name = std::string_view(f.path).starts_with("library:")
                                          ? std::string_view(f.path).substr(8)
                                          : std::string_view(f.path);
        const auto kind = elevationFormatOf(fs::path(name));
        if (!kind) {
            return terrainError(TerrainErrorCode::UnsupportedFormat,
                                std::format("{}/path: unsupported elevation file \"{}\" (use COG/"
                                            "GeoTIFF, .hgt, .asc, .img, .las or .laz)",
                                            f.jsonPath, f.path));
        }
        if (*kind != spec.format) {
            return terrainError(
                TerrainErrorCode::MixedFormats,
                std::format("{}/path: \"{}\" is not a {} file (format is \"{}\")", f.jsonPath,
                            f.path, raster ? "raster" : "LAS/LAZ", format));
        }
    }
    return spec;
}

std::optional<ElevationFormat> elevationFormatOf(const fs::path& file) {
    const std::string ext = lowerExtension(file);
    if (ext == ".tif" || ext == ".tiff" || ext == ".hgt" || ext == ".asc" || ext == ".img") {
        return ElevationFormat::Raster;
    }
    if (ext == ".las" || ext == ".laz") {
        return ElevationFormat::PointCloud;
    }
    return std::nullopt;
}

bool isOrthophotoFile(const fs::path& file) {
    const std::string ext = lowerExtension(file);
    return ext == ".tif" || ext == ".tiff" || ext == ".jp2" || ext == ".png" || ext == ".jpg" ||
           ext == ".jpeg";
}

TerrainResult<fs::path> resolveTerrainPath(std::string_view reference,
                                           const TerrainFileRoots& roots) {
    const bool library = reference.starts_with("library:");
    const std::string_view rel = library ? reference.substr(8) : reference;
    const fs::path& base = library ? roots.libraryDir : roots.missionDir;
    if (rel.empty()) {
        return terrainError(TerrainErrorCode::PathRejected, "empty path");
    }
    if (rel.find('\\') != std::string_view::npos) {
        return terrainError(TerrainErrorCode::PathRejected,
                            std::format("\"{}\": use '/' as separator", reference));
    }
    if (rel.front() == '/' || (rel.size() >= 2 && rel[1] == ':')) {
        return terrainError(TerrainErrorCode::PathRejected,
                            std::format("\"{}\": absolute paths are not allowed; use a path "
                                        "relative to the mission folder or library:<name>",
                                        reference));
    }
    for (const auto segment : std::views::split(rel, '/')) {
        if (std::string_view(segment.begin(), segment.end()) == "..") {
            return terrainError(TerrainErrorCode::PathRejected,
                                std::format("\"{}\": '..' is not allowed", reference));
        }
    }
    std::error_code ec;
    const fs::path canonicalBase = fs::canonical(base, ec);
    if (ec) {
        return terrainError(TerrainErrorCode::FileNotFound,
                            std::format("\"{}\": the {} folder does not exist", reference,
                                        library ? "geodata library" : "mission"));
    }
    const fs::path file = fs::canonical(canonicalBase / fs::path(rel), ec);
    if (ec) {
        return terrainError(TerrainErrorCode::FileNotFound,
                            std::format("\"{}\": file not found", reference));
    }
    if (!isInside(canonicalBase, file)) {
        return terrainError(TerrainErrorCode::PathRejected,
                            std::format("\"{}\": leads outside the {} folder (symbolic link?)",
                                        reference, library ? "geodata library" : "mission"));
    }
    if (!fs::is_regular_file(file, ec)) {
        return terrainError(TerrainErrorCode::FileNotFound,
                            std::format("\"{}\": not a regular file", reference));
    }
    return file;
}

TerrainResult<PreparedFileTerrain> prepareFileTerrain(const FileTerrainSpec& spec,
                                                      const TerrainFileRoots& roots,
                                                      const TerrainFileLimits& limits,
                                                      std::stop_token stop) {
    PreparedFileTerrain out;
    std::uint64_t total = 0;
    std::vector<std::string> hashes;
    for (const FileRef& ref : spec.elevationFiles) {
        auto file = resolveAndCheck(ref, roots, stop);
        if (!file) {
            return std::unexpected(file.error());
        }
        total += file->sizeBytes;
        hashes.push_back(file->sha256);
        out.elevation.push_back(std::move(*file));
    }
    const std::uint64_t limit =
        spec.format == ElevationFormat::Raster ? limits.maxRasterBytes : limits.maxPointCloudBytes;
    if (total > limit) {
        return terrainError(TerrainErrorCode::TooLarge,
                            std::format("elevation files total {:.1f} GB; the limit is {:.1f} GB",
                                        static_cast<double>(total) / (1U << 30U),
                                        static_cast<double>(limit) / (1U << 30U)));
    }

    std::uint64_t orthoTotal = 0;
    for (const FileRef& ref : spec.orthophotoFiles) {
        if (!isOrthophotoFile(fs::path(ref.path))) {
            out.warnings.push_back(
                std::format("{}: unsupported orthophoto format \"{}\"; imagery disabled",
                            ref.jsonPath, ref.path));
            out.orthophotos.clear();
            break;
        }
        auto file = resolveAndCheck(ref, roots, stop);
        if (!file) {
            if (file.error().code == TerrainErrorCode::Cancelled) {
                return std::unexpected(file.error());
            }
            out.warnings.push_back(file.error().message + "; imagery disabled");
            out.orthophotos.clear();
            break;
        }
        orthoTotal += file->sizeBytes;
        out.orthophotos.push_back(std::move(*file));
    }
    if (orthoTotal > limits.maxOrthophotoBytes) {
        out.warnings.push_back(
            std::format("orthophotos total {:.1f} GB; the limit is {:.1f} GB; imagery disabled",
                        static_cast<double>(orthoTotal) / (1U << 30U),
                        static_cast<double>(limits.maxOrthophotoBytes) / (1U << 30U)));
        out.orthophotos.clear();
    }
    out.terrainDataHash = terrainDataHash(hashes, spec);
    return out;
}

std::string terrainDataHash(const std::vector<std::string>& fileHashes,
                            const FileTerrainSpec& spec) {
    common::Sha256 hash;
    hash.update("drone-simulator terrain v1\n");
    for (const std::string& h : fileHashes) {
        hash.update(h);
        hash.update("\n");
    }
    const bool raster = spec.format == ElevationFormat::Raster;
    hash.update(std::format(
        "format={}\ncrs={}\nverticalDatum={}\nband={}\nresolutionM={}\ninterpolation={}\n"
        "classifyGround={}\nsurface=dtm\n",
        raster ? "raster" : "pointcloud", spec.crs, spec.verticalDatum, spec.band,
        spec.resolutionM ? std::format("{:.6f}", *spec.resolutionM) : std::string("auto"),
        spec.interpolation == LidarInterpolation::Idw ? "idw" : "tin", spec.classifyGround));
    return common::Sha256::toHex(hash.finish());
}

}  // namespace ds::terrain
