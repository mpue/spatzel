# fitzel — Architecture

A portable engine base with a swappable graphics backend. The point of this
repository is not features; it is the **RHI seam**: a single header that
describes everything the engine is allowed to know about the GPU.

## The rule

> No `vk*`, `Vk*`, `VK_*`, `Vma*` or `VMA_*` symbol may appear in `rhi.hpp` or
> in anything that includes it. Vulkan lives in `src/rhi/vulkan/` and nowhere
> else.

This is machine-checked. `cmake/CheckSeam.cmake` runs as the `check_seam`
target on every build and greps `src/engine/`, `src/platform/`,
`src/rhi/rhi.hpp` and `src/main.cpp`. A single hit fails the build.

The one deliberate exception is the token `Vulkan` in `rhi::Backend::Vulkan` —
the enum value passed to `rhi::createDevice`. That is the only place in
engine-level code where a backend is named at all, and the check's patterns
(`vk[A-Z]`, `Vk[A-Z]`, …) do not match it.

## Target graph

```
             fitzel (executable)
              /                \
   fitzel_engine            fitzel_rhi_vulkan     <-- only Vulkan-aware target
        /      \                    |
fitzel_platform  \             volk, VMA,
   (glfw)         \            vk-bootstrap
                   \                /
                    fitzel_rhi (INTERFACE, header-only, zero dependencies)
```

`fitzel_engine` **does not link** `fitzel_rhi_vulkan`. It only sees
`fitzel_rhi`, which is an interface target carrying a single header and no
dependencies whatsoever — not even the Vulkan headers. The backend is linked in
by the executable. Swapping a backend is therefore two edits: one link line and
one enum value.

## Layers

| Directory | Responsibility | May depend on |
|---|---|---|
| `src/platform/` | Window, input, timing | GLFW only |
| `src/rhi/rhi.hpp` | The seam: handles, descriptors, `Device`, `CommandList` | nothing |
| `src/rhi/vulkan/` | The Vulkan implementation of the seam | volk, VMA, vk-bootstrap, GLFW |
| `src/engine/` | Frame orchestration, app loop | `rhi.hpp`, platform |
| `src/main.cpp` | Picks a backend, runs the app | engine + a backend |

### Platform layer

`platform::Window` is a thin RAII wrapper over GLFW. It exposes the native
window as an opaque `void*` (`nativeHandle()`); nothing above the RHI seam ever
dereferences it. The Vulkan backend links GLFW directly so it can call
`glfwCreateWindowSurface` and `glfwGetRequiredInstanceExtensions` — surface
creation is inherently a per-API concern and belongs on the backend side of the
seam, not in the platform layer.

Minimisation is handled in the platform layer (`isMinimised()`): the engine
idles on `waitEvents()` rather than pushing zero-sized frames at the backend.

### RHI

`rhi.hpp` carries opaque generational handles (`BufferHandle`, `TextureHandle`,
`ShaderHandle`, `PipelineHandle`), the descriptors needed to create those
resources, a `CommandList` for recording and a `Device` for the frame lifecycle.

What is deliberately **not** in the interface:

- barriers and image layout transitions
- descriptor sets, pools and layouts
- semaphores, fences, frames in flight
- swapchain acquisition and presentation (folded into `beginFrame`/`endFrame`)
- any notion of memory heaps beyond a three-value `MemoryAccess` enum

Every one of those is a place where backends differ enough that exposing them
would leak the first backend's model into the interface.

The one concession the interface makes to the GPU model is
`ComputePipelineDesc::bindings` — an explicit list of what descriptor set 0
contains. The alternative would be SPIR-V reflection, which would drag a
SPIR-V dependency to a layer that must not know what SPIR-V is.

### Vulkan backend

- **volk** loads every entry point; the backend is built with
  `VK_NO_PROTOTYPES`, and VMA resolves its own functions through the two loader
  pointers it is handed at allocator creation.
- **vk-bootstrap** handles instance, physical device selection (discrete GPU
  preferred, Vulkan 1.3 with `synchronization2`), logical device and swapchain.
- **VMA** owns all image and buffer allocations.

Handle → object mapping lives in `Pool<T, H>` (`vk_resources.hpp`). A handle
packs a slot index (low 24 bits, stored as index + 1 so a live handle is never
zero) and an 8-bit generation, so a stale handle is detected rather than
silently aliasing a recycled slot.

Destruction is deferred: `Device::destroy` moves the Vulkan objects into a
deletion queue tagged with `currentFrame + framesInFlight` and they are freed
once no in-flight frame can still reference them. That is what lets the engine
recreate its render target in the middle of recording without an explicit
`waitIdle`.

Two synchronisation details worth naming, because both are classic sources of
validation errors:

