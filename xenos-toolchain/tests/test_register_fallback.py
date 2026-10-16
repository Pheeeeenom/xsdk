#!/usr/bin/env python3
"""xsc lays out a pixel shader's fetches apart when laying them out together needs past the registers it may use, and
refuses one past them either way as a register budget's refusal.

Laid out together (milestone 36's fetch grouping), a fetch's coordinates and result stay live for the run of fetches,
and Aurora's GLSL for 16 TEV stages with 14 fetches then needed registers past r63: the first instruction naming one
stopped the encoding with "dest=64 does not fit in 6 unsigned bits", before the budget's check could refuse the shader
or the fetches be laid out apart. Seven of vulkan4xenon milestone 18's coverage programs failed so.

- vk_many_fetches.frag, one of the seven, lowers inside xsdk's 64 registers, its fetches laid out apart, where it
  needs 22.
- Under a 16-register budget it's refused as a shader needing more registers, not by the encoder.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xsc.spirv as spirv  # noqa: E402
from xasm.program import XSDK_REGISTERS, RegisterBudget, RegisterBudgetError  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from test_xsc_lower import TD  # noqa: E402


def main():
    module = spirv.Module.parse((TD / "vk_many_fetches.frag.spv").read_bytes())
    lower_ex(module, XSDK_REGISTERS)
    print("  PASS vk_many_fetches.frag lowers inside xsdk's 64 registers")
    try:
        lower_ex(module, RegisterBudget(pixel=16, vertex=64))
    except RegisterBudgetError as error:
        if "registers" not in str(error):
            raise SystemExit(f"  FAIL refused under 16 registers, but as: {error}")
        print(f"  PASS refused under 16 registers: {error}")
    else:
        raise SystemExit("  FAIL lowered under 16 registers")
    print("OK")


if __name__ == "__main__":
    main()
