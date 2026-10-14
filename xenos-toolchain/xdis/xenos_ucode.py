#!/usr/bin/env python3
# xenos_ucode.py — Xenos (Xbox 360 GPU) shader microcode decoder/disassembler.
#
# Part of the xost (Xenos Open Shader Toolchain) project: an open-source
# toolchain for authoring Xbox 360 GPU shaders for homebrew (libxenon).
#
# All instruction encodings (enums, bitfield layouts) in this file are
# faithfully transcribed from the Xenia project's GPU microcode definitions:
#   xenia/src/xenia/gpu/ucode.h, ucode.cc, shader.h, shader_translator.cc,
#   shader_translator_disasm.cc, xenos.h
#   Copyright 2015-2022 Ben Vanik / the Xenia developers. All rights reserved.
#   Released under the BSD-3-Clause license — see the LICENSE file in the
#   Xenia repository root (reference/xenia/LICENSE) for the full text.
# Parts of Xenia's ucode.h in turn derive from the freedreno project
# (Copyright (c) 2012 Rob Clark, MIT license).
#
# Bit positions follow the C bitfield semantics used by Xenia: fields fill
# from the least-significant bit upward within each uint32_t word.
#
# Disassembly text conventions match shader_translator_disasm.cc so output
# can be diffed against xenia-gpu-shader-compiler.
#
# Pure Python 3.10+ standard library. No dependencies.
#
# Public API:
#   disassemble(dwords: list[int], shader_type: str) -> str
#       shader_type in {"vertex", "pixel"}; dwords are host-order uint32
#       values in ucode order. Returns the full multi-line disassembly.
#   parse_program(dwords: list[int], shader_type: str) -> list[DecodedInstruction]
#       Structured decode; each entry has .kind ('cf'|'alu'|'vfetch'|'tfetch'
#       |'unknown'), .offset (cf index or instruction slot), .opcode (name),
#       .text (disassembly line(s)), .detail (parsed object), .raw (dwords).

from __future__ import annotations

import enum
from dataclasses import dataclass, field as dc_field

__all__ = [
    "disassemble",
    "parse_program",
    "DecodedInstruction",
    "ControlFlowOpcode",
    "AddressingMode",
    "AllocType",
    "FetchOpcode",
    "FetchDestinationSwizzle",
    "AluScalarOpcode",
    "AluVectorOpcode",
    "ExportRegister",
    "TextureFilter",
    "AnisoFilter",
    "FetchOpDimension",
    "SampleLocation",
    "SignedRepeatingFractionMode",
    "VertexFormat",
    "UnpackControlFlowInstructions",
    "IsControlFlowOpcodeExec",
    "DoesControlFlowOpcodeEndShader",
]

# Marker emitted on lines that could not be decoded (unknown opcodes etc.).
UNKNOWN_MARKER = "/* !UNKNOWN */"


# =============================================================================
# Bitfield helpers (C bitfields fill from LSB upward within each uint32_t).
# =============================================================================

def _bits(value: int, lo: int, width: int) -> int:
    """Extract an unsigned bitfield [lo, lo+width) from a 32-bit word."""
    return (value >> lo) & ((1 << width) - 1)


def _sbits(value: int, lo: int, width: int) -> int:
    """Extract a signed (two's-complement) bitfield."""
    raw = _bits(value, lo, width)
    if raw & (1 << (width - 1)):
        raw -= 1 << width
    return raw


def _u32(value: int) -> int:
    return value & 0xFFFFFFFF


# =============================================================================
# Enums — transcribed from ucode.h (names kept).
# =============================================================================

class ControlFlowOpcode(enum.IntEnum):
    """ucode.h: enum class ControlFlowOpcode (line ~92)."""
    kNop = 0
    kExec = 1
    kExecEnd = 2
    kCondExec = 3
    kCondExecEnd = 4
    kCondExecPred = 5
    kCondExecPredEnd = 6
    kLoopStart = 7
    kLoopEnd = 8
    kCondCall = 9
    kReturn = 10
    kCondJmp = 11
    kAlloc = 12
    kCondExecPredClean = 13
    kCondExecPredCleanEnd = 14
    kMarkVsFetchDone = 15


def IsControlFlowOpcodeExec(opcode: ControlFlowOpcode) -> bool:
    return opcode in (
        ControlFlowOpcode.kExec,
        ControlFlowOpcode.kExecEnd,
        ControlFlowOpcode.kCondExec,
        ControlFlowOpcode.kCondExecEnd,
        ControlFlowOpcode.kCondExecPred,
        ControlFlowOpcode.kCondExecPredEnd,
        ControlFlowOpcode.kCondExecPredClean,
        ControlFlowOpcode.kCondExecPredCleanEnd,
    )


def DoesControlFlowOpcodeEndShader(opcode: ControlFlowOpcode) -> bool:
    return opcode in (
        ControlFlowOpcode.kExecEnd,
        ControlFlowOpcode.kCondExecEnd,
        ControlFlowOpcode.kCondExecPredEnd,
        ControlFlowOpcode.kCondExecPredCleanEnd,
    )


class AddressingMode(enum.IntEnum):
    """ucode.h: enum class AddressingMode."""
    kRelative = 0  # aL-based, e.g. c[aL + 5]
    kAbsolute = 1  # a0-based, e.g. c[a0 + 5]


class AllocType(enum.IntEnum):
    """ucode.h: enum class AllocType. Note kVsInterpolators == kPsColors == 2."""
    kNone = 0
    kVsPosition = 1
    kVsInterpolators = 2  # also kPsColors for pixel shaders
    kMemory = 3


kPsColors = AllocType.kVsInterpolators  # alias, value 2


class FetchOpcode(enum.IntEnum):
    """ucode.h: enum class FetchOpcode (line ~531)."""
    kVertexFetch = 0
    kTextureFetch = 1
    kGetTextureBorderColorFrac = 16
    kGetTextureComputedLod = 17
    kGetTextureGradients = 18
    kGetTextureWeights = 19
    kSetTextureLod = 24
    kSetTextureGradientsHorz = 25
    kSetTextureGradientsVert = 26


class FetchDestinationSwizzle(enum.IntEnum):
    """ucode.h: enum class FetchDestinationSwizzle. Absolute components.
    Note: 6 is an invalid/unknown encoding (handled like k0, see
    shader_translator.cc ParseFetchInstructionResult TODO)."""
    kX = 0
    kY = 1
    kZ = 2
    kW = 3
    k0 = 4
    k1 = 5
    kKeep = 7


def GetFetchDestinationComponentSwizzle(swizzle: int, component: int) -> int:
    return (swizzle >> (3 * component)) & 0b111


class AluScalarOpcode(enum.IntEnum):
    """ucode.h: enum class AluScalarOpcode (line ~1006)."""
    kAdds = 0
    kAddsPrev = 1
    kMuls = 2
    kMulsPrev = 3
    kMulsPrev2 = 4
    kMaxs = 5
    kMins = 6
    kSeqs = 7
    kSgts = 8
    kSges = 9
    kSnes = 10
    kFrcs = 11
    kTruncs = 12
    kFloors = 13
    kExp = 14
    kLogc = 15
    kLog = 16
    kRcpc = 17
    kRcpf = 18
    kRcp = 19
    kRsqc = 20
    kRsqf = 21
    kRsq = 22
    kMaxAs = 23
    kMaxAsf = 24
    kSubs = 25
    kSubsPrev = 26
    kSetpEq = 27
    kSetpNe = 28
    kSetpGt = 29
    kSetpGe = 30
    kSetpInv = 31
    kSetpPop = 32
    kSetpClr = 33
    kSetpRstr = 34
    kKillsEq = 35
    kKillsGt = 36
    kKillsGe = 37
    kKillsNe = 38
    kKillsOne = 39
    kSqrt = 40
    # 42..47: one bit of the temp register index is stored in the opcode bit 0.
    kMulsc0 = 42
    kMulsc1 = 43
    kAddsc0 = 44
    kAddsc1 = 45
    kSubsc0 = 46
    kSubsc1 = 47
    kSin = 48
    kCos = 49
    kRetainPrev = 50
    # DECODER FIX: ucode.h leaves 41 and 51-63 undefined, but the 6-bit field
    # can hold them and kAluScalarOpcodeInfos has "opcode_NN" entries for all
    # 64 encodings ("this never fails"). Without these members,
    # AluScalarOpcode(41) raised ValueError and crashed the decoder on such
    # ucode. Placeholder members make the decode total; disassembly text for
    # all previously-decodable programs is unchanged.
    kOpcode41 = 41
    kOpcode51 = 51
    kOpcode52 = 52
    kOpcode53 = 53
    kOpcode54 = 54
    kOpcode55 = 55
    kOpcode56 = 56
    kOpcode57 = 57
    kOpcode58 = 58
    kOpcode59 = 59
    kOpcode60 = 60
    kOpcode61 = 61
    kOpcode62 = 62
    kOpcode63 = 63


class AluVectorOpcode(enum.IntEnum):
    """ucode.h: enum class AluVectorOpcode (line ~1359)."""
    kAdd = 0
    kMul = 1
    kMax = 2
    kMin = 3
    kSeq = 4
    kSgt = 5
    kSge = 6
    kSne = 7
    kFrc = 8
    kTrunc = 9
    kFloor = 10
    kMad = 11
    kCndEq = 12
    kCndGe = 13
    kCndGt = 14
    kDp4 = 15
    kDp3 = 16
    kDp2Add = 17
    kCube = 18
    kMax4 = 19
    kSetpEqPush = 20
    kSetpNePush = 21
    kSetpGtPush = 22
    kSetpGePush = 23
    kKillEq = 24
    kKillGt = 25
    kKillGe = 26
    kKillNe = 27
    kDst = 28
    kMaxA = 29
    # DECODER FIX: same as AluScalarOpcode.kOpcode41 — the 5-bit field can
    # hold 30/31 and kAluVectorOpcodeInfos has "opcode_30"/"opcode_31"
    # entries, but AluVectorOpcode(30) raised ValueError. Placeholder members
    # make the decode total; output for valid programs is unchanged.
    kOpcode30 = 30
    kOpcode31 = 31


class ExportRegister(enum.IntEnum):
    """ucode.h: enum class ExportRegister. (VS and PS namespaces overlap; the
    PS aliases are defined as module constants below.)"""
    kVSInterpolator0 = 0
    kVSInterpolator1 = 1
    kVSInterpolator2 = 2
    kVSInterpolator3 = 3
    kVSInterpolator4 = 4
    kVSInterpolator5 = 5
    kVSInterpolator6 = 6
    kVSInterpolator7 = 7
    kVSInterpolator8 = 8
    kVSInterpolator9 = 9
    kVSInterpolator10 = 10
    kVSInterpolator11 = 11
    kVSInterpolator12 = 12
    kVSInterpolator13 = 13
    kVSInterpolator14 = 14
    kVSInterpolator15 = 15
    kExportAddress = 32
    kExportData0 = 33
    kExportData1 = 34
    kExportData2 = 35
    kExportData3 = 36
    kExportData4 = 37
    kPSDepth = 61
    kVSPosition = 62
    kVSPointSizeEdgeFlagKillVertex = 63


kPSColor0 = 0
kPSColor1 = 1
kPSColor2 = 2
kPSColor3 = 3

# eM# (kExportData) register count.
kMaxMemExportElementCount = 5


# --- xenos.h enums referenced by the fetch instruction fields -----------------

class TextureFilter(enum.IntEnum):
    """xenos.h: enum class TextureFilter."""
    kPoint = 0
    kLinear = 1
    kBaseMap = 2  # mip filter only
    kUseFetchConst = 3


class AnisoFilter(enum.IntEnum):
    """xenos.h: enum class AnisoFilter. Note value 6 is undefined."""
    kDisabled = 0
    kMax_1_1 = 1
    kMax_2_1 = 2
    kMax_4_1 = 3
    kMax_8_1 = 4
    kMax_16_1 = 5
    kUseFetchConst = 7


class ArbitraryFilter(enum.IntEnum):
    """xenos.h: enum class ArbitraryFilter (deprecated in final hardware)."""
    k2x4Sym = 0
    k2x4Asym = 1
    k4x2Sym = 2
    k4x2Asym = 3
    k4x4Sym = 4
    k4x4Asym = 5
    kUseFetchConst = 7


class FetchOpDimension(enum.IntEnum):
    """xenos.h: enum class FetchOpDimension."""
    k1D = 0
    k2D = 1
    k3DOrStacked = 2
    kCube = 3


def GetFetchOpDimensionComponentCount(dimension: int) -> int:
    return {
        FetchOpDimension.k1D: 1,
        FetchOpDimension.k2D: 2,
        FetchOpDimension.k3DOrStacked: 3,
        FetchOpDimension.kCube: 3,
    }[FetchOpDimension(dimension)]


class SampleLocation(enum.IntEnum):
    """xenos.h: enum class SampleLocation."""
    kCentroid = 0
    kCenter = 1


class SignedRepeatingFractionMode(enum.IntEnum):
    """xenos.h: enum class SignedRepeatingFractionMode."""
    kZeroClampMinusOne = 0
    kNoZero = 1


class VertexFormat(enum.IntEnum):
    """xenos.h: enum class VertexFormat."""
    kUndefined = 0
    k_8_8_8_8 = 6
    k_2_10_10_10 = 7
    k_10_11_11 = 16
    k_11_11_10 = 17
    k_16_16 = 25
    k_16_16_16_16 = 26
    k_16_16_FLOAT = 31
    k_16_16_16_16_FLOAT = 32
    k_32 = 33
    k_32_32 = 34
    k_32_32_32_32 = 35
    k_32_FLOAT = 36
    k_32_32_FLOAT = 37
    k_32_32_32_32_FLOAT = 38
    k_32_32_32_FLOAT = 57


