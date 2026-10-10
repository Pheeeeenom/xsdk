#!/usr/bin/env python3
"""Generate the two shaders the GPU set-up loads, the programs libxenon's set-up loads
(libxenon:drivers/xenos/xe.c:398-441): a vertex shader that exports interpolator 0 as zeros and the position as
(0, 0, 0, 1), and a pixel shader that exports r0 as colour 0.

Each field comes from Xenia's instruction definitions (xenia:src/xenia/gpu/ucode.h); xost's encoders
(xenos-toolchain/xasm) pack them, and its decoder prints the result. Nothing is taken from a compiled shader. Prints
both as C initializers, then their disassembly.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import xdis.xenos_ucode as xu  # noqa: E402
from xasm import encode as enc  # noqa: E402

# ucode.h:1793-1811
EXPORT_INTERPOLATOR_0 = 0
EXPORT_POSITION = 62
EXPORT_COLOR_0 = 0
WRITE_XYZW = 0xF
WRITE_W = 0x8
# ucode.h:209-220
ALLOC_POSITION = 1
ALLOC_INTERPOLATORS = 2
ALLOC_COLORS = 2
SLOT_DWORDS = 3
# Each program ends in an empty instruction slot, as libxenon's do.
EMPTY_SLOT = [0] * SLOT_DWORDS


def no_write():
    """max with no write mask on constants, as libxenon's vertex shader has around its exports."""
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMax, src1_is_temp=False, src2_is_temp=False,
        src3_is_temp=False)


def export(dest, vector_mask, scalar_mask, source, constants):
    """An export of max(source, source); with constants, unwritten components are 0 and components in both masks 1
    (ucode.h:2025-2036, 2051-2059)."""
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMax, vector_dest=dest, vector_write_mask=vector_mask,
        scalar_write_mask=scalar_mask, is_scalar_dest_relative=constants, is_export=True, src1_reg=source,
        src2_reg=source, src3_is_temp=False)


def execute(address, end=False):
    opcode = xu.ControlFlowOpcode.kExecEnd if end else xu.ControlFlowOpcode.kExec
    return enc.build_cf_exec(opcode=opcode, address=address, count=1, sequence=0, is_predicate_clean=True,
        addressing_mode=0)


def vertex_shader():
    # exec {no write}; alloc interpolators; exec {o0 = 0}; alloc position; exec {oPos = (0, 0, 0, 1)};
    # exece {no write}. The instructions start at slot 3, after three control-flow pairs.
    control_flow = [
        execute(3), enc.build_cf_alloc(alloc_type=ALLOC_INTERPOLATORS),
        execute(4), enc.build_cf_alloc(alloc_type=ALLOC_POSITION),
        execute(5), execute(6, end=True),
    ]
    dwords = []
    for i in range(0, len(control_flow), 2):
        dwords.extend(enc.pack_cf_pair(control_flow[i], control_flow[i + 1]))
    dwords.extend(no_write())
    dwords.extend(export(EXPORT_INTERPOLATOR_0, 0, 0, 0, constants=True))
    dwords.extend(export(EXPORT_POSITION, WRITE_W, WRITE_W, 1, constants=True))
    dwords.extend(no_write())
    return dwords + EMPTY_SLOT


def pixel_shader():
    # alloc colors; exece {oC0 = r0}. The instruction is slot 1, after one control-flow pair.
    dwords = list(enc.pack_cf_pair(enc.build_cf_alloc(alloc_type=ALLOC_COLORS), execute(1, end=True)))
    dwords.extend(export(EXPORT_COLOR_0, WRITE_XYZW, 0, 0, constants=False))
    return dwords + EMPTY_SLOT


def print_array(name, dwords):
    print(f"static const uint32_t {name}[] = {{")
    for i in range(0, len(dwords), 6):
        print("\t" + ", ".join(f"0x{d:08X}" for d in dwords[i:i + 6]) + ",")
    print("};")


def main():
    vertex = vertex_shader()
    pixel = pixel_shader()
    print_array("setup_vertex_shader", vertex)
    print_array("setup_pixel_shader", pixel)
    print(xu.disassemble(vertex, "vertex"), file=sys.stderr)
    print(xu.disassemble(pixel, "pixel"), file=sys.stderr)


if __name__ == "__main__":
    main()
