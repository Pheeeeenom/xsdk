#!/usr/bin/env python3
"""xsc reads gl_FrontFacing from the pixel parameters' sign, and takes it where GLSL takes a bool.

The pixel parameters' x holds the pixel's column, its sign set on a back face (xenia:src/xenia/gpu/registers.h:224-227;
XenosRecomp:XenosRecomp/shader_recompiler.cpp:1465-1467), column 0's zero a -0.0 there, whose sign rcpc keeps as
-FLT_MAX. xsc took the variable for an interpolator at r0.

- vk_front_facing.frag, a ?: on it, vk_front_facing_if.frag, an if/else on it beside gl_FragCoord, and
  vk_front_facing_discard.frag, a discard on its not, turn the pixel parameters on past the interpolator.
- On the simulator each gives the front face's value where x is 3.0 or 0.0, and the back face's where it's -3.0 or
  -0.0; gl_FragCoord reads x's magnitude either way.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from test_local_stores import check, module  # noqa: E402

COLOR = [0.25, 0.5, 0.75, 1.0]
GREEN = [0.0, 1.0, 0.0, 1.0]
ROW = 7.0
# The pixel parameters' x on front and back faces, column 3 and column 0.
FACES = ((3.0, True), (-3.0, False), (0.0, True), (-0.0, False))
# SQ_PROGRAM_CNTL's param_gen, and SQ_CONTEXT_MISC's register for the parameters (xenia:src/xenia/gpu/registers.h:140,
# 234).
PARAM_GEN = 1 << 18
PARAM_GEN_POS_SHIFT = 8


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def run(name, x):
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS)
    constants = {r: list(v) for r, v in manifest}
    return simulate.run(blob, registers={0: COLOR, 1: [x, ROW, 0.0, 0.0]}, constants=constants)


def parameters(name):
    c = container.Container(lower_ex(module(name), XSDK_REGISTERS)[0])
    if not c.program_control & PARAM_GEN or c.context_misc >> PARAM_GEN_POS_SHIFT & 0xFF != 1:
        fail(f"{name}: the pixel parameters aren't on in r1")
    print(f"  PASS {name}: the pixel parameters in r1, past the interpolator")


def main():
    for name in ("vk_front_facing.frag", "vk_front_facing_if.frag", "vk_front_facing_discard.frag"):
        parameters(name)
    for x, front in FACES:
        side = f"x {x!r}, a {'front' if front else 'back'} face"
        check(f"vk_front_facing.frag, {side}", run("vk_front_facing.frag", x).exports[0], COLOR if front else GREEN)
        coord = [(abs(x) + 0.5) * 0.001, (ROW + 0.5) * 0.001, 0.0, 1.0]
        check(f"vk_front_facing_if.frag, {side}", run("vk_front_facing_if.frag", x).exports[0],
              COLOR if front else coord)
        if run("vk_front_facing_discard.frag", x).killed == front:
            fail(f"vk_front_facing_discard.frag, {side}: {'killed' if front else 'kept'}")
        print(f"  PASS vk_front_facing_discard.frag, {side}: {'kept' if front else 'killed'}")
    print("OK")


if __name__ == "__main__":
    main()
