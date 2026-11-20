// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/mesh_loader.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <system_error>
#include <utility>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace ds::graphics {

namespace {

std::string lowerExtension(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// Colour, opacity and emission of a material.
struct VertexLook {
    glm::vec3 albedo{0.8F};
    float opacity = 1.0F;
    float emissive = 0.0F;
};

VertexLook lookOf(const aiMaterial* material) {
    VertexLook look;
    if (material == nullptr) {
        return look;
    }
    aiColor4D base;
    if (material->Get(AI_MATKEY_BASE_COLOR, base) == AI_SUCCESS ||
        material->Get(AI_MATKEY_COLOR_DIFFUSE, base) == AI_SUCCESS) {
        look.albedo = glm::clamp(glm::vec3(base.r, base.g, base.b), 0.0F, 1.0F);
        look.opacity = std::clamp(base.a, 0.0F, 1.0F);
    }
    // glTF stores the opacity in the base colour and Assimp may copy it here: take the
    // smaller, so it is not applied twice.
    if (float opacity = 1.0F; material->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
        look.opacity = std::min(look.opacity, std::clamp(opacity, 0.0F, 1.0F));
    }
    if (aiColor3D e; material->Get(AI_MATKEY_COLOR_EMISSIVE, e) == AI_SUCCESS) {
        const float peak = std::max({e.r, e.g, e.b});
        if (peak > 1e-4F) {
            float strength = 1.0F;
            (void)material->Get(AI_MATKEY_EMISSIVE_INTENSITY, strength);
            // The shader emits colour × emissive: an emitting material shows its light colour.
            look.albedo = glm::vec3(e.r, e.g, e.b) / peak;
            look.emissive = peak * std::max(strength, 0.0F) * kModelEmissiveCdM2;
        }
    }
    return look;
}

void transformVertices(Mesh& mesh, const glm::mat3& m) {
    for (MeshVertex& v : mesh.vertices) {
        v.position = m * v.position;
        v.normal = m * v.normal;
    }
}

}  // namespace

bool isModelFile(const std::filesystem::path& file) {
    const std::string ext = lowerExtension(file);
    return ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

common::Result<Mesh> loadMesh(const std::filesystem::path& file) {
    if (!isModelFile(file)) {
        return common::makeError(
            std::format("{}: not a model file (.gltf, .glb or .obj)", file.string()));
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return common::makeError(std::format("{}: file not found", file.string()));
    }
    Assimp::Importer importer;
    // Points and lines are dropped; all node transforms are baked into the vertices.
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                                aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    // Files without normals: smooth across gentle bends only, so box edges stay sharp.
    importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, 60.0F);
    const aiScene* scene = importer.ReadFile(
        file.string(), aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                           aiProcess_PreTransformVertices | aiProcess_JoinIdenticalVertices |
                           aiProcess_SortByPType | aiProcess_ValidateDataStructure);
    if (scene == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0U) {
        return common::makeError(
            std::format("{}: cannot read the model: {}", file.string(), importer.GetErrorString()));
    }
    Mesh out;
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* m = scene->mMeshes[mi];
        if (m == nullptr || (m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0U) {
            continue;
        }
        if (out.vertices.size() + m->mNumVertices > kMaxModelVertices) {
            return common::makeError(std::format("{}: the model has more than {} vertices",
                                                 file.string(), kMaxModelVertices));
        }
        const VertexLook look =
            lookOf(m->mMaterialIndex < scene->mNumMaterials ? scene->mMaterials[m->mMaterialIndex]
                                                            : nullptr);
        if (look.opacity < 0.999F) {
            out.transparent = true;
        }
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        const bool colours = m->HasVertexColors(0);
        for (unsigned int i = 0; i < m->mNumVertices; ++i) {
            MeshVertex v;
            v.position = {m->mVertices[i].x, m->mVertices[i].y, m->mVertices[i].z};
            if (m->HasNormals()) {
                const glm::vec3 n(m->mNormals[i].x, m->mNormals[i].y, m->mNormals[i].z);
                const float len = glm::length(n);
                v.normal = len > 1e-6F ? n / len : glm::vec3(0.0F, 1.0F, 0.0F);
            }
            glm::vec4 c(look.albedo, look.opacity);
            if (colours) {
                const aiColor4D& vc = m->mColors[0][i];
                c *= glm::clamp(glm::vec4(vc.r, vc.g, vc.b, vc.a), 0.0F, 1.0F);
            }
            v.color = c;
            v.emissive = look.emissive;
            out.vertices.push_back(v);
        }
        for (unsigned int f = 0; f < m->mNumFaces; ++f) {
            const aiFace& face = m->mFaces[f];
            if (face.mNumIndices != 3) {
                continue;
            }
            for (unsigned int k = 0; k < 3; ++k) {
                out.indices.push_back(base + face.mIndices[k]);
            }
        }
    }
    if (out.indices.empty()) {
        return common::makeError(std::format("{}: the model has no triangles", file.string()));
    }
    return out;
}

MeshBounds meshBounds(const Mesh& mesh) {
    constexpr float kBig = std::numeric_limits<float>::max();
    MeshBounds b{.min = glm::vec3(kBig), .max = glm::vec3(-kBig)};
    for (const MeshVertex& v : mesh.vertices) {
        b.min = glm::min(b.min, v.position);
        b.max = glm::max(b.max, v.position);
    }
    return b;
}

void gltfToBodyFrame(Mesh& mesh) {
    // Columns are the images of the glTF x, y and z axes: x → −y (left), y → −z (up),
    // z → +x (forward). A proper rotation, so the winding is kept.
    transformVertices(mesh, glm::mat3(glm::vec3(0.0F, -1.0F, 0.0F), glm::vec3(0.0F, 0.0F, -1.0F),
                                      glm::vec3(1.0F, 0.0F, 0.0F)));
}

void gltfToItemFrame(Mesh& mesh) {
    transformVertices(mesh, glm::mat3(glm::vec3(-1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F),
                                      glm::vec3(0.0F, 0.0F, -1.0F)));
}

void fitMesh(Mesh& mesh, const MeshBounds& target, int downAxis, float downSign) {
    if (mesh.vertices.empty()) {
        return;
    }
    const MeshBounds b = meshBounds(mesh);
    const glm::vec3 size = b.max - b.min;
    const glm::vec3 room = target.max - target.min;
    float scale = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        if (size[i] > 1e-6F) {
            scale = std::min(scale, room[i] / size[i]);
        }
    }
    if (scale == std::numeric_limits<float>::max() || scale <= 0.0F) {
        scale = 1.0F;
    }
    glm::vec3 shift = 0.5F * (target.min + target.max) - scale * 0.5F * (b.min + b.max);
    const auto d = static_cast<std::size_t>(std::clamp(downAxis, 0, 2));
    shift[static_cast<glm::length_t>(d)] = downSign > 0.0F
                                               ? target.max[static_cast<glm::length_t>(d)] -
                                                     scale * b.max[static_cast<glm::length_t>(d)]
                                               : target.min[static_cast<glm::length_t>(d)] -
                                                     scale * b.min[static_cast<glm::length_t>(d)];
    for (MeshVertex& v : mesh.vertices) {
        v.position = scale * v.position + shift;
    }
}

