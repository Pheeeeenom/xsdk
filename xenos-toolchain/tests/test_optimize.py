#!/usr/bin/env python3
"""xasm/optimize.py's rewrites, case by case, on hand-built pixel programs.

Each case builds a program twice, without and with the pass, and checks:
  - the instructions the pass gave compute the same exports as those it was given, run on random inputs and constants
    by xdis/equivalence.py, and so does the allocated microcode;
  - the pass left the number of ALU instructions the case expects: what it may remove it does, and what it mustn't
    touch (a move other values share a register with, a source changed in between, a third constant) it keeps.
A last case hands the comparison a wrong rewrite, to show it would see one.

tools/check_optimize.py runs the same comparison over a folder of real shaders.
"""
import math
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xasm.program as program  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.container_emit as ce  # noqa: E402
import xdis.equivalence as equivalence  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xasm.program import ABS_SOURCE, PixelProgram, XSDK_REGISTERS  # noqa: E402
from xsc.lower import _templates  # noqa: E402

V = xu.AluVectorOpcode
S = xu.AluScalarOpcode
T = _templates()
INPUTS = 2                                   # r0 and r1 are interpolators; temps start at r2


def r(reg, swizzle="xyzw", negate=False, absolute=False):
    return ("r", reg | (ABS_SOURCE if absolute else 0), swizzle, negate)


def c(reg, swizzle="xyzw", negate=False):
    return ("c", reg, swizzle, negate)


def alu(vop, dest, mask, s1, s2=None, s3=None, sop=S.kRetainPrev, sdest=0, smask=0, export=False, clamp=False):
    f = dict(T["mov"], vector_opcode=vop, vector_dest=dest, vector_write_mask=mask, vector_clamp=clamp,
             scalar_opcode=sop, scalar_dest=sdest, scalar_write_mask=smask, scalar_clamp=False, is_export=export)
    for n, s in ((1, s1), (2, s2 or s1), (3, s3 or s1)):
        kind, reg, swizzle, negate = s
        f[f"src{n}_reg"] = reg
        f[f"src{n}_is_temp"] = kind == "r"
        f[f"src{n}_swizzle"] = program.swizzle4(swizzle)
        f[f"src{n}_negate"] = negate
    return f


def move(dest, src, mask=0xF):
    return alu(V.kMax, dest, mask, src, src)


def scalar(sop, dest, src, mask=0xF):
    """A scalar op alone, as xsc's emit_scalar builds it."""
    f = alu(V.kMax, 0, 0, src, src, src, sop=sop, sdest=dest, smask=mask)
    return f


def export(src):
    return alu(V.kMax, 0, 0xF, src, src, export=True)


def build(body, exports, optimize):
    """The container, and the instructions as the pass left them [(kind, fields)]."""
    p = PixelProgram(XSDK_REGISTERS)
    p.whole_sources = True
    p.optimize = optimize
    for i in range(INPUTS):
        p.input(i, ce.USAGE_TEXCOORD, i)
    for f in body:
        p.alu(**f)
    p.alloc_colors()
    for f in exports:
        p.alu(**f)
    seen = []
    original = p._optimize

    def capture():
        original()
        seen.extend((ins.kind, dict(ins.fields)) for stage in p._stages for ins in stage)
    p._optimize = capture
    return p.finish([]), seen


def alu_count(blob):
    cont = container.Container(blob)
    return sum(1 for rec in xu._decode_program(cont.ucode_dwords, cont.shader_type)[0] if rec.kind == "alu")


def differ(a, b):
    if a is None or b is None:
        return a is not b
    return a != b and not (math.isnan(a) and math.isnan(b))


def compare(before, after, plain, optimized, trials=8):
    """What differs between the two, if anything: the instructions, then the microcode on what they define."""
    rng = random.Random(7)
    for trial in range(trials):
        registers = {i: [rng.uniform(-3.0, 3.0) for _ in range(4)] for i in range(INPUTS)}
        constants = {i: [rng.uniform(-3.0, 3.0) for _ in range(4)] for i in range(16)}
        want = equivalence.run_fields(before, registers, constants, ABS_SOURCE)
        got = equivalence.run_fields(after, registers, constants, ABS_SOURCE)
        for reg in want:
            for i in range(4):
                if want[reg][i] is not None and differ(want[reg][i], got.get(reg, [None] * 4)[i]):
                    return f"trial {trial}: instructions' export {reg}.{'xyzw'[i]} {want[reg][i]} -> {got[reg][i]}"
        hw_want = equivalence.run(plain, registers, constants)
        hw_got = equivalence.run(optimized, registers, constants)
        for reg in want:
            for i in range(4):
                if want[reg][i] is not None and differ(hw_want[reg][i], hw_got[reg][i]):
                    return f"trial {trial}: microcode's export {reg}.{'xyzw'[i]} {hw_want[reg][i]} -> {hw_got[reg][i]}"
    return None


