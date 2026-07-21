# fitzel — Architecture

A portable engine base with swappable graphics backends. The point of this
repository is not features; it is the **RHI seam**: a single header that
describes everything the engine is allowed to know about the GPU.

Two backends exist — Vulkan and OpenGL 4.6 core — and they are selected at
runtime with `--backend=vulkan|opengl`, without a rebuild. The second backend
exists to prove the seam, not because the engine needs OpenGL.

## The rule

> No graphics API symbol may appear in `rhi.hpp` or in anything that includes
> it. Vulkan lives in `src/rhi/vulkan/`, OpenGL in `src/rhi/opengl/`, and
> nowhere else.

This is machine-checked. `cmake/CheckSeam.cmake` runs as the `check_seam`
target on every build and greps `src/engine/`, `src/platform/`,
`src/rhi/rhi.hpp`, `src/rhi/handle_pool.hpp`, `src/rhi/factory.cpp`, both
`*_backend.hpp` headers and `src/main.cpp`. A single hit fails the build.

Two deliberate exceptions, both naming-only:

- `rhi::Backend::Vulkan` / `rhi::Backend::OpenGL`, the values passed to
  `rhi::createDevice` and parsed from `--backend`.
- `rhi::ClientApi::OpenGLCore`, described under leak #1 below.

Neither is matched by the check's patterns (`vk[A-Z]`, `gl[A-Z]`, `GL_`, …).
Note that `GL_` is anchored to a non-identifier character: `GLFW_OPENGL_API`
contains the substring `GL_`, and GLFW is a windowing library, not a graphics
API.

## Target graph

```
                    fitzel (executable)
                   /                   \
          fitzel_engine          fitzel_rhi_factory
             /      \             /              \
   fitzel_platform   \   fitzel_rhi_vulkan   fitzel_rhi_opengl
      (glfw)          \    volk, VMA,             glad
                       \   vk-bootstrap             /
                        \        |                 /
                         fitzel_rhi (INTERFACE, header-only, no dependencies)
```

`fitzel_engine` **links no backend at all**. It sees `fitzel_rhi`, an interface
target carrying two headers and no dependencies whatsoever. The backends are
reached only through `fitzel_rhi_factory`, whose one translation unit includes
`vk_backend.hpp` and `gl_backend.hpp` — headers that are themselves free of
graphics API symbols.

`fitzel_platform` links `fitzel_rhi` for exactly one type; see leak #1.

Which backends exist is a build-time choice (`FITZEL_WITH_VULKAN`,
`FITZEL_WITH_OPENGL`, both `ON` by default). Turning either off must leave the
other working, and `rhi::isBackendAvailable` lets the CLI say so cleanly rather
than crashing.

## Layers

| Directory | Responsibility | May depend on |
|---|---|---|
| `src/platform/` | Window, input, timing | GLFW, `rhi.hpp` |
| `src/rhi/rhi.hpp` | The seam | nothing |
| `src/rhi/handle_pool.hpp` | Generational handle pool, shared by backends | `rhi.hpp` |
| `src/rhi/factory.cpp` | Backend registry | `rhi.hpp` + backend headers |
| `src/rhi/vulkan/` | Vulkan implementation | volk, VMA, vk-bootstrap, GLFW |
| `src/rhi/opengl/` | OpenGL implementation | glad, GLFW |
| `src/engine/` | Frame orchestration, app loop | `rhi.hpp`, platform |
| `src/main.cpp` | Parses `--backend`, runs the app | engine + registry |

---

# The backend contract

This is the real output of adding a second backend. Every item below is a place
where the first backend's model had leaked upward — invisibly, because with one
backend a leak and a design are indistinguishable. Each was found by asking
"what would OpenGL do here?" and each was pushed back behind the seam.

## Leak #1 — the window knew which API it was for

**Symptom.** `platform::Window` hard-coded `glfwWindowHint(GLFW_CLIENT_API,
GLFW_NO_API)`, and the engine created the window before the device. GLFW
requires the client-API hint *at window creation*: a GL context cannot be
attached afterwards, and `GLFW_OPENGL_DEBUG_CONTEXT` cannot be requested
afterwards either. So the backend choice had to move ahead of window creation
without the engine learning what a client API is.

**Fix.** `rhi.hpp` gained a small, backend-neutral description:

```cpp
enum class ClientApi : uint8_t { None, OpenGLCore };
struct WindowRequirements { ClientApi api; uint32_t majorVersion, minorVersion; bool debugContext; };
WindowRequirements windowRequirements(Backend, bool enableDebug);
```

