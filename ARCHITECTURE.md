# fitzel — Architecture

A portable engine base with swappable graphics backends, rendering a
signed-distance-field scene two ways: a brute-force reference raymarcher and a
sparse-brick-accelerated renderer validated against it.

Three things in here are load-bearing:

- the **RHI seam** — a single header describing everything the engine is
  allowed to know about the GPU. Two backends exist, Vulkan and OpenGL 4.6
  core, selected at runtime with `--backend=vulkan|opengl` without a rebuild.
  The second backend exists to prove the seam, not because the engine needs
  OpenGL.
- the **reference renderer** — a deliberately unaccelerated raymarcher that
  defines what correct output looks like for a given scene description.
- the **brick renderer** — an accelerated path that marches a baked, cached
  copy of the field. It is correct exactly insofar as it reproduces the
  reference, within a documented tolerance; the reference is what makes that a
  checkable claim rather than an assertion.

The first two exist to make the third safe rather than to be fast.

On top of these sits an **editor**: a Dear ImGui panel that changes the scene at
runtime and saves and loads it as JSON. It edits the parametric edit list
directly — that list stays the single source of truth — and its UI reaches the
screen through the same seam, so no graphics-API symbol crosses it for the UI
either.

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
- **The buffer API.** `createBuffer` / `updateBuffer` / `bindStorageBuffer` were
  unproven surface at the time of the second backend. They are no longer: the
  edit list exercised them, and the brick renderer leans on them hard —
  alongside the `clearBuffer` / `readBuffer` and dispatch-ordering additions the
  brick milestone made (see the RHI section).

---

## RHI

`rhi.hpp` carries opaque generational handles, resource descriptors, a
`CommandList` for recording and a `Device` for the frame lifecycle.

What is deliberately **not** in the interface: barriers, image layouts,
descriptor pools, semaphores, fences, frames in flight, swapchain acquisition,
device-idle waits, and shader binaries. Every one of those is a place where the
two existing backends already disagree.

### What the brick milestone added to the contract

Building the brick renderer needed three things the seam could not express. Each
is a contract change stated in `rhi.hpp`, not a reach into a backend — the same
discipline the second backend established.

- **Dispatch ordering.** A multi-pass compute algorithm needs pass *N+1* to see
  what pass *N* wrote. Both backends already covered the image hazard between
  two dispatches; neither covered storage buffers. Since barriers are absent
  from the interface, the caller cannot fix that itself, so the guarantee is now
  contractual: *everything a dispatch (or a `clearBuffer`) writes is visible to
  every dispatch recorded after it.* Vulkan emits a global `VkMemoryBarrier2`
  before each dispatch; GL adds the storage-buffer and buffer-update bits to the
  barrier it already issued. Ordering only — never a synchronisation primitive
  the caller can name.
- **`clearBuffer`.** The bake's bump allocator needs its counter zeroed before
  the atomics run. Doing that with a host write would pin the counter in
  host-visible memory, the worst place for an atomically-updated buffer. So the
  seam gained a GPU-side zero (`vkCmdFillBuffer` / `glClearNamedBufferData`),
  ordered like a dispatch.
- **`readBuffer`.** The mirror of `readTexture`: a blocking, verification-only
  readback so what the bake wrote can be inspected, not merely believed. The
  source must carry `BufferUsage::CopySrc`. Both backends stage a device-local
  buffer through a host copy — on GL that also sidesteps the driver's
  video→host migration warning, keeping the bake free of debug messages.

### And what the editor milestone added

The debug UI overlay is the fourth seam addition: `Device::initUi` /
`beginUiFrame` / `shutdownUi` and `CommandList::endUiFrame`, carrying no ImGui
types. The engine drives Dear ImGui's neutral core; the backend owns the
API-specific render backend and draws the built frame over the presented image.
`initUi` returning false is a first-class outcome — the OpenGL backend does
exactly that and the engine runs without an overlay. The full rationale is under
"The editor".

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

---

# The renderer

## Scene representation: the edit list is the truth

The scene is a small `std::vector<GpuPrimitive>` built on the CPU
(`src/engine/scene.cpp`), uploaded once into a storage buffer, and evaluated by
the compute kernel. **Nothing about the scene is baked into the shader body** —
the kernel iterates a list it knows nothing about.

