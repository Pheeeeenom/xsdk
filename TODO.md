# Later

Items left over from SCOPE.md's milestones, or put off by them. None is scoped or started; each needs agreeing before
work begins.

## Runtime and display

- **Symbol names in `std::stacktrace`.** A trace gives addresses only, as a XEX carries no symbols. `elf2xex` would
  write a symbol map beside the `.xex`, and the runtime would name frames from it (SCOPE.md section 2.5).
- **A native TLS model.** `thread_local` uses GCC's emulated TLS over `KeTlsGetValue`. A GCC change that loads
  `tls_ptr` from `0(r13)` first would reach static TLS directly, if emulated TLS proves too slow (SCOPE.md section
  2.5).
- **720p on interlaced outputs (1080i, 480i).** `XSDK_DISPLAY_720P` keeps the kernel's 640x480 there, as scaling on an
  interlaced output isn't tried. Likely pieces: libxenon writes `AVIVO_D1MODE_DATA_FORMAT` 1 for interlaced modes
  (libxenon:drivers/xenos/xenos.c:391), and xenon-emu names a second vertical start phase, 0x65DC,
  `D1SCL_VERT_FILTER_INIT_BOT` (XenosRegisters.h:935).
- **A sharper scaler filter at 1080p.** 1280x720 is scaled with the built-in 2-tap filter, the only setting that came
  out whole. Every variant on the kernel's coefficient table scrambled the image, and 2 taps on the table wasn't tried.
  More taps would mean loading our own coefficients through 0x6578 and 0x657C, in the layout AMD's
  `program_multi_taps_filter` uses (docs/HARDWARE.md, "What AMD's code says about the scaler" and "The built-in 2-tap
  filter scales 1280x720 to 1920x1080 whole").

## Toolchain

- **xost off Python.** Its compiler (`xsc`), assembler (`xasm`) and disassembler (`xdis`) are Python; only the
  combiner generator `xcomb` is C99 (xost:README.md:49-71, docs/xsc.md:1-14). The combined SDK shouldn't need
  Python. Until then, the Vulkan layer compiles shaders offline and looks them up by their SPIR-V, as vulkan4vita
  does.

## GPU features SCOPE.md left out of milestones 11 to 18

- **Multiple render targets.** Unproven in xost.
- **Float render targets and multisampling.**
- **Predicated tiling,** for targets larger than eDRAM.
- **Cube and 3D textures.** The fetch constant's dimension field (xenia:src/xenia/gpu/xenos.h:97-102, 1280) and the
  layout's slices (xenia:src/xenia/gpu/texture_util.cc:209-449).
- **Anisotropic filtering,** put off by milestone 15. The fetch constant's `aniso_filter` (xenos.h:1259); the SDK's
  texture shader already takes it from the fetch constant.
- **Occlusion queries.**
- **Depth resolves, and resolving part of a target.**

## GPU gaps the milestones left

- **Two-sided stencil.** The SDK leaves `backface_enable` clear, so the front face's stencil serves both
  (src/gpu_draw.c). Vulkan sets front and back apart: `backface_enable` and the `_bf` fields of `RB_DEPTHCONTROL`, and
  `RB_STENCILREFMASK_BF` (xenia:src/xenia/gpu/registers.h:799-834).
- **A constant blend colour.** The blend factors stop short of `kConstantColor` to `kOneMinusConstantAlpha`
  (xenos.h:719-722), which read `RB_BLEND_RED` to `RB_BLEND_ALPHA` (xenia:src/xenia/gpu/register_table.inc:506-509).
  Vulkan's blend constants need them.
- **Primitive restart.** `VGT_MULTI_PRIM_IB_RESET_INDX` (registers.h:343-360) and `PA_SU_SC_MODE_CNTL`'s
  `multi_prim_ib_ena` (registers.h:480).
- **Draws of more than 65535 indices.** The draw initiator's count is 16 bits (registers.h:327), so a layer would split
  them.
- **Wireframe and point fill.** `PA_SU_SC_MODE_CNTL`'s `poly_mode` and polygon types (registers.h:463-468).
- **Border and mirror-once addressing.** `ClampMode` 3 to 7, with a black or white border (xenos.h:104-162); Vulkan's
  clamp to border and mirror clamp to edge.
- **Level-of-detail clamps.** The fetch constant's `mip_min_level` and `mip_max_level` (xenos.h:1265-1266), for
  Vulkan's `minLod`, `maxLod` and image views' level ranges. The SDK always samples every level.
- **More texture formats.** Milestone 16 covers 13. A Vulkan layer will also want DXN (BC5), 16-bit fixed point,
  32-bit float, 10-11-11, and the signed and gamma (sRGB) variants: `TextureFormat` (xenos.h:450-532) and
  `TextureSign` (xenos.h:121-129).
- **Render areas smaller than the framebuffer.** vulkan4xenon refuses them with `VK_ERROR_FEATURE_NOT_PRESENT`
  (vulkan4xenon:docs/ROADMAP.md, milestone 4): a render pass may change only its area, but the SDK's resolves copy and
  clear the whole target from its corner. It needs a resolve of a rectangle at an offset, which Xenos takes from the
  resolve's drawn rectangle, widened to 8 pixels (xenia:src/xenia/gpu/draw_util.cc:893-902), or clears by drawing with
  the rest of the target loaded first. encounter's Aurora always renders whole targets. A port that needs them gets
  `vulkan4xenon: not supported: render areas smaller than the framebuffer` in its log, and `vkEndCommandBuffer` fails.
- **Mipped linear textures.** `xsdk_texture_create` returns `STATUS_NOT_IMPLEMENTED` for them. Xenia keeps their rows
  256-byte aligned (texture_util.cc:209-449). Vulkan rarely needs them.

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
- **DXT3 and DXT5 colour blocks whose first endpoint isn't above the second:** four colours, as BC2 and BC3 say, or
  DXT1's three and transparent black. Milestone 16 kept them four-colour.
- **Linear filtering's precision.** Milestone 15 bounds it only by its tolerance of 3 per channel.
- **Which partial constant write hangs the GPU:** the floats, or the lone boolean or loop word. It matters only for
  writing just the changed constants, to save ring space (docs/HARDWARE.md, "Shader constants: partial writes hang the
  GPU, libxenon's whole blocks don't").
