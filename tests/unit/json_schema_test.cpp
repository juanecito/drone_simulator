// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/common/json_schema.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using ds::common::Json;
using ds::common::JsonSchema;

JsonSchema compile(const char* text) {
    auto schema = JsonSchema::compile(Json::parse(text));
    EXPECT_TRUE(schema.has_value()) << (schema ? "" : schema.error().message);
    return std::move(*schema);
}

std::string firstIssue(const JsonSchema& schema, const char* instance) {
    const auto issues = schema.validate(Json::parse(instance));
    return issues.empty() ? std::string() : issues.front().pointer + " " + issues.front().message;
}

TEST(JsonSchema, PointerToDotted) {
    EXPECT_EQ(ds::common::pointerToDotted(""), "(root)");
    EXPECT_EQ(ds::common::pointerToDotted("/objectives/1/target"), "objectives[1].target");
    EXPECT_EQ(ds::common::pointerToDotted("/a~1b/0"), "a/b[0]");
}

TEST(JsonSchema, RejectsUnsupportedKeywordsAndBadReferences) {
    EXPECT_FALSE(JsonSchema::compile(Json::parse(R"({"unevaluatedProperties": false})")));
    EXPECT_FALSE(JsonSchema::compile(Json::parse(R"({"$ref": "#/$defs/missing"})")));
    EXPECT_FALSE(JsonSchema::compile(Json::parse(R"({"$ref": "other.json"})")));
    EXPECT_FALSE(JsonSchema::compile(Json::parse(R"({"pattern": "("})")));
    EXPECT_FALSE(JsonSchema::compile(Json::parse(R"({"type": "float"})")));
}

TEST(JsonSchema, TypesNumbersAndStrings) {
    const auto s = compile(R"({"type": "object", "properties": {
        "n": {"type": "integer", "minimum": 1, "maximum": 5},
        "x": {"type": "number", "exclusiveMinimum": 0, "multipleOf": 0.5},
        "s": {"type": "string", "minLength": 2, "pattern": "^[a-z]+$"},
        "e": {"enum": ["a", "b"]},
        "c": {"const": 3},
        "o": {"type": ["string", "null"]}}})");
    EXPECT_TRUE(s.validate(Json::parse(R"({"n": 3, "x": 1.5, "s": "ab", "e": "a", "c": 3,
                                          "o": null})")).empty());
    EXPECT_EQ(firstIssue(s, R"({"n": 2.5})"), "/n must be of type integer (got number)");
    EXPECT_TRUE(s.validate(Json::parse(R"({"n": 2.0})")).empty());  // Integral float.
    EXPECT_EQ(firstIssue(s, R"({"n": 9})"), "/n must be ≤ 5 (got 9)");
    EXPECT_EQ(firstIssue(s, R"({"x": 0})"), "/x must be > 0 (got 0)");
    EXPECT_EQ(firstIssue(s, R"({"x": 0.3})"), "/x must be a multiple of 0.5");
    EXPECT_EQ(firstIssue(s, R"({"s": "A1"})"), "/s \"A1\" does not match the pattern ^[a-z]+$");
    EXPECT_EQ(firstIssue(s, R"({"e": "z"})"), "/e must be one of \"a\", \"b\" (got \"z\")");
    EXPECT_EQ(firstIssue(s, R"({"c": 4})"), "/c must be 3");
    EXPECT_EQ(firstIssue(s, R"({"o": 1})"), "/o must be of type string or null (got integer)");
}

TEST(JsonSchema, ObjectsArraysAndExtensions) {
    const auto s = compile(R"({"type": "object", "required": ["id"],
        "properties": {"id": {"type": "string"},
                       "p": {"type": "array", "prefixItems": [{"type": "number"},
                             {"type": "string"}], "items": false, "minItems": 2},
                       "u": {"type": "array", "uniqueItems": true, "maxItems": 3}},
        "patternProperties": {"^x-": true}, "additionalProperties": false})");
    EXPECT_TRUE(s.validate(Json::parse(R"({"id": "a", "x-note": 1, "p": [1, "b"]})")).empty());
    EXPECT_EQ(firstIssue(s, R"({})"), " missing required property \"id\"");
    EXPECT_EQ(firstIssue(s, R"({"id": "a", "foo": 1})"), "/foo unknown property \"foo\"");
    EXPECT_EQ(firstIssue(s, R"({"id": "a", "p": [1, 2]})"),
              "/p/1 must be of type string (got integer)");
    EXPECT_EQ(firstIssue(s, R"({"id": "a", "p": [1, "b", 3]})"), "/p/2 is not allowed here");
    EXPECT_EQ(firstIssue(s, R"({"id": "a", "u": [1, 1]})"), "/u/1 duplicate of element 0");
    EXPECT_EQ(firstIssue(s, R"({"id": "a", "u": [1, 2, 3, 4]})"),
              "/u must have at most 3 elements");
}

TEST(JsonSchema, ReferencesAndConditionals) {
    const auto s = compile(R"({"$defs": {
            "a": {"type": "object", "properties": {"kind": {"const": "a"}, "r": {"type": "number"}},
                  "required": ["r"], "additionalProperties": false},
            "b": {"type": "object", "properties": {"kind": {"const": "b"}, "w": {"type": "number"}},
                  "required": ["w"], "additionalProperties": false}},
        "type": "array", "items": {"type": "object", "required": ["kind"],
            "allOf": [{"if": {"properties": {"kind": {"const": "a"}}}, "then": {"$ref": "#/$defs/a"}},
                      {"if": {"properties": {"kind": {"const": "b"}}}, "then": {"$ref": "#/$defs/b"}}]}})");
    EXPECT_TRUE(s.validate(Json::parse(R"([{"kind": "a", "r": 1}, {"kind": "b", "w": 2}])")).empty());
    EXPECT_EQ(firstIssue(s, R"([{"kind": "a", "w": 1}])"), "/0 missing required property \"r\"");
    EXPECT_EQ(firstIssue(s, R"([{"kind": "b", "w": 1, "r": 2}])"), "/0/r unknown property \"r\"");
}

TEST(JsonSchema, OneOfReportsTheClosestAlternative) {
    const auto s = compile(R"({"oneOf": [{"type": "string"},
        {"type": "object", "required": ["a", "b"]}]})");
    EXPECT_TRUE(s.validate(Json::parse(R"("x")")).empty());
    EXPECT_EQ(firstIssue(s, R"({"a": 1})"), " missing required property \"b\"");
    const auto both = compile(R"({"oneOf": [{"type": "number"}, {"minimum": 0}]})");
    EXPECT_EQ(firstIssue(both, "1"), " matches more than one allowed form");
}

TEST(JsonSchema, LimitsTheNumberOfIssues) {
    const auto s = compile(R"({"type": "array", "items": {"type": "string"}})");
    EXPECT_EQ(s.validate(Json::parse("[1, 2, 3, 4, 5]"), 2).size(), 2U);
}

TEST(JsonSchema, MissionSchemaCompiles) {
    auto schema = JsonSchema::load(std::string(DS_TEST_ASSETS_DIR) + "/missions/mission.schema.json");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;
    EXPECT_EQ(schema->document()["$schema"], "https://json-schema.org/draft/2020-12/schema");
}

}  // namespace