Each backend answers for itself — Vulkan returns `ClientApi::None` because it
creates its own surface, OpenGL returns a 4.6 core context. The platform layer
executes the hints and never learns which backend asked. The engine forwards
the struct and never inspects it.

**Cost, stated honestly.** The token `OpenGLCore` now appears in `rhi.hpp`. The
alternative — a `switch (backend)` in the platform layer — keeps the header
free of it but pushes backend knowledge into a layer that had none and forces
an edit there for every future backend. The enum value was the cheaper leak.

## Leak #2 — shaders were assumed to be portable bytes

**Symptom.** `createShader(std::span<const uint32_t> spirv)`. The engine loaded
the file, knew the path layout, knew SPIR-V is 32-bit words. Worse, it could
not have been correct: the same GLSL does not compile to the same SPIR-V for
both targets. Vulkan GLSL uses descriptor sets and `push_constant`; OpenGL GLSL
uses binding points and uniform blocks. Choosing between variants is not a
decision the engine has the information to make.

**Fix.** Shaders are referenced by logical name:

```cpp
ShaderHandle createShader(std::string_view logicalName);   // "raymarch_probe"
```

`DeviceCreateInfo::shaderRoot` points at the compiled shader tree; each backend
appends its own subdirectory (`shaders/vulkan/`, `shaders/opengl/`) and its own
stage suffix. The build compiles one GLSL source twice — `glslc
--target-env=vulkan1.3 -DTARGET_VULKAN` and `--target-env=opengl
-DTARGET_OPENGL` — and stages both trees next to the executable.

`std::string_view` rather than a `ShaderId` enum, so a new shader asset does not
force an edit to `rhi.hpp`.

**Two things this turned up.** `--target-env=opengl` rejects `#version 450`; it
needs an explicit profile, `#version 450 core`. And OpenGL has no push
constants at all, so the GL backend maps the same byte blob onto a uniform
block — std140 places `vec2` at offset 0 and `float` at offset 8, which is
exactly the push constant layout. `pushConstants(std::span<const std::byte>)`
therefore stayed a blob and did not need to change.

## Leak #3 — barriers and layouts (already clean, mostly)

**Finding.** `CommandList` never had a barrier or transition method; that was
right from the start. The residue was vocabulary: `BufferUsage::TransferSrc` /
`TransferDst` are Vulkan's words for something OpenGL calls a copy.

**Fix.** Renamed to `CopySrc` / `CopyDst`. Purely cosmetic, but the vocabulary
of a seam is part of its contract — a name that only makes sense in one backend
invites the next implementer to assume that backend's model.

The substantive half of this was already in place and stayed: the Vulkan
backend emits `VkImageMemoryBarrier2` around every dispatch and blit, the
OpenGL backend emits `glMemoryBarrier` in the same two places, and the engine
issues `dispatch` and `blitToSwapchain` without knowing either exists.

## Leak #4 — `waitIdle` exported a synchronisation model

**Symptom.** `Device::waitIdle()` on the interface, called twice by
`~Application`. That is `vkDeviceWaitIdle` wearing a neutral name. OpenGL has
no equivalent — `glFinish` is a different thing — and a backend built on
implicit driver synchronisation would have to implement it as a no-op, which
means the engine was relying on a guarantee that only one backend actually
provided.

**Fix.** Removed. The contract is now: *destruction is always safe to request*.
The Vulkan backend defers the release through its frame-indexed deletion queue;
the OpenGL backend deletes immediately because the driver already tracks
references. Neither behaviour is visible above the seam.

## Leak #5 — the factory lived inside a backend

**Symptom.** `rhi::createDevice` was defined in `src/rhi/vulkan/vk_factory.cpp`
and handled exactly one enum value. With two backends there was nowhere for the
dispatch to live, and no way for `--backend` to fail cleanly on a backend that
was not compiled in.

**Fix.** `src/rhi/factory.cpp` holds the registry, guarded by `FITZEL_WITH_*`,
dispatching through the symbol-free `vk_backend.hpp` / `gl_backend.hpp`.
`rhi::isBackendAvailable(Backend)` lets `main` report an unlinked backend
instead of throwing from inside device creation.

## Leak #6 — the contract never said which corner is (0, 0)

**Symptom.** Nothing in `rhi.hpp` mentioned orientation, because with one
backend there was nothing to disagree with. `vkCmdBlitImage` addresses the
destination from the top-left; OpenGL's default framebuffer puts y = 0 at the
bottom. The two backends would have presented the same compute output mirrored
vertically.

**Fix.** The convention is now stated in `rhi.hpp` (`kTopLeftOrigin`): texel
(0, 0) is top-left, for textures and for the presented image. A backend whose
presentation surface disagrees flips on its own side — the GL backend inverts
the destination rectangle in `glBlitNamedFramebuffer`.

