// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/missions/catalog.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace ds::missions {
namespace {

namespace fs = std::filesystem;

bool isMissionFile(const fs::path& p) {
    const std::string name = p.filename().string();
    return name.size() > 13 && name.ends_with(".mission.json");
}

std::vector<fs::path> missionFilesIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && !entry.is_symlink(ec) && isMissionFile(entry.path())) {
            out.push_back(entry.path());
        }
    }
    std::ranges::sort(out);
    return out;
}

// Id written in the file, even if the file is otherwise invalid.
std::string keyOf(const fs::path& file) {
    if (auto j = common::readJsonFile(file, kMaxMissionFileBytes);
        j && j->is_object() && j->contains("id") && (*j)["id"].is_string()) {
        return (*j)["id"].get<std::string>();
    }
    std::string stem = file.filename().string();
    return stem.substr(0, stem.size() - std::string_view(".mission.json").size());
}

}  // namespace

std::vector<fs::path> findMissionFiles(const fs::path& folder) {
    std::vector<fs::path> out = missionFilesIn(folder);
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.is_directory(ec) && !entry.is_symlink(ec)) {
            dirs.push_back(entry.path());
        }
    }
    std::ranges::sort(dirs);
    for (const fs::path& dir : dirs) {
        const auto inside = missionFilesIn(dir);
        out.insert(out.end(), inside.begin(), inside.end());
    }
    return out;
}

std::vector<CatalogEntry> scanMissions(const fs::path& builtInDir, const fs::path& userDir,
                                       const ValidationContext& context) {
    std::vector<CatalogEntry> entries;
    std::map<std::string, fs::path> taken;
    const auto scan = [&](const fs::path& dir, MissionOrigin origin) {
        if (dir.empty()) {
            return;
        }
        for (const fs::path& file : findMissionFiles(dir)) {
            CatalogEntry e;
            e.origin = origin;
            e.load = loadMissionFile(file, context);
            e.key = e.load.definition ? e.load.definition->id : keyOf(file);
            if (const auto it = taken.find(e.key); it != taken.end()) {
                e.load.definition.reset();
                e.load.errors.push_back(
                    {"id", std::format("mission id \"{}\" is already used by {}", e.key,
                                       it->second.string())});
            } else {
                taken.emplace(e.key, file);
            }
            entries.push_back(std::move(e));
        }
    };
    scan(builtInDir, MissionOrigin::BuiltIn);
    scan(userDir, MissionOrigin::User);
    return entries;
}

common::Result<fs::path> importMission(const fs::path& source, const fs::path& userDir,
                                       bool overwrite) {
    std::error_code ec;
    const auto status = fs::symlink_status(source, ec);
    if (ec || !fs::exists(status)) {
        return common::makeError(std::format("{} does not exist", source.string()));
    }
    if (fs::is_symlink(status)) {
        return common::makeError(std::format("{} is a symbolic link", source.string()));
    }
    fs::create_directories(userDir, ec);
    if (ec) {
        return common::makeError(std::format("cannot create {}: {}", userDir.string(), ec.message()));
    }
    if (fs::is_regular_file(status)) {
        if (!isMissionFile(source)) {
            return common::makeError("a mission file must end in .mission.json");
        }
        const fs::path target = userDir / source.filename();
        if (fs::exists(target, ec) && !overwrite) {
            return common::makeError(std::format("{} already exists", target.string()));
        }
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return common::makeError(std::format("cannot copy {}: {}", source.string(), ec.message()));
        }
        return target;
    }
    if (!fs::is_directory(status)) {
        return common::makeError(std::format("{} is not a file or folder", source.string()));
    }
    const auto files = missionFilesIn(source);
    if (files.size() != 1) {
        return common::makeError(std::format("a mission folder must contain exactly one "
                                             ".mission.json file ({} found)",
                                             files.size()));
    }
    const fs::path target = userDir / source.filename();
    if (fs::exists(target, ec)) {
        if (!overwrite) {
            return common::makeError(std::format("{} already exists", target.string()));
        }
        fs::remove_all(target, ec);
    }
    fs::copy(source, target, fs::copy_options::recursive | fs::copy_options::skip_symlinks, ec);
    if (ec) {
        return common::makeError(std::format("cannot copy {}: {}", source.string(), ec.message()));
    }
    return target / files.front().filename();
}

std::map<std::string, bool> unlockedMissions(const std::vector<CampaignMission>& missions,
                                             const std::map<std::string, int>& bestStars) {
    const auto stars = [&](const std::string& key) {
        const auto it = bestStars.find(key);
        return it == bestStars.end() ? 0 : it->second;
    };
    std::map<std::string, std::vector<const CampaignMission*>> chapters;
    for (const CampaignMission& m : missions) {
        chapters[m.chapter].push_back(&m);
    }
    std::map<std::string, bool> unlocked;
    for (auto& [chapter, list] : chapters) {
        std::ranges::sort(list, [](const CampaignMission* a, const CampaignMission* b) {
            return std::tie(a->order, a->key) < std::tie(b->order, b->key);
        });
        for (std::size_t i = 0; i < list.size(); ++i) {
            const CampaignMission& m = *list[i];
            bool open = false;
            if (m.unlock) {
                open = std::ranges::all_of(m.unlock->missions, [&](const std::string& k) {
                    return stars(k) >= m.unlock->minStars;
                });
            } else {
                open = chapter.empty() || i == 0 || stars(list[i - 1]->key) >= 1;
            }
            unlocked[m.key] = open;
        }
    }
    return unlocked;
}

}  // namespace ds::missions