ModelLibrary::ModelLibrary(std::vector<std::filesystem::path> searchDirs)
    : searchDirs_(std::move(searchDirs)) {}

common::Result<std::filesystem::path> ModelLibrary::resolve(
    const std::string& file, const std::filesystem::path& extraDir) const {
    const std::filesystem::path p(file);
    std::error_code ec;
    if (file.empty()) {
        return common::makeError("no model file given");
    }
    if (p.is_absolute()) {
        if (std::filesystem::is_regular_file(p, ec)) {
            return p;
        }
        return common::makeError(std::format("model {} not found", file));
    }
    std::vector<std::filesystem::path> dirs;
    if (!extraDir.empty()) {
        dirs.push_back(extraDir);
    }
    dirs.insert(dirs.end(), searchDirs_.begin(), searchDirs_.end());
    std::string searched;
    for (const std::filesystem::path& dir : dirs) {
        const std::filesystem::path candidate = dir / p;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.lexically_normal();
        }
        searched += (searched.empty() ? "" : ", ") + dir.string();
    }
    return common::makeError(std::format("model {} not found (searched: {})", file,
                                         searched.empty() ? "no folders" : searched));
}

common::Result<std::shared_ptr<const Mesh>> ModelLibrary::load(
    const std::string& file, const std::filesystem::path& extraDir) {
    auto path = resolve(file, extraDir);
    if (!path) {
        return std::unexpected(path.error());
    }
    const std::scoped_lock lock(mutex_);
    if (const auto it = cache_.find(*path); it != cache_.end()) {
        return it->second;
    }
    common::Result<std::shared_ptr<const Mesh>> result = loadMesh(*path).transform(
        [](Mesh m) { return std::make_shared<const Mesh>(std::move(m)); });
    cache_.emplace(*path, result);
    return result;
}

}  // namespace ds::graphics
