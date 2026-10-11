# vulkan4xenon: scope (draft, 2026-10-11)

**Goal.** A Vulkan 1.0 subset for the Xbox 360: a static library on the SDK's GPU layer, for ports, such as recompiled
games, that run on the retail kernel from Aurora.

**The template.** vulkan4vita is the layout to follow (vulkan4vita:docs/ROADMAP.md:3-30): what it did on the Vita's
native GPU API, GXM, this does on Xenos through the SDK's GPU layer. Its Vita specifics don't carry over.

**Status.** A draft for agreement. Nothing here is built. Section 6 lists what needs a decision.

**Constraints.** The SDK's, unchanged:
- No Microsoft tooling, no leaked SDK material, no OpenXeChain.
- Every claim about Xenos cites Xenia, libxenon, xost or a hardware result.
- The console decides: a milestone is done only on a clean hardware run. Xenia is for smoke checks.

**Sources.**
- vulkan4vita, cited as `vulkan4vita:<path>:<line>` (Z:\vulkan4vita at `37af886`).
- The SDK, cited by path. What the console has settled is in `docs/HARDWARE.md`.
- xost, cited as `xost:<path>:<line>`, and Xenia and libxenon as in SCOPE.md.

## 1. What carries over from vulkan4vita

- **A static library.**
  - Apps call `vk*` directly; `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` work too
    (vulkan4vita:docs/ROADMAP.md:10-13).
  - It reports Vulkan 1.0, refuses 1.1 and up, and claims no conformance (vulkan4vita:docs/SUPPORT.md:5-7).
- **Khronos headers, vendored.** vulkan4vita carries them in `third_party/vulkan-headers/`. They're Apache-2.0, as the
  SDK's rules allow.
- **docs/SUPPORT.md** is the one list of what exists, every limit below the spec's minimum, and every deviation, each
  with a reason. It changes in the same commit as the code (vulkan4vita:CODE_GUIDELINES.md:131-138).
- **Milestones written as vulkan4vita writes them** (vulkan4vita:docs/ROADMAP.md:3-64):
  - **Done when:** criteria a hardware run can check.
  - **Passed on hardware** on a date, at a commit, with the results' folder.
  - **Decisions**, **How it maps onto Xenos**, **New functions**, **New tests** and **Not in milestone N**.
- **Feature tests** in one title, each checking pixels of the presented image, as vulkan4vita's `VKVT00001` does
  (vulkan4vita:README.md:157-162). They're broken on purpose before a hardware run, as the SDK's tests are.
- **A benchmark against the raw GPU layer.** vulkan4vita's first step ran the same scenes through raw GXM, vitaGL and
  itself (vulkan4vita:README.md:12-48). Here the raw GPU layer is the ceiling; there's no vitaGL to compare with.
- **Lessons that transfer:**
  - Where the GPU's own ordering isn't proven, wait on the CPU. GXM's sync flags didn't order transfers against scenes
    on hardware (vulkan4vita:docs/ROADMAP.md:203-205).
  - Measure vendor fast paths before keeping them: GXM's precomputed draws didn't pay, and that milestone closed
    (vulkan4vita:docs/ROADMAP.md:322-395).
  - Check the accounting before the ratio: a miscounted present skewed every early benchmark
    (vulkan4vita:docs/ROADMAP.md:264-271).
  - A passing test can hide an app that hangs, as the boot logo's present chain did
    (vulkan4vita:docs/ROADMAP.md:729-733).

## 2. Decided

- **Shaders are compiled offline and looked up by their SPIR-V** (the user, 2026-10-11).
  - Apps keep standard SPIR-V.
  - A build step runs xost over each module and writes a bundle of Xenos containers keyed by the SPIR-V. xost goes
    SPIR-V → Xenos (xost:docs/xsc.md:1-14).
  - `vkCreateShaderModule` hashes the code and finds its container. A module missing from the bundle fails with
    `VK_ERROR_FEATURE_NOT_PRESENT`, as vulkan4vita fails without GXP (vulkan4vita:src/shader.c).
  - vulkan4vita also compiles offline, but takes the native program through its own extension, `VK_VITA_gxp_shader`
    (vulkan4vita:docs/SUPPORT.md:11-16). Here the key is the app's SPIR-V.
- **Python only in that offline step.** xost moving off Python is a later item (TODO.md, "Toolchain").
- **The shader subset is xost's.** Its limits become the layer's, listed in SUPPORT.md:
  - no bitwise operations;
  - data-dependent branches inside loops raise an error;
  - 16 registers a pixel shader;
  - vertex shaders lower less than fragment shaders.

  See xost:docs/xsc.md:124-131, 176-195.
- **Milestones 11 to 18 are the base.** They're done on hardware (docs/HARDWARE.md, milestones 11 to 18).

## 3. How Vulkan maps onto Xenos (proposed)