# Display names used by shader_translator_disasm.cc (kVertexFetchDataFormats).
_VERTEX_FETCH_DATA_FORMAT_NAMES = {
    6: "FMT_8_8_8_8",
    7: "FMT_2_10_10_10",
    16: "FMT_10_11_11",
    17: "FMT_11_11_10",
    25: "FMT_16_16",
    26: "FMT_16_16_16_16",
    31: "FMT_16_16_FLOAT",
    32: "FMT_16_16_16_16_FLOAT",
    33: "FMT_32",
    34: "FMT_32_32",
    35: "FMT_32_32_32_32",
    36: "FMT_32_FLOAT",
    37: "FMT_32_32_FLOAT",
    38: "FMT_32_32_32_32_FLOAT",
    57: "FMT_32_32_32_FLOAT",
}


# =============================================================================
# Control flow instructions — 48 bits each, packed two per three dwords.
# Bitfield layouts transcribed from ucode.h. Word 0 is 32 bits; word 1 is the
# low 16 bits of the second (unpacked) dword. Within each word, fields fill
# from the LSB upward, exactly as the C bitfields declare them top to bottom.
# =============================================================================

class ControlFlowExecInstruction:
    """ucode.h struct ControlFlowExecInstruction (kExec, kExecEnd).

    Word 0: address_:12, count_:3, is_yield_:1, sequence_:12, vc_hi_:4
    Word 1: vc_lo_:2, pad:7, is_predicate_clean_:1, pad:1, address_mode_:1,
            opcode_:4
    """

    def __init__(self, dword_0: int, dword_1: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)

    def opcode(self): return ControlFlowOpcode(_bits(self.dword_1, 12, 4))
    def addressing_mode(self): return AddressingMode(_bits(self.dword_1, 11, 1))
    def address(self): return _bits(self.dword_0, 0, 12)
    def count(self): return _bits(self.dword_0, 12, 3)
    def is_yield(self): return _bits(self.dword_0, 15, 1) == 1
    # Sequence bits, 2 per instruction: [0] - ALU (0) or fetch (1),
    # [1] - serialize.
    def sequence(self): return _bits(self.dword_0, 16, 12)
    def vc_hi(self): return _bits(self.dword_0, 28, 4)  # Vertex cache?
    def vc_lo(self): return _bits(self.dword_1, 0, 2)
    def is_predicate_clean(self): return _bits(self.dword_1, 9, 1) == 1


class ControlFlowCondExecInstruction:
    """ucode.h struct ControlFlowCondExecInstruction (kCondExec, kCondExecEnd,
    kCondExecPredClean, kCondExecPredCleanEnd).

    Word 0: address_:12, count_:3, is_yield_:1, sequence_:12, vc_hi_:4
    Word 1: vc_lo_:2, bool_address_:8, condition_:1, address_mode_:1, opcode_:4
    """

    def __init__(self, dword_0: int, dword_1: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)

    def opcode(self): return ControlFlowOpcode(_bits(self.dword_1, 12, 4))
    def addressing_mode(self): return AddressingMode(_bits(self.dword_1, 11, 1))
    def address(self): return _bits(self.dword_0, 0, 12)
    def count(self): return _bits(self.dword_0, 12, 3)
    def is_yield(self): return _bits(self.dword_0, 15, 1) == 1
    def sequence(self): return _bits(self.dword_0, 16, 12)
    def vc_hi(self): return _bits(self.dword_0, 28, 4)
    def vc_lo(self): return _bits(self.dword_1, 0, 2)
    def bool_address(self): return _bits(self.dword_1, 2, 8)
    def condition(self): return _bits(self.dword_1, 10, 1) == 1


class ControlFlowCondExecPredInstruction:
    """ucode.h struct ControlFlowCondExecPredInstruction (kCondExecPred,
    kCondExecPredEnd).

    Word 0: address_:12, count_:3, is_yield_:1, sequence_:12, vc_hi_:4
    Word 1: vc_lo_:2, pad:7, is_predicate_clean_:1, condition_:1,
            address_mode_:1, opcode_:4
    """

    def __init__(self, dword_0: int, dword_1: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)

    def opcode(self): return ControlFlowOpcode(_bits(self.dword_1, 12, 4))
    def addressing_mode(self): return AddressingMode(_bits(self.dword_1, 11, 1))
    def address(self): return _bits(self.dword_0, 0, 12)
    def count(self): return _bits(self.dword_0, 12, 3)
    def is_yield(self): return _bits(self.dword_0, 15, 1) == 1
    def sequence(self): return _bits(self.dword_0, 16, 12)
    def vc_hi(self): return _bits(self.dword_0, 28, 4)
    def vc_lo(self): return _bits(self.dword_1, 0, 2)
    def is_predicate_clean(self): return _bits(self.dword_1, 9, 1) == 1
    def condition(self): return _bits(self.dword_1, 10, 1) == 1


class ControlFlowLoopStartInstruction:
    """ucode.h struct ControlFlowLoopStartInstruction (kLoopStart).

    Word 0: address_:13, is_repeat_:1, pad:2, loop_id_:5, pad:11
    Word 1: pad:11, address_mode_:1, opcode_:4
    """

    def __init__(self, dword_0: int, dword_1: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)

    def opcode(self): return ControlFlowOpcode(_bits(self.dword_1, 12, 4))
    def addressing_mode(self): return AddressingMode(_bits(self.dword_1, 11, 1))
    def address(self): return _bits(self.dword_0, 0, 13)
    def is_repeat(self): return _bits(self.dword_0, 13, 1) == 1
    def loop_id(self): return _bits(self.dword_0, 16, 5)


class ControlFlowLoopEndInstruction:
    """ucode.h struct ControlFlowLoopEndInstruction (kLoopEnd).

    Word 0: address_:13, pad:3, loop_id_:5, is_predicated_break_:1, pad:10
    Word 1: pad:10, condition_:1, address_mode_:1, opcode_:4
    """

    def __init__(self, dword_0: int, dword_1: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)

    def opcode(self): return ControlFlowOpcode(_bits(self.dword_1, 12, 4))
    def addressing_mode(self): return AddressingMode(_bits(self.dword_1, 11, 1))
    def address(self): return _bits(self.dword_0, 0, 13)
    def loop_id(self): return _bits(self.dword_0, 16, 5)
    def is_predicated_break(self): return _bits(self.dword_0, 21, 1) == 1
    def condition(self): return _bits(self.dword_1, 10, 1) == 1


class ControlFlowCondCallInstruction:
    """ucode.h struct ControlFlowCondCallInstruction (kCondCall).

    Word 0: address_:13, is_unconditional_:1, is_predicated_:1, pad:17
    Word 1: pad:2, bool_address_:8, condition_:1, address_mode_:1, opcode_:4
    """

    def __init__(self, dword_0: int, dword_1: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)

    def opcode(self): return ControlFlowOpcode(_bits(self.dword_1, 12, 4))
    def addressing_mode(self): return AddressingMode(_bits(self.dword_1, 11, 1))
    def address(self): return _bits(self.dword_0, 0, 13)
    def is_unconditional(self): return _bits(self.dword_0, 13, 1) == 1
    def is_predicated(self): return _bits(self.dword_0, 14, 1) == 1
    def bool_address(self): return _bits(self.dword_1, 2, 8)
    def condition(self): return _bits(self.dword_1, 10, 1) == 1


class ControlFlowReturnInstruction:
    """ucode.h struct ControlFlowReturnInstruction (kReturn).

    Word 0: pad:32
    Word 1: pad:11, address_mode_:1, opcode_:4
    """

    def __init__(self, dword_0: int, dword_1: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)

    def opcode(self): return ControlFlowOpcode(_bits(self.dword_1, 12, 4))
    def addressing_mode(self): return AddressingMode(_bits(self.dword_1, 11, 1))


class ControlFlowCondJmpInstruction:
    """ucode.h struct ControlFlowCondJmpInstruction (kCondJmp).

    Word 0: address_:13, is_unconditional_:1, is_predicated_:1, pad:17
    Word 1: pad:1, direction_:1, bool_address_:8, condition_:1,
            address_mode_:1, opcode_:4
    """

    def __init__(self, dword_0: int, dword_1: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)

    def opcode(self): return ControlFlowOpcode(_bits(self.dword_1, 12, 4))
    def addressing_mode(self): return AddressingMode(_bits(self.dword_1, 11, 1))
    def address(self): return _bits(self.dword_0, 0, 13)
    def is_unconditional(self): return _bits(self.dword_0, 13, 1) == 1
    def is_predicated(self): return _bits(self.dword_0, 14, 1) == 1
    def direction(self): return _bits(self.dword_1, 1, 1)
    def bool_address(self): return _bits(self.dword_1, 2, 8)
    def condition(self): return _bits(self.dword_1, 10, 1) == 1


class ControlFlowAllocInstruction:
    """ucode.h struct ControlFlowAllocInstruction (kAlloc).

    Word 0: size_:3, pad:29
    Word 1: pad:8, is_unserialized_:1, alloc_type_:2, pad:1, opcode_:4
    """

    def __init__(self, dword_0: int, dword_1: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)

    def opcode(self): return ControlFlowOpcode(_bits(self.dword_1, 12, 4))
    def size(self): return _bits(self.dword_0, 0, 3)
    def is_unserialized(self): return _bits(self.dword_1, 8, 1) == 1
    def alloc_type(self): return AllocType(_bits(self.dword_1, 9, 2))


class ControlFlowInstruction:
    """ucode.h union ControlFlowInstruction — 48 bits in (dword_0, dword_1)."""

    def __init__(self, dword_0: int, dword_1: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1) & 0xFFFF

    def opcode(self) -> ControlFlowOpcode:
        return ControlFlowOpcode(_bits(self.dword_1, 12, 4))

    # Typed views (mirroring the C++ union members).
    @property
    def exec(self): return ControlFlowExecInstruction(self.dword_0, self.dword_1)
    @property
    def cond_exec(self): return ControlFlowCondExecInstruction(self.dword_0, self.dword_1)
    @property
    def cond_exec_pred(self): return ControlFlowCondExecPredInstruction(self.dword_0, self.dword_1)
    @property
    def loop_start(self): return ControlFlowLoopStartInstruction(self.dword_0, self.dword_1)
    @property
    def loop_end(self): return ControlFlowLoopEndInstruction(self.dword_0, self.dword_1)
    @property
    def cond_call(self): return ControlFlowCondCallInstruction(self.dword_0, self.dword_1)
    @property
    def ret(self): return ControlFlowReturnInstruction(self.dword_0, self.dword_1)
    @property
    def cond_jmp(self): return ControlFlowCondJmpInstruction(self.dword_0, self.dword_1)
    @property
    def alloc(self): return ControlFlowAllocInstruction(self.dword_0, self.dword_1)


def UnpackControlFlowInstructions(dwords) -> tuple:
    """ucode.h inline UnpackControlFlowInstructions: two 48-bit CF
    instructions packed into three dwords."""
    dword_0 = _u32(dwords[0])
    dword_1 = _u32(dwords[1])
    dword_2 = _u32(dwords[2])
    cf_a = ControlFlowInstruction(dword_0, dword_1 & 0xFFFF)
    cf_b = ControlFlowInstruction(_u32((dword_1 >> 16) | (dword_2 << 16)),
                                  dword_2 >> 16)
    return cf_a, cf_b


# =============================================================================
# Fetch instructions — 96 bits (three dwords).
# =============================================================================

class VertexFetchInstruction:
    """ucode.h struct VertexFetchInstruction.

    Dword 0: opcode_value:5, src_reg:6, src_reg_am:1, dst_reg:6, dst_reg_am:1,
             must_be_one:1, const_index:5, const_index_sel:2,
             prefetch_count:3, src_swiz:2
    Dword 1: dst_swiz:12, fomat_comp_all:1, num_format_all:1,
             signed_rf_mode_all:1, is_index_rounded:1, format:6, reserved2:2,
             exp_adjust:6 (signed), is_mini_fetch:1, is_predicated:1
    Dword 2: stride:8, offset:23 (signed), pred_condition:1
    """

    def __init__(self, dword_0: int, dword_1: int, dword_2: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)
        self.dword_2 = _u32(dword_2)

    def opcode(self):
        return _bits(self.dword_0, 0, 5)  # FetchOpcode (raw int; may be invalid)

    def is_predicated(self): return _bits(self.dword_1, 31, 1) == 1
    def predicate_condition(self): return _bits(self.dword_2, 31, 1) == 1

    def fetch_constant_index(self):
        # Vertex fetch constant index [0-95] (vfetch_full only).
        return _bits(self.dword_0, 20, 5) * 3 + _bits(self.dword_0, 25, 2)

    def const_index(self): return _bits(self.dword_0, 20, 5)
    def const_index_sel(self): return _bits(self.dword_0, 25, 2)
    def must_be_one(self): return _bits(self.dword_0, 19, 1)

    def dest(self): return _bits(self.dword_0, 12, 6)
    def dest_swizzle(self): return _bits(self.dword_1, 0, 12)
    def is_dest_relative(self): return _bits(self.dword_0, 18, 1) == 1
    def src(self): return _bits(self.dword_0, 5, 6)
    def src_swizzle(self): return _bits(self.dword_0, 30, 2)
    def is_src_relative(self): return _bits(self.dword_0, 11, 1) == 1

    def prefetch_count(self): return _bits(self.dword_0, 27, 3)  # minus 1
    def is_mini_fetch(self): return _bits(self.dword_1, 30, 1) == 1

    def data_format(self): return _bits(self.dword_1, 16, 6)  # xenos::VertexFormat
    def exp_adjust(self): return _sbits(self.dword_1, 24, 6)  # [-32, 31]
    def is_signed(self): return _bits(self.dword_1, 12, 1) == 1  # fomat_comp_all
    def is_normalized(self): return _bits(self.dword_1, 13, 1) == 0  # num_format_all
    def signed_rf_mode(self):
        return SignedRepeatingFractionMode(_bits(self.dword_1, 14, 1))
    def is_index_rounded(self): return _bits(self.dword_1, 15, 1) == 1
    def stride(self): return _bits(self.dword_2, 0, 8)  # dwords, [0, 255]
    def offset(self): return _sbits(self.dword_2, 8, 23)  # dwords, signed


