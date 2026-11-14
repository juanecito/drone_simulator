// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/provider_store.hpp"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <charconv>
#include <chrono>
#include <format>

#include "ds/common/log.hpp"
#include "ds/storage/storage_service.hpp"

#include "store_util.hpp"

namespace ds::storage {
namespace {

QVariant optionalInt(const std::optional<int>& v) {
    return v ? QVariant(*v) : QVariant(QMetaType(QMetaType::Int));
}

QVariant optionalInt64(const std::optional<std::int64_t>& v) {
    return v ? QVariant(static_cast<qlonglong>(*v)) : QVariant(QMetaType(QMetaType::LongLong));
}

std::string text(const QSqlQuery& q, int column) {
    return q.value(column).toString().toStdString();
}

StorageResult<std::optional<net::ProviderConfig>> parseRow(const QSqlQuery& q) {
    const std::string json = text(q, 0);
    auto parsed = common::parseJson(json);
    if (!parsed) {
        return storageError(StorageErrorCode::InvalidValue, parsed.error().message);
    }
    auto provider = net::parseProvider(*parsed);
    if (!provider) {
        log::warn("storage: provider row \"{}\" no longer valid: {}", text(q, 1),
                  provider.error().message);
        return std::optional<net::ProviderConfig>{};
    }
    provider->origin = text(q, 2);
    return std::optional{std::move(*provider)};
}

template <typename T>
common::Result<T> toCommon(StorageResult<T> r) {
    if (!r) {
        return common::makeError(r.error().message);
    }
    if constexpr (std::is_void_v<T>) {
        return {};
    } else {
        return std::move(*r);
    }
}

/// Runs @p task on the storage thread and waits for it.
template <typename Task>
auto runOn(StorageService& service, Task&& task) {
    using R = std::invoke_result_t<std::decay_t<Task>&, DbConnection&>;
    try {
        return service.submit(std::forward<Task>(task)).get();
    } catch (const std::exception& e) {  // Broken promise: the service is stopping.
        return R(storageError(StorageErrorCode::Stopped, e.what()));
    }
}

}  // namespace

// --- Time ---------------------------------------------------------------------------------------

std::string unixToIsoUtc(std::int64_t unixS) {
    const std::chrono::sys_seconds t{std::chrono::seconds(unixS)};
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", t);
}

std::optional<std::int64_t> isoUtcToUnix(std::string_view s) {
    // YYYY-MM-DDTHH:MM:SSZ
    if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
        s[16] != ':' || s[19] != 'Z') {
        return std::nullopt;
    }
    auto num = [&](std::size_t pos, std::size_t len) -> std::optional<int> {
        int v = 0;
        const char* b = s.data() + pos;
        if (auto [p, ec] = std::from_chars(b, b + len, v); ec != std::errc{} || p != b + len) {
            return std::nullopt;
        }
        return v;
    };
    const auto y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2),
               se = num(17, 2);
    if (!y || !mo || !d || !h || !mi || !se || *h > 23 || *mi > 59 || *se > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year(*y),
                                          std::chrono::month(static_cast<unsigned>(*mo)),
                                          std::chrono::day(static_cast<unsigned>(*d))};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days(ymd).time_since_epoch().count();
    return (static_cast<std::int64_t>(days) * 86400) + (*h * 3600) + (*mi * 60) + *se;
}

// --- ProviderStore ------------------------------------------------------------------------------

StorageResult<std::optional<std::int64_t>> ProviderStore::idOf(std::string_view key) {
    auto q = connection_->prepare("SELECT id FROM data_providers WHERE provider_key = :key");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    if (!q->next()) {
        return std::optional<std::int64_t>{};
    }
    return std::optional<std::int64_t>{q->value(0).toLongLong()};
}

