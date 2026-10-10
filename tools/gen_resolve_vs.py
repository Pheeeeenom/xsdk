#!/usr/bin/env python3
"""Generate the vertex shader a resolve draws with: it fetches a float2 position from vertex fetch constant 0 and exports
it as the position, with z and w 1.

Each field comes from Xenia's instruction definitions (xenia:src/xenia/gpu/ucode.h, xenos.h); xost's encoders
(xenos-toolchain/xasm) pack them, and its decoder prints the result. Nothing is taken from a compiled shader. Prints the
microcode as a C initializer, then its disassembly.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import xdis.xenos_ucode as xu  # noqa: E402
from xasm import encode as enc  # noqa: E402

# Fetch destination swizzle, 3 bits a component: 0-3 pick x-w, 4 is 0, 5 is 1 (ucode.h:676-687).
DEST_X, DEST_Y, DEST_ONE = 0, 1, 5
DEST_XY11 = DEST_X | DEST_Y << 3 | DEST_ONE << 6 | DEST_ONE << 9
# xenos.h:602-617
FORMAT_32_32_FLOAT = 37
# ucode.h:1793-1811
EXPORT_POSITION = 62
WRITE_XYZW = 0xF
# The vertex index arrives in r0.x (xenia:src/xenia/gpu/spirv_shader_translator.cc:1510).
INDEX_REGISTER = 0
POSITION_REGISTER = 0
STRIDE_DWORDS = 2
# One bit a fetch, as in libxenon's resolve shader and xost (xenos-toolchain/examples/author_passthrough.py).
VERTEX_CACHE_ONE_FETCH = 1
# Instruction slots follow the two control-flow pairs, three dwords each.
FETCH_SLOT = 2
EXPORT_SLOT = 3


def build():
    fetch = enc.build_vfetch_full(
        src=INDEX_REGISTER, src_swizzle=0, dest=POSITION_REGISTER, dest_swizzle=DEST_XY11, must_be_one=1,
        const_index=0, const_index_sel=0, data_format=FORMAT_32_32_FLOAT, is_signed=True, is_normalized=False,
        stride=STRIDE_DWORDS)
    export = enc.build_alu(
        vector_opcode=xu.AluVectorOpcode.kMax, vector_dest=EXPORT_POSITION, vector_write_mask=WRITE_XYZW,
        is_export=True, src1_reg=POSITION_REGISTER, src2_reg=POSITION_REGISTER, src3_is_temp=False)
    # exec {fetch}; alloc interpolators; alloc position; exece {export}. Sequence bit 0 marks a fetch slot.
    control_flow = [
        enc.build_cf_exec(opcode=xu.ControlFlowOpcode.kExec, address=FETCH_SLOT, count=1, sequence=0b1,
            vc_hi=VERTEX_CACHE_ONE_FETCH, is_predicate_clean=True, addressing_mode=0),
        enc.build_cf_alloc(alloc_type=2),
        enc.build_cf_alloc(alloc_type=1),
        enc.build_cf_exec(opcode=xu.ControlFlowOpcode.kExecEnd, address=EXPORT_SLOT, count=1, sequence=0,
            is_predicate_clean=True, addressing_mode=0),
    ]
    dwords = []
    for i in range(0, len(control_flow), 2):
        dwords.extend(enc.pack_cf_pair(control_flow[i], control_flow[i + 1]))
    assert len(dwords) == FETCH_SLOT * 3
    dwords.extend(fetch)
    dwords.extend(export)
    return dwords


def main():
    dwords = build()
    print("static const uint32_t resolve_vertex_shader[] = {")
    for i in range(0, len(dwords), 6):
        print("\t" + ", ".join(f"0x{d:08X}" for d in dwords[i:i + 6]) + ",")
    print("};")
    print(xu.disassemble(dwords, "vertex"), file=sys.stderr)


if __name__ == "__main__":
    main()