```cpp
struct alignas(16) GpuPrimitive {
    float   position[4];  // xyz = translation, w = smooth-union blend radius
    float   rotation[4];  // unit quaternion
    float   params[4];    // per type
    float   albedo[4];    // rgb
    int32_t control[4];   // x = PrimitiveType, y = Operator
};
```

Every member is a 16-byte slot. That makes std430 (the Vulkan variant's storage
buffer) and std140 agree, and leaves the C++ struct byte-identical to its GLSL
counterpart without a single padding calculation. The same trick is used for
the camera uniforms: four `vec4`s with the scalars tucked into their `.w`
components, because std140 aligns a `vec3` to 16 bytes anyway.

Parameter conventions are shared between `scene.hpp` and `raymarch.comp`:

| Type | `params` |
|---|---|
| Sphere | `x` = radius |
| Box | `xyz` = half extents, `w` = corner rounding |
| Torus | `x` = major radius, `y` = minor radius |
| Plane | `xyz` = unit normal, `w` = offset |

**There is no scale, deliberately.** A non-uniform scale destroys the distance
metric that sphere tracing depends on: the field stops being a true distance
function, and the marcher either overshoots through surfaces or crawls.
Translation and rotation are what an SDF primitive can carry exactly, so those
are what the transform holds.

Operators are `Union` and `SmoothUnion` (polynomial `smin`). `smin` returns its
mix factor alongside the blended distance so the albedo can follow the same
blend — which is what makes the transition zone legible instead of only
visible in silhouette.

This is also the first real consumer of the RHI buffer API
(`createBuffer` / `updateBuffer` / `bindStorageBuffer`), which had been carried
unproven since milestone 1.

## The reference renderer

The raymarcher is **brute force on purpose**. Every distance query evaluates
every primitive in the list; there is no acceleration structure, no spatial
subdivision, no caching. Normals cost six more full evaluations per shaded
pixel.

That is not a shortcut to be optimised away later — it is the point. This
renderer is the definition of correct for this scene representation. The
brick-accelerated path that comes next consumes the same edit list and is
correct exactly insofar as it reproduces this one, pixel for pixel. Any
divergence is a bug in the accelerated path, never a disagreement between two
equally valid renderers.

Which is why the verification harness from the previous milestone matters here:
`--dump` / `--compare` already reads a render target back and diffs it against
a reference within a tolerance. The accelerated renderer inherits that harness
unchanged.

The marching loop itself: 192 steps maximum, a hit threshold that widens with
distance (a texel covers more world space further out, so demanding a fixed
epsilon there only wastes steps), and a fade into the background near the march
limit so the unbounded ground plane does not end in a hard line.

Shading is one directional light plus a hemispherical ambient term. No shadows,
no ambient occlusion — both were offered and both were declined, because
neither is needed to validate the path.

---

# The brick renderer

The accelerated path. The reference marches the edit list at every distance
query; the brick renderer marches a **baked, cached copy** of the field and
consults the edit list only once per shaded pixel, for the hit albedo. The
reference stays as ground truth and both are switchable at runtime (keys `1`
and `2`, or `--renderer`).

## What is shared, and why it must be

The scene SDF — the primitive distance functions, the operators, the fold over
the edit list — lives in `shaders/sdf_scene.glsl`, and the shading model in
`shaders/shading.glsl`. The reference marcher, both bake passes and the brick
marcher all `#include` them. This is load-bearing: "the brick path reproduces
the reference" is only meaningful if the two evaluate *the same* field. A second
copy of the evaluation would let the claim decay into "two implementations that
happen to agree today". The build compiles each `.comp` twice as before; a
changed include rebuilds every dependent module (glslc's `-MD` depfile, plus an
explicit `INCLUDES` list for glslangValidator, which emits none).

## The structure

Constants live once in `shaders/brick_common.glsl` and are mirrored in
`src/engine/brick.hpp`; the two must agree byte for byte.

- **Bounded AABB, dense top-level index.** A fixed `64³` grid covers a bounded
  world AABB (`brick::kAabbMin`/`kAabbMax`). One `Cell { int brickSlot; float
  emptyDistance; }` per grid cell, in a single storage buffer indexed
  `z·64² + y·64 + x`. `brickSlot < 0` marks an **empty** cell — one the bake
  proved holds no surface — and `emptyDistance` is then the signed scene
  distance at the cell centre, the basis for empty-space skipping. Otherwise
  `brickSlot` indexes the pool.
