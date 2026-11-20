// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Mesh files for vehicles and items (FR-GFX-12): glTF and OBJ through Assimp, the frame
// conversions, fitting and the model library's search order.

#include "ds/graphics/mesh_loader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "ds/graphics/drone_model.hpp"
#include "ds/graphics/rover_model.hpp"
#include "ds/items/item.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/ground_profile.hpp"
#include "ds/sim/item_visuals.hpp"

namespace {

using namespace ds;
using graphics::Mesh;
using graphics::MeshBounds;

const std::filesystem::path kModels = std::filesystem::path(DS_TEST_FIXTURES_DIR) / "models";

void expectVec(const glm::vec3& a, const glm::vec3& b, float tol = 1e-5F) {
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}

Mesh box(glm::vec3 centre, glm::vec3 half) {
    Mesh m;
    graphics::appendBox(m, centre, half, {});
    return m;
}

TEST(MeshLoader, ReadsGltfWithNodeTransformsAndMaterials) {
    auto mesh = graphics::loadMesh(kModels / "two-triangles.gltf");
    ASSERT_TRUE(mesh.has_value()) << mesh.error().message;
    ASSERT_EQ(mesh->vertices.size(), 6U);
    ASSERT_EQ(mesh->indices.size(), 6U);
    // Node transforms are applied: the first triangle lifted by 1 m, the second doubled.
    const MeshBounds b = graphics::meshBounds(*mesh);
    expectVec(b.min, {0.0F, 0.0F, 0.0F});
    expectVec(b.max, {2.0F, 2.0F, 1.0F});
    // The translucent material makes the mesh transparent.
    EXPECT_TRUE(mesh->transparent);
    int red = 0;
    int glowing = 0;
    for (const graphics::MeshVertex& v : mesh->vertices) {
        if (v.position.y == 1.0F && v.position.x <= 1.0F) {
            // Lifted, red, opaque, facing up (its winding is counter-clockwise from above).
            ++red;
            expectVec(glm::vec3(v.color), {1.0F, 0.0F, 0.0F});
            EXPECT_FLOAT_EQ(v.color.a, 1.0F);
            EXPECT_FLOAT_EQ(v.emissive, 0.0F);
            expectVec(v.normal, {0.0F, 1.0F, 0.0F});
        } else {
            // Emits green, half transparent: shown in its light colour.
            ++glowing;
            expectVec(glm::vec3(v.color), {0.0F, 1.0F, 0.0F});
            EXPECT_NEAR(v.color.a, 0.5F, 1e-6F);
            EXPECT_NEAR(v.emissive, graphics::kModelEmissiveCdM2, 1e-3F);
            expectVec(v.normal, {0.0F, 0.0F, -1.0F});
        }
    }
    EXPECT_EQ(red, 3);
    EXPECT_EQ(glowing, 3);
}

TEST(MeshLoader, ReadsObjWithItsMaterialAndTriangulatesQuads) {
    auto mesh = graphics::loadMesh(kModels / "blue-quad.obj");
    ASSERT_TRUE(mesh.has_value()) << mesh.error().message;
    EXPECT_EQ(mesh->indices.size(), 6U);
    EXPECT_FALSE(mesh->transparent);
    for (const graphics::MeshVertex& v : mesh->vertices) {
        expectVec(glm::vec3(v.color), {0.0F, 0.0F, 1.0F});
        expectVec(v.normal, {0.0F, 1.0F, 0.0F});
    }
    const MeshBounds b = graphics::meshBounds(*mesh);
    expectVec(b.max - b.min, {2.0F, 0.0F, 1.0F});
}

TEST(MeshLoader, RefusesMissingBrokenAndUnsupportedFiles) {
    EXPECT_TRUE(graphics::isModelFile("a/b/Rover.GLB"));
    EXPECT_TRUE(graphics::isModelFile("x.obj"));
    EXPECT_FALSE(graphics::isModelFile("x.fbx"));
    EXPECT_FALSE(graphics::loadMesh(kModels / "missing.gltf").has_value());
    EXPECT_FALSE(graphics::loadMesh(kModels / "not-a-model.gltf").has_value());
    const auto fbx = graphics::loadMesh(kModels / "blue-quad.mtl");
    ASSERT_FALSE(fbx.has_value());
    EXPECT_NE(fbx.error().message.find(".gltf"), std::string::npos);
}

TEST(MeshLoader, FrameConversionsKeepTheWindingAndPointTheFrontForward) {
    // A wedge pointing to the front of a glTF model (+Z), its top up (+Y), its left side +X.
    Mesh m;
    m.vertices = {{.position = {0.0F, 0.0F, 1.0F}},
                  {.position = {0.0F, 1.0F, 0.0F}},
                  {.position = {1.0F, 0.0F, 0.0F}}};
    Mesh body = m;
    graphics::gltfToBodyFrame(body);
    // Body frame: x forward, y right, z down.
    expectVec(body.vertices[0].position, {1.0F, 0.0F, 0.0F});
    expectVec(body.vertices[1].position, {0.0F, 0.0F, -1.0F});
    expectVec(body.vertices[2].position, {0.0F, -1.0F, 0.0F});
    Mesh item = m;
    graphics::gltfToItemFrame(item);
    // Item frame: y up, the front towards −z (north at heading 0).
    expectVec(item.vertices[0].position, {0.0F, 0.0F, -1.0F});
    expectVec(item.vertices[1].position, {0.0F, 1.0F, 0.0F});
    expectVec(item.vertices[2].position, {-1.0F, 0.0F, 0.0F});
    // Proper rotations: the handedness (and so the triangle winding) is kept.
    const auto handedness = [](const Mesh& t) {
        return glm::dot(glm::cross(t.vertices[1].position - t.vertices[0].position,
                                   t.vertices[2].position - t.vertices[0].position),
                        t.vertices[0].position);
    };
    EXPECT_NEAR(handedness(body), handedness(m), 1e-6F);
    EXPECT_NEAR(handedness(item), handedness(m), 1e-6F);
}

TEST(MeshLoader, FitScalesUniformlyCentresAndRestsOnTheBottom) {
    // A 4 × 1 × 2 model into a 2 × 2 × 2 box from y = 0: scaled by 0.5 (its length limits),
    // centred in x and z, resting on y = 0.
    Mesh m = box({10.0F, 5.0F, -3.0F}, {2.0F, 0.5F, 1.0F});
    graphics::fitMesh(m, {.min = {-1.0F, 0.0F, -1.0F}, .max = {1.0F, 2.0F, 1.0F}}, 1, -1.0F);
    MeshBounds b = graphics::meshBounds(m);
    expectVec(b.min, {-1.0F, 0.0F, -0.5F});
    expectVec(b.max, {1.0F, 0.5F, 0.5F});
    // Body frame: resting on the max z face (z points down).
    graphics::fitMesh(m, {.min = {-1.0F, -1.0F, -1.0F}, .max = {1.0F, 1.0F, 0.2F}}, 2, 1.0F);
    b = graphics::meshBounds(m);
    EXPECT_NEAR(b.max.z, 0.2F, 1e-5F);
    EXPECT_NEAR(b.max.x - b.min.x, 2.0F, 1e-5F);
}

TEST(ModelLibrary, SearchesTheExtraFolderFirstAndCaches) {
    const auto tmp = std::filesystem::temp_directory_path() / "ds_model_library_test";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "mission");
    std::filesystem::create_directories(tmp / "user");
    std::filesystem::copy_file(kModels / "blue-quad.obj", tmp / "mission" / "blue-quad.obj");
    graphics::ModelLibrary library({tmp / "user", kModels});

