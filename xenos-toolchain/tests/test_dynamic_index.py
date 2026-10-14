#!/usr/bin/env python3
"""xsc reads block arrays at a run-time index through a0, and a pixel shader's gl_FragCoord.xy from the pixel
parameters, as vulkan4xenon's milestone 11 needs for Aurora's matrices picked per vertex and range fog.

- vk_dynamic_rows.vert picks three matrix rows by a vertex attribute and a colour by that index over 3;
  vk_dynamic_matrices.vert a whole mat4 by its index. A small simulator of the ALU, MOVA and a0-relative reads
  included, runs each on every index and checks the position and colour; the oracle must accept them and read at a0.
- vk_frag_coord.frag picks a colour by the pixel's column, 128 pixels a band. Its container must turn the pixel
  parameters on in the register past its interpolator, and the simulator, given the pixel parameters as the GPU
  writes them (the column's magnitude, the sign bit the facing), must pick each column's band.
- vk_frag_coord_depth.frag reads gl_FragCoord.z, which the pixel parameters don't hold, and must be refused.
"""
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xsc.spirv as spirv  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_xsc_lower import TD, oracle_hlsl  # noqa: E402

V = xu.AluVectorOpcode
S = xu.AluScalarOpcode
OPOS = int(xu.ExportRegister.kVSPosition)
PARAM_GEN = 1 << 18          # SQ_PROGRAM_CNTL (xenia:src/xenia/gpu/registers.h:140)
PARAM_GEN_POS_SHIFT = 8      # SQ_CONTEXT_MISC (registers.h:234)
SC_OUTPUT_SCREEN_XY = 1 << 1  # SQ_CONTEXT_MISC (registers.h:156), which the console needs with PARAM_GEN


def lowered(name):
    return lower_ex(spirv.Module.parse((TD / name).read_bytes()), XSDK_REGISTERS)


def run(blob, inputs, consts):
    """Runs a program's ALU, MOVA's a0 and a0- or absolute-addressed constants included; returns its exports."""
    regs = {k: list(v) for k, v in inputs.items()}
    exports = {}
    a0 = 0

    def source(op, n):
        reg = op.src_reg(n)
        if op.src_is_temp(n):
            val = list(regs.get(reg & 0x3F, [0.0] * 4))
            if reg & 0x80:
                val = [abs(x) for x in val]
        else:
            if op.src_const_is_addressed(n):
                if not op.is_const_address_register_relative():
                    raise SystemExit("test_dynamic_index: an aL-relative constant outside a loop")
                reg += a0
            val = list(consts.get(reg, [0.0] * 4))
        val = [val[xu.AluInstruction.GetSwizzledComponentIndex(op.src_swizzle(n), i)] for i in range(4)]
        return [-x for x in val] if op.src_negate(n) else val

    c = container.Container(blob)
    for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if rec.kind != "alu":
            continue
        op = xu.AluInstruction(*rec.raw)
        dst = exports if op.is_export() else regs
        vmask = op.GetVectorOpResultWriteMask()
        if vmask:
            s1, s2 = source(op, 1), source(op, 2)
            vo = op.vector_opcode()
            if vo == V.kMul:
                r = [s1[i] * s2[i] for i in range(4)]
            elif vo == V.kAdd:
                r = [s1[i] + s2[i] for i in range(4)]
            elif vo == V.kMax:
                r = [max(s1[i], s2[i]) for i in range(4)]
            elif vo == V.kMad:
                s3 = source(op, 3)
                r = [s1[i] * s2[i] + s3[i] for i in range(4)]
            elif vo == V.kDp4:
                r = [sum(s1[i] * s2[i] for i in range(4))] * 4
            elif vo == V.kTrunc:
                r = [float(math.trunc(x)) for x in s1]
            else:
                raise SystemExit(f"test_dynamic_index: unhandled vector op {vo}")
            out = dst.setdefault(op.vector_dest(), [0.0] * 4)
            for i in range(4):
                if vmask & (1 << i):
                    out[i] = r[i]
        so = op.scalar_opcode()
        if so != S.kRetainPrev:
            s3 = source(op, 3)
            a, b = s3[3], s3[0]          # src3's w and x (xenia:src/xenia/gpu/shader_translator.cc:1246)
            if so == S.kMaxAs:
                a0 = int(max(-256.0, min(255.0, math.floor(a + 0.5))))
                result = max(a, b)
            elif so == S.kMaxAsf:
                a0 = int(max(-256.0, min(255.0, math.floor(a))))
                result = max(a, b)
            elif so == S.kRcp:
                result = 1.0 / a
            else:
                raise SystemExit(f"test_dynamic_index: unhandled scalar op {so}")
            smask = op.GetScalarOpResultWriteMask()
            if smask:
                out = dst.setdefault(op.scalar_dest(), [0.0] * 4)
                for i in range(4):
                    if smask & (1 << i):
                        out[i] = result
        if op.is_export():
            out = exports.setdefault(op.vector_dest(), [0.0] * 4)
            for i in range(4):
                if op.GetConstant1WriteMask() & (1 << i):
                    out[i] = 1.0
                if op.GetConstant0WriteMask() & (1 << i):
                    out[i] = 0.0
    return exports


