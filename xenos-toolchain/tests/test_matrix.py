#!/usr/bin/env python3
"""Numerical gate for the matrix builtins (transpose / determinant / inverse).

Matrix math is oracle-blind -- XenosRecomp validates the decode, not the
values -- so this simulates the emitted Xenos ALU (both pipes) against a
reference and checks the actual numbers. It is what caught the dp3/dp2 .wzxy
swizzle bug (a full 4-dot is permutation-invariant, a 3-dot is not).
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xsc.spirv as spirv                      # noqa: E402
from xsc.lower import lower_ex                 # noqa: E402
import xdis.container as container             # noqa: E402
import xdis.xenos_ucode as xu                  # noqa: E402

TD = ROOT / "xsc" / "testdata"
V = xu.AluVectorOpcode
S = xu.AluScalarOpcode


def _swz(src, s):
    return [src[xu.AluInstruction.GetSwizzledComponentIndex(s, i)]
            for i in range(4)]


def _src(op, n, regs, consts):
    reg = op.src_reg(n)
    if op.src_is_temp(n):
        val = list(regs.get(reg & 0x3F, [0.0] * 4))
        if reg & 0x80:
            val = [abs(x) for x in val]
    else:
        val = list(consts.get(reg, [0.0] * 4))
    val = _swz(val, op.src_swizzle(n))
    return [-x for x in val] if op.src_negate(n) else val


def evaluate(blob, regs, consts):
    """Run the fragment program's ALU and return the exported oC0 (xyz; the
    .w constant-1 export mask is not modelled)."""
    regs = {k: list(v) for k, v in regs.items()}
    c = container.Container(blob)
    recs = xu._decode_program(c.ucode_dwords, c.shader_type)[0]
    out = [0.0, 0.0, 0.0, 0.0]
    for rec in recs:
        if rec.kind != "alu":
            continue
        op = xu.AluInstruction(*rec.raw)
        vmask = op.GetVectorOpResultWriteMask()
        if vmask:
            s1, s2 = _src(op, 1, regs, consts), _src(op, 2, regs, consts)
            vo = op.vector_opcode()
            if vo == V.kMul:
                r = [s1[i] * s2[i] for i in range(4)]
            elif vo == V.kAdd:
                r = [s1[i] + s2[i] for i in range(4)]
            elif vo == V.kMax:
                r = [max(s1[i], s2[i]) for i in range(4)]
            elif vo == V.kMin:
                r = [min(s1[i], s2[i]) for i in range(4)]
            elif vo == V.kMad:
                s3 = _src(op, 3, regs, consts)
                r = [s1[i] * s2[i] + s3[i] for i in range(4)]
            elif vo == V.kDp3:
                r = [sum(s1[i] * s2[i] for i in range(3))] * 4
            elif vo == V.kDp4:
                r = [sum(s1[i] * s2[i] for i in range(4))] * 4
            else:
                raise SystemExit(f"test_matrix: unhandled vector op {vo}")
            dst = out if op.is_export() else regs.setdefault(
                op.vector_dest(), [0.0] * 4)
            for i in range(4):
                if vmask & (1 << i):
                    dst[i] = r[i]
        if op.scalar_opcode() == S.kRcp:           # 1/x scalar pipe
            s3 = _src(op, 3, regs, consts)
            v = 1.0 / s3[0] if s3[0] != 0 else 0.0
            for i in range(4):
                regs.setdefault(op.scalar_dest(), [0.0] * 4)[i] = v
    return out


M = [[2.0, -1.0, 0.5], [0.25, 3.0, -2.0], [1.0, 0.5, 4.0]]   # rows
COL = [1.5, -2.0, 0.5, 1.0]


def _mv(m, v):
    return [sum(m[i][j] * v[j] for j in range(3)) for i in range(3)]


def _T(m):
    return [[m[j][i] for j in range(3)] for i in range(3)]


def _det(m):
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))


def _inv(m):
    d = _det(m)
    cof = [[(m[(i + 1) % 3][(j + 1) % 3] * m[(i + 2) % 3][(j + 2) % 3]
             - m[(i + 1) % 3][(j + 2) % 3] * m[(i + 2) % 3][(j + 1) % 3])
            for j in range(3)] for i in range(3)]
    return [[cof[j][i] / d for j in range(3)] for i in range(3)]


def _close(a, b, eps=2e-3):
    return all(abs(a[i] - b[i]) <= eps for i in range(len(b)))


def _consts(man):
    c = {0: M[0] + [0.0], 1: M[1] + [0.0], 2: M[2] + [0.0]}
    for reg, vals in man:
        if isinstance(reg, int):
            c[reg] = list(vals)
    return c


def main():
    ok = True
    cases = [
        ("fs_mtest", _mv(_T(M), COL[:3]) + [_det(M)],
         "transpose(mat3) + determinant(mat3)"),
        ("fs_minv", _mv(_inv(M), COL[:3]),
         "inverse(mat3) via adjugate / det"),
    ]
    for name, want, desc in cases:
        blob, man = lower_ex(spirv.Module.parse(
            (TD / f"{name}.frag.spv").read_bytes()))
        got = evaluate(blob, {0: COL}, _consts(man))
        n = len(want)
        if _close(got[:n], want):
            print(f"  PASS {name}.frag: {desc} "
                  f"-> {[round(x, 3) for x in got[:n]]}")
        else:
            ok = False
            print(f"  FAIL {name}.frag: {desc} got "
                  f"{[round(x, 3) for x in got[:n]]} want "
                  f"{[round(x, 3) for x in want]}")
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
