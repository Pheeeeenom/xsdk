# XenosRecomp as a cross-validation oracle

Built 2026-06-11 from the shallow clone at `reference/XenosRecomp`
(hedge-dev/XenosRecomp, Unleashed Recompiled's shader recompiler).
It consumes the *same* 0x102A11xx XDK shader containers as our corpus and
emits readable HLSL — an independent decode of ucode, constant table,
vertex elements and interpolators to diff our disassembler against.

## Build (what actually worked)

Toolchain: VS 18 Community x64 (MSVC 19.50), CMake 4.2, Ninja.
Upstream only tests Clang; this VS install has no clang-cl, so MSVC was used.
One portability shim was needed (MSVC lacks `__builtin_bswap*`) — added to
`XenosRecomp/pch.h`, guarded by `#if defined(_MSC_VER) && !defined(__clang__)`,
mapping to `_byteswap_ushort/_ulong/_uint64`. No recompiler logic touched.

1. Submodules (clone was shallow, no submodules):

   ```
   git -C reference\XenosRecomp submodule update --init --recursive --depth 1
   ```

   All five are required: `fmt`, `xxHash`, `zstd`, `smol-v` (sources),
   `dxc-bin` (prebuilt dxcompiler.dll/dxil.dll the exe links against).

2. Configure + build (helper bats `build_cfg.bat` / `build_run.bat` in the
   repo root wrap these; vcvars64 must be initialized first):

   ```
   call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
   cmake -S reference\XenosRecomp -B reference\XenosRecomp\build -G Ninja ^
         -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
   cmake --build reference\XenosRecomp\build --target XenosRecomp
   ```

Binary: `reference\XenosRecomp\build\XenosRecomp\XenosRecomp.exe`
(dxcompiler.dll + dxil.dll are copied alongside it; keep them together).

## CLI

```
XenosRecomp [input path] [output path] [shader common header file path]
```

* Third arg is mandatory: pass
  `reference\XenosRecomp\XenosRecomp\shader_common.h` (its text is pasted
  verbatim at the top of the output HLSL).
* If `input` is a **file**, it must be a full XDK shader container starting
  at byte 0 (magic `0x102A11xx`) — *not* raw ucode. There is no raw-ucode
  mode; the recompiler reads the constant table, definition table and
  shader data via the container header offsets.
* If `input` is a **directory**, it scans every file for embedded containers
  (validates `(flags & 0xFFFFFF00) == 0x102A1100`, `field1C == 0`,
  `field20 == 0`, `virtualSize + physicalSize` in range) and emits a
  DXIL/SPIR-V shader-cache .cpp instead of HLSL.
* Exit code is 0 even on success/silence; check that the output file exists.

## Corpus results

Both golden blobs translated cleanly when fed as whole containers
(no ucode extraction needed):

```
XenosRecomp.exe corpus\vs0.bin    corpus\vs0.recomp.hlsl    ...\shader_common.h
XenosRecomp.exe corpus\ps_fb0.bin corpus\ps_fb0.recomp.hlsl ...\shader_common.h
```

* `vs0.recomp.hlsl` — vertex shader: inputs POSITION0, TEXCOORD0/1, COLOR0
  (decoded from the vertex-element table); constant table resolved `ortho`
  as `float4 ortho[4]` at c0; body = 4 dot products of `r2.wzxy` against
  `ortho(0..3).wzxy` into oPos, plus mov-via-`max(x,x)` copies of the two
  texcoords and color to interpolators. Matches the expected ortho-transform
  passthrough shader.
* `ps_fb0.recomp.hlsl` — pixel shader: decodes the `use_tex` boolean branch
  (`if (b128 != 0) { tfetch2D(tex0, ...) } else { pass vertex color }`) and
  the `tex0` sampler from the constant table. Note: the emitted HLSL
  references `b128` without declaring it (XDK put the PS bool at register
  b128; XenosRecomp's `g_Booleans` only models 16 bools — a known README
  limitation), so this one is a valid *disassembly oracle* but would need a
  one-line `b128` definition to actually compile with DXC. The `(1 << 16)`
  define it emits for `use_tex` shows its internal bool-bit mapping.

## Format facts confirmed by XenosRecomp's parser (`shader.h`, `main.cpp`)

* Container header (all BE u32): flags/magic, virtualSize, physicalSize,
  fieldC, constantTableOffset (0x10), definitionTableOffset (0x14),
  shaderOffset (0x18), field1C, field20. Total container size =
  virtualSize + physicalSize.
* Validity check used when scanning: top 24 bits of flags == 0x102A11,
  field1C == 0, field20 == 0.
* `Shader` struct at shaderOffset: physicalOffset, size, field8,
  fieldC (svPos register = (fieldC >> 8) & 0xFF), field10,
  interpolatorInfo (count = (interpolatorInfo >> 5) & 0x1F). Ucode lives at
  `physicalOffset` within the *physical* part — consistent with our
  container.md (`offset + sh_off`).
* VS extends this with vertexElementCount + packed
  VertexElement{address:12, usage:4, usageIndex:4}; PS with an outputs mask
  (COLOR0..3 | DEPTH) — useful answers to container.md open questions 1–2.
* Constant table at constantTableOffset is the D3DX-style BE table;
  `constant_table.h` has the exact record layout (name offsets, register
  set/index/count, class/type) — our "metadata region".
