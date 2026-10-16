#!/usr/bin/env python3
"""xsc marks every vertex fetch in an exec's mask, the fifth and sixth too.

A vertex shader's fetch-bearing exec carries a mask of its fetches over vc_hi's four bits and vc_lo's two
(xenia:src/xenia/gpu/ucode.h:243-246). xsc capped it at four, so an exec of five or six fetches left the last ones
unmarked, and on the console those fetched wrong: Melee's fighters, whose GX vertices make five fetches, drew spikes
(xsdk:docs/HARDWARE.md, "A fifth vertex fetch").

- vk_local_reassign.vert's exec of fetches has the mask of its count, all in vc_hi; vk_five_fetches.vert's is 0x1F,
  vc_hi 0xF and vc_lo 0x1; vk_six_fetches.vert's 0x3F, vc_lo 0x3.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from test_local_stores import module  # noqa: E402

EXEC_OPCODES = (1, 2)


def fetch_exec(name):
    """The first exec's fetch count, from its sequence's fetch bits, and its vc_hi and vc_lo."""
    words = container.Container(lower_ex(module(name), XSDK_REGISTERS)[0]).ucode_dwords
    for slot in range(0, len(words) - 2, 3):
        a, b, c = words[slot:slot + 3]
        for low, high in ((a, b & 0xFFFF), (b >> 16 | (c & 0xFFFF) << 16, c >> 16)):
            if high >> 12 & 0xF in EXEC_OPCODES:
                sequence = low >> 16 & 0xFFF
                fetches = sum(sequence >> (2 * k) & 1 for k in range(6))
                if fetches:
                    return fetches, low >> 28 & 0xF, high & 0x3
    raise SystemExit(f"  FAIL {name}: no exec of fetches")


def main():
    for name in ("vk_local_reassign.vert", "vk_five_fetches.vert", "vk_six_fetches.vert"):
        fetches, vc_hi, vc_lo = fetch_exec(name)
        mask = (1 << fetches) - 1
        if (vc_hi, vc_lo) != (mask & 0xF, mask >> 4):
            raise SystemExit(f"  FAIL {name}: {fetches} fetches, vc_hi {vc_hi:#x} vc_lo {vc_lo:#x}")
        print(f"  PASS {name}: {fetches} fetches, vc_hi {vc_hi:#x} vc_lo {vc_lo:#x}")
    print("OK")


if __name__ == "__main__":
    main()