def registers(floats, first):
    return {first + i: list(floats[4 * i:4 * i + 4]) for i in range(len(floats) // 4)}


def close(a, b):
    return len(a) == len(b) and all(abs(x - y) <= 1e-4 for x, y in zip(a, b))


def oracle_reads_a0(blob, tag, names):
    hlsl = oracle_hlsl(blob, tag)
    if hlsl is None:
        return "XenosRecomp rejected it"
    missing = [n for n in names if f"{n}(" not in hlsl or "+ a0)" not in hlsl]
    if "a0 = (int)" not in hlsl or missing:
        return f"the oracle shows no a0 read of {missing or names}"
    return None


POSITION = [0.5, -2.0, 3.0, 1.0]


def check_rows():
    blob, manifest = lowered("vk_dynamic_rows.vert.spv")
    rows = [[float(r * 4 + c + 1) * (0.5 if r % 2 else 1.0) for c in range(4)] for r in range(12)]
    colors = [[0.1 * k, 0.2 * k, 0.3, 1.0] for k in range(1, 5)]
    consts = registers([x for row in rows + colors for x in row], 0)
    consts.update({reg: list(values) for reg, values in manifest})
    for index in (0, 3, 6, 9):
        exports = run(blob, {0: POSITION, 1: [float(index), 0.0, 0.0, 1.0]}, consts)
        want = [sum(rows[index + k][i] * POSITION[i] for i in range(4)) for k in range(3)] + [1.0]
        if not close(exports.get(OPOS, []), want) or not close(exports.get(0, []), colors[index // 3]):
            print(f"  FAIL vk_dynamic_rows.vert: index {index}: position {exports.get(OPOS)} (want {want}), "
                  f"colour {exports.get(0)} (want {colors[index // 3]})")
            return False
    problem = oracle_reads_a0(blob, "vk_dynamic_rows_vert", ["rows", "colors"])
    if problem:
        print(f"  FAIL vk_dynamic_rows.vert: {problem}")
        return False
    print(f"  PASS vk_dynamic_rows.vert: rows i to i + 2 and colour i / 3 at a0, for i 0, 3, 6 and 9 ({len(blob)} B)")
    return True


def check_matrices():
    blob, manifest = lowered("vk_dynamic_matrices.vert.spv")
    matrices = [[[float(m * 16 + r * 4 + c) - 7.5 for c in range(4)] for r in range(4)] for m in range(3)]
    columns = [matrices[m][r][c] for m in range(3) for c in range(4) for r in range(4)]   # std140: column-major
    consts = registers(columns, 0)
    consts.update({reg: list(values) for reg, values in manifest})
    for index in range(3):
        exports = run(blob, {0: POSITION, 1: [float(index), 0.0, 0.0, 1.0]}, consts)
        want = [sum(matrices[index][r][c] * POSITION[c] for c in range(4)) for r in range(4)]
        if not close(exports.get(OPOS, []), want):
            print(f"  FAIL vk_dynamic_matrices.vert: index {index}: position {exports.get(OPOS)} (want {want})")
            return False
    problem = oracle_reads_a0(blob, "vk_dynamic_matrices_vert", ["m"])
    if problem:
        print(f"  FAIL vk_dynamic_matrices.vert: {problem}")
        return False
    print(f"  PASS vk_dynamic_matrices.vert: m[i] * position, a0 i times 4, for i 0 to 2 ({len(blob)} B)")
    return True


def as_float(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def check_frag_coord():
    blob, manifest = lowered("vk_frag_coord.frag.spv")
    c = container.Container(blob)
    position = c.context_misc >> PARAM_GEN_POS_SHIFT & 0xFF
    if not c.program_control & PARAM_GEN or position != 1 or not c.context_misc & SC_OUTPUT_SCREEN_XY:
        print(f"  FAIL vk_frag_coord.frag: SQ_PROGRAM_CNTL {c.program_control:#010x}, SQ_CONTEXT_MISC "
              f"{c.context_misc:#010x}: want PARAM_GEN and SC_OUTPUT_SCREEN_XY on, the parameters in r1 past the "
              f"interpolator")
        return False
    colors = [[0.05 * k, 1.0 - 0.05 * k, 0.5, 1.0] for k in range(10)]
    consts = registers([x for color in colors for x in color], 0)
    consts.update({reg: list(values) for reg, values in manifest})
    tint = [1.0, 0.5, 2.0, 1.0]
    for column in (0, 127, 128, 640, 1279):
        for back_facing in (False, True):
            # |x| the column, a back face's sign bit set: -0.0 for column 0 (xenia:src/xenia/gpu/registers.h:224-227).
            x = as_float(0x80000000) if back_facing and column == 0 else (-1.0 if back_facing else 1.0) * column
            exports = run(blob, {0: tint, 1: [x, 300.0, 0.0, 0.0]}, consts)
            band = colors[column // 128]
            want = [band[i] * tint[i] for i in range(4)]
            if not close(exports.get(0, []), want):
                print(f"  FAIL vk_frag_coord.frag: column {column}{' back-facing' if back_facing else ''}: colour "
                      f"{exports.get(0)} (want band {column // 128}, {want})")
                return False
    problem = oracle_reads_a0(blob, "vk_frag_coord_frag", ["colors"])
    if problem:
        print(f"  FAIL vk_frag_coord.frag: {problem}")
        return False
    print(f"  PASS vk_frag_coord.frag: pixel parameters in r1, each column's band at a0, front and back faces "
          f"({len(blob)} B)")
    return True


def check_frag_coord_depth():
    try:
        lowered("vk_frag_coord_depth.frag.spv")
    except LowerError as error:
        print(f"  PASS vk_frag_coord_depth.frag: refused ({error})")
        return True
    print("  FAIL vk_frag_coord_depth.frag: gl_FragCoord.z lowered, though the pixel parameters hold no depth")
    return False


def main():
    results = [check_rows(), check_matrices(), check_frag_coord(), check_frag_coord_depth()]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
