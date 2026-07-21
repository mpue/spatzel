#include "engine/application.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace engine {
namespace {

// Local workgroup size of raymarch_probe.comp.
constexpr uint32_t kWorkgroupSize = 16;

constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) {
    return (value + divisor - 1) / divisor;
}

template <typename T>
std::span<const std::byte> asBytes(const T& value) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value), sizeof(T));
}

} // namespace

Application::Application(const AppConfig& config)
    // The window must be created for whatever the chosen backend needs: a
    // client-API context cannot be attached after the fact. The engine only
    // forwards the answer — it never interprets it.
    : m_window({.width    = config.width,
                .height   = config.height,
                .title    = config.title,
                .graphics = rhi::windowRequirements(config.backend, config.enableDebug)}),
      m_shaderRoot(config.shaderRoot.string()),
      m_maxFrames(config.maxFrames),
      m_dumpPath(config.dumpPath),
      m_comparePath(config.comparePath),
      m_pinTime(!config.dumpPath.empty() || !config.comparePath.empty()),
      m_fixedTime(config.fixedTime),
      m_tolerance(config.tolerance) {
    const platform::Extent2D framebuffer = m_window.framebufferSize();

    m_device = rhi::createDevice(config.backend,
                                 {
                                     .nativeWindowHandle = m_window.nativeHandle(),
                                     .framebufferSize    = {framebuffer.width, framebuffer.height},
                                     .enableDebug        = config.enableDebug,
                                     .applicationName    = config.title.c_str(),
                                     .shaderRoot         = m_shaderRoot.c_str(),
                                 });

    m_shader = m_device->createShader("raymarch_probe");

    constexpr std::array<rhi::BindingDesc, 1> bindings{
        rhi::BindingDesc{.slot = 0, .type = rhi::BindingType::StorageTexture}};
    m_pipeline = m_device->createComputePipeline({
        .cs               = m_shader,
        .pushConstantSize = sizeof(ProbePushConstants),
        .bindings         = bindings,
        .debugName        = "raymarch_probe",
    });

    resizeRenderTarget(m_device->swapchainExtent());
}

Application::~Application() {
    // No idle wait: destruction is safe to request at any time, and a backend
    // that can still have work in flight defers the release itself.
    destroyRenderTarget();
    if (rhi::isValid(m_pipeline)) {
        m_device->destroy(m_pipeline);
    }
    if (rhi::isValid(m_shader)) {
        m_device->destroy(m_shader);
    }
}

void Application::resizeRenderTarget(rhi::Extent2D extent) {
    if (extent.width == 0 || extent.height == 0 || extent == m_targetExtent) {
        return;
    }
    destroyRenderTarget();

    m_renderTarget = m_device->createTexture({
        .width     = extent.width,
        .height    = extent.height,
        .depth     = 1,
        .format    = rhi::Format::RGBA16Float,
        .usage     = rhi::TextureUsage::Storage | rhi::TextureUsage::CopySrc,
        .debugName = "probe_target",
    });
    m_targetExtent = extent;
}

void Application::destroyRenderTarget() {
    if (rhi::isValid(m_renderTarget)) {
        m_device->destroy(m_renderTarget);
        m_renderTarget = rhi::TextureHandle::Invalid;
        m_targetExtent = {};
    }
}

bool Application::run() {
    while (!m_window.shouldClose()) {
        m_window.pollEvents();

        if (m_window.isKeyDown(platform::Key::Escape)) {
            m_window.requestClose();
            continue;
        }

        if (m_window.consumeResized()) {
            const platform::Extent2D size = m_window.framebufferSize();
            m_device->onResize(size.width, size.height);
        }

        if (m_window.isMinimised()) {
            // Nothing to present into; idle instead of spinning.
            m_window.waitEvents();
            continue;
        }

        renderFrame();
        ++m_framesDrawn;

        if (m_maxFrames != 0 && m_framesDrawn >= m_maxFrames) {
            m_window.requestClose();
        }
    }

    if (m_framesDrawn == 0) {
        return true;
    }
    if (!m_dumpPath.empty()) {
        writeDump(m_dumpPath);
    }
    if (!m_comparePath.empty()) {
        return compareAgainst(m_comparePath);
    }
    return true;
}