    auto fromModels = library.resolve("blue-quad.obj");
    ASSERT_TRUE(fromModels.has_value());
    EXPECT_EQ(fromModels->parent_path(), kModels.lexically_normal());
    auto fromMission = library.resolve("blue-quad.obj", tmp / "mission");
    ASSERT_TRUE(fromMission.has_value());
    EXPECT_EQ(fromMission->parent_path(), (tmp / "mission").lexically_normal());
    // Absolute paths are used as they are.
    EXPECT_TRUE(library.resolve((kModels / "two-triangles.gltf").string()).has_value());

    const auto missing = library.resolve("nowhere.glb");
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().message.find((tmp / "user").string()), std::string::npos);
    EXPECT_FALSE(library.resolve("").has_value());

    auto a = library.load("two-triangles.gltf");
    auto b = library.load("two-triangles.gltf");
    ASSERT_TRUE(a.has_value() && b.has_value());
    EXPECT_EQ(a->get(), b->get());
    EXPECT_FALSE(library.load("not-a-model.gltf").has_value());
    std::filesystem::remove_all(tmp);
}

TEST(ModelLibrary, VehicleBodiesAreReplacedAndFitted) {
    graphics::RoverShape shape;
    shape.wheelHubs = {
        {0.15, -0.15, 0.05}, {-0.15, -0.15, 0.05}, {0.15, 0.15, 0.05}, {-0.15, 0.15, 0.05}};
    graphics::RoverModel rover(shape);
    const MeshBounds target = rover.bodyBounds();
    auto mesh = graphics::loadMesh(kModels / "two-triangles.gltf");
    ASSERT_TRUE(mesh.has_value());
    auto body = std::make_shared<Mesh>(*mesh);
    graphics::gltfToBodyFrame(*body);
    graphics::fitMesh(*body, target, 2, 1.0F);
    const MeshBounds b = graphics::meshBounds(*body);
    EXPECT_GE(b.min.x, target.min.x - 1e-5F);
    EXPECT_LE(b.max.x, target.max.x + 1e-5F);
    EXPECT_NEAR(b.max.z, target.max.z, 1e-5F);
    rover.setBodyMesh(body);
    std::vector<graphics::DrawItem> items;
    rover.appendDrawItems({}, items);
    ASSERT_FALSE(items.empty());
    EXPECT_EQ(items.front().mesh.get(), body.get());
    // Null brings the procedural body back.
    rover.setBodyMesh(nullptr);
    items.clear();
    rover.appendDrawItems({}, items);
    EXPECT_NE(items.front().mesh.get(), body.get());

    graphics::DroneModel drone(
        graphics::droneShapeFor({glm::dvec3(0.1, 0.1, 0), glm::dvec3(-0.1, 0.1, 0),
                                 glm::dvec3(-0.1, -0.1, 0), glm::dvec3(0.1, -0.1, 0)},
                                0.127, 3, 0.03, 30.0));
    drone.setFrameMesh(body);
    items.clear();
    drone.appendDrawItems({}, items);
    EXPECT_EQ(items.front().mesh.get(), body.get());
}