StorageResult<std::int64_t> ProviderStore::upsert(const net::ProviderConfig& p) {
    auto q = connection_->prepare(
        "INSERT INTO data_providers (provider_key, name, kind, protocol, url_template, "
        "params_json, attribution, licence, requires_key, allow_cache, max_age_days, "
        "rate_limit_rps, daily_call_limit, insecure_http, origin) VALUES (:key, :name, :kind, "
        ":protocol, :url, :params, :attribution, :licence, :requiresKey, :allowCache, :maxAge, "
        ":rps, :daily, :insecure, :origin) "
        "ON CONFLICT (provider_key) DO UPDATE SET name = excluded.name, kind = excluded.kind, "
        "protocol = excluded.protocol, url_template = excluded.url_template, "
        "params_json = excluded.params_json, attribution = excluded.attribution, "
        "licence = excluded.licence, requires_key = excluded.requires_key, "
        "allow_cache = excluded.allow_cache, max_age_days = excluded.max_age_days, "
        "rate_limit_rps = excluded.rate_limit_rps, daily_call_limit = excluded.daily_call_limit, "
        "insecure_http = excluded.insecure_http, origin = excluded.origin RETURNING id");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":key"), toQString(p.key));
    q->bindValue(QStringLiteral(":name"), toQString(p.name));
    q->bindValue(QStringLiteral(":kind"), toQString(p.kind));
    q->bindValue(QStringLiteral(":protocol"), toQString(p.protocol));
    q->bindValue(QStringLiteral(":url"), toQString(p.urlTemplate));
    q->bindValue(QStringLiteral(":params"), toQString(net::toJson(p).dump()));
    q->bindValue(QStringLiteral(":attribution"), toQString(p.attribution));
    q->bindValue(QStringLiteral(":licence"), toQString(p.licence));
    q->bindValue(QStringLiteral(":requiresKey"), p.requiresKey ? 1 : 0);
    q->bindValue(QStringLiteral(":allowCache"), p.allowCache ? 1 : 0);
    q->bindValue(QStringLiteral(":maxAge"), optionalInt(p.maxAgeDays));
    q->bindValue(QStringLiteral(":rps"), optionalReal(p.rateLimitRps));
    q->bindValue(QStringLiteral(":daily"), optionalInt(p.dailyCallLimit));
    q->bindValue(QStringLiteral(":insecure"), p.insecureHttp ? 1 : 0);
    q->bindValue(QStringLiteral(":origin"), toQString(p.origin));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    if (!q->next()) {
        return storageError(StorageErrorCode::Query, "saving the provider returned no id");
    }
    return static_cast<std::int64_t>(q->value(0).toLongLong());
}