void Application::renderFrame() {
    rhi::CommandList& cmd = m_device->beginFrame();

    // beginFrame is what actually rebuilds the swapchain, so the authoritative
    // surface size is only known now. Creating and destroying resources during
    // recording is fine: destruction is deferred past the frames in flight.
    resizeRenderTarget(m_device->swapchainExtent());

    const ProbePushConstants push{
        .resolution = {static_cast<float>(m_targetExtent.width),
                       static_cast<float>(m_targetExtent.height)},
        .time = m_pinTime ? m_fixedTime : static_cast<float>(platform::timeSeconds()),
    };

    cmd.bindComputePipeline(m_pipeline);
    cmd.bindStorageTexture(0, m_renderTarget);
    cmd.pushConstants(asBytes(push));
    cmd.dispatch(divideRoundUp(m_targetExtent.width, kWorkgroupSize),
                 divideRoundUp(m_targetExtent.height, kWorkgroupSize), 1);
    cmd.blitToSwapchain(m_renderTarget);

    m_device->endFrame();
}

// ---------------------------------------------------------------------------
// Verification
//
// The dump is a deliberately dumb container: magic, extent, linear RGBA
// floats. It exists so two backends can be diffed against each other, and as
// the starting point for golden-image tests later.
// ---------------------------------------------------------------------------
namespace {

constexpr char kDumpMagic[4] = {'F', 'Z', 'L', 'D'};

struct DumpHeader {
    char     magic[4]{};
    uint32_t width  = 0;
    uint32_t height = 0;
};

} // namespace

void Application::writeDump(const std::filesystem::path& path) {
    std::vector<float> pixels(static_cast<size_t>(m_targetExtent.width) * m_targetExtent.height * 4);
    m_device->readTexture(m_renderTarget, pixels);

    DumpHeader header{};
    std::memcpy(header.magic, kDumpMagic, sizeof(kDumpMagic));
    header.width  = m_targetExtent.width;
    header.height = m_targetExtent.height;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("cannot write dump: " + path.string());
    }
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(pixels.data()),
               static_cast<std::streamsize>(pixels.size() * sizeof(float)));
    if (!file) {
        throw std::runtime_error("short write on dump: " + path.string());
    }
    std::fprintf(stderr, "[engine] wrote %ux%u dump to %s\n", header.width, header.height,
                 path.string().c_str());
}

bool Application::compareAgainst(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot read reference: " + path.string());
    }

    DumpHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || std::memcmp(header.magic, kDumpMagic, sizeof(kDumpMagic)) != 0) {
        throw std::runtime_error("not a fitzel dump: " + path.string());
    }
    if (header.width != m_targetExtent.width || header.height != m_targetExtent.height) {
        std::fprintf(stderr, "[compare] FAIL: reference is %ux%u, this run is %ux%u\n",
                     header.width, header.height, m_targetExtent.width, m_targetExtent.height);
        return false;
    }

    const size_t       count = static_cast<size_t>(header.width) * header.height * 4;
    std::vector<float> reference(count);
    file.read(reinterpret_cast<char*>(reference.data()),
              static_cast<std::streamsize>(count * sizeof(float)));
    if (!file) {
        throw std::runtime_error("truncated reference: " + path.string());
    }

    std::vector<float> actual(count);
    m_device->readTexture(m_renderTarget, actual);

    float  maxDiff  = 0.0f;
    double sumDiff  = 0.0;
    size_t overCount = 0;
    for (size_t i = 0; i < count; ++i) {
        const float diff = std::abs(actual[i] - reference[i]);
        maxDiff = std::max(maxDiff, diff);
        sumDiff += diff;
        overCount += diff > m_tolerance ? 1 : 0;
    }

    const bool passed = maxDiff <= m_tolerance;
    std::fprintf(stderr,
                 "[compare] %s — max %.6f, mean %.6f, %zu/%zu components over tolerance %.6f\n",
                 passed ? "PASS" : "FAIL", static_cast<double>(maxDiff), sumDiff / double(count),
                 overCount, count, static_cast<double>(m_tolerance));
    return passed;
}

} // namespace engine
