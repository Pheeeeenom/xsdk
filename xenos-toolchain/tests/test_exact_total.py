#!/usr/bin/env python3
"""Exact-mode totality test: for every shader xsc lowers from xsc/testdata,
and the combiner's demo bank, disassemble(exact=True) ->
reconstruct_from_exact -> bytes identical to the original ucode region."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xenos_ucode  # noqa: E402
from test_textasm import blob_list  # noqa: E402


def main():
    sys.setrecursionlimit(20000)
    ok = True
    blobs = blob_list()
    for name, blob in blobs:
        c = container.Container(blob)
        exact_text = xenos_ucode.disassemble(c.ucode_dwords, c.shader_type,
                                             exact=True)
        rebuilt = xenos_ucode.reconstruct_from_exact(exact_text)
        if rebuilt != c.ucode_bytes:
            ok = False
            print(f"FAIL {name}: rebuilt {len(rebuilt)} bytes vs "
                  f"{len(c.ucode_bytes)} original")
    print(f"{'PASS' if ok else 'FAIL'} {len(blobs)} shaders: exact text -> "
          f"identical ucode")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