StorageResult<void> ProviderStore::syncBuiltin(std::span<const net::ProviderConfig> presets) {
    auto tx = Transaction::begin(*connection_);
    if (!tx) {
        return std::unexpected(tx.error());
    }
    for (net::ProviderConfig p : presets) {
        auto existing = find(p.key);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        if (*existing && (*existing)->origin == "user") {
            log::info("storage: keeping user provider \"{}\" (same key as a built-in preset)",
                      p.key);
            continue;
        }
        p.origin = "builtin";
        if (auto ok = upsert(p); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return tx->commit();
}

StorageResult<std::vector<net::ProviderConfig>> ProviderStore::list() {
    auto q = connection_->prepare(
        "SELECT params_json, provider_key, origin FROM data_providers "
        "ORDER BY name COLLATE NOCASE, id");
    if (!q) {
        return std::unexpected(q.error());
    }
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<net::ProviderConfig> out;
    while (q->next()) {
        auto row = parseRow(*q);
        if (!row) {
            return std::unexpected(row.error());
        }
        if (*row) {
            out.push_back(std::move(**row));
        }
    }
    return out;
}

StorageResult<std::optional<net::ProviderConfig>> ProviderStore::find(std::string_view key) {
    auto q = connection_->prepare(
        "SELECT params_json, provider_key, origin FROM data_providers WHERE provider_key = :key");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    if (!q->next()) {
        return std::optional<net::ProviderConfig>{};
    }
    return parseRow(*q);
}

StorageResult<std::int64_t> ProviderStore::saveUser(net::ProviderConfig provider) {
    auto existing = find(provider.key);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (*existing && (*existing)->origin == "builtin") {
        return storageError(
            StorageErrorCode::InvalidValue,
            std::format("\"{}\" is a built-in provider; choose another key", provider.key));
    }
    provider.origin = "user";
    if (auto valid = net::parseProvider(net::toJson(provider)); !valid) {
        return storageError(StorageErrorCode::InvalidValue, valid.error().message);
    }
    return upsert(provider);
}

StorageResult<bool> ProviderStore::removeUser(std::string_view key) {
    auto q = connection_->prepare(
        "DELETE FROM data_providers WHERE provider_key = :key AND origin = 'user'");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    return q->numRowsAffected() > 0;
}

StorageResult<void> ProviderStore::recordCall(const net::ProviderCall& call) {
    auto id = idOf(call.providerKey);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (!*id) {
        return storageError(StorageErrorCode::InvalidValue,
                            std::format("unknown provider \"{}\"", call.providerKey));
    }
    auto q = connection_->prepare(
        "INSERT INTO provider_calls (provider_id, called_at, dataset, west, south, east, north, "
        "http_status, bytes) VALUES (:id, :at, :dataset, :w, :s, :e, :n, :status, :bytes)");
    if (!q) {
        return std::unexpected(q.error());
    }
    const auto coord = [&](double net::GeoRect::* m) {
        return call.area ? optionalReal((*call.area).*m) : optionalReal(std::nullopt);
    };
    q->bindValue(QStringLiteral(":id"), static_cast<qlonglong>(**id));
    q->bindValue(QStringLiteral(":at"), toQString(unixToIsoUtc(call.calledAtUnixS)));
    q->bindValue(QStringLiteral(":dataset"), toQString(call.dataset));
    q->bindValue(QStringLiteral(":w"), coord(&net::GeoRect::west));
    q->bindValue(QStringLiteral(":s"), coord(&net::GeoRect::south));
    q->bindValue(QStringLiteral(":e"), coord(&net::GeoRect::east));
    q->bindValue(QStringLiteral(":n"), coord(&net::GeoRect::north));
    q->bindValue(QStringLiteral(":status"), static_cast<qlonglong>(call.httpStatus));
    q->bindValue(QStringLiteral(":bytes"), static_cast<qulonglong>(call.bytes));
    return execute(*q);
}

StorageResult<std::vector<std::int64_t>> ProviderStore::callTimesSince(std::string_view key,
                                                                       std::int64_t sinceUnixS) {
    auto q = connection_->prepare(
        "SELECT c.called_at FROM provider_calls c JOIN data_providers p ON p.id = c.provider_id "
        "WHERE p.provider_key = :key AND c.called_at >= :since ORDER BY c.called_at");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":key"), toQString(key));
    q->bindValue(QStringLiteral(":since"), toQString(unixToIsoUtc(sinceUnixS)));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<std::int64_t> out;
    while (q->next()) {
        if (auto t = isoUtcToUnix(text(*q, 0))) {
            out.push_back(*t);
        }
    }
    return out;
}

StorageResult<ProviderUsage> ProviderStore::usageSince(std::string_view key,
                                                       std::int64_t sinceUnixS) {
    auto q = connection_->prepare(
        "SELECT COUNT(*), COALESCE(SUM(c.http_status = 0 OR c.http_status >= 400), 0), "
        "COALESCE(SUM(c.bytes), 0) FROM provider_calls c "
        "JOIN data_providers p ON p.id = c.provider_id "
        "WHERE p.provider_key = :key AND c.called_at >= :since");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":key"), toQString(key));
    q->bindValue(QStringLiteral(":since"), toQString(unixToIsoUtc(sinceUnixS)));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    if (!q->next()) {
        return ProviderUsage{};
    }
    return ProviderUsage{q->value(0).toInt(), q->value(1).toInt(),
                         static_cast<std::uint64_t>(q->value(2).toLongLong())};
}

StorageResult<int> ProviderStore::pruneCalls(std::int64_t beforeUnixS) {
    auto q = connection_->prepare("DELETE FROM provider_calls WHERE called_at < :before");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":before"), toQString(unixToIsoUtc(beforeUnixS)));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    return q->numRowsAffected();
}