def case(name, body, exports, expect_alu):
    plain, before = build(body, exports, False)
    optimized, after = build(body, exports, True)
    problem = compare(before, after, plain, optimized)
    count = alu_count(optimized)
    ok = problem is None and count == expect_alu
    detail = problem or f"{alu_count(plain)} -> {count} ALU instructions, {expect_alu} expected"
    print(f"  {'PASS' if ok else 'FAIL'} {name}: {detail}")
    return ok


CASES = [
    ("a move's readers read its source, and the move goes",
     [move(2, r(0, "yxwz")), alu(V.kAdd, 3, 0xF, r(2), c(0))],
     [export(r(3))], 2),
    ("a negated move from a constant folds into its reader",
     [move(2, c(1, negate=True)), alu(V.kMul, 3, 0xF, r(2), r(0))],
     [export(r(3))], 2),
    ("an absolute move from a temp folds, its reader keeping the absolute value",
     [move(2, r(0, absolute=True)), alu(V.kAdd, 3, 0xF, r(2, "wzyx", negate=True), r(1))],
     [export(r(3))], 2),
    ("a move from a constant whose reader takes an absolute value stays",
     [move(2, c(1)), alu(V.kAdd, 3, 0xF, r(2, absolute=True), r(1))],
     [export(r(3))], 3),
    ("a vector assembled by two moves stays, as no one source holds it",
     [move(2, r(0), 0x3), move(2, c(0, "xyzz"), 0x4), alu(V.kDp3, 3, 0xF, r(2), r(1))],
     [export(r(3))], 4),
    ("a move whose source changes before its reader stays",
     [move(2, r(0)), alu(V.kAdd, 0, 0xF, r(1), c(0)), alu(V.kMul, 3, 0xF, r(2), r(0))],
     [export(r(3))], 4),
    ("a recomputed sum is the first one's",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), alu(V.kAdd, 3, 0xF, r(0), c(0)), alu(V.kMul, 4, 0xF, r(2), r(3))],
     [export(r(4))], 3),
    ("a sum of different values isn't merged",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), alu(V.kAdd, 3, 0xF, r(0), c(1)), alu(V.kMul, 4, 0xF, r(2), r(3))],
     [export(r(4))], 4),
    ("mul then add is a mad, the product's negation moving to a factor",
     [alu(V.kMul, 2, 0xF, r(0), c(0)), alu(V.kAdd, 3, 0xF, r(2, "zwxy", negate=True), r(1))],
     [export(r(3))], 2),
    ("a product read twice isn't fused",
     [alu(V.kMul, 2, 0xF, r(0), c(0)), alu(V.kAdd, 3, 0xF, r(2), r(1)), alu(V.kAdd, 4, 0xF, r(3), r(2))],
     [export(r(4))], 4),
    ("a scalar op joins an independent vector op",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), scalar(S.kRcp, 3, r(1, "xyzw"), 0x1),
      alu(V.kMul, 4, 0xF, r(2), r(3, "xxxx"))],
     [export(r(4))], 3),
    ("a scalar op reading the vector op's result doesn't join it",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), scalar(S.kRsq, 3, r(2, "xyzw"), 0x1),
      alu(V.kMul, 4, 0xF, r(2), r(3, "xxxx"))],
     [export(r(4))], 4),
    ("a third constant isn't folded into an instruction holding two",
     [move(2, c(2)), alu(V.kMad, 3, 0xF, c(0), c(1), r(2))],
     [export(r(3))], 3),
    ("dead code goes",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), alu(V.kMul, 3, 0xF, r(1), c(1))],
     [export(r(3))], 2),
]


def wrong_rewrite_is_seen():
    """A rewrite that reads the wrong component must show as a difference."""
    body = [move(2, r(0, "yxwz")), alu(V.kAdd, 3, 0xF, r(2), c(0))]
    plain, before = build(body, [export(r(3))], False)
    wrong, _ = build([alu(V.kAdd, 3, 0xF, r(0), c(0))], [export(r(3))], False)
    after = [(kind, dict(f)) for kind, f in before]
    after[1][1]["src1_reg"], after[1][1]["src1_swizzle"] = 0, program.swizzle4("xyzw")
    after.pop(0)
    problem = compare(before, after, plain, wrong)
    print(f"  {'PASS' if problem else 'FAIL'} a wrong rewrite shows: {problem or 'nothing seen'}")
    return problem is not None


def main():
    ok = all([case(*c_) for c_ in CASES]) & wrong_rewrite_is_seen()
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
