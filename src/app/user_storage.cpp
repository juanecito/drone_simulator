// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/user_storage.hpp"

#include <format>

#include "ds/common/log.hpp"
#include "ds/storage/data_location.hpp"

namespace ds::app {

storage::StorageResult<UserStorage> openUserStorage(
    const std::optional<std::filesystem::path>& databaseOverride,
    const std::filesystem::path& home) {
    std::string problem;
    std::optional<std::filesystem::path> file = databaseOverride;
    if (!file) {
        if (auto directory = storage::prepareDataDirectory(home)) {
            file = *directory / storage::kDatabaseFileName;
        } else {
            problem = directory.error().message;
        }
    }

    if (file) {
        auto service = storage::StorageService::start({.databaseFile = file});
        if (service) {
            const auto& info = (*service)->info();
            log::info("Database {} (schema v{})", info.databaseFile.string(),
                      info.migration.toVersion);
            return UserStorage{.service = std::move(*service), .warning = {}};
        }
        if (service.error().code == storage::StorageErrorCode::DriverUnavailable) {
            return std::unexpected(service.error());
        }
        problem = service.error().message;
    }

    // FR-DB-02: keep running without persistence.
    log::error("Cannot use the user database: {}", problem);
    auto memory = storage::StorageService::start({.databaseFile = std::nullopt});
    if (!memory) {
        return std::unexpected(memory.error());
    }
    return UserStorage{
        .service = std::move(*memory),
        .warning = std::format("{}\n\nDrone Simulator will run with a temporary database: "
                               "settings, mission progress and flight history will not be "
                               "saved in this session.",
                               problem)};
}

}  // namespace ds::app
