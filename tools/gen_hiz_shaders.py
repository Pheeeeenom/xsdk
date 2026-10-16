#!/usr/bin/env python3
"""Generate milestone 30's pixel shaders, for timing hidden pixels and for pixels a shader kills, and one writing depth.

- The long pixel shader runs 64 multiply-adds, r0 = r0 * c0 + r0 with c0 0, on the interpolated colour and writes it,
  so a pixel costs what a long shader's does and keeps its colour (tests/probe_hiz.c, tests/test_hiz.c).
- The killing pixel shader kills the pixel if c1 > c2 in any component, else writes the interpolated colour; it showed
  that killed pixels leave the hierarchical buffer alone (results/2026-10-13-milestone-30/probe_hiz_discovery1.c).
- The depth pixel shader writes the interpolated colour, and c0.x as the pixel's depth through oDepth's x, as Xenia
  takes it (xenia:src/xenia/gpu/shader_translator.cc:1337-1338, dxbc_shader_translator.cc:1591-1594)
  (tests/probe_depth_export.c, tests/test_hiz.c).

Packed as in tools/gen_register_shaders.py, from Xenia's instruction definitions (xenia:src/xenia/gpu/ucode.h) through
xost's encoders and container emitter, and the generator stops unless xost's text assembler gives the same microcode.
Prints a C header with both containers, then their disassembly on stderr.
"""

import struct
import sys

import gen_register_shaders as reg
import gen_triangle_shaders as tri
from gen_triangle_shaders import ce, container, enc, xu
from xasm import textasm

LONG_MULTIPLY_ADDS = 64
COLOR_REGISTER = 0
KILL_REGISTER = 1
ZERO_CONSTANT = 0
KILL_LEFT_CONSTANT = 1
KILL_RIGHT_CONSTANT = 2
DEPTH_CONSTANT = 0
WRITE_X = 0b0001
# ps_export_mode 3 exports depth too: with libxenon's 2 the console drew nothing of the depth shader (docs/HARDWARE.md).
PS_EXPORT_MODE_COLOR_AND_DEPTH = 3


def multiply_add():
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMad, vector_dest=COLOR_REGISTER,
        vector_write_mask=tri.WRITE_XYZW, src1_reg=COLOR_REGISTER, src1_is_temp=True, src2_reg=ZERO_CONSTANT,
        src2_is_temp=False, src3_reg=COLOR_REGISTER, src3_is_temp=True)


# An unused src3 stays a temporary, as xost's assembler encodes vector-only instructions (xasm/textasm.py:45-63).
def kill():
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kKillGt, vector_dest=KILL_REGISTER,
        vector_write_mask=tri.WRITE_XYZW, src1_reg=KILL_LEFT_CONSTANT, src1_is_temp=False,
        src2_reg=KILL_RIGHT_CONSTANT, src2_is_temp=False, src3_is_temp=True)


def export_color():
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMax, vector_dest=tri.EXPORT_COLOR_0,
        vector_write_mask=tri.WRITE_XYZW, is_export=True, src1_reg=COLOR_REGISTER, src2_reg=COLOR_REGISTER,
        src3_is_temp=True)


def export_depth():
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMax, vector_dest=int(xu.ExportRegister.kPSDepth),
        vector_write_mask=WRITE_X, is_export=True, src1_reg=DEPTH_CONSTANT, src1_is_temp=False,
        src2_reg=DEPTH_CONSTANT, src2_is_temp=False, src3_is_temp=True)


def pixel_container(name, parts, text_parts, last_register, export_mode=tri.PS_EXPORT_MODE_COLOR):
    dwords, _ = reg.program(parts)
    ucode = struct.pack(f">{len(dwords)}I", *dwords)
    if textasm.assemble_text(reg.program_text(text_parts), "pixel") != ucode:
        sys.exit(f"gen_hiz_shaders: the {name} differs from xost's assembly of it")
    return ce.build_container(
        "pixel", ucode, constants=[],
        program_control=tri.program_control(ps_num_reg=last_register, ps_export_mode=export_mode),
        context_misc=tri.CONTEXT_MISC_CENTROIDS_AND_CENTERS,
        interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        ps_field18=tri.PS_INTERPOLATOR_WORD, creator=tri.CREATOR)


def long_pixel_shader():
    export_text = [f"max oC0, r{COLOR_REGISTER}, r{COLOR_REGISTER}"]
    return pixel_container("long pixel shader",
        [("exec", [multiply_add()] * LONG_MULTIPLY_ADDS), ("alloc", tri.ALLOC_COLORS), ("exec", [export_color()])],
        [("exec", [f"mad r{COLOR_REGISTER}, r{COLOR_REGISTER}, c{ZERO_CONSTANT}, r{COLOR_REGISTER}"] *
            LONG_MULTIPLY_ADDS), ("alloc",), ("exec", export_text)],
        COLOR_REGISTER)


def kill_pixel_shader():
    export_text = [f"max oC0, r{COLOR_REGISTER}, r{COLOR_REGISTER}"]
    return pixel_container("killing pixel shader",
        [("exec", [kill()]), ("alloc", tri.ALLOC_COLORS), ("exec", [export_color()])],
        [("exec", [f"kill_gt r{KILL_REGISTER}, c{KILL_LEFT_CONSTANT}, c{KILL_RIGHT_CONSTANT}"]), ("alloc",),
            ("exec", export_text)],
        KILL_REGISTER)


def depth_pixel_shader():
    export_text = [f"max oC0, r{COLOR_REGISTER}, r{COLOR_REGISTER}",
        f"max oDepth.x___, c{DEPTH_CONSTANT}, c{DEPTH_CONSTANT}"]
    parts = [("alloc", tri.ALLOC_COLORS), ("exec", [export_color(), export_depth()])]
    return pixel_container("depth pixel shader", parts, [("alloc",), ("exec", export_text)], COLOR_REGISTER,
        PS_EXPORT_MODE_COLOR_AND_DEPTH)


def main():
    print("#ifndef XSDK_HIZ_SHADERS_H\n#define XSDK_HIZ_SHADERS_H\n\n#include <stdint.h>\n")
    print("// Generated by tools/gen_hiz_shaders.py.")
    shaders = (("long_pixel_shader", long_pixel_shader()), ("kill_pixel_shader", kill_pixel_shader()),
        ("depth_pixel_shader", depth_pixel_shader()))
    for name, blob in shaders:
        tri.print_array(name, blob)
        parsed = container.Container(blob)
        print(f"{name}:", file=sys.stderr)
        print(xu.disassemble(parsed.ucode_dwords, parsed.shader_type), file=sys.stderr)
    print("\n#endif")


if __name__ == "__main__":
    main()
