#!/usr/bin/env python3
"""A loop an int in a block counts runs as a hardware loop, its count set by the runtime (xsdk:src/xsc_unroll.c,
src/xsc_lower_flow.c).

xsc refused a Vulkan shader's loop counted by a push constant or a uniform buffer: the unroller needs a constant count,
and the hardware loop lowering took a bare uniform alone, which Vulkan's GLSL has none of. The unroller now walks such
a loop's pass once, each local and output it carries a value at the top of the pass, and the lowering lays it out
between LOOP_START and LOOP_END: what it carries in registers, each pass's values put in them at its end, the
induction variable a float counting from 0. The manifest gives the loop constant and where the count is, which the
runtime reads from the block (("block_loop", id, kind, set, binding, offset)); a vertex shader's loops take loop
constants from 0 and a pixel shader's from 16. The simulator runs each loop the count's passes:

- vk_loop_count.frag adds a step to a base a pass, counted by a push constant: 0, 1, 3 and 7 passes.
- vk_loop_count_if.frag, counted by a uniform buffer, has an if/else on data in its pass, reads a block's array at the
  pass's index and the index as a float, and breaks once what it sums passes a limit: a setp and a predicated LOOP_END,
  the break on a value every pixel shares. Each pixel below takes a different set of arms.
- vk_vertex_loop_count.vert moves its position and sums a colour a pass.
- vk_loop_swap.frag swaps two locals a pass, which the loop's registers take all at once.
- vk_loop_break_data.frag breaks on data, which is refused: LOOP_END breaks when all 64 vertices or pixels agree
  (xenia:src/xenia/gpu/ucode.h:372-373).
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
from test_local_stores import check, module, push_constants  # noqa: E402

OPOS = int(xu.ExportRegister.kVSPosition)
STEP = [0.125, 0.0625, 0.25, 0.03125]
BASE = [0.5, 0.25, 0.0, 1.0]
COLORS = [[0.0625 * (i + 1), 0.125, 0.03125 * i, 0.25] for i in range(8)]
LIMIT = [0.0, 0.3, 0.0, 0.0]


def lowered(name):
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS)
    loops = [entry for entry in manifest if entry[0] == "block_loop"]
    literals = {entry[0]: list(entry[1]) for entry in manifest if entry[0] != "block_loop"}
    return blob, loops, literals


def check_manifest(name, loops, want):
    if loops != want:
        raise SystemExit(f"  FAIL {name}: its loops' needs are {loops}, not {want}")
    print(f"  PASS {name}: needs {loops}")


def loop_count_if(u, count):
    total, seen = [0.0] * 4, [0.0] * 4
    for i in range(count):
        if u > i * 0.125:
            seen = [a + b for a, b in zip(seen, COLORS[i])]
        else:
            seen[3] += 0.0625
        total = [a + b * 0.5 for a, b in zip(total, COLORS[i])]
        if total[1] >= LIMIT[1]:
            break
    return [a + b * 0.25 for a, b in zip(seen, total)]


def uniform_block(name, values):
    block = next(b for b in resolve_interface(module(name)).blocks if b.kind == "uniform")
    return {block.reg + i: v for i, v in enumerate(values)}


def main():
    name = "vk_loop_count.frag"
    blob, loops, literals = lowered(name)
    check_manifest(name, loops, [("block_loop", 16, "push", 0, 0, 32)])
    for count in (0, 1, 3, 7):
        constants = push_constants(name, [STEP, BASE])
        constants.update(literals)
        got = simulate.run(blob, registers={0: [0.5, 0.5, 0.0, 0.0]}, constants=constants, loops={16: count})
        check(f"{name}, {count} passes", got.exports[0], [b + s * count for b, s in zip(BASE, STEP)])

    name = "vk_loop_count_if.frag"
    blob, loops, literals = lowered(name)
    check_manifest(name, loops, [("block_loop", 16, "uniform", 0, 0, 144)])
    for u, count in ((0.0, 3), (0.2, 3), (0.3, 8), (0.9, 8), (0.9, 0)):
        constants = uniform_block(name, COLORS + [LIMIT])
        constants.update(literals)
        got = simulate.run(blob, registers={0: [u, 0.5, 0.0, 0.0]}, constants=constants, loops={16: count})
        check(f"{name} at u {u}, {count} passes", got.exports[0], loop_count_if(u, count))

    name = "vk_vertex_loop_count.vert"
    blob, loops, literals = lowered(name)
    check_manifest(name, loops, [("block_loop", 0, "push", 0, 0, 16)])
    position, color = [0.25, -0.5, 0.5, 1.0], [0.5, 0.25, 1.0, 0.75]
    for count in (0, 2, 5):
        constants = push_constants(name, [STEP])
        constants.update(literals)
        fetched = [color, position]
        got = simulate.run(blob, registers={0: [0.0] * 4}, constants=constants, loops={0: count},
                           vertex_fetch=lambda n, fetch, index: fetched[n]).exports
        check(f"{name}, {count} passes: gl_Position", got[OPOS],
              [position[0] + STEP[0] * count, position[1] + STEP[1] * count] + position[2:])
        check(f"{name}, {count} passes: v_color", got[0], [c * 0.25 * count for c in color])

    name = "vk_loop_swap.frag"
    blob, loops, literals = lowered(name)
    a, b = [0.5, 0.25, 0.75, 1.0], [0.125, 0.0625, 0.375, 0.5]
    for count in (0, 1, 2, 5):
        x, y = a, b
        for _ in range(count):
            x, y = y, [t + 0.125 for t in x]
        constants = push_constants(name, [a, b])
        constants.update(literals)
        got = simulate.run(blob, registers={0: [0.5, 0.5, 0.0, 0.0]}, constants=constants, loops={16: count})
        check(f"{name}, {count} passes", got.exports[0], [p * 0.5 + q for p, q in zip(x, y)])

    try:
        lower_ex(module("vk_loop_break_data.frag"), XSDK_REGISTERS)
    except LowerError as error:
        if "break" not in str(error):
            raise SystemExit(f"  FAIL vk_loop_break_data.frag refused, but as: {error}")
        print(f"  PASS vk_loop_break_data.frag refused: {error}")
    else:
        raise SystemExit("  FAIL vk_loop_break_data.frag lowered")
    print("OK")


if __name__ == "__main__":
    main()
