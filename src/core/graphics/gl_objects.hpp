// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

/// @file
/// @brief Private header: move-only RAII owners for OpenGL objects (FR-GFX-03), created with
/// DSA functions (FR-GFX-02). Destruction needs the owning context to be current.
/// @ingroup graphics

#include <glad/gl.h>

#include <utility>

/// @brief Thin OpenGL wrappers used by the renderer.
namespace ds::graphics::gl {

/// @brief Owns one OpenGL object name and deletes it with @p Delete.
template <void (*Delete)(GLuint)>
class Handle {
public:
    Handle() = default;
    /// @brief Takes ownership of @p id.
    explicit Handle(GLuint id) noexcept : id_(id) {}
    ~Handle() { reset(); }
    /// @brief Moves ownership.
    Handle(Handle&& other) noexcept : id_(std::exchange(other.id_, 0)) {}
    /// @brief Releases the current object and moves ownership.
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            id_ = std::exchange(other.id_, 0);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    /// @brief The object name (0 if empty).
    [[nodiscard]] GLuint id() const noexcept { return id_; }
    /// @brief True if an object is owned.
    explicit operator bool() const noexcept { return id_ != 0; }
    /// @brief Deletes the object, if any.
    void reset() noexcept {
        if (id_ != 0) {
            Delete(id_);
            id_ = 0;
        }
    }

private:
    GLuint id_ = 0;
};

/// @cond INTERNAL
namespace detail {
inline void deleteBuffer(GLuint id) { glDeleteBuffers(1, &id); }
inline void deleteVertexArray(GLuint id) { glDeleteVertexArrays(1, &id); }
inline void deleteTexture(GLuint id) { glDeleteTextures(1, &id); }
inline void deleteFramebuffer(GLuint id) { glDeleteFramebuffers(1, &id); }
inline void deleteProgram(GLuint id) { glDeleteProgram(id); }
inline void deleteShader(GLuint id) { glDeleteShader(id); }
}  // namespace detail
/// @endcond

/// @brief Buffer object.
using Buffer = Handle<&detail::deleteBuffer>;
/// @brief Vertex array object.
using VertexArray = Handle<&detail::deleteVertexArray>;
/// @brief Texture object.
using Texture = Handle<&detail::deleteTexture>;
/// @brief Framebuffer object.
using Framebuffer = Handle<&detail::deleteFramebuffer>;
/// @brief Program object.
using Program = Handle<&detail::deleteProgram>;
/// @brief Shader object.
using Shader = Handle<&detail::deleteShader>;

/// @brief Creates a buffer with immutable storage (`glNamedBufferStorage`).
inline Buffer createBuffer(GLsizeiptr size, const void* data, GLbitfield flags) {
    GLuint id = 0;
    glCreateBuffers(1, &id);
    glNamedBufferStorage(id, size, data, flags);
    return Buffer(id);
}

/// @brief Creates an empty vertex array object.
inline VertexArray createVertexArray() {
    GLuint id = 0;
    glCreateVertexArrays(1, &id);
    return VertexArray(id);
}

/// @brief Creates a 2D texture with immutable storage, linear filtering and edge clamping.
inline Texture createTexture2D(GLenum internalFormat, GLsizei width, GLsizei height,
                               GLsizei levels = 1) {
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &id);
    glTextureStorage2D(id, levels, internalFormat, width, height);
    glTextureParameteri(id, GL_TEXTURE_MIN_FILTER,
                        levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return Texture(id);
}

/// @brief Creates a multisampled 2D texture (fixed sample locations).
inline Texture createTexture2DMultisample(GLenum internalFormat, GLsizei samples, GLsizei width,
                                          GLsizei height) {
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &id);
    glTextureStorage2DMultisample(id, samples, internalFormat, width, height, GL_TRUE);
    return Texture(id);
}

/// @brief Creates a depth texture array for shadow maps: linear filtering with depth
/// comparison (`sampler2DArrayShadow`), clamped to the border (lit outside the map).
inline Texture createShadowArray(GLsizei size, GLsizei layers) {
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &id);
    glTextureStorage3D(id, 1, GL_DEPTH_COMPONENT32F, size, size, layers);
    glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const GLfloat border[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    glTextureParameterfv(id, GL_TEXTURE_BORDER_COLOR, border);
    glTextureParameteri(id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTextureParameteri(id, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    return Texture(id);
}

/// @brief Creates a framebuffer object without attachments.
inline Framebuffer createFramebuffer() {
    GLuint id = 0;
    glCreateFramebuffers(1, &id);
    return Framebuffer(id);
}

}  // namespace ds::graphics::gl
