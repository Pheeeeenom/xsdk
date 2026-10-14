#!/usr/bin/env python3
"""compare.py <old file> <new file>...: each pair's arrays, the containers among them checked to differ only in their
creator string, every other array (SPIR-V) checked unchanged."""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, r"Z:\Xbox-360-SDK\xenos-toolchain")
import xdis.container as container  # noqa: E402

MAGICS = (0x102A1100, 0x102A1101)


def arrays(path):
    text = Path(path).read_text(encoding="utf-8")
    return {name: [int(w, 16) for w in re.findall(r"0x[0-9A-Fa-f]{1,8}", body)]
            for name, body in re.findall(r"(\w+)\[\] = \{([^}]*)\}", text)}


def creator(blob):
    table = 0x24 + 4
    offset = struct.unpack_from(">I", blob, table + 4)[0]
    return blob[table + offset:blob.index(b"\0", table + offset)]


def same_but_creator(old, new):
    a, b = (container.Container(struct.pack(f">{len(w)}I", *w)) for w in (old, new))
    return (a.ucode_dwords == b.ucode_dwords and a.program_control == b.program_control
            and a.context_misc == b.context_misc and a.data_unk == b.data_unk and a.shader_type == b.shader_type
            and creator(a.blob) == b"2.0.11626.0" and creator(b.blob) == b"xost")


def main():
    ok = True
    for old_path, new_path in zip(sys.argv[1::2], sys.argv[2::2]):
        old, new = arrays(old_path), arrays(new_path)
        containers = [n for n in old if old[n] and old[n][0] in MAGICS]
        others = [n for n in old if n not in containers]
        good = sum(n in new and same_but_creator(old[n], new[n]) for n in containers)
        kept = sum(old[n] == new.get(n) for n in others)
        passed = good == len(containers) and kept == len(others) and set(old) == set(new)
        ok &= passed
        print(f"{'PASS' if passed else 'FAIL'} {Path(new_path).name}: {good} of {len(containers)} containers differ "
              f"only in the creator, {kept} of {len(others)} other arrays unchanged")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
