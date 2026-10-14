#!/usr/bin/env python3
"""Generate milestone 36's pixel shaders for tests/probe_texel_rate.c (docs/scope/36-texel-rate.md): four samples a
texel apart, at coordinates each shader works out from the interpolated ones and c0, averaged by c1, laid out a way
each. The layouts are written in xost's assembly (xenos-toolchain/xasm/textasm.py, its grammar the disassembler's)
and assembled by it; one more is xost's own build of the same GLSL, compiled by glslang and lowered by xsc as
vulkan4xenon's shaders are. Prints the containers as C initializers, then their disassembly.
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import gen_triangle_shaders as tri
from gen_fill_shaders import print_array

XOST = tri.ROOT / "xenos-toolchain"

from xasm.program import XSDK_REGISTERS  # noqa: E402
from xasm.textasm import assemble_container  # noqa: E402
from xsc import spirv  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402

GLSLANG = XOST / "tools" / "glslang" / "bin" / "glslang.exe"

GLSL = """#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push { vec4 delta; vec4 scale; } pc;

void main()
{
	o_color = (texture(u_texture, v_uv) + texture(u_texture, v_uv + pc.delta.xz) +
		texture(u_texture, v_uv + pc.delta.zy) + texture(u_texture, v_uv + pc.delta.xy)) * pc.scale;
}
"""


def xost_build():
    """xost's container for GLSL, with its optimiser on, as vulkan4xenon's shaders are built."""
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "texel.frag"
        source.write_text(GLSL, newline="\n")
        output = Path(directory) / "texel.spv"
        subprocess.run([str(GLSLANG), "-V", str(source), "-o", str(output)], check=True, capture_output=True)
        blob, literals = lower_ex(spirv.Module.parse(output.read_bytes()), XSDK_REGISTERS, True)
    if literals:
        sys.exit(f"xost wants literal constants the probe doesn't set: {literals}")
    return blob


HEADER = """.pixel
.interpolator 0 texcoord
.sampler u_texture 0
"""

# xost's own sum after its fetches into a, b, c and d, a chain through r2, by c1, to the colour; with the wait bit on
# the first instruction reading a sample if asked.
def xost_average(a, b, c, d, serialize):
    wait = "serialize\n" if serialize else ""
    return f"""      alloc colors
      exece
{wait}   add r2, {a}, {b}
   add {a}, r2, {c}
   add r2, {a}, {d}
   mul oC0, r2, c1
"""


# The other layouts' sum, as a tree.
def average(a, b, c, d, serialize):
    wait = "serialize\n" if serialize else ""
    return f"""      alloc colors
      exece
{wait}   add {a}, {a}, {b}
   add {c}, {c}, {d}
   add {a}, {a}, {c}
   mul oC0, {a}, c1
"""


# The same coordinates each way: r0.xy, then + c0.xz, + c0.zy and + c0.xy, a texel across, down and both.
LAYOUTS = {
    # As xost lays the GLSL out now: each coordinate's add, then its fetch, in one exec, the three through r2.
    "xost": """      exec
   tfetch2D r1, r0.xy, tf0
   add r2, r0, c0.xzzw
   tfetch2D r3, r2.xy, tf0
   add r2, r0, c0.zyzw
   tfetch2D r4, r2.xy, tf0
   add r2, r0, c0
      exec
   tfetch2D r5, r2.xy, tf0
""" + "{average}",
    # The same order, each coordinate in a register of its own.
    "own": """      exec
   tfetch2D r4, r0.xy, tf0
   add r1, r0, c0.xzzw
   tfetch2D r5, r1.xy, tf0
   add r2, r0, c0.zyzw
   tfetch2D r6, r2.xy, tf0
   add r3, r0, c0
      exec
   tfetch2D r7, r3.xy, tf0
""" + "{average}",
    # The coordinates first, then the four fetches together, in an exec of their own.
    "together": """      exec
   add r1, r0, c0.xzzw
   add r2, r0, c0.zyzw
   add r3, r0, c0
      exec
   tfetch2D r4, r0.xy, tf0
   tfetch2D r5, r1.xy, tf0
   tfetch2D r6, r2.xy, tf0
   tfetch2D r7, r3.xy, tf0
""" + "{average}",
    # Two coordinates to a register, r1.xy and r1.zw, then the fetches together.
    "paired": """      exec
   add r1, r0.xyxy, c0.xzzy
   add r2, r0, c0
      exec
   tfetch2D r3, r0.xy, tf0
   tfetch2D r4, r1.xy, tf0
   tfetch2D r5, r1.zw, tf0
   tfetch2D r6, r2.xy, tf0
""" + "{average}",
}

# Each layout's four fetch destinations, in the order average sums them.
FETCHED = {"xost": ("r1", "r3", "r4", "r5"), "own": ("r4", "r5", "r6", "r7"), "together": ("r4", "r5", "r6", "r7"),
           "paired": ("r3", "r4", "r5", "r6")}

# Milestone 31's shape, which reached the texture units' 16 a clock: four fetches at the interpolated coordinates
# themselves, in an exec of their own (docs/HARDWARE.md, "The pixel rate, milestone 31").
REFERENCE = """      exec
   tfetch2D r1, r0.xy, tf0
   tfetch2D r2, r0.xy, tf0
   tfetch2D r3, r0.xy, tf0
   tfetch2D r4, r0.xy, tf0
""" + average("r1", "r2", "r3", "r4", False)


def hand_built():
    shaders = [("texel_reference_shader", assemble_container(HEADER + REFERENCE, "pixel", XSDK_REGISTERS))]
    for name, text in LAYOUTS.items():
        for serialize in (False, True):
            tail = (xost_average if name == "xost" else average)(*FETCHED[name], serialize)
            body = text.replace("{average}", tail)
            suffix = "_wait" if serialize else ""
            shaders.append((f"texel_{name}{suffix}_shader",
                            assemble_container(HEADER + body, "pixel", XSDK_REGISTERS)))
    return shaders


def main():
    shaders = hand_built()
    built = xost_build()
    laid_out = dict(shaders)["texel_xost_shader"]
    same = tri.container.Container(built).ucode_dwords == tri.container.Container(laid_out).ucode_dwords
    print(f"xost's build {'matches' if same else 'differs from'} the xost layout", file=sys.stderr)
    shaders.append(("texel_xost_build_shader", built))
    print("#ifndef XSDK_TEXEL_SHADERS_H\n#define XSDK_TEXEL_SHADERS_H\n\n#include <stdint.h>\n")
    print("// Generated by tools/gen_texel_shaders.py.")
    for name, blob in shaders:
        print_array(name, blob)
        parsed = tri.container.Container(blob)
        print(f"{name}:", file=sys.stderr)
        print(tri.xu.disassemble(parsed.ucode_dwords, parsed.shader_type), file=sys.stderr)
    print("\n#endif")


if __name__ == "__main__":
    main()