class TextureFetchInstruction:
    """ucode.h struct TextureFetchInstruction.

    Dword 0: opcode_value:5, src_reg:6, src_reg_am:1, dst_reg:6, dst_reg_am:1,
             fetch_valid_only:1, const_index:5, tx_coord_denorm:1, src_swiz:6
    Dword 1: dst_swiz:12, mag_filter:2, min_filter:2, mip_filter:2,
             aniso_filter:3, arbitrary_filter:3, vol_mag_filter:2,
             vol_min_filter:2, use_comp_lod:1, use_reg_lod:1, unk:1,
             is_predicated:1
    Dword 2: use_reg_gradients:1, sample_location:1, lod_bias:7 (signed),
             unused:5, dimension:2, offset_x:5 (signed), offset_y:5 (signed),
             offset_z:5 (signed), pred_condition:1
    """

    def __init__(self, dword_0: int, dword_1: int, dword_2: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)
        self.dword_2 = _u32(dword_2)

    def opcode(self):
        return _bits(self.dword_0, 0, 5)  # FetchOpcode (raw int; may be invalid)

    def is_predicated(self): return _bits(self.dword_1, 31, 1) == 1
    def predicate_condition(self): return _bits(self.dword_2, 31, 1) == 1
    def fetch_constant_index(self): return _bits(self.dword_0, 20, 5)  # [0-31]

    def dest(self): return _bits(self.dword_0, 12, 6)
    def dest_swizzle(self): return _bits(self.dword_1, 0, 12)
    def is_dest_relative(self): return _bits(self.dword_0, 18, 1) == 1
    def src(self): return _bits(self.dword_0, 5, 6)
    def src_swizzle(self): return _bits(self.dword_0, 26, 6)  # 3 components
    def is_src_relative(self): return _bits(self.dword_0, 11, 1) == 1

    def dimension(self): return FetchOpDimension(_bits(self.dword_2, 14, 2))
    def fetch_valid_only(self): return _bits(self.dword_0, 19, 1) == 1
    def unnormalized_coordinates(self): return _bits(self.dword_0, 25, 1) == 1
    def mag_filter(self): return TextureFilter(_bits(self.dword_1, 12, 2))
    def min_filter(self): return TextureFilter(_bits(self.dword_1, 14, 2))
    def mip_filter(self): return TextureFilter(_bits(self.dword_1, 16, 2))
    def aniso_filter(self): return _bits(self.dword_1, 18, 3)  # xenos::AnisoFilter
    def arbitrary_filter(self): return _bits(self.dword_1, 21, 3)
    def vol_mag_filter(self): return TextureFilter(_bits(self.dword_1, 24, 2))
    def vol_min_filter(self): return TextureFilter(_bits(self.dword_1, 26, 2))
    def use_computed_lod(self): return _bits(self.dword_1, 28, 1) == 1
    def use_register_lod(self): return _bits(self.dword_1, 29, 1) == 1
    def use_register_gradients(self): return _bits(self.dword_2, 0, 1) == 1
    def sample_location(self): return SampleLocation(_bits(self.dword_2, 1, 1))
    def lod_bias(self): return _sbits(self.dword_2, 2, 7) * (1.0 / 16.0)
    def offset_x(self): return _sbits(self.dword_2, 16, 5) * 0.5
    def offset_y(self): return _sbits(self.dword_2, 21, 5) * 0.5
    def offset_z(self): return _sbits(self.dword_2, 26, 5) * 0.5


# =============================================================================
# ALU instructions — 96 bits (three dwords).
# =============================================================================

class AluInstruction:
    """ucode.h struct AluInstruction.

    Dword 0: vector_dest:6, vector_dest_rel:1, abs_constants:1, scalar_dest:6,
             scalar_dest_rel:1, export_data:1, vector_write_mask:4,
             scalar_write_mask:4, vector_clamp:1, scalar_clamp:1, scalar_opc:6
    Dword 1: src3_swiz:8, src2_swiz:8, src1_swiz:8, src3_reg_negate:1,
             src2_reg_negate:1, src1_reg_negate:1, pred_condition:1,
             is_predicated:1, const_address_register_relative:1,
             const_1_rel_abs:1, const_0_rel_abs:1
    Dword 2: src3_reg:8, src2_reg:8, src1_reg:8, vector_opc:5, src3_sel:1,
             src2_sel:1, src1_sel:1
    """

    def __init__(self, dword_0: int, dword_1: int, dword_2: int):
        self.dword_0 = _u32(dword_0)
        self.dword_1 = _u32(dword_1)
        self.dword_2 = _u32(dword_2)

    def is_export(self): return _bits(self.dword_0, 15, 1) == 1
    def is_predicated(self): return _bits(self.dword_1, 28, 1) == 1
    def predicate_condition(self): return _bits(self.dword_1, 27, 1) == 1

    def abs_constants(self): return _bits(self.dword_0, 7, 1) == 1
    def is_const_0_addressed(self): return _bits(self.dword_1, 31, 1) == 1
    def is_const_1_addressed(self): return _bits(self.dword_1, 30, 1) == 1
    def is_const_address_register_relative(self):
        return _bits(self.dword_1, 29, 1) == 1

    def vector_opcode(self): return AluVectorOpcode(_bits(self.dword_2, 24, 5))
    def vector_write_mask(self): return _bits(self.dword_0, 16, 4)
    def vector_dest(self): return _bits(self.dword_0, 0, 6)
    def is_vector_dest_relative(self): return _bits(self.dword_0, 6, 1) == 1
    def vector_clamp(self): return _bits(self.dword_0, 24, 1) == 1

    def scalar_opcode(self): return AluScalarOpcode(_bits(self.dword_0, 26, 6))
    def scalar_opcode_raw(self): return _bits(self.dword_0, 26, 6)
    def scalar_write_mask(self): return _bits(self.dword_0, 20, 4)
    def scalar_dest(self): return _bits(self.dword_0, 8, 6)
    def is_scalar_dest_relative(self): return _bits(self.dword_0, 14, 1) == 1
    def scalar_clamp(self): return _bits(self.dword_0, 25, 1) == 1

    @staticmethod
    def src_temp_reg(src_reg: int) -> int:
        return src_reg & 0x3F

    @staticmethod
    def is_src_temp_relative(src_reg: int) -> bool:
        return (src_reg & 0x40) != 0

    @staticmethod
    def is_src_temp_value_absolute(src_reg: int) -> bool:
        return (src_reg & 0x80) != 0

    def src_reg(self, i: int) -> int:
        if i == 1:
            return _bits(self.dword_2, 16, 8)
        if i == 2:
            return _bits(self.dword_2, 8, 8)
        if i == 3:
            return _bits(self.dword_2, 0, 8)
        raise ValueError(f"bad ALU source index {i}")

    def src_is_temp(self, i: int) -> bool:
        if i == 1:
            return _bits(self.dword_2, 31, 1) != 0
        if i == 2:
            return _bits(self.dword_2, 30, 1) != 0
        if i == 3:
            return _bits(self.dword_2, 29, 1) != 0
        raise ValueError(f"bad ALU source index {i}")

    def src_const_is_addressed(self, i: int) -> bool:
        # See the long comment in ucode.h about const_0_rel_abs/const_1_rel_abs
        # selection: whether to use const_0_rel_abs or const_1_rel_abs is
        # essentially min(sum of whether the previous operands are constants,
        # 1). (Note: the XNA *disassembler* takes the addressing mode for the
        # constant scalar operand of mulsc/addsc/subsc unconditionally from
        # const_1_rel_abs, but that is most likely an XNA bug; Xenia, and this
        # transcription, use the same selection rule as for vector operands.)
        if i == 1:
            return self.is_const_0_addressed()
        if i == 2:
            return (self.is_const_0_addressed() if self.src_is_temp(1)
                    else self.is_const_1_addressed())
        if i == 3:
            return (self.is_const_0_addressed()
                    if (self.src_is_temp(1) and self.src_is_temp(2))
                    else self.is_const_1_addressed())
        raise ValueError(f"bad ALU source index {i}")

    def src_swizzle(self, i: int) -> int:
        if i == 1:
            return _bits(self.dword_1, 16, 8)
        if i == 2:
            return _bits(self.dword_1, 8, 8)
        if i == 3:
            return _bits(self.dword_1, 0, 8)
        raise ValueError(f"bad ALU source index {i}")

    def src_negate(self, i: int) -> bool:
        if i == 1:
            return _bits(self.dword_1, 26, 1) == 1
        if i == 2:
            return _bits(self.dword_1, 25, 1) == 1
        if i == 3:
            return _bits(self.dword_1, 24, 1) == 1
        raise ValueError(f"bad ALU source index {i}")

    def src3_sel(self): return _bits(self.dword_2, 29, 1)

    def scalar_const_reg_op_src_temp_reg(self) -> int:
        # For mulsc/addsc/subsc: temp register index constructed from
        # [0:0] = scalar_opcode[0:0], [1:1] = src3_sel[0:0],
        # [2:5] = src3_swizzle[2:5].
        return ((self.scalar_opcode_raw() & 1) | (self.src3_sel() << 1) |
                (self.src_swizzle(3) & 0x3C))

    @staticmethod
    def GetSwizzledComponentIndex(swizzle: int, component_index: int) -> int:
        # ALU source swizzles are component-relative.
        return ((swizzle >> (2 * component_index)) + component_index) & 3

    def GetVectorOpResultWriteMask(self) -> int:
        mask = self.vector_write_mask()
        if self.is_export():
            mask &= ~self.scalar_write_mask() & 0b1111
        return mask

    def GetScalarOpResultWriteMask(self) -> int:
        mask = self.scalar_write_mask()
        if self.is_export():
            mask &= ~self.vector_write_mask() & 0b1111
        return mask

    def GetConstant0WriteMask(self) -> int:
        if not self.is_export() or not self.is_scalar_dest_relative():
            return 0b0000
        return 0b1111 & ~(self.vector_write_mask() | self.scalar_write_mask())

    def GetConstant1WriteMask(self) -> int:
        if not self.is_export():
            return 0b0000
        return self.vector_write_mask() & self.scalar_write_mask()


# =============================================================================
# Opcode info tables — transcribed from ucode.cc.
# =============================================================================

# (name, operand_count, single_operand_is_two_component)
# 6 scalar opcode bits - 64 entries.
kAluScalarOpcodeInfos = (
    ("adds", 1, True),
    ("adds_prev", 1, False),
    ("muls", 1, True),
    ("muls_prev", 1, False),
    ("muls_prev2", 1, True),
    ("maxs", 1, True),
    ("mins", 1, True),
    ("seqs", 1, False),
    ("sgts", 1, False),
    ("sges", 1, False),
    ("snes", 1, False),
    ("frcs", 1, False),
    ("truncs", 1, False),
    ("floors", 1, False),
    ("exp", 1, False),
    ("logc", 1, False),
    ("log", 1, False),
    ("rcpc", 1, False),
    ("rcpf", 1, False),
    ("rcp", 1, False),
    ("rsqc", 1, False),
    ("rsqf", 1, False),
    ("rsq", 1, False),
    ("maxas", 1, True),
    ("maxasf", 1, True),
    ("subs", 1, True),
    ("subs_prev", 1, False),
    ("setp_eq", 1, False),
    ("setp_ne", 1, False),
    ("setp_gt", 1, False),
    ("setp_ge", 1, False),
    ("setp_inv", 1, False),
    ("setp_pop", 1, False),
    ("setp_clr", 0, False),
    ("setp_rstr", 1, False),
    ("kills_eq", 1, False),
    ("kills_gt", 1, False),
    ("kills_ge", 1, False),
    ("kills_ne", 1, False),
    ("kills_one", 1, False),
    ("sqrt", 1, False),
    ("opcode_41", 0, False),
    ("mulsc", 2, False),
    ("mulsc", 2, False),
    ("addsc", 2, False),
    ("addsc", 2, False),
    ("subsc", 2, False),
    ("subsc", 2, False),
    ("sin", 1, False),
    ("cos", 1, False),
    ("retain_prev", 0, False),
    ("opcode_51", 0, False),
    ("opcode_52", 0, False),
    ("opcode_53", 0, False),
    ("opcode_54", 0, False),
    ("opcode_55", 0, False),
    ("opcode_56", 0, False),
    ("opcode_57", 0, False),
    ("opcode_58", 0, False),
    ("opcode_59", 0, False),
    ("opcode_60", 0, False),
    ("opcode_61", 0, False),
    ("opcode_62", 0, False),
    ("opcode_63", 0, False),
)

# (name, operand_count) — operand counts derived from ucode.cc
# operand_components_used (number of non-zero entries).
# 5 vector opcode bits - 32 entries.
kAluVectorOpcodeInfos = (
    ("add", 2),
    ("mul", 2),
    ("max", 2),
    ("min", 2),
    ("seq", 2),
    ("sgt", 2),
    ("sge", 2),
    ("sne", 2),
    ("frc", 1),
    ("trunc", 1),
    ("floor", 1),
    ("mad", 3),
    ("cndeq", 3),
    ("cndge", 3),
    ("cndgt", 3),
    ("dp4", 2),
    ("dp3", 2),
    ("dp2add", 3),
    ("cube", 2),
    ("max4", 1),
    ("setp_eq_push", 2),
    ("setp_ne_push", 2),
    ("setp_gt_push", 2),
    ("setp_ge_push", 2),
    ("kill_eq", 2),
    ("kill_gt", 2),
    ("kill_ge", 2),
    ("kill_ne", 2),
    ("dst", 2),
    ("maxa", 2),
    ("opcode_30", 0),
    ("opcode_31", 0),
)


# =============================================================================
# Parsed instruction layer — mirrors shader.h / shader_translator.cc.
# =============================================================================

class SwizzleSource(enum.IntEnum):
    """shader.h: enum class SwizzleSource."""
    kX = 0
    kY = 1
    kZ = 2
    kW = 3
    k0 = 4
    k1 = 5


def GetSwizzleFromComponentIndex(i: int) -> SwizzleSource:
    return SwizzleSource(i)


def GetSwizzledAluSourceComponent(swizzle: int, component_index: int) -> SwizzleSource:
    return GetSwizzleFromComponentIndex(
        AluInstruction.GetSwizzledComponentIndex(swizzle, component_index))


