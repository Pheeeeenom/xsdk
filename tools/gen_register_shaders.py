#!/usr/bin/env python3
"""Generate milestone 24's shader containers, which use 18 and 64 registers.

A pixel shader may use only its share of the shader unit's register split, and one past it hangs the GPU; xsdk's
set-up leaves each kind of shader 64 (src/gpu_setup.c, SCOPE.md milestone 24). Each shader here fills every register
it has from r1 up with a constant of its own, all held at once, then subtracts each register's constant from it and
adds up the squares, so a register missing or shared turns the sum from 0. The sum, scaled by a gain past any colour,
goes into the colour on top of what the shader passes on, so one wrong register washes it out:
- the pixel shaders, with 18 and 64 registers: r0 is the interpolated colour, c0 the gain and c1 up the fills;
- the vertex shader, with 64: r0 is the vertex index and r1 the position it fetches, placed with c0 and c1 as
  tools/gen_constant_shaders.py places its quad, after the check; c2 is the colour, c3 the gain and c4 up the fills
  of r2 up.

Packed as in tools/gen_triangle_shaders.py, from Xenia's instruction definitions (xenia:src/xenia/gpu/ucode.h) through
xost's encoders and container emitter. The pixel shaders are also written in xost's assembly, and the generator stops
unless xost's text assembler gives the same microcode. Prints a C header with the three containers, then their
disassembly on stderr.
"""

import struct
import sys

import gen_triangle_shaders as tri
from gen_triangle_shaders import ce, container, enc, xu
from xasm import textasm

# A control-flow exec holds up to 6 instructions (xenos-toolchain/xasm/program.py, MAX_EXEC_COUNT).
EXEC_MAX = 6
SEQUENCE_ONE_FETCH = 0b01
VERTEX_CACHE_ONE_FETCH = 0b1
PIXEL_COLOR_REGISTER = 0
PIXEL_GAIN_CONSTANT = 0
VERTEX_POSITION_REGISTER = 1
VERTEX_SCALE_CONSTANT = 0
VERTEX_OFFSET_CONSTANT = 1
VERTEX_COLOR_CONSTANT = 2
VERTEX_GAIN_CONSTANT = 3
# The vertex shader's fills start at r2, from c4.
VERTEX_FILL_CONSTANT_OFFSET = 2


# An unused src3 stays a temporary, as xost's assembler encodes vector-only instructions (xasm/textasm.py:45-63).
def fill(register, constant):
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMax, vector_dest=register, vector_write_mask=tri.WRITE_XYZW,
        src1_reg=constant, src1_is_temp=False, src2_reg=constant, src2_is_temp=False, src3_is_temp=True)


def subtract(register, constant):
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kAdd, vector_dest=register, vector_write_mask=tri.WRITE_XYZW,
        src1_reg=register, src1_is_temp=True, src2_reg=constant, src2_is_temp=False, src2_negate=True,
        src3_is_temp=True)


def square(register):
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMul, vector_dest=register, vector_write_mask=tri.WRITE_XYZW,
        src1_reg=register, src2_reg=register, src3_is_temp=True)


def add_square(total, register):
    return enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMad, vector_dest=total, vector_write_mask=tri.WRITE_XYZW,
        src1_reg=register, src2_reg=register, src3_reg=total)


def check_registers(first, last, constant_offset):
    """Fills first..last from their constants, then leaves in last the sum of each one's squared error."""
    instructions = [fill(r, r + constant_offset) for r in range(first, last + 1)]
    instructions += [subtract(last, last + constant_offset), square(last)]
    for r in range(last - 1, first - 1, -1):
        instructions += [subtract(r, r + constant_offset), add_square(last, r)]
    return instructions


def check_text(first, last, constant_offset):
    lines = [f"max r{r}, c{r + constant_offset}, c{r + constant_offset}" for r in range(first, last + 1)]
    lines += [f"add r{last}, r{last}, -c{last + constant_offset}", f"mul r{last}, r{last}, r{last}"]
    for r in range(last - 1, first - 1, -1):
        lines += [f"add r{r}, r{r}, -c{r + constant_offset}", f"mad r{last}, r{r}, r{r}, r{last}"]
    return lines


def program(parts):
    """Control flow and instructions from parts: ("exec", instructions[, fetches]) or ("alloc", type). Each exec part
    runs in execs of up to 6, the last of the program ending it. Returns the dwords and the first instruction's slot."""
    execs = []
    for part in parts:
        if part[0] == "alloc":
            execs.append(part)
            continue
        instructions = part[1]
        for i in range(0, len(instructions), EXEC_MAX):
            execs.append(("exec", instructions[i:i + EXEC_MAX], part[2] if len(part) > 2 and i == 0 else 0))
    control_count = len(execs) + len(execs) % 2
    slot = first_slot = control_count // 2
    last_exec = max(i for i, e in enumerate(execs) if e[0] == "exec")
    control_flow = []
    instructions = []
    for i, part in enumerate(execs):
        if part[0] == "alloc":
            control_flow.append(enc.build_cf_alloc(alloc_type=part[1]))
            continue
        fetches = part[2]
        control_flow.append(tri.execute(slot, len(part[1]), end=i == last_exec,
            sequence=SEQUENCE_ONE_FETCH if fetches else 0, vc_hi=VERTEX_CACHE_ONE_FETCH if fetches else 0))
        instructions += part[1]
        slot += len(part[1])
    return tri.pack(control_flow, instructions), first_slot


