# xasm.textasm — the text assembler

Assembles the toolchain's own disassembly format (Xenia ucode-disasm style)
back into microcode, byte-identically, plus directives for authoring
complete shader containers by hand.

```
python -m xasm.textasm in.xasm -o out.bin --type {vertex,pixel} [--container]
```

Library: `assemble_text(text, shader_type) -> ucode bytes`,
`assemble_file(...)` (container when directives present / `--container`).

## Round-trip contract

`disassemble(pretty) -> assemble` reproduces the original ucode bytes
exactly, **up to trailing all-zero pad slots and the hidden fields below**:
xsc fills some of those its own way, a one-source op's unused source register
among them, so such a shader comes back disassembling to the same text, not
the same bytes. (Pretty text cannot express
unreferenced slots; the `--exact` disassembly mode carries those via
`;; pad` and is covered by `reconstruct_from_exact`). Verified against every
shader xsc lowers from `xsc/testdata` in the forms the grammar takes, the
9-mode combiner bank, and 200 fuzzed conventional programs
(`tests/test_textasm.py`). Not in the grammar yet, and refused with an
error: a slot holding only a scalar op, constant 0 and 1 in an export's
mask, relative and `_abs` operands, and a few rarer forms the test lists.

## Grammar (what the disassembler emits)

- `/* N.j */` / `/* N */` position comments — ignored; layout is recomputed.
- CF lines: `exec` / `exece` / `cexec b<N>, …` / `alloc position|
  interpolators|colors` / `cjmp [!]b<N>|[!]p0, L<k>` / `call` / `ret` /
  `loop` / `endloop` / `cnop` / `vfetch_done`. Instructions after an
  exec-type line belong to its block until the next CF line.
- `label L<k>` — jump/call/loop targets; CF entry indices are recomputed
  during layout, two-pass.
- `serialize` on its own line — applies to the next instruction
  (sequence bit 1).
- ALU: `op[_sat] dest[.mask], src[, src[, src]]` with `(p0)`/`(!p0)`
  predication prefixes, `-` negation, `rN`/`cN` banks, printed 4-component
  swizzles, export dests `oPos oPts oC<n> oDepth o<n> eA eM<n>`.
- `+ scalarop …` continuation — co-issued scalar half (incl. the
  mulsc-family const+temp operand scattering).
- `vfetch_full/_mini`, `tfetch1D/2D/3D/Cube` with the attribute suffixes the
  disassembler prints; unprinted attributes get the suppression defaults.

## Hidden-field conventions (not representable in pretty text)

These match both XDK corpus vintages and our hardware-verified shaders:

| field | convention |
|---|---|
| exec `is_predicate_clean` | 1 |
| exec `vc_hi` | vertex shaders: `(1 << n_fetches_in_block) - 1` (vs0: 4→0xF, cube_vs: 3→0x7); pixel: 0 |
| exec `is_yield`, `vc_lo`, addressing | 0 |
| alloc `size`/`is_unserialized` | 0 / false |
| ALU scalar half without `+` line | kRetainPrev idiom (corpus mov encoding) |
| scalar 2-comp source swizzle | printed chars at components 3,0; unprinted 1-2 replicate the right-hand char (cube_ps: `.ww` → wwww) |
| tfetch `arbitrary_filter` | 0 (never printed; deprecated in hardware) |
| tfetch `aniso_filter` default | 7 = kUseFetchConst (NB: enum value 6 is undefined) |

## Directives (source mode)

```
.pixel | .vertex
.interpolator <idx> <texcoord|color> [usage_index]
.element <position|color|texcoord> [usage_index]   (VS, in fetch order)
.sampler <name> <fetch_slot>
.constant <name> c<N> [count]
.matrix <name> c<N>
```

With directives present, output is a complete container (header, D3DX-style
constant table, metadata) via the same machinery as the Program builder.
Example: see `tests/test_textasm.py` source-mode smoke (hand-written
passthrough PS → container → XenosRecomp-accepted).