_SWIZZLE_CHARS = "xyzw01"


def GetCharForSwizzle(swizzle_source: SwizzleSource) -> str:
    return _SWIZZLE_CHARS[int(swizzle_source)]


class InstructionStorageTarget(enum.IntEnum):
    """shader.h: enum class InstructionStorageTarget."""
    kNone = 0
    kRegister = 1
    kInterpolator = 2
    kPosition = 3
    kPointSizeEdgeFlagKillVertex = 4
    kExportAddress = 5
    kExportData = 6
    kColor = 7
    kDepth = 8


def GetInstructionStorageTargetUsedComponentCount(target) -> int:
    if target == InstructionStorageTarget.kNone:
        return 0
    if target == InstructionStorageTarget.kPointSizeEdgeFlagKillVertex:
        return 3
    if target == InstructionStorageTarget.kDepth:
        return 1
    return 4


class InstructionStorageAddressingMode(enum.IntEnum):
    """shader.h: enum class InstructionStorageAddressingMode."""
    kAbsolute = 0
    kAddressRegisterRelative = 1  # a0
    kLoopRelative = 2  # aL


class InstructionStorageSource(enum.IntEnum):
    """shader.h: enum class InstructionStorageSource."""
    kRegister = 0
    kConstantFloat = 1
    kVertexFetchConstant = 2
    kTextureFetchConstant = 3


@dataclass
class InstructionResult:
    """shader.h: struct InstructionResult."""
    storage_target: InstructionStorageTarget = InstructionStorageTarget.kNone
    storage_index: int = 0
    storage_addressing_mode: InstructionStorageAddressingMode = \
        InstructionStorageAddressingMode.kAbsolute
    is_clamped: bool = False
    original_write_mask: int = 0b0000
    components: list = dc_field(default_factory=lambda: [
        SwizzleSource.kX, SwizzleSource.kY, SwizzleSource.kZ, SwizzleSource.kW])

    def GetUsedWriteMask(self) -> int:
        target_component_count = \
            GetInstructionStorageTargetUsedComponentCount(self.storage_target)
        return self.original_write_mask & ((1 << target_component_count) - 1)

    def IsStandardSwizzle(self) -> bool:
        return (self.GetUsedWriteMask() == 0b1111 and
                self.components[0] == SwizzleSource.kX and
                self.components[1] == SwizzleSource.kY and
                self.components[2] == SwizzleSource.kZ and
                self.components[3] == SwizzleSource.kW)

    def GetUsedResultComponents(self) -> int:
        used_write_mask = self.GetUsedWriteMask()
        used_components = 0b0000
        for i in range(4):
            if (used_write_mask & (1 << i)) and \
                    SwizzleSource.kX <= self.components[i] <= SwizzleSource.kW:
                used_components |= 1 << (int(self.components[i]) -
                                         int(SwizzleSource.kX))
        return used_components


@dataclass
class InstructionOperand:
    """shader.h: struct InstructionOperand."""
    storage_source: InstructionStorageSource = InstructionStorageSource.kRegister
    storage_index: int = 0
    storage_addressing_mode: InstructionStorageAddressingMode = \
        InstructionStorageAddressingMode.kAbsolute
    is_negated: bool = False
    is_absolute_value: bool = False
    component_count: int = 4
    components: list = dc_field(default_factory=lambda: [
        SwizzleSource.kX, SwizzleSource.kY, SwizzleSource.kZ, SwizzleSource.kW])

    def IsStandardSwizzle(self) -> bool:
        if self.component_count == 4:
            return (self.components[0] == SwizzleSource.kX and
                    self.components[1] == SwizzleSource.kY and
                    self.components[2] == SwizzleSource.kZ and
                    self.components[3] == SwizzleSource.kW)
        return False


# --- Text formatting (shader_translator_disasm.cc) ---------------------------

def _disassemble_result_operand(result: InstructionResult) -> str:
    out = []
    uses_storage_index = False
    t = result.storage_target
    if t == InstructionStorageTarget.kRegister:
        out.append("r")
        uses_storage_index = True
    elif t == InstructionStorageTarget.kInterpolator:
        out.append("o")
        uses_storage_index = True
    elif t == InstructionStorageTarget.kPosition:
        out.append("oPos")
    elif t == InstructionStorageTarget.kPointSizeEdgeFlagKillVertex:
        out.append("oPts")
    elif t == InstructionStorageTarget.kExportAddress:
        out.append("eA")
    elif t == InstructionStorageTarget.kExportData:
        out.append("eM")
        uses_storage_index = True
    elif t == InstructionStorageTarget.kColor:
        out.append("oC")
        uses_storage_index = True
    elif t == InstructionStorageTarget.kDepth:
        out.append("oDepth")
    # kNone: nothing.
    if uses_storage_index:
        m = result.storage_addressing_mode
        if m == InstructionStorageAddressingMode.kAbsolute:
            out.append(f"{result.storage_index}")
        elif m == InstructionStorageAddressingMode.kAddressRegisterRelative:
            out.append(f"[{result.storage_index}+a0]")
        else:
            out.append(f"[{result.storage_index}+aL]")
    # Not using GetUsedWriteMask/IsStandardSwizzle here because they filter
    # out components without runtime effect that are still in the microcode.
    if not result.original_write_mask:
        out.append("._")
    elif (result.original_write_mask != 0b1111 or
          result.components[0] != SwizzleSource.kX or
          result.components[1] != SwizzleSource.kY or
          result.components[2] != SwizzleSource.kZ or
          result.components[3] != SwizzleSource.kW):
        out.append(".")
        for i in range(4):
            if result.original_write_mask & (1 << i):
                out.append(GetCharForSwizzle(result.components[i]))
            else:
                out.append("_")
    return "".join(out)


def _disassemble_source_operand(op: InstructionOperand) -> str:
    out = []
    if op.is_negated:
        out.append("-")
    s = op.storage_source
    if s == InstructionStorageSource.kRegister:
        out.append("r")
    elif s == InstructionStorageSource.kConstantFloat:
        out.append("c")
    # kTextureFetchConstant / kVertexFetchConstant never reach here.
    if op.is_absolute_value:
        out.append("_abs")
    m = op.storage_addressing_mode
    if m == InstructionStorageAddressingMode.kAbsolute:
        if op.is_absolute_value:
            out.append(f"[{op.storage_index}]")
        else:
            out.append(f"{op.storage_index}")
    elif m == InstructionStorageAddressingMode.kAddressRegisterRelative:
        out.append(f"[{op.storage_index}+a0]")
    else:
        out.append(f"[{op.storage_index}+aL]")
    if not op.IsStandardSwizzle():
        out.append(".")
        for j in range(op.component_count):
            out.append(GetCharForSwizzle(op.components[j]))
    return "".join(out)


def _predication_prefix(is_predicated: bool, predicate_condition: bool) -> str:
    if is_predicated:
        return " (p0) " if predicate_condition else "(!p0) "
    return "      "


# --- Parsed control flow instructions ----------------------------------------

class _ExecType(enum.IntEnum):
    kUnconditional = 0
    kConditional = 1
    kPredicated = 2


@dataclass
class ParsedExecInstruction:
    dword_index: int = 0
    opcode: ControlFlowOpcode = ControlFlowOpcode.kExec
    opcode_name: str = ""
    instruction_address: int = 0
    instruction_count: int = 0
    type: _ExecType = _ExecType.kUnconditional
    bool_constant_index: int = 0
    condition: bool = False
    is_end: bool = False
    is_predicate_clean: bool = True
    is_yield: bool = False
    sequence: int = 0

    def Disassemble(self) -> str:
        out = []
        if self.type == _ExecType.kUnconditional:
            out.append(f"      {self.opcode_name}")
        elif self.type == _ExecType.kPredicated:
            out.append(" (p0) " if self.condition else "(!p0) ")
            out.append(self.opcode_name)
        else:  # kConditional
            out.append(f"      {self.opcode_name} "
                       f"{'' if self.condition else '!'}b{self.bool_constant_index}")
        if self.is_yield:
            if self.type == _ExecType.kConditional:
                out.append(",")
            out.append(" Yield=true")
        if not self.is_predicate_clean:
            out.append("    // PredicateClean=false")
        out.append("\n")
        return "".join(out)


@dataclass
class ParsedLoopStartInstruction:
    dword_index: int = 0
    loop_constant_index: int = 0
    is_repeat: bool = False
    loop_skip_address: int = 0

    def Disassemble(self) -> str:
        out = ["      loop ",
               f"i{self.loop_constant_index}, L{self.loop_skip_address}"]
        if self.is_repeat:
            out.append(", Repeat=true")
        out.append("\n")
        return "".join(out)


@dataclass
class ParsedLoopEndInstruction:
    dword_index: int = 0
    is_predicated_break: bool = False
    predicate_condition: bool = False
    loop_constant_index: int = 0
    loop_body_address: int = 0

    def Disassemble(self) -> str:
        out = []
        if self.is_predicated_break:
            out.append(" (p0) " if self.predicate_condition else "(!p0) ")
        else:
            out.append("      ")
        out.append(f"endloop i{self.loop_constant_index}, "
                   f"L{self.loop_body_address}\n")
        return "".join(out)


class _CallJumpType(enum.IntEnum):
    kUnconditional = 0
    kConditional = 1
    kPredicated = 2


@dataclass
class ParsedCallInstruction:
    dword_index: int = 0
    target_address: int = 0
    type: _CallJumpType = _CallJumpType.kUnconditional
    bool_constant_index: int = 0
    condition: bool = False

    def Disassemble(self) -> str:
        out = []
        if self.type == _CallJumpType.kUnconditional:
            out.append("      call ")
        elif self.type == _CallJumpType.kPredicated:
            out.append(" (p0) " if self.condition else "(!p0) ")
            out.append("call ")
        else:
            out.append("      ccall ")
            if not self.condition:
                out.append("!")
            out.append(f"b{self.bool_constant_index}, ")
        out.append(f"L{self.target_address}\n")
        return "".join(out)


@dataclass
class ParsedReturnInstruction:
    dword_index: int = 0

    def Disassemble(self) -> str:
        return "      ret\n"


@dataclass
class ParsedJumpInstruction:
    dword_index: int = 0
    target_address: int = 0
    type: _CallJumpType = _CallJumpType.kUnconditional
    bool_constant_index: int = 0
    condition: bool = False

    def Disassemble(self) -> str:
        out = []
        if self.type == _CallJumpType.kUnconditional:
            out.append("      jmp ")
        elif self.type == _CallJumpType.kPredicated:
            out.append(" (p0) " if self.condition else "(!p0) ")
            out.append("jmp ")
        else:
            out.append("      cjmp ")
            if not self.condition:
                out.append("!")
            out.append(f"b{self.bool_constant_index}, ")
        out.append(f"L{self.target_address}\n")
        return "".join(out)


@dataclass
class ParsedAllocInstruction:
    dword_index: int = 0
    type: AllocType = AllocType.kNone
    count: int = 0
    is_vertex_shader: bool = False

    def Disassemble(self) -> str:
        out = ["      alloc "]
        if self.type == AllocType.kVsPosition:
            out.append("position")
        elif self.type == AllocType.kVsInterpolators:  # or kPsColors
            out.append("interpolators" if self.is_vertex_shader else "colors")
        elif self.type == AllocType.kMemory:
            out.append(f"export = {self.count}")
        out.append("\n")
        return "".join(out)


def ParseControlFlowExec(cf: ControlFlowExecInstruction,
                         cf_index: int) -> ParsedExecInstruction:
    instr = ParsedExecInstruction()
    instr.dword_index = cf_index
    instr.opcode = cf.opcode()
    instr.opcode_name = ("exece" if cf.opcode() == ControlFlowOpcode.kExecEnd
                         else "exec")
    instr.instruction_address = cf.address()
    instr.instruction_count = cf.count()
    instr.type = _ExecType.kUnconditional
    instr.is_end = cf.opcode() == ControlFlowOpcode.kExecEnd
    instr.is_predicate_clean = cf.is_predicate_clean()
    instr.is_yield = cf.is_yield()
    instr.sequence = cf.sequence()
    return instr


def ParseControlFlowCondExec(cf: ControlFlowCondExecInstruction,
                             cf_index: int) -> ParsedExecInstruction:
    instr = ParsedExecInstruction()
    instr.dword_index = cf_index
    instr.opcode = cf.opcode()
    instr.opcode_name = "cexec"
    if cf.opcode() in (ControlFlowOpcode.kCondExecEnd,
                       ControlFlowOpcode.kCondExecPredCleanEnd):
        instr.opcode_name = "cexece"
        instr.is_end = True
    instr.instruction_address = cf.address()
    instr.instruction_count = cf.count()
    instr.type = _ExecType.kConditional
    instr.bool_constant_index = cf.bool_address()
    instr.condition = cf.condition()
    if cf.opcode() in (ControlFlowOpcode.kCondExec,
                       ControlFlowOpcode.kCondExecEnd):
        instr.is_predicate_clean = False
    instr.is_yield = cf.is_yield()
    instr.sequence = cf.sequence()
    return instr


def ParseControlFlowCondExecPred(cf: ControlFlowCondExecPredInstruction,
                                 cf_index: int) -> ParsedExecInstruction:
    instr = ParsedExecInstruction()
    instr.dword_index = cf_index
    instr.opcode = cf.opcode()
    instr.opcode_name = ("exece"
                         if cf.opcode() == ControlFlowOpcode.kCondExecPredEnd
                         else "exec")
    instr.instruction_address = cf.address()
    instr.instruction_count = cf.count()
    instr.type = _ExecType.kPredicated
    instr.condition = cf.condition()
    instr.is_end = cf.opcode() == ControlFlowOpcode.kCondExecPredEnd
    instr.is_predicate_clean = cf.is_predicate_clean()
    instr.is_yield = cf.is_yield()
    instr.sequence = cf.sequence()
    return instr


def ParseControlFlowLoopStart(cf: ControlFlowLoopStartInstruction,
                              cf_index: int) -> ParsedLoopStartInstruction:
    instr = ParsedLoopStartInstruction()
    instr.dword_index = cf_index
    instr.loop_constant_index = cf.loop_id()
    instr.is_repeat = cf.is_repeat()
    instr.loop_skip_address = cf.address()
    return instr


