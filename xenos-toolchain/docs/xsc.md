# xsc — GLSL → Xenos shader compiler

Pipeline: **GLSL → glslang → SPIR-V → xsc → Xenos container**. glslang
(Khronos reference compiler) does parsing/semantics/optimization; xsc lowers
the SPIR-V to microcode through the hardware-proven Program builder. xsc is C,
xsdk's `src/xsc_*.c`: titles call `xsdk_xsc_compile` (`include/xsdk/xsc.h`), and
on the PC `xsc/lower.py` calls the same code built as `build/host/libxsc.so`.
Until 2026-10-14 it was Python; the C port gives the same bytes. The
Mesa/NIR optimizer is a someday option; the runtime combiner case is already
served by `xcomb`.

## Use

```
python3 -m xsc.lower shader.vert.spv -o shader.bin     # SPIR-V -> container, in WSL
```
Library: `lower(module) -> bytes`, `lower_ex(module) -> (bytes, manifest)`.
Get the SPIR-V with: `glslang -G --target-env opengl shader.frag -o out.spv`,
or Vulkan's with `glslang -V shader.frag -o out.spv`: vertex inputs, varyings
and outputs lower the same either way (`tests/test_vulkan_spirv.py`).
Vulkan's uniform blocks, push constants and samplers land as "Interface
conventions" says; `resolve_interface(module).blocks`, `.samplers` and
`.comparisons` give where.

Instruction templates (the field skeletons the lowering fills in) come from
Xenia's definitions, `xsc/templates.py`. Before xost joined xsdk,
`tests/test_templates.py` checked them against the instructions they first
came from; that test and its test vectors stay in xost's archived repo (github.com/xenosworks/xenos-toolchain, last commit 4a785c6).

## Supported (v1, branch-free)

