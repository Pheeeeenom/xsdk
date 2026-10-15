#!/usr/bin/env python3
"""xsc lowers dot() of two vec2s to dp2add, adding zero, so nothing past y counts.

It took dp4 for every dot product but a vec3's, so a vec2's added the products of whatever its registers held in z and
w: Aurora's line corners came out 0.05% short across, the direction to the other end measured too long.

- vk_dot2.frag dots two pairs of push constants' halves and a computed vec2 with one; the simulator gives what the GLSL
  says for each.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

from test_local_stores import UV, check, run_fragment  # noqa: E402

A = [1.0, 2.0, 3.0, 4.0]
B = [0.5, -1.0, 8.0, 0.25]


def main():
    p = [A[0] * UV[0], A[1] * UV[1]]
    want = [A[0] * B[0] + A[1] * B[1], A[2] * B[2] + A[3] * B[3], p[0] * B[0] + p[1] * B[1], 1.0]
    check("vk_dot2.frag, three vec2 dot products", run_fragment("vk_dot2.frag", [A, B]), want)
    print("OK")


if __name__ == "__main__":
    main()
