#!/usr/bin/env python3
"""Emitter for the XDK-era Xenos shader container format.

Inverse of container.py's parser; layout per docs/container.md, struct
definitions cross-checked against XenosRecomp (shader.h, constant_table.h)
and libxenon xe.c. Produces containers loadable by Xe_LoadShaderFromMemory.

Byte-layout notes captured from the golden corpus (XDK D3DX 2.0.11626.0):
- constant table: CTAB header, ConstantInfo records, then per-constant
  (name string, TypeInfo), then target string, then creator string; strings
  NUL-terminated and padded to 4-byte alignment with 0xAB.
- VS shader-record tail: count word, mystery word (0x290 in corpus, meaning
  TBD), packed VertexElement words ({address:12, usage:4, usageIndex:4} —
  address = ucode slot index of the template vfetch to patch, plus
  undocumented high flag bits), Interpolator words
  ({usageIndex:4, usage:4, reg:4} + high bits), then one 0x1000|slot word
  per interpolator export ALU slot.
- PS tail: field18, output mask, Interpolator words.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

PAD = 0xAB

# D3DXPARAMETER_CLASS / _TYPE / D3DXREGISTER_SET (constant_table.h)
CLASS_SCALAR, CLASS_VECTOR, CLASS_MATRIX_ROWS, CLASS_MATRIX_COLS = 0, 1, 2, 3
CLASS_OBJECT = 4
TYPE_BOOL, TYPE_FLOAT, TYPE_SAMPLER = 1, 3, 10
RS_BOOL, RS_INT4, RS_FLOAT4, RS_SAMPLER = 0, 1, 2, 3

# DeclUsage (shader.h)
USAGE_POSITION, USAGE_NORMAL, USAGE_TEXCOORD, USAGE_COLOR = 0, 3, 5, 10

VERSION_TOKEN = {"vertex": 0xFFFE0300, "pixel": 0xFFFF0300}
TARGET_STR = {"vertex": "vs_3_0", "pixel": "ps_3_0"}
MAGIC = {"vertex": 0x102A1101, "pixel": 0x102A1100}


def _pad4(b: bytearray, pad: int = PAD):
    while len(b) % 4:
        b.append(pad)


@dataclass
class Constant:
    name: str
    register_set: int        # RS_*
    register_index: int
    register_count: int
    param_class: int         # CLASS_*
    param_type: int          # TYPE_*
    rows: int = 1
    columns: int = 1
    elements: int = 1


@dataclass
class VertexElement:
    slot: int                # ucode instruction slot of the template vfetch
    usage: int               # USAGE_*
    usage_index: int = 0
    flags: int = 0           # undocumented high bits seen in corpus

    def word(self) -> int:
        return (self.flags << 20) | (self.usage_index << 16) | \
               (self.usage << 12) | self.slot


@dataclass
class Interpolator:
    reg: int                 # export register oN
    usage: int               # USAGE_*
    usage_index: int = 0
    flags: int = 0           # undocumented high bits seen in corpus

    def word(self) -> int:
        return (self.flags << 12) | (self.reg << 8) | (self.usage << 4) | \
               self.usage_index


def build_constant_table(shader_type: str, constants: list[Constant],
                         creator: str = "2.0.11626.0") -> bytes:
    """D3DX-style big-endian constant table incl. the u32 size wrapper."""
    HDR = 0x1C               # sizeof ConstantTable
    CI = 0x14                # sizeof ConstantInfo
    TI = 0x10                # sizeof TypeInfo
    body = bytearray(HDR + CI * len(constants))
    name_off, ti_off = {}, {}
    for c in constants:
        name_off[c.name] = len(body)
        body.extend(c.name.encode() + b"\0")
        _pad4(body)
        ti_off[c.name] = len(body)
        body.extend(struct.pack(">4H2HI", c.param_class, c.param_type,
                                c.rows, c.columns, c.elements, 0, 0))
        assert len(body) % 4 == 0
    target_off = len(body)
    body.extend(TARGET_STR[shader_type].encode() + b"\0")
    creator_off = len(body)
    body.extend(creator.encode() + b"\0")
    _pad4(body)
    struct.pack_into(">7I", body, 0,
                     HDR, creator_off, VERSION_TOKEN[shader_type],
                     len(constants), HDR if constants else 0,
                     0x00001000, target_off)
    for i, c in enumerate(constants):
        # name, registerSet, registerIndex, registerCount, reserved,
        # typeInfo, defaultValue — 0x14 bytes
        struct.pack_into(">I4H2I", body, HDR + i * CI,
                         name_off[c.name], c.register_set, c.register_index,
                         c.register_count, 0, ti_off[c.name], 0)
    return struct.pack(">I", len(body)) + bytes(body)


def build_container(shader_type: str,
                    ucode: bytes,
                    constants: list[Constant],
                    program_control: int,
                    context_misc: int,
                    interpolators: list[Interpolator],
                    *,
                    field10: int = 0,
                    interp_info_extra: int = 0x2000,
                    # vertex shaders:
                    vs_field18: int = 1,
                    vertex_elements: list[VertexElement] | None = None,
                    vs_tail_count: int | None = None,
                    vs_mystery: int = 0x290,
                    export_slots: list[int] | None = None,
                    # pixel shaders:
                    ps_field18: int = 0x30007,
                    ps_outputs: int = 0x1,
                    creator: str = "2.0.11626.0") -> bytes:
    """Assemble a full shader container around raw big-endian ucode bytes."""
    assert len(ucode) % 12 == 0, "ucode must be whole 96-bit slots"
    ctab = build_constant_table(shader_type, constants, creator)
    shader_off = 0x24 + len(ctab)

    # corpus: 0x2063 for 3 interpolators in both VS and PS; (count<<5) is
    # confirmed by XenosRecomp's decode, low bits 0x3 meaning TBD (observed
    # constant), 0x2000 extra likewise.
    interp_info = interp_info_extra | (len(interpolators) << 5) | 0x3

    rec = bytearray()
    rec.extend(struct.pack(">6I", 0, len(ucode), program_control,
                           context_misc, field10, interp_info))
    if shader_type == "vertex":
        ve = vertex_elements or []
        rec.extend(struct.pack(">3I", vs_field18, len(ve),
                               vs_tail_count if vs_tail_count is not None
                               else len(interpolators)))
        rec.extend(struct.pack(">I", vs_mystery))
        for e in ve:
            rec.extend(struct.pack(">I", e.word()))
        for it in interpolators:
            rec.extend(struct.pack(">I", it.word()))
        for slot in (export_slots or []):
            rec.extend(struct.pack(">I", 0x1000 | slot))
    else:
        rec.extend(struct.pack(">2I", ps_field18, ps_outputs))
        for it in interpolators:
            rec.extend(struct.pack(">I", it.word()))

    virtual_size = shader_off + len(rec)
    out = bytearray()
    out.extend(struct.pack(">9I", MAGIC[shader_type], virtual_size,
                           len(ucode), 0, 0x24, 0, shader_off, 0, 0))
    out.extend(ctab)
    out.extend(rec)
    assert len(out) == virtual_size
    out.extend(ucode)
    return bytes(out)