def ParseControlFlowLoopEnd(cf: ControlFlowLoopEndInstruction,
                            cf_index: int) -> ParsedLoopEndInstruction:
    instr = ParsedLoopEndInstruction()
    instr.dword_index = cf_index
    instr.is_predicated_break = cf.is_predicated_break()
    instr.predicate_condition = cf.condition()
    instr.loop_constant_index = cf.loop_id()
    instr.loop_body_address = cf.address()
    return instr


def ParseControlFlowCondCall(cf: ControlFlowCondCallInstruction,
                             cf_index: int) -> ParsedCallInstruction:
    instr = ParsedCallInstruction()
    instr.dword_index = cf_index
    instr.target_address = cf.address()
    if cf.is_unconditional():
        instr.type = _CallJumpType.kUnconditional
    elif cf.is_predicated():
        instr.type = _CallJumpType.kPredicated
        instr.condition = cf.condition()
    else:
        instr.type = _CallJumpType.kConditional
        instr.bool_constant_index = cf.bool_address()
        instr.condition = cf.condition()
    return instr


def ParseControlFlowReturn(cf: ControlFlowReturnInstruction,
                           cf_index: int) -> ParsedReturnInstruction:
    instr = ParsedReturnInstruction()
    instr.dword_index = cf_index
    return instr


def ParseControlFlowCondJmp(cf: ControlFlowCondJmpInstruction,
                            cf_index: int) -> ParsedJumpInstruction:
    instr = ParsedJumpInstruction()
    instr.dword_index = cf_index
    instr.target_address = cf.address()
    if cf.is_unconditional():
        instr.type = _CallJumpType.kUnconditional
    elif cf.is_predicated():
        instr.type = _CallJumpType.kPredicated
        instr.condition = cf.condition()
    else:
        instr.type = _CallJumpType.kConditional
        instr.bool_constant_index = cf.bool_address()
        instr.condition = cf.condition()
    return instr


def ParseControlFlowAlloc(cf: ControlFlowAllocInstruction, cf_index: int,
                          is_vertex_shader: bool) -> ParsedAllocInstruction:
    instr = ParsedAllocInstruction()
    instr.dword_index = cf_index
    instr.type = cf.alloc_type()
    instr.count = cf.size()
    instr.is_vertex_shader = is_vertex_shader
    return instr


# --- Parsed fetch instructions -----------------------------------------------

def _g(value: float) -> str:
    """C++ '{:g}' formatting equivalent."""
    return f"{value:g}"


def ParseFetchInstructionResult(dest: int, swizzle: int,
                                is_relative: bool) -> InstructionResult:
    result = InstructionResult()
    result.storage_target = InstructionStorageTarget.kRegister
    result.storage_index = dest
    result.is_clamped = False
    result.storage_addressing_mode = (
        InstructionStorageAddressingMode.kLoopRelative if is_relative
        else InstructionStorageAddressingMode.kAbsolute)
    result.original_write_mask = 0b1111
    for i in range(4):
        component_swizzle = GetFetchDestinationComponentSwizzle(swizzle, i)
        if component_swizzle == FetchDestinationSwizzle.kX:
            component_source = SwizzleSource.kX
        elif component_swizzle == FetchDestinationSwizzle.kY:
            component_source = SwizzleSource.kY
        elif component_swizzle == FetchDestinationSwizzle.kZ:
            component_source = SwizzleSource.kZ
        elif component_swizzle == FetchDestinationSwizzle.kW:
            component_source = SwizzleSource.kW
        elif component_swizzle == FetchDestinationSwizzle.k1:
            component_source = SwizzleSource.k1
        elif component_swizzle == FetchDestinationSwizzle.kKeep:
            result.original_write_mask &= ~(1 << i) & 0b1111
            component_source = SwizzleSource.k0
        else:
            # FetchDestinationSwizzle::k0 — or the invalid swizzle 6.
            # AMBIGUITY: the correct hardware handling of the invalid fetch
            # destination swizzle 6 is unknown (Xenia TODO(Triang3l) in
            # shader_translator.cc) — treated like constant 0 here, as Xenia
            # does.
            component_source = SwizzleSource.k0
        result.components[i] = component_source
    return result


@dataclass
class ParsedVertexFetchInstruction:
    opcode: int = int(FetchOpcode.kVertexFetch)
    opcode_name: str = ""
    is_mini_fetch: bool = False
    is_predicated: bool = False
    predicate_condition: bool = False
    result: InstructionResult = dc_field(default_factory=InstructionResult)
    operand_count: int = 0
    operands: list = dc_field(default_factory=list)

    # Attributes (ParsedVertexFetchInstruction::Attributes).
    data_format: int = int(VertexFormat.kUndefined)
    offset: int = 0
    stride: int = 0  # in dwords
    exp_adjust: int = 0
    prefetch_count: int = 0  # minus 1
    signed_rf_mode: SignedRepeatingFractionMode = \
        SignedRepeatingFractionMode.kZeroClampMinusOne
    is_index_rounded: bool = False
    is_signed: bool = False
    is_integer: bool = False

    def Disassemble(self) -> str:
        out = ["   ", _predication_prefix(self.is_predicated,
                                          self.predicate_condition)]
        out.append(self.opcode_name)
        out.append(" ")
        out.append(_disassemble_result_operand(self.result))
        if not self.is_mini_fetch:
            out.append(", ")
            out.append(_disassemble_source_operand(self.operands[0]))
            out.append(f", vf{95 - self.operands[1].storage_index}")
            if self.is_index_rounded:
                out.append(", RoundIndex=true")
        if self.exp_adjust:
            out.append(f", ExpAdjust={self.exp_adjust}")
        if self.offset:
            out.append(f", Offset={self.offset}")
        if self.data_format != int(VertexFormat.kUndefined):
            fmt_name = _VERTEX_FETCH_DATA_FORMAT_NAMES.get(
                self.data_format, f"FMT_UNKNOWN_{self.data_format}")
            out.append(f", DataFormat={fmt_name}")
        if not self.is_mini_fetch and self.stride:
            out.append(f", Stride={self.stride}")
        if self.is_signed:
            out.append(", Signed=true")
        if self.is_integer:
            out.append(", NumFormat=integer")
        if self.prefetch_count:
            out.append(f", PrefetchCount={self.prefetch_count + 1}")
        out.append("\n")
        return "".join(out)


def ParseVertexFetchInstruction(
        op: VertexFetchInstruction,
        previous_full_op: VertexFetchInstruction) -> tuple:
    """Returns (instr, is_full) — is_full means the next mini fetch should
    inherit this op (mirrors the bool return of Xenia's function)."""
    instr = ParsedVertexFetchInstruction()
    instr.opcode = int(FetchOpcode.kVertexFetch)
    instr.opcode_name = "vfetch_mini" if op.is_mini_fetch() else "vfetch_full"
    instr.is_mini_fetch = op.is_mini_fetch()
    instr.is_predicated = op.is_predicated()
    instr.predicate_condition = op.predicate_condition()

    instr.result = ParseFetchInstructionResult(
        op.dest(), op.dest_swizzle(), op.is_dest_relative())

    # Reuse previous vfetch_full if this is a mini.
    full_op = previous_full_op if op.is_mini_fetch() else op
    src_op = InstructionOperand()
    src_op.storage_source = InstructionStorageSource.kRegister
    src_op.storage_index = full_op.src()
    src_op.storage_addressing_mode = (
        InstructionStorageAddressingMode.kLoopRelative
        if full_op.is_src_relative()
        else InstructionStorageAddressingMode.kAbsolute)
    src_op.is_negated = False
    src_op.is_absolute_value = False
    src_op.component_count = 1
    src_op.components = [GetSwizzleFromComponentIndex(full_op.src_swizzle() & 0x3)]
    instr.operands.append(src_op)

    const_op = InstructionOperand()
    const_op.storage_source = InstructionStorageSource.kVertexFetchConstant
    const_op.storage_index = full_op.fetch_constant_index()
    instr.operands.append(const_op)
    instr.operand_count = 2

    instr.data_format = op.data_format()
    instr.offset = op.offset()
    instr.stride = full_op.stride()
    instr.exp_adjust = op.exp_adjust()
    instr.prefetch_count = op.prefetch_count()
    instr.is_index_rounded = full_op.is_index_rounded()
    instr.is_signed = op.is_signed()
    instr.is_integer = not op.is_normalized()
    instr.signed_rf_mode = op.signed_rf_mode()

    return instr, not op.is_mini_fetch()


_TEXTURE_FILTER_NAMES = ("point", "linear", "basemap", "keep")
_ANISO_FILTER_NAMES = ("disabled", "max1to1", "max2to1", "max4to1",
                       "max8to1", "max16to1", "keep")


@dataclass
class ParsedTextureFetchInstruction:
    opcode: int = int(FetchOpcode.kTextureFetch)
    opcode_name: str = ""
    dimension: FetchOpDimension = FetchOpDimension.k1D
    is_predicated: bool = False
    predicate_condition: bool = False
    result: InstructionResult = dc_field(default_factory=InstructionResult)
    operand_count: int = 0
    operands: list = dc_field(default_factory=list)

    # Attributes (ParsedTextureFetchInstruction::Attributes).
    fetch_valid_only: bool = True
    unnormalized_coordinates: bool = False
    mag_filter: TextureFilter = TextureFilter.kUseFetchConst
    min_filter: TextureFilter = TextureFilter.kUseFetchConst
    mip_filter: TextureFilter = TextureFilter.kUseFetchConst
    aniso_filter: int = int(AnisoFilter.kUseFetchConst)
    vol_mag_filter: TextureFilter = TextureFilter.kUseFetchConst
    vol_min_filter: TextureFilter = TextureFilter.kUseFetchConst
    use_computed_lod: bool = True
    use_register_lod: bool = False
    use_register_gradients: bool = False
    lod_bias: float = 0.0
    offset_x: float = 0.0
    offset_y: float = 0.0
    offset_z: float = 0.0

    def has_result(self) -> bool:
        return self.result.storage_target != InstructionStorageTarget.kNone

    def Disassemble(self) -> str:
        out = ["   ", _predication_prefix(self.is_predicated,
                                          self.predicate_condition)]
        out.append(self.opcode_name)
        out.append(" ")
        needs_comma = False
        if self.has_result():
            out.append(_disassemble_result_operand(self.result))
            needs_comma = True
        if needs_comma:
            out.append(", ")
        out.append(_disassemble_source_operand(self.operands[0]))
        if self.operand_count > 1:
            if needs_comma:
                out.append(", ")
            out.append(f"tf{self.operands[1].storage_index}")

        if not self.fetch_valid_only:
            out.append(", FetchValidOnly=false")
        if self.unnormalized_coordinates:
            out.append(", UnnormalizedTextureCoords=true")
        if self.mag_filter != TextureFilter.kUseFetchConst:
            out.append(f", MagFilter={_TEXTURE_FILTER_NAMES[int(self.mag_filter)]}")
        if self.min_filter != TextureFilter.kUseFetchConst:
            out.append(f", MinFilter={_TEXTURE_FILTER_NAMES[int(self.min_filter)]}")
        if self.mip_filter != TextureFilter.kUseFetchConst:
            out.append(f", MipFilter={_TEXTURE_FILTER_NAMES[int(self.mip_filter)]}")
        if self.aniso_filter != int(AnisoFilter.kUseFetchConst):
            out.append(f", AnisoFilter={_ANISO_FILTER_NAMES[self.aniso_filter]}")
        if self.vol_mag_filter != TextureFilter.kUseFetchConst:
            out.append(
                f", VolMagFilter={_TEXTURE_FILTER_NAMES[int(self.vol_mag_filter)]}")
        if self.vol_min_filter != TextureFilter.kUseFetchConst:
            out.append(
                f", VolMinFilter={_TEXTURE_FILTER_NAMES[int(self.vol_min_filter)]}")
        if not self.use_computed_lod:
            out.append(", UseComputedLOD=false")
        if self.use_register_lod:
            out.append(", UseRegisterLOD=true")
        if self.use_register_gradients:
            out.append(", UseRegisterGradients=true")
        if self.lod_bias != 0.0:
            out.append(f", LODBias={_g(self.lod_bias)}")
        component_count = GetFetchOpDimensionComponentCount(self.dimension)
        if self.offset_x != 0.0:
            out.append(f", OffsetX={_g(self.offset_x)}")
        if component_count > 1 and self.offset_y != 0.0:
            out.append(f", OffsetY={_g(self.offset_y)}")
        if component_count > 2 and self.offset_z != 0.0:
            out.append(f", OffsetZ={_g(self.offset_z)}")
        out.append("\n")
        return "".join(out)


# (name template(s), has_dest, has_const, has_attributes,
#  override_component_count) — from ParseTextureFetchInstruction.
_TFETCH_OPCODE_INFO = {
    int(FetchOpcode.kTextureFetch): (
        ("tfetch1D", "tfetch2D", "tfetch3D", "tfetchCube"), True, True, True, 0),
    int(FetchOpcode.kGetTextureBorderColorFrac): (
        ("getBCF1D", "getBCF2D", "getBCF3D", "getBCFCube"), True, True, True, 0),
    int(FetchOpcode.kGetTextureComputedLod): (
        ("getCompTexLOD1D", "getCompTexLOD2D", "getCompTexLOD3D",
         "getCompTexLODCube"), True, True, True, 0),
    int(FetchOpcode.kGetTextureGradients): (
        ("getGradients",) * 4, True, True, True, 2),
    int(FetchOpcode.kGetTextureWeights): (
        ("getWeights1D", "getWeights2D", "getWeights3D", "getWeightsCube"),
        True, True, True, 0),
    int(FetchOpcode.kSetTextureLod): (
        ("setTexLOD",) * 4, False, False, False, 1),
    int(FetchOpcode.kSetTextureGradientsHorz): (
        ("setGradientH",) * 4, False, False, False, 3),
    int(FetchOpcode.kSetTextureGradientsVert): (
        ("setGradientV",) * 4, False, False, False, 3),
}


