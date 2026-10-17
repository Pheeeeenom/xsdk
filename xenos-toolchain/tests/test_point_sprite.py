#!/usr/bin/env python3
"""xsc exports gl_PointSize and reads gl_PointCoord as Xenos's point sprites take them (vulkan4xenon milestone 26).

A vertex shader's oPts (export 63) holds the point's size in x, its full width in pixels, and its edge and kill flags in
y and z (xenia:src/xenia/gpu/ucode.h:1813-1834); SQ_PROGRAM_CNTL's vs_export_mode says a second position vector is
exported, 2 for a sprite's size (xenia:src/xenia/gpu/registers.h:144, xenos.h:790-800). A pixel shader's parameters
hold a point sprite's coordinates in their zw (xenia:src/xenia/gpu/registers.h:184-212), which shaders take the
absolute value of.

- vk_point_sprite.vert exports its pushed size to oPts.x alone, and its record's vs_export_mode is 2;
  vk_block_literals.vert, without a size, keeps 0.
- vk_point_coord.frag colours by gl_PointCoord: the simulator, given the parameters with a sign on z, exports their
  |zw|, and the record turns the parameters on.
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

OPOS = int(xu.ExportRegister.kVSPosition)
OPTS = int(xu.ExportRegister.kVSPointSizeEdgeFlagKillVertex)
POSITION = [0.5, -0.25, 0.75, 1.0]
OFFSET = [0.25, 0.5, 0.0, 0.0]
SIZE = 24.0
# The pixel parameters: the column and row, then the sprite's coordinates, z's sign set as a line's flag would be.
PARAMETERS = [100.0, 40.0, -0.25, 0.75]
SQ_PROGRAM_CNTL_PARAM_GEN = 1 << 18


def export_mode(blob):
    return container.Container(blob).program_control >> 24 & 7


def check_point_size():
    name = "vk_point_sprite.vert"
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS)
    constants = {0: OFFSET, 1: [SIZE, 0.0, 0.0, 0.0]}
    constants.update({r: list(v) for r, v in manifest})
    result = simulate.run(blob, registers={0: [0.0] * 4}, constants=constants,
                          vertex_fetch=lambda n, fetch, index: POSITION)
    check(f"{name}, the position", result.exports[OPOS], [p + o for p, o in zip(POSITION, OFFSET)])
    check(f"{name}, its size in oPts.x", result.exports[OPTS][:1], [SIZE])
    parsed = container.Container(blob)
    text = xu.disassemble(parsed.ucode_dwords, parsed.shader_type)
    exports = [line for line in text.splitlines() if "oPts" in line]
    if len(exports) != 1 or "oPts.x___" not in exports[0]:
        raise SystemExit(f"  FAIL {name}: its oPts exports are {exports}, not one of x alone")
    print(f"  PASS {name}: {exports[0].strip()}")
    for shader, want in ((name, 2), ("vk_block_literals.vert", 0)):
        got = export_mode(lower_ex(module(shader), XSDK_REGISTERS)[0])
        if got != want:
            raise SystemExit(f"  FAIL {shader}: vs_export_mode {got}, not {want}")
        print(f"  PASS {shader}: vs_export_mode {got}")


def check_point_coord():
    name = "vk_point_coord.frag"
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS)
    parsed = container.Container(blob)
    if not parsed.program_control & SQ_PROGRAM_CNTL_PARAM_GEN:
        raise SystemExit(f"  FAIL {name}: its record leaves the pixel parameters off")
    param_reg = parsed.context_misc >> 8 & 0xFF
    result = simulate.run(blob, registers={param_reg: PARAMETERS}, constants={r: list(v) for r, v in manifest})
    check(f"{name}, gl_PointCoord in r{param_reg}'s |zw|", result.exports[0], [0.25, 0.75, 0.0, 1.0])


def main():
    check_point_size()
    check_point_coord()
    print("OK")


if __name__ == "__main__":
    main()
