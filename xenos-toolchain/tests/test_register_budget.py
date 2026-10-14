#!/usr/bin/env python3
"""Exit gate for register budgets (xasm.program.RegisterBudget): a shader
past its kind's share of the runtime's register split hangs the GPU, so the
program builder and the text assembler refuse it, under libxenon's split
(16 pixel, 112 vertex) by default and xsdk's (64 and 64) when asked.

Each shader fills r1 up from constants and then reads every one of them, so
all are live at once and recycling dead temporaries can't shrink it.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xdis.container as container          # noqa: E402
import xdis.container_emit as ce            # noqa: E402
import xdis.xenos_ucode as xu               # noqa: E402
from xsc.lower import _templates            # noqa: E402
from xasm import textasm                    # noqa: E402
from xasm.program import (LIBXENON_REGISTERS, XSDK_REGISTERS,  # noqa: E402
                          PixelProgram, RegisterBudget, RegisterBudgetError,
                          VertexProgram)

_V = xu.AluVectorOpcode
EXPORT_POSITION = 62


def alu(op, dest, srcs, export=False):
    f = dict(_templates()["mov"], vector_opcode=op, vector_dest=dest,
             is_export=export, vector_write_mask=0xF, vector_clamp=False,
             scalar_write_mask=0)
    for i, (reg, is_temp) in enumerate(srcs, start=1):
        f[f"src{i}_reg"] = reg
        f[f"src{i}_is_temp"] = is_temp
        f[f"src{i}_swizzle"] = 0
        f[f"src{i}_negate"] = False
    return f


def fill_and_sum(p, count):
    """r1..r<count-1> = c1..c<count-1>, then r<count-1> += each of the rest."""
    top = count - 1
    for r in range(1, count):
        p.alu(**alu(_V.kMax, r, [(r, False), (r, False)]))
    for r in range(1, top):
        p.alu(**alu(_V.kAdd, top, [(top, True), (r, True)]))
    return top


def pixel_shader(count, registers):
    p = PixelProgram(registers)
    p.input(0, ce.USAGE_COLOR)
    top = fill_and_sum(p, count)
    p.alloc_colors()
    p.alu(**alu(_V.kAdd, 0, [(top, True), (0, True)], export=True))
    return p.finish(constants=[])


def vertex_shader(count, registers):
    v = VertexProgram(registers)
    top = fill_and_sum(v, count)
    v.alloc_position()
    v.alu(**alu(_V.kMax, EXPORT_POSITION, [(1, True), (1, True)],
                export=True))
    v.alloc_interpolators()
    v.export(0, ce.USAGE_COLOR, alu(_V.kMax, 0, [(top, True), (top, True)]))
    return v.finish(constants=[])


def registers_used(blob):
    """The register count SQ_PROGRAM_CNTL gets, from its 6-bit field."""
    c = container.Container(blob)
    shift = 8 if c.shader_type == "pixel" else 0
    return (c.program_control >> shift & 0x3F) + 1


def outcome(build, count, registers):
    """The registers the container says it uses, or None if refused."""
    try:
        return registers_used(build(count, registers))
    except RegisterBudgetError:
        return None


def assembled_pixel(top, registers):
    text = f"""
      exec
   max r{top}, r0, r0
      alloc colors
      exece
   max oC0, r{top}, r{top}
"""
    path = Path(__file__).resolve().parent / "budget_tmp.xasm"
    path.write_text(text)
    try:
        return registers_used(textasm.assemble_file(
            str(path), "pixel", container=True, registers=registers))
    except RegisterBudgetError:
        return None
    finally:
        path.unlink()


def main():
    ok = True
    cases = [
        # (what, got, want): want None means refused.
        ("16-register pixel shader, libxenon's split",
         outcome(pixel_shader, 16, LIBXENON_REGISTERS), 16),
        ("18-register pixel shader, libxenon's split",
         outcome(pixel_shader, 18, LIBXENON_REGISTERS), None),
        ("18-register pixel shader, xsdk's split",
         outcome(pixel_shader, 18, XSDK_REGISTERS), 18),
        ("64-register pixel shader, libxenon's split",
         outcome(pixel_shader, 64, LIBXENON_REGISTERS), None),
        ("64-register pixel shader, xsdk's split",
         outcome(pixel_shader, 64, XSDK_REGISTERS), 64),
        ("64-register vertex shader, libxenon's split",
         outcome(vertex_shader, 64, LIBXENON_REGISTERS), 64),
        ("64-register vertex shader, xsdk's split",
         outcome(vertex_shader, 64, XSDK_REGISTERS), 64),
        ("40-register vertex shader, a 32-register vertex share",
         outcome(vertex_shader, 40, RegisterBudget(pixel=16, vertex=32)),
         None),
        ("assembled pixel shader to r15, libxenon's split",
         assembled_pixel(15, LIBXENON_REGISTERS), 16),
        ("assembled pixel shader to r17, libxenon's split",
         assembled_pixel(17, LIBXENON_REGISTERS), None),
        ("assembled pixel shader to r17, xsdk's split",
         assembled_pixel(17, XSDK_REGISTERS), 18),
    ]
    for what, got, want in cases:
        passed = got == want
        ok = ok and passed
        said = "refused" if got is None else f"{got} registers"
        print(f"  {'PASS' if passed else 'FAIL'} {what}: {said}")
    # A shader that fits 16 builds the same under any budget.
    same = (pixel_shader(12, LIBXENON_REGISTERS)
            == pixel_shader(12, XSDK_REGISTERS))
    print(f"  {'PASS' if same else 'FAIL'} a 12-register pixel shader builds "
          f"the same under both splits")
    ok = ok and same
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