def ParseTextureFetchInstruction(
        op: TextureFetchInstruction) -> ParsedTextureFetchInstruction:
    opcode = op.opcode()
    names, has_dest, has_const, has_attributes, override_component_count = \
        _TFETCH_OPCODE_INFO[opcode]

    instr = ParsedTextureFetchInstruction()
    instr.opcode = opcode
    instr.opcode_name = names[int(op.dimension())]
    instr.dimension = op.dimension()
    instr.is_predicated = op.is_predicated()
    instr.predicate_condition = op.predicate_condition()

    if has_dest:
        instr.result = ParseFetchInstructionResult(
            op.dest(), op.dest_swizzle(), op.is_dest_relative())
    else:
        instr.result.storage_target = InstructionStorageTarget.kNone

    src_op = InstructionOperand()
    src_op.storage_source = InstructionStorageSource.kRegister
    src_op.storage_index = op.src()
    src_op.storage_addressing_mode = (
        InstructionStorageAddressingMode.kLoopRelative if op.is_src_relative()
        else InstructionStorageAddressingMode.kAbsolute)
    src_op.is_negated = False
    src_op.is_absolute_value = False
    src_op.component_count = (
        override_component_count if override_component_count
        else GetFetchOpDimensionComponentCount(op.dimension()))
    swizzle = op.src_swizzle()
    src_op.components = []
    for _ in range(src_op.component_count):
        src_op.components.append(GetSwizzleFromComponentIndex(swizzle & 0x3))
        swizzle >>= 2
    instr.operands.append(src_op)
    instr.operand_count = 1

    if has_const:
        const_op = InstructionOperand()
        const_op.storage_source = InstructionStorageSource.kTextureFetchConstant
        const_op.storage_index = op.fetch_constant_index()
        instr.operands.append(const_op)
        instr.operand_count = 2

    if has_attributes:
        instr.fetch_valid_only = op.fetch_valid_only()
        instr.unnormalized_coordinates = op.unnormalized_coordinates()
        instr.mag_filter = op.mag_filter()
        instr.min_filter = op.min_filter()
        instr.mip_filter = op.mip_filter()
        instr.aniso_filter = op.aniso_filter()
        instr.vol_mag_filter = op.vol_mag_filter()
        instr.vol_min_filter = op.vol_min_filter()
        instr.use_computed_lod = op.use_computed_lod()
        instr.use_register_lod = op.use_register_lod()
        instr.use_register_gradients = op.use_register_gradients()
        instr.lod_bias = op.lod_bias()
        instr.offset_x = op.offset_x()
        instr.offset_y = op.offset_y()
        instr.offset_z = op.offset_z()
    return instr


# --- Parsed ALU instructions --------------------------------------------------

@dataclass
class ParsedAluInstruction:
    vector_opcode: AluVectorOpcode = AluVectorOpcode.kAdd
    scalar_opcode: AluScalarOpcode = AluScalarOpcode.kAdds
    vector_opcode_name: str = ""
    scalar_opcode_name: str = ""
    is_predicated: bool = False
    predicate_condition: bool = False
    vector_and_constant_result: InstructionResult = \
        dc_field(default_factory=InstructionResult)
    scalar_result: InstructionResult = dc_field(default_factory=InstructionResult)
    vector_operand_count: int = 0
    vector_operands: list = dc_field(default_factory=list)
    scalar_operand_count: int = 0
    scalar_operands: list = dc_field(default_factory=list)

    def IsVectorOpDefaultNop(self) -> bool:
        if (self.vector_opcode != AluVectorOpcode.kMax or
                self.vector_and_constant_result.original_write_mask or
                self.vector_and_constant_result.is_clamped or
                self.vector_operands[0].storage_source !=
                InstructionStorageSource.kRegister or
                self.vector_operands[0].storage_index != 0 or
                self.vector_operands[0].storage_addressing_mode !=
                InstructionStorageAddressingMode.kAbsolute or
                self.vector_operands[0].is_negated or
                self.vector_operands[0].is_absolute_value or
                not self.vector_operands[0].IsStandardSwizzle() or
                self.vector_operands[1].storage_source !=
                InstructionStorageSource.kRegister or
                self.vector_operands[1].storage_index != 0 or
                self.vector_operands[1].storage_addressing_mode !=
                InstructionStorageAddressingMode.kAbsolute or
                self.vector_operands[1].is_negated or
                self.vector_operands[1].is_absolute_value or
                not self.vector_operands[1].IsStandardSwizzle()):
            return False
        if (self.vector_and_constant_result.storage_target ==
                InstructionStorageTarget.kRegister):
            if (self.vector_and_constant_result.storage_index != 0 or
                    self.vector_and_constant_result.storage_addressing_mode !=
                    InstructionStorageAddressingMode.kAbsolute):
                return False
        else:
            # If both vector and scalar are nop, still need to write somewhere
            # that this is an export.
            if self.IsScalarOpDefaultNop():
                return False
        return True

    def IsScalarOpDefaultNop(self) -> bool:
        if (self.scalar_opcode != AluScalarOpcode.kRetainPrev or
                self.scalar_result.original_write_mask or
                self.scalar_result.is_clamped):
            return False
        if self.scalar_result.storage_target == InstructionStorageTarget.kRegister:
            if (self.scalar_result.storage_index != 0 or
                    self.scalar_result.storage_addressing_mode !=
                    InstructionStorageAddressingMode.kAbsolute):
                return False
        return True

    def Disassemble(self) -> str:
        out = []
        is_vector_op_default_nop = self.IsVectorOpDefaultNop()
        is_scalar_op_default_nop = self.IsScalarOpDefaultNop()
        if is_vector_op_default_nop and is_scalar_op_default_nop:
            out.append("   ")
            out.append(_predication_prefix(self.is_predicated,
                                           self.predicate_condition))
            out.append("nop\n")
            return "".join(out)
        if not is_vector_op_default_nop:
            out.append("   ")
            out.append(_predication_prefix(self.is_predicated,
                                           self.predicate_condition))
            out.append(self.vector_opcode_name)
            if self.vector_and_constant_result.is_clamped:
                out.append("_sat")
            out.append(" ")
            out.append(_disassemble_result_operand(
                self.vector_and_constant_result))
            for i in range(self.vector_operand_count):
                out.append(", ")
                out.append(_disassemble_source_operand(self.vector_operands[i]))
            out.append("\n")
        if not is_scalar_op_default_nop:
            out.append("   " if is_vector_op_default_nop else "              + ")
            out.append(_predication_prefix(self.is_predicated,
                                           self.predicate_condition))
            out.append(self.scalar_opcode_name)
            if self.scalar_result.is_clamped:
                out.append("_sat")
            out.append(" ")
            out.append(_disassemble_result_operand(self.scalar_result))
            for i in range(self.scalar_operand_count):
                out.append(", ")
                out.append(_disassemble_source_operand(self.scalar_operands[i]))
            out.append("\n")
        return "".join(out)


def ParseAluInstructionOperand(op: AluInstruction, i: int,
                               swizzle_component_count: int) -> InstructionOperand:
    out_op = InstructionOperand()
    out_op.is_negated = op.src_negate(i)
    reg = op.src_reg(i)
    if op.src_is_temp(i):
        out_op.storage_source = InstructionStorageSource.kRegister
        out_op.storage_index = AluInstruction.src_temp_reg(reg)
        out_op.is_absolute_value = AluInstruction.is_src_temp_value_absolute(reg)
        out_op.storage_addressing_mode = (
            InstructionStorageAddressingMode.kLoopRelative
            if AluInstruction.is_src_temp_relative(reg)
            else InstructionStorageAddressingMode.kAbsolute)
    else:
        out_op.storage_source = InstructionStorageSource.kConstantFloat
        out_op.storage_index = reg
        if op.src_const_is_addressed(i):
            if op.is_const_address_register_relative():
                out_op.storage_addressing_mode = \
                    InstructionStorageAddressingMode.kAddressRegisterRelative
            else:
                out_op.storage_addressing_mode = \
                    InstructionStorageAddressingMode.kLoopRelative
        else:
            out_op.storage_addressing_mode = \
                InstructionStorageAddressingMode.kAbsolute
        out_op.is_absolute_value = op.abs_constants()
    out_op.component_count = swizzle_component_count
    swizzle = op.src_swizzle(i)
    if swizzle_component_count == 1:
        # Scalar `a` (W).
        out_op.components = [GetSwizzledAluSourceComponent(swizzle, 3)]
    elif swizzle_component_count == 2:
        # Scalar left-hand `a` (W) and right-hand `b` (X).
        out_op.components = [GetSwizzledAluSourceComponent(swizzle, 3),
                             GetSwizzledAluSourceComponent(swizzle, 0)]
    else:  # 4
        out_op.components = [GetSwizzledAluSourceComponent(swizzle, j)
                             for j in range(swizzle_component_count)]
    return out_op


def ParseAluInstruction(op: AluInstruction,
                        shader_type: str) -> ParsedAluInstruction:
    instr = ParsedAluInstruction()
    instr.is_predicated = op.is_predicated()
    instr.predicate_condition = op.predicate_condition()

    is_export = op.is_export()

    storage_target = InstructionStorageTarget.kRegister
    storage_index_export = 0
    if is_export:
        storage_target = InstructionStorageTarget.kNone
        # Both vector and scalar operations export to vector_dest.
        export_register = op.vector_dest()
        if export_register == int(ExportRegister.kExportAddress):
            storage_target = InstructionStorageTarget.kExportAddress
        elif (int(ExportRegister.kExportData0) <= export_register <=
              int(ExportRegister.kExportData4)):
            storage_target = InstructionStorageTarget.kExportData
            storage_index_export = \
                export_register - int(ExportRegister.kExportData0)
        elif shader_type == "vertex":
            if (int(ExportRegister.kVSInterpolator0) <= export_register <=
                    int(ExportRegister.kVSInterpolator15)):
                storage_target = InstructionStorageTarget.kInterpolator
                storage_index_export = \
                    export_register - int(ExportRegister.kVSInterpolator0)
            elif export_register == int(ExportRegister.kVSPosition):
                storage_target = InstructionStorageTarget.kPosition
            elif export_register == \
                    int(ExportRegister.kVSPointSizeEdgeFlagKillVertex):
                storage_target = \
                    InstructionStorageTarget.kPointSizeEdgeFlagKillVertex
        elif shader_type == "pixel":
            if kPSColor0 <= export_register <= kPSColor3:
                storage_target = InstructionStorageTarget.kColor
                storage_index_export = export_register - kPSColor0
            elif export_register == int(ExportRegister.kPSDepth):
                storage_target = InstructionStorageTarget.kDepth
        # Unsupported export targets keep kNone (Xenia logs an error there;
        # the result operand then disassembles as empty).

    # Vector operation and constant 0/1 writes.
    vector_opcode = op.vector_opcode()
    instr.vector_opcode = vector_opcode
    vector_name, vector_operand_count = kAluVectorOpcodeInfos[int(vector_opcode)]
    instr.vector_opcode_name = vector_name

    vres = instr.vector_and_constant_result
    vres.storage_target = storage_target
    vres.storage_addressing_mode = InstructionStorageAddressingMode.kAbsolute
    if is_export:
        vres.storage_index = storage_index_export
    else:
        vres.storage_index = op.vector_dest()
        if op.is_vector_dest_relative():
            vres.storage_addressing_mode = \
                InstructionStorageAddressingMode.kLoopRelative
    vres.is_clamped = op.vector_clamp()
    constant_0_mask = op.GetConstant0WriteMask()
    constant_1_mask = op.GetConstant1WriteMask()
    vres.original_write_mask = (op.GetVectorOpResultWriteMask() |
                                constant_0_mask | constant_1_mask)
    for i in range(4):
        component = GetSwizzleFromComponentIndex(i)
        if constant_0_mask & (1 << i):
            component = SwizzleSource.k0
        elif constant_1_mask & (1 << i):
            component = SwizzleSource.k1
        vres.components[i] = component

    instr.vector_operand_count = vector_operand_count
    instr.vector_operands = [ParseAluInstructionOperand(op, i + 1, 4)
                             for i in range(vector_operand_count)]
    # IsVectorOpDefaultNop inspects the first two operand slots regardless of
    # operand count (in C++ those are default-constructed) — provide defaults.
    while len(instr.vector_operands) < 2:
        instr.vector_operands.append(InstructionOperand())

    # Scalar operation.
    scalar_opcode = op.scalar_opcode()
    instr.scalar_opcode = scalar_opcode
    scalar_name, scalar_operand_count, single_operand_is_two_component = \
        kAluScalarOpcodeInfos[int(scalar_opcode)]
    instr.scalar_opcode_name = scalar_name

    sres = instr.scalar_result
    sres.storage_target = storage_target
    sres.storage_addressing_mode = InstructionStorageAddressingMode.kAbsolute
    if is_export:
        sres.storage_index = storage_index_export
    else:
        sres.storage_index = op.scalar_dest()
        if op.is_scalar_dest_relative():
            sres.storage_addressing_mode = \
                InstructionStorageAddressingMode.kLoopRelative
    sres.is_clamped = op.scalar_clamp()
    sres.original_write_mask = op.GetScalarOpResultWriteMask()
    sres.components = [GetSwizzleFromComponentIndex(i) for i in range(4)]

    instr.scalar_operand_count = scalar_operand_count
    if scalar_operand_count:
        if scalar_operand_count == 1:
            instr.scalar_operands = [ParseAluInstructionOperand(
                op, 3, 2 if single_operand_is_two_component else 1)]
        else:
            # mulsc/addsc/subsc: constant and temporary register.
            src3_negate = op.src_negate(3)
            src3_swizzle = op.src_swizzle(3)

            # Left-hand constant operand (`a` - W swizzle).
            const_op = InstructionOperand()
            const_op.is_negated = src3_negate
            const_op.is_absolute_value = op.abs_constants()
            const_op.storage_source = InstructionStorageSource.kConstantFloat
            const_op.storage_index = op.src_reg(3)
            if op.src_const_is_addressed(3):
                if op.is_const_address_register_relative():
                    const_op.storage_addressing_mode = \
                        InstructionStorageAddressingMode.kAddressRegisterRelative
                else:
                    const_op.storage_addressing_mode = \
                        InstructionStorageAddressingMode.kLoopRelative
            else:
                const_op.storage_addressing_mode = \
                    InstructionStorageAddressingMode.kAbsolute
            const_op.component_count = 1
            const_op.components = [GetSwizzledAluSourceComponent(src3_swizzle, 3)]

            # Right-hand temporary register operand (`b` - X swizzle).
            temp_op = InstructionOperand()
            temp_op.is_negated = src3_negate
            temp_op.is_absolute_value = op.abs_constants()
            temp_op.storage_source = InstructionStorageSource.kRegister
            temp_op.storage_index = op.scalar_const_reg_op_src_temp_reg()
            temp_op.storage_addressing_mode = \
                InstructionStorageAddressingMode.kAbsolute
            temp_op.component_count = 1
            temp_op.components = [GetSwizzledAluSourceComponent(src3_swizzle, 0)]

            instr.scalar_operands = [const_op, temp_op]
    return instr