TEST(ModelLibrary, ObstacleModelReplacesTheShapeMesh) {
    items::Item obstacle;
    obstacle.key = "crate";
    obstacle.type = items::ItemType::Obstacle;
    obstacle.properties =
        common::Json::parse(R"({"shape":"box","sizeM":[2,1,1],"model":"blue-quad.obj"})");
    obstacle.shape = items::defaultShape(obstacle.type, obstacle.properties);
    const auto procedural = sim::ItemVisuals::meshFor(obstacle, true);
    ASSERT_NE(procedural, nullptr);
    graphics::ModelLibrary library({kModels});
    const auto model = sim::ItemVisuals::modelMeshFor(obstacle, *procedural, library);
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->indices.size(), 6U);
    const MeshBounds shape = graphics::meshBounds(*procedural);
    const MeshBounds b = graphics::meshBounds(*model);
    // Resting on the bottom of the shape, as long as it fits.
    EXPECT_NEAR(b.min.y, shape.min.y, 1e-5F);
    EXPECT_LE(b.max.x - b.min.x, shape.max.x - shape.min.x + 1e-5F);
    EXPECT_LE(b.max.z - b.min.z, shape.max.z - shape.min.z + 1e-5F);

    // A missing file or another item type: no model (the shape is drawn).
    obstacle.properties["model"] = "missing.glb";
    EXPECT_EQ(sim::ItemVisuals::modelMeshFor(obstacle, *procedural, library), nullptr);
    items::Item pad = obstacle;
    pad.type = items::ItemType::LandingPad;
    pad.properties = common::Json::parse(R"({"model":"blue-quad.obj"})");
    EXPECT_EQ(sim::ItemVisuals::modelMeshFor(pad, *procedural, library), nullptr);

    // Through the registry: the obstacle is drawn with the model.
    obstacle.properties["model"] = "blue-quad.obj";
    items::ItemRegistry registry;
    ASSERT_TRUE(registry.add(obstacle).has_value());
    const sim::ItemVisuals visuals(registry, true, &library);
    std::vector<graphics::DrawItem> drawn;
    visuals.appendDrawItems(0.0, {}, drawn);
    ASSERT_EQ(drawn.size(), 1U);
    EXPECT_EQ(drawn.front().mesh->indices.size(), 6U);
}

TEST(ModelLibrary, ProfilesKeepTheirModelFile) {
    physics::GroundProfile g;
    g.key = "user.rover";
    g.name = "Rover";
    g.modelFile = "my-rover.glb";
    auto back = physics::groundProfileFromJson(physics::groundProfileToJson(g));
    ASSERT_TRUE(back.has_value()) << back.error().message;
    EXPECT_EQ(back->modelFile, "my-rover.glb");
    // Omitted when empty.
    g.modelFile.clear();
    EXPECT_FALSE(physics::groundProfileToJson(g).contains("modelFile"));

    physics::AerialProfile a;
    a.key = "user.quad";
    a.name = "Quad";
    a.modelFile = "/home/me/quad.gltf";
    auto aback = physics::aerialProfileFromJson(physics::aerialProfileToJson(a));
    ASSERT_TRUE(aback.has_value()) << aback.error().message;
    EXPECT_EQ(aback->modelFile, "/home/me/quad.gltf");
}

}  // namespace

namespace {

TEST(MeshLoader, BuiltInCrateHasSharpEdges) {
    auto mesh =
        ds::graphics::loadMesh(std::filesystem::path(DS_TEST_ASSETS_DIR) / "models" / "crate.obj");
    ASSERT_TRUE(mesh.has_value()) << mesh.error().message;
    // Flat faces: every normal is along an axis.
    for (const ds::graphics::MeshVertex& v : mesh->vertices) {
        const glm::vec3 a = glm::abs(v.normal);
        EXPECT_NEAR(std::max({a.x, a.y, a.z}), 1.0F, 1e-4F);
    }
    const auto b = ds::graphics::meshBounds(*mesh);
    EXPECT_NEAR(b.min.y, 0.0F, 1e-6F);
    EXPECT_NEAR(b.max.y, 1.0F, 1e-6F);
}

}  // namespace
