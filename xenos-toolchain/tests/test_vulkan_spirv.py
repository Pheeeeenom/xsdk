#!/usr/bin/env python3
"""xsc lowers Vulkan-flavoured SPIR-V (glslang -V), as vulkan4xenon hands it.

- vk_triangle.{vert,frag} are compiled both ways in xsc/testdata: .spv with -V
  and .gl.spv with -G --target-env opengl. The containers must be identical, and
  the oracle must accept them.
- vk_blocks.vert and vk_matrices.vert read uniform and push constant blocks.
  Their registers must be where resolve.py's convention puts them, and running
  the program on the blocks' raw std140 bytes must give the right position and
  colour. The oracle can't see values, so a small simulator of the ALU checks
  them, as tests/test_matrix.py does for fragment shaders.
- vk_samplers.frag must take fetch constants in (set, binding) order.
- vk_three_inputs.vert must fetch into r0 last, as every fetch reads the vertex index there.
- vk_block_literals.vert must give its uniform block whole groups of 4, the literals after them.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xsc.spirv as spirv  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xsc.lower import lower, lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402
from test_xsc_lower import TD, oracle_hlsl  # noqa: E402

V = xu.AluVectorOpcode
OPOS = int(xu.ExportRegister.kVSPosition)


def module(name):
    return spirv.Module.parse((TD / name).read_bytes())


def lowered(path):
    return lower(spirv.Module.parse(path.read_bytes()))


def _src(op, n, regs, consts):
    reg = op.src_reg(n)
    val = list(regs.get(reg & 0x3F, [0.0] * 4)) if op.src_is_temp(n) else list(consts.get(reg, [0.0] * 4))
    val = [val[xu.AluInstruction.GetSwizzledComponentIndex(op.src_swizzle(n), i)] for i in range(4)]
    return [-x for x in val] if op.src_negate(n) else val


def run_vertex(blob, inputs, consts):
    """Runs a vertex program's vector ALU; returns its exports by register (62 is the position)."""
    regs = {k: list(v) for k, v in inputs.items()}
    exports = {}
    c = container.Container(blob)
    for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if rec.kind != "alu":
            continue
        op = xu.AluInstruction(*rec.raw)
        mask = op.GetVectorOpResultWriteMask()
        if not mask:
            continue
        s1, s2 = _src(op, 1, regs, consts), _src(op, 2, regs, consts)
        vo = op.vector_opcode()
        if vo == V.kMul:
            r = [s1[i] * s2[i] for i in range(4)]
        elif vo == V.kAdd:
            r = [s1[i] + s2[i] for i in range(4)]
        elif vo == V.kMax:
            r = [max(s1[i], s2[i]) for i in range(4)]
        elif vo == V.kMad:
            s3 = _src(op, 3, regs, consts)
            r = [s1[i] * s2[i] + s3[i] for i in range(4)]
        elif vo == V.kDp4:
            r = [sum(s1[i] * s2[i] for i in range(4))] * 4
        else:
            raise SystemExit(f"test_vulkan_spirv: unhandled vector op {vo}")
        dst = exports if op.is_export() else regs
        out = dst.setdefault(op.vector_dest(), [0.0] * 4)
        for i in range(4):
            if mask & (1 << i):
                out[i] = r[i]
    return exports


