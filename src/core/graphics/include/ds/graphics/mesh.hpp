// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

/// @file
/// @brief Triangle meshes for objects (vehicles, later items) and the instances drawn each
/// frame (FR-GFX-12).
/// @ingroup graphics

namespace ds::graphics {

/// @brief Vertex of an object mesh (44 bytes).
struct MeshVertex {
    glm::vec3 position{0.0F};            ///< Model space, metres.
    glm::vec3 normal{0.0F, 1.0F, 0.0F};  ///< Unit normal, model space.
    glm::vec4 color{1.0F};               ///< Linear albedo (rgb) and opacity (a).
    float emissive = 0.0F;  ///< Emitted luminance, cd/m² (LEDs), before the instance scale.
};
static_assert(sizeof(MeshVertex) == 44);

/// @brief Indexed triangle list (counter-clockwise front faces).
struct Mesh {
    std::vector<MeshVertex> vertices;    ///< Vertices.
    std::vector<std::uint32_t> indices;  ///< Three per triangle.
    bool transparent = false;  ///< Drawn after the opaque scene, blended, without depth writes.
};

/// @brief Material of a primitive.
struct MeshMaterial {
    glm::vec3 albedo{0.5F};  ///< Linear albedo.
    float opacity = 1.0F;    ///< 1 = opaque.
    float emissive = 0.0F;   ///< Emitted luminance, cd/m².
};

/// @brief Appends a box centred at @p centre with half sizes @p halfExtents, rotated by
/// @p rotation (24 vertices, flat normals).
void appendBox(Mesh& mesh, const glm::vec3& centre, const glm::vec3& halfExtents,
               const MeshMaterial& material, const glm::quat& rotation = glm::quat(1, 0, 0, 0));

/// @brief Appends a closed cylinder between @p a and @p b with @p segments sides.
void appendCylinder(Mesh& mesh, const glm::vec3& a, const glm::vec3& b, float radius, int segments,
                    const MeshMaterial& material);

/// @brief Appends a disc (both faces) centred at @p centre, facing @p normal.
void appendDisc(Mesh& mesh, const glm::vec3& centre, const glm::vec3& normal, float radius,
                int segments, const MeshMaterial& material);

/// @brief Appends a UV sphere.
void appendSphere(Mesh& mesh, const glm::vec3& centre, float radius, int segments,
                  const MeshMaterial& material);

/// @brief One mesh drawn this frame.
struct DrawItem {
    std::shared_ptr<const Mesh> mesh;            ///< Geometry (cached on the GPU by address).
    glm::dvec3 position{0.0};                    ///< World position of the model origin.
    glm::dquat orientation{1.0, 0.0, 0.0, 0.0};  ///< Model → world rotation.
    glm::vec3 scale{1.0F};                       ///< Non-uniform scale (model axes).
    glm::vec4 tint{1.0F};                        ///< Multiplies albedo and opacity.
    float emissiveScale = 1.0F;                  ///< Multiplies the vertex emission (0 = off).
    bool castsShadow = true;                     ///< Drawn into the shadow maps.
};

}  // namespace ds::graphics