# =============================================================================
# Program-level decoding (mirrors Shader::AnalyzeUcode in
# shader_translator.cc).
# =============================================================================

@dataclass
class DecodedInstruction:
    """One decoded instruction of the program.

    kind:   'cf' | 'alu' | 'vfetch' | 'tfetch' | 'unknown'
    offset: control flow index (for kind 'cf') or instruction slot address
            (for ALU/fetch instructions)
    opcode: opcode name string (e.g. 'exec', 'max', 'vfetch_full')
    text:   disassembly line(s) for this instruction, without the leading
            '/* addr */' marker, trailing newline stripped
    detail: the parsed instruction object (Parsed*Instruction), or None
    raw:    tuple of the raw dwords the instruction was decoded from
    """
    kind: str
    offset: int
    opcode: str
    text: str
    detail: object = None
    raw: tuple = ()
    # Extra info for slot instructions:
    serialize: bool = False
    exec_cf_index: int = -1


_CF_OPCODE_NAMES = {
    ControlFlowOpcode.kNop: "cnop",
    ControlFlowOpcode.kExec: "exec",
    ControlFlowOpcode.kExecEnd: "exece",
    ControlFlowOpcode.kCondExec: "cexec",
    ControlFlowOpcode.kCondExecEnd: "cexece",
    ControlFlowOpcode.kCondExecPred: "exec",
    ControlFlowOpcode.kCondExecPredEnd: "exece",
    ControlFlowOpcode.kLoopStart: "loop",
    ControlFlowOpcode.kLoopEnd: "endloop",
    ControlFlowOpcode.kCondCall: "call",
    ControlFlowOpcode.kReturn: "ret",
    ControlFlowOpcode.kCondJmp: "jmp",
    ControlFlowOpcode.kAlloc: "alloc",
    ControlFlowOpcode.kCondExecPredClean: "cexec",
    ControlFlowOpcode.kCondExecPredCleanEnd: "cexece",
    ControlFlowOpcode.kMarkVsFetchDone: "vfetch_done",
}


def _scan_program(dwords) -> tuple:
    """Returns (cf_pair_index_bound, label_addresses) the way
    Shader::AnalyzeUcode determines them: the upper bound of the control flow
    program is guessed by scanning for the first kExec-ish instruction and
    using its address (this is what freedreno does); jump/call/loop target
    addresses become labels."""
    cf_pair_index_bound = len(dwords) // 3
    label_addresses = set()
    i = 0
    while i < cf_pair_index_bound:
        cf_ab = UnpackControlFlowInstructions(dwords[i * 3:i * 3 + 3])
        for cf in cf_ab:
            opcode = cf.opcode()
            if IsControlFlowOpcodeExec(opcode):
                cf_pair_index_bound = min(cf_pair_index_bound,
                                          cf.exec.address())
            if opcode == ControlFlowOpcode.kCondCall:
                label_addresses.add(cf.cond_call.address())
            elif opcode == ControlFlowOpcode.kCondJmp:
                label_addresses.add(cf.cond_jmp.address())
            elif opcode == ControlFlowOpcode.kLoopStart:
                label_addresses.add(cf.loop_start.address())
            elif opcode == ControlFlowOpcode.kLoopEnd:
                label_addresses.add(cf.loop_end.address())
        i += 1
    return cf_pair_index_bound, label_addresses


def _decode_exec_slots(dwords, exec_instr: ParsedExecInstruction,
                       shader_type: str, previous_vfetch_full: list,
                       records: list, out: list, exact: bool = False,
                       covered_slots: set | None = None) -> None:
    """Decodes the ALU/fetch slots referenced by an exec-type CF instruction
    (mirrors Shader::GatherExecInformation)."""
    sequence = exec_instr.sequence
    for instr_offset in range(
            exec_instr.instruction_address,
            exec_instr.instruction_address + exec_instr.instruction_count):
        if covered_slots is not None:
            covered_slots.add(instr_offset)
        out.append(f"/* {instr_offset:4d}   */ ")
        serialize = bool(sequence & 0b10)
        if serialize:
            out.append("         serialize\n             ")
        base = instr_offset * 3
        if base + 3 > len(dwords):
            line = (f"{UNKNOWN_MARKER} instruction slot {instr_offset} is "
                    f"out of range of the ucode\n")
            out.append(line)
            records.append(DecodedInstruction(
                kind="unknown", offset=instr_offset, opcode="?",
                text=line.rstrip("\n"), raw=(), serialize=serialize,
                exec_cf_index=exec_instr.dword_index))
            sequence >>= 2
            continue
        raw = (dwords[base], dwords[base + 1], dwords[base + 2])
        if sequence & 0b01:
            # Fetch instruction.
            fetch_opcode = _bits(_u32(raw[0]), 0, 5)
            if fetch_opcode == int(FetchOpcode.kVertexFetch):
                op = VertexFetchInstruction(*raw)
                instr, is_full = ParseVertexFetchInstruction(
                    op, previous_vfetch_full[0])
                if is_full:
                    previous_vfetch_full[0] = op
                text = instr.Disassemble()
                out.append(text)
                records.append(DecodedInstruction(
                    kind="vfetch", offset=instr_offset,
                    opcode=instr.opcode_name, text=text.rstrip("\n"),
                    detail=instr, raw=raw, serialize=serialize,
                    exec_cf_index=exec_instr.dword_index))
            elif fetch_opcode in _TFETCH_OPCODE_INFO:
                op = TextureFetchInstruction(*raw)
                instr = ParseTextureFetchInstruction(op)
                text = instr.Disassemble()
                out.append(text)
                records.append(DecodedInstruction(
                    kind="tfetch", offset=instr_offset,
                    opcode=instr.opcode_name, text=text.rstrip("\n"),
                    detail=instr, raw=raw, serialize=serialize,
                    exec_cf_index=exec_instr.dword_index))
            else:
                line = (f"{UNKNOWN_MARKER} unknown fetch opcode "
                        f"{fetch_opcode}: "
                        f"0x{_u32(raw[0]):08X} 0x{_u32(raw[1]):08X} "
                        f"0x{_u32(raw[2]):08X}\n")
                out.append(line)
                records.append(DecodedInstruction(
                    kind="unknown", offset=instr_offset,
                    opcode=f"fetch_opcode_{fetch_opcode}",
                    text=line.rstrip("\n"), raw=raw, serialize=serialize,
                    exec_cf_index=exec_instr.dword_index))
        else:
            # ALU instruction. All 64 scalar / 32 vector opcode encodings have
            # table entries (unknown ones decode as opcode_NN), so this never
            # fails.
            op = AluInstruction(*raw)
            instr = ParseAluInstruction(op, shader_type)
            text = instr.Disassemble()
            out.append(text)
            opcode_name = (instr.scalar_opcode_name
                           if instr.IsVectorOpDefaultNop()
                           and not instr.IsScalarOpDefaultNop()
                           else instr.vector_opcode_name)
            records.append(DecodedInstruction(
                kind="alu", offset=instr_offset, opcode=opcode_name,
                text=text.rstrip("\n"), detail=instr, raw=raw,
                serialize=serialize, exec_cf_index=exec_instr.dword_index))
        if exact and base + 3 <= len(dwords):
            out.append(f"             ;; raw {raw[0]:#010x} {raw[1]:#010x} "
                       f"{raw[2]:#010x}\n")
        sequence >>= 2


