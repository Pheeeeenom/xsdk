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

Built on 2026-10-17 and checked on the PC, then on the console. The optimiser's last pass in its first round,
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
  takes out. `probe_xsc`, built from the pack as the step's xost makes it, times the same compiles: 3.39 s in all
  against 1.32 s before the milestone, 22.6 ms a program against 8.8, the slowest 228 ms against 76; all 150 come out
  as the PC makes them.
- **vulkan4xenon_test:** its committed shaders are compiled without the optimiser, so the step changes none of them.
  Built with its 84 shaders (tests, the layer's and vkbench's) compiled with `--optimize`, by main's xost and by the
  step's, which differ in 681 lines of `tests/shaders.c`: 426 passed with main's, and 426 in each of two runs with the
  step's.
- **Dusklight,** its pack built with the step's xost, runs the title scene as before. Its frames aren't a check yet:
  the runs first recorded here as matching the baseline fetched a capture an earlier run had left, as Dusklight hadn't
  reached the title scene's 600th frame before the run ended. Given the time, two runs of one build capture the same
  frame but for the sky's top 40 rows, which move with the clock (25,545 pixels); a build whose shaders cost less
  reaches its 600th frame at another point of the ride, so the step's frame and the baseline's differ in almost every
  pixel.
- **Broken on purpose,** the copy's clamp left off the instruction that takes its place: 12 of the probe's 100
  pictures differ from the baseline, so the probe's pictures catch such a fault.

## Step 3: registers packed

Built on 2026-10-17 and checked on the PC, then on the console. In a program the optimiser rewrote, the
allocator (`pack_registers`, `src/xsc_program.c`) gives each temp's value the components it holds in a register, not a
register of its own: values whose components don't overlap in time share one register's lanes.

- **Liveness.** Each component of a temp lives from its first write to its last read or write, by the optimiser's own
  model of what an op reads and writes (`xsdk_xsc_read_keys`, `xsdk_xsc_write_keys`); what a run of fetches reads stays
  till the run ends, as before. A component a temp never writes but something reads is undefined, as step 2 took it,
  and takes no lane.
- **Placement.** Values go, in the order they start, into the lowest register their components fit, in their own lanes
  if those are free and in others if not. Choosing the fullest register that fits instead gave every shader the same
  count.
- **Lanes moved.** A scalar op's result fills any lane it writes (xenia:src/xenia/gpu/shader_interpreter.cc:948-955), a
  dot product's or `max4`'s is the same in every lane (436-519, 614-618), and an elementwise op's lane takes its
  operands' same lane, so its operands' swizzles move with it (`xsdk_xsc_move_lanes`, `src/xsc_optimize.c`). Every
  reader's swizzle follows the value to its lanes. What `cube` and `dst` write keeps its lanes, which mean something,
  and so does a `mad` or `cnd` beside a scalar op, which reads its third operand in its own lanes.
- **Registers the hardware fills.** A pixel shader's interpolators arrive in r0 up and its pixel parameters where
  `SQ_CONTEXT_MISC` says (xenia:src/xenia/gpu/registers.h:159-161), and a vertex shader's index in r0.x, which its last
  vertex fetch overwrites already (`fetch_inputs`, `src/xsc_lower_vertex.c`). Each stays where it arrives, its
  components live from the start to their last read; then the register takes other values. Xenia loads them into
  ordinary registers before the first instruction (xenia:src/xenia/gpu/spirv_shader_translator.cc:1510-1518,
  2050-2071). A pixel shader writing a register its interpolator arrived in is new on the console: 79 of Dusklight's
  103 pixel shaders now do. The count still covers every interpolator and the pixel parameters, read or not, as it
  always has: two of testdata's shaders would otherwise drop to one register under two interpolators.
- **Fetches as they were.** xost's fetches write all four components (`DEST_XYZW`; the runtime's patch picks a source, 0
  or 1 for each, `picked_swizzle` in `src/shader.c`), and a fetch's destination is the fetch's alone from the fetch to
  its result's last read, as before: no other value's write can race the fetch's. Letting the lanes nothing reads go
  would save 22 registers over the corpus, 1 in each of 22 shaders. A fetch's source keeps its lanes and swizzle;
  moving it, its swizzle rewritten (xenia:src/xenia/gpu/shader_translator.cc:1034-1037, 1128-1135), would save 4.
