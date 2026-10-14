# GLSL math/transcendental builtin → Xenos microcode lowering

How `xsc` (and any GLSL→Xenos lowerer) maps the GLSL.std.450 math and
transcendental builtins onto the Xenos (Xbox 360, R400/Adreno-2xx-class) ALU.

Source of truth for every opcode name/value/semantic below is
`xdis/xenos_ucode.py` (`AluScalarOpcode` ~line 180, `AluVectorOpcode` ~line 255,
the `kAluScalarOpcodeInfos`/`kAluVectorOpcodeInfos` tables ~line 935), which is a
faithful transcription of Xenia's `src/xenia/gpu/ucode.h`. The per-opcode pseudo
math quoted here is from the ucode.h comments (mirrored in
`reference/xenia/src/xenia/gpu/ucode.h`).

This is a derivation/reference document — no code is changed by it. Cross-ref
`docs/xsc.md` for `mix`/`max`/`min`/`clamp` which are already handled by the
shipping lowerer.

---

## The Xenos ALU model (what "1 instruction" means here)

Each ALU instruction slot is a **co-issued pair**: one **vector** op
(`AluVectorOpcode`, 5-bit field, component-wise over xyzw) **plus** one
**scalar** op (`AluScalarOpcode`, 6-bit field, single value). They share the
three source-read ports. So in the best case a vector op and a scalar op run in
the *same* slot for free.

- **Vector ops** are per-component: `dest.x = f(src0.x, src1.x, ...)`, etc.
- **Scalar ops** read one or two components of one source and produce **one
  value broadcast to `dest.xyzw`** (every per-op comment is literally
  `dest.xyzw = ...`). All the transcendentals (rcp/rsq/sqrt/exp/log/sin/cos) are
  **scalar-only** — there is no vector reciprocal/sqrt/exp/log. To apply one to a
  vector you compute the scalar then `mul` by it (broadcast), as in `normalize`.
- The scalar result is also retained in the **`ps` previous-scalar register**,
  which `*_prev` ops (`kAddsPrev`, `kSubsPrev`, `kMulsPrev`...) can consume in the
  next slot — useful for chaining `log → mul → exp` style sequences.

**Free source modifiers** (no extra instruction; encoded in the ALU word):
- **negate** per source (`src_negate`, dword_1 bits 24-26) → `-x`.
- **absolute value** per source (`abs_constants` for constants; the temp-reg
  `_abs` modifier seen as `r0_abs.z` in the `cube` sequence in ucode.h) → `|x|`.
- **swizzle** per source, and a per-component **write mask** + a free
  **saturate/clamp** (`vector_clamp`/`scalar_clamp`, the `_sat` suffix) on the
  destination. `clamp(x,0,1)` is therefore free (see `docs/xsc.md`).

These modifiers are why `abs()` and the constant-multiply in `exp`/`log` often
fold into a neighboring op and cost **0** extra instructions.

---

## Summary table

Instruction count = number of ALU slots assuming the transcendental scalar can
co-issue with the vector op it depends on **only when there is no data
dependency**; counts below are the dependency-honest worst case (the common
case). "broadcast" = scalar result replicated to xyzw.

