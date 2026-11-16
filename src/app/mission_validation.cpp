// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/mission_validation.hpp"

#include <format>
#include <memory>

#include "ds/app/asset_paths.hpp"
#include "ds/app/scene_config.hpp"
#include "ds/storage/data_location.hpp"

namespace ds::app {

common::Result<std::unique_ptr<MissionValidation>> MissionValidation::create(
    const std::filesystem::path& assetDir, const std::filesystem::path& libraryDir) {
    auto schema = common::JsonSchema::load(assetDir / "missions" / "mission.schema.json");
    if (!schema) {
        return std::unexpected(schema.error());
    }
    std::unique_ptr<MissionValidation> v(new MissionValidation(std::move(*schema)));
    v->context.schema = &v->schema;
    v->context.libraryDir = libraryDir;
    missions::addBuiltInKeys(v->context, assetDir);
    for (const BuiltInPreset& p : builtInPresets()) {
        constexpr std::string_view kPrefix = "builtin.";
        v->context.terrainPresets.insert(
            p.key.starts_with(kPrefix) ? p.key.substr(kPrefix.size()) : p.key);
    }
    return v;
}

int validateMissionFiles(std::span<const std::filesystem::path> files, std::string& out) {
    const std::filesystem::path library =
        storage::homeDirectory() / std::string(storage::kDataDirectoryName) / "library";
    auto validation = MissionValidation::create(findAssetDirectory(), library);
    if (!validation) {
        out += std::format("drone_simulator: {}\n", validation.error().message);
        return 1;
    }
    int invalid = 0;
    for (const std::filesystem::path& file : files) {
        const missions::MissionLoadResult r = missions::loadMissionFile(file, (*validation)->context);
        out += r.report();
        if (r.valid()) {
            out += std::format("{}: valid ({} warning(s))\n", file.string(), r.warnings.size());
        } else {
            out += std::format("{}: invalid ({} error(s))\n", file.string(), r.errors.size());
            ++invalid;
        }
    }
    return invalid == 0 ? 0 : 1;
}

}  // namespace ds::app
