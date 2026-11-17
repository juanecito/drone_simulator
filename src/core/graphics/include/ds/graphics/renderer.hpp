// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/graphics/camera.hpp"
#include "ds/graphics/exposure.hpp"
#include "ds/graphics/mesh.hpp"
#include "ds/graphics/sky_model.hpp"
#include "ds/terrain/chunk.hpp"
#include "ds/terrain/height_source.hpp"

/// @file
/// @brief OpenGL 4.5 core renderer (SPECIFICATION §4.4): terrain, water, atmosphere, sun, moon,
/// stars, HDR with auto exposure and tone mapping.
/// @ingroup graphics

namespace ds::graphics {

/// @brief Address of an OpenGL function, as returned by the platform loader.
using GlProcAddress = void (*)();

/// @brief Resolves OpenGL function names for the current context.
/// @details Called with the opaque @p user pointer given to Renderer::initialize(). The UI layer
/// supplies it (e.g. wrapping `QOpenGLContext::getProcAddress`), so this module never depends
/// on Qt (SPECIFICATION §3.1).
using GlProcLoader = GlProcAddress (*)(void* user, const char* name);

/// @brief Information about the OpenGL context the renderer was initialised with.
struct GlInfo {
    int major = 0;              ///< Context major version.
    int minor = 0;              ///< Context minor version.
    bool coreProfile = false;   ///< True for a core-profile context.
    bool debugContext = false;  ///< True if the context was created with the debug flag.
    std::string vendor;         ///< `GL_VENDOR`.
    std::string renderer;       ///< `GL_RENDERER`.
    std::string version;        ///< `GL_VERSION`.
};

/// @brief Where to draw a frame.
struct FrameTarget {
    std::uint32_t framebuffer = 0;  ///< OpenGL framebuffer object name (0 = default framebuffer).
    int width = 0;                  ///< Width in device pixels.
    int height = 0;                 ///< Height in device pixels.
};

/// @brief Linear RGBA colour.
struct Color {
    float r = 0.0F;  ///< Red, 0–1.
    float g = 0.0F;  ///< Green, 0–1.
    float b = 0.0F;  ///< Blue, 0–1.
    float a = 1.0F;  ///< Alpha, 0–1.
};

/// @brief Files the renderer loads in loadResources().
struct RendererResources {
    std::filesystem::path shaderDirectory;  ///< `assets/shaders` (or the installed copy).
    /// Moon albedo map (`moon_albedo.pgm`); if empty or unreadable the moon is untextured.
    std::filesystem::path moonTexture;
};

/// @brief Quality settings (Settings tab → Graphics).
struct RenderSettings {
    int msaaSamples = 4;           ///< 0, 2, 4 or 8 (FR-GFX-15).
    float starPointSizePx = 3.0F;  ///< Size of the star sprites.
    /// Extra size of the water plane around the camera when it is not bounded by the terrain.
    double waterRadiusM = 40'000.0;
    bool shadows = true;       ///< Cascaded shadow maps (FR-GFX-10).
    int shadowMapSize = 2048;  ///< Texels per cascade side (512–4096).
    /// @brief Equality.
    bool operator==(const RenderSettings&) const = default;
};

/// @brief Terrain to draw this frame.
struct TerrainView {
    /// Chunk meshes selected by the streamer (TerrainStreamer::update()).
    std::span<const std::shared_ptr<const terrain::ChunkMesh>> chunks;
    int gridSize = 129;                     ///< Vertices per chunk side (ChunkLayout::gridSize).
    std::optional<terrain::Extent> extent;  ///< Bounded terrain, or unbounded.
    std::optional<double> waterLevelM;      ///< World height of the water surface, if any.
    bool waterBeyondExtent = false;         ///< Water continues outside a bounded extent.
};

/// @brief Everything needed to draw one frame.
struct FrameInput {
    Camera camera;                 ///< Camera (world position in double).
    double originAltitudeM = 0.0;  ///< Altitude AMSL of world height 0.
    SkyState sky;                  ///< Sun, moon and sidereal rotation.
    /// Stars (apparent equatorial); re-uploaded when @c starsVersion changes.
    const std::vector<StarInstance>* stars = nullptr;
    std::uint64_t starsVersion = 0;  ///< SkyModel::starsVersion().
    TerrainView terrain;             ///< Terrain and water.
    /// Objects (vehicles, later items); meshes are uploaded once and cached by address.
    std::span<const DrawItem> objects;
    ExposureSettings exposure;      ///< Exposure mode.
    double visibilityM = 60'000.0;  ///< Meteorological visibility (aerosol density, FR-GFX-09).
    /// Background night-sky luminance (airglow + unresolved stars), cd/m² (moonless night
    /// ≈ 0.001 lux on the ground, FR-GFX-08c).
    double nightSkyLuminance = 2.5e-4;
    double dtSeconds = 0.0;    ///< Time since the previous frame (exposure adaptation).
    double timeSeconds = 0.0;  ///< Animation time (water waves).
    /// Jump straight to the exposure suited to the scene (first frame, large time jumps).
    bool resetExposure = false;
};

/// @brief Counters of the last frame (FR-GFX-17).
struct FrameStats {
    int drawCalls = 0;            ///< Draw and dispatch calls.
    int chunksDrawn = 0;          ///< Terrain chunks drawn.
    int chunksCulled = 0;         ///< Chunks outside the view frustum.
    int objectsDrawn = 0;         ///< Object draw items drawn.
    int shadowCascades = 0;       ///< Shadow cascades rendered (0 when no light casts).
    std::uint64_t triangles = 0;  ///< Triangles submitted.
    std::size_t gpuChunks = 0;    ///< Chunk meshes resident on the GPU.
};

/// @brief State of the automatic exposure (read back from the GPU).
struct ExposureReadout {
    double ev100 = 0.0;             ///< Current EV100.
    double averageLuminance = 0.0;  ///< Measured average scene luminance, cd/m².
};

/// @brief Top-level renderer.
/// @details All methods must be called on the thread that owns the current OpenGL context,
/// including the destructor (it releases GL objects).
class Renderer {
public:
    /// @brief Minimum supported OpenGL version (FR-GFX-01).
    static constexpr int kRequiredMajor = 4;
    /// @brief Minimum supported OpenGL minor version (FR-GFX-01).
    static constexpr int kRequiredMinor = 5;

    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    /// @brief Loads OpenGL entry points and checks the context (4.5 core or newer).
    /// @details Called again for a new context (the UI may replace the context, e.g. after the
    /// graphics device was lost): resources still held from the previous context are abandoned
    /// without deleting their objects, as they died with that context. Call releaseResources()
    /// while the old context is still current to free them properly.
    /// @param loader Function resolving GL symbols for the current context; must not be null.
    /// @param user   Opaque pointer passed back to @p loader.
    /// @return Context information, or an error with a user-readable message if loading fails,
    ///         the version is below 4.5 or the context is not a core profile.
    [[nodiscard]] common::Result<GlInfo> initialize(GlProcLoader loader, void* user);