def program_text(parts):
    lines = []
    for part in parts:
        if part[0] == "alloc":
            lines.append("alloc colors")
            continue
        for i in range(0, len(part[1]), EXEC_MAX):
            lines.append("exec")
            lines += part[1][i:i + EXEC_MAX]
    last_exec = max(i for i, line in enumerate(lines) if line == "exec")
    lines[last_exec] = "exece"
    return "\n".join(lines) + "\n"


def pixel_shader(register_count):
    last = register_count - 1
    export = enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMad, vector_dest=tri.EXPORT_COLOR_0,
        vector_write_mask=tri.WRITE_XYZW, is_export=True, src1_reg=last, src1_is_temp=True,
        src2_reg=PIXEL_GAIN_CONSTANT, src2_is_temp=False, src3_reg=PIXEL_COLOR_REGISTER, src3_is_temp=True)
    dwords, _ = program([("exec", check_registers(1, last, 0)), ("alloc", tri.ALLOC_COLORS), ("exec", [export])])
    ucode = struct.pack(f">{len(dwords)}I", *dwords)
    text = program_text([("exec", check_text(1, last, 0)), ("alloc",),
        ("exec", [f"mad oC0, r{last}, c{PIXEL_GAIN_CONSTANT}, r{PIXEL_COLOR_REGISTER}"])])
    if textasm.assemble_text(text, "pixel") != ucode:
        sys.exit(f"gen_register_shaders: the {register_count}-register pixel shader differs from xost's assembly of it")
    return ce.build_container(
        "pixel", ucode, constants=[],
        program_control=tri.program_control(ps_num_reg=last, ps_export_mode=tri.PS_EXPORT_MODE_COLOR),
        context_misc=tri.CONTEXT_MISC_CENTROIDS_AND_CENTERS,
        interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        ps_field18=tri.PS_INTERPOLATOR_WORD, creator=tri.CREATOR)


def vertex_shader(register_count):
    last = register_count - 1
    first = VERTEX_POSITION_REGISTER + 1
    place = enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMad, vector_dest=tri.EXPORT_POSITION,
        vector_write_mask=tri.WRITE_XYZW, is_export=True, src1_reg=VERTEX_POSITION_REGISTER, src1_is_temp=True,
        src2_reg=VERTEX_SCALE_CONSTANT, src2_is_temp=False, src3_reg=VERTEX_OFFSET_CONSTANT, src3_is_temp=False)
    color = enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMad, vector_dest=tri.EXPORT_INTERPOLATOR_0,
        vector_write_mask=tri.WRITE_XYZW, is_export=True, src1_reg=last, src1_is_temp=True,
        src2_reg=VERTEX_GAIN_CONSTANT, src2_is_temp=False, src3_reg=VERTEX_COLOR_CONSTANT, src3_is_temp=False)
    dwords, fetch_slot = program([
        ("exec", [tri.fetch(VERTEX_POSITION_REGISTER)], True),
        ("exec", check_registers(first, last, VERTEX_FILL_CONSTANT_OFFSET)),
        ("alloc", tri.ALLOC_POSITION),
        ("exec", [place]),
        ("alloc", tri.ALLOC_INTERPOLATORS),
        ("exec", [color]),
    ])
    return ce.build_container(
        "vertex", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(vs_num_reg=last, vs_resource=1, vs_export_count=0),
        context_misc=0,
        interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        vertex_elements=[ce.VertexElement(fetch_slot, tri.USAGE_POSITION, 0)],
        creator=tri.CREATOR)


def main():
    print("#ifndef XSDK_REGISTER_SHADERS_H\n#define XSDK_REGISTER_SHADERS_H\n\n#include <stdint.h>\n")
    print("// Generated by tools/gen_register_shaders.py.")
    for name, blob in (("register_pixel_shader_18", pixel_shader(18)), ("register_pixel_shader_64", pixel_shader(64)),
            ("register_vertex_shader_64", vertex_shader(64))):
        tri.print_array(name, blob)
        parsed = container.Container(blob)
        print(f"{name}:", file=sys.stderr)
        print(xu.disassemble(parsed.ucode_dwords, parsed.shader_type), file=sys.stderr)
    print("\n#endif")


if __name__ == "__main__":
    main()