| GLSL / SPIR-V | Xenos lowering |
|---|---|
| vertex `mvp * pos` (MatrixTimesVector) | 4× `dp4` → oPos |
| vertex chained `mat4 * (mat4 * pos)` | first mat4 x vec4 into a temp, second mat4 x vec4 -> oPos |
| vertex `vec4 * mat4` (VectorTimesMatrix) | row-vector `mul`+`mad` |
| vertex `(mat4 * mat4) * vec4` | product rows via `vec*mat`, then 4× `dp4` |
| vertex `+` `-` `*`, `vec * scalar`, `dot()`, swizzled temps | same ALU lowering subset as fragment, feeding position or interpolator exports |
| passthrough varyings | interpolator exports (max o*N*) |
| `texture()` (ImageSampleImplicitLod) | `tfetch2D` |
| samplerCube `texture()` | cube swizzles + rcp/bias + `tfetchCube .yxw`; each face and its s and t as Vulkan picks them, on the simulator with the cube op as Xenia runs it (`tests/test_cube_and_3d.py`) |
| sampler3D `texture()` | `tfetch3D` at the coordinate's `.xyz` |
| sampler2DShadow `texture()` (OpImageSampleDref) | `tfetch2D`, then the reference clamped to 0-1 (`max_sat`), `sgt`/`seq`/`sgt` for the reference below, equal to and above the depth, and `dp3` with the comparison's register (see Interface conventions; `tests/test_shadow_compare.py`); samplerCubeShadow refused, and textureProj's comparison not lowered |
| `*` `+` `-` (FMul/FAdd/FSub) | mul / add / add-neg |
| `/` (FDiv) | `a · rcp(b)`; scalar-pipe `rcp` per component (scalar divisor: once + broadcast) |
| `mod()` (OpFMod) | `a − b·floor(a/b)` |
| `vec * float` (VectorTimesScalar) | mul, scalar `.xxxx` broadcast |
| scalar int -> float | folded literals or float-backed `trunc` + add/mul |
| `dot()` / `cross()` | `dp2add` with a zero third source (vec2), `dp3`, `dp4`, broadcast / yzx·zxy mad |
| `mix` / `max` / `min` / `clamp(x,0,1)` (GLSL.std.450) | sub+mad / max / min / `_sat` modifier (free) |
| `step` / `smoothstep` | `kSge` / `t·t·(3−2t)`, `t=sat((x−e0)/(e1−e0))` |
| `normalize` / `length` / `distance` | dp3·rsq·mul / dp+sqrt / sub+dp+sqrt (scalar pipe) |
| `sqrt` `inversesqrt` `exp2` `log2` `exp` `log` | scalar-pipe `kSqrt`/`kRsq`/`kExp`/`kLog` (+const for natural) |
| `pow(x,y)` | per-component `exp2(y·log2 x)` (scalar pipe) |
| `reflect(i,n)` | `i - 2·dot(n,i)·n` (dp + mad) |
| `sin` / `cos` / `tan` | scalar `kSin`/`kCos`; `tan = sin·rcp(cos)` — **range-limited ~[-π,π]** (see below) |
| `asin` / `acos` / `atan` | polynomial approx (A-S sqrt form / π/2−asin / odd poly); ~7e-5 over [-1,1], atan range [-1,1] (see below) |
| `.rgb` `.xy` (identity) / `.bgra` (reorder) / two-source `vec4(a.xy,b.zw)` | same regs / swizzled mov / per-component masked movs |
| `v.x` (CompositeExtract), `vec4(a,b,c,d)` (CompositeConstruct) | masked exports |
| `floor` / `ceil` / `trunc` / `round` / `fract` | `kFloor` / `-floor(-x)` / `kTrunc` / `floor(x+.5)` / `kFrc` |
| `abs()` / `sign()` | `max(x,-x)` / `sgt(x,-x)−sgt(-x,x)` |
| `radians` / `degrees` | mul by pooled `π/180` / `180/π` |
| `clamp(x, lo, hi)` arbitrary | `min(max(x,lo),hi)`, lo/hi pooled literals |
| **`if/else`**, incl. **nested**, assigning the output | branch-free chained `cndgt`/`cndge`/`cndeq` selects |
| **`if/else` assigning a Function-local** later read by the output | branch-free `cnd*` select into a temp registered as that local's merged value (pixel shaders) |
| `if/else`, or a `?:` glslang branches for, in a **vertex** shader, nested, `&&`/`\|\|` (a Phi) | the unroller runs both arms and merges each local and output they leave apart with a select: a `cnd*` on the comparison branched on, on a condition every vertex shares too, as execs on the predicate hung the GPU (see Control flow; `tests/test_vertex_branches.py`) |
| **`discard` / `OpKill` inside `if(cond)`** | `kill_gt`/`kill_ge`/`kill_eq`/`kill_ne` ALU side effect; confirmed on the console, a `discard` on `!gl_FrontFacing` (vulkan4xenon milestone 22) |
| **`for`/`while`** with a compile-time trip count, incl. **nested** | unrolled to branch-free SSA; induction variable constant-folded (see below) |
| uniform-bound loop `sum += const` | real `LOOP_START/END` (G28 pending) |
| `for (int i = 0; i < b.n; i++)`, `n` an int in a push constant or uniform block, in a vertex or pixel shader | a hardware loop the runtime counts from the block: what the loop carries in registers, `i` a float counter, an if/else in the pass, a break ending it (see "Loops a block counts"; `tests/test_hardware_loops.py`) |
| `vec4 t = ...;` (function-local) | store-forwarding |
| uniforms (block + bare), samplers | constant regs / fetch consts |
| Vulkan uniform / push constant block members at any std140 offset | the member's register, swizzled from its first component |
| block matrix (column-major, std140) `M * v` / `v * M` / `A * B` | `mul`+`mad` over columns / 4x `dp4` with columns / columns of A times each column of B |
| block array at a run-time index, `rows[int(f) + k]`, `m[i]` | `movasf` (`int(f)`) or `movas` sets a0, then `c[a0 + base + k]` is copied into a temp; reads at one index share the `movas` (`tests/test_dynamic_index.py`) |
| fragment `gl_FragCoord.xy`, `gl_FrontFacing` | the pixel parameters in the register past the interpolators, `\|r.xy\| + 0.5`, `.zw` refused; facing from x's sign (see Facing) |
| fragment `gl_FragDepth`, stored once outside any `if/else` | `oDepth.x___` after the colour, the record's ps_export_mode 3 (`tests/test_frag_depth.py`) |
| fragment `gl_FragDepth` stored in both arms of an `if/else`, or before one and again in an arm | merged by a select as a vertex shader's outputs are, exported once; stored on one path alone, refused (see Control flow) |
| a store into one component of a local or an output, `v.x = …` | an insert into the vector it held (`tests/test_local_stores.py`) |
| vertex `gl_InstanceIndex`, firstInstance included | a float constant the runtime writes each instance's index into (see Interface conventions); `float()` of it, integer arithmetic on it and a block array's index at it lower as integers do (`tests/test_instance_index.py`). `gl_VertexIndex` is refused |
| float / vec literals in FS/VS | constant pool; 0/1 folds (see below) |
| ternary `cond ? a : b` (OpSelect) | `f + (t−f)·cond` (sub + mad) |
| vector relational `lessThan`/`greaterThan`/`equal`/`notEqual`/… → bvec | per-component `sgt`/`sge`/`seq`/`sne` (1.0/0.0) |
| `any` / `all` / `not` (bvec) | horizontal `max` / horizontal `min` / `1−v` |
| `&&` / `\|\|` | `min` / `max` of 0/1 bools |
| `refract(I,N,eta)` | Snell vector + total-internal-reflection (`k<0`) mask |
| runtime int `/` `%` (SDiv/SMod/U*) | `trunc(a·rcp(b)+ε)`; remainder `a−b·q` — **approximate, non-negative domain** |
| `mat3` / `mat2` × vec | per-row `dp3` / `dp2add` (matrix uploaded as rows, same as mat4) |
| `textureLod`, `texture(…,bias)`, `textureGrad` (2D), and their projective forms | a LOD the fetch's own LOD-bias field holds (1/16 steps, -4 to 3.9375), any other through `setTexLOD`; an explicit LOD turns the computed one off; gradients through `setGradientH` and `setGradientV` (see LOD and gradients) |
| `textureProj` | perspective divide `uv·rcp(w)` then `tfetch2D` |
| `texture()` with a computed coord | coord expression emitted, then tfetch (not just bare interpolators); a coord from another sample (a dependent read) has that sample fetched first; in a pixel shader the coords' work goes first and the fetches together (see Fetch layout) |
| `CompositeInsert` (`v[i] = x`) | copy + masked component mov |
| uniform-array loop `for(i<n) sum += w[i]` | real loop; body `add acc, acc, c[base+aL]` (aL-relative array read) |
| `for(i<n){ …; if(acc.c CMP k) break; }` | body `setp_*` + a predicated LOOP_END (data-dependent break) |
| if/else in a loop body with a **constant** condition (e.g. `if(i<k)`) | unroller picks the taken arm per iteration |
| if/else **on data** in an unrolled loop's body | both arms each pass, merged by a select (`tests/test_loop_branches.py`) |
| `transpose` (mat2/3/4) | per-row column gather (swizzled movs) |
| `determinant` (mat2/3) | mat2 `ad−bc`; mat3 `row0·cross(row1,row2)` |
| `inverse` (mat2/3) | mat2 closed form; mat3 adjugate (cofactor crosses) / det · `rcp` |
| `dFdx` / `dFdy` / `fwidth` | `getTextureGradients` fetch; ddx=grad.xz, ddy=grad.yw |

