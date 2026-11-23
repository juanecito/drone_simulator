// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/renderer.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <string_view>
#include <unordered_map>

#include "ds/common/log.hpp"
#include "ds/common/math.hpp"
#include "ds/graphics/image.hpp"
#include "ds/graphics/shadow_cascades.hpp"

#include "gl_objects.hpp"
#include "ortho_textures.hpp"
#include "shader_library.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace ds::graphics {
namespace {

std::string glString(GLenum name) {
    const GLubyte* value = glGetString(name);
    return value != nullptr ? std::string(reinterpret_cast<const char*>(value)) : std::string{};
}

std::string_view debugSourceName(GLenum source) {
    switch (source) {
        case GL_DEBUG_SOURCE_API: return "api";
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM: return "window-system";
        case GL_DEBUG_SOURCE_SHADER_COMPILER: return "shader-compiler";
        case GL_DEBUG_SOURCE_THIRD_PARTY: return "third-party";
        case GL_DEBUG_SOURCE_APPLICATION: return "application";
        default: return "other";
    }
}

// Routes GL_KHR_debug messages to the application logger (FR-GFX-16).
void GLAD_API_PTR onGlDebugMessage(GLenum source, GLenum /*type*/, GLuint id, GLenum severity,
                                   GLsizei length, const GLchar* message,
                                   const void* /*userParam*/) {
    const std::string_view text = length >= 0
                                      ? std::string_view(message, static_cast<std::size_t>(length))
                                      : std::string_view(message);
    log::Level level = log::Level::Trace;
    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH: level = log::Level::Error; break;
        case GL_DEBUG_SEVERITY_MEDIUM: level = log::Level::Warn; break;
        case GL_DEBUG_SEVERITY_LOW: level = log::Level::Debug; break;
        default: level = log::Level::Trace; break;  // GL_DEBUG_SEVERITY_NOTIFICATION
    }
    log::log(level, "[gl:{}:{}] {}", debugSourceName(source), id, text);
}

}  // namespace

common::Result<GlInfo> Renderer::initialize(GlProcLoader loader, void* user) {
    initialized_ = false;
    if (impl_ != nullptr) {
        // Left over from a previous context that was destroyed without releaseResources().
        // Its objects died with that context; deleting them now would delete whatever objects
        // of the current context have the same names, so the wrappers are leaked on purpose.
        log::warn("OpenGL resources of a previous context were not released; abandoning them");
        [[maybe_unused]] Impl* abandoned = impl_.release();  // NOLINT: deliberate leak.
    }
    if (loader == nullptr) {
        return common::makeError("No OpenGL function loader was provided.");
    }

    // GlProcLoader has exactly the signature of glad's GLADuserptrloadfunc.
    const int loaded = gladLoadGLUserPtr(loader, user);
    if (loaded == 0) {
        return common::makeError(
            "Could not load the OpenGL functions. Make sure an OpenGL context is current and the "
            "graphics driver is installed correctly.");
    }

    GlInfo info;
    glGetIntegerv(GL_MAJOR_VERSION, &info.major);
    glGetIntegerv(GL_MINOR_VERSION, &info.minor);
    GLint profileMask = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profileMask);
    GLint contextFlags = 0;
    glGetIntegerv(GL_CONTEXT_FLAGS, &contextFlags);
    info.coreProfile = (profileMask & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
    info.debugContext = (contextFlags & GL_CONTEXT_FLAG_DEBUG_BIT) != 0;
    info.vendor = glString(GL_VENDOR);
    info.renderer = glString(GL_RENDERER);
    info.version = glString(GL_VERSION);

    const bool versionOk = info.major > kRequiredMajor ||
                           (info.major == kRequiredMajor && info.minor >= kRequiredMinor);
    if (!versionOk) {
        return common::makeError(std::format(
            "OpenGL {}.{} is not supported. Drone Simulator needs OpenGL {}.{} (core profile) or "
            "newer.\n\nDetected: {}\nRenderer: {} ({})",
            info.major, info.minor, kRequiredMajor, kRequiredMinor, info.version, info.renderer,
            info.vendor));
    }
    if (!info.coreProfile) {
        return common::makeError(std::format(
            "The OpenGL context is not a core profile. Drone Simulator needs an OpenGL {}.{} core "
            "profile context.\n\nDetected: {}\nRenderer: {} ({})",
            kRequiredMajor, kRequiredMinor, info.version, info.renderer, info.vendor));
    }

    if (info.debugContext) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(onGlDebugMessage, nullptr);
        log::debug("OpenGL debug output enabled");
    }

    log::info("OpenGL {}.{} core — {} ({})", info.major, info.minor, info.renderer, info.vendor);
    initialized_ = true;
    return info;
}

