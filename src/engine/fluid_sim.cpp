#include "engine/fluid_sim.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace engine {
namespace {

constexpr uint32_t kGroupSize = 4; // matches local_size_* in every fluid_*.comp

[[nodiscard]] uint32_t divideRoundUp(uint32_t value, uint32_t divisor) {
    return (value + divisor - 1) / divisor;
}

template <typename T>
[[nodiscard]] std::span<const std::byte> asBytes(const T& value) {
    return {reinterpret_cast<const std::byte*>(&value), sizeof(T)};
}

template <typename T>
[[nodiscard]] std::span<std::byte> asWritableBytes(T& value) {
    return {reinterpret_cast<std::byte*>(&value), sizeof(T)};
}

constexpr rhi::BindingDesc storage(uint32_t slot) {
    return rhi::BindingDesc{.slot = slot, .type = rhi::BindingType::StorageBuffer};
}

} // namespace

FluidSim::~FluidSim() {
    destroy();
}

FluidSim::Pass FluidSim::makePass(const char* name, std::span<const rhi::BindingDesc> bindings) {
    Pass pass;
    pass.shader   = m_device->createShader(name);
    pass.pipeline = m_device->createComputePipeline({
        .cs               = pass.shader,
        .pushConstantSize = sizeof(fluid::GpuPush),
        .bindings         = bindings,
        .debugName        = name,
    });
    return pass;
}

void FluidSim::create(rhi::Device& device) {
    m_device = &device;

    // The binding list per pass mirrors the FLUID_* macros the matching shader
    // #defines before including fluid_common.glsl. Those macros decide which
    // buffers the shader declares, and a declared-but-unbound buffer is a
    // descriptor layout mismatch — so these two lists are the same list, written
    // twice, and they have to be read together.
    const std::array seedBindings{storage(kSlotVelDst), storage(kSlotPhiDst),
                                  storage(kSlotPressure)};
    const std::array solidsBindings{storage(kSlotScene), storage(kSlotSolid)};
    const std::array advectBindings{storage(kSlotVelSrc), storage(kSlotVelDst),
                                    storage(kSlotPhiSrc), storage(kSlotPhiDst),
                                    storage(kSlotSolid)};
    const std::array divergenceBindings{storage(kSlotVelSrc), storage(kSlotPhiSrc),
                                        storage(kSlotSolid), storage(kSlotDivergence)};
    const std::array pressureBindings{storage(kSlotPhiSrc), storage(kSlotSolid),
                                      storage(kSlotDivergence), storage(kSlotPressure)};
    const std::array projectBindings{storage(kSlotVelSrc), storage(kSlotVelDst),
                                     storage(kSlotPhiSrc), storage(kSlotSolid),
                                     storage(kSlotPressure)};
    const std::array extrapolateBindings{storage(kSlotVelSrc), storage(kSlotVelDst),
                                         storage(kSlotPhiSrc), storage(kSlotSolid)};
    const std::array reinitBindings{storage(kSlotPhiSrc), storage(kSlotPhiDst)};
    const std::array statsBindings{storage(kSlotVelSrc), storage(kSlotPhiSrc),
                                   storage(kSlotSolid), storage(kSlotStats)};

    m_seedPass        = makePass("fluid_seed", seedBindings);
    m_solidsPass      = makePass("fluid_solids", solidsBindings);
    m_advectPass      = makePass("fluid_advect", advectBindings);
    m_divergencePass  = makePass("fluid_divergence", divergenceBindings);
    m_pressurePass    = makePass("fluid_pressure", pressureBindings);
    m_projectPass     = makePass("fluid_project", projectBindings);
    m_extrapolatePass = makePass("fluid_extrapolate", extrapolateBindings);
    m_reinitPass      = makePass("fluid_reinit", reinitBindings);
    m_statsPass       = makePass("fluid_stats", statsBindings);

    // The render-side parameter block: tiny, host-visible, re-uploaded whenever
    // a setting changes. It exists from the start even when the fluid is off,
    // because the marchers bind it unconditionally and read `enabled` from it.
    m_params = m_device->createBuffer({
        .size      = sizeof(fluid::GpuParams),
        .usage     = rhi::BufferUsage::Storage,
        .access    = rhi::MemoryAccess::CpuToGpu,
        .debugName = "fluid_params",
    });

    m_statsBuffer = m_device->createBuffer({
        .size  = sizeof(fluid::GpuStats),
        .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc | rhi::BufferUsage::CopyDst,
        .access    = rhi::MemoryAccess::GpuOnly,
        .debugName = "fluid_stats",
    });
}

