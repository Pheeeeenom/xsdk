#!/usr/bin/env python3
"""xsc lowers a float extracted from a vector value, not a variable, as a value of its own.

glslang makes `vec3(x).r` and `(a + b).y` OpCompositeExtract of a value, and xsc took that only where a scalar was
wanted, so a fragment program that multiplied one was refused ("unhandled op CompositeExtract"): Aurora's GLSL for
TEV comparisons of a splatted alpha, which vulkan4xenon milestone 18's coverage check found in 1175 programs.

- vk_composite_extract.frag multiplies, sums and floors such floats; the simulator gives what the GLSL says.
"""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

from test_local_stores import UV, check, run_fragment  # noqa: E402

A = [0.25, 0.5, 0.75, 0.6]
B = [2.0, -1.5, 0.5, 4.0]


def main():
    want = [A[3] * 2.0, (A[1] + B[1] * UV[0]) * B[2], math.floor(A[3] * 255.0 + 0.5) / 255.0, 1.0]
    check("vk_composite_extract.frag, floats of vector values", run_fragment("vk_composite_extract.frag", [A, B]),
          want)
    print("OK")


if __name__ == "__main__":
    main()
