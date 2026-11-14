// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/connection.hpp"

#include <QSqlError>
#include <QString>

#include <atomic>
#include <format>
#include <utility>

#include "ds/storage/data_location.hpp"

namespace ds::storage {
namespace {

const QString kDriver = QStringLiteral("QSQLITE");
std::atomic<unsigned> gConnectionCounter{0};

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string describe(const QSqlError& error) { return error.text().toStdString(); }

}  // namespace

DbConnection::DbConnection(std::string name, std::filesystem::path file)
    : name_(std::move(name)), file_(std::move(file)) {}

DbConnection::DbConnection(DbConnection&& other) noexcept
    : name_(std::exchange(other.name_, {})), file_(std::exchange(other.file_, {})) {}

DbConnection& DbConnection::operator=(DbConnection&& other) noexcept {
    if (this != &other) {
        close();
        name_ = std::exchange(other.name_, {});
        file_ = std::exchange(other.file_, {});
    }
    return *this;
}

DbConnection::~DbConnection() { close(); }

void DbConnection::close() noexcept {
    if (name_.empty()) {
        return;
    }
    const QString name = QString::fromStdString(name_);
    {
        QSqlDatabase db = QSqlDatabase::database(name, false);
        db.close();
    }  // No QSqlDatabase handle may outlive this scope, or removeDatabase() warns.
    QSqlDatabase::removeDatabase(name);
    name_.clear();
}

StorageResult<DbConnection> DbConnection::open(const std::filesystem::path& file) {
    if (file.empty()) {
        return storageError(StorageErrorCode::CannotOpen, "no database file given");
    }
    if (!QSqlDatabase::isDriverAvailable(kDriver)) {
        return storageError(StorageErrorCode::DriverUnavailable,
                            "the Qt SQLite driver (QSQLITE) is not available; install the "
                            "package libqt6sql6-sqlite");
    }
    if (auto created = ensurePrivateFile(file); !created) {
        return std::unexpected(created.error());
    }
    return openNamed(QString::fromStdString(file.string()), file);
}

StorageResult<DbConnection> DbConnection::openInMemory() {
    if (!QSqlDatabase::isDriverAvailable(kDriver)) {
        return storageError(StorageErrorCode::DriverUnavailable,
                            "the Qt SQLite driver (QSQLITE) is not available; install the "
                            "package libqt6sql6-sqlite");
    }
    return openNamed(QStringLiteral(":memory:"), {});
}

StorageResult<DbConnection> DbConnection::openNamed(const QString& databaseName,
                                                    std::filesystem::path file) {
    const std::string name = std::format("ds_storage_{}", gConnectionCounter.fetch_add(1));
    std::string failure;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(kDriver, QString::fromStdString(name));
        db.setDatabaseName(databaseName);
        if (!db.open()) {
            failure = describe(db.lastError());
        }
    }
    DbConnection connection(name, std::move(file));  // Removes the Qt connection on failure.
    if (!failure.empty()) {
        return storageError(
            StorageErrorCode::CannotOpen,
            std::format("cannot open database {}: {}", databaseName.toStdString(), failure));
    }
    for (const std::string_view pragma :
         {"PRAGMA foreign_keys = ON", "PRAGMA journal_mode = WAL"}) {
        if (auto ok = connection.execute(pragma); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return connection;
}

QSqlDatabase DbConnection::database() const {
    return QSqlDatabase::database(QString::fromStdString(name_), false);
}

StorageResult<void> DbConnection::execute(std::string_view sql) {
    QSqlQuery query(database());
    if (!query.exec(toQString(sql))) {
        return storageError(StorageErrorCode::Query,
                            std::format("{} — in: {}", describe(query.lastError()), sql));
    }
    return {};
}

StorageResult<QSqlQuery> DbConnection::prepare(std::string_view sql) {
    QSqlQuery query(database());
    if (!query.prepare(toQString(sql))) {
        return storageError(StorageErrorCode::Query,
                            std::format("{} — in: {}", describe(query.lastError()), sql));
    }
    return query;
}

StorageResult<void> execute(QSqlQuery& query) {
    if (!query.exec()) {
        return storageError(StorageErrorCode::Query,
                            std::format("{} — in: {}", describe(query.lastError()),
                                        query.lastQuery().toStdString()));
    }
    return {};
}

StorageResult<Transaction> Transaction::begin(DbConnection& connection) {
    QSqlDatabase db = connection.database();
    if (!db.transaction()) {
        return storageError(StorageErrorCode::Transaction,
                            std::format("cannot begin transaction: {}", describe(db.lastError())));
    }
    return Transaction(connection);
}

Transaction::Transaction(Transaction&& other) noexcept
    : connection_(std::exchange(other.connection_, nullptr)) {}

Transaction::~Transaction() {
    if (connection_ != nullptr) {
        connection_->database().rollback();
    }
}

StorageResult<void> Transaction::commit() {
    if (connection_ == nullptr) {
        return storageError(StorageErrorCode::Transaction, "transaction is not active");
    }
    QSqlDatabase db = connection_->database();
    if (!db.commit()) {
        const std::string failure = describe(db.lastError());
        db.rollback();
        connection_ = nullptr;
        return storageError(StorageErrorCode::Transaction,
                            std::format("cannot commit transaction: {}", failure));
    }
    connection_ = nullptr;
    return {};
}

}  // namespace ds::storage
