#!/usr/bin/env python3
"""Every container xost writes names xost as its creator, never the XDK compiler's version, 2.0.11626.0."""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xsc.spirv as spirv  # noqa: E402
from xasm.combiner import CombinerCycle, Generator, Src  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xasm.textasm import assemble_container  # noqa: E402
from xsc import templates  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402

TD = ROOT / "xsc" / "testdata"
XDK_VERSION = b"2.0.11626.0"
SOURCE = """.pixel
.interpolator 0 texcoord
      exec
   tfetch2D r1, r0.xy, tf0
      alloc colors
      exece
   max oC0, r1, r1
"""


def creator(blob):
    """The creator string the constant table at 0x24 points to, past the table's size word."""
    table = 0x24 + 4
    offset = struct.unpack_from(">I", blob, table + 4)[0]
    return blob[table + offset:blob.index(b"\0", table + offset)]


def containers():
    lower = lambda name: lower_ex(spirv.Module.parse((TD / name).read_bytes()), XSDK_REGISTERS, True)[0]
    combiner = Generator(alu_template=templates.mov(), tfetch_template=templates.texture_fetch())
    yield "xsc, a vertex shader", lower("vk_triangle.vert.spv")
    yield "xsc, a pixel shader", lower("vk_samplers.frag.spv")
    yield "the text assembler", assemble_container(SOURCE, "pixel", XSDK_REGISTERS)
    yield "the combiner", combiner.generate(CombinerCycle(Src.TEXEL0, Src.ZERO, Src.SHADE, Src.ZERO))


def main():
    ok = True
    for name, blob in containers():
        passed = creator(blob) == b"xost" and XDK_VERSION not in blob
        ok &= passed
        print(f"  {'PASS' if passed else 'FAIL'} {name}: creator {creator(blob).decode()!r}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
