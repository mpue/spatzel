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

## How to add a second backend

*(filled in once the Vulkan backend is complete)*
