#!/usr/bin/env python3
"""The compiler's optimiser (xsdk:src/xsc_optimize.c), case by case, on hand-built pixel programs the C builds as it
builds the compiler's (tests/xsc_internals.py).

Each case builds a program twice, without and with the pass, and checks:
  - the two microcodes compute the same exports, run on random inputs and constants by xdis/equivalence.py, on the
    components the first defines;
  - the pass left the number of ALU instructions the case expects: what it may remove it does, and what it mustn't
    touch (a move other values share a register with, a source changed in between, a third constant) it keeps.
A last case hands the comparison a wrong rewrite, to show it would see one. Runs in WSL, where make builds
build/host/libxsc.so.

tools/check_optimize.py runs the same comparison over a folder of real shaders.
"""
import math
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

sys.path.insert(0, str(ROOT / "tests"))

import xasm.program as program  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.container_emit as ce  # noqa: E402
import xdis.equivalence as equivalence  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import _templates  # noqa: E402
from xsc_internals import ABS_SOURCE, pixel_program  # noqa: E402

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
    return pixel_program(INPUTS, ce.USAGE_TEXCOORD, body, exports, XSDK_REGISTERS.pixel, optimize)


def alu_count(blob):
    cont = container.Container(blob)
    return sum(1 for rec in xu._decode_program(cont.ucode_dwords, cont.shader_type)[0] if rec.kind == "alu")


def differ(a, b):
    if a is None or b is None:
        return a is not b
    return a != b and not (math.isnan(a) and math.isnan(b))


def compare(plain, optimized, trials=8):
    """What differs between the two microcodes' exports, if anything, on the components the first defines."""
    rng = random.Random(7)
    for trial in range(trials):
        registers = {i: [rng.uniform(-3.0, 3.0) for _ in range(4)] for i in range(INPUTS)}
        constants = {i: [rng.uniform(-3.0, 3.0) for _ in range(4)] for i in range(16)}
        want = equivalence.run(plain, registers, constants)
        got = equivalence.run(optimized, registers, constants)
        for reg in want:
            for i in range(4):
                if want[reg][i] is not None and differ(want[reg][i], got.get(reg, [None] * 4)[i]):
                    return f"trial {trial}: export {reg}.{'xyzw'[i]} {want[reg][i]} -> {got[reg][i]}"
    return None


def case(name, body, exports, expect_alu):
    plain = build(body, exports, False)
    optimized = build(body, exports, True)
    problem = compare(plain, optimized)
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
    ("components computed apart and copied into one vector are computed there, a sum swizzled to its lane",
     [alu(V.kDp4, 2, 0x1, r(0), c(0)), alu(V.kAdd, 3, 0x1, r(0, "zzzz"), c(1)), move(4, r(2), 0x1),
      move(4, r(3, "xxxx"), 0x2), alu(V.kMul, 5, 0xF, r(4, "xyxy"), r(1))],
     [export(r(5))], 4),
    ("a copy clamped to 0 to 1 becomes its writer's clamp",
     [alu(V.kMad, 2, 0xF, r(0), c(0), r(1)), alu(V.kMax, 3, 0xF, r(2), r(2), clamp=True),
      alu(V.kMul, 4, 0xF, r(3), c(1))],
     [export(r(4))], 3),
    ("a clamp isn't put on a result something else reads unclamped",
     [alu(V.kMad, 2, 0xF, r(0), c(0), r(1)), alu(V.kMax, 3, 0xF, r(2), r(2), clamp=True),
      alu(V.kMul, 4, 0xF, r(3), r(2))],
     [export(r(4))], 4),
    ("a dot product's other reader reads it from the copy's place",
     [alu(V.kDp3, 2, 0x3, r(0), r(1)), move(3, r(2), 0x1), move(3, r(1), 0x2), scalar(S.kRsq, 4, r(2, "yyyy"), 0x1),
      alu(V.kMul, 5, 0xF, r(3, "xyxy"), r(4, "xxxx"))],
     [export(r(5))], 4),
    ("a product whose other lane is read elsewhere stays in its temp",
     [alu(V.kMul, 2, 0x3, r(0), c(0)), move(3, r(2), 0x1), move(3, r(1), 0x2),
      alu(V.kAdd, 4, 0xF, r(3, "xyxy"), r(2, "yyyy"))],
     [export(r(4))], 5),
    ("a component whose place is read before the copy stays in its temp",
     [move(3, r(1), 0x7), alu(V.kAdd, 3, 0x8, r(0), c(2)), alu(V.kAdd, 2, 0x1, r(0), c(0)),
      alu(V.kMul, 4, 0xF, r(3), c(1)), move(3, r(2, "xxxx"), 0x1), alu(V.kAdd, 5, 0xF, r(3), r(4))],
     [export(r(5))], 7),
    ("an export assembled from parts takes each part, its lanes nothing writes filled by one",
     [],
     [alu(V.kDp4, 2, 0x1, r(0), c(0)), alu(V.kMul, 3, 0x1, r(1, "yyyy"), c(1)), move(4, r(2), 0x1),
      move(4, r(3, "xxxx"), 0x2), alu(V.kMax, 0, 0xF, r(4), r(4), export=True)], 2),
    ("a value computed before the colours' alloc is copied to its export after it",
     [alu(V.kAdd, 2, 0xF, r(0), c(0))],
     [export(r(2))], 2),
]


def wrong_rewrite_is_seen():
    """A rewrite that reads the wrong component must show as a difference."""
    plain = build([move(2, r(0, "yxwz")), alu(V.kAdd, 3, 0xF, r(2), c(0))], [export(r(3))], False)
    wrong = build([alu(V.kAdd, 3, 0xF, r(0), c(0))], [export(r(3))], False)
    problem = compare(plain, wrong)
    print(f"  {'PASS' if problem else 'FAIL'} a wrong rewrite shows: {problem or 'nothing seen'}")
    return problem is not None


def main():
    ok = all([case(*c_) for c_ in CASES]) & wrong_rewrite_is_seen()
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
