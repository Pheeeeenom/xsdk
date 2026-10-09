#!/usr/bin/env python3
"""Write copies of a XEX with one digest broken each, to find which checks a console's kernel enforces."""

import argparse
import struct
from pathlib import Path

# Offsets inside the security info (xenia:src/xenia/kernel/util/xex2_info.h:570-588).
IMPORT_DIGEST = 0x12C
HEADER_DIGEST = 0x164
# A byte in the first page between the PE headers and the first section, which nothing reads.
UNUSED_IMAGE_BYTE = 0x800


def flipped(data, offset):
    copy = bytearray(data)
    copy[offset] ^= 0xFF
    return copy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xex", type=Path)
    args = parser.parse_args()

    data = args.xex.read_bytes()
    header_size = struct.unpack_from(">I", data, 0x08)[0]
    security = struct.unpack_from(">I", data, 0x10)[0]
    variants = {
        "bad_header_digest": security + HEADER_DIGEST,
        "bad_import_digest": security + IMPORT_DIGEST,
        "bad_page_digest": header_size + UNUSED_IMAGE_BYTE,
    }
    for name, offset in variants.items():
        args.xex.with_name(f"{name}.xex").write_bytes(flipped(data, offset))


if __name__ == "__main__":
    main()