void FluidSim::ensureFields() {
    if (rhi::isValid(m_phi[0])) {
        return;
    }

    // Sized for the resolution ceiling, so moving the resolution slider never
    // reallocates. Same trade the brick pool makes, and for the same reason:
    // a reallocation mid-run is a stall and a lifetime problem, and the ceiling
    // is affordable.
    const uint64_t faceBytes =
        static_cast<uint64_t>(fluid::kMaxFaceCount) * 3 * sizeof(float);
    const uint64_t cellBytes = static_cast<uint64_t>(fluid::kMaxCellCount) * sizeof(float);

    const auto field = [&](uint64_t size, const char* name) {
        return m_device->createBuffer({
            .size      = size,
            .usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc |
                     rhi::BufferUsage::CopyDst,
            .access    = rhi::MemoryAccess::GpuOnly,
            .debugName = name,
        });
    };

    m_velocity[0] = field(faceBytes, "fluid_velocity_a");
    m_velocity[1] = field(faceBytes, "fluid_velocity_b");
    m_phi[0]      = field(cellBytes, "fluid_phi_a");
    m_phi[1]      = field(cellBytes, "fluid_phi_b");
    m_pressureBuffer = field(cellBytes, "fluid_pressure");

    m_divergenceBuffer = field(cellBytes, "fluid_divergence");
    m_solidBuffer      = field(cellBytes, "fluid_solid");

    m_needSeed   = true;
    m_needSolids = true;
}

void FluidSim::destroyFields() {
    if (m_device == nullptr) {
        return;
    }
    for (rhi::BufferHandle* buffer :
         {&m_velocity[0], &m_velocity[1], &m_phi[0], &m_phi[1], &m_pressureBuffer,
          &m_divergenceBuffer, &m_solidBuffer}) {
        if (rhi::isValid(*buffer)) {
            m_device->destroy(*buffer);
            *buffer = rhi::BufferHandle::Invalid;
        }
    }
    m_seededRes = 0;
}

void FluidSim::destroy() {
    if (m_device == nullptr) {
        return;
    }
    destroyFields();
    for (rhi::BufferHandle buffer : {m_params, m_statsBuffer}) {
        if (rhi::isValid(buffer)) {
            m_device->destroy(buffer);
        }
    }
    m_params      = rhi::BufferHandle::Invalid;
    m_statsBuffer = rhi::BufferHandle::Invalid;

    for (Pass* pass : {&m_seedPass, &m_solidsPass, &m_advectPass, &m_divergencePass,
                       &m_pressurePass, &m_projectPass, &m_extrapolatePass, &m_reinitPass,
                       &m_statsPass}) {
        if (rhi::isValid(pass->pipeline)) {
            m_device->destroy(pass->pipeline);
            pass->pipeline = rhi::PipelineHandle::Invalid;
        }
        if (rhi::isValid(pass->shader)) {
            m_device->destroy(pass->shader);
            pass->shader = rhi::ShaderHandle::Invalid;
        }
    }
    m_device = nullptr;
}

void FluidSim::uploadParams(const fluid::Settings& settings) {
    if (m_device == nullptr || !rhi::isValid(m_params)) {
        return;
    }

    fluid::GpuParams params{};
    params.origin[0] = settings.origin.x;
    params.origin[1] = settings.origin.y;
    params.origin[2] = settings.origin.z;
    params.origin[3] = settings.cellSize();

    // The marchers must read the resolution the fields were SEEDED at, never the
    // slider's pending value: the two disagree for exactly one frame after the
    // slider moves, and a marcher indexing a 96^3 field as if it were 64^3 draws
    // garbage. Same reason the brick marcher reads m_bakedGridRes.
    params.control[0] = m_seededRes > 0 ? m_seededRes : settings.res;
    params.control[1] = (settings.enabled && resident() && m_seededRes > 0) ? 1 : 0;

    params.water[0] = settings.colour[0];
    params.water[1] = settings.colour[1];
    params.water[2] = settings.colour[2];
    params.water[3] = settings.transmission;

    params.material[0] = settings.roughness;
    params.material[1] = 0.0f; // water is a dielectric
    params.material[2] = settings.ior;

    params.render[0] = settings.trustBandCells;
    params.render[1] = settings.surfaceOffset;

    m_device->updateBuffer(m_params, asBytes(params));
}