| GLSL builtin | instr | Xenos ops (vector / scalar) | notes |
|---|---|---|---|
| `inversesqrt(x)` | 1 | `kRsq` | direct; IEEE variant. |
| `sqrt(x)` | 1 | `kSqrt` | direct hardware op (`SQRT_IEEE`) — exists, use it. |
| `length(v)` | 2 | `kDp3`/`kDp4` → `kSqrt` | dot then sqrt; scalar sqrt reads the dp result. |
| `distance(a,b)` | 3 | `kAdd`(−) → `kDp3`/`kDp4` → `kSqrt` | = `length(a−b)`; sub is free-negate add. |
| `normalize(v)` | 3 | `kDp3`/`kDp4`, `kRsq`, `kMul`(broadcast) | mul by broadcast rsq. |
| `dot(a,b)` | 1 | `kDp4` (or `kDp3`/`kDp2Add`) | result broadcast xyzw. Already in xsc. |
| `pow(x,y)` | 3 | `kLog`, `kMuls`/`kMul`, `kExp` | `exp2(y·log2(x))`; domain x>0. |
| `exp(x)` | 2 | `kMuls`(·1.4427) → `kExp` | `exp2(x·log2 e)`; const folds → effectively 1 + setup. |
| `log(x)` | 2 | `kLog` → `kMuls`(·0.6931) | `log2(x)·ln2`. |
| `exp2(x)` | 1 | `kExp` | direct (note x==0 → exactly 1.0). |
| `log2(x)` | 1 | `kLog` | direct IEEE; use `kLog` not `kLogc`. |
| `floor(x)` | 1 | `kFloor` (vec) / `kFloors` (scalar) | direct. |
| `fract(x)` | 1 | `kFrc` (vec) / `kFrcs` (scalar) | direct, `x − floor(x)`. |
| `trunc(x)` | 1 | `kTrunc` (vec) / `kTruncs` (scalar) | direct. |
| `abs(x)` | 0 | source `_abs` modifier | free; folds into the consumer op. |
| `sign(x)` | 2–3 | `kSgt` + `kSgt`(−) + `kAdd`(−) | no direct op; see derivation. |
| `min(x,y)` | 1 | `kMin` (vec) / `kMins` (scalar) | direct. Already in xsc. |
| `max(x,y)` | 1 | `kMax` (vec) / `kMaxs` (scalar) | direct. Already in xsc. |
| `reflect(i,n)` | 3 | `kDp3`/`kDp4`, `kMul`, `kMad` | `i − 2·dot(n,i)·n`. |
| `mix`/`clamp` | — | sub+mad / `_sat` | already handled, see `docs/xsc.md`. |
| `sin(x)` | 1* | `kSin` | direct, **but range-reduction caveat** — see flag. |
| `cos(x)` | 1* | `kCos` | direct, same caveat. |

`*` `sin`/`cos` are single ops but may need a host/lowerer range-reduction
multiply for large arguments; see the final flag.

---

## The available Xenos ops (verbatim from the enums)

### Scalar transcendentals/reciprocals (`AluScalarOpcode`)
Reciprocal family — **three precision variants each**:

| op | value | semantic (ucode.h) |
|---|---|---|
| `kRcpc` | 17 | recip, **clamp to ±FLT_MAX** on ±INF |
| `kRcpf` | 18 | recip, **clamp to ±0.0** on ±INF (fixed-function mimic) |
| `kRcp`  | 19 | recip, **IEEE** (`1.0/src`, no clamp) |
| `kRsqc` | 20 | 1/sqrt, clamp to ±FLT_MAX on ±INF |
| `kRsqf` | 21 | 1/sqrt, clamp to ±0.0 on ±INF |
| `kRsq`  | 22 | 1/sqrt, **IEEE** |
| `kExp`  | 14 | base-2 exp (`EXP_IEEE`); `src==0 → 1.0` |
| `kLogc` | 15 | base-2 log, **clamped**: `−INF → −FLT_MAX` |
| `kLog`  | 16 | base-2 log, **IEEE** (`LOG_IEEE`) |
| `kSqrt` | 40 | `sqrt(src)` directly (`SQRT_IEEE`) |
| `kSin`  | 48 | `sin(src)` |
| `kCos`  | 49 | `cos(src)` |

Other scalar ops used below: `kAdds`=0, `kMuls`=2, `kMaxs`=5, `kMins`=6,
`kFrcs`=11, `kTruncs`=12, `kFloors`=13, `kSubs`=25, plus the
constant-multiply forms `kMulsc0/1`=42/43, `kAddsc0/1`=44/45, `kSubsc0/1`=46/47
(these take an inline constant register operand).

**The `c`/`f`/plain distinction.** `c` = clamp-to-max (`±FLT_MAX`), `f` =
fixed-function flush (`±0.0`), plain = IEEE (returns ±INF). Note all three
reciprocal/rsq variants special-case `src==1.0 → 1.0` exactly. **For general
GLSL semantics use the plain IEEE variant** (`kRcp`, `kRsq`, `kLog`): GLSL
`inversesqrt`/`pow`/`log` are specified against IEEE, and glslang/host code does
not expect the FLT_MAX or 0.0 clamping. Reach for `kRsqc`/`kLogc` only when you
deliberately want the old D3D "no-INF" behavior (e.g. feeding a value straight
into another transcendental where an INF would poison the result).

