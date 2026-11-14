// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/settings_store.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QString>
#include <QVariant>

#include <cmath>
#include <format>

namespace ds::storage {
namespace {

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// Parses any JSON value, including bare scalars (QJsonDocument only accepts arrays/objects).
std::optional<QJsonValue> parseValue(std::string_view json) {
    QByteArray wrapped = "[";
    wrapped.append(json.data(), static_cast<qsizetype>(json.size()));
    wrapped.append(']');
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(wrapped, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() != 1) {
        return std::nullopt;
    }
    return doc.array().at(0);
}

std::string serialize(const QJsonValue& value) {
    const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    // Strip the surrounding "[" and "]".
    return array.mid(1, array.size() - 2).toStdString();
}

StorageError wrongType(std::string_view key, std::string_view expected) {
    return StorageError{StorageErrorCode::InvalidValue,
                        std::format("setting '{}' is not a {}", key, expected)};
}

}  // namespace

StorageResult<std::optional<std::string>> SettingsStore::json(std::string_view key) {
    auto query = connection_->prepare("SELECT value FROM settings WHERE key = :key");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>{query->value(0).toString().toStdString()};
}

StorageResult<void> SettingsStore::setJson(std::string_view key, std::string_view json) {
    if (key.empty()) {
        return storageError(StorageErrorCode::InvalidValue, "setting key must not be empty");
    }
    const auto value = parseValue(json);
    if (!value) {
        return storageError(StorageErrorCode::InvalidValue,
                            std::format("value of setting '{}' is not valid JSON", key));
    }
    auto query = connection_->prepare(
        "INSERT INTO settings (key, value) VALUES (:key, :value) "
        "ON CONFLICT (key) DO UPDATE SET value = excluded.value");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(key));
    query->bindValue(QStringLiteral(":value"), QString::fromStdString(serialize(*value)));
    return execute(*query);
}

namespace {

// Reads a setting and converts it with @p convert, which returns nullopt for a wrong type.
template <typename T, typename Convert>
StorageResult<std::optional<T>> typed(SettingsStore& store, std::string_view key,
                                      std::string_view typeName, Convert convert) {
    auto raw = store.json(key);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    if (!raw->has_value()) {
        return std::optional<T>{};
    }
    const auto value = parseValue(**raw);
    if (!value) {
        return std::unexpected(StorageError{StorageErrorCode::InvalidValue,
                                            std::format("setting '{}' holds invalid JSON", key)});
    }
    std::optional<T> converted = convert(*value);
    if (!converted) {
        return std::unexpected(wrongType(key, typeName));
    }
    return converted;
}

}  // namespace

StorageResult<std::optional<std::string>> SettingsStore::string(std::string_view key) {
    return typed<std::string>(*this, key, "string",
                              [](const QJsonValue& v) -> std::optional<std::string> {
                                  if (!v.isString()) {
                                      return std::nullopt;
                                  }
                                  return v.toString().toStdString();
                              });
}

StorageResult<void> SettingsStore::setString(std::string_view key, std::string_view value) {
    return setJson(key, serialize(QJsonValue(toQString(value))));
}

StorageResult<std::optional<std::int64_t>> SettingsStore::integer(std::string_view key) {
    return typed<std::int64_t>(*this, key, "integer",
                               [](const QJsonValue& v) -> std::optional<std::int64_t> {
                                   if (!v.isDouble()) {
                                       return std::nullopt;
                                   }
                                   const double d = v.toDouble();
                                   if (d != std::trunc(d) || std::abs(d) > 9007199254740992.0) {
                                       return std::nullopt;
                                   }
                                   return static_cast<std::int64_t>(d);
                               });
}

StorageResult<void> SettingsStore::setInteger(std::string_view key, std::int64_t value) {
    return setJson(key, std::format("{}", value));
}

StorageResult<std::optional<double>> SettingsStore::number(std::string_view key) {
    return typed<double>(*this, key, "number", [](const QJsonValue& v) -> std::optional<double> {
        if (!v.isDouble()) {
            return std::nullopt;
        }
        return v.toDouble();
    });
}

StorageResult<void> SettingsStore::setNumber(std::string_view key, double value) {
    if (!std::isfinite(value)) {
        return storageError(StorageErrorCode::InvalidValue,
                            std::format("setting '{}' must be a finite number", key));
    }
    return setJson(key, std::format("{}", value));  // Shortest round-trip representation.
}

StorageResult<std::optional<bool>> SettingsStore::boolean(std::string_view key) {
    return typed<bool>(*this, key, "boolean", [](const QJsonValue& v) -> std::optional<bool> {
        if (!v.isBool()) {
            return std::nullopt;
        }
        return v.toBool();
    });
}

StorageResult<void> SettingsStore::setBoolean(std::string_view key, bool value) {
    return setJson(key, value ? "true" : "false");
}

StorageResult<std::optional<std::vector<std::uint8_t>>> SettingsStore::bytes(std::string_view key) {
    return typed<std::vector<std::uint8_t>>(
        *this, key, "base64 string",
        [](const QJsonValue& v) -> std::optional<std::vector<std::uint8_t>> {
            if (!v.isString()) {
                return std::nullopt;
            }
            const auto decoded = QByteArray::fromBase64Encoding(
                v.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded) {
                return std::nullopt;
            }
            const QByteArray& data = *decoded;
            return std::vector<std::uint8_t>(data.begin(), data.end());
        });
}

StorageResult<void> SettingsStore::setBytes(std::string_view key,
                                            const std::vector<std::uint8_t>& value) {
    const QByteArray data(reinterpret_cast<const char*>(value.data()),
                          static_cast<qsizetype>(value.size()));
    return setJson(key, serialize(QJsonValue(QString::fromLatin1(data.toBase64()))));
}

StorageResult<void> SettingsStore::remove(std::string_view key) {
    auto query = connection_->prepare("DELETE FROM settings WHERE key = :key");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(key));
    return execute(*query);
}

StorageResult<std::vector<std::string>> SettingsStore::keys() {
    auto query = connection_->prepare("SELECT key FROM settings ORDER BY key");
    if (!query) {
        return std::unexpected(query.error());
    }
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<std::string> result;
    while (query->next()) {
        result.push_back(query->value(0).toString().toStdString());
    }
    return result;
}

}  // namespace ds::storage