## Leak #7 — the contract never said which colour space either

**Symptom.** Found only by looking at both windows side by side: the Vulkan
output was visibly lighter. vk-bootstrap's default surface format selection
prefers `B8G8R8A8_SRGB`, so `vkCmdBlitImage` from a linear `RGBA16F` render
target performed a linear → sRGB *encode* on the way out. The OpenGL backend
wrote the same values into a non-sRGB default framebuffer, unconverted.
Measured on the screenshots: Vulkan 98 where OpenGL had 31, and
`sRGB_encode(31/255) · 255 = 98` exactly.

**Fix.** The convention is stated (`kPresentsUnconverted`): presentation applies
no colour space conversion. The Vulkan backend now requests
`B8G8R8A8_UNORM` explicitly instead of accepting vk-bootstrap's sRGB
preference. After the fix the two windows are pixel-identical.

**Why this one is worth dwelling on.** The numeric pixel diff *passed* while
this bug was live, because it compares the storage image — which both backends
computed identically — not the presented image. A verification harness only
covers the part of the path it actually reads. Both checks were needed.

## Considered and deliberately not changed

- **`blitToSwapchain` / `swapchainExtent`.** "Swapchain" is Vulkan and DXGI
  vocabulary; OpenGL has a default framebuffer. This is a naming leak with no
  semantic content — the GL backend maps both onto the window's default
  framebuffer in a few lines. Renaming would be churn across every call site
  for no change in what the interface guarantees.
- **`ComputePipelineDesc::pushConstantSize` and `bindings`.** Descriptor-set
  vocabulary, but the concepts map cleanly (slot → image unit, push constants →
  uniform block). Explicit bindings remain preferable to SPIR-V reflection,
  which would drag a reflection dependency to a layer that must not know what
  SPIR-V is.
- **The buffer API.** `createBuffer` / `updateBuffer` / `bindStorageBuffer` are
  implemented by both backends and used by nothing. They are unproven surface;
  see the proposals at the end of this document.

---

## RHI

`rhi.hpp` carries opaque generational handles, resource descriptors, a
`CommandList` for recording and a `Device` for the frame lifecycle.

What is deliberately **not** in the interface: barriers, image layouts,
descriptor pools, semaphores, fences, frames in flight, swapchain acquisition,
device-idle waits, and shader binaries. Every one of those is a place where the
two existing backends already disagree.

## Vulkan backend

- **volk** loads every entry point (`VK_NO_PROTOTYPES`); VMA resolves its own
  functions through the two loader pointers it is handed.
- **vk-bootstrap** handles instance, physical device selection (discrete GPU
  preferred, Vulkan 1.3 with `synchronization2`), device and swapchain.
- **VMA** owns all image and buffer allocations.

Destruction is deferred through a deletion queue tagged with
`currentFrame + framesInFlight`, which is what lets the engine recreate its
render target mid-recording without any idle wait.

Two synchronisation details, both classic sources of validation errors:
presentation semaphores are per swapchain image rather than per frame in flight
(a frame-indexed semaphore can still be pending in the presentation engine when
it is reused), and the pre-present barrier uses a `NONE` destination stage
because the semaphore signal supplies the execution dependency.

## OpenGL backend

GL is immediate-mode and single-context, so most of the Vulkan machinery has no
counterpart: `beginFrame` is bookkeeping, `endFrame` is `glfwSwapBuffers`,
there is no image to acquire and no swapchain to rebuild — the default
framebuffer follows the window on its own.

