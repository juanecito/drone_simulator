// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/migrations.hpp"

#include <QString>
#include <QVariant>

#include <format>

#include "ds/storage/data_location.hpp"

namespace ds::storage {
namespace {

StorageResult<std::filesystem::path> backup(DbConnection& connection, int version) {
    std::filesystem::path target = connection.file();
    target += std::format(".bak-v{}", version);
    std::error_code ec;
    std::filesystem::remove(target, ec);  // VACUUM INTO refuses to overwrite.
    // VACUUM INTO writes a consistent, compacted copy, including WAL content.
    auto query = connection.prepare("VACUUM INTO :target");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":target"), QString::fromStdString(target.string()));
    if (auto ok = execute(*query); !ok) {
        return storageError(
            StorageErrorCode::Migration,
            std::format("cannot back up the database before migrating: {}", ok.error().message));
    }
    if (auto ok = ensurePrivateFile(target); !ok) {
        return std::unexpected(ok.error());
    }
    return target;
}

StorageResult<void> checkOrder(std::span<const Migration> migrations) {
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        if (migrations[i].version != static_cast<int>(i) + 1) {
            return storageError(StorageErrorCode::Migration,
                                std::format("migration #{} has version {}, expected {}", i,
                                            migrations[i].version, i + 1));
        }
    }
    return {};
}

}  // namespace

StorageResult<int> schemaVersion(DbConnection& connection) {
    if (auto ok = connection.execute(
            "CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL)");
        !ok) {
        return std::unexpected(ok.error());
    }
    auto query = connection.prepare("SELECT COALESCE(MAX(version), 0) FROM schema_version");
    if (!query) {
        return std::unexpected(query.error());
    }
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    return query->next() ? query->value(0).toInt() : 0;
}

StorageResult<MigrationReport> migrate(DbConnection& connection,
                                       std::span<const Migration> migrations) {
    if (auto ok = checkOrder(migrations); !ok) {
        return std::unexpected(ok.error());
    }
    const auto current = schemaVersion(connection);
    if (!current) {
        return std::unexpected(current.error());
    }
    const int latest = static_cast<int>(migrations.size());
    MigrationReport report{*current, *current, std::nullopt};
    if (*current > latest) {
        return storageError(
            StorageErrorCode::NewerSchema,
            std::format("the database has schema version {}, but this version of Drone "
                        "Simulator only knows versions up to {}; please update the application",
                        *current, latest));
    }
    if (*current == latest) {
        return report;
    }

    if (*current > 0 && !connection.inMemory()) {
        auto saved = backup(connection, *current);
        if (!saved) {
            return std::unexpected(saved.error());
        }
        report.backup = *saved;
    }

    auto transaction = Transaction::begin(connection);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    for (std::size_t i = static_cast<std::size_t>(*current); i < migrations.size(); ++i) {
        const Migration& migration = migrations[i];
        for (const std::string_view statement : migration.statements) {
            if (auto ok = connection.execute(statement); !ok) {
                return storageError(
                    StorageErrorCode::Migration,
                    std::format("migration to version {} ({}) failed: {}", migration.version,
                                migration.description, ok.error().message));
            }
        }
    }
    if (auto ok = connection.execute("DELETE FROM schema_version"); !ok) {
        return std::unexpected(ok.error());
    }
    auto insert = connection.prepare("INSERT INTO schema_version (version) VALUES (:version)");
    if (!insert) {
        return std::unexpected(insert.error());
    }
    insert->bindValue(QStringLiteral(":version"), latest);
    if (auto ok = execute(*insert); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = transaction->commit(); !ok) {
        return storageError(StorageErrorCode::Migration, ok.error().message);
    }
    report.toVersion = latest;
    return report;
}

StorageResult<MigrationReport> migrate(DbConnection& connection) {
    return migrate(connection, schemaMigrations());
}

}  // namespace ds::storage