fluid::GpuPush FluidSim::pushFor(const fluid::Settings& settings, int primitiveCount) const {
    fluid::GpuPush push{};
    push.domain[0] = settings.origin.x;
    push.domain[1] = settings.origin.y;
    push.domain[2] = settings.origin.z;
    push.domain[3] = settings.cellSize();

    push.control[0] = std::clamp(settings.res, fluid::kMinRes, fluid::kMaxRes);
    push.control[1] = primitiveCount;

    push.step[0] = settings.timestep;
    push.step[1] = settings.gravity;

    push.seedMin[0] = settings.seedMin.x;
    push.seedMin[1] = settings.seedMin.y;
    push.seedMin[2] = settings.seedMin.z;
    push.seedMin[3] = settings.poolLevel;

    push.seedMax[0] = settings.seedMax.x;
    push.seedMax[1] = settings.seedMax.y;
    push.seedMax[2] = settings.seedMax.z;
    return push;
}

void FluidSim::recordSeed(rhi::CommandList& cmd, const fluid::GpuPush& push, int res) {
    const uint32_t groups = divideRoundUp(static_cast<uint32_t>(res) + 1, kGroupSize);

    cmd.bindComputePipeline(m_seedPass.pipeline);
    cmd.pushConstants(asBytes(push));
    cmd.bindStorageBuffer(kSlotVelDst, m_velocity[m_velocityIndex]);
    cmd.bindStorageBuffer(kSlotPhiDst, m_phi[m_phiIndex]);
    cmd.bindStorageBuffer(kSlotPressure, m_pressureBuffer);
    cmd.dispatch(groups, groups, groups);
}

void FluidSim::recordSolids(rhi::CommandList& cmd, const fluid::GpuPush& push, int res,
                            rhi::BufferHandle sceneBuffer) {
    const uint32_t groups = divideRoundUp(static_cast<uint32_t>(res), kGroupSize);

    cmd.bindComputePipeline(m_solidsPass.pipeline);
    cmd.pushConstants(asBytes(push));
    // The edit list, at the slot every pass in the engine reads it from. Bound
    // here rather than relying on the frame's earlier binding: binding a
    // pipeline drops the descriptors bound for the previous one, by contract.
    cmd.bindStorageBuffer(kSlotScene, sceneBuffer);
    cmd.bindStorageBuffer(kSlotSolid, m_solidBuffer);
    cmd.dispatch(groups, groups, groups);
}

