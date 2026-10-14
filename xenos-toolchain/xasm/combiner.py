#!/usr/bin/env python3
"""N64-style color-combiner -> Xenos pixel-shader generator.

The emulator-enabling artifact: compiles parametric RDP combiner state

    out = (A - B) * C + D     (RGB and Alpha pipelines, independently)

in 1-cycle or 2-cycle form directly to Xenos microcode containers via
xasm.program.PixelProgram — the role GLideN64-class plugins fill by
generating GLSL, with no shader compiler involved at any point.

Register/constant contract (pairs with xost_vs2's interpolator layout):
    r0 = TEXCOORD0          c0 = PRIM   (app-set, Xe_SetPixelShaderConstantF)
    r1 = SHADE              c1 = ENV
    r2 = TEXEL0 (tfetch0)   c2 = (1,1,1,1) for ONE
    r3 = scratch
    r4 = COMBINED (cycle-0 result, 2-cycle modes)
    r5 = TEXEL1 (tfetch1)
Constant indices are fixed regardless of which are referenced, so a host
can set all three unconditionally. TEXEL0/TEXEL1 sample fetch constants
0/1 (Xe_SetTexture slots 0/1) at the TEXCOORD0 interpolator.

The generator folds algebra so trivial modes cost one instruction:
A==B or C==0 -> D; B==0 drops the subtract; A==0 folds to a negated
operand; C==1 drops the multiply; D==0 fuses to mul; the general case is
a single mad. Encoder constraint honored: at most ONE distinct constant
register per ALU instruction (the ucode const_0/const_1 encoding is
shared across operands; we stay in the simple regime and split through
scratch when a step would need two).
"""
from __future__ import annotations

import sys
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

import xdis.container_emit as ce  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xasm.program import PixelProgram, swizzle4, mask4  # noqa: E402

V = xu.AluVectorOpcode
_SCRATCH = 3
_COMBINED_REG = 4
_TEXEL1_REG = 5


class Src(Enum):
    ZERO = "zero"
    ONE = "one"
    TEXEL0 = "texel0"
    TEXEL1 = "texel1"
    SHADE = "shade"
    PRIM = "prim"
    ENV = "env"
    COMBINED = "combined"  # cycle-0 result; only valid in cycle 1


_TEMPS = {Src.TEXEL0: 2, Src.SHADE: 1, Src.COMBINED: _COMBINED_REG,
          Src.TEXEL1: _TEXEL1_REG}
_CONSTS = {Src.PRIM: 0, Src.ENV: 1, Src.ONE: 2}


@dataclass(frozen=True)
class Op:
    """A resolved ALU operand."""
    reg: int
    is_temp: bool
    negate: bool = False

    @staticmethod
    def of(src: Src) -> "Op":
        if src in _TEMPS:
            return Op(_TEMPS[src], True)
        return Op(_CONSTS[src], False)

    def negated(self) -> "Op":
        return Op(self.reg, self.is_temp, not self.negate)

    @property
    def is_const(self) -> bool:
        return not self.is_temp


SCRATCH_OP = Op(_SCRATCH, True)


@dataclass
class CombinerCycle:
    """(A-B)*C+D selects; alpha selects default to the color selects."""
    a: Src
    b: Src
    c: Src
    d: Src
    aa: Src | None = None
    ab: Src | None = None
    ac: Src | None = None
    ad: Src | None = None

    def color_sel(self):
        return (self.a, self.b, self.c, self.d)

    def alpha_sel(self):
        return (self.aa if self.aa is not None else self.a,
                self.ab if self.ab is not None else self.b,
                self.ac if self.ac is not None else self.c,
                self.ad if self.ad is not None else self.d)

    def sources(self):
        return set(self.color_sel()) | set(self.alpha_sel())