- **glad2** generates the 4.6 core loader at configure time (needs Python 3).
- The context is created *with the window* (leak #1) and adopted with
  `glfwMakeContextCurrent`.
- SPIR-V is consumed through `GL_ARB_gl_spirv`: `glShaderBinary` followed by
  `glSpecializeShader(shader, "main", …)`. The extension is checked at startup
  and its absence is a hard error.
- Synchronisation is `glMemoryBarrier` on both sides of the dispatch —
  `SHADER_IMAGE_ACCESS` before, plus `FRAMEBUFFER` and `TEXTURE_UPDATE` after —
  occupying exactly the positions the Vulkan backend puts its barriers in.
- `glDebugMessageCallback` is installed in debug builds, synchronously so a
  message names the call that produced it. The startup line reports whether the
  context actually came back with the debug bit set, so "no GL errors" is a
  checkable claim rather than an assumption.

Note that GL commonly lands on a different GPU than Vulkan: on this laptop the
Vulkan backend selects the discrete NVIDIA device while GL gets the Intel
integrated one. That made the pixel comparison a cross-vendor test by accident,
and it still agrees to one half-float ULP.

## Engine

```
beginFrame()                 -> rebuilds the swapchain if it was invalidated
swapchainExtent()            -> authoritative only now; render target resized
bindComputePipeline / bindStorageTexture / pushConstants / dispatch
blitToSwapchain(target)
endFrame()                   -> submit + present, or just swap buffers
```

The render target is resized *after* `beginFrame`, because that is the first
moment the new surface size is known. Creating and destroying resources during
recording is safe by contract.

## Verification

`--frames N` runs a fixed number of frames and shuts down normally.

`--dump <file>` writes the final frame — magic, extent, linear RGBA floats —
via `Device::readTexture`. `--compare <file>` reads one back and reports max
and mean per-component deviation, failing the process if the maximum exceeds
`--tolerance` (default 1/255). Both flags pin the animation clock, since two
runs of a time-varying shader could otherwise never agree.

```
fitzel --backend vulkan --frames 30 --dump probe.fzld
fitzel --backend opengl --frames 30 --compare probe.fzld
[compare] PASS — max 0.000488, mean 0.000000, 0/3686400 components over tolerance 0.003922
```

0.000488 is one ULP of `RGBA16F` in that range. This is the intended
foundation for golden-image tests — with the caveat recorded under leak #7:
it reads the storage image, not the presented image.

## Platforms

Windows is the primary target and is what this was verified on. The CMake
configuration is platform-neutral: no Windows SDK, no `WIN32` branches, no
per-platform source lists. On Linux, GLFW needs the usual X11 and/or Wayland
development packages at configure time. macOS is out of scope: MoltenVK for
Vulkan, and OpenGL there is capped at 4.1 — no compute shaders — so the GL
backend would need a different presentation path entirely.

## Build

CMake ≥ 3.24, all dependencies via `FetchContent` (pinned tags), C++20.

- Vulkan-Headers `vulkan-sdk-1.4.350.1`
- volk `vulkan-sdk-1.4.350.1` (built with `VK_NO_PROTOTYPES`)
- VulkanMemoryAllocator `v3.4.0`
- vk-bootstrap `v1.4.350` — must not be newer than the headers tag; its
  dispatch table references entry points from the header revision it was
  generated against
- glad `v2.0.8` (gl:core=4.6, generated at configure time — requires Python 3)
- GLFW `3.4`

The Vulkan SDK is required for the validation layers and for `glslc`; the
Vulkan headers themselves come from `FetchContent`.

## How to add a third backend

The seam has now survived one such exercise, so this is no longer speculative.

1. **Create `src/rhi/<backend>/`** with a symbol-free `<backend>_backend.hpp`
   declaring `windowRequirements(bool)` and `createDevice(const DeviceCreateInfo&)`.
2. **Add it to CMake** as its own static library, mirroring
   `fitzel_rhi_opengl`, plus a `FITZEL_WITH_<BACKEND>` option.
3. **Add the enum value** to `rhi::Backend` and a case to `factory.cpp`,
   `main.cpp`'s `parseBackend`, and `isBackendAvailable`.
4. **Add a shader variant** in `shaders/CMakeLists.txt` with its own target
   environment and defines.
5. **Extend the seam check** with the new API's symbol patterns.
6. **Verify with `--compare`** against a dump from an existing backend, and
   then look at both windows — leak #7 is the standing reminder that the
   numeric check does not cover the present path.

Things that would make this harder and were therefore avoided: exposing
resource state, letting the engine own synchronisation primitives, returning
backend pointers instead of handles, reflecting shader bindings, and leaving
orientation or colour space unstated.

## Status

Both backends render the compute probe pass and present it. Switching is
`--backend=vulkan|opengl` with no rebuild. Output is pixel-identical on screen
and agrees to one half-float ULP under `--compare`. Debug builds produce zero
Vulkan validation messages and zero GL debug messages; the build itself is
warning-free.

## Proposals, not built

- **The buffer API is still unexercised.** `createBuffer`, `updateBuffer` and
  `bindStorageBuffer` now exist twice over and are used by nothing. Either give
  the probe pass a reason to use one, or remove them until a feature needs them.
- **`--compare` should read the presented image, not just the storage image.**
  Leak #7 slipped past it. A swapchain readback would close that gap and make
  the harness a real golden-image test.
- **CI.** Windows and Linux configure/build, `check_seam`, and a
  `--backend vulkan --dump` / `--backend opengl --compare` pair as a smoke
  test. The comparison is already exit-code driven, so this is mostly YAML.
