// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/opengl_check.hpp"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>

#include <format>
#include <string>

#include "ds/graphics/renderer.hpp"

namespace ds::app {
namespace {

// Enums from the OpenGL 3.2+ core headers (QOpenGLFunctions only declares ES 2.0 ones).
constexpr GLenum kGlMajorVersion = 0x821B;
constexpr GLenum kGlMinorVersion = 0x821C;
constexpr GLenum kGlContextProfileMask = 0x9126;
constexpr GLint kGlContextCoreProfileBit = 0x1;

std::string glString(QOpenGLFunctions& gl, GLenum name) {
    const GLubyte* text = gl.glGetString(name);
    return text != nullptr ? std::string(reinterpret_cast<const char*>(text))
                           : std::string{"unknown"};
}

}  // namespace

std::expected<void, common::Error> checkOpenGlContext() {
    constexpr int kMajor = graphics::Renderer::kRequiredMajor;
    constexpr int kMinor = graphics::Renderer::kRequiredMinor;
    const std::string requirement = std::format(
        "Drone Simulator needs OpenGL {}.{} (core profile) or newer. Check that the graphics "
        "driver is installed and supports it.",
        kMajor, kMinor);

    QOpenGLContext context;
    context.setFormat(QSurfaceFormat::defaultFormat());
    if (!context.create()) {
        return common::makeError("Could not create an OpenGL context.\n\n" + requirement);
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface)) {
        return common::makeError("Could not activate an OpenGL context.\n\n" + requirement);
    }

    // Query the context itself: QOpenGLContext::format() echoes the requested format on some
    // platforms (e.g. Qt's "offscreen" platform), even when it created a legacy context.
    QOpenGLFunctions& gl = *context.functions();
    GLint major = 0;
    GLint minor = 0;
    GLint profileMask = 0;
    gl.glGetIntegerv(kGlMajorVersion, &major);
    gl.glGetIntegerv(kGlMinorVersion, &minor);
    gl.glGetIntegerv(kGlContextProfileMask, &profileMask);
    const bool core = (profileMask & kGlContextCoreProfileBit) != 0;
    const bool versionOk = major > kMajor || (major == kMajor && minor >= kMinor);
    const std::string details =
        std::format("Detected: {}\nRenderer: {} ({})", glString(gl, GL_VERSION),
                    glString(gl, GL_RENDERER), glString(gl, GL_VENDOR));
    context.doneCurrent();

    if (!versionOk || !core) {
        return common::makeError(std::format("OpenGL {}.{}{} is not supported.\n\n{}\n\n{}", major,
                                             minor, core ? " core" : " (not core profile)",
                                             requirement, details));
    }
    return {};
}

}  // namespace ds::app
