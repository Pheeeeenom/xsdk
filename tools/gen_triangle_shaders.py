#!/usr/bin/env python3
"""Generate milestone 6's shader containers: a vertex shader that fetches a position and a colour and passes them on,
and a pixel shader that writes the interpolated colour.

Each field comes from Xenia's instruction definitions (xenia:src/xenia/gpu/ucode.h, registers.h); xost's encoders
(xenos-toolchain/xasm) pack the instructions and its emitter (xenos-toolchain/xdis/container_emit.py) wraps them in
libxenon's container. Nothing is taken from a compiled shader. The vertex fetches are templates: the SDK's loader
patches their format, stride and offset to the title's vertex layout, through the container's patch table. Prints
both containers as C initializers, then their disassembly.
"""

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import xdis.container as container  # noqa: E402
import xdis.container_emit as ce  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xasm import encode as enc  # noqa: E402

# Fetch destination swizzle, 3 bits a component (ucode.h:676-687).
DEST_XYZW = 0 | 1 << 3 | 2 << 6 | 3 << 9
# xenos.h:602-617; the loader patches the real format in.
FORMAT_32_32_32_32_FLOAT = 38
# Stream 0's vertex fetch constant, as in libxenon (libxenon:drivers/xenos/xe.c:1965), split as ucode.h:705-706 does.
VERTEX_FETCH_CONSTANT = 95
# The vertex index arrives in r0.x (xenia:src/xenia/gpu/spirv_shader_translator.cc:1510).
INDEX_REGISTER = 0
POSITION_REGISTER = 1
COLOR_REGISTER = 2
# ucode.h:1793-1811
EXPORT_POSITION = 62
EXPORT_INTERPOLATOR_0 = 0
EXPORT_COLOR_0 = 0
WRITE_XYZW = 0xF
# ucode.h:209-220
ALLOC_POSITION = 1
ALLOC_INTERPOLATORS = 2
ALLOC_COLORS = 2
# Two bits an instruction, bit 0 marking a fetch; one vertex cache bit a fetch, as gen_resolve_vs.py sets for one.
SEQUENCE_TWO_FETCHES = 0b0101
VERTEX_CACHE_TWO_FETCHES = 0b11
# A pixel shader writes the colour export in mode 2, as libxenon's set-up does (libxenon:drivers/xenos/xe.c:419).
PS_EXPORT_MODE_COLOR = 2
# sc_sample_cntl kCentroidsAndCenters (xenos.h:804-808), with libxenon's all-centres SQ_INTERPOLATOR_CNTL (xe.c:1400).
CONTEXT_MISC_CENTROIDS_AND_CENTERS = 2 << 2
# D3D's declaration usages, which libxenon's patcher matches on (libxenon:drivers/xenos/xe.h:159-169).
USAGE_POSITION = 0
USAGE_COLOR = 10
CREATOR = "xsdk"
# The pixel record's interpolator count, in the high half (xenos-toolchain/examples/author_passthrough.py).
PS_INTERPOLATOR_WORD = 1 << 16 | 7


def program_control(vs_num_reg=0, ps_num_reg=0, vs_resource=0, vs_export_count=0, ps_export_mode=0):
    """SQ_PROGRAM_CNTL, register and export counts minus one (xenia:src/xenia/gpu/registers.h:127-148)."""
    return vs_num_reg | ps_num_reg << 8 | vs_resource << 16 | vs_export_count << 20 | ps_export_mode << 27


def fetch(dest):
    return enc.build_vfetch_full(
        src=INDEX_REGISTER, src_swizzle=0, dest=dest, dest_swizzle=DEST_XYZW, must_be_one=1,
        const_index=VERTEX_FETCH_CONSTANT // 3, const_index_sel=VERTEX_FETCH_CONSTANT % 3,
        data_format=FORMAT_32_32_32_32_FLOAT, is_signed=False, is_normalized=False, stride=0, offset=0)


def export(dest, source):
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMax, vector_dest=dest, vector_write_mask=WRITE_XYZW,
        is_export=True, src1_reg=source, src2_reg=source, src3_is_temp=False)


def execute(address, count, end=False, sequence=0, vc_hi=0):
    opcode = xu.ControlFlowOpcode.kExecEnd if end else xu.ControlFlowOpcode.kExec
    return enc.build_cf_exec(opcode=opcode, address=address, count=count, sequence=sequence, vc_hi=vc_hi,
        is_predicate_clean=True, addressing_mode=0)


def pack(control_flow, instructions):
    if len(control_flow) % 2:
        control_flow.append((0, 0))
    dwords = []
    for i in range(0, len(control_flow), 2):
        dwords.extend(enc.pack_cf_pair(control_flow[i], control_flow[i + 1]))
    for instruction in instructions:
        dwords.extend(instruction)
    return dwords


def vertex_shader():
    # exec {fetch position, colour}; alloc position; exec {oPos}; alloc interpolators; exece {o0}; from slot 3.
    control_flow = [
        execute(3, 2, sequence=SEQUENCE_TWO_FETCHES, vc_hi=VERTEX_CACHE_TWO_FETCHES),
        enc.build_cf_alloc(alloc_type=ALLOC_POSITION),
        execute(5, 1),
        enc.build_cf_alloc(alloc_type=ALLOC_INTERPOLATORS),
        execute(6, 1, end=True),
    ]
    dwords = pack(control_flow, [
        fetch(POSITION_REGISTER),
        fetch(COLOR_REGISTER),
        export(EXPORT_POSITION, POSITION_REGISTER),
        export(EXPORT_INTERPOLATOR_0, COLOR_REGISTER),
    ])
    return ce.build_container(
        "vertex", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=program_control(vs_num_reg=COLOR_REGISTER, vs_resource=1, vs_export_count=0),
        context_misc=0,
        interpolators=[ce.Interpolator(EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        vertex_elements=[ce.VertexElement(3, USAGE_POSITION, 0), ce.VertexElement(4, USAGE_COLOR, 0)],
        creator=CREATOR)


def pixel_shader():
    # alloc colors; exece {oC0 = r0, interpolator 0}. The instruction is slot 1, after one control-flow pair.
    dwords = pack([enc.build_cf_alloc(alloc_type=ALLOC_COLORS), execute(1, 1, end=True)],
        [export(EXPORT_COLOR_0, 0)])
    return ce.build_container(
        "pixel", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=program_control(ps_num_reg=0, ps_export_mode=PS_EXPORT_MODE_COLOR),
        context_misc=CONTEXT_MISC_CENTROIDS_AND_CENTERS,
        interpolators=[ce.Interpolator(EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        ps_field18=PS_INTERPOLATOR_WORD, creator=CREATOR)


def print_array(name, blob):
    words = struct.unpack(f">{len(blob) // 4}I", blob)
    print(f"static const uint32_t {name}[] = {{")
    for i in range(0, len(words), 6):
        print("\t" + ", ".join(f"0x{w:08X}" for w in words[i:i + 6]) + ",")
    print("};")


def main():
    for name, blob in (("triangle_vertex_shader", vertex_shader()), ("triangle_pixel_shader", pixel_shader())):
        print_array(name, blob)
        parsed = container.Container(blob)
        print(f"{name}:", file=sys.stderr)
        print(xu.disassemble(parsed.ucode_dwords, parsed.shader_type), file=sys.stderr)


if __name__ == "__main__":
    main()