void Renderer::renderFrame(const FrameTarget& target) {
    if (!initialized_ || target.width <= 0 || target.height <= 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(0, 0, target.width, target.height);

    const std::array<GLfloat, 4> color{clearColor_.r, clearColor_.g, clearColor_.b, clearColor_.a};
    glClearNamedFramebufferfv(target.framebuffer, GL_COLOR, 0, color.data());
    // Reversed-Z: the far plane is depth 0 (FR-GFX-06).
    glClearNamedFramebufferfi(target.framebuffer, GL_DEPTH_STENCIL, 0, 0.0F, 0);
}

// --- Scene rendering ---------------------------------------------------------------------

namespace {

// std140 mirror of FrameUniforms in assets/shaders/common.glsl.
struct FrameUniforms {
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::mat4 equatorialToWorld;
    glm::vec4 sunDirLux;
    glm::vec4 sunColorRadius;
    glm::vec4 moonDirLux;
    glm::vec4 moonColorRadius;
    glm::vec4 moonToSunDisc;
    glm::vec4 moonNorth;
    glm::vec4 viewport;
    glm::vec4 camera;
    glm::vec4 misc;
    glm::vec4 water;
};
static_assert(sizeof(FrameUniforms) == 3 * 64 + 10 * 16);

// std140 mirror of AtmosphereUniforms in assets/shaders/atmosphere.glsl.
struct AtmosphereUniforms {
    glm::vec4 rayleigh;
    glm::vec4 mie;
    glm::vec4 mieExt;
    glm::vec4 ozone;
    glm::vec4 radii;
    glm::vec4 ground;
};

// std140 mirror of ShadowUniforms in assets/shaders/shadow.glsl.
struct ShadowUniforms {
    std::array<glm::mat4, kShadowCascades> matrix;
    glm::vec4 far;
    glm::vec4 texel;
    glm::vec4 forward;
    glm::vec4 depthRange;
};
static_assert(sizeof(ShadowUniforms) == 3 * 64 + 4 * 16);

// std140 mirror of LightUniforms in assets/shaders/lighting.glsl.
struct LightUniforms {
    glm::vec4 count;
    std::array<glm::vec4, kMaxSpotLights> posRange;
    std::array<glm::vec4, kMaxSpotLights> dirCos;
    std::array<glm::vec4, kMaxSpotLights> colorCd;
    std::array<glm::vec4, kMaxSpotLights> cosInner;
};
static_assert(sizeof(LightUniforms) == 16 + 4 * kMaxSpotLights * 16);

// std140 mirror of WeatherUniforms in assets/shaders/weather.glsl.
struct WeatherUniforms {
    std::array<glm::vec4, weather::kMaxCloudLayers> layerHeights;
    std::array<glm::vec4, weather::kMaxCloudLayers> layerNoise;
    std::array<glm::vec4, weather::kMaxCloudLayers> layerLook;
    glm::vec4 light;
    glm::vec4 fog;
    glm::vec4 wind;
    glm::vec4 surface;
    glm::vec4 precip;
    glm::vec4 precipOffset;
    glm::vec4 precipVelocity;
};
static_assert(sizeof(WeatherUniforms) == 16 * 16);

// Shutter time of the rain streaks (the length the eye or a camera smears a drop over), s.
constexpr double kRainShutterS = 1.0 / 30.0;

float precipitationCode(weather::PrecipitationType type) noexcept {
    switch (type) {
        case weather::PrecipitationType::None: return 0.0F;
        case weather::PrecipitationType::Drizzle: return 1.0F;
        case weather::PrecipitationType::Rain: return 2.0F;
        case weather::PrecipitationType::Snow: return 3.0F;
        case weather::PrecipitationType::Sleet: return 4.0F;
    }
    return 0.0F;
}

// Weather uniforms for a camera at @p camera moving at @p cameraVelocity.
WeatherUniforms weatherUniforms(const WeatherView& w, const glm::dvec3& camera,
                                const glm::dvec3& cameraVelocity) {
    WeatherUniforms u{};
    for (std::size_t i = 0; i < w.layerCount; ++i) {
        const CloudLayerView& l = w.layers[i];
        u.layerHeights[i] = {static_cast<float>(l.baseY - camera.y),
                             static_cast<float>(l.topY - camera.y),
                             static_cast<float>(l.extinctionPerM), static_cast<float>(l.cover)};
        u.layerNoise[i] = {static_cast<float>(camera.x - l.driftM.x),
                           static_cast<float>(camera.z - l.driftM.y),
                           static_cast<float>(l.frequency), static_cast<float>(l.threshold)};
        u.layerLook[i] = {static_cast<float>(l.softness), static_cast<float>(l.type), 0.0F, 0.0F};
    }
    u.light = {static_cast<float>(w.skyFactor), static_cast<float>(w.beamDiffuse),
               static_cast<float>(w.clearFraction), static_cast<float>(w.layerCount)};
    u.fog = {static_cast<float>(w.fogTopY - camera.y), static_cast<float>(w.fogExtinctionPerM),
             static_cast<float>(w.aerosolVisibilityM), 0.0F};
    const double windSpeed = glm::length(glm::dvec2(w.windMs.x, w.windMs.z));
    const glm::dvec2 windDir =
        windSpeed > 1e-6 ? glm::dvec2(w.windMs.x, w.windMs.z) / windSpeed : glm::dvec2(0.0);
    u.wind = {static_cast<float>(windDir.x), static_cast<float>(windDir.y),
              static_cast<float>(windSpeed), 0.0F};
    u.surface = {static_cast<float>(w.wetness), static_cast<float>(w.snowDepthM),
                 static_cast<float>(w.lightningLux), 0.0F};
    u.precip = {precipitationCode(w.precipitation), static_cast<float>(w.rateMmH),
                static_cast<float>(kPrecipitationBoxM), static_cast<float>(w.particles)};
    // The particles move with the fall speed and the wind; wrapping the offset (in double)
    // keeps the floats small however far the camera goes and however long the session lasts.
    const glm::dvec3 velocity = w.windMs + glm::dvec3(0.0, -w.fallSpeedMs, 0.0);
    const glm::dvec3 offset = velocity * w.timeS - camera;
    const auto wrap = [](double x) {
        const double m = std::fmod(x, kPrecipitationBoxM);
        return static_cast<float>(m < 0.0 ? m + kPrecipitationBoxM : m);
    };
    u.precipOffset = {wrap(offset.x), wrap(offset.y), wrap(offset.z),
                      static_cast<float>(std::fmod(w.timeS, 3600.0))};
    u.precipVelocity =
        glm::vec4(glm::vec3(velocity - cameraVelocity), static_cast<float>(kRainShutterS));
    return u;
}

// Lowest elevation (sine) at which the Sun or the Moon casts shadows.
constexpr float kMinShadowLightY = 0.02F;

// std430 mirror of ExposureState (header only; the histogram follows).
struct ExposureHeader {
    float exposure;
    float ev100;
    float avgLuminance;
    float pad;
    glm::vec4 skyIrradiance;
    glm::vec4 groundIrradiance;
};
constexpr GLsizeiptr kExposureBufferSize = sizeof(ExposureHeader) + 256 * sizeof(std::uint32_t);

constexpr int kTransmittanceW = 256;
constexpr int kTransmittanceH = 64;
constexpr int kMultiScatterSize = 32;
constexpr int kSkyViewW = 192;
constexpr int kSkyViewH = 108;

// Koschmieder: visibility V ⇔ extinction 3.912 / V at ground level; Rayleigh contributes
// its share at 550 nm, aerosols (Mie) the rest (aerosolExtinctionPerKm()).
AtmosphereUniforms atmosphereFor(double visibilityM) {
    const auto mieExt = static_cast<float>(aerosolExtinctionPerKm(visibilityM));
    const float mieScat = mieExt * 0.9F;
    AtmosphereUniforms a{};
    a.rayleigh = {static_cast<float>(kRayleighExtinctionPerKm.x),
                  static_cast<float>(kRayleighExtinctionPerKm.y),
                  static_cast<float>(kRayleighExtinctionPerKm.z), 8.0F};
    a.mie = {mieScat, mieScat, mieScat, 1.2F};
    a.mieExt = {mieExt, mieExt, mieExt, 0.8F};
    a.ozone = {0.650e-3F, 1.881e-3F, 0.085e-3F, 25.0F};
    a.radii = {6360.0F, 6460.0F, 15.0F, 0.0F};
    a.ground = {0.15F, 0.15F, 0.15F, 0.0F};
    return a;
}

enum ProgramId : std::size_t {
    kTransmittance,
    kMultiScatter,
    kSkyView,
    kSkyIrradiance,
    kSky,
    kStars,
    kTerrain,
    kWater,
    kObject,
    kShadowTerrain,
    kShadowObject,
    kPrecipitation,
    kHistogram,
    kExposure,
    kTonemap,
};

struct GpuChunk {
    std::shared_ptr<const terrain::ChunkMesh> mesh;  // Keeps the key alive.
    gl::Buffer vertices;
    std::uint64_t lastFrame = 0;
};

constexpr std::uint64_t kChunkKeepFrames = 240;

struct GpuMesh {
    std::shared_ptr<const Mesh> mesh;  // Keeps the key alive.
    gl::Buffer vertices;
    gl::Buffer indices;
    GLsizei indexCount = 0;
    std::uint64_t lastFrame = 0;
};

// Camera-relative model matrix of a draw item (translation computed in double).
glm::mat4 modelMatrix(const DrawItem& item, const glm::dvec3& camera) {
    glm::dmat4 m = glm::mat4_cast(item.orientation);
    m[0] *= static_cast<double>(item.scale.x);
    m[1] *= static_cast<double>(item.scale.y);
    m[2] *= static_cast<double>(item.scale.z);
    m[3] = glm::dvec4(item.position - camera, 1.0);
    return glm::mat4(m);
}

// True if any corner of the camera-relative box may fall inside the cascade (xy within the
// square, not entirely beyond the far depth; nearer casters are depth-clamped).
bool boxInCascade(const glm::mat4& m, const glm::vec3& lo, const glm::vec3& hi) {
    glm::vec3 mn(1e30F);
    glm::vec3 mx(-1e30F);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 p((i & 1) != 0 ? hi.x : lo.x, (i & 2) != 0 ? hi.y : lo.y,
                          (i & 4) != 0 ? hi.z : lo.z);
        const glm::vec4 c = m * glm::vec4(p, 1.0F);
        mn = glm::min(mn, glm::vec3(c));
        mx = glm::max(mx, glm::vec3(c));
    }
    return mx.x >= -1.0F && mn.x <= 1.0F && mx.y >= -1.0F && mn.y <= 1.0F && mn.z <= 1.0F;
}

GLuint dispatchSize(int size, int local) { return static_cast<GLuint>((size + local - 1) / local); }

}  // namespace