StorageResult<void> ProviderStore::setCredential(std::string_view key, std::string_view apiKey) {
    auto id = idOf(key);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (!*id) {
        return storageError(StorageErrorCode::InvalidValue,
                            std::format("unknown provider \"{}\"", key));
    }
    auto q = connection_->prepare(
        "INSERT INTO provider_credentials (provider_id, api_key) VALUES (:id, :apiKey) "
        "ON CONFLICT (provider_id) DO UPDATE SET api_key = excluded.api_key");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":id"), static_cast<qlonglong>(**id));
    q->bindValue(QStringLiteral(":apiKey"), toQString(apiKey));
    return execute(*q);
}

StorageResult<std::optional<std::string>> ProviderStore::credential(std::string_view key) {
    auto q = connection_->prepare(
        "SELECT c.api_key FROM provider_credentials c JOIN data_providers p "
        "ON p.id = c.provider_id WHERE p.provider_key = :key");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    if (!q->next()) {
        return std::optional<std::string>{};
    }
    return std::optional{text(*q, 0)};
}

StorageResult<void> ProviderStore::clearCredential(std::string_view key) {
    auto q = connection_->prepare(
        "DELETE FROM provider_credentials WHERE provider_id = "
        "(SELECT id FROM data_providers WHERE provider_key = :key)");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":key"), toQString(key));
    return execute(*q);
}

// --- CacheIndexStore ----------------------------------------------------------------------------

namespace {

constexpr const char* kTileColumns =
    "provider, key, path, size, content_type, etag, last_modified, content_hash, stored_at, "
    "last_access, expires_at, pinned";

net::CacheEntry readTile(const QSqlQuery& q) {
    net::CacheEntry e;
    e.provider = text(q, 0);
    e.key = text(q, 1);
    e.relativePath = text(q, 2);
    e.sizeBytes = static_cast<std::uint64_t>(q.value(3).toLongLong());
    e.contentType = text(q, 4);
    e.etag = text(q, 5);
    e.lastModified = text(q, 6);
    e.contentHash = text(q, 7);
    e.storedAtUnixS = q.value(8).toLongLong();
    e.lastAccessUnixS = q.value(9).toLongLong();
    if (!q.value(10).isNull()) {
        e.expiresAtUnixS = q.value(10).toLongLong();
    }
    e.pinned = q.value(11).toInt() != 0;
    return e;
}

}  // namespace

StorageResult<std::optional<net::CacheEntry>> CacheIndexStore::find(std::string_view provider,
                                                                    std::string_view key) {
    auto q = connection_->prepare(std::string("SELECT ") + kTileColumns +
                                  " FROM tiles WHERE provider = :provider AND key = :key");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":provider"), toQString(provider));
    q->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    if (!q->next()) {
        return std::optional<net::CacheEntry>{};
    }
    return std::optional{readTile(*q)};
}

StorageResult<void> CacheIndexStore::put(const net::CacheEntry& e) {
    auto q = connection_->prepare(
        "INSERT OR REPLACE INTO tiles (provider, key, path, size, content_type, etag, "
        "last_modified, content_hash, stored_at, last_access, expires_at, pinned) VALUES "
        "(:provider, :key, :path, :size, :type, :etag, :modified, :hash, :stored, :access, "
        ":expires, :pinned)");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":provider"), toQString(e.provider));
    q->bindValue(QStringLiteral(":key"), toQString(e.key));
    q->bindValue(QStringLiteral(":path"), toQString(e.relativePath));
    q->bindValue(QStringLiteral(":size"), static_cast<qulonglong>(e.sizeBytes));
    q->bindValue(QStringLiteral(":type"), toQString(e.contentType));
    q->bindValue(QStringLiteral(":etag"), toQString(e.etag));
    q->bindValue(QStringLiteral(":modified"), toQString(e.lastModified));
    q->bindValue(QStringLiteral(":hash"), toQString(e.contentHash));
    q->bindValue(QStringLiteral(":stored"), static_cast<qlonglong>(e.storedAtUnixS));
    q->bindValue(QStringLiteral(":access"), static_cast<qlonglong>(e.lastAccessUnixS));
    q->bindValue(QStringLiteral(":expires"), optionalInt64(e.expiresAtUnixS));
    q->bindValue(QStringLiteral(":pinned"), e.pinned ? 1 : 0);
    return execute(*q);
}

