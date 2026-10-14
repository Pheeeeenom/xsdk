#!/usr/bin/env python3
"""Regression test for xsc SPIR-V reader + interface resolution.

Asserts the two exit-gate test shaders resolve to the exact Xenos roles our
hand-built, hardware-proven shaders use (spincube.vert == xost_vs2 layout;
modulate.frag == the modulate combiner / xost_ps1 layout)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xdis.container_emit as ce  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402


def load(name):
    spv = ROOT / "xsc" / "testdata" / name
    if not spv.exists():
        # compile on the fly if only the .vert/.frag is present
        raise SystemExit(f"missing {spv} (run glslang on the .vert/.frag)")
    return resolve_interface(spirv.Module.parse(spv.read_bytes()))


def main():
    ok = True

    vs = load("spincube.vert.spv")
    checks = [
        (vs.stage == "vertex", "vs stage"),
        ([(v.name, u, i) for v, u, i in vs.vfetch] ==
         [("a_position", ce.USAGE_POSITION, 0),
          ("a_color", ce.USAGE_COLOR, 0),
          ("a_texcoord", ce.USAGE_TEXCOORD, 0)], "vs vfetch"),
        ([(v.name, o) for v, o, _ in vs.interp_out] ==
         [("v_texcoord", 0), ("v_color", 1)], "vs interp_out"),
        (vs.position_out is not None, "vs position"),
        ([(c.name, c.reg, c.regs) for c in vs.constants] ==
         [("u_mvp", 0, 4)], "vs constant u_mvp -> c0..c3"),
    ]

    fs = load("modulate.frag.spv")
    checks += [
        (fs.stage == "pixel", "fs stage"),
        ([(v.name, r) for v, r, _ in fs.interp_in] ==
         [("v_texcoord", 0), ("v_color", 1)], "fs interp_in"),
        ([(v.name, s) for v, s in fs.samplers] ==
         [("u_tex0", 0)], "fs sampler -> tf0"),
        (fs.sampler_dims == {0: 1}, "fs sampler Dim 2D"),
        ([idx for _, idx in fs.color_out] == [0], "fs color_out -> oC0"),
    ]

    cube = load("fs_cubemap.frag.spv")
    checks += [
        ([(v.name, s) for v, s in cube.samplers] ==
         [("u_cube", 0)], "cube sampler -> tf0"),
        (cube.sampler_dims == {0: 3}, "cube sampler Dim Cube"),
    ]

    for passed, label in checks:
        print(f"  {'PASS' if passed else 'FAIL'}  {label}")
        ok = ok and passed

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
