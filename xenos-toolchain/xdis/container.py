#!/usr/bin/env python3
"""Parser for the XDK-era Xenos shader container format.

Structure derived from the open-source consumer in libxenon
(reference/libxenon/libxenon/drivers/xenos/xe.h + xe.c):

  XenosShaderHeader (at 0, all u32 big-endian):
    magic           0x102A1100 | type   (observed: ..01 = vertex, ..00 = pixel)
    offset          base added to XenosShaderData.sh_off to locate ucode
    unk1[3]
    off_constants   offset of constant-table section
    off_shader      offset of XenosShaderData record
    unk2[2]

  XenosShaderData (at off_shader):
    sh_off, sh_size     ucode location: bytes [offset+sh_off, +sh_size)
    program_control     value for GPU reg SQ_PROGRAM_CNTL (per xe.c usage)
    context_misc        value for SQ_CONTEXT_MISC
    unk1[4]

The region between the header and the ucode holds a big-endian variant of the
D3DX constant table (names like "ortho"/"tex0", types, register assignments)
plus the compiler version string. Parsed leniently here; documented as we
learn it (docs/container.md).
"""
import struct
import sys
from pathlib import Path

MAGIC_BASE = 0x102A1100


def u32s(data: bytes, off: int, n: int):
    return struct.unpack_from(f">{n}I", data, off)


class Container:
    def __init__(self, blob: bytes):
        self.blob = blob
        (self.magic, self.offset,
         u1a, u1b, u1c,
         self.off_constants, self.off_shader,
         u2a, u2b) = u32s(blob, 0, 9)
        self.unk1 = (u1a, u1b, u1c)
        self.unk2 = (u2a, u2b)
        if self.magic & 0xFFFFFF00 != MAGIC_BASE:
            raise ValueError(f"bad magic {self.magic:#010x}")
        self.shader_type = "vertex" if self.magic & 0xFF else "pixel"

        (self.sh_off, self.sh_size,
         self.program_control, self.context_misc,
         *self.data_unk) = u32s(blob, self.off_shader, 8)

        code_start = self.offset + self.sh_off
        self.ucode_bytes = blob[code_start:code_start + self.sh_size]
        if len(self.ucode_bytes) != self.sh_size:
            raise ValueError(
                f"ucode runs past blob end: start={code_start:#x} "
                f"size={self.sh_size:#x} blob={len(blob):#x}")
        self.ucode_dwords = list(struct.unpack(
            f">{len(self.ucode_bytes)//4}I", self.ucode_bytes))

    def strings(self, lo: int, hi: int, minlen: int = 3):
        """ASCII strings in blob[lo:hi] (constant names, compiler version)."""
        out, run, start = [], bytearray(), None
        for i in range(lo, min(hi, len(self.blob))):
            b = self.blob[i]
            if 0x20 <= b < 0x7F:
                if start is None:
                    start = i
                run.append(b)
            else:
                if len(run) >= minlen:
                    out.append((start, run.decode()))
                run, start = bytearray(), None
        if len(run) >= minlen:
            out.append((start, run.decode()))
        return out

    def describe(self) -> str:
        L = []
        L.append(f"magic           {self.magic:#010x}  ({self.shader_type} shader)")
        L.append(f"offset (base)   {self.offset:#x}")
        L.append(f"unk1            {self.unk1[0]:#x} {self.unk1[1]:#x} {self.unk1[2]:#x}")
        L.append(f"off_constants   {self.off_constants:#x}")
        L.append(f"off_shader      {self.off_shader:#x}")
        L.append(f"unk2            {self.unk2[0]:#x} {self.unk2[1]:#x}")
        L.append(f"sh_off          {self.sh_off:#x}   -> ucode at {self.offset + self.sh_off:#x}")
        L.append(f"sh_size         {self.sh_size:#x} ({self.sh_size} bytes, {self.sh_size//12} instr slots)")
        L.append(f"program_control {self.program_control:#010x}   (SQ_PROGRAM_CNTL)")
        L.append(f"context_misc    {self.context_misc:#010x}   (SQ_CONTEXT_MISC)")
        L.append(f"data.unk1       " + " ".join(f"{v:#x}" for v in self.data_unk))
        meta_lo, meta_hi = 9 * 4, self.offset + self.sh_off
        strs = self.strings(meta_lo, meta_hi)
        if strs:
            L.append("metadata strings:")
            for off, s in strs:
                L.append(f"  {off:#06x}  {s!r}")
        return "\n".join(L)


def main():
    for arg in sys.argv[1:]:
        blob = Path(arg).read_bytes()
        print(f"=== {arg} ({len(blob)} bytes) ===")
        print(Container(blob).describe())
        print()


if __name__ == "__main__":
    main()
