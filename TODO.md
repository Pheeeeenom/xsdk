# Later

Items left over from SCOPE.md's milestones, or put off by them. None is scoped or started; each needs agreeing before
work begins.

## Runtime and display

- **A stack overflow hangs the console,** with a fault handler or without, until it's restarted (docs/HARDWARE.md, "How
  the kernel hands a title its faults, milestone 40's probe"). The kernel puts the exception record and context on the
  faulting thread's own stack, so an overflowed one leaves it nowhere to. What could stop it, such as room kept at
  each stack's end, isn't known.
- **Traps never reach the fault handler on the user's console,** which runs a debug monitor; the title carries on past
  them (the same section). What a console without one does, and so whether `-fsanitize-trap` could report, isn't
  known.
- **Symbol names in `std::stacktrace`.** A trace gives addresses only, as a XEX carries no symbols. `elf2xex` would
  write a symbol map beside the `.xex`, and the runtime would name frames from it (SCOPE.md section 2.5).
- **A native TLS model.** `thread_local` uses GCC's emulated TLS over `KeTlsGetValue`. A GCC change that loads
  `tls_ptr` from `0(r13)` first would reach static TLS directly, if emulated TLS proves too slow (SCOPE.md section
  2.5).
- **720p on interlaced outputs (1080i, 480i).** `XSDK_DISPLAY_720P` keeps the kernel's 640x480 there, as scaling on an
  interlaced output isn't tried. Likely pieces: libxenon writes `AVIVO_D1MODE_DATA_FORMAT` 1 for interlaced modes
  (libxenon:drivers/xenos/xenos.c:391), and xenon-emu names a second vertical start phase, 0x65DC,
  `D1SCL_VERT_FILTER_INIT_BOT` (XenosRegisters.h:935).
- **Mutexes holding their critical section.** A `pthread_mutex_t` is newlib's 32-bit word, which xsdk makes a pointer
  to a critical section `malloc`ed on first lock (src/pthread.c, `mutex_section`). Keeping the critical section in the
  mutex, with a newlib patch making `pthread_mutex_t` that big and every C++ library rebuilt again, would end two costs:
  - a mutex locked after its memory is freed takes newlib's free-list link there for its critical section and corrupts
    the heap, where a desktop's mutex would only touch dead memory (Dusklight's, in docs/HARDWARE.md, "Dusklight runs
    the title scene");
  - `std::mutex` has no destructor when its type has a static initializer (libstdc++ bits/std_mutex.h), so each one
    ever locked leaks its 28-byte critical section when it goes.
- **A folder removed while it's open.** xsdk opens every file and folder sharing nothing (src/file.c, src/path.c), so
  the kernel refuses to delete one another handle holds open, 0xC0000043. libstdc++'s `remove_all` deletes an emptied
  folder within another while its own handle on it is still open (gcc:libstdc++-v3/src/c++17/fs_dir.cc:492-506), so it
  fails on any folder within a folder; a flat one goes. POSIX allows both. The likely fix is opening with
  `FILE_SHARE_DELETE` and the other shares, as a POSIX title expects, with `unlinkat` sharing all three.
- **Small GPU allocations other than shaders.** `xsdk_gpu_alloc` rounds each one up to a 64 KiB large page (src/gpu.c,
  `allocate`). Shaders' microcode shares pages since milestone 25; the rest, three small ones in Dusklight's title
  scene (results/2026-10-12-gpu-allocations/), still take a page each.
- **The Guide, beyond milestone 37** (docs/scope/37-guide.md):
  - **Pausing while it's open.** `XN_SYS_UI` says when the Guide opens and closes (tests/guide_check.c reads it); a title
    that should pause, as consoles' games do, would want it from xsdk.
  - **A title ending by itself with the Guide open hangs** as it ends: XAM's Guide thread waits for a fence only the
    title's next swap would write (docs/HARDWARE.md, "The Guide, milestone 37"). Leaving through Xbox Home is fine. A
    fix would have `xsdk_present_close` keep swapping its last frame until XAM's system command buffer is empty and
    done, which XAM may not allow before the Guide closes; unexplored.
  - **On the kernel's display mode and interlaced outputs,** where xsdk doesn't scale the display, XAM's surface isn't
    shown, so the Guide still isn't; the log says so once.
