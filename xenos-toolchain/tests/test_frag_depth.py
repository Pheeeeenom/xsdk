#!/usr/bin/env python3
"""xsc exports gl_FragDepth as Xenos's pixel shaders export depth, or refuses where it can't.

xsc ignored gl_FragDepth, so a shader writing it drew with its polygon's depth. On the console a depth export reached
the depth test only with SQ_PROGRAM_CNTL's ps_export_mode at 3, where colour alone is 2
(xsdk:results/2026-10-16-depth-export/), as vulkan4xenon milestone 18's Z textures need.

- vk_frag_depth.frag exports its colour and, after a discard that keeps the pixel, its depth to oDepth's x alone, as
  Xenia takes it (xenia:src/xenia/gpu/dxbc_shader_translator.cc:1591-1594), and its record's ps_export_mode is 3;
  vk_composite_extract.frag, without a depth, keeps 2.
- vk_frag_depth_branch.frag stores gl_FragDepth in one arm of an if/else, which is refused.
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
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_local_stores import UV, check, module, push_constants  # noqa: E402

EXPORT_DEPTH = int(xu.ExportRegister.kPSDepth)
A = [0.25, 0.5, 0.75, 1.0]
B = [0.5, 0.125, 0.0, 0.0]


def export_mode(blob):
    return container.Container(blob).program_control >> 27 & 0xF


def main():
    blob, manifest = lower_ex(module("vk_frag_depth.frag"), XSDK_REGISTERS)
    constants = push_constants("vk_frag_depth.frag", [A, B])
    constants.update({r: list(v) for r, v in manifest})
    result = simulate.run(blob, registers={0: UV + [0.0, 0.0]}, constants=constants)
    if result.killed:
        raise SystemExit("  FAIL vk_frag_depth.frag killed its pixel")
    check("vk_frag_depth.frag, its colour", result.exports[0], [a * UV[0] for a in A])
    depth = result.exports.get(EXPORT_DEPTH)
    if depth is None:
        raise SystemExit("  FAIL vk_frag_depth.frag exported no depth")
    check("vk_frag_depth.frag, its depth in oDepth.x", depth[:1], [UV[1] * B[0] + B[1]])
    parsed = container.Container(blob)
    if "oDepth.x___" not in xu.disassemble(parsed.ucode_dwords, parsed.shader_type):
        raise SystemExit("  FAIL vk_frag_depth.frag: its depth export writes more than x")
    print("  PASS vk_frag_depth.frag: its depth export writes x alone")
    for name, want in (("vk_frag_depth.frag", 3), ("vk_composite_extract.frag", 2)):
        got = export_mode(lower_ex(module(name), XSDK_REGISTERS)[0])
        if got != want:
            raise SystemExit(f"  FAIL {name}: ps_export_mode {got}, not {want}")
        print(f"  PASS {name}: ps_export_mode {got}")
    try:
        lower_ex(module("vk_frag_depth_branch.frag"), XSDK_REGISTERS)
    except LowerError as error:
        if "gl_FragDepth" not in str(error):
            raise SystemExit(f"  FAIL vk_frag_depth_branch.frag refused, but as: {error}")
        print(f"  PASS vk_frag_depth_branch.frag refused: {error}")
    else:
        raise SystemExit("  FAIL vk_frag_depth_branch.frag lowered")
    print("OK")


if __name__ == "__main__":
    main()