double estimateEv100(const SkyState& sky, double compensationEv, double lightFactor) noexcept {
    const auto horizontal = [](const SkyLight& light) {
        const double s = std::sin(common::degToRad(light.elevationDeg));
        // Direct light (crudely attenuated) plus diffuse sky light.
        return s > 0.0 ? static_cast<double>(light.illuminanceLux) * s * (0.55 + 0.25 * s) : 0.0;
    };
    double illuminance = horizontal(sky.sun) + horizontal(sky.moon) + 1e-3;
    if (sky.sun.elevationDeg <= 0.0 && sky.sun.elevationDeg > -18.0) {
        // Twilight: sky light falls by about a decade per 2.5° of solar depression.
        illuminance += 400.0 * std::pow(10.0, sky.sun.elevationDeg / 2.5);
    }
    illuminance *= std::clamp(lightFactor, 1e-3, 1.0);
    // Average ground reflectance 0.15 (Lambertian), plus a bright sky in part of the view.
    const double luminance = illuminance * 0.15 / common::kPi * 1.5;
    return autoExposureEv100(luminance, compensationEv);
}

// Offscreen HDR targets of one view (multisampled scene, resolved colour).
struct HdrTargets {
    int width = 0;
    int height = 0;
    int samples = -1;
    gl::Texture sceneColor;  // Multisampled (or the resolved texture when samples == 0).
    gl::Texture sceneDepth;
    gl::Texture resolved;
    gl::Framebuffer sceneFbo;
    gl::Framebuffer resolveFbo;

    void create(int w, int h, int msaa);
};