- **Sparse brick pool.** Only surface-adjacent cells take a slot. A brick is an
  `8³` block of interior voxels plus a **one-voxel apron** on every side —
  `10³ = 1000` floats — in a flat pool buffer, slot `s` at `[s·1000, s·1000 +
  1000)`. Voxels are **cell-centred**: interior voxel 0 sits half a voxel inside
  the cell's minimum corner, so the apron voxels straddle the cell boundary.
  That is exactly what lets a trilinear read *anywhere inside the cell* reach
  only the eight samples this brick owns, with no cross-brick fetch and no seam
  where two bricks meet.
- **Fixed pool.** `kPoolCapacity` slots (`24576`, ~94 MiB). The fixed scene
  bakes to ~16.6k occupied cells; the ground plane dominates, because the
  conservative occupancy test uses a cell's 3D diagonal and a flat plane still
  claims several vertical layers. Overflow is counted and reported as a hard
  failure — never silently truncated.

Everything is a `std430` storage buffer, not a 3D texture: the trilinear filter
is done by hand, so the RHI needs no sampler or 3D-image vocabulary.

## The bake — two compute passes

Recorded once, ahead of the first frame's marcher, into the same command list.
The pass ordering is the seam's guarantee (see the RHI section), so the marcher
in that frame already sees a finished bake.

1. **`bake_classify.comp`** — one thread per cell. It evaluates the shared scene
   SDF at the cell centre. A surface farther from the centre than the distance
   to the farthest sampled corner (`length(½·cell + ½·voxel)`) cannot reach any
   voxel, because the field is **Lipschitz-1** (both `min` and the polynomial
   `smin` are). So `abs(d) ≤ that radius` is a *conservative* occupancy test: it
   never drops a cell that holds a surface, with no tuned fudge factor. Occupied
   cells claim a slot from a bump allocator (`atomicAdd` on a stats buffer that
   `clearBuffer` zeroed first); empty cells store the centre distance.
2. **`bake_fill.comp`** — one workgroup per cell, one thread per voxel (`10³`).
   Empty cells early-out; occupied cells sample the shared scene SDF at each
   voxel's cell-centred world position and write it into the brick.

After the frame is submitted, the host reads the stats buffer back
(`Device::readBuffer`) and logs occupancy and overflow. `--debug-view 2` (or key
`3`) tints brick-hit pixels, a direct visual check that occupancy matches the
silhouettes.

## The march

`raymarch_brick.comp` DDAs the top-level grid. In an **empty** cell — provably
surface-free — it advances at least to the cell's far face, and further when the
Lipschitz bound `abs(emptyDistance) − dist(p, centre)` allows a bigger leap. In
an **occupied** cell it sphere-traces the trilinearly-sampled brick, but never
steps past the cell's far face: a brick only describes surfaces within its own
cell and apron, so a larger reported distance must not be trusted to leap over a
neighbour's surface. A ray exactly parallel to an axis makes the slab arithmetic
`0·∞ = NaN` on a cell boundary; the components of the direction are nudged off
zero to keep that from seaming the image down the middle.

