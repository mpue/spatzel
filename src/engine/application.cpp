#include "engine/application.hpp"

#include <array>
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

std::vector<uint32_t> loadSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("cannot open SPIR-V module: " + path.string());
    }

    const std::streamsize byteCount = file.tellg();
    if (byteCount <= 0 || byteCount % sizeof(uint32_t) != 0) {
        throw std::runtime_error("not a SPIR-V module (bad size): " + path.string());
    }

    std::vector<uint32_t> words(static_cast<size_t>(byteCount) / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), byteCount);
    if (!file) {
        throw std::runtime_error("short read on SPIR-V module: " + path.string());
    }
    return words;
}

template <typename T>
std::span<const std::byte> asBytes(const T& value) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value), sizeof(T));
}

} // namespace

Application::Application(const AppConfig& config)
    : m_window({.width = config.width, .height = config.height, .title = config.title}),
      m_maxFrames(config.maxFrames) {
    const platform::Extent2D framebuffer = m_window.framebufferSize();

    m_device = rhi::createDevice(config.backend,
                                 {
                                     .nativeWindowHandle = m_window.nativeHandle(),
                                     .framebufferSize    = {framebuffer.width, framebuffer.height},
                                     .enableValidation   = config.enableValidation,
                                     .applicationName    = config.title.c_str(),
                                 });

    const std::vector<uint32_t> spirv =
        loadSpirv(config.shaderDirectory / "raymarch_probe.comp.spv");
    m_shader = m_device->createShader(spirv);

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
    // The device outlives every resource, but in-flight work must finish
    // before anything it references goes away.
    m_device->waitIdle();
    destroyRenderTarget();
    if (rhi::isValid(m_pipeline)) {
        m_device->destroy(m_pipeline);
    }
    if (rhi::isValid(m_shader)) {
        m_device->destroy(m_shader);
    }
    m_device->waitIdle();
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
        .usage     = rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSrc,
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

void Application::run() {
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
        .time       = static_cast<float>(platform::timeSeconds()),
    };

    cmd.bindComputePipeline(m_pipeline);
    cmd.bindStorageTexture(0, m_renderTarget);
    cmd.pushConstants(asBytes(push));
    cmd.dispatch(divideRoundUp(m_targetExtent.width, kWorkgroupSize),
                 divideRoundUp(m_targetExtent.height, kWorkgroupSize), 1);
    cmd.blitToSwapchain(m_renderTarget);

    m_device->endFrame();
}

} // namespace engine
