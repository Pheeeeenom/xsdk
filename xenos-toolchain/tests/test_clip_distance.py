#!/usr/bin/env python3
"""xsc takes gl_ClipDistance in the form Xenos's user clip planes take, or refuses it (vulkan4xenon milestone 26).

Xenos clips against six planes in clip space, dot(the exported position, plane) >= 0 kept, the planes in
PA_CL_UCP_0_X to PA_CL_UCP_5_W (xenia:src/xenia/gpu/registers.h:539-544, dxbc_shader_translator.cc:1033-1053). So a
distance written as dot(gl_Position, a plane from a block) costs no instruction: the manifest names the plane's register
for each distance, ("clip", n, register), and the runtime sets the planes from there.

- vk_clip_distance.vert writes distances 0, 2 and 5 from a push constant's plane and a uniform block's two, the
  position read back from gl_Position and through a local: the manifest names their registers, and the simulator
  exports the position alone. gl_PerVertex's other members were once taken for the position, as they're stored
  through it too.
- vk_clip_distance_world.vert's distance is from another point, vk_clip_distance_literal.vert's plane a literal, and
  vk_cull_distance.vert writes a cull distance: each is refused.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402
from test_local_stores import check, module  # noqa: E402

OPOS = int(xu.ExportRegister.kVSPosition)
POSITION = [0.5, -0.25, 0.75, 1.0]
SCALE = [2.0, 4.0, 0.5, 1.0]
OFFSET = [0.25, 0.5, 0.125, 0.0]


def block_register(name, kind):
    return next(b.reg for b in resolve_interface(module(name)).blocks if b.kind == kind)


def check_planes():
    name = "vk_clip_distance.vert"
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS)
    push = block_register(name, "push")
    uniform = block_register(name, "uniform")
    planes = sorted(entry for entry in manifest if entry[0] == "clip")
    want = [("clip", 0, push + 2), ("clip", 2, uniform + 1), ("clip", 5, uniform)]
    if planes != want:
        raise SystemExit(f"  FAIL {name}: its planes are {planes}, not {want}")
    print(f"  PASS {name}: its planes {planes}")
    constants = {push: SCALE, push + 1: OFFSET, push + 2: [9.0] * 4, uniform: [7.0] * 4, uniform + 1: [5.0] * 4}
    constants.update({entry[0]: list(entry[1]) for entry in manifest if entry[0] != "clip"})
    result = simulate.run(blob, registers={0: [0.0] * 4}, constants=constants,
                          vertex_fetch=lambda n, fetch, index: POSITION)
    if sorted(result.exports) != [OPOS]:
        raise SystemExit(f"  FAIL {name}: it exports {sorted(result.exports)}, not the position alone")
    check(f"{name}, the position", result.exports[OPOS], [p * s + o for p, s, o in zip(POSITION, SCALE, OFFSET)])


def check_refused(name, word):
    try:
        lower_ex(module(name), XSDK_REGISTERS)
    except LowerError as error:
        if word not in str(error):
            raise SystemExit(f"  FAIL {name} refused, but as: {error}")
        print(f"  PASS {name} refused: {error}")
        return
    raise SystemExit(f"  FAIL {name} lowered")


def main():
    check_planes()
    check_refused("vk_clip_distance_world.vert", "gl_ClipDistance")
    check_refused("vk_clip_distance_literal.vert", "gl_ClipDistance")
    check_refused("vk_cull_distance.vert", "gl_CullDistance")
    print("OK")


if __name__ == "__main__":
    main()