### Vector ops (`AluVectorOpcode`) used below
`kAdd`=0, `kMul`=1, `kMax`=2, `kMin`=3, `kSgt`=5 (set-if-greater→1.0/0.0),
`kFrc`=8, `kTrunc`=9, `kFloor`=10, `kMad`=11 (`src0*src1+src2`, documented as a
true FMA — single round after the add), `kDp4`=15, `kDp3`=16, `kDp2Add`=17
(`src0.x*src1.x + src0.y*src1.y + src2.x`), `kMaxA`=29 (max + a0 export).
**Dot products write the scalar sum to all of `dest.xyzw`** (broadcast), which is
exactly what the sqrt/rsq that follows wants to read.

---

## Per-builtin derivation

### `inversesqrt(x)` → `kRsq`  — 1 instruction
Direct. `rsq dest, x.x` → `dest.xyzw = 1/sqrt(x)`. Scalar, broadcast.
Use `kRsq` (IEEE). Domain: `x>0` for a finite result; `x==0 → +INF` under IEEE
(use `kRsqc` if you must clamp that to FLT_MAX). `x<0` → NaN.

### `sqrt(x)` → `kSqrt`  — 1 instruction
A **direct `kSqrt` (value 40, `SQRT_IEEE`) exists** — confirmed in the enum and
ucode.h (`dest.xyzw = sqrt(src0.a)`). Prefer it: one op, correct at `x==0`
(`sqrt(0)=0`, whereas the rsq-based forms divide by zero there).
Alternatives if you ever lack `kSqrt`:
- `x * rsq(x)` — 2 ops (`kRsq` then `kMuls`/`kMul`), but **wrong at x=0**
  (`0 * +INF = NaN`); needs a guard.
- `1.0 / rsq(x)` — 2 ops (`kRsq` then `kRcp`), also blows up at 0.
Hardware favors the direct `kSqrt`; only the rsq path is needed when you *also*
need `rsq` anyway and want to share it. Scalar, broadcast. Domain `x≥0`.

### `length(v)` → `sqrt(dot(v,v))`  — 2 instructions
1. `kDp3 t, v, v` (vec3) or `kDp4 t, v, v` (vec4) → `t.xyzw = Σ vᵢ²` (broadcast).
2. `kSqrt len, t.x` → `len.xyzw = sqrt(t)`.
The dp broadcasts its scalar sum, so the scalar `kSqrt` reads `t.x` directly. For
a 2-component length use `kDp2Add` with src2=0, or `kDp3` with the z component
swizzled/masked to 0. No domain issue (`dot(v,v) ≥ 0`).

### `distance(a,b)` → `length(a-b)`  — 3 instructions
1. `kAdd d, a, -b` (the subtract is a free source-negate on `b`; equivalently
   `kSub`). → `d = a − b`.
2. `kDp3 t, d, d` (or `kDp4`).
3. `kSqrt dist, t.x`.

### `normalize(v)` → `v * rsq(dot(v,v))`  — 3 instructions
1. `kDp3 t, v, v` (or `kDp4`) → `t.xyzw = |v|²` broadcast.
2. `kRsq s, t.x` → `s.xyzw = 1/|v|` broadcast (scalar op).
3. `kMul n, v, s` → per-component `v * (1/|v|)` (the broadcast `s` multiplies each
   component). Exact op triple: **`dp3`, `rsq`, `mul`**.
Caveat: zero-length `v` gives `rsq(0)=+INF` then `0*INF=NaN`. If robustness
matters, use `kRsqc` (clamps to FLT_MAX) or guard. The vector `kMul` here can
co-issue with nothing useful (it depends on `s`), so this is genuinely 3 slots.

