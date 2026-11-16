// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/common/json_schema.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace ds::common {
namespace {

constexpr int kMaxDepth = 200;  // Guards against `$ref` cycles that never descend.

const std::unordered_set<std::string_view>& knownKeywords() {
    static const std::unordered_set<std::string_view> kKeywords{
        "$schema", "$id", "$defs", "$ref", "$comment", "title", "description", "default",
        "examples", "deprecated", "readOnly", "writeOnly", "format", "type", "enum", "const",
        "properties", "required", "additionalProperties", "patternProperties", "propertyNames",
        "minProperties", "maxProperties", "dependentRequired", "items", "prefixItems",
        "minItems", "maxItems", "uniqueItems", "contains", "minimum", "maximum",
        "exclusiveMinimum", "exclusiveMaximum", "multipleOf", "minLength", "maxLength",
        "pattern", "allOf", "anyOf", "oneOf", "not", "if", "then", "else"};
    return kKeywords;
}

std::string escapeToken(std::string_view key) {
    std::string out;
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

std::string unescapeToken(std::string_view token) {
    std::string out;
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '~' && i + 1 < token.size()) {
            out += token[i + 1] == '1' ? '/' : '~';
            ++i;
        } else {
            out += token[i];
        }
    }
    return out;
}

std::string shortDump(const Json& value) {
    std::string text = value.dump();
    if (text.size() > 60) {
        text.resize(57);
        text += "...";
    }
    return text;
}

std::string_view typeName(const Json& value) {
    switch (value.type()) {
        case Json::value_t::null: return "null";
        case Json::value_t::boolean: return "boolean";
        case Json::value_t::string: return "string";
        case Json::value_t::array: return "array";
        case Json::value_t::object: return "object";
        case Json::value_t::number_integer:
        case Json::value_t::number_unsigned: return "integer";
        case Json::value_t::number_float: return "number";
        default: return "unknown";
    }
}

bool isIntegral(const Json& value) {
    if (value.is_number_integer()) {
        return true;
    }
    if (value.is_number_float()) {
        const double d = value.get<double>();
        return std::isfinite(d) && std::floor(d) == d;
    }
    return false;
}

bool hasType(const Json& value, std::string_view type) {
    if (type == "integer") {
        return isIntegral(value);
    }
    if (type == "number") {
        return value.is_number();
    }
    if (type == "string") {
        return value.is_string();
    }
    if (type == "boolean") {
        return value.is_boolean();
    }
    if (type == "object") {
        return value.is_object();
    }
    if (type == "array") {
        return value.is_array();
    }
    if (type == "null") {
        return value.is_null();
    }
    return false;
}

// Length in Unicode code points (JSON Schema counts characters, not bytes).
std::size_t codePoints(const std::string& text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

}  // namespace

std::string pointerToDotted(std::string_view pointer) {
    if (pointer.empty()) {
        return "(root)";
    }
    std::string out;
    std::size_t pos = 1;
    while (pos <= pointer.size()) {
        const std::size_t next = std::min(pointer.find('/', pos), pointer.size());
        const std::string token = unescapeToken(pointer.substr(pos, next - pos));
        if (!token.empty() && std::ranges::all_of(token, [](char c) { return c >= '0' && c <= '9'; })) {
            out += std::format("[{}]", token);
        } else {
            if (!out.empty()) {
                out += '.';
            }
            out += token;
        }
        pos = next + 1;
    }
    return out;
}

struct JsonSchema::Impl {
    Json document;
    std::unordered_map<std::string, std::regex> regexes;

