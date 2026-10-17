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

**Differ, none.** The check compares only the components each output's GLSL type has, read from the SPIR-V. Counting
every component the unoptimised program writes, 42 vertex shaders differed, 30 of Dusklight's and 12 of vulkan4xenon's
`aurora_*.vert`, each first at the w of a `vec2` or `vec3` interpolator: a component no GLSL writes and no pixel
shader reads, which the two programs export differently. With an export component of the optimised run moved by 1
on purpose, the check reports a component the output's type has and passes one past it.

## On the console

`probe_shader_cost` (`tests/probe_shader_cost.c`) is the baseline's other half, run on the console at this commit.
Built outside `all`, from Dusklight's pack:

```
make XSC_PACK=/mnt/z/dusklight-xenon/platforms/xenon/glsl_pack.c build/probe_shader_cost.xex
```

Run twice on the console on 2026-10-17 through the console agent: `probe_baseline_1.log` and `probe_baseline_2.log`.
Each run takes about 12 s, 6.7 s of it hashing.

- **Counts:** every line's ALU and register counts equal the `on` counts in `baseline.json` for the same shader.
- **Pixel shaders, 103:** 77,867 and 78,034 µs for a full-screen draw of each. The cheapest take 267 µs, about 7
  pixels a clock; above that the time grows with the ALU: `1510c05add37f2b3.frag`'s 70 take 2,760 µs,
  against 70 × 921,600 / 48 clocks, 2,700 µs, at 48 ALUs a clock.
- **Vertex shaders, 47:** 16,474 and 16,488 µs for a 65,535-vertex draw of each. The cheapest take 132 µs, a vertex a
  clock; `c0ddfd4547c71e5a.vert`'s 245 ALU instructions take 704 µs. Each run's query says nothing reached the pixels.
- **Repeatability:** a shader's time moves 0.46% between the runs on average, 6.2% at most.
- **Pictures:** 100 of the 103 hash the same in both runs. `1510c05add37f2b3.frag`, `29fab4bb2516d607.frag` and
  `2eefbbd89e53921c.frag` hash differently in every run, so a step's run is compared on the other 100. All three
  sample a texture at coordinates offset by another texture's texel times 255 (GX's indirect texturing), coordinates in
  the thousands. Two other shaders that do the same repeat. Why these three don't isn't known: xost's simulator finds
  no component they export undefined, and the probe's texture has one level, which xsdk clamps the fetch to.

The probe's first run drew every pixel shader with all 16 interpolators. Every pixel shader up to 45 ALU instructions
then took about 1,852 µs, 921,600 pixels in as many clocks: interpolating 64 components a pixel held them all to a
pixel a clock. Each pixel shader is now drawn with as many interpolators as it reads, one past its highest input
Location.

## Step 2: values computed into place

Built on 2026-10-17 and checked on the PC; the console runs are to come. The optimiser's last pass in its first round,
`compute_in_place` (`src/xsc_optimize.c`), takes a copy's place for what the copy copies: where a copy moves a computed
component into another temp's or an export's, the instruction that computes it writes there and the copy goes.

- **What moves.** A vector op, or a scalar op into a temp, when nothing between it and the copy reads or writes the
  copy's lanes and it doesn't read them itself. An elementwise op's sources are swizzled to the copy's lanes. A dot
  product's or `max4`'s result is the same in every lane (xenia:src/xenia/gpu/shader_interpreter.cc:436-519, 614-618),
  as a scalar op's is (948-955); any other op keeps its own lanes.
- **Other readers.** A dot product or scalar op often fills two lanes, one copied and one read elsewhere. That reader
  then reads the copy's place, if nothing writes that lane in between. An elementwise result whose other lanes are
  read elsewhere stays.
- **Clamps.** A `max_sat` copy becomes its writer's clamp when nothing else reads the writer's result. Xenia saturates
  an op's result after the op, whatever the op (shader_interpreter.cc:902-909), and a `max` of a value with itself
  gives that value (347-354), so the clamped op and the `max_sat` of the op give the same bits. The lowering already
  clamps adds and multiplies, and fusing gives `mad_sat`; a clamp on another vector op, or on a scalar op (`exp_sat` in
  `05716a5bd2aae204.frag`), is new to the console.
- **Exports.** Only a vector op exports, as the lowering's do (`xsdk_xsc_emit_construct`'s masked exports,
  `mat_vec_ops`' `dp4 oPos.x`), and only from the stage after the same alloc. It moves down to where the copy was, so
  each export stays where xost put it before, next to its register's other writes; nothing between may write what it
  reads. Xenia takes a scalar result to an export too (xenia:src/xenia/gpu/ucode.h:2041-2060), but xost has never
  exported one, so the step doesn't. A lane of the copy that nothing writes, such as a `vec3` interpolator's w, takes a
  computed lane's value, so the export is still written whole.

It ran as the baseline's line with `--json ../results/2026-10-17-milestone-45/step2.json`, in 86 s; `step2.txt` is
what it printed. The optimiser on, the baseline then step 2:

