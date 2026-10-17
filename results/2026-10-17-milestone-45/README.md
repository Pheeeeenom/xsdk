# Milestone 45's baseline

What xost's code looks like before milestone 45 changes it (docs/scope/45-xost-code-quality.md, step 1): every shader
of the corpus compiled at xsdk `ee55892`, with the optimiser off and on, counted by `tools/check_optimize.py` as of the
commit that adds this folder. `baseline.json` holds every shader's counts and the totals, which a later run compares
against with `--baseline`; `baseline.txt` is what the run printed.

## How it was made

In WSL Debian, after `make` had built `build/host/libxsc.so`, from `xenos-toolchain/`:

```
python3 tools/check_optimize.py /mnt/z/dusklight-xenon/platforms/xenon/shaders xsc/testdata/*.spv \
    /mnt/z/vulkan4xenon/tests/shaders /mnt/z/vulkan4xenon/src/shaders /mnt/z/vulkan4xenon/bench/shaders \
    --trials 100 --json ../results/2026-10-17-milestone-45/baseline.json \
    > ../results/2026-10-17-milestone-45/baseline.txt
```

It took 67 s. A step's run is the same line with `--baseline ../results/2026-10-17-milestone-45/baseline.json` and
its own `--json`. The corpus, 383 files:
- **Dusklight's GX shaders,** GLSL, at dusklight-xenon `ad096dfd57`: 47 vertex and 103 pixel shaders, the programs its
  pack holds (`platforms/xenon/glsl_pack.c`).
- **xost's testdata,** its 149 SPIR-V files, which xost's tests compile. Its GLSL is OpenGL's, which glslang's `-V`
  won't take, so the tool reads the SPIR-V.
- **vulkan4xenon's shaders** at `6efda94`: `tests/shaders` 72, `src/shaders` 5, and vkbench's `bench/shaders` 7.

The counts are each program as xdis disassembles it. A copy is an ALU instruction whose vector operation is a `max`
of one source with itself, no scalar operation beside it; that takes in `max_sat`, a clamp to 0 to 1, and exports. A
co-issued slot carries a vector and a scalar operation. Fetches are texture and vertex fetches, control flow every
control flow instruction but `cnop`, and registers the shader's count from `SQ_PROGRAM_CNTL`. On two shaders,
Dusklight's `412f75e329bbe3a2.vert` (23 ALU, 15 copies, 2 fetches, 8 control flow instructions) and
`05716a5bd2aae204.frag` (33 ALU, 8 copies, 3 co-issued, 3 fetches, 8 control flow instructions), the counts were
checked against the disassembly line by line.

## The totals

The 367 shaders that compiled, optimiser off then on:

| | Vertex, 121 | | Pixel, 246 | |
|---|---|---|---|---|
| ALU | 7172 | 6106 | 4098 | 2789 |
| Copies | 3055, 42.6% | 2417, 39.6% | 1673, 40.8% | 836, 30.0% |
| Co-issued | 0 | 476, 7.8% | 3, 0.1% | 199, 7.1% |
| Fetches | 337 | 337 | 277 | 277 |
| Control flow | 1695 | 1520 | 1275 | 1053 |
| Registers a shader, most | 7.0, 45 | 7.2, 44 | 5.9, 32 | 5.1, 32 |

With the optimiser on, as titles compile, the corpus has 8895 ALU instructions, 3253 of them copies (36.6%), and 675
co-issued slots (7.6%). The milestone is done when copies are under 5% of the ALU and the ALU is down by at least
25%, to 6671 or fewer.

**Dusklight's and vkbench's shaders,** optimiser on, match the scope's 6641 ALU instructions, 4947 vertex and 1694
pixel, and its 583 co-issued slots. Copies come to 2397, **36.1%** of the ALU, against the scope's 2344: 1725 moves
between registers, 342 `max_sat` clamps and 330 into exports. How the scope counted isn't recorded, so the 53 between
the two can't be traced. Dusklight's vertex shaders take 9.8 registers on average and 15 at most, its pixel shaders
6.2 and 17, as the scope has them.

## What the check left out

**Not simulated, 24:** `xdis/equivalence.py` runs straight-line programs, and these do something it doesn't model.
They are counted, but their exports aren't compared:
- relative constant addressing, the scalar `maxasf` or `maxas` that loads the address register: Dusklight's
  `04a469b1240c5698.vert`, `93d36ce55e3f9bad.vert` and `c0ddfd4547c71e5a.vert`; testdata's `vk_dynamic_rows.vert`,
  `vk_dynamic_matrices.vert`, `vk_frag_coord.frag` and `vk_instance_index.vert`; vulkan4xenon's `band.frag` and
  `matrix_index.vert`;
- hardware loops: testdata's `loop_array.frag`, `loop_break.frag`, `loop_uniform.frag`, `vk_loop_count.frag`,
  `vk_loop_count_if.frag`, `vk_loop_swap.frag` and `vk_vertex_loop_count.vert`; vulkan4xenon's `loop_count.frag`,
  `loop_count.vert` and `loop_count_if.frag`;
- the cube instruction: testdata's `fs_cubemap.frag` and `vk_cube.frag`, vulkan4xenon's `cube.frag`;
- predicates set by `setp_gt`: testdata's `vk_loop_uniform_if.frag`, vulkan4xenon's `loop_uniform_if.frag`.

**Not compiled, 16,** all testdata xost refuses on purpose, each with its message in `baseline.txt`: clip and cull
distances it doesn't lower, a cube's gradients, `gl_FragCoord.zw`, depth written on one path, locals and loops the
unroller doesn't take, a shadow cube, texel offsets, an integer load it can't fold (`vk_vertex_index.vert`), a colour
output past oC3, one stored twice, and `vk_many_fetches.frag`, which needs 383 registers.

**Differ, 42, so the run exits 1:** 30 of Dusklight's vertex shaders and 12 of vulkan4xenon's `aurora_*.vert`. Each
stops at its first difference, the w of an interpolator whose GLSL output is a `vec2` or `vec3`: a component no
GLSL writes, which the unoptimised program happens to export and the optimised one exports differently. The check
counts a component as defined when the first program writes it, so these show as differences; and as it stops at the
first, what comes after it in those 42 shaders isn't compared. The scope's "exports match with the optimiser on and
off" needs the check to compare only the components the GLSL defines.

## On the console

`probe_shader_cost` (`tests/probe_shader_cost.c`) is the baseline's other half, run on the console at this commit.
Built outside `all`, from Dusklight's pack:

```
make XSC_PACK=/mnt/z/dusklight-xenon/platforms/xenon/glsl_pack.c build/probe_shader_cost.xex
```

Its log goes in this folder. Each line's ALU and register counts should equal the `on` counts in `baseline.json`
for the same shader.
