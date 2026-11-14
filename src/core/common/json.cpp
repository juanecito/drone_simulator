// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/common/json.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>

namespace ds::common {
namespace {

const Json& nullJson() {
    static const Json kNull;
    return kNull;
}

// JSON pointer escaping (RFC 6901).
std::string escapePointer(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (const char c : key) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

std::string rangeText(double min, double max) {
    return std::format("must be a number between {} and {}", min, max);
}

}  // namespace

Result<Json> parseJson(std::string_view text) {
    try {
        return Json::parse(text.begin(), text.end());
    } catch (const Json::parse_error& e) {
        return makeError(std::format("invalid JSON: {}", e.what()));
    }
}

Result<Json> readJsonFile(const std::filesystem::path& file, std::size_t maxBytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        return makeError(std::format("cannot read {}: {}", file.string(), ec.message()));
    }
    if (size > maxBytes) {
        return makeError(
            std::format("{} is too large ({} bytes, limit {})", file.string(), size, maxBytes));
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return makeError(std::format("cannot open {}", file.string()));
    }
    // Read at most the checked size (the file may grow meanwhile; it is untrusted input).
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    auto parsed = parseJson(text);
    if (!parsed) {
        return makeError(std::format("{}: {}", file.string(), parsed.error().message));
    }
    return parsed;
}

JsonReader::JsonReader(const Json& value, std::string path)
    : JsonReader(&value, std::move(path), std::make_shared<std::vector<std::string>>()) {}

JsonReader::JsonReader(const Json* value, std::string path,
                       std::shared_ptr<std::vector<std::string>> errors)
    : value_(value), path_(std::move(path)), errors_(std::move(errors)) {}

const Json* JsonReader::member(std::string_view key) const {
    if (!value_->is_object()) {
        return nullptr;
    }
    const auto it = value_->find(key);
    return it == value_->end() ? nullptr : &*it;
}

std::string JsonReader::childPath(std::string_view key) const {
    return key.empty() ? path_ : path_ + "/" + escapePointer(key);
}

bool JsonReader::has(std::string_view key) const { return member(key) != nullptr; }

void JsonReader::fail(std::string_view key, std::string_view message) {
    const std::string p = childPath(key);
    errors_->push_back(std::format("{}: {}", p.empty() ? "/" : p, message));
}

std::string JsonReader::errorText() const {
    std::string text;
    for (const auto& e : *errors_) {
        if (!text.empty()) {
            text += '\n';
        }
        text += e;
    }
    return text;
}

double JsonReader::number(std::string_view key, double min, double max) {
    const Json* m = member(key);
    if (m == nullptr) {
        fail(key, "is required");
        return min;
    }
    if (!m->is_number() || !std::isfinite(m->get<double>()) || m->get<double>() < min ||
        m->get<double>() > max) {
        fail(key, rangeText(min, max));
        return m->is_number() && std::isfinite(m->get<double>())
                   ? std::clamp(m->get<double>(), min, max)
                   : min;
    }
    return m->get<double>();
}

double JsonReader::number(std::string_view key, double min, double max, double fallback) {
    return has(key) ? number(key, min, max) : fallback;
}

int JsonReader::integer(std::string_view key, int min, int max) {
    const Json* m = member(key);
    if (m == nullptr) {
        fail(key, "is required");
        return min;
    }
    if (!m->is_number_integer() || m->get<long long>() < min || m->get<long long>() > max) {
        fail(key, std::format("must be an integer between {} and {}", min, max));
        return min;
    }
    return static_cast<int>(m->get<long long>());
}

int JsonReader::integer(std::string_view key, int min, int max, int fallback) {
    return has(key) ? integer(key, min, max) : fallback;
}

bool JsonReader::boolean(std::string_view key, bool fallback) {
    const Json* m = member(key);
    if (m == nullptr) {
        return fallback;
    }
    if (!m->is_boolean()) {
        fail(key, "must be true or false");
        return fallback;
    }
    return m->get<bool>();
}

std::string JsonReader::string(std::string_view key) {
    const Json* m = member(key);
    if (m == nullptr) {
        fail(key, "is required");
        return {};
    }
    if (!m->is_string() || m->get_ref<const std::string&>().empty()) {
        fail(key, "must be a non-empty string");
        return {};
    }
    return m->get<std::string>();
}

std::string JsonReader::string(std::string_view key, std::string_view fallback) {
    const Json* m = member(key);
    if (m == nullptr) {
        return std::string(fallback);
    }
    if (!m->is_string()) {
        fail(key, "must be a string");
        return std::string(fallback);
    }
    return m->get<std::string>();
}

std::string JsonReader::choice(std::string_view key, const std::vector<std::string_view>& allowed) {
    const Json* m = member(key);
    if (m == nullptr) {
        fail(key, "is required");
        return allowed.empty() ? std::string{} : std::string(allowed.front());
    }
    if (m->is_string()) {
        const auto& text = m->get_ref<const std::string&>();
        if (std::ranges::find(allowed, std::string_view(text)) != allowed.end()) {
            return text;
        }
    }
    std::string list;
    for (const auto a : allowed) {
        list += list.empty() ? "" : ", ";
        list += a;
    }
    fail(key, std::format("must be one of: {}", list));
    return allowed.empty() ? std::string{} : std::string(allowed.front());
}

JsonReader JsonReader::object(std::string_view key) {
    const Json* m = member(key);
    if (m == nullptr) {
        fail(key, "is required");
        return {&nullJson(), childPath(key), errors_};
    }
    if (!m->is_object()) {
        fail(key, "must be an object");
        return {&nullJson(), childPath(key), errors_};
    }
    return {m, childPath(key), errors_};
}

std::optional<JsonReader> JsonReader::optionalObject(std::string_view key) {
    if (!has(key)) {
        return std::nullopt;
    }
    return object(key);
}

std::size_t JsonReader::array(std::string_view key) {
    const Json* m = member(key);
    if (m == nullptr) {
        fail(key, "is required");
        return 0;
    }
    if (!m->is_array()) {
        fail(key, "must be an array");
        return 0;
    }
    return m->size();
}

std::size_t JsonReader::optionalArray(std::string_view key) { return has(key) ? array(key) : 0; }

JsonReader JsonReader::element(std::string_view key, std::size_t index) {
    const Json* m = member(key);
    const std::string p = std::format("{}/{}", childPath(key), index);
    if (m == nullptr || !m->is_array() || index >= m->size()) {
        return {&nullJson(), p, errors_};
    }
    return {&(*m)[index], p, errors_};
}

double JsonReader::asNumber(double min, double max) {
    if (!value_->is_number() || !std::isfinite(value_->get<double>()) ||
        value_->get<double>() < min || value_->get<double>() > max) {
        fail("", rangeText(min, max));
        return min;
    }
    return value_->get<double>();
}

}  // namespace ds::common
