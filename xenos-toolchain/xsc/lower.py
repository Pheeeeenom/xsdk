#!/usr/bin/env python3
"""xsc lowering, SPIR-V to a Xenos shader container: xost's compiler, which runs in C (xsdk:src/xsc_compile.c) and
xsc/native.py calls. lower_ex and lower keep the signatures the Python compiler had, so the tools that compile shaders
call them as they did; they run in WSL, where make builds the library.

docs/xsc.md says what the compiler lowers and how.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.native as native  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xsc.templates import templates  # noqa: E402
from xasm.program import LIBXENON_REGISTERS, RegisterBudget, RegisterBudgetError  # noqa: E402


class LowerError(Exception):
    """The compiler doesn't lower something the shader does."""


def _templates():
    """The instruction templates the hand-built programs start from (xsc/templates.py)."""
    return templates()


def lower_ex(m: spirv.Module, registers: RegisterBudget = LIBXENON_REGISTERS, optimize: bool = False,
             group_fetches: bool = True):
    """Returns (container_bytes, literal_manifest). The manifest is [(constant_register, (f0,f1,f2,f3))] for GLSL
    literals, with ("array", reg, regs, uniform_name) and ("loop", loop_id, uniform_name) for a hardware loop's array
    and trip count, which the host uploads to integer_constants[8+loop_id], ("block_loop", loop_id, kind, set, binding,
    offset_bytes) for a loop an int in a block counts, which the host reads from the block for loop constant loop_id,
    and ("clip", plane, constant_register) for each user clip plane gl_ClipDistance reads, which the host sets from that
    register. registers is the runtime's register split; a shader past its kind's share raises RegisterBudgetError.
    optimize has the compiler rewrite the instructions before they're allocated; group_fetches off lays a pixel
    shader's fetches out as before milestone 36."""
    try:
        container_bytes, manifest, _ = native.compile_spirv(m.blob, registers.pixel, registers.vertex, optimize,
                                                            group_fetches)
    except native.CompileError as e:
        if e.status == native.REFUSED:
            raise LowerError(str(e)) from None
        if e.status == native.REGISTERS:
            raise RegisterBudgetError(str(e)) from None
        raise ValueError(str(e)) from None
    return container_bytes, manifest


def lower(m: spirv.Module, registers: RegisterBudget = LIBXENON_REGISTERS, optimize: bool = False) -> bytes:
    return lower_ex(m, registers, optimize)[0]


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("spv")
    ap.add_argument("-o", "--out")
    ap.add_argument("--pixel-registers", type=int, default=LIBXENON_REGISTERS.pixel,
                    help="pixel shaders' share of the register split (libxenon's by default)")
    ap.add_argument("--vertex-registers", type=int, default=LIBXENON_REGISTERS.vertex,
                    help="vertex shaders' share of the register split (libxenon's by default)")
    ap.add_argument("--optimize", action="store_true", help="rewrite the instructions first")
    args = ap.parse_args()
    m = spirv.Module.parse(Path(args.spv).read_bytes())
    blob = lower(m, RegisterBudget(pixel=args.pixel_registers, vertex=args.vertex_registers), args.optimize)
    if args.out:
        Path(args.out).write_bytes(blob)
        print(f"wrote {args.out} ({len(blob)} B)")
    c = container.Container(blob)
    print(xu.disassemble(c.ucode_dwords, c.shader_type))


if __name__ == "__main__":
    main()
