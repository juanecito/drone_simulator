// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/storage_service.hpp"

namespace ds::storage {

StorageResult<std::unique_ptr<StorageService>> StorageService::start(const StorageConfig& config) {
    std::unique_ptr<StorageService> service(new StorageService());
    std::promise<StorageResult<StorageInfo>> started;
    auto startedFuture = started.get_future();
    service->thread_ = std::thread(&StorageService::run, service.get(), config, std::move(started));
    auto info = startedFuture.get();
    if (!info) {
        service->thread_.join();  // The thread has already left run().
        return std::unexpected(info.error());
    }
    service->info_ = std::move(*info);
    return service;
}

StorageService::~StorageService() {
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void StorageService::enqueue(Job job) {
    {
        const std::scoped_lock lock(mutex_);
        if (!stopping_) {
            queue_.push_back(std::move(job));
            job = nullptr;
        }
    }
    // Destroying an unqueued packaged_task makes its future report broken_promise.
    wake_.notify_one();
}

void StorageService::run(StorageConfig config, std::promise<StorageResult<StorageInfo>> started) {
    auto opened = config.databaseFile ? DbConnection::open(*config.databaseFile)
                                      : DbConnection::openInMemory();
    if (!opened) {
        started.set_value(std::unexpected(opened.error()));
        return;
    }
    DbConnection connection = std::move(*opened);
    auto migrated = migrate(connection);
    if (!migrated) {
        started.set_value(std::unexpected(migrated.error()));
        return;
    }
    started.set_value(StorageInfo{connection.file(), connection.inMemory(), *migrated});

    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                break;  // Stopping and drained.
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job(connection);
    }
}

}  // namespace ds::storage
