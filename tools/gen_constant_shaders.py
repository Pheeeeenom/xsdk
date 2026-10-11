#!/usr/bin/env python3
"""Generate milestone 11's shader containers, which draw only from constants.

The vertex shader places a unit quad with two float constants (position * c0 + c1) and passes a third, c2, on as the
colour. The pixel shader adds its own constant c0 to that colour, or c1 instead when boolean constant 128 is set, then
adds c2 once a pass of loop 0. So each channel of a quad's colour shows one kind of constant: red the vertex shader's,
green the pixel shader's and the boolean, blue the loop.

Both are packed as in tools/gen_triangle_shaders.py, from Xenia's instruction definitions (xenia:src/xenia/gpu/ucode.h)
through xost's encoders and container emitter. The loop's addresses follow xost's program builder: LOOP_START skips to
just past LOOP_END, which jumps back to the body (xenos-toolchain/xasm/program.py:239-241). The pixel shader is also
written in xost's assembly, and the generator stops unless xost's text assembler gives the same microcode, so the hand
layout and the assembler check each other. Prints a C header with both containers, then their disassembly on stderr.
"""

import struct
import sys

import gen_triangle_shaders as tri
from gen_triangle_shaders import ce, container, enc, xu
from xasm import textasm

POSITION_REGISTER = 1
SCALE_CONSTANT = 0
OFFSET_CONSTANT = 1
COLOR_CONSTANT = 2
SEQUENCE_ONE_FETCH = 0b01
VERTEX_CACHE_ONE_FETCH = 0b1

INTERPOLATED_REGISTER = 0
SUM_REGISTER = 1
BASE_CONSTANT = 0
BOOL_BASE_CONSTANT = 1
STEP_CONSTANT = 2
# The pixel shader's first boolean in libxenon's split (libxenon:drivers/xenos/xe.c:2051-2064).
BOOL_CONSTANT = 128
LOOP_CONSTANT = 0
PIXEL_SHADER_TEXT = """
      exec
   add r1, r0, c0
      cexec b128
   add r1, r0, c1
      loop i0, L1
label L0
      exec
   add r1, r1, c2
      endloop i0, L0
label L1
      alloc colors
      exece
   max oC0, r1, r1
"""


def vertex_shader():
    # exec {fetch position}; alloc position; exec {oPos = position * c0 + c1}; alloc interpolators; exece {o0 = c2}.
    control_flow = [
        tri.execute(3, 1, sequence=SEQUENCE_ONE_FETCH, vc_hi=VERTEX_CACHE_ONE_FETCH),
        enc.build_cf_alloc(alloc_type=tri.ALLOC_POSITION),
        tri.execute(4, 1),
        enc.build_cf_alloc(alloc_type=tri.ALLOC_INTERPOLATORS),
        tri.execute(5, 1, end=True),
    ]
    place = enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMad, vector_dest=tri.EXPORT_POSITION,
        vector_write_mask=tri.WRITE_XYZW, is_export=True, src1_reg=POSITION_REGISTER, src1_is_temp=True,
        src2_reg=SCALE_CONSTANT, src2_is_temp=False, src3_reg=OFFSET_CONSTANT, src3_is_temp=False)
    # Two constant operands, the most an instruction reads, and an unused src3 left a temporary as below.
    color = enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMax, vector_dest=tri.EXPORT_INTERPOLATOR_0,
        vector_write_mask=tri.WRITE_XYZW, is_export=True, src1_reg=COLOR_CONSTANT, src1_is_temp=False,
        src2_reg=COLOR_CONSTANT, src2_is_temp=False, src3_is_temp=True)
    dwords = tri.pack(control_flow, [tri.fetch(POSITION_REGISTER), place, color])
    return ce.build_container(
        "vertex", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(vs_num_reg=POSITION_REGISTER, vs_resource=1, vs_export_count=0),
        context_misc=0,
        interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        vertex_elements=[ce.VertexElement(3, tri.USAGE_POSITION, 0)],
        creator=tri.CREATOR)


# An unused src3 stays a temporary, as xost's assembler encodes vector-only instructions (xasm/textasm.py:45-63).
def add(dest, temp, constant):
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kAdd, vector_dest=dest, vector_write_mask=tri.WRITE_XYZW,
        src1_reg=temp, src1_is_temp=True, src2_reg=constant, src2_is_temp=False, src3_is_temp=True)


def export_color(source):
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMax, vector_dest=tri.EXPORT_COLOR_0,
        vector_write_mask=tri.WRITE_XYZW, is_export=True, src1_reg=source, src2_reg=source, src3_is_temp=True)


def pixel_shader():
    # 0 exec {r1 = r0 + c0}; 1 cexec b128 {r1 = r0 + c1}; 2 loop i0, skip to 5; 3 exec {r1 += c2}; 4 endloop i0, back to
    # 3; 5 alloc colors; 6 exece {oC0 = r1}; then a pad, so four control-flow pairs and the instructions from slot 4.
    control_flow = [
        tri.execute(4, 1),
        enc.build_cf_cond_exec(opcode=xu.ControlFlowOpcode.kCondExecPredClean, address=5, count=1,
            bool_address=BOOL_CONSTANT, condition=True, addressing_mode=0),
        enc.build_cf_loop_start(opcode=xu.ControlFlowOpcode.kLoopStart, address=5, is_repeat=False,
            loop_id=LOOP_CONSTANT, addressing_mode=0),
        tri.execute(6, 1),
        enc.build_cf_loop_end(opcode=xu.ControlFlowOpcode.kLoopEnd, address=3, loop_id=LOOP_CONSTANT,
            is_predicated_break=False, condition=False, addressing_mode=0),
        enc.build_cf_alloc(alloc_type=tri.ALLOC_COLORS),
        tri.execute(7, 1, end=True),
    ]
    dwords = tri.pack(control_flow, [
        add(SUM_REGISTER, INTERPOLATED_REGISTER, BASE_CONSTANT),
        add(SUM_REGISTER, INTERPOLATED_REGISTER, BOOL_BASE_CONSTANT),
        add(SUM_REGISTER, SUM_REGISTER, STEP_CONSTANT),
        export_color(SUM_REGISTER),
    ])
    ucode = struct.pack(f">{len(dwords)}I", *dwords)
    if textasm.assemble_text(PIXEL_SHADER_TEXT, "pixel") != ucode:
        sys.exit("gen_constant_shaders: the hand-packed pixel shader differs from xost's assembly of it")
    return ce.build_container(
        "pixel", ucode, constants=[],
        program_control=tri.program_control(ps_num_reg=SUM_REGISTER, ps_export_mode=tri.PS_EXPORT_MODE_COLOR),
        context_misc=tri.CONTEXT_MISC_CENTROIDS_AND_CENTERS,
        interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        ps_field18=tri.PS_INTERPOLATOR_WORD, creator=tri.CREATOR)


def main():
    print("#ifndef XSDK_CONSTANT_SHADERS_H\n#define XSDK_CONSTANT_SHADERS_H\n\n#include <stdint.h>\n")
    print("// Generated by tools/gen_constant_shaders.py.")
    for name, blob in (("constant_vertex_shader", vertex_shader()), ("constant_pixel_shader", pixel_shader())):
        tri.print_array(name, blob)
        parsed = container.Container(blob)
        print(f"{name}:", file=sys.stderr)
        print(xu.disassemble(parsed.ucode_dwords, parsed.shader_type), file=sys.stderr)
    print("\n#endif")


if __name__ == "__main__":
    main()