| | Vertex, 121 | | Pixel, 246 | |
|---|---|---|---|---|
| ALU | 6106 | 4561 | 2789 | 2326 |
| Copies | 2417, 39.6% | 872, 19.1% | 836, 30.0% | 399, 17.2% |
| Co-issued | 476, 7.8% | 465, 10.2% | 199, 7.1% | 195, 8.4% |
| Fetches | 337 | 337 | 277 | 277 |
| Control flow | 1520 | 1240 | 1053 | 988 |
| Registers a shader, most | 7.2, 44 | 6.1, 44 | 5.1, 32 | 5.0, 32 |

The corpus has 6887 ALU instructions, 22.6% fewer than 8895, and 1271 copies, 18.5% of them, against the milestone's
6671 and 5%.

**Dusklight's and vkbench's shaders:** ALU 6641 to 4956, 25.4% fewer; copies 2397 to 733, 14.8%: moves between
registers 1725 to 480, `max_sat` clamps 342 to 24, and copies into exports 330 to 220, with 9 clamped ones from
`max_sat` copies of constants and inputs now exported. Co-issued slots stay 583. What's left, counted at the
optimiser's end:
- 185 are in the three vertex shaders it leaves alone for their relative constant addressing (`can_optimize`):
  `04a469b1240c5698.vert`, `93d36ce55e3f9bad.vert` and `c0ddfd4547c71e5a.vert`.
- 282 copy a constant, 4 of them exports' constant-0 fills, and 5 a pixel shader's input: nothing computes them.
- 170 copy a fetched value. A fetch can't write an export (78 of them); into a temp it would take the fetch's
  destination swizzle, step 5's work.
- 91 copy a computed value that can't move: 25 an elementwise result whose other lane another instruction reads, 19 a
  result another instruction reads in one operand with another writer's lanes, 25 a vertex shader's clip-space zw
  exported as an interpolator, computed before the interpolators' alloc and read by `oPos` too, 20 other exports whose
  writer's result is read elsewhere, and 2 clamps whose writer's result is read unclamped.

Dusklight's vertex shaders take 7.5 registers on average (9.8 before), its pixel shaders 6.2 (6.2). An export's writer
moved down keeps its sources live longer: 19 pixel shaders take a register more, `85c8b022385334bf.frag` two, while the
corpus's total falls from 2115 to 1973. Exporting from where the writer was would cost 4 shaders a register instead,
but would split a register's exports with other instructions, which xost's optimised code never does.

**Checks:**
- `check_optimize.py`: 0 differ, with the baseline's 24 not simulated and 16 not compiled.
- xost's tests, a file at a time in WSL, all pass but `test_xcomb_parity.py`, which runs on Windows.
  `test_optimize.py` has 8 cases for the step. Broken on purpose, each way fails both them and `check_optimize.py`
  on Dusklight's shaders: sources left unswizzled (3 differ), the clamp dropped (81), other readers left as they were
  (3).
- Containers: 125 of the 150 programs in Dusklight's pack change, 43 vertex and 82 pixel, the ones the step moves a
  copy in; the other 25 come out byte for byte as before.
- xsdk_test's compiler check: `tests/xsc_shaders.h`, regenerated with `tools/gen_xsc_shaders.py`, has 4 of its 8
  containers changed: `vk_block_literals.vert`, `vk_vertex_if.vert`, `vk_vertex_uniform_if.vert` and
  `vk_clip_distance.vert`. Their SPIR-V, literals and resources are the same.
- `probe_shader_cost` builds from the pack unchanged.

### Step 2 on the console

Run on 2026-10-17 through the console agent, everything cleaned and built in full:
- **xsdk_test:** 380 passed, among them the compiler check's 8 containers compiled on the console. The two controller
  checks failed with 0x48F, no controller connected, as at step 1.
- **`probe_shader_cost`** (`probe_step2.log`): pixel shaders 77,668 → 66,036 µs for a draw of each (−15.0%), vertex
  shaders 16,472 → 14,054 µs (−14.7%), against the better of the two baseline runs; the same 100 pictures as the
  baseline, and no vertex draw reaching the pixels. The run took 13.8 s against 11.8 s, the hashing the same: compiling
  the 150 programs on the console takes about 2 s more, as the step's pass starts its scan over after each copy it
  takes out.
- **vulkan4xenon_test:** its committed shaders are compiled without the optimiser, so the step changes none of them.
  Built with its 84 shaders (tests, the layer's and vkbench's) compiled with `--optimize`, by main's xost and by the
  step's, which differ in 681 lines of `tests/shaders.c`: 426 passed with main's, and 426 in each of two runs with the
  step's.
- **Dusklight,** its pack built with the step's xost: the title scene's 600th frame matches the baseline's, 0 of
  921,600 pixels different.
- **Broken on purpose,** the copy's clamp left off the instruction that takes its place: 12 of the probe's 100
  pictures differ from the baseline. Dusklight's frame doesn't change, so the probe's pictures are the check that
  catches such a fault.