## Interface conventions (GLSL ↔ Xenos)

- VS inputs → vfetch in Location order; vertex-element usage by location:
  0=POSITION, 1=COLOR, 2=TEXCOORD, 3+=TEXCOORD(idx++). Host vertex format
  must match. VS output BuiltIn Position → oPos; Location *N* → interpolator
  o*N*. FS input Location *N* → register r*N*; output Location 0 → oC0.
- Bare uniforms → constant registers by Location; scalars read the register's
  `.x`.
- Uniform and push constant blocks → the block's raw std140 bytes in
  consecutive registers, for a host that copies them in as they are. Push
  constants take the first registers (after any bare uniforms), then each
  uniform block in (DescriptorSet, Binding) order. A member at byte offset *o*
  of a block at register *b* sits in *b* + *o*/16 from component (*o*%16)/4.
  Matrices keep their memory layout: column-major ones (glslang's default) hold
  a column a register, so `M * v` lowers to multiply-adds over columns.
- A block's array read at a run-time index takes `a0` from the index: one
  index a chain, into an array on 16-byte lines, `int(f)` for `f >= 0` as
  indices are (a0 is `floor(f)`), and the result within -256 to 255
  (xenia:src/xenia/gpu/ucode.h:1138-1150).
- gl_InstanceIndex (Vulkan's InstanceIndex) → the float constant register
  past the blocks, before the literals, so no uniform block the GPU may load
  shares its group of 4. The runtime writes the instance's index, a float, into
  every component of it before each instance's draw, as vulkan4xenon draws an
  instance at a time (vulkan4xenon:docs/ROADMAP.md, milestone 24). The
  compiler lists it among the resources, kind `XSDK_XSC_INSTANCE_INDEX`
  (`include/xsdk/xsc.h`), "instance" in `xsc/native.py`, and the container's
  constant table names it. A vertex shader's other built-in inputs, as
  gl_VertexIndex, aren't streams, and loading one is refused.
- gl_FragCoord → the pixel parameters, which the container turns on: its
  record's SQ_PROGRAM_CNTL has PARAM_GEN, and its SQ_CONTEXT_MISC has
  PARAM_GEN_POS, the register past the interpolators, where the GPU writes the
  pixel's column and row (xenia:src/xenia/gpu/registers.h:140, 158-234), and
  SC_OUTPUT_SCREEN_XY, without which x read 0 on the console
  (vulkan4xenon:results/2026-10-12-milestone-11-step-3/). xsdk takes both
  registers from the record as they are.
- gl_FragDepth → export 61's x (xenia:src/xenia/gpu/ucode.h:1842,
  dxbc_shader_translator.cc:1591-1594), with SQ_PROGRAM_CNTL's ps_export_mode
  3 in the record, where libxenon's 2 is colour alone: with 2 the console drew
  nothing of such a shader, and with 3 its depth reached the depth test
  (xsdk:results/2026-10-16-depth-export/). xsdk turns hierarchical depth off
  for a pixel shader whose record has it.
- The execs keep addressing mode 0 when ALU instructions read at a0: a pixel
  shader with its execs' mode 1 drew black on the console (the same results).
- Samplers (UniformConstant) → with a DescriptorSet decoration (Vulkan), fetch
  constants from 0 in (DescriptorSet, Binding) order; without (OpenGL), fetch
  constant = Binding.
- A sampler2DShadow's comparison → a register past the blocks, before the
  literals, named `_compare<slot>`, its resource `comparison` (the C's
  `XSDK_XSC_COMPARISON`). A Vulkan shader takes its compare op from the
  sampler bound at run time, so the runtime writes the op's weights there:
  1 or 0 for the reference below the depth, equal to it and above it, then 0,
  VkCompareOp's bits 0 to 2. The comparison runs in the ALU after the fetch,
  which works whatever the hardware does: whether Xenos compares in the fetch
  hasn't been checked on the console (vulkan4xenon:docs/ROADMAP.md, milestone
  23). It compares the filtered depth, so a linear filter blends depths, not
  results.
