// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <QApplication>
#include <QSurfaceFormat>

#include <gtest/gtest.h>

#include "ds/graphics/renderer.hpp"

#ifndef DS_TEST_FORCE_OFFSCREEN
namespace {

bool hasDisplay() {
    return !qEnvironmentVariableIsEmpty("DISPLAY") ||
           !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
}

}  // namespace
#endif

// One QApplication for all tests, with the surface format main() requests.
int main(int argc, char* argv[]) {
#ifdef DS_TEST_FORCE_OFFSCREEN
    const bool offscreen = true;
#else
    const bool offscreen = !hasDisplay();  // GL tests then skip themselves.
#endif
    if (offscreen && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(ds::graphics::Renderer::kRequiredMajor,
                      ds::graphics::Renderer::kRequiredMinor);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);

    ::testing::InitGoogleTest(&argc, argv);
    const QApplication app(argc, argv);
    return RUN_ALL_TESTS();
}