- **A sharper scaler filter at 1080p.** 1280x720 is scaled with the built-in 2-tap filter, the only setting that came
  out whole. Every variant on the kernel's coefficient table scrambled the image, and 2 taps on the table wasn't tried.
  More taps would mean loading our own coefficients through 0x6578 and 0x657C, in the layout AMD's
  `program_multi_taps_filter` uses (docs/HARDWARE.md, "What AMD's code says about the scaler" and "The built-in 2-tap
  filter scales 1280x720 to 1920x1080 whole").

## Toolchain

- **xost's hardware loops, past vulkan4xenon milestone 21** (xenos-toolchain/docs/xsc.md, "Loops a block counts"). A
  break on data needs each vertex or pixel that broke to stop changing what the loop carries, as LOOP_END breaks only
  when all 64 agree (xenia:src/xenia/gpu/ucode.h:372-373): a flag each pass's values are selected by would do. Also
  unbuilt: `continue`, a loop a block counts inside another loop or an if/else on data, and a sample at coordinates
  a pass works out.
- **Vulkan command recording 10-12% slower since the calling-convention switch** (milestone 22, docs/HARDWARE.md,
  "The switch to the console's calling convention"). vkbench on the console (vulkan4xenon
  results/2026-10-12-mcall-xsdk/, against 2026-10-11-milestone-8-reverted, scaled for the clock fix): the layer's
  draw and state scenes record 10.5-12% slower, record plus submit 0.5-1.7% slower; the 8000-object simulation runs
  14% faster and xsdk's own recording 2-3% faster. The recording functions are the same instructions under both
  compilers; every non-leaf frame now reserves the 64-byte parameter area (vkCmdDraw's grew from 32 to 96 bytes), a
  candidate cause not yet checked. The user put it off until the Dusklight session has it rendering.
- **C code can't name `char16_t`.** newlib has no `<uchar.h>`, so C code sharing UTF-16 text with MSVC code spells it
  `__CHAR16_TYPE__` (local/abi's probe does); C++ has the keyword.
- **xost off Python.** Its compiler (`xsc`), assembler (`xasm`) and disassembler (`xdis`) are Python; only the
  combiner generator `xcomb` is C99 (xost:README.md:49-71, docs/xsc.md:1-14). The combined SDK shouldn't need
  Python. Until then, the Vulkan layer compiles shaders offline and looks them up by their SPIR-V.

## GPU features SCOPE.md left out of milestones 11 to 18

- **Multiple render targets.** Unproven in xost.
- **Float render targets and multisampling.**
- **Predicated tiling,** for targets larger than eDRAM.
- **Cube and 3D textures, the rest.** vulkan4xenon milestone 23 brought tiled cubes with their mips and tiled 3D
  textures of one level (include/xsdk/texture.h), unproven on the console till its checks run. Left: linear cubes and
  3D textures; a 3D texture's mips, which Xenia sizes by the base's depth at every level
  (xenia:src/xenia/gpu/texture_util.cc:347-350) and packs along z once they're small (texture_util.cc:190-198); and
  resolving into a face or slice (vulkan4xenon milestone 25).
- **Occlusion queries.** vulkan4xenon milestone 19's step 6 put them off: every `ZPASS_DONE` the probe sent, as D3D
  sends it and as Adreno 3xx's driver does, left the sample counts unwritten and hung the next draw, the render backend
  and memory hub busy (docs/HARDWARE.md, "Queries and events"). Left to try: `RB_SAMPLE_COUNT_CTL`'s RESET bit, a
  resolve between the event and the next draw, and the state the console leaves before xsdk's set-up, a register group
  at a time. vulkan4xenon refuses occlusion query pools till then.
- **Filtering 10-10-10-2 and 16-bit float textures.** The console filtered them wrong halfway between two texels,
  nearest sampling right (docs/HARDWARE.md, "Texture and vertex formats, vulkan4xenon milestone 20"); vulkan4xenon
  samples them nearest. To try: `k_2_10_10_10_AS_16_16_16_16` and the `_EXPAND` formats
  (xenia:src/xenia/gpu/xenos.h:503-505, 530).
- **Resolving part of a target.** Depth resolves came with vulkan4xenon's milestone 14 (docs/HARDWARE.md, "Depth
  resolves").

## GPU gaps the milestones left

- **Two-sided stencil.** The SDK leaves `backface_enable` clear, so the front face's stencil serves both
  (src/gpu_draw.c). Vulkan sets front and back apart: `backface_enable` and the `_bf` fields of `RB_DEPTHCONTROL`, and
  `RB_STENCILREFMASK_BF` (xenia:src/xenia/gpu/registers.h:799-834).
- **Destination alpha,** vulkan4xenon milestone 18's step 4, put off (vulkan4xenon:docs/ROADMAP.md). Aurora's backend
  refuses a pipeline with `GXSetDstAlpha` on and skips its draws, logging `vulkan4xenon can't draw destination alpha`
  (aurora-melee:lib/vulkan4xenon/pipeline.cpp, `unsupported`). Dawn's backend writes the constant as alpha through a
  blend constant; where the colour blend reads source alpha, it needs dual-source blending, which Xenos lacks, or an
  alpha-only pass before the colour (aurora-melee:lib/gx/pipeline.cpp, `create_pipeline`). The constant blend colour
  (`xsdk_gpu_set_blend_constants`) and Vulkan's blend constants in the layer came with vulkan4xenon milestone 19's
  step 3, so it needs that pass in the backend. Melee sets it through its
  HSD state (melee-pc:src/sysdolphin/baselib/state.c:376); no Melee session or Dusklight title scene has logged the
  refusal yet.
- **Primitive restart.** `VGT_MULTI_PRIM_IB_RESET_INDX` (registers.h:343-360) and `PA_SU_SC_MODE_CNTL`'s
  `multi_prim_ib_ena` (registers.h:480).
- **Draws of more than 65535 indices.** The draw initiator's count is 16 bits (registers.h:327), so a layer would split
  them.
- **Wireframe and point fill.** `PA_SU_SC_MODE_CNTL`'s `poly_mode` and polygon types (registers.h:463-468).
- **Border and mirror-once addressing.** `ClampMode` 3 to 7, with a black or white border (xenos.h:104-162); Vulkan's
  clamp to border and mirror clamp to edge.
- **Image views of some of a texture's levels.** vulkan4xenon refuses a view that doesn't cover every level
  (vulkan4xenon:src/image.c). The sampler's lowest level and count of levels (include/xsdk/texture.h), which took
  Vulkan's `minLod` and `maxLod` in vulkan4xenon milestone 18's step 2, could take a view's range too.
- **More texture formats.** Milestone 16 covers 13, milestone 28 R32F, and vulkan4xenon's milestone 20 DXN, DXT5A,
  16-bit fixed point and 10-11-11, and signed and gamma reads (`xsdk_texture_signs`). Left: 32-bit floats in two and
  four channels (`k_32_32_FLOAT`, `k_32_32_32_32_FLOAT`); DXN and DXT5A read signed, which Xenia doesn't read
  (vulkan/vulkan_texture_cache.cc:333-337, 384-388), and so the layer's BC4 and BC5 `SNORM`; and the formats no
  Vulkan format or port has asked for, such as CTX1 and DXT3A (`TextureFormat`, xenos.h:450-532).
- **Render areas smaller than the framebuffer.** vulkan4xenon refuses them with `VK_ERROR_FEATURE_NOT_PRESENT`
  (vulkan4xenon:docs/ROADMAP.md, milestone 4): a render pass may change only its area, but the SDK's resolves copy and
  clear the whole target from its corner. It needs a resolve of a rectangle at an offset, which Xenos takes from the
  resolve's drawn rectangle, widened to 8 pixels (xenia:src/xenia/gpu/draw_util.cc:893-902), or clears by drawing with
  the rest of the target loaded first. encounter's Aurora always renders whole targets. A port that needs them gets
  `vulkan4xenon: not supported: render areas smaller than the framebuffer` in its log, and `vkEndCommandBuffer` fails.
- **Mipped linear textures.** `xsdk_texture_create` returns `STATUS_NOT_IMPLEMENTED` for them. Xenia keeps their rows
  256-byte aligned (texture_util.cc:209-449). Vulkan rarely needs them.

## Costs measured but not chased

- **A pushed draw's extra 0.47 us at submit (vulkan4xenon).** Through the layer a pushed cube and a uniform one write
  the same 17.3 dwords and 18 kicks a frame, yet the pushed one's submit costs 0.97 us and the uniform one's 0.50
  (vulkan4xenon:docs/ROADMAP.md, milestone 8). That's 0.47 ms a frame for every 1,000 pushed draws, 2.8% of a 60 fps
  frame. It isn't the command stream: milestone 8 cut those dwords by three quarters and the submit didn't move.
  - **Where it goes, measured on 2026-10-15** (a vkbench build timing each part of the replay with the time base,
    after xsdk's milestone 39, local only): writing the push blocks into constants through xsdk, 347 clocks a cube
    for the vertex shader's 4 registers and 461 for the pixel shader's 5, as its tint sits 64 bytes in. Reading the
    push array just after the copy into it costs nothing measurable.
  - **Tried and not taken:** writing only the registers each stage's pushes since the last draw reached, from the
    recording. Finding them cost about 110 clocks a stage and reading the recording about 115 more, more than the
    register group saved: `draws_1000` drew and submitted in 905 us, from 852. vulkan4xenon stays as it was.
  - **Left because** encounter's Aurora, which Dusklight draws through, passes per-draw data in dynamic uniform buffers
    and pushes 16 bytes a draw (vulkan4xenon:docs/ROADMAP.md, section 5), so its push blocks have no registers to
    skip; what's left is xsdk's constant setter itself, about 340 clocks a group of 4.
- **Constants written between draws hold the GPU.** After milestone 39 a draw costs the GPU 0.118 us alone, its two
  constant groups 0.112 alone, and the two together 0.285: each constant write waits for the draw before it to finish
  (docs/HARDWARE.md, "The GPU's cost of a draw, milestone 39"). With vkbench's cubes, which cover the screen between
  them, each draw waits out the last one's pixels, and the cube scenes' time to their fence didn't move.
  - **What might let the next draw's constants in while one is drawing,** untried: alternating constant ranges
    between draws through `SQ_VS_CONST` and `SQ_PS_CONST`'s bases, or constants loaded from memory in some other
    pattern (loaded as they are now, they cost 0.07 us more).
  - **`xsdk_gpu_set_vertices`' fetch constant flush,** kept untried where the texture's went.
  - **When to take it up:** next for the GPU, if the user agrees; the probe (`tests/probe_gpu_draw_cost.c`) takes
    the variants.

## Tried and reverted

Changes that worked but weren't faster on the console. Each stays in history, to bring back if a run shows the cost
it removes.

- **A ring the GPU loads push constants from** (vulkan4xenon `2e94c05` and xost `af5bb8b`, reverted by vulkan4xenon
  `0579b93` and xost `77c7605`). Each draw copied its push blocks into 4 MB of write-combined memory and had the GPU
  load them with `xsdk_gpu_load_*_constants`, as uniform blocks are; xost put push blocks on whole groups of 4 for it.
  - **What it saved:** a pushed cube went from 65.3 dwords to 17.3, and a frame from 64 kicks to 18.
  - **What it didn't:** `draws_1000` took 1,217 us against 1,215, its submit 974 us both times, and `states_1000` 2,021
    against 1,975; the revert ran milestone 7's figures again to within 5 us (vulkan4xenon:docs/ROADMAP.md,
    milestone 8).
  - **When to bring it back:** if push-heavy frames fill the command stream, which `xsdk_gpu_stats` shows as waits, or
    the GPU becomes the bottleneck reading constants. At 65 dwords a cube a frame of about 16,000 pushed draws laps
    the 4 MB stream; at 17, about 60,000.

## Questions the console hasn't answered

- **The depth-bias slope.** `XsdkRaster` takes it, but milestone 14 tested only the constant term.
- **Point mip filtering exactly halfway between two levels:** the nearer level, or the one below. Milestone 15
  avoided it.
- **Linear filtering's precision.** Milestone 15 bounds it only by its tolerance of 3 per channel.
- **Execs on the predicate in vertex shaders.** An if/else on a push constant, its arms in a `setp`'s `(p0)` and
  `(!p0)` execs, hung the GPU in a vertex shader and drew right in a pixel shader (docs/HARDWARE.md, "Branches,
  vulkan4xenon milestone 21"), so xost keeps a vertex shader's as selects. Why is unknown: a probe of a vertex
  shader's lone predicated exec, then of the PredicateClean bit on the `setp`'s exec, would narrow it.
- **An if/else on data in execs on the predicate.** xost makes one selects, both arms running, as an exec on the
  predicate runs for a whole group of 64 vertices or pixels when any of them needs it (xenia:src/xenia/gpu/ucode.h:114-124);
  only an if/else every vertex or pixel agrees on goes in such execs (xenos-toolchain/docs/xsc.md, "Control flow").
  Where they agree over whole groups, as a mesh's parts often do, the execs would skip the arm not taken; whether that
  pays against an exec's own cost is for the console to measure.
- **Which partial constant write hangs the GPU:** the floats, or the lone boolean or loop word. It matters only for
  writing just the changed constants, to save ring space (docs/HARDWARE.md, "Shader constants: partial writes hang the
  GPU, libxenon's whole blocks don't").
- **Texture fetches in vertex shaders** (vulkan4xenon milestone 22's step 3): whether an exec's vertex cache bits
  should mark them, as xost gives them none, and whether a fetch reading what another fetch wrote in its exec needs
  its serialize bit, which xost now sets (xenos-toolchain/docs/xsc.md, "Vertex textures").
