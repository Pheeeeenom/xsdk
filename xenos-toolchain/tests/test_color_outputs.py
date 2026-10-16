#!/usr/bin/env python3
"""xsc lowers fragment outputs at locations 0 to 3 to oC0 to oC3, the four colour targets, or refuses what it can't.

xsc exported its first colour output alone, to oC0, so a shader writing several drew only the first (xsdk:TODO.md,
"Multiple render targets"). vulkan4xenon milestone 25 draws up to four colour attachments a subpass, each from the
output at its location (xenia:src/xenia/gpu/ucode.h:1836-1839).

- vk_four_outputs.frag writes four outputs, from push constants, coordinates and literals; each lands in its export,
  and the record's outputs mask names all four (XenosRecomp:XenosRecomp/shader.h:80-92).
- vk_outputs_apart.frag writes locations 3 and 1 alone, declared in that order, the second a float: each lands at its
  location, the float in every component, and the mask names those two.
- vk_outputs_selected.frag has an if/else assign a local both outputs read.
- vk_frag_depth.frag, with one output, keeps its mask of oC0 alone, and its ps_export_mode 3; the others keep 2.
- vk_outputs_branch.frag assigns its second output in an if/else, and vk_output_location_4.frag has an output past
  oC3: each is refused.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_local_stores import UV, check, module, push_constants  # noqa: E402

A = [1.0, 2.0, 3.0, 4.0]
B = [0.5, -1.0, 8.0, 0.25]
# The pixel record's words after the ucode's offset and size, SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, field 10, the
# interpolator info and field 18 (docs/container.md).
OUTPUTS_WORD = 7


def lowered(name, values):
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS)
    constants = push_constants(name, values)
    constants.update({r: list(v) for r, v in manifest})
    return blob, simulate.run(blob, registers={0: UV + [0.0, 0.0]}, constants=constants).exports


def outputs_mask(blob):
    parsed = container.Container(blob)
    return struct.unpack_from(">I", blob, parsed.off_shader + 4 * OUTPUTS_WORD)[0]


def export_mode(blob):
    return container.Container(blob).program_control >> 27 & 0xF


def check_word(label, got, want):
    if got != want:
        raise SystemExit(f"  FAIL {label}: {got:#x}, not {want:#x}")
    print(f"  PASS {label}: {got:#x}")


def check_exports(name, exports, want):
    if sorted(exports) != sorted(want):
        raise SystemExit(f"  FAIL {name}: exports {sorted(exports)}, not {sorted(want)}")
    for export, values in want.items():
        check(f"{name}, oC{export}", exports[export], values)


def main():
    blob, exports = lowered("vk_four_outputs.frag", [A, B])
    check_exports("vk_four_outputs.frag", exports, {
        0: A,
        1: [b * UV[0] for b in B],
        2: [UV[0], UV[1], 0.25, 2.5],
        3: [a + b for a, b in zip(A, B)],
    })
    check_word("vk_four_outputs.frag, its outputs mask", outputs_mask(blob), 0xF)
    check_word("vk_four_outputs.frag, its ps_export_mode", export_mode(blob), 2)

    blob, exports = lowered("vk_outputs_apart.frag", [A, B])
    check_exports("vk_outputs_apart.frag", exports, {1: [B[2] + UV[0]] * 4, 3: [a * UV[1] for a in A]})
    check_word("vk_outputs_apart.frag, its outputs mask", outputs_mask(blob), 0xA)

    blob, exports = lowered("vk_outputs_selected.frag", [A, B])
    chosen = A if UV[0] > 0.5 else B
    check_exports("vk_outputs_selected.frag", exports, {0: chosen, 1: [2.0 * c for c in chosen]})

    blob, _ = lower_ex(module("vk_frag_depth.frag"), XSDK_REGISTERS)
    check_word("vk_frag_depth.frag, its outputs mask", outputs_mask(blob), 0x1)
    check_word("vk_frag_depth.frag, its ps_export_mode", export_mode(blob), 3)

    for name, why in (("vk_outputs_branch.frag", "location 1 stored 2 times"),
                      ("vk_output_location_4.frag", "past oC3")):
        try:
            lower_ex(module(name), XSDK_REGISTERS)
        except LowerError as error:
            if why not in str(error):
                raise SystemExit(f"  FAIL {name} refused, but as: {error}")
            print(f"  PASS {name} refused: {error}")
        else:
            raise SystemExit(f"  FAIL {name} lowered")
    print("OK")


if __name__ == "__main__":
    main()