    // ----- compilation ---------------------------------------------------------------
    Result<void> check(const Json& schema, const std::string& where) {
        if (schema.is_boolean()) {
            return {};
        }
        if (!schema.is_object()) {
            return makeError(std::format("schema {}: must be an object or a boolean", where));
        }
        for (const auto& [key, value] : schema.items()) {
            if (!knownKeywords().contains(key)) {
                return makeError(std::format("schema {}: unsupported keyword \"{}\"", where, key));
            }
        }
        const auto sub = [&](const Json& s, std::string_view key) -> Result<void> {
            return check(s, std::format("{}/{}", where, key));
        };
        for (const char* key : {"additionalProperties", "propertyNames", "items", "contains",
                                "not", "if", "then", "else"}) {
            if (schema.contains(key)) {
                if (auto ok = sub(schema[key], key); !ok) {
                    return ok;
                }
            }
        }
        for (const char* key : {"$defs", "properties", "patternProperties"}) {
            if (!schema.contains(key)) {
                continue;
            }
            if (!schema[key].is_object()) {
                return makeError(std::format("schema {}/{}: must be an object", where, key));
            }
            for (const auto& [name, s] : schema[key].items()) {
                if (std::string_view(key) == "patternProperties") {
                    if (auto ok = addRegex(name, where); !ok) {
                        return ok;
                    }
                }
                if (auto ok = check(s, std::format("{}/{}/{}", where, key, escapeToken(name)));
                    !ok) {
                    return ok;
                }
            }
        }
        for (const char* key : {"prefixItems", "allOf", "anyOf", "oneOf"}) {
            if (!schema.contains(key)) {
                continue;
            }
            if (!schema[key].is_array() || schema[key].empty()) {
                return makeError(
                    std::format("schema {}/{}: must be a non-empty array", where, key));
            }
            for (std::size_t i = 0; i < schema[key].size(); ++i) {
                if (auto ok = check(schema[key][i], std::format("{}/{}/{}", where, key, i)); !ok) {
                    return ok;
                }
            }
        }
        if (schema.contains("pattern")) {
            if (!schema["pattern"].is_string()) {
                return makeError(std::format("schema {}/pattern: must be a string", where));
            }
            if (auto ok = addRegex(schema["pattern"].get<std::string>(), where); !ok) {
                return ok;
            }
        }
        if (schema.contains("$ref")) {
            const Json& ref = schema["$ref"];
            if (!ref.is_string() || !ref.get<std::string>().starts_with('#')) {
                return makeError(
                    std::format("schema {}/$ref: only local references (\"#/...\") are supported",
                                where));
            }
            if (resolve(ref.get<std::string>()) == nullptr) {
                return makeError(std::format("schema {}/$ref: \"{}\" does not exist", where,
                                             ref.get<std::string>()));
            }
        }
        if (schema.contains("type")) {
            const Json& t = schema["type"];
            const auto valid = [](const Json& v) {
                static constexpr std::array<std::string_view, 7> kTypes{
                    "null", "boolean", "object", "array", "number", "string", "integer"};
                return v.is_string() &&
                       std::ranges::find(kTypes, v.get<std::string>()) != kTypes.end();
            };
            if (!(valid(t) || (t.is_array() && !t.empty() && std::ranges::all_of(t, valid)))) {
                return makeError(std::format("schema {}/type: invalid type", where));
            }
        }
        if (schema.contains("required") && !schema["required"].is_array()) {
            return makeError(std::format("schema {}/required: must be an array", where));
        }
        if (schema.contains("enum") && !schema["enum"].is_array()) {
            return makeError(std::format("schema {}/enum: must be an array", where));
        }
        return {};
    }

    Result<void> addRegex(const std::string& pattern, const std::string& where) {
        if (regexes.contains(pattern)) {
            return {};
        }
        try {
            regexes.emplace(pattern, std::regex(pattern, std::regex::ECMAScript));
        } catch (const std::regex_error& e) {
            return makeError(
                std::format("schema {}: invalid pattern \"{}\": {}", where, pattern, e.what()));
        }
        return {};
    }

    [[nodiscard]] const Json* resolve(const std::string& ref) const {
        if (ref == "#") {
            return &document;
        }
        if (!ref.starts_with("#/")) {
            return nullptr;
        }
        try {
            const Json::json_pointer pointer(ref.substr(1));
            if (!document.contains(pointer)) {
                return nullptr;
            }
            return &document.at(pointer);
        } catch (const Json::exception&) {
            return nullptr;
        }
    }

    [[nodiscard]] bool matches(const std::string& pattern, const std::string& text) const {
        const auto it = regexes.find(pattern);
        return it != regexes.end() && std::regex_search(text, it->second);
    }

    // ----- validation ----------------------------------------------------------------
    struct Context {
        std::vector<SchemaIssue>* issues;
        std::size_t maxIssues;
    };