- Presentation semaphores are allocated **per swapchain image**, not per frame
  in flight. A frame-indexed semaphore can still be pending in the presentation
  engine when it comes up for reuse.
- The pre-present barrier uses a `NONE` destination stage and access mask; the
  semaphore signal supplies the execution dependency.

Barriers are emitted unconditionally before each dispatch rather than only on a
layout change, so back-to-back dispatches on the same image still get their
write-after-write dependency.

### Engine

`engine::Application` owns the window, the device and the probe pass. The frame
looks like this:

```
beginFrame()                 -> recreates the swapchain if it was invalidated
swapchainExtent()            -> now authoritative; render target resized to match
bindComputePipeline / bindStorageTexture / pushConstants / dispatch
blitToSwapchain(target)
endFrame()                   -> submit + present
```

The render target is resized *after* `beginFrame`, because that is the first
moment the new surface size is known. Creating and destroying resources during
recording is safe thanks to the deferred deletion queue.

`--frames N` runs a fixed number of frames and then shuts down normally, so the
full startup/render/teardown path can be exercised unattended.

## Build

CMake ≥ 3.24, all dependencies via `FetchContent` (pinned tags), C++20.

- Vulkan-Headers `vulkan-sdk-1.4.350.1`
- volk `vulkan-sdk-1.4.350.1` (built with `VK_NO_PROTOTYPES`)
- VulkanMemoryAllocator `v3.4.0`
- vk-bootstrap `v1.4.350` — must not be newer than the headers tag; its
  dispatch table references entry points from the header revision it was
  generated against
- GLFW `3.4`

Shaders are compiled GLSL → SPIR-V at build time by `cmake/CompileShaders.cmake`
(`glslc` preferred, `glslangValidator` accepted) and staged next to the
executable in `shaders/`. They are loaded at runtime as raw binaries. No
hot-reload.

The Vulkan SDK is required for the validation layers; the headers themselves
come from `FetchContent`, so the SDK is a debug-time dependency only.

### Platforms

Windows is the primary target and is what milestone 1 was verified on
(NVIDIA, discrete). The CMake configuration is platform-neutral: no Windows
SDK, no `WIN32` branches, no per-platform source lists. On Linux, GLFW needs
the usual X11 and/or Wayland development packages present at configure time;
everything else comes from `FetchContent`. macOS/MoltenVK is out of scope —
`Format::BGRA8Unorm` and the `VK_KHR_portability_subset` handling would be the
first things to revisit.

## How to add a second backend

The seam was cut so this is additive. Nothing above `rhi.hpp` changes.

1. **Create `src/rhi/<backend>/`.** This becomes the second directory allowed
   to include a graphics API header. Add it to `CMakeLists.txt` as its own
   static library — mirroring `fitzel_rhi_vulkan` — linking `fitzel_rhi`
   publicly and its API dependencies privately.

2. **Implement two classes.** A `Device` subclass and a `CommandList`
   subclass. The interface is small on purpose: nine resource entry points,
   four frame entry points, six recording entry points. The Vulkan backend is
   the reference for what belongs where — in particular, anything that looks
   like a barrier, a descriptor or a fence stays inside your `.cpp` files.

3. **Add the enum value.** `rhi::Backend` gains one entry. This is the only
   edit to `rhi.hpp` the whole exercise requires.

4. **Extend the factory.** `createDevice` currently lives in
   `src/rhi/vulkan/vk_factory.cpp` and handles exactly one enum value. With two
   backends, move it to a small `src/rhi/factory.cpp` that dispatches to
   per-backend creation functions declared in backend-private headers, guarded
   by whichever `FITZEL_WITH_<BACKEND>` definitions the build enables. Keep the
   `throw` for backends that are not linked in — a binary is allowed to ship
   with a subset.

5. **Link it.** `target_link_libraries(fitzel PRIVATE fitzel_engine fitzel_rhi_<backend>)`.
   `fitzel_engine` still links neither.

6. **Extend the seam check.** `cmake/CheckSeam.cmake` matches the Vulkan
   naming conventions; add the new API's prefixes to `_patterns` so the same
   guarantee holds for it.

Things that would make this harder and were therefore avoided: exposing
concrete resource state in the interface, letting the engine own
synchronisation primitives, returning backend pointers instead of handles, and
reflecting shader binding layouts instead of declaring them.

## Status

Milestone 1 is complete and verified on Windows / NVIDIA RTX 4070 Laptop:
window opens, resizes and minimises correctly (swapchain recreated), the
compute pass writes an animated UV/time pattern into an `RGBA16Float` storage
image, the image is blitted to the swapchain and presented, shutdown is clean,
and a full run under the validation layers produces zero errors and zero
warnings.
