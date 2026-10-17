#!/usr/bin/env python3
"""gen_shader_cost_probe.py <glsl_pack.c> <out.h>: the programs for probe_shader_cost, milestone 45's console probe of
what xost's code costs the GPU (docs/scope/45-xost-code-quality.md, step 1). From a pack Aurora's
tools/build_glsl_pack.py wrote, each program's SPIR-V, which the probe compiles on the console, and a pixel shader's
interpolators, one past the highest Location it reads; then the probe's own shaders, packed as
tools/gen_constant_shaders.py packs its own: for each count from 1 to 16, a vertex shader that passes a position on and
exports that many interpolators, interpolator n = position * c[n] + c[16 + n], with the constants it's drawn with, so
a pixel shader is drawn with the interpolators it reads and no more, as all 16 hold every pixel shader to a pixel a
clock; and a pixel shader that writes interpolator 0, for the vertex shaders' draws. The generator stops unless xost's
simulator (xenos-toolchain/xdis/simulate.py) runs each vertex shader to the exports the constants say.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import gen_triangle_shaders as tri  # noqa: E402
import gen_xsc_probe as pack  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from gen_triangle_shaders import ce, enc, xu  # noqa: E402

INTERPOLATORS = 16
POSITION_REGISTER = 1
SEQUENCE_ONE_FETCH = 0b01
VERTEX_CACHE_ONE_FETCH = 0b1
# An exec holds up to 6 instructions, 2 sequence bits each (xenia:src/xenia/gpu/ucode.h:239-259).
EXEC_MOST = 6
# The control flow below: fetch, alloc, position, alloc, then the interpolators' execs, padded to whole pairs.
CONTROL_FLOW_PAIRS = 4
CHECK_POSITION = (-0.75, 0.5, 0.5, 1.0)
SPV_DECORATION_LOCATION = 30
SPV_OP_VARIABLE = 59
SPV_STORAGE_INPUT = 1


def interpolant_constants():
    """Scales then offsets, so each component of each interpolator varies across the screen by a scale of its own."""
    scales = [[0.25 + 0.015625 * (4 * n + i) for i in range(4)] for n in range(INTERPOLATORS)]
    offsets = [[0.75 - 0.0078125 * (4 * n + i) for i in range(4)] for n in range(INTERPOLATORS)]
    return scales + offsets


def vertex_shader(count):
    first = CONTROL_FLOW_PAIRS
    interpolate = first + 2
    control_flow = [
        tri.execute(first, 1, sequence=SEQUENCE_ONE_FETCH, vc_hi=VERTEX_CACHE_ONE_FETCH),
        enc.build_cf_alloc(alloc_type=tri.ALLOC_POSITION),
        tri.execute(first + 1, 1),
        enc.build_cf_alloc(alloc_type=tri.ALLOC_INTERPOLATORS),
    ]
    for start in range(0, count, EXEC_MOST):
        size = min(EXEC_MOST, count - start)
        control_flow.append(tri.execute(interpolate + start, size, end=start + size == count))
    assert len(control_flow) <= 2 * CONTROL_FLOW_PAIRS
    control_flow += [(0, 0)] * (2 * CONTROL_FLOW_PAIRS - len(control_flow))
    interpolators = [enc.build_alu(vector_opcode=xu.AluVectorOpcode.kMad, vector_dest=tri.EXPORT_INTERPOLATOR_0 + n,
        vector_write_mask=tri.WRITE_XYZW, is_export=True, src1_reg=POSITION_REGISTER, src1_is_temp=True, src2_reg=n,
        src2_is_temp=False, src3_reg=INTERPOLATORS + n, src3_is_temp=False) for n in range(count)]
    dwords = tri.pack(control_flow, [
        tri.fetch(POSITION_REGISTER),
        tri.export(tri.EXPORT_POSITION, POSITION_REGISTER),
    ] + interpolators)
    return ce.build_container(
        "vertex", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(vs_num_reg=POSITION_REGISTER, vs_resource=1, vs_export_count=count - 1),
        context_misc=0,
        interpolators=[ce.Interpolator(n, ce.USAGE_TEXCOORD, n) for n in range(count)],
        vertex_elements=[ce.VertexElement(first, tri.USAGE_POSITION, 0)],
        creator=tri.CREATOR)


def check_vertex_shader(blob, constants, count):
    """Runs the vertex shader on one position; stops unless it exports the position and each interpolator."""
    result = simulate.run(blob, {0: [0.0] * 4}, dict(enumerate(constants)),
                          vertex_fetch=lambda n, fetch, index: list(CHECK_POSITION))
    want = {tri.EXPORT_POSITION: [simulate.single(p) for p in CHECK_POSITION]}
    for n in range(count):
        want[n] = [simulate.single(simulate.single(p * s) + o)
                   for p, s, o in zip(CHECK_POSITION, constants[n], constants[INTERPOLATORS + n])]
    for reg, values in want.items():
        got = result.exports.get(reg)
        if got is None or any(abs(g - w) > 1e-6 for g, w in zip(got, values)):
            sys.exit(f"gen_shader_cost_probe: export {reg} is {got}, not {values}")


def interpolators_read(words):
    """One past the highest Location a pixel shader reads, at least 1: xost reads Location n from interpolator n
    (src/xsc_resolve.c, interp_in)."""
    m = spirv.Module.parse(struct.pack(f"<{len(words)}I", *words))
    location = {target: args[0] for target, decorations in m.decorations.items()
                for decoration, args in decorations if decoration == SPV_DECORATION_LOCATION}
    read = [location[i.result_id] for i in m.instrs
            if i.opcode == SPV_OP_VARIABLE and i.operands[0] == SPV_STORAGE_INPUT and i.result_id in location]
    return max(read, default=0) + 1


def words_of(blob):
    return list(struct.unpack(f">{len(blob) // 4}I", blob))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pack_path, out = Path(sys.argv[1]), Path(sys.argv[2])
    constants = interpolant_constants()
    lines = [f"// Generated by tools/gen_shader_cost_probe.py from {pack_path.name}; don't edit.", ""]
    for count in range(1, INTERPOLATORS + 1):
        vertex = vertex_shader(count)
        check_vertex_shader(vertex, constants, count)
        lines += [f"static const uint32_t probe_vertex_shader_{count}[] = {{", pack.word_lines(words_of(vertex)),
                  "};", ""]
    lines += ["// The probe's vertex shader for 1 to 16 interpolators.",
              "static const ProbeShader probe_vertex_shaders[] = {"]
    lines += [f"\t{{ probe_vertex_shader_{n}, sizeof probe_vertex_shader_{n} }}," for n in range(1, INTERPOLATORS + 1)]
    lines += ["};", "", "static const uint32_t probe_pixel_shader[] = {", pack.word_lines(words_of(tri.pixel_shader())),
              "};", ""]
    lines += ["static const float interpolant_constants[][4] = {"]
    lines += ["\t{ " + ", ".join(f"{v}f" for v in c) + " }," for c in constants] + ["};", ""]
    table = []
    for i, (name, words, _, _, _) in enumerate(pack.programs(pack_path.read_text(encoding="utf-8"))):
        lines += [f"static const uint32_t spirv_{i}[] = {{", pack.word_lines(words), "};", ""]
        hash_name, stage = name.removeprefix("glsl_").rsplit("_", 1)
        pixel = stage == "frag"
        table.append(f"\t{{ \"{hash_name}.{stage}\", {'true' if pixel else 'false'}, spirv_{i}, {len(words)}, "
                     f"{interpolators_read(words) if pixel else 0} }},")
    lines += ["static const ProbeProgram programs[] = {"] + table + ["};", ""]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
