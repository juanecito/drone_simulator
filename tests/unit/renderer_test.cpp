// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/renderer.hpp"

#include <gtest/gtest.h>

namespace {

using ds::graphics::GlProcAddress;
using ds::graphics::Renderer;

GlProcAddress resolveNothing(void* /*user*/, const char* /*name*/) { return nullptr; }

TEST(Renderer, RequiresOpenGl45) {
    EXPECT_EQ(Renderer::kRequiredMajor, 4);
    EXPECT_EQ(Renderer::kRequiredMinor, 5);
}

TEST(Renderer, StartsUninitialised) {
    const Renderer renderer;
    EXPECT_FALSE(renderer.isInitialized());
}

TEST(Renderer, NullLoaderIsAnError) {
    Renderer renderer;
    const auto result = renderer.initialize(nullptr, nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().message.empty());
    EXPECT_FALSE(renderer.isInitialized());
}

TEST(Renderer, LoaderWithoutFunctionsIsAnError) {
    Renderer renderer;
    const auto result = renderer.initialize(&resolveNothing, nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("OpenGL"), std::string::npos);
    EXPECT_FALSE(renderer.isInitialized());
}

TEST(Renderer, RenderFrameWithoutContextIsANoOp) {
    Renderer renderer;
    renderer.renderFrame({.framebuffer = 0, .width = 640, .height = 480});  // Must not crash.
    SUCCEED();
}

TEST(Renderer, ClearColorCanBeChanged) {
    Renderer renderer;
    renderer.setClearColor({.r = 0.1F, .g = 0.2F, .b = 0.3F, .a = 1.0F});
    EXPECT_FLOAT_EQ(renderer.clearColor().r, 0.1F);
    EXPECT_FLOAT_EQ(renderer.clearColor().g, 0.2F);
    EXPECT_FLOAT_EQ(renderer.clearColor().b, 0.3F);
    EXPECT_FLOAT_EQ(renderer.clearColor().a, 1.0F);
}

}  // namespace
