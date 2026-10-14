# Xenos shader container format

The on-disk/in-memory format consumed by libxenon's `Xe_LoadShaderFromMemory`
(`reference/libxenon/.../xenos/xe.c`) and historically produced by the XDK
shader compiler. All fields **big-endian u32** unless noted.

Evidence: three independent sources, all converging —
1. **xe.c** (libxenon loader) — fields actually read on console.
2. **XenosRecomp's parser** (`shader.h`, validated by translating our corpus)
   — field names/semantics for most of what xe.h called `unk`.
3. **Byte-level analysis** of two containers from mupen64-360's
   `shaders.S` (392 and 304 B), kept as test vectors in xost's archived repo (github.com/xenosworks/xenos-toolchain, last commit 4a785c6).

## Header (offset 0, 0x24 bytes)

| off | name (merged) | VS | PS | semantics |
|---|---|---|---|---|
| 0x00 | magic/flags | `0x102A1101` | `0x102A1100` | valid iff `(v & 0xFFFFFF00)==0x102A1100`; low byte 1 = vertex, 0 = pixel |
| 0x04 | virtualSize | 0xD4 | 0xDC | size of metadata portion; also the ucode base (`xe.c: hdr->offset`). `virtualSize + physicalSize = file size` (392/304 ✓) |
| 0x08 | physicalSize | 0xB4 | 0x54 | ucode byte length (mirrors XenosShaderData.sh_size) |
| 0x0C | ? | 0 | 0 | |
| 0x10 | constantTableOffset | 0x24 | 0x24 | constant table immediately follows header |
| 0x14 | definitionTableOffset | 0 | 0 | (xe.h misnames this `off_constants`) |
| 0x18 | shaderOffset | 0x84 | 0xB0 | offset of the Shader record (`xe.c: off_shader`) |
| 0x1C | must be 0 | 0 | 0 | XenosRecomp validity check |
| 0x20 | must be 0 | 0 | 0 | XenosRecomp validity check |

## Shader record (at `shaderOffset`)

| field | VS | PS | semantics |
|---|---|---|---|
| sh_off | 0 | 0 | ucode at `virtualSize + sh_off` **[xe.c]** |
| sh_size | 0xB4 | 0x54 | ucode length, multiple of 12 (96-bit instr slots) **[xe.c]** |
| program_control | `0x00210002` | `0x10000200` | → GPU reg `SQ_PROGRAM_CNTL` **[xe.c]** |
| context_misc | `0x00000008` (PS) | | → `SQ_CONTEXT_MISC` **[xe.c]**; carries e.g. param_gen/pixel-pos config |
| +0x0C-ish | svPos reg = `(fieldC>>8)&0xFF`; interpolator count = `(interpolatorInfo>>5)&0x1F` | | per XenosRecomp |
| trailing | VS: packed `VertexElement{address:12, usage:4, usageIndex:4}` records (the vertex declaration → which vfetch templates exist); PS: an output mask | | per XenosRecomp |

## Constant table (at 0x24)

D3DX-style, big-endian: name strings (`ortho`; `tex0`, `use_tex`), type/class,
register set + index, register count. XenosRecomp resolves names from it (our
corpus HLSL shows `ortho(0..3)` at c0–c3, `tex0` sampler, bool at b128 —
note: D3D9-360 places PS booleans at 128+ in the unified bool space).
xe.c ignores this table entirely (homebrew sets constants by index), so our
emitter needs only a minimal valid table.

## Vertex shaders: vfetch patch mechanism

The VS ucode contains *template* `vfetch_full` instructions (corpus: four
slots all nominally `vf0`); at instantiation `Xe_ShaderApplyVFetchPatches`
rewrites them to the live vertex-buffer format, guided by the VertexElement
records above and `XenosShaderVertex{cnt0, cnt_vfetch, cnt2}` counts. Our
Phase 1 emitter must produce: template vfetches + correct VertexElement
records, in the form the patcher expects.

## Remaining unknowns (non-blocking)
- Exact layout/order of the Shader-record tail fields (svPos/interpolator
  words vs the `0x2063` word seen in both corpus blobs) — pin down by diffing
  against XenosRecomp `shader.h` field-by-field when building the emitter.
- Constant-table record binary layout (have semantics; need exact struct) —
  same source.

See also: `docs/xenosrecomp-oracle.md` (build + usage of the oracle tool).
