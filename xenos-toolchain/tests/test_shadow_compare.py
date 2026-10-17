#!/usr/bin/env python3
"""xsc compares a sampler2DShadow's depth after the fetch, by the compare op the runtime writes.

A Vulkan shader's comparison takes its op from the sampler bound at run time, not from the shader. Whether Xenos
compares in the fetch hasn't been checked on the console, so xsc fetches the depth as it fetches any texel and
compares in the ALU: the reference, clamped to 0 to 1 as a UNORM depth's is, below the depth, equal to it and above
it, 1 or 0 each, dotted with a register of weights, 1 for each the op passes (vulkan4xenon:docs/ROADMAP.md, milestone
23). The register comes after the blocks, a resource of its own.

- vk_shadow.frag's resources have the comparison at the register after its push constants, named _compare0 in its
  constant table, and the simulator gives Vulkan's result for all eight compare ops, a reference past 1 among them,
  times the push constant.
- vk_shadow_cube.frag, a samplerCubeShadow, is refused.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_local_stores import check, module, push_constants  # noqa: E402
from test_xsc_lower import TD  # noqa: E402

TINT = [1.0, 0.5, 0.25, 2.0]
# The depth the fetch returns, and references below, on and above it, then past 1 against a depth of 1.
CASES = [(0.5, 0.25), (0.5, 0.5), (0.5, 0.75), (1.0, 1.5)]
# VkCompareOp: less, equal and greater are its bits 0, 1 and 2.
OPS = ["never", "less", "equal", "less or equal", "greater", "not equal", "greater or equal", "always"]


def vulkan(op, depth, reference):
    """Vulkan's result: the reference, clamped for a UNORM depth, against the texel's depth."""
    reference = min(max(reference, 0.0), 1.0)
    return float(bool(op & 1 and reference < depth or op & 2 and reference == depth or op & 4 and reference > depth))


def main():
    blob, manifest = lower_ex(module("vk_shadow.frag"), XSDK_REGISTERS)
    _, _, resources = native.compile_spirv((TD / "vk_shadow.frag.spv").read_bytes(), 64, 64)
    push = next(r for r in resources if r[0] == "push")
    comparison = [r for r in resources if r[0] == "comparison"]
    if comparison != [("comparison", 0, 0, push[3] + push[4], 1)]:
        raise SystemExit(f"  FAIL vk_shadow.frag: resources {resources}, the comparison not after the block")
    register = comparison[0][3]
    if any(r == register for r, _ in manifest):
        raise SystemExit(f"  FAIL vk_shadow.frag: a literal at the comparison's register {register}")
    names = [text for _, text in container.Container(blob).strings(0, len(blob))]
    if "_compare0" not in names:
        raise SystemExit(f"  FAIL vk_shadow.frag: no _compare0 in the constant table {names}")
    print(f"  PASS vk_shadow.frag: its comparison at c{register}, after its push constants, named _compare0")
    for op, name in enumerate(OPS):
        for depth, reference in CASES:
            constants = push_constants("vk_shadow.frag", [TINT])
            constants.update({r: list(v) for r, v in manifest})
            constants[register] = [float(op & 1), float(op >> 1 & 1), float(op >> 2 & 1), 0.0]
            result = simulate.run(blob, registers={0: [0.25, 0.75, reference, 0.0]}, constants=constants,
                                  texture_fetch=lambda slot, coordinates: [depth, 0.0, 0.0, 1.0])
            want = vulkan(op, depth, reference)
            check(f"vk_shadow.frag, {name}, reference {reference} against depth {depth}", result.exports[0],
                  [want * t for t in TINT])
    try:
        lower_ex(module("vk_shadow_cube.frag"), XSDK_REGISTERS)
    except LowerError as error:
        print(f"  PASS vk_shadow_cube.frag refused: {error}")
    else:
        raise SystemExit("  FAIL vk_shadow_cube.frag lowered")
    print("OK")


if __name__ == "__main__":
    main()