StorageResult<void> CacheIndexStore::touch(std::string_view provider, std::string_view key,
                                           std::int64_t unixS) {
    auto q = connection_->prepare(
        "UPDATE tiles SET last_access = :t WHERE provider = :provider AND key = :key");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":t"), static_cast<qlonglong>(unixS));
    q->bindValue(QStringLiteral(":provider"), toQString(provider));
    q->bindValue(QStringLiteral(":key"), toQString(key));
    return execute(*q);
}

StorageResult<bool> CacheIndexStore::setPinned(std::string_view provider, std::string_view key,
                                               bool pinned) {
    auto q = connection_->prepare(
        "UPDATE tiles SET pinned = :pinned WHERE provider = :provider AND key = :key");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":pinned"), pinned ? 1 : 0);
    q->bindValue(QStringLiteral(":provider"), toQString(provider));
    q->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    return q->numRowsAffected() > 0;
}

StorageResult<void> CacheIndexStore::remove(std::string_view provider, std::string_view key) {
    auto q = connection_->prepare("DELETE FROM tiles WHERE provider = :provider AND key = :key");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":provider"), toQString(provider));
    q->bindValue(QStringLiteral(":key"), toQString(key));
    return execute(*q);
}

StorageResult<std::vector<net::CacheEntry>> CacheIndexStore::list(std::string_view provider) {
    auto q = connection_->prepare(
        std::string("SELECT ") + kTileColumns +
        " FROM tiles WHERE :all = 1 OR provider = :provider ORDER BY last_access, rowid");
    if (!q) {
        return std::unexpected(q.error());
    }
    q->bindValue(QStringLiteral(":all"), provider.empty() ? 1 : 0);
    q->bindValue(QStringLiteral(":provider"), toQString(provider));
    if (auto ok = execute(*q); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<net::CacheEntry> out;
    while (q->next()) {
        out.push_back(readTile(*q));
    }
    return out;
}

// --- Adapters -----------------------------------------------------------------------------------

common::Result<std::vector<std::int64_t>> StorageUsageLedger::callTimesSince(
    std::string_view providerKey, std::int64_t sinceUnixS) {
    return toCommon(runOn(*service_, [key = std::string(providerKey), sinceUnixS](DbConnection& c) {
        return ProviderStore(c).callTimesSince(key, sinceUnixS);
    }));
}

common::Result<void> StorageUsageLedger::record(const net::ProviderCall& call) {
    return toCommon(
        runOn(*service_, [call](DbConnection& c) { return ProviderStore(c).recordCall(call); }));
}

common::Result<std::optional<net::CacheEntry>> StorageCacheIndex::find(std::string_view provider,
                                                                       std::string_view key) {
    return toCommon(
        runOn(*service_, [p = std::string(provider), k = std::string(key)](DbConnection& c) {
            return CacheIndexStore(c).find(p, k);
        }));
}

common::Result<void> StorageCacheIndex::put(const net::CacheEntry& entry) {
    return toCommon(
        runOn(*service_, [entry](DbConnection& c) { return CacheIndexStore(c).put(entry); }));
}

common::Result<void> StorageCacheIndex::touch(std::string_view provider, std::string_view key,
                                              std::int64_t unixS) {
    return toCommon(
        runOn(*service_, [p = std::string(provider), k = std::string(key), unixS](DbConnection& c) {
            return CacheIndexStore(c).touch(p, k, unixS);
        }));
}

common::Result<bool> StorageCacheIndex::setPinned(std::string_view provider, std::string_view key,
                                                  bool pinned) {
    return toCommon(runOn(
        *service_, [p = std::string(provider), k = std::string(key), pinned](DbConnection& c) {
            return CacheIndexStore(c).setPinned(p, k, pinned);
        }));
}

common::Result<void> StorageCacheIndex::remove(std::string_view provider, std::string_view key) {
    return toCommon(
        runOn(*service_, [p = std::string(provider), k = std::string(key)](DbConnection& c) {
            return CacheIndexStore(c).remove(p, k);
        }));
}

common::Result<std::vector<net::CacheEntry>> StorageCacheIndex::list(std::string_view provider) {
    return toCommon(runOn(*service_, [p = std::string(provider)](DbConnection& c) {
        return CacheIndexStore(c).list(p);
    }));
}

}  // namespace ds::storage
