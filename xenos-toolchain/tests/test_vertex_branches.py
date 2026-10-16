#!/usr/bin/env python3
"""A vertex shader's if/else lowers to selects where its arms meet (xsdk:src/xsc_unroll.c, src/xsc_lower_flow.c).

A vertex shader lowered as straight-line code, a local's value its last store, so a branch would have lowered as its
last arm alone; xsc refused it. The unroller now runs both arms of an if/else on data, each from the locals and outputs
as they were before it, and merges each one the arms leave apart with a select, which the lowering makes a cnd on the
comparison branched on. The simulator gives what the GLSL says for each arm:

- vk_vertex_branch.vert scales its output by a ?: of calls, which glslang makes an if/else storing a local in each arm.
- vk_vertex_if.vert stores a component of a local and an output in each arm, one arm with an if of its own, and an &&
  glslang makes a Phi of; each vertex below takes a different set of arms.
- vk_vertex_uniform_if.vert branches on a push constant, both values of it. Every vertex takes the same arm, so its
  arms go in execs on the predicate a setp sets, every instruction in them predicated as its exec is
  (xenia:src/xenia/gpu/ucode.h:114-124), and the arm not taken is skipped; the simulator follows the control flow.
  vk_vertex_if.vert's branches are on data and stay selects, with no predicated exec.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from test_local_stores import check, module, push_constants  # noqa: E402

OPOS = int(xu.ExportRegister.kVSPosition)
OFFSET = [0.25, 0.375, 0.5, 0.0]
TINT = [0.125, 0.25, 0.75, 1.0]
SCALE = [2.0, 0.5, 0.25, 1.0]
MOVE = [0.0625, -0.125, 0.25, 0.0]


def run_vertex(name, blocks, attributes):
    """The exports for one vertex; the shader fetches its last attribute first, as it fetches r0, its index, last."""
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS)
    constants = push_constants(name, blocks) if blocks else {}
    constants.update({r: list(v) for r, v in manifest})
    fetched = list(reversed(attributes))
    return simulate.run(blob, registers={0: [0.0] * 4}, constants=constants,
                        vertex_fetch=lambda n, fetch, index: fetched[n]).exports


def predication(name):
    """Each exec's condition, None for one that always runs, with whether every ALU instruction in it is predicated as
    the exec is; and whether a setp sets the predicate."""
    blob, _ = lower_ex(module(name), XSDK_REGISTERS)
    parsed = container.Container(blob)
    execs, setp, current = [], False, None
    for rec in xu._decode_program(parsed.ucode_dwords, parsed.shader_type)[0]:
        if rec.kind == "cf" and isinstance(rec.detail, xu.ParsedExecInstruction):
            d = rec.detail
            current = [d.condition if d.type == xu._ExecType.kPredicated else None, True]
            execs.append(current)
        elif rec.kind == "alu" and current is not None:
            op = xu.AluInstruction(*rec.raw)
            setp = setp or xu.AluScalarOpcode.kSetpEq <= op.scalar_opcode() <= xu.AluScalarOpcode.kSetpGe
            matches = op.is_predicated() == (current[0] is not None) and (
                current[0] is None or op.predicate_condition() == current[0])
            current[1] = current[1] and matches
    return execs, setp


def check_layout():
    execs, setp = predication("vk_vertex_uniform_if.vert")
    conditions = [c for c, _ in execs]
    if not setp or True not in conditions or False not in conditions or conditions[-1] is not None:
        raise SystemExit(f"  FAIL vk_vertex_uniform_if.vert: execs {conditions}, a setp {setp}")
    if not all(ok for _, ok in execs):
        raise SystemExit("  FAIL vk_vertex_uniform_if.vert: an instruction not predicated as its exec is")
    print(f"  PASS vk_vertex_uniform_if.vert: a setp, then execs on {conditions}, each instruction predicated as its "
          f"exec is")
    execs, setp = predication("vk_vertex_if.vert")
    if setp or any(c is not None for c, _ in execs):
        raise SystemExit("  FAIL vk_vertex_if.vert: a branch on data went into predicated execs")
    print("  PASS vk_vertex_if.vert: its branches on data stay selects")


def vertex_if(position, color):
    p = list(position)
    if color[3] > 0.5:
        p[0] += OFFSET[0]
        v = list(color)
        if color[0] < 0.25:
            v = list(TINT)
    else:
        p[1] -= OFFSET[1]
        v = [1.0, 0.0, 0.0, 1.0]
    if position[0] > 0.0 and position[1] < OFFSET[2]:
        p[2] = 0.5
    return p, v


def uniform_if(position, color, mode):
    p = [a * s for a, s in zip(position, SCALE)]
    if mode > 0.5:
        p = [p[1], p[0], p[2], p[3]]
        return [a + b for a, b in zip(p, MOVE)], [0.0, 1.0, 0.0, 1.0]
    return p, [c * SCALE[2] for c in color]


def main():
    check_layout()
    for position in ([3.0, 0.0, 4.0, 1.0], [0.0, 0.0, 0.0, 1.0]):
        length = sum(x * x for x in position[:3]) ** 0.5
        want = [x / length for x in position] if length else position
        exports = run_vertex("vk_vertex_branch.vert", [], [position])
        check(f"vk_vertex_branch.vert at {position}, v_scaled", exports[0], want)

    vertices = [
        ([0.5, 0.25, 0.1, 1.0], [0.5, 0.5, 0.5, 1.0]),
        ([0.5, 0.75, 0.1, 1.0], [0.125, 0.5, 0.5, 1.0]),
        ([-0.5, 0.25, 0.1, 1.0], [0.5, 0.5, 0.5, 0.25]),
        ([0.5, 0.25, 0.1, 1.0], [0.75, 0.5, 0.5, 0.0]),
    ]
    for position, color in vertices:
        want_position, want_color = vertex_if(position, color)
        exports = run_vertex("vk_vertex_if.vert", [OFFSET, TINT], [position, color])
        check(f"vk_vertex_if.vert at {position}, {color}: gl_Position", exports[OPOS], want_position)
        check(f"vk_vertex_if.vert at {position}, {color}: v_color", exports[0], want_color)

    position, color = [0.5, -0.25, 0.75, 1.0], [0.25, 0.5, 0.75, 1.0]
    for mode in (0.0, 1.0):
        want_position, want_color = uniform_if(position, color, mode)
        exports = run_vertex("vk_vertex_uniform_if.vert", [SCALE, MOVE, [mode, 0.0, 0.0, 0.0]], [position, color])
        check(f"vk_vertex_uniform_if.vert, mode {mode}: gl_Position", exports[OPOS], want_position)
        check(f"vk_vertex_uniform_if.vert, mode {mode}: v_color", exports[0], want_color)
    print("OK")


if __name__ == "__main__":
    main()