    void add(Context& ctx, const std::string& pointer, std::string message) const {
        if (ctx.issues->size() < ctx.maxIssues) {
            ctx.issues->push_back(SchemaIssue{pointer, std::move(message)});
        }
    }

    [[nodiscard]] std::vector<SchemaIssue> trial(const Json& schema, const Json& instance,
                                                 const std::string& pointer, int depth) const {
        std::vector<SchemaIssue> issues;
        Context ctx{&issues, 20};
        run(schema, instance, pointer, ctx, depth);
        return issues;
    }

    void run(const Json& schema, const Json& instance, const std::string& pointer, Context& ctx,
             int depth) const {
        if (depth > kMaxDepth) {
            add(ctx, pointer, "schema nesting too deep");
            return;
        }
        if (schema.is_boolean()) {
            if (!schema.get<bool>()) {
                add(ctx, pointer, "is not allowed here");
            }
            return;
        }
        if (schema.contains("$ref")) {
            if (const Json* target = resolve(schema["$ref"].get<std::string>())) {
                run(*target, instance, pointer, ctx, depth + 1);
            }
        }
        if (schema.contains("type") && !checkType(schema["type"], instance, pointer, ctx)) {
            return;  // Other keywords would only repeat the type error.
        }
        if (schema.contains("const") && instance != schema["const"]) {
            add(ctx, pointer, std::format("must be {}", shortDump(schema["const"])));
        }
        if (schema.contains("enum")) {
            const Json& values = schema["enum"];
            if (std::ranges::find(values, instance) == values.end()) {
                std::string list;
                for (const Json& v : values) {
                    list += list.empty() ? "" : ", ";
                    list += shortDump(v);
                }
                add(ctx, pointer, std::format("must be one of {} (got {})", list,
                                              shortDump(instance)));
            }
        }
        if (instance.is_number()) {
            checkNumber(schema, instance.get<double>(), pointer, ctx);
        } else if (instance.is_string()) {
            checkString(schema, instance.get<std::string>(), pointer, ctx);
        } else if (instance.is_array()) {
            checkArray(schema, instance, pointer, ctx, depth);
        } else if (instance.is_object()) {
            checkObject(schema, instance, pointer, ctx, depth);
        }
        checkCombinators(schema, instance, pointer, ctx, depth);
    }

    bool checkType(const Json& type, const Json& instance, const std::string& pointer,
                   Context& ctx) const {
        if (type.is_string()) {
            if (!hasType(instance, type.get<std::string>())) {
                add(ctx, pointer, std::format("must be of type {} (got {})",
                                              type.get<std::string>(), typeName(instance)));
                return false;
            }
            return true;
        }
        std::string list;
        for (const Json& t : type) {
            if (hasType(instance, t.get<std::string>())) {
                return true;
            }
            list += list.empty() ? "" : " or ";
            list += t.get<std::string>();
        }
        add(ctx, pointer, std::format("must be of type {} (got {})", list, typeName(instance)));
        return false;
    }

    void checkNumber(const Json& s, double v, const std::string& pointer, Context& ctx) const {
        if (s.contains("minimum") && v < s["minimum"].get<double>()) {
            add(ctx, pointer, std::format("must be ≥ {} (got {})", s["minimum"].dump(), v));
        }
        if (s.contains("maximum") && v > s["maximum"].get<double>()) {
            add(ctx, pointer, std::format("must be ≤ {} (got {})", s["maximum"].dump(), v));
        }
        if (s.contains("exclusiveMinimum") && v <= s["exclusiveMinimum"].get<double>()) {
            add(ctx, pointer,
                std::format("must be > {} (got {})", s["exclusiveMinimum"].dump(), v));
        }
        if (s.contains("exclusiveMaximum") && v >= s["exclusiveMaximum"].get<double>()) {
            add(ctx, pointer,
                std::format("must be < {} (got {})", s["exclusiveMaximum"].dump(), v));
        }
        if (s.contains("multipleOf")) {
            const double m = s["multipleOf"].get<double>();
            const double q = v / m;
            if (m > 0.0 && std::abs(q - std::round(q)) > 1e-9 * std::max(1.0, std::abs(q))) {
                add(ctx, pointer, std::format("must be a multiple of {}", s["multipleOf"].dump()));
            }
        }
    }