def registers(raw, first):
    """A block's raw bytes as the float constant registers the layer copies them into."""
    floats = struct.unpack(f"<{len(raw) // 4}f", raw)
    return {first + i: list(floats[4 * i:4 * i + 4]) for i in range(len(floats) // 4)}


def columns(m):
    """A row-indexed 4x4 matrix as std140's column-major bytes."""
    return struct.pack("<16f", *(m[r][c] for c in range(4) for r in range(4)))


def mat_vec(m, v):
    return [sum(m[r][c] * v[c] for c in range(4)) for r in range(4)]


def vec_mat(v, m):
    return [sum(v[r] * m[r][c] for r in range(4)) for c in range(4)]


def mat_mat(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def close(a, b):
    return all(abs(x - y) <= 1e-4 for x, y in zip(a, b))


MVP = [[1.0, 2.0, 0.0, 3.0], [0.0, 1.0, -1.0, 0.5], [2.0, 0.0, 1.0, -1.0], [0.0, 0.5, 0.0, 1.0]]
VIEW = [[0.0, -1.0, 0.0, 2.0], [1.0, 0.0, 0.0, -3.0], [0.0, 0.0, 1.0, 0.25], [0.0, 0.0, 0.0, 1.0]]
PROJECTION = [[1.5, 0.0, 0.0, 0.0], [0.0, 2.0, 0.0, 0.0], [0.0, 0.0, -1.0, -0.2], [0.0, 0.0, -1.0, 0.0]]
POSITION = [0.5, -2.0, 3.0, 1.0]


def check_triangle():
    ok = True
    for stage in ("vert", "frag"):
        vulkan = lowered(TD / f"vk_triangle.{stage}.spv")
        opengl = lowered(TD / f"vk_triangle.{stage}.gl.spv")
        if vulkan != opengl:
            ok = False
            print(f"  FAIL vk_triangle.{stage}: Vulkan and OpenGL SPIR-V lower differently")
        elif oracle_hlsl(vulkan, f"vk_triangle_{stage}") is None:
            ok = False
            print(f"  FAIL vk_triangle.{stage}: XenosRecomp rejected it")
        else:
            print(f"  PASS vk_triangle.{stage}: Vulkan SPIR-V lowers as the OpenGL one ({len(vulkan)} B), oracle OK")
    return ok


def check_blocks():
    m = module("vk_blocks.vert.spv")
    si = resolve_interface(m)
    layout = [(b.kind, b.descriptor_set, b.binding, b.reg, b.regs) for b in si.blocks]
    want = [("push", None, None, 0, 1), ("uniform", 0, 0, 4, 5), ("uniform", 0, 1, 12, 1)]
    if layout != want:
        print(f"  FAIL vk_blocks.vert: blocks at {layout}, want {want}")
        return False
    blob, manifest = lower_ex(m)
    # Each uniform block has its groups of 4 to itself, so the literals start past the last one's.
    if any(reg < 16 for reg, _ in manifest):
        print(f"  FAIL vk_blocks.vert: literals at {[reg for reg, _ in manifest]}, inside a uniform block's group")
        return False
    color, tint, scale, offset = [0.5, 0.25, 1.0, 2.0], [2.0, 4.0, 0.5, 1.0], 3.0, [0.125, -0.5]
    consts = registers(struct.pack("<4f", *color), 0)
    consts.update(registers(columns(MVP) + struct.pack("<4f", *tint), 4))
    consts.update(registers(struct.pack("<f4x2f", scale, *offset), 12))
    for reg, values in manifest:
        consts[reg] = list(values)
    exports = run_vertex(blob, {0: POSITION}, consts)
    want_position = mat_vec(MVP, POSITION)
    want_color = [color[i] * tint[i] * scale + (offset + [0.0, 0.0])[i] for i in range(4)]
    if not close(exports.get(OPOS, []), want_position) or not close(exports.get(0, []), want_color):
        print(f"  FAIL vk_blocks.vert: position {exports.get(OPOS)} (want {want_position}), "
              f"colour {exports.get(0)} (want {want_color})")
        return False
    if oracle_hlsl(blob, "vk_blocks_vert") is None:
        print("  FAIL vk_blocks.vert: XenosRecomp rejected it")
        return False
    print("  PASS vk_blocks.vert: push c0, blocks c4-c8 and c12, literals from c16; mvp * position and the colour "
          "from raw std140 bytes")
    return True


def check_matrices():
    m = module("vk_matrices.vert.spv")
    blob, manifest = lower_ex(m)
    consts = registers(columns(VIEW) + columns(PROJECTION), 0)
    for reg, values in manifest:
        consts[reg] = list(values)
    exports = run_vertex(blob, {0: POSITION}, consts)
    want_position = mat_vec(mat_mat(PROJECTION, VIEW), POSITION)
    want_row = vec_mat(POSITION, VIEW)
    if not close(exports.get(OPOS, []), want_position) or not close(exports.get(0, []), want_row):
        print(f"  FAIL vk_matrices.vert: position {exports.get(OPOS)} (want {want_position}), "
              f"row {exports.get(0)} (want {want_row})")
        return False
    if oracle_hlsl(blob, "vk_matrices_vert") is None:
        print("  FAIL vk_matrices.vert: XenosRecomp rejected it")
        return False
    print("  PASS vk_matrices.vert: projection * view * position and position * view over columns")
    return True


def check_samplers():
    m = module("vk_samplers.frag.spv")
    si = resolve_interface(m)
    slots = sorted((v.descriptor_set, v.binding, slot) for v, slot in si.samplers)
    blocks = [(b.kind, b.reg, b.regs) for b in si.blocks]
    if slots != [(0, 2, 0), (1, 0, 1)] or blocks != [("push", 0, 2)]:
        print(f"  FAIL vk_samplers.frag: samplers {slots}, blocks {blocks}")
        return False
    blob = lower(m)
    c = container.Container(blob)
    fetched = sorted(xu.TextureFetchInstruction(*r.raw).fetch_constant_index()
                     for r in xu._decode_program(c.ucode_dwords, c.shader_type)[0] if r.kind == "tfetch")
    if fetched != [0, 1] or oracle_hlsl(blob, "vk_samplers_frag") is None:
        print(f"  FAIL vk_samplers.frag: fetches from {fetched}, or the oracle rejected it")
        return False
    print("  PASS vk_samplers.frag: set 0 binding 2 -> tf0, set 1 binding 0 -> tf1, push offset 16 -> c1")
    return True


def check_position_only():
    """No varyings: interpolators are allocated before the position, as xsdk's resolve shader does, and the export
    count is 0; an alloc after the program's end would never run, and a count of -1 doesn't fit the field."""
    blob = lower(module("vk_position_only.vert.spv"))
    c = container.Container(blob)
    text = xu.disassemble(c.ucode_dwords, c.shader_type)
    lines = [line.split("*/")[-1].strip() for line in text.splitlines() if "alloc" in line or "exec" in line]
    want = ["exec", "alloc interpolators", "alloc position", "exece"]
    export_count = c.program_control >> 20 & 0xF
    if lines != want or export_count or oracle_hlsl(blob, "vk_position_only_vert") is None:
        print(f"  FAIL vk_position_only.vert: control flow {lines}, want {want}; export count {export_count}; "
              "or the oracle rejected it")
        return False
    print("  PASS vk_position_only.vert: alloc interpolators, alloc position, exece, export count 0; oracle OK")
    return True


def check_constant_exports():
    """A constant goes straight to an export, as xsdk's hardware-proven constant shaders do."""
    color = lower(module("vk_constant_color.frag.spv"))
    c = container.Container(color)
    text = xu.disassemble(c.ucode_dwords, c.shader_type)
    exports = run_vertex(lower(module("vk_constant_varying.vert.spv")), {0: POSITION}, {0: [0.25, 0.5, 0.75, 1.0]})
    if "max oC0, c2, c2" not in text or exports.get(0) != [0.25, 0.5, 0.75, 1.0] or             oracle_hlsl(color, "vk_constant_color_frag") is None:
        print(f"  FAIL constant exports: colour shader {text!r}, varying {exports.get(0)}")
        return False
    print("  PASS constant exports: oC0 from push constant c2, and o0 from uniform c0")
    return True


def check_block_literals():
    """A uniform block has whole groups of 4 to itself, the console's unit for loading constants from memory
    (xsdk:docs/HARDWARE.md), so a host can load one without overwriting anything: push c0, uniform c4, literals c8."""
    m = module("vk_block_literals.vert.spv")
    blocks = [(b.kind, b.reg, b.regs) for b in resolve_interface(m).blocks]
    blob, manifest = lower_ex(m)
    literals = [reg for reg, _ in manifest]
    offset, scale = [0.25, -1.0, 0.0, 0.0], [2.0, 0.5, 0.0, 0.0]
    consts = {0: offset, 4: scale}
    for reg, values in manifest:
        consts[reg] = list(values)
    exports = run_vertex(blob, {0: POSITION}, consts)
    # w comes from the export's constant 1, which the simulator leaves out; x and y need both blocks, z the literal.
    want = [POSITION[0] * scale[0] + offset[0], POSITION[1] * scale[1] + offset[1], 0.5]
    if blocks != [("push", 0, 1), ("uniform", 4, 1)] or not literals or min(literals) < 8 or \
            not close(exports.get(OPOS, [])[:3], want) or oracle_hlsl(blob, "vk_block_literals_vert") is None:
        print(f"  FAIL vk_block_literals.vert: blocks {blocks}, literals at {literals}, position {exports.get(OPOS)} "
              f"(want {want}), or the oracle rejected it")
        return False
    print("  PASS vk_block_literals.vert: push c0, uniform c4, literals from c8; the position from all three")
    return True


def check_fetch_order():
    """r0 holds the vertex index each fetch reads as it runs (xenia:src/xenia/gpu/dxbc_shader_translator_fetch.cc:80-88),
    so the fetch into r0 comes last; the allocator packs the others where it likes. Fetched first, it made
    vulkan4xenon's vkbench sprites read their uv and colour, in Xenia, from the vertex their x position named."""
    blob = lower(module("vk_three_inputs.vert.spv"))
    c = container.Container(blob)
    text = xu.disassemble(c.ucode_dwords, c.shader_type)
    operands = [line.split("*/")[-1].split() for line in text.splitlines() if "vfetch" in line]
    fetches = [o[1].rstrip(",") for o in operands]
    indices = [o[2].rstrip(",") for o in operands]
    oracle_ok = oracle_hlsl(blob, "vk_three_inputs_vert") is not None
    if len(set(fetches)) != 3 or fetches[-1] != "r0" or any(i != "r0.x" for i in indices) or not oracle_ok:
        print(f"  FAIL vk_three_inputs.vert: fetches into {fetches} by {indices}, want three registers, r0 last, "
              "each by r0.x; or the oracle rejected it")
        return False
    print(f"  PASS vk_three_inputs.vert: fetches into {', '.join(fetches)}, r0 last, each by r0.x; oracle OK")
    return True


def main():
    results = [check_triangle(), check_blocks(), check_matrices(), check_samplers(), check_position_only(),
               check_constant_exports(), check_fetch_order(), check_block_literals()]
    ok = all(results)
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
