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

Built on 2026-10-17 and checked on the PC; the console runs are to come. In a program the optimiser rewrote, the
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
