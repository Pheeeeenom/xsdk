#!/usr/bin/env python3
"""A vertex shader with a branch is refused (xsc/lower.py, lower_vertex).

A vertex shader lowers as straight-line code, a local's value its last store, so a branch left after unrolling would
lower as its last arm alone. vk_vertex_branch.vert scales its output by a ?: of calls, which glslang makes an if/else
storing a local in each arm; lowered that way, it would always take the 1.0 arm. It must be refused. With the refusal
undone, the simulation shows the wrong arm taken, which is what the refusal prevents.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_xsc_lower import TD  # noqa: E402

POSITION = [3.0, 0.0, 4.0, 1.0]  # length 5: the shader scales it by 1/5


def main():
    module = spirv.Module.parse((TD / "vk_vertex_branch.vert.spv").read_bytes())
    try:
        blob, manifest = lower_ex(module, XSDK_REGISTERS)
    except LowerError as error:
        print(f"  PASS vk_vertex_branch.vert: refused ({error})")
        print("OK")
        return
    result = simulate.run(blob, registers={0: [0.0] * 4}, constants={r: list(v) for r, v in manifest},
                          vertex_fetch=lambda n, fetch, index: POSITION)
    print(f"  FAIL vk_vertex_branch.vert: lowered, and v_scaled is {result.exports.get(0)}, "
          f"where the shader gives {[x / 5.0 for x in POSITION]}")
    raise SystemExit("FAILED")


if __name__ == "__main__":
    main()