void HdrTargets::create(int w, int h, int msaa) {
    if (w == width && h == height && msaa == samples) {
        return;
    }
    width = w;
    height = h;
    samples = msaa;
    resolved = gl::createTexture2D(GL_RGBA16F, w, h);
    glTextureParameteri(resolved.id(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(resolved.id(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    sceneFbo = gl::createFramebuffer();
    if (msaa > 0) {
        sceneColor = gl::createTexture2DMultisample(GL_RGBA16F, msaa, w, h);
        sceneDepth = gl::createTexture2DMultisample(GL_DEPTH_COMPONENT32F, msaa, w, h);
        glNamedFramebufferTexture(sceneFbo.id(), GL_COLOR_ATTACHMENT0, sceneColor.id(), 0);
        resolveFbo = gl::createFramebuffer();
        glNamedFramebufferTexture(resolveFbo.id(), GL_COLOR_ATTACHMENT0, resolved.id(), 0);
    } else {
        sceneColor.reset();
        resolveFbo.reset();
        sceneDepth = gl::createTexture2D(GL_DEPTH_COMPONENT32F, w, h);
        glNamedFramebufferTexture(sceneFbo.id(), GL_COLOR_ATTACHMENT0, resolved.id(), 0);
    }
    glNamedFramebufferTexture(sceneFbo.id(), GL_DEPTH_ATTACHMENT, sceneDepth.id(), 0);
    if (glCheckNamedFramebufferStatus(sceneFbo.id(), GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log::error("HDR framebuffer incomplete ({}×{}, {} samples)", w, h, msaa);
    }
}

struct Renderer::Impl {
    explicit Impl(std::filesystem::path shaderDir) : shaders(std::move(shaderDir)) {}

    // UBO -> Uniform Buffer Object
    // VAO -> Vertex Array Object
    // TEX -> Texture

    ShaderLibrary shaders;
    gl::Buffer frameUbo;
    gl::Buffer atmosphereUbo;
    gl::Buffer exposureSsbo;
    gl::Buffer starsSsbo;
    GLsizei starCount = 0;
    std::uint64_t starsVersion = ~std::uint64_t{0};
    gl::VertexArray emptyVao;
    gl::VertexArray terrainVao;
    gl::Buffer chunkIndices;
    GLsizei chunkIndexCount = 0;
    int chunkGridSize = 0;

    gl::Texture transmittance;
    gl::Texture multiScatter;
    gl::Texture skyView;
    gl::Texture moonAlbedo;

    // HDR targets of the main view and of the insets (drawn one after the other).
    HdrTargets mainTargets;
    HdrTargets insetTargets;

    std::optional<double> atmosphereVisibility;
    bool exposureReset = true;

    std::unordered_map<const terrain::ChunkMesh*, GpuChunk> chunks;
    std::uint64_t frame = 0;
    detail::OrthoTextures ortho;  // Orthophoto tiles (FR-TER-I06).

    // Objects.
    gl::VertexArray objectVao;
    std::unordered_map<const Mesh*, GpuMesh> meshes;

    // Shadows.
    gl::Buffer shadowUbo;
    gl::Buffer lightUbo;
    gl::Buffer weatherUbo;
    gl::Texture shadowMap;
    std::array<gl::Framebuffer, kShadowCascades> shadowFbos;
    int shadowSize = 0;

    GpuMesh& meshFor(const std::shared_ptr<const Mesh>& mesh);
    void ensureShadowMap(int size);

    void updateAtmosphere(double visibilityM, FrameStats& stats);
    void uploadStars(const FrameInput& input);
    GpuChunk& chunkFor(const std::shared_ptr<const terrain::ChunkMesh>& mesh);
    void ensureChunkIndices(int gridSize);
    void evictChunks();
};

void Renderer::Impl::updateAtmosphere(double visibilityM, FrameStats& stats) {
    // The LUTs are rebuilt only when the visibility changes by more than 0.5 % (weather
    // transitions change it a little every frame).
    if (atmosphereVisibility &&
        std::abs(*atmosphereVisibility - visibilityM) < 0.005 * visibilityM + 1.0) {
        return;
    }
    atmosphereVisibility = visibilityM;
    const AtmosphereUniforms a = atmosphereFor(visibilityM);
    glNamedBufferSubData(atmosphereUbo.id(), 0, sizeof(a), &a);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, atmosphereUbo.id());

    glUseProgram(shaders.program(kTransmittance));
    glBindImageTexture(0, transmittance.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glDispatchCompute(dispatchSize(kTransmittanceW, 8), dispatchSize(kTransmittanceH, 8), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glBindTextureUnit(0, transmittance.id());
    glUseProgram(shaders.program(kMultiScatter));
    glBindImageTexture(0, multiScatter.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glDispatchCompute(kMultiScatterSize, kMultiScatterSize, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    stats.drawCalls += 2;
}

void Renderer::Impl::uploadStars(const FrameInput& input) {
    if (input.stars == nullptr || input.stars->empty()) {
        starCount = 0;
        return;
    }
    if (input.starsVersion == starsVersion && starsSsbo) {
        return;
    }
    const auto bytes = static_cast<GLsizeiptr>(input.stars->size() * sizeof(StarInstance));
    starsSsbo = gl::createBuffer(bytes, input.stars->data(), 0);
    starCount = static_cast<GLsizei>(input.stars->size());
    starsVersion = input.starsVersion;
}

void Renderer::Impl::ensureChunkIndices(int gridSize) {
    if (gridSize == chunkGridSize) {
        return;
    }
    const std::vector<std::uint16_t> indices = terrain::buildChunkIndices(gridSize);
    chunkIndices = gl::createBuffer(static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint16_t)),
                                    indices.data(), 0);
    chunkIndexCount = static_cast<GLsizei>(indices.size());
    chunkGridSize = gridSize;
    glVertexArrayElementBuffer(terrainVao.id(), chunkIndices.id());
    chunks.clear();  // Vertex counts depend on the grid size.
}

GpuChunk& Renderer::Impl::chunkFor(const std::shared_ptr<const terrain::ChunkMesh>& mesh) {
    auto [it, inserted] = chunks.try_emplace(mesh.get());
    if (inserted) {
        it->second.mesh = mesh;
        it->second.vertices = gl::createBuffer(
            static_cast<GLsizeiptr>(mesh->vertices.size() * sizeof(terrain::TerrainVertex)),
            mesh->vertices.data(), 0);
    }
    it->second.lastFrame = frame;
    return it->second;
}

GpuMesh& Renderer::Impl::meshFor(const std::shared_ptr<const Mesh>& mesh) {
    auto [it, inserted] = meshes.try_emplace(mesh.get());
    if (inserted) {
        it->second.mesh = mesh;
        it->second.vertices =
            gl::createBuffer(static_cast<GLsizeiptr>(mesh->vertices.size() * sizeof(MeshVertex)),
                             mesh->vertices.data(), 0);
        it->second.indices =
            gl::createBuffer(static_cast<GLsizeiptr>(mesh->indices.size() * sizeof(std::uint32_t)),
                             mesh->indices.data(), 0);
        it->second.indexCount = static_cast<GLsizei>(mesh->indices.size());
    }
    it->second.lastFrame = frame;
    return it->second;
}

void Renderer::Impl::ensureShadowMap(int size) {
    if (size == shadowSize && shadowMap) {
        return;
    }
    shadowSize = size;
    shadowMap = gl::createShadowArray(size, kShadowCascades);
    for (int i = 0; i < kShadowCascades; ++i) {
        auto& fbo = shadowFbos[static_cast<std::size_t>(i)];
        fbo = gl::createFramebuffer();
        glNamedFramebufferTextureLayer(fbo.id(), GL_DEPTH_ATTACHMENT, shadowMap.id(), 0, i);
        glNamedFramebufferDrawBuffer(fbo.id(), GL_NONE);
        glNamedFramebufferReadBuffer(fbo.id(), GL_NONE);
        if (glCheckNamedFramebufferStatus(fbo.id(), GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log::error("Shadow framebuffer {} incomplete", i);
        }
    }
}

void Renderer::Impl::evictChunks() {
    // Meshes not drawn for a while are released; the streamer's cache can rebuild them.
    std::erase_if(chunks, [this](const auto& entry) {
        return frame - entry.second.lastFrame > kChunkKeepFrames;
    });
    std::erase_if(meshes, [this](const auto& entry) {
        return frame - entry.second.lastFrame > kChunkKeepFrames;
    });
}

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::hasResources() const noexcept { return impl_ != nullptr; }

void Renderer::releaseResources() noexcept {
    impl_.reset();
    initialized_ = false;
}

common::Result<void> Renderer::loadResources(const RendererResources& resources) {
    if (!initialized_) {
        return common::makeError("The renderer is not initialised.");
    }
    // The old objects go first: their names may be reused by the new ones.
    impl_.reset();
    auto impl = std::make_unique<Impl>(resources.shaderDirectory);
    auto& lib = impl->shaders;
    const auto compute = [](const char* file) {
        return std::vector<ShaderLibrary::Stage>{{GL_COMPUTE_SHADER, file}};
    };
    const auto graphics = [](const char* vert, const char* frag) {
        return std::vector<ShaderLibrary::Stage>{{GL_VERTEX_SHADER, vert},
                                                 {GL_FRAGMENT_SHADER, frag}};
    };
    lib.add("transmittance", compute("transmittance.comp"));
    lib.add("multiscatter", compute("multiscatter.comp"));
    lib.add("skyview", compute("skyview.comp"));
    lib.add("sky_irradiance", compute("sky_irradiance.comp"));
    lib.add("sky", graphics("fullscreen.vert", "sky.frag"));
    lib.add("stars", graphics("stars.vert", "stars.frag"));
    lib.add("terrain", graphics("terrain.vert", "terrain.frag"));
    lib.add("water", graphics("water.vert", "water.frag"));
    lib.add("object", graphics("object.vert", "object.frag"));
    lib.add("shadow_terrain", graphics("shadow_terrain.vert", "shadow.frag"));
    lib.add("shadow_object", graphics("shadow_object.vert", "shadow.frag"));
    lib.add("precipitation", graphics("precipitation.vert", "precipitation.frag"));
    lib.add("histogram", compute("histogram.comp"));
    lib.add("exposure", compute("exposure.comp"));
    lib.add("tonemap", graphics("fullscreen.vert", "tonemap.frag"));
    if (auto built = lib.buildAll(); !built) {
        return built;
    }

    impl->frameUbo = gl::createBuffer(sizeof(FrameUniforms), nullptr, GL_DYNAMIC_STORAGE_BIT);
    impl->atmosphereUbo =
        gl::createBuffer(sizeof(AtmosphereUniforms), nullptr, GL_DYNAMIC_STORAGE_BIT);
    std::vector<std::byte> exposureInit(kExposureBufferSize, std::byte{0});
    ExposureHeader header{};
    header.ev100 = 12.0F;
    header.exposure = static_cast<float>(exposureFromEv100(12.0));
    std::memcpy(exposureInit.data(), &header, sizeof(header));
    impl->exposureSsbo =
        gl::createBuffer(kExposureBufferSize, exposureInit.data(), GL_DYNAMIC_STORAGE_BIT);
    impl->emptyVao = gl::createVertexArray();

    // TerrainVertex: float height; u16 u, v; snorm16×2 normal; unorm8×4 material.
    impl->terrainVao = gl::createVertexArray();
    const GLuint vao = impl->terrainVao.id();
    glEnableVertexArrayAttrib(vao, 0);
    glVertexArrayAttribFormat(vao, 0, 1, GL_FLOAT, GL_FALSE, 0);
    glEnableVertexArrayAttrib(vao, 1);
    glVertexArrayAttribFormat(vao, 1, 2, GL_UNSIGNED_SHORT, GL_FALSE, 4);
    glEnableVertexArrayAttrib(vao, 2);
    glVertexArrayAttribFormat(vao, 2, 2, GL_SHORT, GL_TRUE, 8);
    glEnableVertexArrayAttrib(vao, 3);
    glVertexArrayAttribFormat(vao, 3, 4, GL_UNSIGNED_BYTE, GL_TRUE, 12);
    for (GLuint a = 0; a < 4; ++a) {
        glVertexArrayAttribBinding(vao, a, 0);
    }
    static_assert(sizeof(terrain::TerrainVertex) == 16);

    // MeshVertex: vec3 position; vec3 normal; vec4 colour; float emissive.
    impl->objectVao = gl::createVertexArray();
    {
        const GLuint ovao = impl->objectVao.id();
        glEnableVertexArrayAttrib(ovao, 0);
        glVertexArrayAttribFormat(ovao, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glEnableVertexArrayAttrib(ovao, 1);
        glVertexArrayAttribFormat(ovao, 1, 3, GL_FLOAT, GL_FALSE, 12);
        glEnableVertexArrayAttrib(ovao, 2);
        glVertexArrayAttribFormat(ovao, 2, 4, GL_FLOAT, GL_FALSE, 24);
        glEnableVertexArrayAttrib(ovao, 3);
        glVertexArrayAttribFormat(ovao, 3, 1, GL_FLOAT, GL_FALSE, 40);
        for (GLuint a = 0; a < 4; ++a) {
            glVertexArrayAttribBinding(ovao, a, 0);
        }
    }
    impl->shadowUbo = gl::createBuffer(sizeof(ShadowUniforms), nullptr, GL_DYNAMIC_STORAGE_BIT);
    impl->lightUbo = gl::createBuffer(sizeof(LightUniforms), nullptr, GL_DYNAMIC_STORAGE_BIT);
    impl->weatherUbo = gl::createBuffer(sizeof(WeatherUniforms), nullptr, GL_DYNAMIC_STORAGE_BIT);
    impl->ensureShadowMap(1);  // A valid (lit) map until shadows are first rendered.

    impl->transmittance = gl::createTexture2D(GL_RGBA32F, kTransmittanceW, kTransmittanceH);
    impl->multiScatter = gl::createTexture2D(GL_RGBA32F, kMultiScatterSize, kMultiScatterSize);
    impl->skyView = gl::createTexture2D(GL_RGBA32F, kSkyViewW, kSkyViewH);
    glTextureParameteri(impl->skyView.id(), GL_TEXTURE_WRAP_S, GL_REPEAT);  // Azimuth wraps.

    // Moon albedo: sRGB luma → linear, normalised to a mean of 1.
    std::vector<float> albedo{1.0F};
    int moonW = 1;
    int moonH = 1;
    if (!resources.moonTexture.empty()) {
        if (auto image = loadPgm(resources.moonTexture)) {
            moonW = image->width;
            moonH = image->height;
            albedo.resize(image->pixels.size());
            double sum = 0.0;
            for (std::size_t i = 0; i < albedo.size(); ++i) {
                const double c = image->pixels[i] / 255.0;
                const double lin = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
                albedo[i] = static_cast<float>(lin);
                sum += lin;
            }
            const auto mean = static_cast<float>(sum / static_cast<double>(albedo.size()));
            for (float& a : albedo) {
                a /= std::max(mean, 1e-6F);
            }
        } else {
            log::warn("Moon texture not loaded: {}", image.error().message);
        }
    }
    const int levels = 1 + static_cast<int>(std::floor(std::log2(std::max(moonW, moonH))));
    impl->moonAlbedo = gl::createTexture2D(GL_R16F, moonW, moonH, levels);
    glTextureSubImage2D(impl->moonAlbedo.id(), 0, 0, 0, moonW, moonH, GL_RED, GL_FLOAT,
                        albedo.data());
    glGenerateTextureMipmap(impl->moonAlbedo.id());
    glTextureParameteri(impl->moonAlbedo.id(), GL_TEXTURE_WRAP_S, GL_REPEAT);

    impl_ = std::move(impl);
    log::info("Renderer resources loaded from {}", resources.shaderDirectory.string());
    return {};
}

common::Result<bool> Renderer::reloadChangedShaders() {
    if (impl_ == nullptr) {
        return false;
    }
    auto result = impl_->shaders.reloadChanged();
    if (result && *result) {
        impl_->atmosphereVisibility.reset();  // LUT shaders may have changed.
    }
    return result;
}

void Renderer::setSettings(const RenderSettings& settings) {
    settings_ = settings;
    settings_.msaaSamples = std::clamp(settings.msaaSamples, 0, 8);
    settings_.shadowMapSize = std::clamp(settings.shadowMapSize, 512, 4096);
    if (settings_.msaaSamples == 1) {
        settings_.msaaSamples = 0;
    }
}

ExposureReadout Renderer::readExposure() const {
    if (impl_ == nullptr) {
        return {};
    }
    ExposureHeader header{};
    glGetNamedBufferSubData(impl_->exposureSsbo.id(), 0, sizeof(header), &header);
    return {.ev100 = header.ev100, .averageLuminance = header.avgLuminance};
}

void Renderer::renderFrame(const FrameTarget& target, const FrameInput& input) {
    if (!initialized_ || impl_ == nullptr) {
        renderFrame(target);
        return;
    }
    if (target.width <= 0 || target.height <= 0) {
        return;
    }
    ++(impl_->frame);
    stats_ = {};
    renderView(target, {.x = 0, .y = 0, .width = target.width, .height = target.height}, input,
               true);
}

void Renderer::renderInset(const FrameTarget& target, const ViewRect& rect,
                           const FrameInput& input) {
    if (!initialized_ || impl_ == nullptr) {
        return;
    }
    // Clip the rectangle to the target.
    const int x0 = std::clamp(rect.x, 0, target.width);
    const int y0 = std::clamp(rect.y, 0, target.height);
    const int x1 = std::clamp(rect.x + rect.width, 0, target.width);
    const int y1 = std::clamp(rect.y + rect.height, 0, target.height);
    if (x1 - x0 < 2 || y1 - y0 < 2) {
        return;
    }
    renderView(target, {.x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0}, input, false);
}

void Renderer::renderLocalSpotLights(const FrameInput& input, const Camera& camera,
                                    bool mainView)
{
    Impl& r = *impl_;
    LightUniforms lu{};
    const std::size_t count = std::min(input.lights.size(), kMaxSpotLights);
    for (std::size_t i = 0; i < count; ++i) {
        const SpotLight& l = input.lights[i];
        lu.posRange[i] =
            glm::vec4(glm::vec3(l.position - camera.position), static_cast<float>(l.rangeM));
        lu.dirCos[i] = glm::vec4(glm::vec3(glm::normalize(l.direction)),
                                 static_cast<float>(std::cos(l.outerAngleRad)));
        lu.colorCd[i] = glm::vec4(l.color, static_cast<float>(l.intensityCd));
        lu.cosInner[i] = glm::vec4(static_cast<float>(std::cos(l.innerAngleRad)), 0, 0, 0);
    }
    lu.count.x = static_cast<float>(count);
    if (mainView) {
        stats_.spotLights = static_cast<int>(count);
    }
    glNamedBufferSubData(r.lightUbo.id(), 0, sizeof(lu), &lu);
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, r.lightUbo.id());
}

void Renderer::renderWeather(const FrameInput& input, const Camera& camera)
{
    Impl& r = *impl_;
    const WeatherUniforms wu =
        weatherUniforms(input.weather, camera.position, input.cameraVelocity);
    glNamedBufferSubData(r.weatherUbo.id(), 0, sizeof(wu), &wu);
    glBindBufferBase(GL_UNIFORM_BUFFER, 4, r.weatherUbo.id());
}

void Renderer::renderAtmosphere(const FrameInput& input)
{
    Impl& r = *impl_;
    r.updateAtmosphere(input.weather.aerosolVisibilityM, stats_);
    glBindTextureUnit(0, r.transmittance.id());
    glBindTextureUnit(1, r.multiScatter.id());
    glUseProgram(r.shaders.program(kSkyView));
    glBindImageTexture(0, r.skyView.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glDispatchCompute(dispatchSize(kSkyViewW, 8), dispatchSize(kSkyViewH, 8), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindTextureUnit(2, r.skyView.id());
    glUseProgram(r.shaders.program(kSkyIrradiance));
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glBindTextureUnit(3, r.moonAlbedo.id());
    stats_.drawCalls += 2;
}

void Renderer::renderView(const FrameTarget& target, const ViewRect& view, const FrameInput& input,
                          bool mainView) {
    Impl& r = *impl_;
    HdrTargets& t = mainView ? r.mainTargets : r.insetTargets;
    const int width = view.width;
    const int height = view.height;
    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    t.create(width, height, std::min(settings_.msaaSamples, maxSamples));

    // --- Uniforms ---------------------------------------------------------------------------
    Camera camera = input.camera;
    camera.aspect = static_cast<double>(width) / static_cast<double>(height);
    const glm::mat4 viewProj = camera.viewProjection();
    const SkyState& sky = input.sky;
    FrameUniforms u{};
    u.viewProj = viewProj;
    u.invViewProj = glm::inverse(viewProj);
    u.equatorialToWorld = glm::mat4(sky.equatorialToWorld);
    u.sunDirLux = glm::vec4(sky.sun.direction, sky.sun.illuminanceLux);
    u.sunColorRadius = glm::vec4(sky.sun.color, sky.sun.angularRadiusRad);
    u.moonDirLux = glm::vec4(sky.moon.direction, sky.moon.illuminanceLux);
    u.moonColorRadius = glm::vec4(sky.moon.color, sky.moon.angularRadiusRad);
    u.moonToSunDisc = glm::vec4(sky.moonToSun, sky.moonDiscLuminance);
    u.moonNorth = glm::vec4(sky.moonNorth, 0.0F);
    u.viewport = {static_cast<float>(width), static_cast<float>(height),
                  1.0F / static_cast<float>(width), 1.0F / static_cast<float>(height)};
    const double camAltitudeM = input.originAltitudeM + camera.position.y;
    u.camera = {static_cast<float>(camera.position.y), static_cast<float>(camAltitudeM / 1000.0),
                static_cast<float>(input.originAltitudeM),
                static_cast<float>(std::tan(0.5 * camera.fovYRad))};
    u.misc = {static_cast<float>(std::fmod(input.timeSeconds, 3600.0)),
              static_cast<float>(input.nightSkyLuminance),
              static_cast<float>(sky.moonPhase.illuminatedFraction), settings_.starPointSizePx};
    const bool water = input.terrain.waterLevelM.has_value();
    u.water = {static_cast<float>(input.terrain.waterLevelM.value_or(0.0)), water ? 1.0F : 0.0F,
               0.0F, 0.0F};
    glNamedBufferSubData(r.frameUbo.id(), 0, sizeof(u), &u);

    glBindBufferBase(GL_UNIFORM_BUFFER, 0, r.frameUbo.id());
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, r.atmosphereUbo.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, r.exposureSsbo.id());

    // Local spot lights (FR-GFX-08d), camera-relative.
    renderLocalSpotLights(input, camera, mainView);

    // Weather (FR-WX-09): clouds, fog, precipitation, camera-relative.
    renderWeather(input, camera);

    // --- Atmosphere -------------------------------------------------------------------------
    renderAtmosphere(input);

    const glm::dvec3 cam = camera.position;
    const glm::vec3 camF(cam);
    const TerrainView& ter = input.terrain;
    constexpr double kUnbounded = 1.0e9;
    glm::vec4 extentRel(-kUnbounded, -kUnbounded, kUnbounded, kUnbounded);
    if (ter.extent) {
        extentRel = glm::vec4(static_cast<float>(-0.5 * ter.extent->widthM - cam.x),
                              static_cast<float>(-0.5 * ter.extent->lengthM - cam.z),
                              static_cast<float>(0.5 * ter.extent->widthM - cam.x),
                              static_cast<float>(0.5 * ter.extent->lengthM - cam.z));
    }

    // --- Shadow maps (FR-GFX-10) ------------------------------------------------------------
    // The Sun casts shadows while it is up; at night the Moon does, if it is up.
    glm::dvec3 shadowLight(0.0);
    float shadowCaster = 0.0F;  // 0 = Sun, 1 = Moon.
    if (sky.sun.direction.y > kMinShadowLightY) {
        shadowLight = glm::dvec3(sky.sun.direction);
    } else if (sky.moon.direction.y > kMinShadowLightY) {
        shadowLight = glm::dvec3(sky.moon.direction);
        shadowCaster = 1.0F;
    }
    ShadowSetup shadow;
    if (settings_.shadows && glm::length(shadowLight) > 0.0) {
        r.ensureShadowMap(settings_.shadowMapSize);
        shadow = computeShadowCascades(camera, shadowLight, r.shadowSize);
    }
    {
        ShadowUniforms su{};
        for (int i = 0; i < kShadowCascades; ++i) {
            const ShadowCascade& c = shadow.cascades[static_cast<std::size_t>(i)];
            su.matrix[static_cast<std::size_t>(i)] = c.matrix;
            su.far[i] = c.farM;
            su.texel[i] = c.texelM;
            su.depthRange[i] = std::max(c.depthRangeM, 1.0F);
        }
        su.far.w = shadow.enabled ? 1.0F : 0.0F;
        su.texel.w = shadowCaster;
        su.forward = glm::vec4(glm::vec3(camera.forward()), 0.0F);
        glNamedBufferSubData(r.shadowUbo.id(), 0, sizeof(su), &su);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, r.shadowUbo.id());
    }
    if (shadow.enabled) {
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_CLAMP);  // Casters between the light and the cascade still cast.
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.5F, 2.0F);
        glViewport(0, 0, r.shadowSize, r.shadowSize);
        const double side = static_cast<double>(ter.gridSize - 1);
        if (!ter.chunks.empty()) {
            r.ensureChunkIndices(ter.gridSize);
        }
        for (int i = 0; i < kShadowCascades; ++i) {
            const ShadowCascade& c = shadow.cascades[static_cast<std::size_t>(i)];
            const GLuint fbo = r.shadowFbos[static_cast<std::size_t>(i)].id();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            const GLfloat one = 1.0F;
            glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &one);
            // Terrain.
            const GLuint tp = r.shaders.program(kShadowTerrain);
            glUseProgram(tp);
            glBindVertexArray(r.terrainVao.id());
            glProgramUniform4fv(tp, 2, 1, &extentRel[0]);
            glProgramUniformMatrix4fv(tp, 5, 1, GL_FALSE, &c.matrix[0][0]);
            for (const auto& mesh : ter.chunks) {
                if (mesh == nullptr || mesh->vertices.empty()) {
                    continue;
                }
                const double size = mesh->spacingM * side;
                const glm::vec3 lo(static_cast<float>(mesh->originX - cam.x),
                                   static_cast<float>(static_cast<double>(mesh->minHeight) - cam.y),
                                   static_cast<float>(mesh->originZ - cam.z));
                const glm::vec3 hi(static_cast<float>(mesh->originX + size - cam.x),
                                   static_cast<float>(static_cast<double>(mesh->maxHeight) - cam.y),
                                   static_cast<float>(mesh->originZ + size - cam.z));
                if (!boxInCascade(c.matrix, lo, hi)) {
                    continue;
                }
                const GpuChunk& gpu = r.chunkFor(mesh);
                const glm::vec3 origin(static_cast<float>(mesh->originX - cam.x),
                                       static_cast<float>(-cam.y),
                                       static_cast<float>(mesh->originZ - cam.z));
                glProgramUniform3fv(tp, 0, 1, &origin[0]);
                glProgramUniform1f(tp, 1, static_cast<float>(mesh->spacingM));
                glVertexArrayVertexBuffer(r.terrainVao.id(), 0, gpu.vertices.id(), 0,
                                          sizeof(terrain::TerrainVertex));
                glDrawElements(GL_TRIANGLES, r.chunkIndexCount, GL_UNSIGNED_SHORT, nullptr);
                ++stats_.drawCalls;
            }
            // Objects.
            const GLuint op = r.shaders.program(kShadowObject);
            glUseProgram(op);
            glBindVertexArray(r.objectVao.id());
            glProgramUniformMatrix4fv(op, 5, 1, GL_FALSE, &c.matrix[0][0]);
            for (const DrawItem& item : input.objects) {
                if (!item.castsShadow || item.mesh == nullptr || item.mesh->transparent ||
                    item.mesh->indices.empty()) {
                    continue;
                }
                const GpuMesh& gpu = r.meshFor(item.mesh);
                const glm::mat4 model = modelMatrix(item, cam);
                glProgramUniformMatrix4fv(op, 0, 1, GL_FALSE, &model[0][0]);
                glVertexArrayVertexBuffer(r.objectVao.id(), 0, gpu.vertices.id(), 0,
                                          sizeof(MeshVertex));
                glVertexArrayElementBuffer(r.objectVao.id(), gpu.indices.id());
                glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_INT, nullptr);
                ++stats_.drawCalls;
            }
            ++stats_.shadowCascades;
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_DEPTH_CLAMP);
    }
    glBindTextureUnit(5, r.shadowMap.id());

    // --- Opaque scene (reversed-Z) ----------------------------------------------------------
    glBindFramebuffer(GL_FRAMEBUFFER, t.sceneFbo.id());
    glViewport(0, 0, width, height);
    const std::array<GLfloat, 4> black{0.0F, 0.0F, 0.0F, 1.0F};
    glClearNamedFramebufferfv(t.sceneFbo.id(), GL_COLOR, 0, black.data());
    const GLfloat farDepth = 0.0F;
    glClearNamedFramebufferfv(t.sceneFbo.id(), GL_DEPTH, 0, &farDepth);
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    if (t.samples > 0) {
        glEnable(GL_MULTISAMPLE);
    }

    if (!ter.chunks.empty()) {
        r.ensureChunkIndices(ter.gridSize);
        const Frustum frustum(viewProj);
        const GLuint program = r.shaders.program(kTerrain);
        glUseProgram(program);
        glBindVertexArray(r.terrainVao.id());
        glProgramUniform4fv(program, 2, 1, &extentRel[0]);
        glProgramUniform3fv(program, 3, 1, &camF[0]);
        r.ortho.setBudget(settings_.orthoBudgetBytes);
        r.ortho.beginFrame(ter.ortho, r.frame);
        detail::OrthoSlots slots{};
        const double side = static_cast<double>(ter.gridSize - 1);
        // Visible chunks first, so the orthophoto tiles can be planned for the whole view.
        std::vector<const terrain::ChunkMesh*> visible;
        std::vector<terrain::ChunkId> visibleIds;
        visible.reserve(ter.chunks.size());
        for (const auto& mesh : ter.chunks) {
            if (mesh == nullptr || mesh->vertices.empty()) {
                continue;
            }
            const double size = mesh->spacingM * side;
            const auto minH = static_cast<double>(mesh->minHeight);
            const auto maxH = static_cast<double>(mesh->maxHeight);
            const double drop =
                mesh->spacingM * 2.0 + 0.05 * (maxH - minH) + 220.0;  // Skirts, walls.
            const RelativeBox box{
                .min = glm::vec3(static_cast<float>(mesh->originX - cam.x),
                                 static_cast<float>(minH - drop - cam.y),
                                 static_cast<float>(mesh->originZ - cam.z)),
                .max = glm::vec3(static_cast<float>(mesh->originX + size - cam.x),
                                 static_cast<float>(maxH - cam.y),
                                 static_cast<float>(mesh->originZ + size - cam.z)),
            };
            if (!frustum.intersects(box)) {
                ++stats_.chunksCulled;
                continue;
            }
            r.chunkFor(mesh);  // Uploaded (or kept) while its shared_ptr is at hand.
            visible.push_back(mesh.get());
            visibleIds.push_back(mesh->id);
        }
        if (ter.ortho != nullptr) {
            r.ortho.prepare(visibleIds);
        }
        for (const terrain::ChunkMesh* mesh : visible) {
            const double size = mesh->spacingM * side;
            const GpuChunk& gpu = r.chunks.at(mesh);
            const glm::vec3 origin(static_cast<float>(mesh->originX - cam.x),
                                   static_cast<float>(-cam.y),
                                   static_cast<float>(mesh->originZ - cam.z));
            glProgramUniform3fv(program, 0, 1, &origin[0]);
            glProgramUniform1f(program, 1, static_cast<float>(mesh->spacingM));
            glProgramUniform1f(program, 4, static_cast<float>(size));
            // Orthophoto tiles of the chunk (or coarser stand-ins), else the materials.
            const bool imagery = ter.ortho != nullptr && r.ortho.slotsFor(mesh->id, slots);
            glProgramUniform1i(program, 5, imagery ? 1 : 0);
            if (imagery) {
                glProgramUniform4fv(program, 6, static_cast<GLsizei>(slots.size()), &slots[0][0]);
                glBindTextureUnit(6, r.ortho.texture());
            }
            glVertexArrayVertexBuffer(r.terrainVao.id(), 0, gpu.vertices.id(), 0,
                                      sizeof(terrain::TerrainVertex));
            glDrawElements(GL_TRIANGLES, r.chunkIndexCount, GL_UNSIGNED_SHORT, nullptr);
            ++stats_.drawCalls;
            ++stats_.chunksDrawn;
            stats_.triangles += static_cast<std::uint64_t>(r.chunkIndexCount / 3);
        }
    }

    // Objects drawn with the object program; opaque or translucent pass.
    const auto drawObjects = [&](bool transparent) {
        const GLuint program = r.shaders.program(kObject);
        glUseProgram(program);
        glBindVertexArray(r.objectVao.id());
        for (const DrawItem& item : input.objects) {
            if (item.mesh == nullptr || item.mesh->indices.empty() ||
                item.mesh->transparent != transparent) {
                continue;
            }
            const GpuMesh& gpu = r.meshFor(item.mesh);
            const glm::mat4 model = modelMatrix(item, cam);
            const glm::mat3 normal = glm::transpose(glm::inverse(glm::mat3(model)));
            glProgramUniformMatrix4fv(program, 0, 1, GL_FALSE, &model[0][0]);
            glProgramUniformMatrix3fv(program, 4, 1, GL_FALSE, &normal[0][0]);
            glProgramUniform4fv(program, 7, 1, &item.tint[0]);
            glProgramUniform1f(program, 8, item.emissiveScale);
            glVertexArrayVertexBuffer(r.objectVao.id(), 0, gpu.vertices.id(), 0,
                                      sizeof(MeshVertex));
            glVertexArrayElementBuffer(r.objectVao.id(), gpu.indices.id());
            glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_INT, nullptr);
            ++stats_.drawCalls;
            ++stats_.objectsDrawn;
            stats_.triangles += static_cast<std::uint64_t>(gpu.indexCount / 3);
        }
    };
    drawObjects(false);

    glBindVertexArray(r.emptyVao.id());
    if (water) {
        glm::vec4 rect(static_cast<float>(-settings_.waterRadiusM),
                       static_cast<float>(-settings_.waterRadiusM),
                       static_cast<float>(settings_.waterRadiusM),
                       static_cast<float>(settings_.waterRadiusM));
        if (ter.extent && !ter.waterBeyondExtent) {
            rect =
                glm::vec4(glm::max(glm::vec2(rect.x, rect.y), glm::vec2(extentRel.x, extentRel.y)),
                          glm::min(glm::vec2(rect.z, rect.w), glm::vec2(extentRel.z, extentRel.w)));
        }
        if (rect.x < rect.z && rect.y < rect.w) {
            const GLuint program = r.shaders.program(kWater);
            glUseProgram(program);
            glProgramUniform4fv(program, 0, 1, &rect[0]);
            glProgramUniform3fv(program, 3, 1, &camF[0]);
            glDisable(GL_CULL_FACE);  // Visible from below as well.
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glEnable(GL_CULL_FACE);
            ++stats_.drawCalls;
            stats_.triangles += 2;
        }
    }

    // --- Sky and stars where nothing was drawn ----------------------------------------------
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glUseProgram(r.shaders.program(kSky));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ++stats_.drawCalls;

    r.uploadStars(input);
    if (r.starCount > 0) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, r.starsSsbo.id());
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glUseProgram(r.shaders.program(kStars));
        glDrawArrays(GL_POINTS, 0, r.starCount);
        glDisable(GL_PROGRAM_POINT_SIZE);
        glDisable(GL_BLEND);
        ++stats_.drawCalls;
    }
    // Translucent objects (propeller discs) over everything, without depth writes.
    glDepthFunc(GL_GREATER);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    drawObjects(true);
    // Rain and snow around the camera (FR-WX-09), in front of the scene.
    if (input.weather.particles > 0) {
        glBindVertexArray(r.emptyVao.id());
        glUseProgram(r.shaders.program(kPrecipitation));
        glDrawArrays(GL_TRIANGLES, 0, 6 * input.weather.particles);
        ++stats_.drawCalls;
        stats_.triangles += 2 * static_cast<std::uint64_t>(input.weather.particles);
        if (mainView) {
            stats_.precipitation = input.weather.particles;
        }
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);

    // --- Resolve, exposure, tone mapping ----------------------------------------------------
    if (t.samples > 0) {
        glBlitNamedFramebuffer(t.sceneFbo.id(), t.resolveFbo.id(), 0, 0, width, height, 0, 0, width,
                               height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    glBindTextureUnit(4, t.resolved.id());
    if (mainView) {
        // Insets use the exposure of the main view: only the main view is metered.
        glUseProgram(r.shaders.program(kHistogram));
        glDispatchCompute(dispatchSize(width, 16), dispatchSize(height, 16), 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        ++stats_.drawCalls;
    }

    // Tone mapping uses this frame's exposure: run it before the exposure update. The
    // rectangle is given from the top of the target; OpenGL counts from the bottom.
    const int glY = target.height - view.y - height;
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(view.x, glY, width, height);
    glDisable(GL_FRAMEBUFFER_SRGB);
    const GLuint tonemap = r.shaders.program(kTonemap);
    glProgramUniform2i(tonemap, 0, view.x, glY);
    static const glm::mat3 kWhiteBalance =
        whiteBalanceMatrix(glm::dvec3(blackbodyColor(kDaylightWhiteK)));
    glProgramUniformMatrix3fv(tonemap, 1, 1, GL_FALSE, &kWhiteBalance[0][0]);
    glProgramUniform2f(tonemap, 2, static_cast<float>(std::clamp(input.lensWetness, 0.0, 1.0)),
                       static_cast<float>(std::fmod(input.timeSeconds, 3600.0)));
    glUseProgram(tonemap);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ++stats_.drawCalls;

    if (mainView) {
        const GLuint exposure = r.shaders.program(kExposure);
        const bool reset = input.resetExposure || r.exposureReset;
        r.exposureReset = false;
        glProgramUniform1f(exposure, 0, static_cast<float>(std::max(input.dtSeconds, 0.0)));
        glProgramUniform1i(exposure, 1, input.exposure.mode == ExposureMode::Manual ? 1 : 0);
        glProgramUniform1f(exposure, 2, static_cast<float>(input.exposure.manualEv100));
        glProgramUniform1f(exposure, 3, static_cast<float>(input.exposure.compensationEv));
        glProgramUniform1i(exposure, 4, reset ? 1 : 0);
        glProgramUniform1f(exposure, 5,
                           static_cast<float>(estimateEv100(sky, input.exposure.compensationEv,
                                                            input.weather.skyFactor)));
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        glUseProgram(exposure);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        ++stats_.drawCalls;
    }

    glUseProgram(0);
    glBindVertexArray(0);
    if (mainView) {
        r.evictChunks();
    }
    stats_.gpuChunks = r.chunks.size();
    const detail::OrthoTextureStats ortho = r.ortho.stats();
    stats_.orthoTiles = ortho.residentTiles;
    stats_.orthoBytes = ortho.bytes;
    stats_.orthoUploads += ortho.uploads;
}

}  // namespace ds::graphics