### `dot(a,b)` → `kDp4` / `kDp3` / `kDp2Add`  — 1 instruction
Already implemented (see `docs/xsc.md`: "OpDot → dp3/dp4, result broadcast to all
components"). Listed for completeness. `kDp2Add` covers the 2-component case and
can fold an add (`+src2.x`) for free — handy as the inner step of `length` of a
vec2, or a `dot2 + bias`.

### `pow(x,y)` → `exp2(y * log2(x))`  — 3 instructions
1. `kLog l, x.x` → `l = log2(x)` (use IEEE `kLog`).
2. `kMuls m, l, y` (scalar) or `kMul` if vector → `m = y * log2(x)`.
3. `kExp p, m.x` → `p = 2^m = x^y`.
**Domain: x > 0 strictly.** `log2` of `x≤0` is −INF/NaN, matching GLSL's
undefined `pow` for negative base. If `x` and `y` are vectors, do this
per-component but note the transcendentals are scalar — you'd need one
`log`/`exp` pair per active component (so `pow` on a vec3 base is up to
3×(log,exp)+mul). For the common scalar-exponent case it's the 3 ops above.

### `exp(x)` → `exp2(x * 1.4426950408)`  — 2 instructions (1 + const)
`exp(x) = 2^(x·log2 e)`, `log2 e = 1.44269504088896…`.
1. `kMuls m, x, c` where `c = 1.4426950408` — supply via `kMulsc0/1` (inline
   constant operand) or a pooled constant register (see `docs/xsc.md` literal
   pool). This multiply can co-issue with an unrelated vector op.
2. `kExp e, m.x` → `e = 2^m = e^x`.
If the scale is materialized as a constant-multiply (`mulsc`), the slot cost can
collapse toward 1 effective op when paired with the surrounding work.

### `log(x)` → `log2(x) * 0.6931471805`  — 2 instructions
`ln(x) = log2(x)·ln2`, `ln2 = 0.69314718055994…`.
1. `kLog l, x.x` → `l = log2(x)` (IEEE).
2. `kMuls r, l, c` with `c = 0.6931471805`. Same constant-supply options as
   `exp`. Domain `x>0`. For `log2`/`log10` just change the constant
   (`log10 = log2(x)·0.30103…`).

### `exp2(x)` → `kExp`,  `log2(x)` → `kLog`  — 1 instruction each
Direct. `kExp` (`EXP_IEEE`, note `exp2(0)` is forced to exactly `1.0`),
`kLog` (`LOG_IEEE`). Prefer the plain IEEE `kLog` over `kLogc` for GLSL — `kLogc`
clamps `−INF → −FLT_MAX`, which is not GLSL `log2` semantics. Both scalar,
broadcast, domain `x>0` for `log2`.

### `floor(x)` → `kFloor`/`kFloors`,  `fract(x)` → `kFrc`/`kFrcs`,  `trunc(x)` → `kTrunc`/`kTruncs`  — 1 instruction each
All three exist as **both** a component-wise **vector** op and a **scalar** op:
- `floor`: vector `kFloor` (10) / scalar `kFloors` (13) → `floor(x)`.
- `fract`: vector `kFrc` (8) / scalar `kFrcs` (11) → `x − floor(x)` (exactly GLSL
  `fract`).
- `trunc`: vector `kTrunc` (9) / scalar `kTruncs` (12) →
  `x≥0 ? floor(x) : −floor(−x)` (round toward zero, exactly GLSL `trunc`).
Use the **vector** form for vec2/3/4 operands (one op, all components), the
scalar form for a single channel when you want to co-issue a vector op alongside.
`round`/`roundEven`/`ceil` have **no** direct op — derive: `ceil(x) =
-floor(-x)` (free negate + `kFloor` + negate on the consumer, ~1 op);
`round(x) = floor(x + 0.5)` (`kAdd`/`mad` then `kFloor`, ~2 ops).

### `abs(x)` → source `_abs` modifier  — 0 instructions
There is no `abs` opcode and none is needed: every ALU source has a free
absolute-value modifier (`abs_constants` for constant sources; the temp-register
`_abs` form, e.g. `r0_abs.z` in ucode.h's `cube` example). `abs(x)` folds into
whatever op consumes it, costing nothing. If `abs(x)` is a standalone assignment
with no consumer, emit a `kMax x, -x` or a `mov` (`kAdd x,0` / `kMuls x,1`) with
the `_abs` source — 1 op only in that degenerate standalone case.

### `sign(x)` → comparisons  — 2–3 instructions (no direct op)
No `sign` opcode. `sign(x)` is `(x>0) - (x<0)` ∈ {−1,0,+1}. Derive with the
set-if-greater ops (`kSgt`/`kSgts`, which yield `1.0`/`0.0`):
1. `kSgt p, x, 0` → `p = (x > 0) ? 1 : 0`.
2. `kSgt n, 0, x` → `n = (x < 0) ? 1 : 0` (compare 0 > x; or `kSgt n, -x, 0`).
3. `kAdd s, p, -n` → `s = p − n` ∈ {−1, 0, +1}.
3 vector ops (the two `kSgt` can each co-issue with an unrelated scalar, but they
are mutually independent so still 2 slots + the add = 3). A cheaper 2-op
approximation when you don't need exact 0-at-0: `kSgt` + `kMad` to map {0,1}→
{−1,1}. For a guaranteed-nonzero input, `sign(x) = x * rsq(x*x)` style tricks
exist but cost more and mishandle 0; prefer the comparison form.

### `reflect(i, n)` = `i - 2*dot(n,i)*n`  — 3 instructions
(`n` assumed normalized, as GLSL requires.)
1. `kDp3 d, n, i` (or `kDp4`) → `d.xyzw = dot(n,i)` broadcast.
2. `kMul k, d, c2` where `c2 = (2,2,2,2)` constant → `k = 2·dot(n,i)` (or fold the
   2 into step 3's first multiply by pre-scaling `n`; or use the free
   source-negate to avoid a separate sub).
3. `kMad r, -k, n, i` → `r = i − k·n = i − 2·dot(n,i)·n`. The `mad`'s free negate
   on its first source supplies the minus. Exact triple: **`dp3`, `mul`, `mad`**.
   If you have a spare constant you can pre-bake `2` into the dot's source scale
   and drop to **2 ops** (`dp3` then `mad r, -d2, n, i` with `d2 = 2·dot`).
`refract` is more involved (needs a sqrt and a clamp/compare for total internal
reflection) — derivable from the same primitives but out of scope here.

### `mix` / `clamp` — already handled
See `docs/xsc.md`: `mix(a,b,t)` → `kAdd`(b−a) + `kMad` (`(b−a)*t + a`); `clamp` to
`[0,1]` → the free `_sat` destination modifier; general `clamp(x,lo,hi)` →
`kMax` then `kMin`. Not re-derived here.

---

## Flags — builtins WITHOUT a clean Xenos lowering

**`sin(x)` / `cos(x)` — single op (`kSin`=48 / `kCos`=49) BUT argument-range
limited.** The hardware `SIN`/`COS` are accurate only over a reduced argument
range (roughly `[-π, π]`, like every GPU's polynomial-backed sin/cos — including
the R400/Adreno-2xx these descend from). For arbitrary GLSL `sin(x)` you must
**range-reduce first**: `xr = (x·(1/2π)) ; xr = frac(xr+0.5)-0.5 ; xr = xr·2π`
(or the equivalent `frac`-based reduction), i.e. ~2–3 extra ALU ops
(`kMuls`/`kFrcs`/`kMad`) **in front of** the single `kSin`/`kCos`. So treat
sin/cos as "1 hardware op + a range-reduction prologue", not a clean 1:1. If a
caller guarantees the argument is already in range (common for animated phase
values authored in `[0,1]·2π`), the bare `kSin`/`kCos` is exact-enough and the
prologue can be dropped. **Recommend deferring full general-range sin/cos** until
the range-reduction helper is in the lowerer; flag any shader using `sin`/`cos`
on an unbounded argument.

**`tan(x)`** — no op. Lower as `sin(x) * rcp(cos(x))` = (sin prologue) + `kSin` +
`kCos` + `kRcp` + `kMul`, and inherits the sin/cos range caveat. Defer with
sin/cos.

**`asin`/`acos`/`atan`/`atan2`** — **no hardware op and no clean identity.**
These need a polynomial/rational approximation (typically a 5–7 term minimax
poly evaluated with `kMad` chains, plus quadrant/sign fixup for `atan2`). Real
cost is ~6–12 ALU ops each. **Defer to a hand-written approximation library**;
there is no 1–3 op lowering.

**`sinh`/`cosh`/`tanh`** — no op; build from `exp` (`kExp` + the `log2 e` scale):
e.g. `tanh(x) = (e^{2x}-1)/(e^{2x}+1)`. Several ops, but composable from the clean
`exp` lowering above — not a blocker, just multi-op.

**`round`/`roundEven`/`ceil`** — no direct op but trivially derived from
`kFloor` (see the floor section): `ceil = -floor(-x)`, `round = floor(x+0.5)`.
Not a concern.

Everything else in the table (`inversesqrt`, `sqrt`, `length`, `distance`,
`normalize`, `dot`, `pow`, `exp`, `log`, `exp2`, `log2`, `floor`, `fract`,
`trunc`, `abs`, `sign`, `min`, `max`, `reflect`) has a clean ≤3-op lowering with
direct hardware support and no approximation required.
