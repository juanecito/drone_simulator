// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/terrain_preset_store.hpp"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <bit>

#include "store_util.hpp"

namespace ds::storage {
namespace {

constexpr const char* kColumns =
    "id, preset_key, name, source_type, seed, origin_lat, origin_lon, origin_alt, "
    "extent_width_m, extent_length_m, params_json";

TerrainPresetRecord readRow(const QSqlQuery& query) {
    TerrainPresetRecord r;
    r.id = query.value(0).toLongLong();
    r.key = query.value(1).toString().toStdString();
    r.name = query.value(2).toString().toStdString();
    r.sourceType = query.value(3).toString().toStdString();
    if (!query.value(4).isNull()) {
        r.seed =
            std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(query.value(4).toLongLong()));
    }
    r.originLatDeg = query.value(5).toDouble();
    r.originLonDeg = query.value(6).toDouble();
    r.originAltM = query.value(7).toDouble();
    if (!query.value(8).isNull()) {
        r.extentWidthM = query.value(8).toDouble();
    }
    if (!query.value(9).isNull()) {
        r.extentLengthM = query.value(9).toDouble();
    }
    r.paramsJson = query.value(10).toString().toStdString();
    return r;
}

}  // namespace

StorageResult<std::vector<TerrainPresetRecord>> TerrainPresetStore::list() {
    auto query = connection_->prepare(std::string("SELECT ") + kColumns +
                                      " FROM terrain_presets ORDER BY name COLLATE NOCASE, id");
    if (!query) {
        return std::unexpected(query.error());
    }
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<TerrainPresetRecord> records;
    while (query->next()) {
        records.push_back(readRow(*query));
    }
    return records;
}

StorageResult<std::optional<TerrainPresetRecord>> TerrainPresetStore::find(std::string_view key) {
    auto query = connection_->prepare(std::string("SELECT ") + kColumns +
                                      " FROM terrain_presets WHERE preset_key = :key");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return std::optional<TerrainPresetRecord>{};
    }
    return std::optional<TerrainPresetRecord>{readRow(*query)};
}

StorageResult<std::int64_t> TerrainPresetStore::save(const TerrainPresetRecord& p) {
    if (p.key.empty() || p.name.empty()) {
        return storageError(StorageErrorCode::InvalidValue,
                            "terrain preset key and name must not be empty");
    }
    if (!isJsonObject(p.paramsJson)) {
        return storageError(StorageErrorCode::InvalidValue,
                            "terrain preset parameters must be a JSON object");
    }
    auto query = connection_->prepare(
        "INSERT INTO terrain_presets (preset_key, name, source_type, seed, origin_lat, "
        "origin_lon, origin_alt, extent_width_m, extent_length_m, params_json) "
        "VALUES (:key, :name, :type, :seed, :lat, :lon, :alt, :width, :length, :params) "
        "ON CONFLICT (preset_key) DO UPDATE SET name = excluded.name, "
        "source_type = excluded.source_type, seed = excluded.seed, "
        "origin_lat = excluded.origin_lat, origin_lon = excluded.origin_lon, "
        "origin_alt = excluded.origin_alt, extent_width_m = excluded.extent_width_m, "
        "extent_length_m = excluded.extent_length_m, params_json = excluded.params_json "
        "RETURNING id");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(p.key));
    query->bindValue(QStringLiteral(":name"), toQString(p.name));
    query->bindValue(QStringLiteral(":type"), toQString(p.sourceType));
    query->bindValue(QStringLiteral(":seed"),
                     p.seed ? QVariant(static_cast<qlonglong>(std::bit_cast<std::int64_t>(*p.seed)))
                            : QVariant(QMetaType(QMetaType::LongLong)));
    query->bindValue(QStringLiteral(":lat"), p.originLatDeg);
    query->bindValue(QStringLiteral(":lon"), p.originLonDeg);
    query->bindValue(QStringLiteral(":alt"), p.originAltM);
    query->bindValue(QStringLiteral(":width"), optionalReal(p.extentWidthM));
    query->bindValue(QStringLiteral(":length"), optionalReal(p.extentLengthM));
    query->bindValue(QStringLiteral(":params"), toQString(p.paramsJson));  // Stored verbatim.
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return storageError(StorageErrorCode::Query, "saving the terrain preset returned no id");
    }
    return static_cast<std::int64_t>(query->value(0).toLongLong());
}

StorageResult<bool> TerrainPresetStore::remove(std::string_view key) {
    auto query = connection_->prepare("DELETE FROM terrain_presets WHERE preset_key = :key");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    return query->numRowsAffected() > 0;
}

}  // namespace ds::storage