| Vulkan | On the SDK's GPU layer |
|---|---|
| Instance, physical device, device, queue | One each. The queue is the ring. |
| Memory | One heap of physical memory, write-combined, from `xsdk_gpu_alloc`. Device-local, host-visible and coherent. Mapping returns the pointer. `vkQueueSubmit` drains the CPU's write-combining. A cached type waits until something measures a need. |
| Buffers | Vertex buffers are streams (`xsdk_gpu_set_vertices`, 16 of them). Index buffers go to `xsdk_gpu_draw_indexed`. Uniform buffers become shader constants (below). |
| Uniform buffers | Copied into float constants when the command buffer replays. Loading them from memory with `PM4_LOAD_ALU_CONSTANT` (xenia:src/xenia/gpu/command_processor.cc:1477-1500) would match Vulkan's timing, but it's unproven on the console, and partial constant writes hang the GPU (docs/HARDWARE.md, milestone 11). It needs a probe first. |
| Push constants | Float constants, set at replay. |
| Images | `XsdkTexture`: optimal tiling is Xenos's tiled layout, linear tiling is linear with one level. The formats and mips are milestones 15 and 16's. |
| Image views, samplers | The texture's and the sampler state's halves of a fetch constant (`xsdk_gpu_set_texture`). |
| Descriptor sets | Combined image samplers fill fetch-constant slots 0 up; uniform buffers fill constant blocks. Bindings are numbered in order across the pipeline layout, as vulkan4vita numbers them (vulkan4vita:docs/SUPPORT.md:41-127). |
| Graphics pipelines | Built once at creation, from depth and stencil, blend and raster state blocks (milestones 13 and 14). The vertex input state gives a patched copy of the vertex shader (milestone 17). |
| Render passes, framebuffers | One subpass. A render pass instance draws into eDRAM. Each attachment's store op is a resolve into its image. A clear load op is the resolve's clear; a load op draws the image back in. 1280x720 colour and depth fit eDRAM (SCOPE.md 3.4); bigger targets need predicated tiling (TODO.md). |
| Command buffers | Recorded into the layer's own list, then replayed into the ring at `vkQueueSubmit`, as vulkan4vita replays (vulkan4vita:docs/ROADMAP.md:21-30). Recording straight into the SDK's command buffers (milestone 18) can come later, where a buffer's state is all known when it's recorded. |
| Fences | The SDK's fences. |
| Semaphores, barriers | One queue, so semaphores do nothing. Barriers order copies against draws; textures already drop stale cache lines when bound. |
| Swapchain | `VK_KHR_surface` and `VK_KHR_display`, as vulkan4vita has, over `xsdk_present`. It holds 2 or 3 images at 1280x720, acquired at `xsdk_present_begin` and presented at `xsdk_present_end`. |
| Copies | `vkCmdCopyBufferToImage` and the like run on the CPU at replay, as vulkan4vita settled on (vulkan4vita:docs/ROADMAP.md:585-634). Resolves cover copies out of eDRAM. |

## 4. Milestones (proposed, in order)

1. **A triangle through Vulkan.** Done when a feature-test title draws a triangle on the console with only Vulkan
   calls, presented through the swapchain, and checks its pixels.
   - **Builds:** instance, device and queue; memory and buffers; the shader bundle and its build step; one graphics
     pipeline; a render pass with one colour attachment; a command buffer replayed at submit; a fence.
   - **Not in it:** textures, descriptors, depth.
2. **Textures, descriptors and push constants.** Done when the feature tests sample images through descriptor sets,
   read push constants and uniform buffers, and copy a buffer into an image, each checked by pixel.
   - **Builds:** images, views and samplers; descriptor set layouts, pools and sets; pipeline layouts.
3. **Pipeline state.** Done when the feature tests cover depth and stencil, blending, culling, dynamic viewport and
   scissor, indexed draws and the topologies through Vulkan, as milestones 12 to 14 do through the GPU layer.
4. **The benchmark.** Done when the same scenes run through the raw GPU layer and through Vulkan, with the gap
   measured on the console and the causes named.
5. **Render to texture.** Done when an image rendered in one render pass is sampled in the next, with depth in
   eDRAM, checked by pixel.
6. **A target app.** Done when the app runs on the console through vulkan4xenon. The app is the user's choice
   (section 6).

Anything not in a milestone isn't built until a milestone needs it.

## 5. Left out

- Compute, queries, secondary command buffers, more than one queue, multisampling, multiple subpasses and dynamic
  rendering. vulkan4vita left out the same (vulkan4vita:docs/ROADMAP.md:53, 119-120).
- Runtime SPIR-V compilation (section 2), and Vulkan 1.1 and up.
- Geometry and tessellation shaders.
- The GPU-layer gaps in TODO.md, until a milestone or the target app needs one:
  - two-sided stencil;
  - a constant blend colour;
  - primitive restart;
  - depth resolves;
  - more texture formats;
  - predicated tiling.

## 6. Open decisions

1. **Where it lives.**
   - **Recommendation:** its own repo, `vulkan4xenon`, as vulkan4vita is, linking the SDK's `libxsdk.a`. That keeps
     the SDK's scope as it is.
   - It can join the combined SDK and xost later.
2. **Uniform buffers.** Copy into constants at replay first, and probe `PM4_LOAD_ALU_CONSTANT` later as an
   optimisation? This is the recommendation.
3. **The target app** for milestone 6. vulkan4vita's was Quake II, through vkQuake2 (vulkan4vita:docs/ROADMAP.md:176).