    /// @brief True after a successful initialize().
    [[nodiscard]] bool isInitialized() const noexcept { return initialized_; }

    /// @brief Compiles the shaders and creates the GPU resources for scene rendering.
    /// @details Resources loaded earlier (in the same context) are released first.
    /// @return An error naming the file and line of a shader that does not compile.
    [[nodiscard]] common::Result<void> loadResources(const RendererResources& resources);

    /// @brief True after a successful loadResources().
    [[nodiscard]] bool hasResources() const noexcept;

    /// @brief Deletes the GPU resources; the context they were created in must be current.
    /// @details For a context that is about to be destroyed: afterwards the renderer needs
    /// initialize() and loadResources() in a new context before it draws the scene again.
    void releaseResources() noexcept;

    /// @brief Rebuilds shader programs whose files changed (hot reload, FR-GFX-04).
    /// @return True if a program was rebuilt; an error (old program kept) if one failed.
    [[nodiscard]] common::Result<bool> reloadChangedShaders();

    /// @brief Changes the quality settings.
    void setSettings(const RenderSettings& settings);
    /// @brief Current quality settings.
    [[nodiscard]] const RenderSettings& settings() const noexcept { return settings_; }

    /// @brief Colour used by the clear-only renderFrame() overload.
    void setClearColor(const Color& color) noexcept { clearColor_ = color; }
    /// @brief Current clear colour.
    [[nodiscard]] const Color& clearColor() const noexcept { return clearColor_; }

    /// @brief Clears @p target to the clear colour (no scene). Does nothing if the renderer is
    /// not initialised. Depth is cleared to 0 (reversed-Z, FR-GFX-06).
    void renderFrame(const FrameTarget& target);

    /// @brief Draws the scene into @p target. Falls back to the clear-only overload without
    /// resources.
    void renderFrame(const FrameTarget& target, const FrameInput& input);

    /// @brief Counters of the last scene frame.
    [[nodiscard]] const FrameStats& lastStats() const noexcept { return stats_; }

    /// @brief Reads the auto-exposure state back from the GPU (synchronises; call rarely).
    [[nodiscard]] ExposureReadout readExposure() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool initialized_ = false;
    Color clearColor_{0.45F, 0.62F, 0.85F, 1.0F};
    RenderSettings settings_;
    FrameStats stats_;
};

/// @brief Rough EV100 for a sky state (used to start the automatic exposure close to its
/// final value): from the horizontal illuminance of the Sun and the Moon on average ground.
[[nodiscard]] double estimateEv100(const SkyState& sky, double compensationEv = 0.0) noexcept;

}  // namespace ds::graphics
