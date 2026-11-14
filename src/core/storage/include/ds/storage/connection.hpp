// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QSqlDatabase>
#include <QSqlQuery>

#include <filesystem>
#include <string>
#include <string_view>

#include "ds/storage/storage_error.hpp"

/// @file
/// @brief RAII SQLite connection and transaction guard over Qt SQL (FR-DB-04, FR-DB-06).
/// @ingroup storage

namespace ds::storage {

/// @brief Owns one named `QSqlDatabase` connection and removes it on destruction.
/// @details A connection may only be used on the thread that opened it (FR-DB-07). On open it
/// enables foreign keys and WAL journaling (FR-DB-04). All `QSqlQuery` objects created from it
/// must be destroyed before the connection.
class DbConnection {
public:
    /// @brief Opens (creating if needed, with mode 0600) the SQLite database @p file.
    [[nodiscard]] static StorageResult<DbConnection> open(const std::filesystem::path& file);
    /// @brief Opens a private in-memory database (FR-DB-02 fallback, tests).
    [[nodiscard]] static StorageResult<DbConnection> openInMemory();

    /// @brief Takes over @p other's connection; @p other is left closed.
    DbConnection(DbConnection&& other) noexcept;
    /// @brief Closes this connection, then takes over @p other's.
    DbConnection& operator=(DbConnection&& other) noexcept;
    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;
    ~DbConnection();

    /// @brief The underlying Qt connection.
    [[nodiscard]] QSqlDatabase database() const;
    /// @brief Unique Qt connection name.
    [[nodiscard]] const std::string& connectionName() const noexcept { return name_; }
    /// @brief Database file; empty for in-memory databases.
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }
    /// @brief True for an in-memory database.
    [[nodiscard]] bool inMemory() const noexcept { return file_.empty(); }

    /// @brief Executes one SQL statement without parameters (DDL, PRAGMA).
    [[nodiscard]] StorageResult<void> execute(std::string_view sql);
    /// @brief Prepares one statement; bind values with named placeholders (FR-DB-05).
    [[nodiscard]] StorageResult<QSqlQuery> prepare(std::string_view sql);

private:
    DbConnection(std::string name, std::filesystem::path file);
    static StorageResult<DbConnection> openNamed(const QString& databaseName,
                                                 std::filesystem::path file);
    void close() noexcept;

    std::string name_;
    std::filesystem::path file_;
};

/// @brief Runs a prepared query and maps a failure to a Query error.
[[nodiscard]] StorageResult<void> execute(QSqlQuery& query);

/// @brief Transaction guard: rolls back on scope exit unless commit() succeeded (FR-DB-06).
class Transaction {
public:
    /// @brief Starts a transaction on @p connection.
    [[nodiscard]] static StorageResult<Transaction> begin(DbConnection& connection);

    /// @brief Takes over @p other's open transaction; @p other no longer rolls back.
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    /// @brief Commits; after a failure the transaction is rolled back.
    [[nodiscard]] StorageResult<void> commit();

private:
    explicit Transaction(DbConnection& connection) noexcept : connection_(&connection) {}
    DbConnection* connection_ = nullptr;
};

}  // namespace ds::storage
