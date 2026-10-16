#!/usr/bin/env python3
"""An if/else on data inside a loop's body lowers to a select each pass (xsdk:src/xsc_unroll.c).

The unroller refused a conditional store in a loop body: its environment held one value a local. It now runs both arms
of each pass's if/else from the locals as they were and merges what they leave apart with a select, so
vk_loop_if.frag, which adds a step to its sum on the passes whose threshold the pixel's u passes and an eighth to the
sum's w on the others, gives what the GLSL says either side of each threshold.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from test_local_stores import check, module, push_constants  # noqa: E402

STEP = [0.125, 0.0625, 0.25, 0.03125]


def loop_if(u):
    total = [0.0] * 4
    for i in range(4):
        if u > i * 0.25:
            total = [t + s for t, s in zip(total, STEP)]
        else:
            total[3] += 0.125
    return total


def main():
    blob, manifest = lower_ex(module("vk_loop_if.frag"), XSDK_REGISTERS)
    constants = push_constants("vk_loop_if.frag", [STEP])
    constants.update({r: list(v) for r, v in manifest})
    for u in (0.0, 0.125, 0.375, 0.625, 0.875):
        got = simulate.run(blob, registers={0: [u, 0.5, 0.0, 0.0]}, constants=constants).exports[0]
        check(f"vk_loop_if.frag at u {u}", got, loop_if(u))
    print("OK")


if __name__ == "__main__":
    main()
