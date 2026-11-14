// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QMetaType>
#include <QString>
#include <QVariant>

#include <optional>
#include <string_view>

/// @file
/// @brief Helpers shared by the table stores (private to the storage module).

namespace ds::storage {

/// @brief UTF-8 view → QString.
inline QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// @brief Optional real → QVariant (NULL when empty).
inline QVariant optionalReal(const std::optional<double>& value) {
    return value ? QVariant(*value) : QVariant(QMetaType(QMetaType::Double));
}

/// @brief True if @p text is a JSON object.
inline bool isJsonObject(std::string_view text) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray(text.data(), static_cast<qsizetype>(text.size())), &error);
    return error.error == QJsonParseError::NoError && doc.isObject();
}

/// @brief True for the vehicle types of the schema (`aerial`, `ground`).
inline bool isVehicleType(std::string_view type) { return type == "aerial" || type == "ground"; }

}  // namespace ds::storage