def _decode_program(dwords, shader_type: str, exact: bool = False) -> tuple:
    """Walks a whole shader program. Returns (records, full_text).

    exact=True appends lossless annotations to the text: a
    `;; raw <dwords>` line after every CF entry and slot instruction, and a
    `;; pad <slot> <dwords>` footer line for every dword triplet neither in
    the CF program nor referenced by an exec — together these make the text
    reconstructible to the exact original bytes (see docs/xasm.md).
    """
    if shader_type not in ("vertex", "pixel"):
        raise ValueError(
            f"shader_type must be 'vertex' or 'pixel', got {shader_type!r}")
    dwords = [_u32(d) for d in dwords]
    is_vertex_shader = shader_type == "vertex"

    cf_pair_index_bound, label_addresses = _scan_program(dwords)
    covered_slots: set[int] = set()

    records: list[DecodedInstruction] = []
    out: list[str] = []
    # Mutable single-element holder for the previous vfetch_full (zeroed
    # initially, as in ShaderTranslator::Reset / Shader::AnalyzeUcode).
    previous_vfetch_full = [VertexFetchInstruction(0, 0, 0)]

    for i in range(cf_pair_index_bound):
        cf_ab = UnpackControlFlowInstructions(dwords[i * 3:i * 3 + 3])
        for j in range(2):
            cf_index = i * 2 + j
            if cf_index in label_addresses:
                out.append(f"                label L{cf_index}\n")
            out.append(f"/* {i:4d}.{j} */ ")

            cf = cf_ab[j]
            opcode = cf.opcode()
            raw = (cf.dword_0, cf.dword_1)
            detail = None
            text = ""
            if opcode == ControlFlowOpcode.kNop:
                text = "      cnop\n"
                out.append(text)
            elif opcode in (ControlFlowOpcode.kExec,
                            ControlFlowOpcode.kExecEnd):
                detail = ParseControlFlowExec(cf.exec, cf_index)
            elif opcode in (ControlFlowOpcode.kCondExec,
                            ControlFlowOpcode.kCondExecEnd,
                            ControlFlowOpcode.kCondExecPredClean,
                            ControlFlowOpcode.kCondExecPredCleanEnd):
                detail = ParseControlFlowCondExec(cf.cond_exec, cf_index)
            elif opcode in (ControlFlowOpcode.kCondExecPred,
                            ControlFlowOpcode.kCondExecPredEnd):
                detail = ParseControlFlowCondExecPred(cf.cond_exec_pred,
                                                      cf_index)
            elif opcode == ControlFlowOpcode.kLoopStart:
                detail = ParseControlFlowLoopStart(cf.loop_start, cf_index)
                text = detail.Disassemble()
                out.append(text)
            elif opcode == ControlFlowOpcode.kLoopEnd:
                detail = ParseControlFlowLoopEnd(cf.loop_end, cf_index)
                text = detail.Disassemble()
                out.append(text)
            elif opcode == ControlFlowOpcode.kCondCall:
                detail = ParseControlFlowCondCall(cf.cond_call, cf_index)
                text = detail.Disassemble()
                out.append(text)
            elif opcode == ControlFlowOpcode.kReturn:
                detail = ParseControlFlowReturn(cf.ret, cf_index)
                text = detail.Disassemble()
                out.append(text)
            elif opcode == ControlFlowOpcode.kCondJmp:
                detail = ParseControlFlowCondJmp(cf.cond_jmp, cf_index)
                text = detail.Disassemble()
                out.append(text)
            elif opcode == ControlFlowOpcode.kAlloc:
                detail = ParseControlFlowAlloc(cf.alloc, cf_index,
                                               is_vertex_shader)
                text = detail.Disassemble()
                out.append(text)
            elif opcode == ControlFlowOpcode.kMarkVsFetchDone:
                # Xenia's disassembler emits nothing for kMarkVsFetchDone
                # (not even a line break) — replicated here for diffability.
                text = ""

            if isinstance(detail, ParsedExecInstruction):
                text = detail.Disassemble()
                out.append(text)
                if exact:
                    out.append(f"             ;; raw {raw[0]:#010x} "
                               f"{raw[1]:#06x}\n")
                records.append(DecodedInstruction(
                    kind="cf", offset=cf_index, opcode=detail.opcode_name,
                    text=text.rstrip("\n"), detail=detail, raw=raw))
                _decode_exec_slots(dwords, detail, shader_type,
                                   previous_vfetch_full, records, out,
                                   exact=exact, covered_slots=covered_slots)
            else:
                if exact:
                    if opcode == ControlFlowOpcode.kMarkVsFetchDone:
                        # prints no text of its own; raw keeps its bits
                        out.append("\n")
                    out.append(f"             ;; raw {raw[0]:#010x} "
                               f"{raw[1]:#06x}\n")
                records.append(DecodedInstruction(
                    kind="cf", offset=cf_index,
                    opcode=_CF_OPCODE_NAMES.get(opcode, f"cf_{int(opcode)}"),
                    text=text.rstrip("\n"), detail=detail, raw=raw))
    if exact:
        for slot in range(cf_pair_index_bound, len(dwords) // 3):
            if slot not in covered_slots:
                base = slot * 3
                out.append(f"             ;; pad {slot} "
                           f"{dwords[base]:#010x} {dwords[base+1]:#010x} "
                           f"{dwords[base+2]:#010x}\n")
        leftover = len(dwords) % 3
        if leftover:
            tail = " ".join(f"{d:#010x}" for d in dwords[-leftover:])
            out.append(f"             ;; tail {tail}\n")
    return records, "".join(out)


# =============================================================================
# Public API
# =============================================================================

def disassemble(dwords, shader_type: str, exact: bool = False) -> str:
    """Disassembles a whole Xenos shader microcode program.

    dwords: host-order uint32 values in ucode order.
    shader_type: "vertex" or "pixel".
    exact: append lossless `;; raw`/`;; pad`/`;; tail` annotations making the
    text byte-reconstructible (default output is unchanged and stays
    diffable against Xenia's ucode disassembler).
    Returns the full multi-line disassembly text (Xenia ucode disasm style).
    """
    _, text = _decode_program(list(dwords), shader_type, exact=exact)
    return text


def reconstruct_from_exact(text: str) -> bytes:
    """Rebuilds the exact big-endian ucode bytes from exact-mode disassembly
    (the authoritative-bytes path; mnemonic-based assembly lives in xasm).

    CF `;; raw` lines carry (dword0, 16-bit dword1) per 48-bit CF entry; two
    consecutive entries re-pack into one 3-dword pair. `;; pad`/`;; tail`
    lines carry uncovered slots/trailing dwords at their slot positions.
    """
    import re
    import struct
    cf_entries: list[tuple[int, int]] = []
    slot_words: dict[int, tuple] = {}
    tail: list[int] = []
    for line in text.splitlines():
        m = re.search(r";; raw ((?:0x[0-9a-f]+ ?)+)$", line)
        if m:
            words = [int(w, 16) for w in m.group(1).split()]
            if len(words) == 2:
                cf_entries.append((words[0], words[1]))
            continue
        m = re.search(r";; pad (\d+) ((?:0x[0-9a-f]+ ?)+)$", line)
        if m:
            slot_words[int(m.group(1))] = tuple(
                int(w, 16) for w in m.group(2).split())
            continue
        m = re.search(r";; tail ((?:0x[0-9a-f]+ ?)+)$", line)
        if m:
            tail = [int(w, 16) for w in m.group(1).split()]
    # Slot raws appear in exec order; map them back via a second pass over
    # the text pairing "/* N   */" markers with their ;; raw line.
    slot_re = re.compile(r"/\*\s*(\d+)\s+\*/")
    cur_slot = None
    for line in text.splitlines():
        m = slot_re.search(line)
        if m:
            cur_slot = int(m.group(1))
        m = re.search(r";; raw ((?:0x[0-9a-f]+ ?)+)$", line)
        if m:
            words = tuple(int(w, 16) for w in m.group(1).split())
            if len(words) == 3 and cur_slot is not None:
                slot_words[cur_slot] = words
    if len(cf_entries) % 2:
        cf_entries.append((0, 0))
    n_cf_pairs = len(cf_entries) // 2
    total_slots = n_cf_pairs + (
        (max(slot_words) - n_cf_pairs + 1) if slot_words else 0)
    if slot_words:
        total_slots = max(total_slots, max(slot_words) + 1)
    dwords: list[int] = []
    for i in range(n_cf_pairs):
        (a0, a1), (b0, b1) = cf_entries[2 * i], cf_entries[2 * i + 1]
        dwords.extend(_pack_cf_pair(a0, a1, b0, b1))
    for slot in range(n_cf_pairs, total_slots):
        words = slot_words.get(slot, (0, 0, 0))
        dwords.extend(words)
    dwords.extend(tail)
    return struct.pack(f">{len(dwords)}I", *dwords)


def parse_program(dwords, shader_type: str) -> list:
    """Decodes a whole Xenos shader microcode program into a structured list
    of DecodedInstruction records (see DecodedInstruction docstring)."""
    records, _ = _decode_program(list(dwords), shader_type)
    return records


# =============================================================================
# Self-test
# =============================================================================

def _pack_cf_pair(a_d0: int, a_d1: int, b_d0: int, b_d1: int) -> list:
    """Packs two 48-bit CF instructions (32-bit word 0, 16-bit word 1 each)
    into three dwords, inverse of UnpackControlFlowInstructions."""
    assert a_d1 <= 0xFFFF and b_d1 <= 0xFFFF
    d0 = _u32(a_d0)
    d1 = _u32(a_d1 | ((b_d0 & 0xFFFF) << 16))
    d2 = _u32((b_d0 >> 16) | (b_d1 << 16))
    return [d0, d1, d2]


def _self_test() -> None:
    # --- CF pack/unpack round trip ------------------------------------------
    # exec: address=1, count=2, sequence=0b0001 (slot0 fetch, slot1 ALU),
    # opcode kExec, address_mode kAbsolute.
    exec_d0 = 1 | (2 << 12) | (0b0001 << 16)
    exec_d1 = ((1 << 9) |  # is_predicate_clean
               (int(AddressingMode.kAbsolute) << 11) |
               (int(ControlFlowOpcode.kExec) << 12))
    # alloc position: size=0, alloc_type=kVsPosition, opcode kAlloc.
    alloc_d0 = 0
    alloc_d1 = ((int(AllocType.kVsPosition) << 9) |
                (int(ControlFlowOpcode.kAlloc) << 12))
    pair0 = _pack_cf_pair(alloc_d0, alloc_d1, exec_d0, exec_d1)
    cf_a, cf_b = UnpackControlFlowInstructions(pair0)
    assert cf_a.opcode() == ControlFlowOpcode.kAlloc
    assert cf_a.alloc.alloc_type() == AllocType.kVsPosition
    assert cf_b.opcode() == ControlFlowOpcode.kExec
    assert cf_b.exec.address() == 1
    assert cf_b.exec.count() == 2
    assert cf_b.exec.sequence() == 0b0001
    assert cf_b.exec.is_predicate_clean()
    assert not cf_b.exec.is_yield()

    # Make the exec an exece (end of program) for the test program below.
    exece_d1 = exec_d1 ^ ((int(ControlFlowOpcode.kExec) ^
                           int(ControlFlowOpcode.kExecEnd)) << 12)
    pair0 = _pack_cf_pair(alloc_d0, alloc_d1, exec_d0, exece_d1)

    # --- Vertex fetch instruction at slot 1 ----------------------------------
    # vfetch_full r1.xyz_, r0.x, vf95 (fetch constant 0),
    # DataFormat=FMT_32_32_32_FLOAT, Stride=3, Signed=true
    vf_d0 = (int(FetchOpcode.kVertexFetch) |  # opcode_value:5 = 0
             (0 << 5) |    # src_reg = r0
             (0 << 11) |   # src_reg_am
             (1 << 12) |   # dst_reg = r1
             (0 << 18) |   # dst_reg_am
             (1 << 19) |   # must_be_one
             (0 << 20) |   # const_index
             (0 << 25) |   # const_index_sel
             (0 << 27) |   # prefetch_count
             (0 << 30))    # src_swiz = x
    # dst_swiz: x<-X(0), y<-Y(1), z<-Z(2), w<-Keep(7)
    dst_swiz = (0 << 0) | (1 << 3) | (2 << 6) | (7 << 9)
    vf_d1 = (dst_swiz |
             (1 << 12) |   # fomat_comp_all (signed)
             (0 << 13) |   # num_format_all (normalized)
             (0 << 14) |   # signed_rf_mode_all
             (0 << 15) |   # is_index_rounded
             (int(VertexFormat.k_32_32_32_FLOAT) << 16) |
             (0 << 24) |   # exp_adjust
             (0 << 30) |   # is_mini_fetch
             (0 << 31))    # is_predicated
    vf_d2 = 3 | (0 << 8)   # stride=3, offset=0
    vf = VertexFetchInstruction(vf_d0, vf_d1, vf_d2)
    assert vf.opcode() == int(FetchOpcode.kVertexFetch)
    assert vf.dest() == 1
    assert vf.src() == 0
    assert vf.must_be_one() == 1
    assert vf.fetch_constant_index() == 0
    assert vf.data_format() == int(VertexFormat.k_32_32_32_FLOAT)
    assert vf.stride() == 3
    assert vf.is_signed()
    assert vf.is_normalized()
    assert not vf.is_mini_fetch()
    vf_parsed, vf_is_full = ParseVertexFetchInstruction(
        vf, VertexFetchInstruction(0, 0, 0))
    assert vf_is_full
    assert vf_parsed.result.original_write_mask == 0b0111
    vf_text = vf_parsed.Disassemble()
    assert "vfetch_full r1.xyz_, r0.x, vf95" in vf_text, vf_text
    assert "DataFormat=FMT_32_32_32_FLOAT" in vf_text, vf_text
    assert "Stride=3" in vf_text, vf_text
    assert "Signed=true" in vf_text, vf_text

    # --- ALU instruction at slot 2: max r1, r0, r1 ---------------------------
    # (scalar co-issue: retain_prev with empty write mask on r0 = default nop)
    alu_d0 = (1 |           # vector_dest = r1
              (0 << 6) |    # vector_dest_rel
              (0 << 7) |    # abs_constants
              (0 << 8) |    # scalar_dest
              (0 << 14) |   # scalar_dest_rel
              (0 << 15) |   # export_data
              (0b1111 << 16) |  # vector_write_mask
              (0b0000 << 20) |  # scalar_write_mask
              (0 << 24) |   # vector_clamp
              (0 << 25) |   # scalar_clamp
              (int(AluScalarOpcode.kRetainPrev) << 26))
    alu_d1 = 0  # standard swizzles, no negate, not predicated
    alu_d2 = ((0 << 0) |    # src3_reg
              (1 << 8) |    # src2_reg = r1
              (0 << 16) |   # src1_reg = r0
              (int(AluVectorOpcode.kMax) << 24) |
              (0 << 29) |   # src3_sel (constant)
              (1 << 30) |   # src2_sel (temp)
              (1 << 31))    # src1_sel (temp)
    alu = AluInstruction(alu_d0, alu_d1, alu_d2)
    assert alu.vector_opcode() == AluVectorOpcode.kMax
    assert alu.scalar_opcode() == AluScalarOpcode.kRetainPrev
    assert alu.vector_dest() == 1
    assert alu.vector_write_mask() == 0b1111
    assert alu.scalar_write_mask() == 0b0000
    assert not alu.is_export()
    assert alu.src_is_temp(1) and alu.src_is_temp(2)
    assert alu.src_reg(1) == 0 and alu.src_reg(2) == 1
    alu_parsed = ParseAluInstruction(alu, "vertex")
    assert alu_parsed.IsScalarOpDefaultNop()
    assert not alu_parsed.IsVectorOpDefaultNop()
    alu_text = alu_parsed.Disassemble()
    assert "max r1, r0, r1" in alu_text, alu_text

    # --- mulsc temp register index construction ------------------------------
    # scalar_const_reg_op_src_temp_reg = (opc&1) | (src3_sel<<1) |
    #                                    (src3_swiz & 0x3C)
    mulsc_d0 = ((0b0001 << 20) |  # scalar_write_mask = x
                (int(AluScalarOpcode.kMulsc1) << 26))
    mulsc_d1 = 0b00001000  # src3_swiz = 0x08 -> bits[2:5] contribute 8
    mulsc_d2 = ((5 << 0) |   # src3_reg = c5
                (int(AluVectorOpcode.kMax) << 24) |
                (1 << 29))   # src3_sel = 1
    mulsc = AluInstruction(mulsc_d0, mulsc_d1, mulsc_d2)
    # (43 & 1) | (1 << 1) | (0x08 & 0x3C) = 1 | 2 | 8 = 11
    assert mulsc.scalar_const_reg_op_src_temp_reg() == 11
    mulsc_parsed = ParseAluInstruction(mulsc, "pixel")
    assert mulsc_parsed.scalar_opcode_name == "mulsc"
    assert mulsc_parsed.scalar_operand_count == 2
    assert (mulsc_parsed.scalar_operands[0].storage_source ==
            InstructionStorageSource.kConstantFloat)
    assert mulsc_parsed.scalar_operands[0].storage_index == 5
    assert mulsc_parsed.scalar_operands[1].storage_index == 11

    # --- Whole-program disassembly -------------------------------------------
    program = pair0 + [vf_d0, vf_d1, vf_d2] + [alu_d0, alu_d1, alu_d2]
    text = disassemble(program, "vertex")
    assert "alloc position" in text, text
    assert "exece" in text, text
    assert "vfetch_full" in text, text
    assert "max r1, r0, r1" in text, text
    records = parse_program(program, "vertex")
    kinds = [r.kind for r in records]
    assert kinds == ["cf", "cf", "vfetch", "alu"], kinds
    assert records[0].opcode == "alloc"
    assert records[1].opcode == "exece"
    assert records[1].detail.instruction_address == 1
    assert records[1].detail.instruction_count == 2
    assert records[2].offset == 1 and records[3].offset == 2

    # --- Zero padding / empty program: all cnop, no crash --------------------
    text = disassemble([0] * 9, "pixel")
    assert text.count("cnop") == 6, text

    # --- Unknown fetch opcode: warning marker, no crash ----------------------
    # exec with one fetch slot whose opcode is the undefined value 9.
    bad_exec_d0 = 1 | (1 << 12) | (0b01 << 16)
    bad_exec_d1 = ((1 << 9) | (int(AddressingMode.kAbsolute) << 11) |
                   (int(ControlFlowOpcode.kExecEnd) << 12))
    bad_program = _pack_cf_pair(bad_exec_d0, bad_exec_d1, 0, 0) + [9, 0, 0]
    text = disassemble(bad_program, "pixel")
    assert UNKNOWN_MARKER in text, text
    recs = parse_program(bad_program, "pixel")
    assert any(r.kind == "unknown" for r in recs)

    # --- Truncated program (slot out of range): no crash ---------------------
    trunc_program = _pack_cf_pair(bad_exec_d0, bad_exec_d1, 0, 0)
    text = disassemble(trunc_program, "pixel")
    assert "out of range" in text, text

    # --- Scalar adds disassembly (wx operand swizzle) -------------------------
    adds = AluInstruction((0b0001 << 20) | (int(AluScalarOpcode.kAdds) << 26),
                          0, (1 << 29))  # src3 = r0 (temp), swizzle 0
    adds_parsed = ParseAluInstruction(adds, "pixel")
    adds_text = adds_parsed.Disassemble()
    assert "adds r0.x___, r0.wx" in adds_text, adds_text

    print("xenos_ucode self-test: all checks passed.")


if __name__ == "__main__":
    _self_test()
