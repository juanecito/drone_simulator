// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/data_location.hpp"

#include <QDir>

#include <cerrno>
#include <format>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ds::storage {
namespace {

namespace fs = std::filesystem;

constexpr fs::perms kPrivateDirectory = fs::perms::owner_all;
constexpr fs::perms kGroupOther = fs::perms::group_all | fs::perms::others_all;

}  // namespace

fs::path homeDirectory() { return fs::path(QDir::homePath().toStdString()); }

StorageResult<fs::path> prepareDataDirectory(const fs::path& home) {
    if (home.empty() || !home.is_absolute()) {
        return storageError(StorageErrorCode::DataDirectory,
                            "the home directory is not set ($HOME is empty or relative)");
    }
    const fs::path directory = home / kDataDirectoryName;
    std::error_code ec;
    if (!fs::exists(directory, ec)) {
        // mkdir with the final mode (minus umask, which can only remove bits); then set it
        // explicitly in case the umask removed owner bits.
        if (::mkdir(directory.c_str(), 0700) != 0) {
            return storageError(
                StorageErrorCode::DataDirectory,
                std::format("cannot create {}: {}", directory.string(),
                            std::error_code(errno, std::generic_category()).message()));
        }
    } else if (!fs::is_directory(directory, ec)) {
        return storageError(StorageErrorCode::DataDirectory,
                            std::format("{} exists but is not a directory", directory.string()));
    }
    fs::permissions(directory, kPrivateDirectory, fs::perm_options::replace, ec);
    if (ec) {
        return storageError(
            StorageErrorCode::DataDirectory,
            std::format("cannot set permissions of {}: {}", directory.string(), ec.message()));
    }
    if (::access(directory.c_str(), W_OK | X_OK) != 0) {
        return storageError(StorageErrorCode::DataDirectory,
                            std::format("{} is not writable", directory.string()));
    }
    return directory;
}

StorageResult<void> ensurePrivateFile(const fs::path& file) {
    // O_EXCL: create with mode 0600 from the start, never with wider permissions.
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ::close(fd);
        return {};
    }
    if (errno != EEXIST) {
        return storageError(StorageErrorCode::CannotOpen,
                            std::format("cannot create {}: {}", file.string(),
                                        std::error_code(errno, std::generic_category()).message()));
    }
    std::error_code ec;
    const auto current = fs::status(file, ec).permissions();
    if (!ec && (current & kGroupOther) != fs::perms::none) {
        fs::permissions(file, kGroupOther, fs::perm_options::remove, ec);
    }
    if (ec) {
        return storageError(
            StorageErrorCode::CannotOpen,
            std::format("cannot set permissions of {}: {}", file.string(), ec.message()));
    }
    return {};
}

}  // namespace ds::storage
