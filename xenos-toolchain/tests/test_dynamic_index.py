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
- a0 doesn't survive an alloc on the console (docs/HARDWARE.md, "a0 doesn't survive an alloc"). In every testdata
  shader that reads at a0, optimiser off and on, each read at a0 must have an a0 load since its alloc, and a load again
  of what a0 held must come only where such a read needs it. vk_dynamic_interpolator.vert reads its colour, then its
  scale's w, past the interpolators' alloc at the a0 its position loaded: the position's load must come again right
  before the colour's read, and the simulator must give each of its 16 indices' position and colour.
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


def a0_ops(blob):
    """Each a0 load and read at a0 in program order: (the allocs before it, "load" or "read", its words)."""
    c = container.Container(blob)
    ops, allocs = [], 0
    for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if rec.kind == "cf" and rec.opcode == "alloc":
            allocs += 1
        elif rec.kind == "alu":
            op = xu.AluInstruction(*rec.raw)
            if op.scalar_opcode() in (S.kMaxAs, S.kMaxAsf) or op.vector_opcode() == V.kMaxA:
                ops.append((allocs, "load", tuple(rec.raw)))
            elif op.is_const_address_register_relative() and any(
                    not op.src_is_temp(n) and op.src_const_is_addressed(n) for n in (1, 2, 3)):
                ops.append((allocs, "read", tuple(rec.raw)))
    return ops


def a0_reloads(blob):
    """(reloads, problem): the loads of what a0 held before an alloc, and what breaks the console's rule, a0 lost at
    every alloc (docs/HARDWARE.md, "a0 doesn't survive an alloc"): a read at a0 with no load since the last alloc, or a
    load again of what a0 holds that no read after an alloc needs."""
    last, reloads, unread = None, 0, None
    for allocs, kind, words in a0_ops(blob):
        if unread is not None and (kind == "load" or allocs != unread):
            return reloads, f"a0 loaded again after alloc {unread} with no read at a0 after it"
        if kind == "read":
            if last is None or last[0] != allocs:
                return reloads, f"a read at a0 after alloc {allocs} with no a0 load since"
            unread = None
            continue
        if last is not None and last[1] == words:
            reloads += 1
            unread = allocs
            if last[0] == allocs:
                return reloads, f"a0 loaded again in alloc region {allocs}, where it holds that already"
        last = (allocs, words)
    return reloads, None if unread is None else f"a0 loaded again after alloc {unread} with no read at a0 after it"


# The shaders that read at a0, and the reloads each takes: only vk_dynamic_interpolator.vert reads at a0 past an alloc
# what it loaded a0 with before it.
RELOADS = {"vk_dynamic_rows.vert": 0, "vk_dynamic_matrices.vert": 0, "vk_frag_coord.frag": 0,
           "vk_instance_index.vert": 0, "vk_loop_count_if.frag": 0, "vk_dynamic_interpolator.vert": 1}
ROWS = 16


def check_reloads():
    for name, want in RELOADS.items():
        for optimize in (False, True):
            blob, _ = lower_ex(spirv.Module.parse((TD / f"{name}.spv").read_bytes()), XSDK_REGISTERS,
                               optimize=optimize)
            reloads, problem = a0_reloads(blob)
            if problem or reloads != want:
                print(f"  FAIL {name}: optimiser {'on' if optimize else 'off'}: {reloads} a0 reloads, want {want}"
                      f"{f'; {problem}' if problem else ''}")
                return False
    print(f"  PASS a0 reloads: {', '.join(f'{n} {w}' for n, w in RELOADS.items())}, optimiser off and on; every read "
          f"at a0 has a load since its alloc, and no load again goes unread")
    return True


def check_interpolator():
    name = "vk_dynamic_interpolator.vert"
    scales = [[1.0 + 0.1 * i, 2.0 - 0.1 * i, 0.5 + 0.05 * i, 1.0 + 0.25 * i] for i in range(ROWS)]
    colors = [[0.05 * i, 0.2, 0.06 * i, 1.0] for i in range(ROWS)]
    for optimize in (False, True):
        blob, manifest = lower_ex(spirv.Module.parse((TD / f"{name}.spv").read_bytes()), XSDK_REGISTERS,
                                  optimize=optimize)
        ops = a0_ops(blob)
        loads = [i for i, (_, kind, _) in enumerate(ops) if kind == "load"]
        past = [i for i, (allocs, kind, _) in enumerate(ops) if kind == "read" and allocs == 2]
        # The position's load and reads after alloc position, then the reload right before the colour's read and the
        # scale's after alloc interpolators.
        if (len(loads) != 2 or len(past) != 2 or ops[loads[0]][0] != 1 or ops[loads[1]][0] != 2
                or loads[1] != past[0] - 1 or ops[loads[0]][2] != ops[loads[1]][2]):
            print(f"  FAIL {name}: optimiser {'on' if optimize else 'off'}: a0 loads and reads {ops}: want the "
                  f"position's load again after alloc interpolators, right before the first of its two reads there")
            return False
        consts = registers([x for row in scales + colors for x in row], 0)
        consts.update({reg: list(values) for reg, values in manifest})
        for index in range(ROWS):
            exports = run(blob, {0: [0.25, -0.5, float(index), 0.0]}, consts)
            want = [0.25 * scales[index][0], -0.5 * scales[index][1], 0.5 * scales[index][2], scales[index][3]]
            color = [x * scales[index][3] for x in colors[index]]
            if not close(exports.get(OPOS, []), want) or not close(exports.get(0, []), color):
                print(f"  FAIL {name}: index {index}: position {exports.get(OPOS)} (want {want}), colour "
                      f"{exports.get(0)} (want {color})")
                return False
    problem = oracle_reads_a0(blob, "vk_dynamic_interpolator_vert", ["scales", "colors"])
    if problem:
        print(f"  FAIL {name}: {problem}")
        return False
    print(f"  PASS {name}: the position's maxasf again past the interpolators' alloc, right before colors[i]'s read; "
          f"position and colour for i 0 to {ROWS - 1}, optimiser off and on")
    return True


def main():
    results = [check_rows(), check_matrices(), check_frag_coord(), check_frag_coord_depth(), check_reloads(),
               check_interpolator()]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
