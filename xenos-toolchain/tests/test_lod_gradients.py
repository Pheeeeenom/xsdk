#!/usr/bin/env python3
"""xsc samples at a LOD or with gradients the shader works out, as Xenos's fetches take them.

xsc took a constant LOD alone and folded it into the fetch's own bias, so textureLod sampled at the computed LOD plus
it, and a LOD past the field's 4 levels didn't encode; textureGrad's gradients were dropped. A fetch now samples at the
fetch constant's bias, the register LOD setTexLOD sets if it reads it, and its own bias, and works the LOD out from its
coordinate's gradients, or those setGradientH and setGradientV set, only where it computes it
(xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:1498-1536).

- vk_lod.frag: textureLod at a LOD it works out reads the register LOD and computes none; at 2, the bias field, and at
  5, past it, through the register; texture() with a bias it works out computes the LOD and adds the register's.
- vk_grad.frag: textureGrad sets both gradients, x and y, and the fetch computes the LOD from them.
- Each runs on the simulator, and every LOD comes out as the GLSL says.
- vk_texel_offset_operand.frag, an offset, and vk_cube_grad.frag, a cube map's gradients, are refused.
"""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_local_stores import UV, check, module, push_constants  # noqa: E402

# The LOD the simulator's texture computes where the fetch does, without register gradients, and its size.
COMPUTED = 1.0
SIZE = 256
GRADIENT = [0.125, 0.0625, 0.0, 0.0]


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def isotropic(gradients):
    """The LOD a SIZE-texel texture takes for 2D gradients: the longer one's log2 in texels."""
    return math.log2(max(math.hypot(g[0], g[1]) for g in gradients) * SIZE)


def run(name, values=None):
    """The shader's colour on the simulator, and each texture fetch's Lod in order; each fetch returns its LOD."""
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS)
    constants = push_constants(name, values) if values else {}
    constants.update({r: list(v) for r, v in manifest})
    lods = []

    def sample(slot, coordinates, lod):
        lods.append(lod)
        level = lod.bias + (lod.register or 0.0)
        if lod.computed:
            level += isotropic(lod.gradients) if lod.gradients else COMPUTED
        return [level] * 4

    result = simulate.run(blob, registers={0: UV + [0.0, 0.0]}, constants=constants, texture_lod=sample)
    return result.exports[0], lods


def lods():
    color, fetched = run("vk_lod.frag")
    want = [(False, 0.0, UV[0] * 8.0), (False, 2.0, None), (False, 0.0, 5.0), (True, 0.0, UV[1] * 12.0)]
    got = [(lod.computed, lod.bias, lod.register) for lod in fetched]
    if got != want or any(lod.gradients for lod in fetched):
        fail(f"vk_lod.frag: its fetches take {got}, not {want}")
    print("  PASS vk_lod.frag: a computed LOD and 5 through the register LOD, 2 in the bias field, all explicit; a "
          "computed bias through the register LOD on the computed one")
    check("vk_lod.frag, each sample's LOD", color, [UV[0] * 8.0, 2.0, 5.0, COMPUTED + UV[1] * 12.0])


def gradients():
    color, fetched = run("vk_grad.frag", [GRADIENT])
    if len(fetched) != 1 or not fetched[0].computed or fetched[0].register is not None:
        fail(f"vk_grad.frag: its fetch takes {fetched}, not one computed LOD")
    want = ([GRADIENT[0], GRADIENT[1], GRADIENT[0]], [0.0, UV[1] * 0.5, 0.0])
    if fetched[0].gradients != want:
        fail(f"vk_grad.frag: gradients {fetched[0].gradients}, not {want}")
    print("  PASS vk_grad.frag: the fetch computes its LOD from the gradients setGradientH and setGradientV set")
    check("vk_grad.frag, its LOD", color, [isotropic(want)] * 4)


def refusals():
    for name, why in (("vk_texel_offset_operand.frag", "image operands 0x8"),
                      ("vk_cube_grad.frag", "gradients for a cube map")):
        try:
            lower_ex(module(name), XSDK_REGISTERS)
        except LowerError as error:
            if why not in str(error):
                fail(f"{name} refused, but as: {error}")
            print(f"  PASS {name} refused: {error}")
        else:
            fail(f"{name} lowered")


def main():
    lods()
    gradients()
    refusals()
    print("OK")


if __name__ == "__main__":
    main()
