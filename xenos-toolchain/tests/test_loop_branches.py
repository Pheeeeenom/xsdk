#!/usr/bin/env python3
"""An if/else on data inside a loop's body lowers to a select each pass (xsdk:src/xsc_unroll.c).

The unroller refused a conditional store in a loop body: its environment held one value a local. It now runs both arms
of each pass's if/else from the locals as they were and merges what they leave apart with a select, so
vk_loop_if.frag, which adds a step to its sum on the passes whose threshold the pixel's u passes and an eighth to the
sum's w on the others, gives what the GLSL says either side of each threshold. vk_loop_uniform_if.frag's if/else in its
body is on a push constant, so each pass's arms go in execs on the predicate; it gives what the GLSL says for both
values, the simulator following the control flow. vk_loop_if_break.frag breaks out of an unrolled loop on data, which
is refused: the pass after a break would run for some pixels and not others.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_local_stores import check, module, push_constants  # noqa: E402
from test_vertex_branches import predication  # noqa: E402

STEP = [0.125, 0.0625, 0.25, 0.03125]


def loop_if(u):
    total = [0.0] * 4
    for i in range(4):
        if u > i * 0.25:
            total = [t + s for t, s in zip(total, STEP)]
        else:
            total[3] += 0.125
    return total


def loop_uniform_if(uv, mode):
    total = [uv[0], uv[1], 0.0, 1.0]
    for _ in range(3):
        if mode > 0.5:
            total = [t * 0.5 + s for t, s in zip(total, STEP)]
        else:
            total[2] += 0.25
    return total


def check_uniform():
    execs, setp = predication("vk_loop_uniform_if.frag")
    conditions = [c for c, _ in execs]
    if not setp or conditions.count(True) < 3 or conditions.count(False) < 3 or not all(ok for _, ok in execs):
        raise SystemExit(f"  FAIL vk_loop_uniform_if.frag: execs {conditions}, a setp {setp}")
    print(f"  PASS vk_loop_uniform_if.frag: each pass's arms in execs on the predicate, {conditions}")
    blob, manifest = lower_ex(module("vk_loop_uniform_if.frag"), XSDK_REGISTERS)
    uv = [0.75, 0.25]
    for mode in (0.0, 1.0):
        constants = push_constants("vk_loop_uniform_if.frag", [STEP, [mode, 0.0, 0.0, 0.0]])
        constants.update({r: list(v) for r, v in manifest})
        got = simulate.run(blob, registers={0: uv + [0.0, 0.0]}, constants=constants).exports[0]
        check(f"vk_loop_uniform_if.frag, mode {mode}", got, loop_uniform_if(uv, mode))


def main():
    check_uniform()
    blob, manifest = lower_ex(module("vk_loop_if.frag"), XSDK_REGISTERS)
    constants = push_constants("vk_loop_if.frag", [STEP])
    constants.update({r: list(v) for r, v in manifest})
    for u in (0.0, 0.125, 0.375, 0.625, 0.875):
        got = simulate.run(blob, registers={0: [u, 0.5, 0.0, 0.0]}, constants=constants).exports[0]
        check(f"vk_loop_if.frag at u {u}", got, loop_if(u))
    try:
        lower_ex(module("vk_loop_if_break.frag"), XSDK_REGISTERS)
    except LowerError as error:
        if "break" not in str(error):
            raise SystemExit(f"  FAIL vk_loop_if_break.frag refused, but as: {error}")
        print(f"  PASS vk_loop_if_break.frag refused: {error}")
    else:
        raise SystemExit("  FAIL vk_loop_if_break.frag lowered")
    print("OK")


if __name__ == "__main__":
    main()
