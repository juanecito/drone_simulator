// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/keyring.hpp"

#include <QEventLoop>
#include <QTimer>

#include <format>

#include <qt6keychain/keychain.h>

namespace ds::app {
namespace {

const QString kService = QStringLiteral("DroneSimulator");

// Runs @p job until it finishes or @p timeout passes; false on timeout.
bool runJob(QKeychain::Job& job, std::chrono::milliseconds timeout) {
    job.setAutoDelete(false);
    job.setInsecureFallback(false);
    QEventLoop loop;
    bool finished = false;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, [&] {
        finished = true;
        loop.quit();
    });
    QTimer::singleShot(timeout, &loop, &QEventLoop::quit);
    job.start();
    if (!finished) {
        loop.exec();
    }
    return finished;
}

common::Error failure(const char* what, const QKeychain::Job& job) {
    return common::Error{
        std::format("the system keyring cannot {}: {}", what, job.errorString().toStdString())};
}

}  // namespace

std::string apiKeyEntry(std::string_view providerKey) {
    return std::format("provider/{}", providerKey);
}

// --- OsKeyring -------------------------------------------------------------------------------

OsKeyring::OsKeyring(std::chrono::milliseconds timeout) : timeout_(timeout) {}

common::Result<std::optional<std::string>> OsKeyring::read(std::string_view name) {
    QKeychain::ReadPasswordJob job(kService);
    job.setKey(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    if (!runJob(job, timeout_)) {
        return common::makeError("the system keyring did not answer in time");
    }
    if (job.error() == QKeychain::EntryNotFound) {
        return std::optional<std::string>{};
    }
    if (job.error() != QKeychain::NoError) {
        return std::unexpected(failure("be read", job));
    }
    return std::optional(job.textData().toStdString());
}

common::Result<void> OsKeyring::write(std::string_view name, std::string_view secret) {
    QKeychain::WritePasswordJob job(kService);
    job.setKey(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    job.setTextData(QString::fromUtf8(secret.data(), static_cast<qsizetype>(secret.size())));
    if (!runJob(job, timeout_)) {
        return common::makeError("the system keyring did not answer in time");
    }
    if (job.error() != QKeychain::NoError) {
        return std::unexpected(failure("store the key", job));
    }
    return {};
}

common::Result<void> OsKeyring::remove(std::string_view name) {
    QKeychain::DeletePasswordJob job(kService);
    job.setKey(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    if (!runJob(job, timeout_)) {
        return common::makeError("the system keyring did not answer in time");
    }
    if (job.error() != QKeychain::NoError && job.error() != QKeychain::EntryNotFound) {
        return std::unexpected(failure("remove the key", job));
    }
    return {};
}

// --- MemoryKeyring ---------------------------------------------------------------------------

common::Result<std::optional<std::string>> MemoryKeyring::read(std::string_view name) {
    const std::scoped_lock lock(mutex_);
    if (!available_) {
        return common::makeError("no keyring backend is available");
    }
    const auto it = secrets_.find(name);
    return it == secrets_.end() ? std::optional<std::string>{} : std::optional(it->second);
}

common::Result<void> MemoryKeyring::write(std::string_view name, std::string_view secret) {
    const std::scoped_lock lock(mutex_);
    if (!available_) {
        return common::makeError("no keyring backend is available");
    }
    secrets_.insert_or_assign(std::string(name), std::string(secret));
    return {};
}

common::Result<void> MemoryKeyring::remove(std::string_view name) {
    const std::scoped_lock lock(mutex_);
    if (!available_) {
        return common::makeError("no keyring backend is available");
    }
    if (const auto it = secrets_.find(name); it != secrets_.end()) {
        secrets_.erase(it);
    }
    return {};
}

void MemoryKeyring::setAvailable(bool available) {
    const std::scoped_lock lock(mutex_);
    available_ = available;
}

std::optional<std::string> MemoryKeyring::peek(std::string_view name) const {
    const std::scoped_lock lock(mutex_);
    const auto it = secrets_.find(name);
    return it == secrets_.end() ? std::nullopt : std::optional(it->second);
}

}  // namespace ds::app
