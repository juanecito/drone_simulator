// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/graphics/mesh.hpp"

#include <glm/glm.hpp>

/// @file
/// @brief Mesh files for vehicles and items (FR-GFX-12): glTF 2.0 (`.gltf`, `.glb`) preferred,
/// Wavefront OBJ (`.obj`) as a fallback, read with Assimp. Geometry and material colours only:
/// the base colour (or OBJ diffuse colour) times the vertex colours, the opacity and the
/// emissive colour; texture maps are ignored. Meshes come out in the glTF axes — metres, +Y up,
/// the front of the model towards +Z (its right side towards −X) — with the node transforms
/// applied, and are converted to a vehicle body or an item frame with the helpers below.
/// @ingroup graphics

namespace ds::graphics {

/// @brief Largest mesh accepted (vertices after triangulation), against oversized files.
inline constexpr std::size_t kMaxModelVertices = 2'000'000;

/// @brief Luminance of a vertex whose material emits pure white (emissive factor 1), cd/m²:
/// a lit display or LED panel.
inline constexpr float kModelEmissiveCdM2 = 300.0F;

/// @brief True for the file extensions the loader reads (`.gltf`, `.glb`, `.obj`, any case).
[[nodiscard]] bool isModelFile(const std::filesystem::path& file);

/// @brief Reads a mesh file (glTF axes, see above). Fails on unreadable, empty or oversized
/// files and unsupported extensions.
[[nodiscard]] common::Result<Mesh> loadMesh(const std::filesystem::path& file);

/// @brief Axis-aligned bounds of the vertices (min > max for an empty mesh).
struct MeshBounds {
    glm::vec3 min{0.0F};  ///< Smallest coordinates.
    glm::vec3 max{0.0F};  ///< Largest coordinates.
};

/// @brief Bounds of @p mesh.
[[nodiscard]] MeshBounds meshBounds(const Mesh& mesh);

/// @brief glTF axes → vehicle body frame (x forward, y right, z down): x = z_gltf,
/// y = −x_gltf, z = −y_gltf.
void gltfToBodyFrame(Mesh& mesh);

/// @brief glTF axes → item frame (y up, front of the item towards −z, as headings turn it):
/// a half turn about +y.
void gltfToItemFrame(Mesh& mesh);

/// @brief Scales @p mesh uniformly (by the largest factor that keeps it inside @p target) and
/// moves it to the centre of @p target, except along @p downAxis (0 x, 1 y, 2 z) where it rests
/// on the face in the direction of @p downSign (+1: the max face, −1: the min face).
void fitMesh(Mesh& mesh, const MeshBounds& target, int downAxis, float downSign);

/// @brief Loads model files once and shares the meshes (glTF axes). A relative file name is
/// looked for in the extra folder passed to load() (e.g. the mission's folder), then in the
/// search folders in order; absolute paths are used as they are. Thread-safe.
class ModelLibrary {
public:
    /// @brief A library searching @p searchDirs (e.g. the user's and the built-in model folders).
    explicit ModelLibrary(std::vector<std::filesystem::path> searchDirs = {});

    /// @brief Where @p file is found, or an error naming the folders searched.
    [[nodiscard]] common::Result<std::filesystem::path> resolve(
        const std::string& file, const std::filesystem::path& extraDir = {}) const;

    /// @brief The mesh of @p file (cached by its resolved path, failures too).
    [[nodiscard]] common::Result<std::shared_ptr<const Mesh>> load(
        const std::string& file, const std::filesystem::path& extraDir = {});

    /// @brief The search folders.
    [[nodiscard]] const std::vector<std::filesystem::path>& searchDirs() const noexcept {
        return searchDirs_;
    }

private:
    std::vector<std::filesystem::path> searchDirs_;
    std::mutex mutex_;
    std::map<std::filesystem::path, common::Result<std::shared_ptr<const Mesh>>> cache_;
};

}  // namespace ds::graphics