    void checkString(const Json& s, const std::string& v, const std::string& pointer,
                     Context& ctx) const {
        const std::size_t n = codePoints(v);
        if (s.contains("minLength") && n < s["minLength"].get<std::size_t>()) {
            add(ctx, pointer, n == 0 ? std::string("must not be empty")
                                     : std::format("must have at least {} characters",
                                                   s["minLength"].get<std::size_t>()));
        }
        if (s.contains("maxLength") && n > s["maxLength"].get<std::size_t>()) {
            add(ctx, pointer, std::format("must have at most {} characters",
                                          s["maxLength"].get<std::size_t>()));
        }
        if (s.contains("pattern") && !matches(s["pattern"].get<std::string>(), v)) {
            add(ctx, pointer, std::format("\"{}\" does not match the pattern {}", v,
                                          s["pattern"].get<std::string>()));
        }
    }

    void checkArray(const Json& s, const Json& v, const std::string& pointer, Context& ctx,
                    int depth) const {
        if (s.contains("minItems") && v.size() < s["minItems"].get<std::size_t>()) {
            add(ctx, pointer, std::format("must have at least {} elements",
                                          s["minItems"].get<std::size_t>()));
        }
        if (s.contains("maxItems") && v.size() > s["maxItems"].get<std::size_t>()) {
            add(ctx, pointer, std::format("must have at most {} elements",
                                          s["maxItems"].get<std::size_t>()));
        }
        std::size_t first = 0;
        if (s.contains("prefixItems")) {
            const Json& prefix = s["prefixItems"];
            for (; first < prefix.size() && first < v.size(); ++first) {
                run(prefix[first], v[first], std::format("{}/{}", pointer, first), ctx, depth + 1);
            }
        }
        if (s.contains("items")) {
            for (std::size_t i = first; i < v.size(); ++i) {
                run(s["items"], v[i], std::format("{}/{}", pointer, i), ctx, depth + 1);
            }
        }
        if (s.contains("uniqueItems") && s["uniqueItems"].get<bool>()) {
            for (std::size_t i = 0; i < v.size(); ++i) {
                for (std::size_t j = i + 1; j < v.size(); ++j) {
                    if (v[i] == v[j]) {
                        add(ctx, std::format("{}/{}", pointer, j),
                            std::format("duplicate of element {}", i));
                    }
                }
            }
        }
        if (s.contains("contains")) {
            const bool any = std::ranges::any_of(v, [&](const Json& e) {
                return trial(s["contains"], e, pointer, depth + 1).empty();
            });
            if (!any) {
                add(ctx, pointer, "contains no matching element");
            }
        }
    }

    void checkObject(const Json& s, const Json& v, const std::string& pointer, Context& ctx,
                     int depth) const {
        if (s.contains("required")) {
            for (const Json& name : s["required"]) {
                if (!v.contains(name.get<std::string>())) {
                    add(ctx, pointer,
                        std::format("missing required property \"{}\"", name.get<std::string>()));
                }
            }
        }
        if (s.contains("minProperties") && v.size() < s["minProperties"].get<std::size_t>()) {
            add(ctx, pointer, std::format("must have at least {} properties",
                                          s["minProperties"].get<std::size_t>()));
        }
        if (s.contains("maxProperties") && v.size() > s["maxProperties"].get<std::size_t>()) {
            add(ctx, pointer, std::format("must have at most {} properties",
                                          s["maxProperties"].get<std::size_t>()));
        }
        if (s.contains("dependentRequired")) {
            for (const auto& [name, deps] : s["dependentRequired"].items()) {
                if (!v.contains(name)) {
                    continue;
                }
                for (const Json& dep : deps) {
                    if (!v.contains(dep.get<std::string>())) {
                        add(ctx, pointer, std::format("\"{}\" requires \"{}\"", name,
                                                      dep.get<std::string>()));
                    }
                }
            }
        }
        const Json* props = s.contains("properties") ? &s["properties"] : nullptr;
        const Json* patterns = s.contains("patternProperties") ? &s["patternProperties"] : nullptr;
        for (const auto& [name, value] : v.items()) {
            const std::string child = std::format("{}/{}", pointer, escapeToken(name));
            if (s.contains("propertyNames")) {
                const auto issues = trial(s["propertyNames"], Json(name), child, depth + 1);
                if (!issues.empty()) {
                    add(ctx, child, std::format("invalid property name \"{}\"", name));
                }
            }
            bool evaluated = false;
            if (props != nullptr && props->contains(name)) {
                evaluated = true;
                run((*props)[name], value, child, ctx, depth + 1);
            }
            if (patterns != nullptr) {
                for (const auto& [pattern, sub] : patterns->items()) {
                    if (matches(pattern, name)) {
                        evaluated = true;
                        run(sub, value, child, ctx, depth + 1);
                    }
                }
            }
            if (!evaluated && s.contains("additionalProperties")) {
                const Json& extra = s["additionalProperties"];
                if (extra.is_boolean() && !extra.get<bool>()) {
                    add(ctx, child, std::format("unknown property \"{}\"", name));
                } else {
                    run(extra, value, child, ctx, depth + 1);
                }
            }
        }
    }

