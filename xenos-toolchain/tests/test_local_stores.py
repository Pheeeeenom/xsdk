#!/usr/bin/env python3
"""xsc lowers a local variable stored more than once as each load sees it, or refuses it.

The straight-line lowering reads a local as its last store (xsc/lower.py, _store_map), so a shader that stores a local
again lowered every earlier load as the last value: vk_local_reassign.frag's `first + t` came out as t + t, with no
error, and vk_local_chain.frag's 64 multiply-adds on one local recursed until Python gave up. The unroller now rewrites
such a shader into single assignments when it has no branches (xsc/unroll.py), and the lowering refuses what's left.

- vk_local_reassign.frag, vk_local_chain.frag and vk_local_reassign.vert lower, and the simulator gives what the GLSL
  says for each.
- vk_local_partial.frag stores into one component of a local; vk_local_branch_reassign.frag stores a local twice beside
  an if/else; vk_local_merged_early.frag reads the if/else's variable before the if/else. Each is refused.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402
from test_xsc_lower import TD  # noqa: E402

OPOS = int(xu.ExportRegister.kVSPosition)
UV = [0.75, 0.25]
A = [1.0, 2.0, 3.0, 4.0]
B = [0.5, -1.0, 8.0, 0.25]
K = [0.9999, 0.999, 0.99, 0.9]
C = [0.001, 0.002, 0.003, 0.004]


def module(name):
    return spirv.Module.parse((TD / f"{name}.spv").read_bytes())


def push_constants(name, values):
    """The shader's push constant block's registers, filled with values from its first."""
    block = next(b for b in resolve_interface(module(name)).blocks if b.kind == "push")
    return {block.reg + i: v for i, v in enumerate(values)}


def run_fragment(name, values):
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS)
    constants = push_constants(name, values)
    constants.update({r: list(v) for r, v in manifest})
    return simulate.run(blob, registers={0: UV + [0.0, 0.0]}, constants=constants).exports[0]


def close(got, want):
    return all(abs(g - w) <= 1e-4 * max(1.0, abs(w)) for g, w in zip(got, want))


def check(label, got, want):
    if not close(got, want):
        print(f"  FAIL {label}: {got}, where the GLSL gives {want}")
        raise SystemExit("FAILED")
    print(f"  PASS {label}: {[round(x, 5) for x in got]}")


def main():
    got = run_fragment("vk_local_reassign.frag", [A, B])
    check("vk_local_reassign.frag, first + t", got, [a * UV[0] + b * UV[1] for a, b in zip(A, B)])

    x = [UV[0], UV[1], UV[0], UV[1]]
    for _ in range(64):
        x = [simulate.single(v * k + c) for v, k, c in zip(x, K, C)]
    check("vk_local_chain.frag, 64 multiply-adds on one local", run_fragment("vk_local_chain.frag", [K, C]), x)

    blob, manifest = lower_ex(module("vk_local_reassign.vert"), XSDK_REGISTERS)
    constants = push_constants("vk_local_reassign.vert", [B, A])
    constants.update({r: list(v) for r, v in manifest})
    position = [3.0, -2.0, 0.5, 1.0]
    result = simulate.run(blob, registers={0: [0.0] * 4}, constants=constants,
                          vertex_fetch=lambda n, fetch, index: position)
    check("vk_local_reassign.vert, the position scaled then moved", result.exports[OPOS],
          [p * s + o for p, s, o in zip(position, B, A)])

    for name in ("vk_local_partial.frag", "vk_local_branch_reassign.frag", "vk_local_merged_early.frag"):
        try:
            lower_ex(module(name), XSDK_REGISTERS)
        except LowerError as error:
            print(f"  PASS {name}: refused ({error})")
            continue
        print(f"  FAIL {name}: lowered, where it must be refused")
        raise SystemExit("FAILED")
    print("OK")


if __name__ == "__main__":
    main()