void FluidSim::recordSubstep(rhi::CommandList& cmd, const fluid::Settings& settings,
                             const fluid::GpuPush& push) {
    const int      res         = push.control[0];
    const uint32_t cellGroups  = divideRoundUp(static_cast<uint32_t>(res), kGroupSize);
    const uint32_t faceGroups  = divideRoundUp(static_cast<uint32_t>(res) + 1, kGroupSize);

    const auto velSrc  = [&] { return m_velocity[m_velocityIndex]; };
    const auto velDst  = [&] { return m_velocity[1 - m_velocityIndex]; };
    const auto phiSrc  = [&] { return m_phi[m_phiIndex]; };
    const auto phiDst  = [&] { return m_phi[1 - m_phiIndex]; };

    // 1. Advect velocity and the level set, and add gravity. Both fields flip.
    cmd.bindComputePipeline(m_advectPass.pipeline);
    cmd.pushConstants(asBytes(push));
    cmd.bindStorageBuffer(kSlotVelSrc, velSrc());
    cmd.bindStorageBuffer(kSlotVelDst, velDst());
    cmd.bindStorageBuffer(kSlotPhiSrc, phiSrc());
    cmd.bindStorageBuffer(kSlotPhiDst, phiDst());
    cmd.bindStorageBuffer(kSlotSolid, m_solidBuffer);
    cmd.dispatch(faceGroups, faceGroups, faceGroups);
    m_velocityIndex = 1 - m_velocityIndex;
    m_phiIndex      = 1 - m_phiIndex;

    // 2. Right-hand side of the pressure equation.
    cmd.bindComputePipeline(m_divergencePass.pipeline);
    cmd.pushConstants(asBytes(push));
    cmd.bindStorageBuffer(kSlotVelSrc, velSrc());
    cmd.bindStorageBuffer(kSlotPhiSrc, phiSrc());
    cmd.bindStorageBuffer(kSlotSolid, m_solidBuffer);
    cmd.bindStorageBuffer(kSlotDivergence, m_divergenceBuffer);
    cmd.dispatch(cellGroups, cellGroups, cellGroups);

    // 3. The pressure solve: red-black Gauss-Seidel, two dispatches per sweep.
    // The field is warm-started — deliberately not cleared here — so a settled
    // pool begins from the pressure that was already holding it up and the
    // sweeps only have to correct it. It is cleared once, by the seed pass, so
    // a reset still starts from nothing.
    fluid::GpuPush sweep = push;
    for (int i = 0; i < std::max(settings.pressureSweeps, 1); ++i) {
        for (int colour = 0; colour < 2; ++colour) {
            sweep.control[2] = colour;
            cmd.bindComputePipeline(m_pressurePass.pipeline);
            cmd.pushConstants(asBytes(sweep));
            cmd.bindStorageBuffer(kSlotPhiSrc, phiSrc());
            cmd.bindStorageBuffer(kSlotSolid, m_solidBuffer);
            cmd.bindStorageBuffer(kSlotDivergence, m_divergenceBuffer);
            cmd.bindStorageBuffer(kSlotPressure, m_pressureBuffer);
            cmd.dispatch(cellGroups, cellGroups, cellGroups);
        }
    }

    // 4. Subtract the gradient.
    cmd.bindComputePipeline(m_projectPass.pipeline);
    cmd.pushConstants(asBytes(push));
    cmd.bindStorageBuffer(kSlotVelSrc, velSrc());
    cmd.bindStorageBuffer(kSlotVelDst, velDst());
    cmd.bindStorageBuffer(kSlotPhiSrc, phiSrc());
    cmd.bindStorageBuffer(kSlotSolid, m_solidBuffer);
    cmd.bindStorageBuffer(kSlotPressure, m_pressureBuffer);
    cmd.dispatch(faceGroups, faceGroups, faceGroups);
    m_velocityIndex = 1 - m_velocityIndex;

    // 5. Push the fluid velocity a few cells into the air, so the next
    // advection has something to read there.
    for (int i = 0; i < std::max(settings.extrapolateSweeps, 0); ++i) {
        cmd.bindComputePipeline(m_extrapolatePass.pipeline);
        cmd.pushConstants(asBytes(push));
        cmd.bindStorageBuffer(kSlotVelSrc, velSrc());
        cmd.bindStorageBuffer(kSlotVelDst, velDst());
        cmd.bindStorageBuffer(kSlotPhiSrc, phiSrc());
        cmd.bindStorageBuffer(kSlotSolid, m_solidBuffer);
        cmd.dispatch(faceGroups, faceGroups, faceGroups);
        m_velocityIndex = 1 - m_velocityIndex;
    }

    // 6. Restore the level set to a distance function. Not cosmetic: the
    // renderer sphere-traces this field, and a field whose gradient is not unit
    // length is not one a sphere tracer may step by.
    for (int i = 0; i < std::max(settings.reinitIterations, 0); ++i) {
        cmd.bindComputePipeline(m_reinitPass.pipeline);
        cmd.pushConstants(asBytes(push));
        cmd.bindStorageBuffer(kSlotPhiSrc, phiSrc());
        cmd.bindStorageBuffer(kSlotPhiDst, phiDst());
        cmd.dispatch(cellGroups, cellGroups, cellGroups);
        m_phiIndex = 1 - m_phiIndex;
    }
}