class Generator:
    """Stateless once constructed; reuse across many combiner words."""

    def __init__(self, alu_template: dict, tfetch_template: dict):
        """Templates are field dicts as xasm.encode's builders take them:
        xsc/templates.py's mov() and texture_fetch()."""
        self._base = dict(alu_template, is_export=False)
        self._tfetch = tfetch_template

    # ---- low-level emission ----

    def _emit(self, p: PixelProgram, opcode, dest: int, export: bool,
              wmask: int, ops: dict, alpha: bool):
        n_consts = len({o.reg for o in ops.values() if o.is_const})
        assert n_consts <= 1, "split before emitting: >1 const register"
        f = dict(self._base, vector_opcode=opcode, vector_dest=dest,
                 is_export=export, vector_write_mask=wmask)
        swiz = swizzle4("wwww") if alpha else 0
        for n in (1, 2, 3):
            o = ops.get(n)
            if o is None:
                o = Op(0, True)  # unused slot: benign r0 reference
            f[f"src{n}_reg"] = o.reg
            f[f"src{n}_is_temp"] = o.is_temp
            f[f"src{n}_negate"] = o.negate
            f[f"src{n}_swizzle"] = swiz if ops.get(n) else 0
        p.alu(**f)

    def _mov_to_scratch(self, p, op: Op, alpha: bool):
        self._emit(p, V.kMax, _SCRATCH, False, 0xF, {1: op, 2: op}, alpha)
        return SCRATCH_OP

    # ---- one (A-B)*C+D pipeline ----

    def _pipeline(self, p: PixelProgram, sel, alpha: bool, wmask: int,
                  out_dest: int, out_export: bool):
        a, b, c, d = sel

        def final(opcode, ops):
            self._emit(p, opcode, out_dest, out_export, wmask, ops, alpha)

        # difference term
        if a == b:
            diff = None                      # (A-B) == 0
        elif b == Src.ZERO:
            diff = None if a == Src.ZERO else Op.of(a)
        elif a == Src.ZERO:
            diff = Op.of(b).negated()        # -(B)
        else:
            oa, ob = Op.of(a), Op.of(b).negated()
            if oa.is_const and ob.is_const and oa.reg != ob.reg:
                oa = self._mov_to_scratch(p, oa, alpha)
            self._emit(p, V.kAdd, _SCRATCH, False, 0xF,
                       {1: oa, 2: ob}, alpha)
            diff = SCRATCH_OP

        # product & sum
        if diff is None or c == Src.ZERO:
            if d == Src.ZERO:
                # true zero with no constant: SHADE - SHADE
                z = Op.of(Src.SHADE)
                final(V.kAdd, {1: z, 2: z.negated()})
            else:
                od = Op.of(d)
                final(V.kMax, {1: od, 2: od})
            return

        if c == Src.ONE:
            if d == Src.ZERO:
                final(V.kMax, {1: diff, 2: diff})
            else:
                oc, od = diff, Op.of(d)
                if oc.is_const and od.is_const and oc.reg != od.reg:
                    oc = self._mov_to_scratch(p, oc, alpha)
                final(V.kAdd, {1: oc, 2: od})
            return

        ops = {1: diff, 2: Op.of(c)}
        if d != Src.ZERO:
            ops[3] = Op.of(d)
        n_consts = len({o.reg for o in ops.values() if o.is_const})
        if n_consts > 1:
            # split: scratch = diff * C, then out = scratch + D
            o1, o2 = ops[1], ops[2]
            if o1.is_const and o2.is_const:
                o1 = self._mov_to_scratch(p, o1, alpha)
            self._emit(p, V.kMul, _SCRATCH, False, 0xF,
                       {1: o1, 2: o2}, alpha)
            if 3 in ops:
                final(V.kAdd, {1: SCRATCH_OP, 2: ops[3]})
            else:
                final(V.kMax, {1: SCRATCH_OP, 2: SCRATCH_OP})
        else:
            opcode = V.kMad if 3 in ops else V.kMul
            final(opcode, ops)

    def _emit_cycle(self, p: PixelProgram, cycle: CombinerCycle,
                    out_dest: int, out_export: bool):
        color, alpha = cycle.color_sel(), cycle.alpha_sel()
        if color == alpha:
            self._pipeline(p, color, alpha=False, wmask=0xF,
                           out_dest=out_dest, out_export=out_export)
        else:
            self._pipeline(p, color, alpha=False, wmask=mask4("xyz"),
                           out_dest=out_dest, out_export=out_export)
            self._pipeline(p, alpha, alpha=True, wmask=mask4("w"),
                           out_dest=out_dest, out_export=out_export)

    # ---- whole shader ----

    def generate(self, cycle0: CombinerCycle,
                 cycle1: CombinerCycle | None = None) -> bytes:
        """1-cycle (cycle0 exports) or 2-cycle (cycle0 -> COMBINED/r4,
        cycle1 exports) combiner -> pixel-shader container bytes."""
        used = set(cycle0.sources())
        if cycle1 is not None:
            used |= cycle1.sources()
        assert Src.COMBINED not in cycle0.sources(), \
            "COMBINED is only meaningful in cycle 1"

        p = PixelProgram()
        p.input(0, ce.USAGE_TEXCOORD)
        p.input(1, ce.USAGE_COLOR)
        if Src.TEXEL0 in used:
            p.tfetch(self._tfetch, dest=2, src=0)
        if Src.TEXEL1 in used:
            p.tfetch(dict(self._tfetch, fetch_constant_index=1),
                     dest=_TEXEL1_REG, src=0)
        p.alloc_colors()

        if cycle1 is None:
            self._emit_cycle(p, cycle0, out_dest=0, out_export=True)
        else:
            self._emit_cycle(p, cycle0, out_dest=_COMBINED_REG,
                             out_export=False)
            self._emit_cycle(p, cycle1, out_dest=0, out_export=True)

        constants = []
        if Src.TEXEL0 in used:
            constants.append(ce.Constant("tex0", ce.RS_SAMPLER, 0, 1,
                                         ce.CLASS_OBJECT, 0xC,
                                         rows=1, columns=1))
        if Src.TEXEL1 in used:
            constants.append(ce.Constant("tex1", ce.RS_SAMPLER, 1, 1,
                                         ce.CLASS_OBJECT, 0xC,
                                         rows=1, columns=1))
        for s, name in ((Src.PRIM, "prim"), (Src.ENV, "env"),
                        (Src.ONE, "one")):
            if s in used:
                constants.append(ce.Constant(
                    name, ce.RS_FLOAT4, _CONSTS[s], 1,
                    ce.CLASS_VECTOR, ce.TYPE_FLOAT, rows=1, columns=4))
        return p.finish(constants=constants)
