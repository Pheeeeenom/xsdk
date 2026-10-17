#!/usr/bin/env python3
"""xsc samples cube and 3D textures where Vulkan says they're sampled.

xsc lowered samplerCube to the cube op, a reciprocal, a multiply-add and tfetchCube (docs/xsc.md), which nothing had
checked by its numbers, and refused sampler3D. xsdk's cube and 3D textures and vulkan4xenon's cube and 3D images
sample through them (vulkan4xenon:docs/ROADMAP.md, milestone 23).

- vk_cube.frag, run on the simulator with the cube op as Xenia runs it, fetches each direction from the face Vulkan
  picks, +x, -x, +y, -y, +z then -z as faces 0 to 5 (xenia:src/xenia/gpu/spirv_shader_translator_alu.cc:583-640), at
  Vulkan's s and t moved to 1 to 2, as tfetchCube takes them (xenia:src/xenia/gpu/ucode.h:570-576).
- vk_volume.frag fetches once, tfetch3D at the coordinate's x, y and z, and exports what it fetched.
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
from test_local_stores import check, module  # noqa: E402

# Directions toward each face, off its centre and onto it.
DIRECTIONS = [
    [1.0, 0.25, -0.5], [-2.0, 0.5, 0.75], [0.25, 1.0, -0.5], [-0.5, -4.0, 0.25], [0.75, -0.25, 1.0],
    [0.5, 0.25, -1.0], [3.0, 0.0, 0.0], [0.0, 0.0, -0.5],
]
VOLUME = [0.125, 0.625, 0.9375]


def vulkan_face(r):
    """The face Vulkan's cube map face selection picks, and its s and t (Vulkan 1.0, Table 'Cube map face
    selection'): sc and tc over the major axis's magnitude, from 0 to 1."""
    x, y, z = r
    if abs(x) >= abs(y) and abs(x) >= abs(z):
        face, sc, tc, ma = (0, -z, -y, x) if x > 0 else (1, z, -y, x)
    elif abs(y) >= abs(z):
        face, sc, tc, ma = (2, x, z, y) if y > 0 else (3, x, -z, y)
    else:
        face, sc, tc, ma = (4, x, -y, z) if z > 0 else (5, -x, -y, z)
    return face, 0.5 * (sc / abs(ma) + 1.0), 0.5 * (tc / abs(ma) + 1.0)


def fetches(blob):
    c = container.Container(blob)
    return [xu.TextureFetchInstruction(*r.raw) for r in xu._decode_program(c.ucode_dwords, c.shader_type)[0]
            if r.kind == "tfetch"]


def main():
    blob, manifest = lower_ex(module("vk_cube.frag"), XSDK_REGISTERS)
    constants = {r: list(v) for r, v in manifest}
    for direction in DIRECTIONS:
        face, s, t = vulkan_face(direction)
        seen = []

        def fetch(slot, coordinates):
            seen.append(coordinates)
            return [coordinates[0], coordinates[1], coordinates[2], 1.0]

        result = simulate.run(blob, registers={0: direction + [0.0]}, constants=constants, texture_fetch=fetch)
        check(f"vk_cube.frag toward {direction}, face {face} at s {s:.4f} and t {t:.4f}", result.exports[0],
              [1.0 + s, 1.0 + t, float(face), 1.0])
        if len(seen) != 1:
            raise SystemExit(f"  FAIL vk_cube.frag fetched {len(seen)} times")

    blob, _ = lower_ex(module("vk_volume.frag"), XSDK_REGISTERS)
    found = fetches(blob)
    if len(found) != 1 or found[0].dimension() != xu.FetchOpDimension.k3DOrStacked:
        raise SystemExit(f"  FAIL vk_volume.frag: {[f.dimension() for f in found]}, not one tfetch3D")
    print("  PASS vk_volume.frag: one tfetch3D")
    result = simulate.run(blob, registers={0: VOLUME + [0.0]},
                          texture_fetch=lambda slot, coordinates: coordinates + [0.5])
    check("vk_volume.frag, the fetch at the coordinate's x, y and z", result.exports[0], VOLUME + [0.5])
    print("OK")


if __name__ == "__main__":
    main()
