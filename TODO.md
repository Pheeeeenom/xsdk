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
- **Mipped linear textures.** `xsdk_texture_create` returns `STATUS_NOT_IMPLEMENTED` for them. Xenia keeps their rows
  256-byte aligned (texture_util.cc:209-449). Vulkan rarely needs them.

## Questions the console hasn't answered

- **The depth-bias slope.** `XsdkRaster` takes it, but milestone 14 tested only the constant term.
- **Point mip filtering exactly halfway between two levels:** the nearer level, or the one below. Milestone 15
  avoided it.
- **Linear filtering's precision.** Milestone 15 bounds it only by its tolerance of 3 per channel.
- **Which partial constant write hangs the GPU:** the floats, or the lone boolean or loop word. It matters only for
  writing just the changed constants, to save ring space (docs/HARDWARE.md, "Shader constants: partial writes hang the
  GPU, libxenon's whole blocks don't").
