#pragma once

// The fluid solver's GPU side: it owns the field buffers and the nine compute
// pipelines, and records one frame's worth of simulation into a command list.
//
// It sees rhi.hpp and nothing else — no backend, no window, no ImGui. The
// Application decides *when* to step and what the settings are; this class
// decides what a step consists of and in what order the passes run. The split
// matters because the order IS the algorithm, and it should be readable in one
// place (recordStep) rather than spread through the frame loop.
//
// Nothing is allocated until the fluid is first switched on.

#include "engine/fluid.hpp"
#include "rhi/rhi.hpp"

#include <array>
#include <span>

namespace engine {

class FluidSim {
public:
    FluidSim() = default;
    ~FluidSim();

    FluidSim(const FluidSim&)            = delete;
    FluidSim& operator=(const FluidSim&) = delete;

    // Creates the pipelines and the parameter buffer. The large field buffers
    // are deferred to the first enabled frame.
    void create(rhi::Device& device);
    void destroy();

    // Re-seed the level set and zero the velocity on the next recorded frame.
    // Also re-establishes the reference volume the drift figure is measured
    // against, so a reset is the only thing that moves that baseline.
    void requestReset() { m_needSeed = true; }

    // The obstacle field is a sample of the scene distance field, so it is
    // stale whenever the edit list or the domain changes.
    void invalidateSolids() { m_needSolids = true; }

    // Records the obstacle bake (if stale), the reset (if requested), the
    // substeps for `frameSeconds` of wall clock, and the diagnostics pass. Does
    // nothing at all when the settings say the fluid is off.
    //
    // `primitiveCount` is the live edit-list length: the obstacle bake reads
    // the same buffer the marchers do, bound by the caller at slot 1.
    void recordStep(rhi::CommandList& cmd, const fluid::Settings& settings,
                    rhi::BufferHandle sceneBuffer, int primitiveCount, float frameSeconds);

    // Pulls the diagnostics block back. Blocking — it is a verification path,
    // like the brick bake's stats readback — so the caller decides how often it
    // is worth a stall. Returns false when nothing has been simulated yet.
    [[nodiscard]] bool readStats(const fluid::Settings& settings, fluid::Stats& out);

    // --- what the marchers bind -------------------------------------------
    [[nodiscard]] rhi::BufferHandle paramsBuffer() const { return m_params; }

    // The level set the last recorded step left current. Before the fields
    // exist this returns the parameter buffer instead of an invalid handle: the
    // marchers declare slot 11 unconditionally, so something must be bound
    // there, and with `enabled = 0` in the parameter block the shader's fluid
    // path returns before it ever indexes it. A dummy binding is the price of
    // not compiling two variants of each marcher.
    [[nodiscard]] rhi::BufferHandle phiBuffer() const {
        return rhi::isValid(m_phi[m_phiIndex]) ? m_phi[m_phiIndex] : m_params;
    }

    // Uploads the render-side parameter block. Cheap; call it whenever a fluid
    // setting changes (and once before the first frame).
    void uploadParams(const fluid::Settings& settings);

    // True once the field buffers exist, i.e. the fluid has been on at least
    // once this run.
    [[nodiscard]] bool resident() const { return rhi::isValid(m_phi[0]); }

private:
    struct Pass {
        rhi::ShaderHandle   shader   = rhi::ShaderHandle::Invalid;
        rhi::PipelineHandle pipeline = rhi::PipelineHandle::Invalid;
    };

    // Binding slots, shared with fluid_common.glsl. Named rather than spelled
    // out at every call site: nine passes binding the same eight buffers is
    // exactly where a transposed pair of numbers would hide.
    enum Slot : uint32_t {
        kSlotScene      = 1,
        kSlotParams     = 8,
        kSlotVelSrc     = 9,
        kSlotVelDst     = 10,
        kSlotPhiSrc     = 11,
        kSlotPhiDst     = 12,
        kSlotPressure   = 13,
        kSlotDivergence = 15,
        kSlotSolid      = 16,
        kSlotStats      = 17,
    };

    Pass makePass(const char* name, std::span<const rhi::BindingDesc> bindings);
    void ensureFields();
    void destroyFields();

    [[nodiscard]] fluid::GpuPush pushFor(const fluid::Settings& settings,
                                         int primitiveCount) const;

    void recordSeed(rhi::CommandList& cmd, const fluid::GpuPush& push, int res);
    void recordSolids(rhi::CommandList& cmd, const fluid::GpuPush& push, int res,
                      rhi::BufferHandle sceneBuffer);
    void recordSubstep(rhi::CommandList& cmd, const fluid::Settings& settings,
                       const fluid::GpuPush& push);
    void recordStats(rhi::CommandList& cmd, const fluid::GpuPush& push, int res);

    rhi::Device* m_device = nullptr;

    Pass m_seedPass;
    Pass m_solidsPass;
    Pass m_advectPass;
    Pass m_divergencePass;
    Pass m_pressurePass;
    Pass m_projectPass;
    Pass m_extrapolatePass;
    Pass m_reinitPass;
    Pass m_statsPass;

    // Ping-pong pairs. The index is which half of the pair currently holds the
    // live field; a pass reads that one and writes the other, then flips.
    std::array<rhi::BufferHandle, 2> m_velocity{rhi::BufferHandle::Invalid,
                                                rhi::BufferHandle::Invalid};
    std::array<rhi::BufferHandle, 2> m_phi{rhi::BufferHandle::Invalid, rhi::BufferHandle::Invalid};
    int m_velocityIndex = 0;
    int m_phiIndex      = 0;

    // Single-buffered, unlike the two above: the red-black sweep updates the
    // pressure in place (see fluid_pressure.comp), and it is warm-started from
    // the previous step rather than cleared, so a settled pool starts each solve
    // from the answer that was already holding it up.
    rhi::BufferHandle m_pressureBuffer   = rhi::BufferHandle::Invalid;

    rhi::BufferHandle m_divergenceBuffer = rhi::BufferHandle::Invalid;
    rhi::BufferHandle m_solidBuffer      = rhi::BufferHandle::Invalid;
    rhi::BufferHandle m_statsBuffer      = rhi::BufferHandle::Invalid;
    rhi::BufferHandle m_params           = rhi::BufferHandle::Invalid;

    bool m_needSeed   = true;
    bool m_needSolids = true;

    // The resolution the live fields were seeded at. Changing the resolution
    // reinterprets every index, so it forces a re-seed rather than resampling —
    // resampling a level set between resolutions is a real piece of work and
    // not one this stage needs.
    int m_seededRes = 0;

    // Leftover wall-clock time not yet consumed by a fixed-size substep. Kept
    // so the simulation clock tracks real time without the timestep tracking
    // the frame rate.
    float m_timeDebt = 0.0f;

    // The volume measured right after the last seed, in thousandths of a cell —
    // the baseline the drift figure is quoted against.
    float m_referenceVolumeMilli = 0.0f;
    bool  m_referencePending     = false;
};

} // namespace engine
