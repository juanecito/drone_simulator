// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/common/json.hpp"

/// @file
/// @brief Validator for the subset of JSON Schema (draft 2020-12) used by the project's data
/// schemas, e.g. `assets/missions/mission.schema.json` (§4.9.1).
/// @details Supported keywords: `$schema`, `$id`, `$defs`, `$ref` (local `#/…` pointers only),
/// `$comment`, `title`, `description`, `default`, `examples`, `deprecated`, `readOnly`,
/// `writeOnly`, `format` (annotation only), `type`, `enum`, `const`, `properties`, `required`,
/// `additionalProperties`, `patternProperties`, `propertyNames`, `minProperties`,
/// `maxProperties`, `dependentRequired`, `items`, `prefixItems`, `minItems`, `maxItems`,
/// `uniqueItems`, `contains`, `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`,
/// `multipleOf`, `minLength`, `maxLength`, `pattern` (ECMAScript regular expressions),
/// `allOf`, `anyOf`, `oneOf`, `not`, `if` / `then` / `else`.
/// Any other keyword makes the schema itself invalid, so a schema never silently means more
/// than the validator checks.
/// @ingroup common

namespace ds::common {

/// @brief One validation failure.
struct SchemaIssue {
    std::string pointer;  ///< JSON pointer of the offending value ("" for the root).
    std::string message;  ///< What is wrong, e.g. `unknown property "foo"`.
};

/// @brief "objectives[1].target" form of a JSON pointer ("/objectives/1/target"); "(root)" for
/// the document itself.
[[nodiscard]] std::string pointerToDotted(std::string_view pointer);

/// @brief A compiled schema. Immutable and safe to share between threads.
class JsonSchema {
public:
    /// @brief Checks @p schema (keywords, `$ref` targets, regular expressions) and compiles it.
    [[nodiscard]] static Result<JsonSchema> compile(Json schema);
    /// @brief Reads and compiles the schema file @p file.
    [[nodiscard]] static Result<JsonSchema> load(const std::filesystem::path& file);

    /// @brief Validates @p instance; returns all issues found (empty ⇒ valid), at most
    /// @p maxIssues of them.
    [[nodiscard]] std::vector<SchemaIssue> validate(const Json& instance,
                                                    std::size_t maxIssues = 100) const;

    /// @brief The schema document.
    [[nodiscard]] const Json& document() const noexcept;

    /// @brief Copy.
    JsonSchema(const JsonSchema&) = default;
    /// @brief Copy assignment.
    JsonSchema& operator=(const JsonSchema&) = default;
    /// @brief Move.
    JsonSchema(JsonSchema&&) noexcept = default;
    /// @brief Move assignment.
    JsonSchema& operator=(JsonSchema&&) noexcept = default;
    ~JsonSchema();

private:
    struct Impl;
    explicit JsonSchema(std::shared_ptr<const Impl> impl) noexcept;
    std::shared_ptr<const Impl> impl_;
};

}  // namespace ds::common