- **Not packed.** Programs the optimiser leaves alone (`can_optimize`): hardware loops, predication and relative
  addressing, Dusklight's `04a469b1240c5698.vert`, `93d36ce55e3f9bad.vert` and `c0ddfd4547c71e5a.vert` among them. They
  keep a register a temp, as before, and come out byte for byte the same. A value's components stay in one register,
  as an operand reads one.

It ran as the baseline's line with `--json ../results/2026-10-17-milestone-45/step3.json`, in 86 s; `step3.txt` is
what it printed. The optimiser on, registers a shader in the baseline, step 2 and step 3:

| | Vertex, 121 | | | Pixel, 246 | | |
|---|---|---|---|---|---|---|
| Average | 7.2 | 6.1 | 5.0 | 5.1 | 5.0 | 3.8 |
| Most | 44 | 44 | 18 | 32 | 32 | 32 |
| All shaders | 872 | 741 | 601 | 1243 | 1232 | 928 |

The corpus takes 1529 registers, against step 2's 1973 and the baseline's 2115. ALU instructions, copies, co-issued
slots, fetches and control flow are step 2's shader by shader: 6887 ALU instructions, 1271 copies, 660 co-issued slots.
The most a vertex shader takes falls from `vk_many_live.vert`'s 44 to 16, its forty dot products four to a register;
vulkan4xenon's `many_samplers.vert` is the most now, at 18 as before. Its `many_samplers.frag` keeps 32: sixteen
fetches whose results are live at once, each in a register of its own.

**Dusklight's shaders:** vertex shaders take 6.3 registers on average (step 2 7.5, baseline 9.8) and 13 at most
(13, 15): `93d36ce55e3f9bad.vert`, which isn't packed. Pixel shaders take 4.5 (6.2, 6.2) and 16 at most (17, 17):
`e3fba406929fde97.frag`, eight fetches whose results each keep a register. 115 of the 150 take fewer registers than at
step 2, 227 in all, and none more. The 19 shaders step 2 left a register or two above the baseline, 14 of them
Dusklight's, are all below it now but testdata's `fs_mtest.frag`, 6 against 5, with five `vec3` values and its input
live at once; `85c8b022385334bf.frag` goes from 10 to 12 to 6. With vkbench's: vertex 9.4, 7.2, 6.1; pixel 6.1, 6.1,
4.4. A rough floor, the most components live at once over four, is about 692 for Dusklight's shaders against the 765
they take; most of the difference is what stays in place above and the three shaders left alone.

**Checks:**
- `check_optimize.py`: 0 differ, with the baseline's 24 not simulated and 16 not compiled.
- xost's tests, a file at a time in WSL, all pass but `test_xcomb_parity.py`, which runs on Windows.
  `test_register_alloc.py` has 6 checks for the step: `vk_many_live.vert` optimised in 16 registers with the reference's
  sums, and five hand-built pixel shaders, each against the program built without the optimiser on random inputs, in
  the registers the packing should give: an interpolator's register reused, a vector moved beside a live scalar, a
  `dst` and a `mad` beside a scalar op kept in their lanes, a fetch's destination not shared. `tests/xsc_internals.py`'s
  `pixel_program` builds texture fetches for it.
