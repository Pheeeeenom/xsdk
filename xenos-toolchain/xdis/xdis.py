#!/usr/bin/env python3
"""xdis — open-source Xenos shader disassembler (Phase 0 of xost).

Usage: python xdis.py <blob.bin> [...]
Accepts XDK-era shader containers (as consumed by libxenon's
Xe_LoadShaderFromMemory) or raw ucode (--raw vertex|pixel).
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import container as cont
import xenos_ucode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blobs", nargs="+")
    ap.add_argument("--raw", choices=["vertex", "pixel"],
                    help="treat input as raw big-endian ucode, not a container")
    ap.add_argument("--no-header", action="store_true",
                    help="suppress container metadata")
    ap.add_argument("--exact", action="store_true",
                    help="lossless mode: annotate with ;; raw/pad/tail lines "
                         "so the text reconstructs to identical bytes")
    args = ap.parse_args()

    for path in args.blobs:
        data = Path(path).read_bytes()
        print(f"=== {path} ===")
        if args.raw:
            dwords = list(struct.unpack(f">{len(data)//4}I", data))
            shader_type = args.raw
        else:
            c = cont.Container(data)
            if not args.no_header:
                print(c.describe())
                print("-" * 60)
            dwords, shader_type = c.ucode_dwords, c.shader_type
        print(xenos_ucode.disassemble(dwords, shader_type,
                                      exact=args.exact))
        print()


if __name__ == "__main__":
    main()