    void checkCombinators(const Json& s, const Json& v, const std::string& pointer, Context& ctx,
                          int depth) const {
        if (s.contains("allOf")) {
            for (const Json& sub : s["allOf"]) {
                run(sub, v, pointer, ctx, depth + 1);
            }
        }
        if (s.contains("anyOf") || s.contains("oneOf")) {
            const bool one = s.contains("oneOf");
            const Json& list = one ? s["oneOf"] : s["anyOf"];
            std::size_t passed = 0;
            std::vector<SchemaIssue> closest;
            std::size_t closestRank = 0;
            for (const Json& sub : list) {
                auto issues = trial(sub, v, pointer, depth + 1);
                if (issues.empty()) {
                    ++passed;
                    continue;
                }
                // An alternative of the wrong type is the least helpful explanation.
                const bool wrongType = std::ranges::any_of(issues, [&](const SchemaIssue& i) {
                    return i.pointer == pointer && i.message.starts_with("must be of type");
                });
                const std::size_t rank = issues.size() + (wrongType ? 1000U : 0U);
                if (closest.empty() || rank < closestRank) {
                    closest = std::move(issues);
                    closestRank = rank;
                }
            }
            if (passed == 0) {
                // Report the alternative that came closest: its messages are the most useful.
                for (auto& issue : closest) {
                    add(ctx, issue.pointer, std::move(issue.message));
                }
                if (closest.empty()) {
                    add(ctx, pointer, "does not match any allowed form");
                }
            } else if (one && passed > 1) {
                add(ctx, pointer, "matches more than one allowed form");
            }
        }
        if (s.contains("not") && trial(s["not"], v, pointer, depth + 1).empty()) {
            add(ctx, pointer, "is not allowed here");
        }
        if (s.contains("if")) {
            const bool cond = trial(s["if"], v, pointer, depth + 1).empty();
            if (cond && s.contains("then")) {
                run(s["then"], v, pointer, ctx, depth + 1);
            } else if (!cond && s.contains("else")) {
                run(s["else"], v, pointer, ctx, depth + 1);
            }
        }
    }
};

JsonSchema::JsonSchema(std::shared_ptr<const Impl> impl) noexcept : impl_(std::move(impl)) {}
JsonSchema::~JsonSchema() = default;

Result<JsonSchema> JsonSchema::compile(Json schema) {
    auto impl = std::make_shared<Impl>();
    impl->document = std::move(schema);
    if (auto ok = impl->check(impl->document, "#"); !ok) {
        return std::unexpected(ok.error());
    }
    // Every $defs entry is checked by check(); every reachable $ref target was resolved.
    return JsonSchema(std::move(impl));
}

Result<JsonSchema> JsonSchema::load(const std::filesystem::path& file) {
    auto json = readJsonFile(file);
    if (!json) {
        return std::unexpected(json.error());
    }
    auto schema = compile(std::move(*json));
    if (!schema) {
        return makeError(std::format("{}: {}", file.string(), schema.error().message));
    }
    return schema;
}

std::vector<SchemaIssue> JsonSchema::validate(const Json& instance, std::size_t maxIssues) const {
    std::vector<SchemaIssue> issues;
    Impl::Context ctx{&issues, maxIssues};
    impl_->run(impl_->document, instance, "", ctx, 0);
    return issues;
}

const Json& JsonSchema::document() const noexcept { return impl_->document; }

}  // namespace ds::common