- An ALU instruction reads one constant register; a second goes through a temp.

## Literal constant pool (the host-upload contract)

The Xenos has no ALU float immediates, so GLSL literals are allocated
constant registers **after** the uniform-declared constants.
`lower_ex()` returns a **manifest** `[(register, (f0,f1,f2,f3))]`; the host
must upload those values to the matching shader stage before drawing:
`Xe_SetPixelShaderConstantF` for fragment manifests and
`Xe_SetVertexShaderConstantF` for vertex manifests, matching uniforms.
Example: `mix(tex, color, 0.5)` →
`add r3, color, -tex; mad oC0, r3, c0, tex` with manifest
`[(0, (0.5,0.5,0.5,0.5))]` — upload 0.5 to c0.

`0.0`/`1.0` are folded out of the manifest where there is an equivalent
no-literal encoding: algebraic identity operands (`x+0`, `x*1`, `x/1`), unit
axis `dot()` constants (a source swizzle/extract), and final export lanes
(`oC0.z = 0.0`, `oC0.w = 1.0`) via Xenos export constant-result masks. General
ALU sources do **not** have `k0/k1` source components, so uses such as
`max(x, 0.0)` still need a pooled literal unless a separate algebraic lowering
proves an equivalent form.

## Scalar pipe (the transcendentals)

Xenos `rsq`/`sqrt`/`exp2`/`log2`/`sin`/`cos` are **scalar-pipe** ops: the ALU
instruction's vector half is a nop (`vector_write_mask=0`) and the scalar
half reads one component of src3 and broadcasts its result. `normalize`,
`length`, `pow`, etc. compose these with `dp3`/`dp4` and `mul`. No common-
subexpression elimination yet, so `normalize(v)` + `length(v)` recomputes
`dot(v,v)` twice — correct, slightly redundant.

## Approximate builtins (documented accuracy / range)
- **`sin`/`cos`/`tan`** — the bare hardware `kSin`/`kCos` are accurate only
  near `[-π,π]`; arbitrary arguments need a `frac`-based range-reduction
  prologue (math in `docs/builtin-lowering.md`). `tan = sin·rcp(cos)`.
- **`asin`/`acos`** — Abramowitz-Stegun sqrt form, max error ~7e-5 over
  `[-1,1]`. `acos = π/2 − asin`.
- **`atan`** — odd polynomial, accurate on `[-1,1]` (no range reduction for
  `|x|>1` yet; `atan2`/wide-range is a refinement).

## Register budget