Normals come from the **gradient of the brick field** (central differences at
half a voxel, which keeps every sample inside the brick's valid domain). Albedo
is a single edit-list evaluation at the hit, because bricks store distance only
— material is a non-goal here.

`--debug-view 1` (or cycling with key `3`) renders a **step-count heat map**:
sky and near ground come back cheap (few steps, the empty-space skipping
working), the horizon grazing band and the object silhouettes cost more.

## Reference vs brick: the tolerance

The reference is correct by definition, so every difference is the brick path's
to account for. Measured on the pinned comparison camera, `1280×720`:

```
fitzel --backend vulkan --renderer reference --frames 5 --dump ref.fzld
fitzel --backend vulkan --renderer brick     --frames 5 --compare ref.fzld --max-outlier-fraction 0.15
[compare] PASS — max 0.814453, mean 0.006022, 340818/3686400 (9.25%) components over tolerance 0.003922, outlier budget 15.0000%
```

This is deliberately **not** a max-error test, and the reason is structural. Two
distinct differences exist, neither a bug:

- **Trilinear normal shading.** The brick field's gradient approximates the
  analytic normal by a few degrees on curved surfaces, which shifts the diffuse
  term by a few `1/255` across the whole lit surface — not just at silhouettes.
  This is the "trilinear filter error" the milestone anticipated. The *mean*
  deviation is ~1.5/255; the surfaces are visually indistinguishable.
- **The finite AABB and the infinite plane.** The ground plane is unbounded, but
  the brick grid is not. Beyond the AABB the brick marcher shows background
  where the reference shows ground fading toward the horizon — a band near the
  horizon line. The AABB is sized wide enough (`±20` horizontally) that the band
  falls where the reference has already begun fading the ground out, keeping its
  amplitude down, but it cannot be eliminated without a hierarchy the non-goals
  exclude. Note that the plane's SDF is *linear*, so trilinear reproduces it
  **exactly** inside the AABB — the near ground matches to a fraction of a level.

So the acceptance test is an **outlier budget**: `--max-outlier-fraction` passes
the comparison when at most that fraction of components exceed `--tolerance`.
The two effects above put ~9% of components over `1/255`; the budget is set to
`0.15` with headroom. At its default of `0` the flag is inert and `--compare`
keeps its strict cross-backend max-error behaviour unchanged. Because the bake
and march are deterministic, the brick image is **bit-identical across
backends** — GL brick vs Vulkan brick is `max 0.000000`.

## What this deliberately is not

No GPU-driven or dynamic brick allocation (the bump allocator runs once, within
one static bake), no hashed or hierarchical index, no streaming or paging, no
incremental re-bake, no runtime sculpting, no LOD or mip-bricks, no material in
the bricks, and no performance-tuning pass. A dense top-level index over a
bounded AABB is the whole of v1; the structural win is the point, not the
numbers.

---

# The editor

A panel to change the scene at runtime — select a primitive, edit its numbers,
add and delete, undo/redo, and save/load — built so that an interesting or
broken arrangement can be captured to a file and reproduced exactly.

## The edit list stays the one source of truth

The editor edits the `std::vector<GpuPrimitive>` the renderers already consume,
in place. There is deliberately **no second scene representation**: no editor
document, no node graph, no shadow copy that could drift from the list. The undo
history is the one place whole-scene *snapshots* exist, and those are history,
not an alternative present state — a parametric list makes a full snapshot cheap
enough that this is the whole undo implementation.

## Data flow

```
ImGui number field ─► GpuPrimitive in the edit list ─► scene storage buffer ─► renderer
                                                    └─► (brick path) full re-bake
```

Every mutation — a committed field edit, an add/delete, an undo/redo, a load —
does two things: re-upload the active prefix of the edit list to its storage
buffer, and request a re-bake. The **reference renderer needs nothing else**; it
re-evaluates the list every frame, so the change is simply visible next frame.
The **brick renderer re-bakes in full** — there is no incremental or local
re-bake here, on purpose. For the fixed-scale scenes this is a few milliseconds,
reported in the panel.

Two details make this usable rather than merely correct:

- **The scene buffer is allocated at capacity** (`kMaxPrimitives`), not at the
  current size. Adding a primitive never reallocates a GPU resource mid-frame;
  only `primitiveCount` and the uploaded prefix change, and both renderers and
  the bake already key off `primitiveCount`.
- **A live drag re-bakes every frame but is not timed.** Timing the re-bake
  means reading the bake stats back, which stalls the device; doing that every
  frame of a drag would make dragging lurch. So the stats readback (and the
  reported re-bake time) happens only on a *committed* edit — a released drag, an
  add, a load — while the intermediate frames re-bake unmeasured. The brick image
  still follows the drag live.

## The UI seam

Dear ImGui's core API is backend-neutral — its symbols are plain `ImGui::` — but
its rendering is not. The split follows the same seam as everything else:

- **The engine** owns the ImGui context and issues only `ImGui::` calls
  (`CreateContext`, `NewFrame`, the widgets, `Render`). It names no graphics API.
  `engine/editor.cpp` is entirely `ImGui::` and edit-list manipulation.
- **`rhi.hpp`** carries four neutral hooks and no ImGui types:
  `Device::initUi` (returns false if the backend has no overlay),
  `Device::beginUiFrame`, `Device::shutdownUi`, and `CommandList::endUiFrame`
  (draw the built frame over the presented image, after `blitToSwapchain`).
- **The Vulkan backend** owns `imgui_impl_vulkan` and draws the overlay onto the
  swapchain image with dynamic rendering — loading, not clearing, so it sits on
  top of the blitted frame. This is why the swapchain now creates image views
  and the device enables the `dynamicRendering` feature.
- **The platform layer** owns `imgui_impl_glfw` — the input half, which is pure
  windowing and touches no graphics API, so it belongs beside the window and not
  behind the seam. When the pointer or keyboard is over the panel
  (`io.WantCaptureMouse/Keyboard`) the engine withholds that input from the
  camera and the renderer hotkeys.
- **The OpenGL backend** returns false from `initUi` and the engine runs without
  an overlay — the milestone only requires the UI on Vulkan, and this keeps the
  non-UI backend building and running untouched. A GL overlay would be a small
  addition (`imgui_impl_opengl3`), not a design change.

No `imgui_impl_<api>` header is ever included above `src/rhi/<backend>/`, and the
seam check passes unchanged: `ImGui`, `Ui`, and `imgui_impl_glfw` match none of
its `vk[A-Z]` / `gl[A-Z]` / `GL_` patterns.

## Scene files

`engine/scene_io.cpp` writes and reads the edit list as JSON (nlohmann/json),
with word-named types and operators so a person can read and hand-edit a file.
Loading returns the same `std::vector<GpuPrimitive>` the renderer consumes —
again, no second representation. Reproducibility is the point and is checked: a
scene dumped from the built-in `buildScene()`, saved, and reloaded through
`--scene` renders bit-identically (`max 0.000000`). Example scenes live in
`scenes/` and are staged beside the executable; the editor's *Examples* list
loads them with one click, and `--save-scene` seeds them from the canonical
scene. The overlay is forced off during `--dump` / `--compare` so it never lands
in a compared image — the accelerated-renderer A/B harness stays valid.

## What this deliberately is not

No picking in the viewport and no drag gizmos — selection is by list, editing by
number field; both are natural later additions. No sculpting, brushes or
destructive detail layers; no incremental re-bake; no material, light, mover or
animation editing; no multi-select, copy/paste, grouping or asset browser; no
docking or multi-window UI. Rotation is edited as a normalised quaternion rather
than Euler angles or a gizmo, which keeps the schema unchanged. And there is no
scale: the scene has no scale transform (a non-uniform one breaks the distance
metric — see the renderer section), so a primitive's size is edited through its
type-specific dimensions, not a transform.

## Camera and input

`platform::InputState` is a neutral snapshot refreshed by `pollEvents`: key and
button state plus a cursor delta, with GLFW key codes staying inside the
platform layer. Consumers read state rather than subscribing to events, because
everything driven by input so far is continuous rather than discrete.

Look is hold-to-engage on the right mouse button — the cursor is captured only
while it is held, so the window stays resizable and alt-tab needs no special
handling. Raw motion is enabled during capture, and the reference position is
re-seeded on every transition so the first frame after one reports no movement.

`engine::FlyCamera` consumes that snapshot and knows nothing about GLFW. It
stores yaw and pitch rather than a basis, which makes roll structurally
impossible. Horizontal motion follows the view, vertical follows the world, so
looking down does not drag the camera into the floor. WASD, Q/E for vertical,
left shift to boost.

Delta time is clamped to 100 ms, so a stall — a breakpoint, a swapchain rebuild
— cannot teleport the camera on the frame after it. In a pinned run
(`--dump` / `--compare`) the camera is frozen along with the clock, for the same
reason: a free-flying camera would make two runs incomparable.

`src/engine/math.hpp` is deliberately not a maths library. When something needs
matrices, quaternion slerp or SIMD, that is the moment to pull in a real one
rather than to grow that file.

---

## Engine

```
beginFrame()                 -> rebuilds the swapchain if it was invalidated
swapchainExtent()            -> authoritative only now; render target resized
[first frame only] recordBake -> clearBuffer + classify + fill dispatches
bindComputePipeline / bindStorageTexture / bindStorageBuffer / clearBuffer / pushConstants / dispatch
blitToSwapchain(target)
endFrame()                   -> submit + present, or just swap buffers
[after first frame] readBuffer(stats) -> log occupancy / overflow
```

The render target is resized *after* `beginFrame`, because that is the first
moment the new surface size is known. Creating and destroying resources during
recording is safe by contract.

The bake is recorded into the first frame's command list, before that frame's
marcher; the dispatch-ordering guarantee is what lets the marcher read a bake
that was written moments earlier in the same list. The stats readback happens
*after* `endFrame` submits, since `readBuffer` stalls the device until the work
it is reading has completed. One subtlety the multi-pipeline bake surfaced: the
Vulkan command list now clears its pending bindings when a new pipeline is
bound, because a frame that runs classify, fill and the marcher in turn would
otherwise carry a slot from one pipeline's set layout into the next and trip
validation. The caller re-binds after every `bindComputePipeline` regardless, so
this costs nothing.

## Verification

`--frames N` runs a fixed number of frames and shuts down normally.

`--dump <file>` writes the final frame — magic, extent, linear RGBA floats —
via `Device::readTexture`. `--compare <file>` reads one back and reports max and
mean per-component deviation. It fails the process when the maximum exceeds
`--tolerance` (default 1/255) — unless `--max-outlier-fraction f` is given, in
which case it passes as long as at most fraction `f` of components exceed the
tolerance. Both flags pin the animation clock, and the pinned run also freezes
the renderer selection, since two runs of a time-varying shader could otherwise
never agree.

```
fitzel --backend vulkan --frames 30 --dump probe.fzld
fitzel --backend opengl --frames 30 --compare probe.fzld
[compare] PASS — max 0.000488, mean 0.000000, 0/3686400 components over tolerance 0.003922
```

0.000488 is one ULP of `RGBA16F` in that range. The strict max test is right for
the cross-backend comparison, where the same shader must agree to a ULP; the
outlier-fraction form is what the reference-vs-brick A/B needs, where a small,
structural set of components legitimately differs (see the brick renderer's
tolerance section). This is the intended foundation for golden-image tests —
with the caveat recorded under leak #7: it reads the storage image, not the
presented image.

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
- Dear ImGui `v1.91.8` — the editor overlay. Ships no CMake of its own, so the
  core and the GLFW backend are compiled into libraries here; the Vulkan render
  backend is compiled inside `fitzel_rhi_vulkan` with `IMGUI_IMPL_VULKAN_USE_VOLK`
  so it resolves entry points through volk like the rest of the backend.
- nlohmann/json `v3.11.3` — human-readable scene files.

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

Both backends build and run, on Vulkan and OpenGL, switchable with
`--backend=vulkan|opengl` and no rebuild. Both renderers render the fixed scene
— a plane, a torus, two spheres joined by a smooth union, and a rotated rounded
box — with live free-fly navigation:

- The **reference** brute-force raymarcher, unchanged in behaviour (its scene
  and shading moved into shared includes, verified bit-identical).
- The **brick** renderer, baking the field once at startup and marching the
  sparse structure. Runtime `1`/`2` switch renderers, `3` cycles the brick debug
  views; `--renderer` and `--debug-view` set them from the CLI.

On Vulkan, the **editor overlay** runs on top: select a primitive from the list,
edit its numbers, add/delete, undo/redo, switch renderer, and save/load scenes
as JSON. Editing re-uploads the edit list and re-bakes the brick path live; the
panel reports FPS, the active renderer and the last re-bake time. The OpenGL
backend runs without the overlay, unchanged. `--scene` loads a scene file at
startup and `--save-scene` writes one and exits.

Reference-vs-brick agrees within the documented outlier tolerance
(`--max-outlier-fraction`); the brick image is bit-identical across the two
backends, and a saved scene reloads bit-identically. Debug builds produce **zero
Vulkan validation messages and zero GL debug messages** across the bake, both
render paths and the overlay.

One build warning remains and is **not** from this work: MSVC emits `LNK4098`
(a `LIBCMT` CRT-mix from a `FetchContent` dependency) on the Visual Studio
toolchain used here. It is present on the milestone-2 tip too — verified by
building that commit — and is a toolchain/dependency artifact, not a code issue.

Cross-backend pixel parity of the *reference* was a milestone-2 criterion and is
no longer one; both backends currently agree to a ULP, but that is not a
maintained guarantee.

## Proposals, not built

- **`--compare` should read the presented image, not just the storage image.**
  Leak #7 slipped past it. A swapchain readback would close that gap and make
  the harness a real golden-image test — which matters more now that the
  harness is the acceptance test for the accelerated renderer.
- **A golden reference committed to the repository.** `--dump` output for a
  fixed camera pose, so the reference renderer is pinned against regression and
  not only against itself.
- **CI.** Windows and Linux configure/build, `check_seam`, and a `--dump` /
  `--compare` pair as a smoke test. The comparison is already exit-code driven,
  so this is mostly YAML.
