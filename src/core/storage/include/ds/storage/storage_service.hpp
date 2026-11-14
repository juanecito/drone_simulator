// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

#include "ds/storage/connection.hpp"
#include "ds/storage/migrations.hpp"

/// @file
/// @brief The storage thread: owns the user database connection and runs requests (FR-DB-07).
/// @ingroup storage

namespace ds::storage {

/// @brief Where the service keeps its database.
struct StorageConfig {
    /// Database file; `std::nullopt` ⇒ in-memory database (nothing is persisted, FR-DB-02).
    std::optional<std::filesystem::path> databaseFile;
    /// Schema to apply; empty ⇒ the user database schema (schemaMigrations()). The tile cache
    /// index uses cacheSchemaMigrations() on its own `cache.db` (FR-NET-04).
    std::span<const Migration> migrations;
};

/// @brief Facts about the open database.
struct StorageInfo {
    std::filesystem::path databaseFile;  ///< Empty for an in-memory database.
    bool inMemory = false;               ///< True if nothing is persisted.
    MigrationReport migration;           ///< Schema version before/after start-up.
};

/// @brief Runs all database work on one dedicated thread.
/// @details The connection is opened and migrated on that thread (Qt SQL connections are
/// thread-bound). Other threads submit tasks and get a `std::future`; tasks run in submission
/// order. The destructor finishes the queued tasks, then closes the database.
class StorageService {
public:
    /// @brief Starts the thread, opens the database and applies migrations.
    /// @return The running service, or the error that prevented opening or migrating.
    [[nodiscard]] static StorageResult<std::unique_ptr<StorageService>> start(
        const StorageConfig& config);

    StorageService(const StorageService&) = delete;
    StorageService& operator=(const StorageService&) = delete;
    StorageService(StorageService&&) = delete;
    StorageService& operator=(StorageService&&) = delete;
    ~StorageService();

    /// @brief Information gathered at start-up.
    [[nodiscard]] const StorageInfo& info() const noexcept { return info_; }

    /// @brief Queues @p task, called as `task(DbConnection&)` on the storage thread.
    /// @return A future for the task's result. Exceptions thrown by the task are stored in it.
    template <typename Task>
    [[nodiscard]] auto submit(Task&& task)
        -> std::future<std::invoke_result_t<std::decay_t<Task>&, DbConnection&>> {
        using R = std::invoke_result_t<std::decay_t<Task>&, DbConnection&>;
        std::packaged_task<R(DbConnection&)> packaged(std::forward<Task>(task));
        auto future = packaged.get_future();
        enqueue([job = std::move(packaged)](DbConnection& c) mutable { job(c); });
        return future;
    }

private:
    using Job = std::move_only_function<void(DbConnection&)>;

    StorageService() = default;
    void enqueue(Job job);
    void run(StorageConfig config, std::promise<StorageResult<StorageInfo>> started);

    StorageInfo info_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace ds::storage