void FluidSim::recordStats(rhi::CommandList& cmd, const fluid::GpuPush& push, int res) {
    const uint32_t groups = divideRoundUp(static_cast<uint32_t>(res), kGroupSize);

    // Cleared every frame: the pass maxes and sums into it, so a stale value
    // would turn the readout into a running high-water mark instead of a
    // measurement of this frame.
    cmd.clearBuffer(m_statsBuffer);
    cmd.bindComputePipeline(m_statsPass.pipeline);
    cmd.pushConstants(asBytes(push));
    cmd.bindStorageBuffer(kSlotVelSrc, m_velocity[m_velocityIndex]);
    cmd.bindStorageBuffer(kSlotPhiSrc, m_phi[m_phiIndex]);
    cmd.bindStorageBuffer(kSlotSolid, m_solidBuffer);
    cmd.bindStorageBuffer(kSlotStats, m_statsBuffer);
    cmd.dispatch(groups, groups, groups);
}

void FluidSim::recordStep(rhi::CommandList& cmd, const fluid::Settings& settings,
                          rhi::BufferHandle sceneBuffer, int primitiveCount,
                          float frameSeconds) {
    if (m_device == nullptr || !settings.enabled) {
        m_timeDebt = 0.0f;
        return;
    }

    ensureFields();

    const fluid::GpuPush push = pushFor(settings, primitiveCount);
    const int            res  = push.control[0];

    // A resolution change reinterprets every index in every buffer, so the
    // fields are re-seeded rather than resampled. Resampling a level set
    // between resolutions is real work and buys nothing this stage needs.
    if (res != m_seededRes) {
        m_needSeed   = true;
        m_needSolids = true;
    }

    if (m_needSolids) {
        recordSolids(cmd, push, res, sceneBuffer);
        m_needSolids = false;
    }

    if (m_needSeed) {
        recordSeed(cmd, push, res);
        m_needSeed             = false;
        m_seededRes            = res;
        m_timeDebt             = 0.0f;
        m_referencePending     = true;
        m_referenceVolumeMilli = 0.0f;
        // No substeps on the seeding frame: the diagnostics pass below then
        // measures the seeded state exactly, which is the baseline every later
        // volume figure is quoted against.
        recordStats(cmd, push, res);
        return;
    }

    // Fixed-size substeps, catching the simulation clock up to the wall clock
    // without ever letting the timestep follow the frame rate. A frame that
    // needs more substeps than the budget allows drops the excess rather than
    // banking it: banking it would turn one slow frame into a spiral of slower
    // ones.
    const float dt = std::max(settings.timestep, 1.0e-5f);
    m_timeDebt += std::clamp(frameSeconds, 0.0f, 0.25f);

    int steps = static_cast<int>(m_timeDebt / dt);
    if (steps >= std::max(settings.maxSubsteps, 1)) {
        steps      = std::max(settings.maxSubsteps, 1);
        m_timeDebt = 0.0f;
    } else {
        m_timeDebt -= static_cast<float>(steps) * dt;
    }

    for (int i = 0; i < steps; ++i) {
        recordSubstep(cmd, settings, push);
    }

    recordStats(cmd, push, res);
}

bool FluidSim::readStats(const fluid::Settings& settings, fluid::Stats& out) {
    if (m_device == nullptr || !rhi::isValid(m_statsBuffer) || m_seededRes == 0) {
        return false;
    }

    fluid::GpuStats raw{};
    m_device->readBuffer(m_statsBuffer, asWritableBytes(raw));

    if (m_referencePending) {
        m_referenceVolumeMilli = static_cast<float>(raw.volumeMilli);
        m_referencePending     = false;
    }

    const float h          = settings.cellSize();
    const float cellVolume = h * h * h;

    out.valid           = true;
    out.volume          = static_cast<float>(raw.volumeMilli) / 1000.0f * cellVolume;
    out.referenceVolume = m_referenceVolumeMilli / 1000.0f * cellVolume;
    out.maxDivergence   = static_cast<float>(raw.divergenceMicro) / 1.0e6f;
    out.maxSpeed        = static_cast<float>(raw.speedMilli) / 1000.0f;
    return true;
}

} // namespace engine