A shader may use only its kind's share of the shader unit's register split,
which the runtime sets in `SQ_GPR_MANAGEMENT`; past it the GPU silently
**hangs** on hardware (oracle can't see this). libxenon's set-up leaves pixel
shaders **16 GPRs** (r0..r15) and vertex shaders 112; xsdk's leaves each 64,
the kernel's own split, and a pixel shader using 64 ran on the console there
(xsdk:docs/HARDWARE.md, milestone 24). `xasm.program.RegisterBudget` names a
split: `LIBXENON_REGISTERS` by default, `XSDK_REGISTERS` for xsdk's titles
(`lower_ex(module, XSDK_REGISTERS)`, or `--pixel-registers 64
--vertex-registers 64` on `xsc.lower` and `xasm.textasm`). A shader past its
share raises `RegisterBudgetError`.

The program builder runs a **linear-scan register allocator** when it finishes
every program xsc lowers (`xsdk:src/xsc_program.c`), and `xasm/program.py` on a
hand-built one only when it needs >16
registers (pixel) or >64 (vertex, the most an instruction names), so those
compile byte-for-byte as before: it recycles dead temporaries (live range =
first def → last use), pins the read-only interpolator inputs and pixel
parameters and a vertex shader's vertex index in r0, keeps a temp live across a
hardware loop's back edge for the whole loop, and packs everything into the
minimum register count (e.g. `invtrig` 33→6, `mathfns` 20→6). Fewer registers a
thread let the GPU keep more threads in flight: packed, a pixel shader taking
four samples went from 12 registers to 6, and sampled 1.6-1.8 times as fast on
the console. A self-check asserts no two
simultaneously-live temps share a register. xsc numbers a temp for each value,
so its programs hold whole registers in their instructions' fields, encoded
once the allocator has packed them (`whole_sources`, `XSC_ABS_SOURCE`).
`tests/test_register_alloc.py` checks a vertex shader of 145 temps on
`xdis/simulate.py`, which runs a program's ALU, fetches and exports on the CPU.

## Fetch layout

Before allocation, the builder's `group_stage` (`xsdk:src/xsc_program.c`) lays out each block of
a pixel shader xsc lowers: the work its fetches' coordinates need as soon as
it's ready, then every fetch that's ready, again for fetches reading what
others fetched, then the rest in its own order. A block whose ALU work runs
into two fetches or more starts an exec wherever ALU and fetch runs meet, and
the allocator keeps what a run of fetches reads till the run ends, so no fetch
in it writes another's coordinates. Before, xsc emitted each sample's
coordinate work, then its fetch, and packing let the coordinates share one
register: four samples a texel apart ran 10.2 texels a clock on the console
that way, and 15.9 laid out (the texture units' 16; xsdk:docs/HARDWARE.md,
milestone 36). Shaders whose fetches read only their interpolators or pixel
parameters lower byte for byte as before, and so does a lone fetch after its
coordinate's work. `tests/test_fetch_layout.py` checks the layout, the
simulator's results and those shaders.

Within an exec the ALU and the fetches run apart on the console, so the
builder sets an instruction's serialize bit where it reads across them: an
ALU instruction reading or writing a temp a fetch in its exec wrote since the
exec's last serialized instruction, and a fetch reading one any instruction
wrote there (`xsdk:src/xsc_program.c`, exec_entry; xsdk:docs/HARDWARE.md,
"Serialized fetches"; a fetch after a fetch, see Vertex textures). Unserialized,
the ALU read a fetched index too soon. `tests/test_dependent_fetch.py` checks it.

## Loops (unrolling + mem2reg)

glslang (`-G --target-env opengl`) lowers `for`/`while` to mutable-memory form:
an `OpLoopMerge` header, loop-carried values held in Function-storage
`OpVariable`s mutated by `OpStore`, and an integer induction variable. xsc's
unroller (`xsdk:src/xsc_unroll.c`) runs as a pre-pass **whenever a module contains a loop** and rewrites
the whole function body into flat, branch-free SSA by symbolically executing the
structured CFG:

- **Trip count** is recovered from the induction variable's constant
  init / comparison / step and the body is replicated that many times. The
  induction variable is **constant-folded**, so `float(i)`, `i*k`,
  `float(i)*0.2`, etc. become literals (no integer ALU reaches the GPU).
- **mem2reg**: every Function-local variable is threaded through an environment,
  so loop-carried accumulation *and* post-loop read-modify-write (`c *= k;`)
  resolve to SSA values — the lowering's single-store-per-variable model never
  sees the multiple stores.
- **Nested** loops expand innermost-first; **`while`** (increment in the body)
  and **`for`** (increment in the continue block) are both handled.

If unrolling fails on the minimal uniform-bound accumulator pattern
`for(i=0;i<u_count;i++) sum += const;`, xsc routes to a tiny register-machine
lowerer instead: initialize a persistent GPR, wrap the body in
`begin_loop/end_loop`, and return a manifest entry `("loop", id, name)` so the
host uploads `integer_constants[8+id]`. See `loop_uniform.frag`; gallery G28 is
console-pending.

Because the pass is a no-op when there is no loop, loop-free shaders still lower
byte-for-byte as before. Unrolled shaders then go through the normal pipeline,
including the 16-GPR register allocator (e.g. `loop_stair` 17→3 GPRs). See
`loop_*` in `xsc/testdata/` and the `LOOP` gallery slot.

Data-dependent loops use real `LOOP_START/END`: `sum += const` (G28) and now
**`sum += w[i]`** — a uniform array indexed by the loop counter, lowering to the
aL-relative `c[base+aL]` read (the ISA primitive hardware-proven as G29). The
predicated `while`-break primitive is hardware-proven (G30); see below.

A data-dependent `while`-break (`if(cond) break;`) now lowers from GLSL to a
body `setp_*` + a predicated LOOP_END (the G30 primitive). An if/else **with a
constant condition** inside a loop body unrolls (the arm is picked per pass); one
on data runs both arms each pass and merges them (Control flow).

### Loops a block counts

A Vulkan shader has no bare uniforms, so the shape above never comes from one;
its loops count to an int in a push constant or uniform block. The unroller
(`xsdk:src/xsc_unroll.c`) keeps such a loop, `for (int i = 0; i < b.n; i++)`
(or `b.n > i`), stepping `i++` in its continue block, for a hardware loop: it
walks the pass once, each local and whole output the loop stores and held a
value of before it carried as a value at the top of the pass, and a value after
the loop. The lowering (`xsdk:src/xsc_lower_flow.c`) lays it out:

- each carried value's register from its value on entry, then LOOP_START;
- the pass worked out from the registers, what it reads from before the loop
  lowered once before LOOP_START; then each carried value's register takes its
  value at the pass's end, after all are worked out;
- `i` a float counting from 0, as xost holds integers, so `float(i)` and a
  block's array at `i` (`movas` and `c[a0 + base]`) work; the loop constant's
  aL goes unread;
- `if (c) break;` ending a pass a `setp_*` and a predicated LOOP_END, the break
  on a value every vertex or pixel shares, as LOOP_END breaks only when all 64
  agree (xenia:src/xenia/gpu/ucode.h:372-373); an if/else on data inside the
  pass is selects.

The manifest gives `("block_loop", loop_id, kind, set, binding, offset_bytes)`:
the runtime reads the int there for each draw and sets loop constant `loop_id`
to that many passes, from aL 0 by 1, none for 0 or less; the constant counts to
255 (xenia:src/xenia/gpu/xenos.h:1086-1097). On the console a pixel shader's
`loop i0` ran 0, 1 and 3 passes as loop constant 0 said (xsdk:docs/HARDWARE.md,
"Shader constants: partial writes hang the GPU, libxenon's whole blocks
don't"). A vertex shader's loops take loop constants from 0 and a pixel
shader's from 16, 16 each: both stages read all 32 (ucode.h:352), and libxenon
gives each stage half of the boolean constants the same way
(libxenon:drivers/xenos/xe.c:2038-2064). The optimiser leaves a
program with a loop alone, as before. Refused: a loop counted by a block inside
another loop or an if/else on data, a `continue`, a break anywhere but ending a
pass or on data, a store into part of an output in the loop, and a sample or
derivative at coordinates a pass works out.

## Control flow

The unroller (`xsdk:src/xsc_unroll.c`) takes an if/else on data in a vertex
shader, in a loop's body, or around a pixel shader's `gl_FragDepth`: it runs
both arms, each from the locals and outputs as they were before the branch, and
where the arms meet it gives each local and output they leave apart a select of
the condition and the two values, and each Phi there (glslang's `&&` and `||`)
the same. Outputs are kept as locals are and stored once at the end. The
lowering (`xsdk:src/xsc_lower_flow.c`) then picks, for each if/else:

- **Execs on the predicate,** in a pixel shader where every pixel takes the same
  arm, as its condition reads only constants (uniforms, push constants, literals), and
  an arm works anything out of its own beyond picking values or a bool: a
  `setp_*` on `a - b`, or on the bool's 0.0 or 1.0 not being 0, sets p0
  (xenia:src/xenia/gpu/ucode.h:1162-1204); the true arm's values go in `(p0)`
  execs and the false arm's in `(!p0)` execs, each into one register a value,
  every instruction in them predicated as its exec is, as Xenia says the
  compiler does (ucode.h:114-124); then the value is that register. The exec
  that sets p0 is marked PredicateClean=false. What the arms read from before
  the branch is lowered before the `setp`, an earlier if/else's arms with it.
  An exec on the predicate runs for none of a group of 64 that all fail it, so
  the arm not taken costs its execs' control flow and no instructions.
  A vertex shader keeps selects: its arms in execs on the predicate hung the GPU
  on the console (xsdk:docs/HARDWARE.md, "Branches, vulkan4xenon milestone
  21").
- **Selects** otherwise: each a `cnd*` on the comparison branched on, `a - b`
  worked out once a branch, as a pixel shader's if/else has lowered since v1; a
  condition that isn't a float comparison is its 0.0 or 1.0, which `cndgt`
  tests. On data, a predicated exec runs for every vertex or pixel of 64 if any
  of them needs it (ucode.h:114-124), so arms that differ within a group run both
  either way, and selects need no predicate. An if/else inside an arm in execs on
  the predicate is selects too, as there's one predicate.

A pixel shader's other if/else keeps the fragment lowering's selects, and every
shader without such a branch lowers byte for byte as before.

- **No jumps:** Xenia documents how an exec on the predicate treats a group of
  64 but not how a predicated jump does, and the execs a jump would skip cost
  their control flow alone.
- **No boolean constants:** `cexec b#` would need the runtime to work each
  condition out on the CPU at every draw; the `setp` works it out in the shader,
  and every vertex or pixel agrees all the same.
- **Exports stay out of control flow:** each is written once, unconditionally,
  after the arms meet, so every path writes it, which a pixel shader's depth
  needs: nothing else gives the pixel a depth
  (xenia:src/xenia/gpu/draw_util.cc:498-502). `gl_FragDepth` stored on one path
  alone is refused for that; an interpolator or colour stored on one path takes
  that value on the other, which GLSL leaves undefined.
- **Refused:** a `break`, `discard` or `return` in an arm of an if/else the
  unroller merges, a `switch`, and a matrix, array or struct an if/else
  chooses.
- **The optimiser** leaves a program with predicated instructions alone, as it
  did before (`can_optimize`).

`tests/test_vertex_branches.py` and `tests/test_loop_branches.py` check both
layouts and every arm on `xdis/simulate.py`, which follows a program's control
flow as Xenia's interpreter does for one vertex or pixel
(xenia:src/xenia/gpu/shader_interpreter.cc:26-258).

## Not yet / out of scope
- **Bitwise (`& | ^ ~`) and dynamic shifts** — the Xenos ALU has no bit
  operations; these raise a clear compile error rather than miscompiling.
  Constant-amount shifts could be done as mul/div by powers of two (not wired).
- **`matrixCompMult`, mat4 `inverse`** — `transpose`/`determinant`/`inverse`
  cover mat2/mat3 (numerically gated by `tests/test_matrix.py`); per-element
  multiply and 4×4 inverse (register-heavy) are not lowered.
- **Runtime integer division** is float-emulated and **approximate** (a small
  bias corrects exact multiples; non-negative domain).

`dFdx`/`dFdy`/`fwidth` are **hardware-verified** (the `getTextureGradients`
fetch is benign to the pipeline -- confirmed by the DSOLID isolation slot
rendering a clean cube). A raw derivative visualization looks faceted because
screen-space derivatives are constant per triangle and discontinuous at the
shared edge; that is correct, not a geometry bug.

## LOD and gradients

A texture fetch samples at the sum of the fetch constant's LOD bias, the register LOD if it reads it (UseRegisterLOD),
and its own LOD-bias field. Where it computes the LOD (UseComputedLOD), it works it out from its coordinate's gradients,
or from the registers' if it reads them (UseRegisterGradients), and adds the sum to it
(xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:1498-1536). `setTexLOD` sets the register LOD from one component,
and `setGradientH` and `setGradientV` the gradients from three; they write no temp (xenia:src/xenia/gpu/ucode.h:669-674,
spirv_shader_translator_fetch.cc:521-541).
- **An explicit LOD,** `textureLod`'s, turns the computed LOD off. A constant the bias field holds goes there; any other
  goes through `setTexLOD`, a constant copied into a temp first, as fetches read temps alone. Before, a constant LOD was
  added to the computed one, and one past the field's range didn't encode.
- **A bias,** `texture()`'s third argument, keeps the computed LOD and adds to it the same way.
- **Gradients,** `textureGrad`'s, go to `setGradientH` and `setGradientV`, x and y, the z they don't use x again; the
  fetch computes its LOD from them. A cube map's gradients are refused: Xenia takes them in cube space
  (ucode.h:580-583), unchecked on the console.
- **Ordering:** a fetch keeps its place against every state fetch (`xsdk:src/xsc_program.c`, ordered), so each sample
  reads the state set for it; and a state fetch counts as writing no temp, so the allocator never takes its unused
  destination, which keeps every component (DEST_KEEP), for one.

Offsets, a sample index and a minimum LOD, the image operands xsc doesn't take, are refused; they were dropped before.
`tests/test_lod_gradients.py` checks each on `xdis/simulate.py`, which keeps the state the setters write. None of it is
checked on the console yet (vulkan4xenon:docs/ROADMAP.md, milestone 22).

## Facing

The pixel parameters' x holds the pixel's column, its sign set on a back face, as Xenia reads `vFace` and
`gl_FrontFacing` from it (xenia:src/xenia/gpu/registers.h:224-227) and XenosRecomp makes it
(XenosRecomp:XenosRecomp/shader_recompiler.cpp:1465-1467). Column 0's x is a zero, -0.0 on a back face, so xsc takes
`rcpc` of it, which keeps the sign as -FLT_MAX, then `sge` of that and its negation: `gl_FrontFacing` is 1.0 on a front
face and 0.0 on a back one, in every component, and turns the pixel parameters on as `gl_FragCoord` does, both from the
one register. Before, xsc took the variable for an interpolator at r0.

So that `gl_FrontFacing` works where GLSL takes a bool, a pixel shader's if/else or discard whose condition is a bool
value rather than a comparison, `gl_FrontFacing` or a logical operation on comparisons, lowers on it as a value b: the
if/else to `cndgt b, true arm, false arm`, a discard where b holds to `kill_gt b, -b`, and where it doesn't to
`kill_eq b, -b`. `tests/test_front_facing.py` checks a `?:`, an if/else beside `gl_FragCoord`, and a discard on the
simulator, at columns 3 and 0 of each face. The console has to settle which face the sign marks: Xenia gives it the
host's front face, set from PA_SU_SC_MODE_CNTL's face as culling is
(xenia:src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc:625).

## Vertex textures

A vertex shader samples as a pixel shader does, at the LOD it gives. glslang gives `texture()` there an explicit LOD
of 0, and a sample without a LOD or gradients is refused, as a vertex shader has no quad to work one out over
(xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:639-641). Its samples go after the vertex fetches and before the
position's alloc, as a pixel shader's go before its colours'. Each fetches for every vertex, FetchValidOnly off, as
that bit is about a quad's pixels (xenia:src/xenia/gpu/ucode.h:558-561), and the container declares the samplers as a
pixel shader's does.
- Samplers take fetch constants from 0 in each stage, so a host using both moves one stage's: xsdk's
  `xsdk_shader_move_textures` adds a base to each texture fetch's constant.
- Two precautions, neither checked on the console: an exec's vertex cache mask ("A fifth vertex fetch") covers its
  vertex fetches, which lead it, and none of its texture fetches; and a fetch reading a temp another fetch wrote in its
  exec is serialized, as a texture fetch at an attribute's coordinate is. No shader in Dusklight's pack, xsc's testdata
  or vulkan4xenon's bundles lowers differently for either.

`tests/test_vertex_textures.py` checks the fields, the mask and the serialize bit, and the simulator's height and
colour.

## Validation
Every test shader is checked by the XenosRecomp oracle (which does the
inverse ucode→SPIR-V translation); `modulate.frag` is additionally
byte-identical to the hardware-photographed combiner. See
`tests/test_xsc_lower.py`, `tests/test_xsc_resolve.py`. The end-to-end proof
app was `app/glslcube` (cube whose VS+PS compile from GLSL at build time), in
xost's archived repo (github.com/xenosworks/xenos-toolchain, last commit 4a785c6); the SDK's titles and vulkan4xenon's run xsc's shaders on the
console now.

## Still to do

Measured on the console through the SDK: two kinds of code xsc writes run
slower than they need to. A scalar chain lowers as `.xxxx` vector ops rather
than scalar ops co-issued beside vector ones, and a vec4 plus a float lowers
as four adds, a component each.