- Broken on purpose:
  - an elementwise op's operands left in their lanes when its result moves: 2 of the step's checks fail, 4 of
    `test_optimize.py`'s cases, and 101 shaders of the corpus differ;
  - an input's components free from the start: 5 of the step's checks and 4 of `test_optimize.py`'s fail, and 118
    shaders differ;
  - every op's result free to move its lanes: the `dst` and `mad` checks fail, but no shader of the corpus differs, as
    none it simulates moves such an op (its `cube` shaders aren't simulated);
  - a fetch's destination free in the lanes nothing reads: the fetch check fails, 3 registers for 4, and no shader
    differs, as the simulator runs an instruction at a time.
- Serialization: `exec_entry` serializes by whole register, so a shared register could make an instruction wait for
  another value's fetch. Over the corpus's optimised programs 27 instructions are serialized at step 2 and step 3, 9 of
  them Dusklight's.
- Containers: 120 of the 150 programs in Dusklight's pack change against step 2, 40 vertex and 80 pixel; the other 30,
  the three left alone among them, come out byte for byte as before.
- xsdk_test's compiler check: `tools/gen_xsc_shaders.py` run with step 2's library gives the committed
  `tests/xsc_shaders.h` byte for byte. Regenerated, 3 of its 8 containers change: `vk_texel_offsets.frag`,
  `vk_vertex_if.vert` and `vk_vertex_uniform_if.vert`. Their SPIR-V, literals and resources are the same.
- `probe_shader_cost` builds from the pack unchanged.
- Compile time: the check took 86 s, as step 2's did. Compiling the pack's 150 programs on the PC takes 178 ms against
  step 2's 174, best of ten, 2.6% more; at that share the console's compile of them grows by about 0.2 s.

### Step 3 on the console

Run on 2026-10-17 through the console agent, everything cleaned and built in full:
- **xsdk_test:** 380 passed, the compiler check among them; the two controller checks failed with 0x48F, no controller
  connected.
- **`probe_shader_cost`** (`probe_step3.log`): pixel shaders 77,668 → 62,830 µs for a draw of each (−19.1%; −4.9%
  against step 2's 66,036), 4.53 registers a shader against 6.21 at step 2; vertex shaders 14,056 µs, as at step 2,
  with 6.34 registers against 7.49. The same 100 pictures as the baseline, and no vertex draw reaching the pixels.
- **vulkan4xenon_test:** 426 passed as committed, and 426 in each of two runs with its 84 shaders compiled with
  `--optimize` by the step's xost, which changes 348 lines of `tests/shaders.c` against step 2's.
- **Dusklight,** built against the step's xsdk and vulkan4xenon with its pack from the step's xost: the title scene
  runs at 30 fps as before and its 600th frame, captured, shows the ride into Hyrule Field as the baseline's does.
- **Broken on purpose,** an elementwise op's operands left in their lanes when the allocator moves its result: 34 of
  the probe's 100 pictures differ from the baseline.

## Compile time

Step 2 made compiling Dusklight's pack on the console 2.6 times slower, 1.32 s to 3.39 s; agreed on 2026-10-17 to bring
it back within the milestone. Built on 2026-10-17 and checked on the PC, then on the console.

**Where the time went.** Each pass of the optimiser's first round makes one change, then the round starts over
(`first_round`, `src/xsc_optimize.c`), each pass building the flow again. `compute_in_place` was no different, so a
shader ran the whole round once more for every copy it took. Counted with gprof over the pack's 150 programs, step 3
ran 4104 rounds against the baseline's 2422: 1682 more, one for each copy taken. `common_subexpressions`, which
compares each instruction with every one before it, took 55% of the time and building the flow 29%.

**What changed.**
- `compute_in_place` takes every copy it can before the round starts over. The round runs once more for a program
  whose copies it took, 125 more over the pack, 2547 in all.
- After each copy it takes, it scans again from the first instruction, as before, on the flow kept up to date rather
  than built again: the writers' and the redirected readers' reads and writes are found again, the copy's entry goes,
  and the index of which instruction writes and reads each key is rebuilt from them (`find_keys`, `drop_op`,
  `index_flow`). A copy into an export moves its writer down to it, so then the flow is built again: 227 of the 1682.

**The containers.** Each scan sees the flow a rebuild would give, so the pass takes the same copies in the same order.
What's new is that the round's other passes no longer run between two of them. A copy taken could give them something
new, such as a computation then in the same lanes as an earlier one for `common_subexpressions`; they see it once the
pass is done. Over the corpus nothing came of it: every shader, optimiser on and off, compiles byte for byte as step
3's library (`f9f970e`) compiles it, containers, literals and resources. `tests/xsc_shaders.h`, regenerated with
`tools/gen_xsc_shaders.py`, is the same, and so are `check_optimize.py`'s counts, shader by shader, `step3.json`'s.

**On the PC,** compiling the pack's 150 programs through `build/host/libxsc.so` with the optimiser on, at xsdk's split,
in WSL; best of ten compiles of each, and of three such runs. `34e6f58`'s and step 2's libraries were built from `git
archive` in a scratch directory.

| | `34e6f58` | Step 2 | Step 3 | Now |
|---|---|---|---|---|
| 150 programs | 74.3 ms | 170.3 | 174.9 | 77.0 |
| A program | 0.50 ms | 1.14 | 1.17 | 0.51 |
| The slowest, `glsl_407248b726bcb428_vert` | 3.76 ms | 10.57 | 10.85 | 3.89 |

Now takes 3.6% more than `34e6f58`. The PC put step 2 at 2.29 times the baseline where the console put it at 2.57.
Built with gprof's counting, which makes every call dearer, the pack took 605 ms at `34e6f58`, 1577 at step 3, 2.61
times, near the console's ratio, and 627 now, 1.04 times.

**Checks:**
- `check_optimize.py`: 0 differ, with the baseline's 24 not simulated and 16 not compiled. It took 87 s against step
  3's 86: its time is the simulator's.
- xost's tests, a file at a time in WSL, all pass but `test_xcomb_parity.py`, which runs on Windows. `test_optimize.py`
  has a case for this: three copies taken in one scan, the last copying the vector the first two put their writers'
  results in, so it finds those writers only on the flow as it stands after them.
- A scratch build checked the flow kept up to date against one built afresh after every copy taken, over the corpus:
  2045 copies, the same each time.
- Broken on purpose:
  - the redirected readers' reads left as they were: `test_register_alloc.py`'s `vk_many_live.vert` check fails, and
    1 shader of the corpus differs;
  - the writer's left as they were: the new case fails, and 23 shaders differ;
  - the copy left in the flow: the new case fails, 46 shaders differ, and one more isn't compiled.
- `make XSC_PACK=/mnt/z/dusklight-xenon/platforms/xenon/glsl_pack.c build/probe_shader_cost.xex build/probe_xsc.xex`
  builds clean.

### Compile time on the console

Run on 2026-10-17 through the console agent, everything cleaned and built in full. `probe_xsc`
(`probe_xsc_compile_time.log`), built from Dusklight's pack as this xost makes it, byte for byte step 3's: all 150
programs compile on the console as on the PC, in 1.42 s, 9.5 ms a program, the slowest 81 ms, the same to 0.1% in two
runs; before the milestone 1.32 s, 8.8 ms and 76 ms, at step 2 3.39 s, 22.6 ms and 228 ms. xsdk_test passed 380, the
controller checks failing with no controller connected; vulkan4xenon_test 426; `probe_shader_cost` the same 100
pictures and times as step 3, its run back to 11.8 s.

## Step 4: more co-issue

Built on 2026-10-17 and checked on the PC, then on the console. Every ALU instruction has a vector and a
scalar slot (xenia:src/xenia/gpu/ucode.h:1854-2095). The optimiser's last pass, `co_issue` (`src/xsc_optimize.c`), puts
a scalar op alone into an instruction holding a vector op alone, or the other way, moving one of the two past the ops
between.

**Why so few paired.** Counted by what each pipe does in the corpus's optimised microcode at step 3, a vector op
writing a component or changing state and a scalar op other than `retain_prev`: 657 instructions held both, 6114 a
vector op alone, and 116 a scalar op alone. The pass had already paired nearly every scalar op it saw. The 116, counted
at the optimiser's end:
- **73 in programs the optimiser leaves alone:** 62 in the 9 that read a constant at a run-time index, 49 of them in
  Dusklight's `93d36ce55e3f9bad.vert` and `c0ddfd4547c71e5a.vert`, and 11 in hardware loops and predicated code.
- **19 kept from an independent vector op by the order:** 18 in their stage, where the later of the two couldn't move
  up past an op between, and 1 whose vector op is past the colours' alloc.
- **16 depend:** every vector op of their stage they could join reads their result, or a value computed from it, or
  computes what they read.
- **8 had only exports and instructions already co-issued** independent of them. An export's scalar op exports to the
  same register as its vector op (ucode.h:2041-2042), so a scalar op into a temp joins none.

None was kept apart only by the two-constant limit or a destination both write.
In Dusklight's and vkbench's shaders all 50 lone scalar ops were in the three vertex shaders left alone: 47 `rcp`,
`sqrt` and `rsq`, and the 3 `maxasf` that load a0. `check_optimize.py`'s co-issued count, taken from the disassembly,
holds 3 more at every step: lone scalar ops whose idle vector op names a register other than r0.

**What changed.**
- **Both ways.** Once no op joins an earlier one anywhere in the program, an op joins a later one, moving down past
  ops that neither read nor write what it writes nor write what it reads. Either way the later of the two mustn't read
  what the earlier writes, as both pipes read their operands before either writes a result
  (xenia:src/xenia/gpu/shader_interpreter.cc:287-955). Every pair found before is found first, the same way.
- **Constants at a run-time index.** A program reading a constant at a0 past its register goes through `co_issue`
  alone, as the other passes don't model the index, and keeps its registers whole, as before. Nothing moves past the
  `maxas` or `maxasf` that loads a0, as nothing moves past any op that changes state. An instruction's two addressing
  bits go to its constant operands in order (ucode.h:1945-1964) and a temp operand takes neither
  (xenia:src/xenia/gpu/shader_translator.cc:1210-1238), so a scalar op joins there only with a temp operand and no
  index of its own: a constant beside a copy of `c[4+a0]` could be read at a0 too, and the two-constant check counts
  `c4` and `c[4+a0]` as one.
- **The simulator** (`xdis/equivalence.py`) loads a0 as `maxas` rounds and `maxasf` floors it, and reads an addressed
  constant a0 past its register, 0 below c0 (shader_interpreter.cc:260-286, 808-822), so `check_optimize.py` compares
  those 9 programs too; a constant at the loop counter, and a vector `maxa`, stay unsupported. At step 3's xost it
  reports 0 differ on them.
- **Compile time.** `join` asked `stage_of` which stage each op it passed was in, a scan of the program each time; it
  finds its stage's bounds once now.

**The hardware's rules,** as the pass keeps them:
- A scalar op reads src3 alone, its `a` and `b` from the swizzle's w and x (shader_translator.cc:1417-1460), so its
  partner reads two operands at most: `mad`, the `cnd`s and `dp2add` read a third (xenia:src/xenia/gpu/ucode.cc:83-116)
  and never pair, as before.
- Two different constants an instruction, and three only if the second and third are one (`constants_fit`, as
  before).
- Exports: neither op of a pair is one, as above.
- Predication, and the previous scalar result: programs with a predicated instruction, or one reading `ps` (`adds_prev`
  and the like, or `retain_prev` written), are left alone, so no op is moved across a predicate and no `ps` a later op
  reads changes.
- State: nothing moves past a kill, a setp, or what loads a0.
- Register packing: a pair is still a two-operand vector op and a scalar op, so step 3's rule holds. The vector result's
  lanes may move, its two operands' swizzles with them, while the scalar op's src3 follows only its own value's lanes
  (`xsdk_xsc_move_lanes`); the pass never puts a scalar op beside a `mad` or a `cnd`, which step 3 keeps in their lanes.

It ran as the baseline's line with `--json ../results/2026-10-17-milestone-45/step4.json`, in 94 s; `step4.txt` is
what it printed. The optimiser on, the baseline, step 3 and step 4:

| | Corpus | | | Dusklight and vkbench | | |
|---|---|---|---|---|---|---|
| ALU | 8895 | 6887 | 6821 | 6641 | 4956 | 4909 |
| Co-issued | 675, 7.6% | 660, 9.6% | 726, 10.6% | 583, 8.8% | 583, 11.8% | 630, 12.8% |
| Copies | 3253 | 1271 | 1237 | 2397 | 733 | 701 |
| Registers | 2115 | 1529 | 1532 | 1117 | 783 | 785 |

66 more scalar ops pair: 15 both ways, in 14 shaders, and 51 in 5 programs reading at a run-time index, 47 of them in
Dusklight's: `93d36ce55e3f9bad.vert` 221 to 199 ALU instructions, `c0ddfd4547c71e5a.vert` 245 to 220. Copies count 34
fewer as a copy with a scalar op beside it isn't counted one; the instructions stay. Fetches are step 3's, and control
flow 11 instructions fewer, exec clauses holding fewer instructions.

**The ALU target isn't reached.** The corpus's 6821 instructions are 23.3% under the baseline's 8895; the milestone
asks for 6671 or fewer, 150 more. Dusklight's and vkbench's are 26.1% under.

**Registers** grow by 3, a register each in three shaders: testdata's `stepsmooth.frag`, 4 to 5, its two `rcp`s moved
down keeping their sources live longer; and Dusklight's two, 13 to 14 and 12 to 13, where copies of constants moved up
beside a scalar op start their registers earlier in programs whose registers stay whole. Dusklight's vertex shaders take
6.38 registers on average (6.34 at step 3) and 14 at most (13); its pixel shaders are step 3's, 4.53 and 16.

**What's left unpaired:** 50 lone scalar ops over the corpus, counted as above.
- 11 in hardware loops and predicated code, left alone.
- 11 `maxas` and `maxasf`, which load a0 and change state.
- 16 depend on every vector op they could join, as before.
- 11 have only exports and co-issued instructions independent of them: testdata's `divide.frag`, four `rcp`s each
  feeding an export of its own, `gamma.frag` and `mathfns.frag` 3 each, `verify_sqrt.frag` 1. Three of them lost their
  partner to another scalar op now that ops pair both ways.
- 1, `vk_front_facing.frag`'s `rcpc`, whose independent `add` is past the colours' alloc: pairing across a stage's end
  would take this one alone, so the pass still stops there.

In Dusklight's and vkbench's shaders 3 are left, the `maxasf` of the three relative vertex shaders. Every other scalar
op there pairs, so more co-issue would need the scalar unit to take work the lowering gives the vector one.

**Checks:**
- `check_optimize.py`: 0 differ, 15 not simulated, 16 not compiled. With the simulator as it was before the step, the
  same run reports 0 differ with the baseline's 24 not simulated and 16 not compiled.
- xost's tests, a file at a time in WSL, all pass but `test_xcomb_parity.py`, which runs on Windows.
  `test_optimize.py` has 6 cases for the step: a scalar op and a vector op each moved down past a `mad` to their
  partner; a scalar op kept above a `mad` writing what it reads; a scalar op joining a copy at a0; a scalar op reading
  `c4` kept apart from a copy of `c[4+a0]`; and a copy at a0 kept below a second `maxasf`.
- Broken on purpose:
  - the later op's read of the earlier's result checked on the wrong side when an op moves down: 2 of
    `test_optimize.py`'s cases fail, and 20 shaders of the corpus differ;
  - the vector op's index dropped when a scalar op joins it: the copy-at-a0 case fails, and Dusklight's
    `93d36ce55e3f9bad.vert` and `c0ddfd4547c71e5a.vert` differ;
  - an op moved down past one that writes what it reads: the `mad` case fails, and no shader differs;
  - a scalar op's constant let in beside a copy at a0: the `c4` case fails, and no shader differs;
  - ops moved past what loads a0: the second-`maxasf` case fails, and no shader differs.

  The corpus holds none of the last three's shapes: no scalar op there reads a constant in a program read at a
  run-time index, and none could join an op across a `maxasf`.
- Containers: 2 of the 150 programs in Dusklight's pack change against main's library (`62db399`),
  `93d36ce55e3f9bad.vert` and `c0ddfd4547c71e5a.vert`; the other 148 come out byte for byte as before. Over the corpus
  19 shaders change, all optimised: those 2, 6 of testdata's, vulkan4xenon's `matrix_index.vert` and 10 of its
  `aurora_*.vert`. vkbench's 7 don't.
- xsdk_test's compiler check: `tools/gen_xsc_shaders.py` gives the committed `tests/xsc_shaders.h` byte for byte with
  main's library, and with the step's too: none of its 8 containers changes.
- `make XSC_PACK=/mnt/z/dusklight-xenon/platforms/xenon/glsl_pack.c build/probe_shader_cost.xex build/probe_xsc.xex`
  builds clean.
- Compile time, the pack's 150 programs through `build/host/libxsc.so` on the PC as before, best of ten compiles of
  each and the best of several runs: 69.9 ms against main's 69.3, 0.9% more, the slowest program 3.62 ms against 3.74.
  The two relative vertex shaders take 0.87 and 1.11 ms against 0.13 and 0.14, a pass over the program for each of
  their 22 and 25 pairs; before `join` found its stage's bounds once they took 1.23 and 1.58, and the pack 72.3 ms. At
  that share the console's 1.42 s grows by about 0.01 s.

## a0 lost at an alloc: Dusklight's lighting

`probe_relative_constants` found on the console that a0 doesn't survive a control flow `alloc`: a read at a0 after
one needs an a0 load after it (`probe_relative_constants.log`, `probe_relative_constants_stress.log`;
docs/HARDWARE.md, "a0 doesn't survive an alloc"). Built on 2026-10-17 and checked on the PC; the console runs are to
come.

**Dusklight's lighting.** `93d36ce55e3f9bad.vert` and `c0ddfd4547c71e5a.vert` skin a vertex: they load a0 with
`maxasf` after the position's alloc and read its matrix, `c[120..122+a0]`, for the position. Its normal matrix,
`c[152..154+a0]`, they read for the lighting after the interpolators' alloc, at an a0 the console has lost, so their
skinned characters have been lit from wrong rows, before the milestone and at step 4 alike. What step 4 changed is
which wrong values. Before it, `can_optimize` left programs reading at a run-time index alone, and the read of
`c[152+a0]` stood by itself; step 4 let `co_issue` run on them, and in 93d36 that read joined an `rcp`
(`max r8, c[152+a0], c[152+a0]` with `rcp r7, r6.x`), and 22 scalar ops moved to pair, 221 ALU instructions to 199.
What a read at a lost a0 gives depends on the instructions and registers around it: case 8 of the probe replays 93d36's
instructions from its `maxasf` to that read both ways, and neither reads the row, the old span giving `E3E3E3E3`,
`CACACACA`, `73737373` and `55555555` over its four quads, step 4's `ADD1E4FF`, `F5E3E4FF` and `FFFFE4FF`. So step 4
moved the lighting from one wrong normal matrix to another, and neither was right.

**What changed.** xost's lowering (`set_a0`, `src/xsc_lower_block.c`) keeps the instruction that loaded a0 and the
program's alloc count then, which `xsdk_xsc_program_alloc` (`src/xsc_program.c`) counts. A read at the same index with
an alloc since emits that same `maxa`, `maxas` or `maxasf` again first, in the read's alloc region, right before it;
an if/else's arms save and restore that with the rest of a0's state, so a load in one arm doesn't stand for the
pixels that skip it. Every alloc xost emits counts, the position's, the interpolators' and the colours' (it emits no
memory export), in vertex and pixel shaders. The reload is in the program before the optimiser and the allocator:
nothing moves past an a0 load, so `co_issue` keeps it between the alloc and the read, and the allocator keeps its
source, the index or the index times an element's registers, live to it. Over the corpus that cost no register.
- **Which shaders take one:** Dusklight's 93d36 and c0dd, before their `c[152+a0]`, and testdata's new
  `vk_dynamic_interpolator.vert`. Dusklight's `04a469b1240c5698.vert` has no interpolators and reads at a0 only after
  the position's alloc, where it loads a0; vulkan4xenon's `matrix_index.vert` reads its colour after the interpolators'
  alloc at another index, `i / 3`, which it loads there already. Nothing else in the corpus reads at a0 after an alloc.
- **The cost:** 93d36 199 to 200 ALU instructions and c0dd 220 to 221, registers 14 and 13 as before; optimiser off,
  221 to 222 and 245 to 246, an exec more each, registers 13 and 12 as before. In 93d36 the `rcp` step 4 paired with
  the read pairs with a `dp3` instead, so the co-issued slots stay 22 and 25. The index's register now lives from its
  fetch to the reload, past the position, and fits within the registers the lighting needs anyway.
- **aL:** xost reads at the loop index only in `xsdk_xsc_lower_real_loop`'s loop (`src/xsc_lower_loop.c`), before the
  colours' alloc; over the corpus only testdata's `loop_array.frag` does, there. The probe didn't try aL after an
  alloc.

It ran as the baseline's line with `--json ../results/2026-10-17-milestone-45/reload.json`; `reload.txt` is what it
printed. The corpus has 384 shaders now, testdata's `vk_dynamic_interpolator.vert` among them.

**Checks:**
- `check_optimize.py`: 0 differ, 15 not simulated, 16 not compiled. Against `step4.json`, only 93d36, c0dd and the new
  shader's counts change; with the optimiser on the corpus has 6835 ALU instructions, 6823 without the new shader's
  12, against step 4's 6821.
- Containers: every program of the corpus, optimiser off and on, and the pack's 150 as it builds them, against
  `4204b4c`'s library built from `git archive` in a scratch directory: all but 93d36's and c0dd's come out byte for
  byte, containers, literals and resources, 910 of 916. Scanned, each read at a0 in them has a load since its alloc; at
  `4204b4c` 93d36's and c0dd's three `c[152..154+a0]` didn't.
- xost's tests, a file at a time in WSL, all pass but `test_xcomb_parity.py`, which runs on Windows.
  `test_dynamic_index.py` checks, in every testdata shader that reads at a0, optimiser off and on, that each read at a0
  has a load since its alloc and that a load again comes only where such a read needs it, and that
  `vk_dynamic_interpolator.vert`'s comes after the interpolators' alloc, right before the first of its two reads
  there, the same instruction as the position's; the simulator gives each of its 16 indices' position and colour.
  Broken on purpose, the reload dropped fails it (a read at a0 after alloc 2 with no load since), and a reload before
  every read after an alloc fails it too (a0 loaded again where it holds that already).
- xsdk_test's raster test draws `vk_dynamic_interpolator.vert`, compiled on the console, as one draw of 70 by 8 quads
  10 pixels square, quad i's vertices at index i % 16, and checks every quad's centre against its index's colour in
  each of 59 frames; its check line counts the wrong quads, the first 8 logged. It adds a draw of 3360 vertices and
  560 pixel reads to each frame. `tests/xsc_shaders.h`, regenerated with `tools/gen_xsc_shaders.py`, has it as the
  compiler check's ninth shader; its other 8 containers are the same. For the console to show the check failing
  without the fix, a copy of xsdk_test was built with the reload off: from a scratch copy of the fix's commit,
  `set_a0`'s reload replaced by `return true` and `tests/xsc_shaders.h` regenerated with that compiler, so only the
  ninth container differs, by the reload's instruction, and the compiler check agrees; `make BUILD=build-noreload`.
- `make clean && make -j32`, then `make -j32 XSC_PACK=/mnt/z/dusklight-xenon/platforms/xenon/glsl_pack.c
  build/probe_shader_cost.xex build/probe_xsc.xex build/probe_relative_constants.xex`: no warnings.
- Compile time, the pack's 150 programs on the PC as before: 71.5 ms against `4204b4c`'s 72.2, the slowest 3.71 ms
  against 3.73; 93d36 and c0dd 0.84 and 1.07 ms against 0.89 and 1.13.

### Step 4 and the a0 reload on the console

Run on 2026-10-17 through the console agent, everything cleaned and built in full.

Step 4:
- **xsdk_test:** 380 passed; the controller checks failed with 0x48F, no controller connected.
- **`probe_shader_cost`** (`probe_step4.log`): vertex shaders 13,916 µs for a draw of each, against step 3's 14,056 and
  the baseline's 16,472 (−15.5%). Dusklight's `93d36ce55e3f9bad.vert` 638.7 → 578.4 µs and `c0ddfd4547c71e5a.vert`
  704.4 → 635.9 µs. The same 100 pictures.
- **`probe_xsc`,** from the pack as the step's xost makes it: 150 of 150 as on the PC, in 1.45 s.
- **vulkan4xenon_test:** 426 as committed. With its shaders compiled with `--optimize` by the step's xost, 248 lines of
  `tests/shaders.c` different from step 3's, 426 in each of two runs.
- **Dusklight:** runs at 30 fps. Captured every 150th title frame against a build from before the milestone, it lights
  Link and Epona differently. That led to `probe_relative_constants`: both builds read the light's rows at a0 past
  the interpolators' alloc, which a0 doesn't survive (docs/HARDWARE.md, "a0 doesn't survive an alloc").

The a0 reload (`probe_reload.log` for the probe):
- **xsdk_test:** 383 passed, the two controller checks failing as above. The grid of 560 quads read at a0 past the
  interpolators' alloc had 0 of 33,040 wrong, in each of two runs. With the reload off, 33,040 of 33,040 were wrong,
  and that check alone failed.
- **`probe_xsc`,** from the pack as this xost makes it: 150 of 150, in 1.43 s.
- **`probe_shader_cost`:** the same 100 pictures; vertex shaders 13,935 µs.
- **vulkan4xenon_test:** 426.
- **Dusklight,** built against this xsdk with its pack from this xost: Link and Epona lit from the light's own rows,
  the chestnut coat and white mane in the sunset light.
