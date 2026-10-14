"""Xenos shader microcode ENCODER layer.

Exact inverse of xdis/xenos_ucode.py (the verified decoder): named fields in,
raw dwords out, such that constructing the corresponding decoder class from
the returned dwords reproduces every input field.

Builder keyword arguments mirror the decoder property names EXACTLY, so a
decoded instruction's fields can be piped back in as **dict:

  Control flow (each returns a 48-bit pair ``(dword_0, dword_1)`` where
  ``dword_1`` fits in 16 bits; ``opcode`` is passed explicitly because most
  builders serve several ControlFlowOpcode values):
    build_cf_exec            kExec, kExecEnd
    build_cf_cond_exec       kCondExec, kCondExecEnd, kCondExecPredClean,
                             kCondExecPredCleanEnd
    build_cf_cond_exec_pred  kCondExecPred, kCondExecPredEnd
    build_cf_loop_start      kLoopStart
    build_cf_loop_end        kLoopEnd
    build_cf_cond_call       kCondCall
    build_cf_return          kReturn
    build_cf_cond_jmp        kCondJmp
    build_cf_alloc           kAlloc
    pack_cf_pair((a0, a1), (b0, b1)) -> [d0, d1, d2]   (3-dword packing)

  Fetch / ALU (each returns ``(dword_0, dword_1, dword_2)``):
    build_vfetch_full   full vertex fetch (is_mini_fetch forced 0)
    build_vfetch_mini   mini vertex fetch (is_mini_fetch forced 1; only the
                        fields a mini fetch actually encodes — everything else
                        is inherited from the preceding full fetch and encoded
                        as 0)
    build_tfetch        texture fetch family (tfetch/getXXX/setXXX opcodes)
    build_alu           ALU instruction (vector + scalar co-issue)

ALU sources are indexed accessors on the decoder (src_reg(i), src_is_temp(i),
src_swizzle(i), src_negate(i)); the builder flattens them to src1_/src2_/src3_
prefixed kwargs. ``src3_is_temp`` is the same physical bit the decoder also
exposes as ``src3_sel()``.

For the mulsc/addsc/subsc scalar-with-constant ops, the temp register index of
the register operand is scattered across scalar_opcode bit 0, src3_sel and
src3_swizzle bits 2..5 (see AluInstruction.scalar_const_reg_op_src_temp_reg);
use scalar_const_reg_op_kwargs() to compute those three kwargs from a plain
register index.

Signed fields (exp_adjust, offset, lod_bias, offset_x/y/z) accept negative
values and are encoded two's-complement into their bit width. Every builder
raises ValueError on width overflow or invalid enum values — never silently
truncates.
"""

import os
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from xdis.xenos_ucode import (  # noqa: E402
    AddressingMode,
    AllocType,
    AluScalarOpcode,
    AluVectorOpcode,
    ControlFlowOpcode,
    FetchOpcode,
    FetchOpDimension,
    SampleLocation,
    SignedRepeatingFractionMode,
    TextureFilter,
    _pack_cf_pair,
)


# =============================================================================
# Bitfield packing helpers — exact inverses of _bits/_sbits in the decoder.
# =============================================================================

def _enc(value, lo: int, width: int, name: str) -> int:
    """Place an unsigned value into bitfield [lo, lo+width)."""
    value = int(value)
    if not 0 <= value < (1 << width):
        raise ValueError(
            f"{name}={value} does not fit in {width} unsigned bits")
    return value << lo


def _enc_s(value, lo: int, width: int, name: str) -> int:
    """Place a signed value into bitfield [lo, lo+width), two's complement."""
    value = int(value)
    lo_bound = -(1 << (width - 1))
    hi_bound = (1 << (width - 1)) - 1
    if not lo_bound <= value <= hi_bound:
        raise ValueError(
            f"{name}={value} out of range [{lo_bound}, {hi_bound}] "
            f"for {width} signed bits")
    return (value & ((1 << width) - 1)) << lo


def _flag(value, lo: int) -> int:
    """Place a boolean flag at bit lo."""
    return (1 if value else 0) << lo


def _enum(value, enum_cls, name: str):
    """Validate value against an IntEnum; ValueError if not a member."""
    try:
        return int(enum_cls(int(value)))
    except ValueError:
        raise ValueError(f"{name}={value!r} is not a valid {enum_cls.__name__}")


def _scaled_signed(value, scale: int, lo: int, width: int, name: str) -> int:
    """Encode a float that the decoder returns as raw * (1/scale)."""
    raw = float(value) * scale
    if raw != int(raw):
        raise ValueError(
            f"{name}={value} is not representable in steps of 1/{scale}")
    return _enc_s(int(raw), lo, width, name)


def _check_cf_opcode(opcode, allowed, builder: str) -> int:
    op = _enum(opcode, ControlFlowOpcode, "opcode")
    if ControlFlowOpcode(op) not in allowed:
        raise ValueError(
            f"{builder}: opcode {ControlFlowOpcode(op).name} not in "
            f"{sorted(o.name for o in allowed)}")
    return op


# =============================================================================
# Control flow builders — each returns (dword_0, dword_1), dword_1 <= 0xFFFF.
# =============================================================================

_EXEC_OPS = {ControlFlowOpcode.kExec, ControlFlowOpcode.kExecEnd}
_COND_EXEC_OPS = {
    ControlFlowOpcode.kCondExec,
    ControlFlowOpcode.kCondExecEnd,
    ControlFlowOpcode.kCondExecPredClean,
    ControlFlowOpcode.kCondExecPredCleanEnd,
}
_COND_EXEC_PRED_OPS = {
    ControlFlowOpcode.kCondExecPred,
    ControlFlowOpcode.kCondExecPredEnd,
}


def build_cf_exec(opcode, address=0, count=0, is_yield=False, sequence=0,
                  vc_hi=0, vc_lo=0, is_predicate_clean=False,
                  addressing_mode=AddressingMode.kRelative):
    """ControlFlowExecInstruction (kExec, kExecEnd)."""
    op = _check_cf_opcode(opcode, _EXEC_OPS, "build_cf_exec")
    d0 = (_enc(address, 0, 12, "address")
          | _enc(count, 12, 3, "count")
          | _flag(is_yield, 15)
          | _enc(sequence, 16, 12, "sequence")
          | _enc(vc_hi, 28, 4, "vc_hi"))
    d1 = (_enc(vc_lo, 0, 2, "vc_lo")
          | _flag(is_predicate_clean, 9)
          | _enc(_enum(addressing_mode, AddressingMode, "addressing_mode"),
                 11, 1, "addressing_mode")
          | _enc(op, 12, 4, "opcode"))
    return d0, d1


def build_cf_cond_exec(opcode, address=0, count=0, is_yield=False, sequence=0,
                       vc_hi=0, vc_lo=0, bool_address=0, condition=False,
                       addressing_mode=AddressingMode.kRelative):
    """ControlFlowCondExecInstruction (kCondExec, kCondExecEnd,
    kCondExecPredClean, kCondExecPredCleanEnd)."""
    op = _check_cf_opcode(opcode, _COND_EXEC_OPS, "build_cf_cond_exec")
    d0 = (_enc(address, 0, 12, "address")
          | _enc(count, 12, 3, "count")
          | _flag(is_yield, 15)
          | _enc(sequence, 16, 12, "sequence")
          | _enc(vc_hi, 28, 4, "vc_hi"))
    d1 = (_enc(vc_lo, 0, 2, "vc_lo")
          | _enc(bool_address, 2, 8, "bool_address")
          | _flag(condition, 10)
          | _enc(_enum(addressing_mode, AddressingMode, "addressing_mode"),
                 11, 1, "addressing_mode")
          | _enc(op, 12, 4, "opcode"))
    return d0, d1


def build_cf_cond_exec_pred(opcode, address=0, count=0, is_yield=False,
                            sequence=0, vc_hi=0, vc_lo=0,
                            is_predicate_clean=False, condition=False,
                            addressing_mode=AddressingMode.kRelative):
    """ControlFlowCondExecPredInstruction (kCondExecPred, kCondExecPredEnd)."""
    op = _check_cf_opcode(opcode, _COND_EXEC_PRED_OPS,
                          "build_cf_cond_exec_pred")
    d0 = (_enc(address, 0, 12, "address")
          | _enc(count, 12, 3, "count")
          | _flag(is_yield, 15)
          | _enc(sequence, 16, 12, "sequence")
          | _enc(vc_hi, 28, 4, "vc_hi"))
    d1 = (_enc(vc_lo, 0, 2, "vc_lo")
          | _flag(is_predicate_clean, 9)
          | _flag(condition, 10)
          | _enc(_enum(addressing_mode, AddressingMode, "addressing_mode"),
                 11, 1, "addressing_mode")
          | _enc(op, 12, 4, "opcode"))
    return d0, d1


def build_cf_loop_start(opcode=ControlFlowOpcode.kLoopStart, address=0,
                        is_repeat=False, loop_id=0,
                        addressing_mode=AddressingMode.kRelative):
    """ControlFlowLoopStartInstruction (kLoopStart)."""
    op = _check_cf_opcode(opcode, {ControlFlowOpcode.kLoopStart},
                          "build_cf_loop_start")
    d0 = (_enc(address, 0, 13, "address")
          | _flag(is_repeat, 13)
          | _enc(loop_id, 16, 5, "loop_id"))
    d1 = (_enc(_enum(addressing_mode, AddressingMode, "addressing_mode"),
               11, 1, "addressing_mode")
          | _enc(op, 12, 4, "opcode"))
    return d0, d1


def build_cf_loop_end(opcode=ControlFlowOpcode.kLoopEnd, address=0, loop_id=0,
                      is_predicated_break=False, condition=False,
                      addressing_mode=AddressingMode.kRelative):
    """ControlFlowLoopEndInstruction (kLoopEnd)."""
    op = _check_cf_opcode(opcode, {ControlFlowOpcode.kLoopEnd},
                          "build_cf_loop_end")
    d0 = (_enc(address, 0, 13, "address")
          | _enc(loop_id, 16, 5, "loop_id")
          | _flag(is_predicated_break, 21))
    d1 = (_flag(condition, 10)
          | _enc(_enum(addressing_mode, AddressingMode, "addressing_mode"),
                 11, 1, "addressing_mode")
          | _enc(op, 12, 4, "opcode"))
    return d0, d1


def build_cf_cond_call(opcode=ControlFlowOpcode.kCondCall, address=0,
                       is_unconditional=False, is_predicated=False,
                       bool_address=0, condition=False,
                       addressing_mode=AddressingMode.kRelative):
    """ControlFlowCondCallInstruction (kCondCall)."""
    op = _check_cf_opcode(opcode, {ControlFlowOpcode.kCondCall},
                          "build_cf_cond_call")
    d0 = (_enc(address, 0, 13, "address")
          | _flag(is_unconditional, 13)
          | _flag(is_predicated, 14))
    d1 = (_enc(bool_address, 2, 8, "bool_address")
          | _flag(condition, 10)
          | _enc(_enum(addressing_mode, AddressingMode, "addressing_mode"),
                 11, 1, "addressing_mode")
          | _enc(op, 12, 4, "opcode"))
    return d0, d1


def build_cf_return(opcode=ControlFlowOpcode.kReturn,
                    addressing_mode=AddressingMode.kRelative):
    """ControlFlowReturnInstruction (kReturn)."""
    op = _check_cf_opcode(opcode, {ControlFlowOpcode.kReturn},
                          "build_cf_return")
    d1 = (_enc(_enum(addressing_mode, AddressingMode, "addressing_mode"),
               11, 1, "addressing_mode")
          | _enc(op, 12, 4, "opcode"))
    return 0, d1


def build_cf_cond_jmp(opcode=ControlFlowOpcode.kCondJmp, address=0,
                      is_unconditional=False, is_predicated=False,
                      direction=0, bool_address=0, condition=False,
                      addressing_mode=AddressingMode.kRelative):
    """ControlFlowCondJmpInstruction (kCondJmp)."""
    op = _check_cf_opcode(opcode, {ControlFlowOpcode.kCondJmp},
                          "build_cf_cond_jmp")
    d0 = (_enc(address, 0, 13, "address")
          | _flag(is_unconditional, 13)
          | _flag(is_predicated, 14))
    d1 = (_enc(direction, 1, 1, "direction")
          | _enc(bool_address, 2, 8, "bool_address")
          | _flag(condition, 10)
          | _enc(_enum(addressing_mode, AddressingMode, "addressing_mode"),
                 11, 1, "addressing_mode")
          | _enc(op, 12, 4, "opcode"))
    return d0, d1


def build_cf_alloc(opcode=ControlFlowOpcode.kAlloc, size=0,
                   is_unserialized=False, alloc_type=AllocType.kNone):
    """ControlFlowAllocInstruction (kAlloc). No addressing_mode field."""
    op = _check_cf_opcode(opcode, {ControlFlowOpcode.kAlloc},
                          "build_cf_alloc")
    d0 = _enc(size, 0, 3, "size")
    d1 = (_flag(is_unserialized, 8)
          | _enc(_enum(alloc_type, AllocType, "alloc_type"), 9, 2,
                 "alloc_type")
          | _enc(op, 12, 4, "opcode"))
    return d0, d1


def pack_cf_pair(a_pair, b_pair):
    """Packs two 48-bit CF instruction pairs into three dwords; inverse of
    UnpackControlFlowInstructions. Delegates to the decoder's _pack_cf_pair."""
    a_d0, a_d1 = a_pair
    b_d0, b_d1 = b_pair
    for name, val, width in (("a.dword_0", a_d0, 32), ("a.dword_1", a_d1, 16),
                             ("b.dword_0", b_d0, 32), ("b.dword_1", b_d1, 16)):
        if not 0 <= int(val) < (1 << width):
            raise ValueError(f"pack_cf_pair: {name}={val} does not fit in "
                             f"{width} bits")
    return _pack_cf_pair(a_d0, a_d1, b_d0, b_d1)


# =============================================================================
# Vertex fetch builders — each returns (dword_0, dword_1, dword_2).
# =============================================================================

def build_vfetch_full(opcode=FetchOpcode.kVertexFetch, src=0, src_swizzle=0,
                      is_src_relative=False, dest=0, dest_swizzle=0,
                      is_dest_relative=False, must_be_one=1, const_index=0,
                      const_index_sel=0, prefetch_count=0, data_format=0,
                      exp_adjust=0, is_signed=False, is_normalized=True,
                      signed_rf_mode=SignedRepeatingFractionMode.kZeroClampMinusOne,
                      is_index_rounded=False, stride=0, offset=0,
                      is_predicated=False, predicate_condition=False):
    """VertexFetchInstruction, full fetch (is_mini_fetch forced to 0).

    Notes:
      * prefetch_count is the raw 3-bit field (actual count minus 1), exactly
        as the decoder property returns it.
      * is_normalized is INVERTED in the encoding (num_format_all bit 13:
        0 = normalized, 1 = integer), matching the decoder property.
      * exp_adjust is signed 6-bit [-32, 31]; offset is signed 23-bit.
      * fetch_constant_index is derived (const_index * 3 + const_index_sel) —
        pass const_index/const_index_sel.
    """
    op = _enum(opcode, FetchOpcode, "opcode")
    if FetchOpcode(op) is not FetchOpcode.kVertexFetch:
        raise ValueError("build_vfetch_full: opcode must be kVertexFetch")
    d0 = (_enc(op, 0, 5, "opcode")
          | _enc(src, 5, 6, "src")
          | _flag(is_src_relative, 11)
          | _enc(dest, 12, 6, "dest")
          | _flag(is_dest_relative, 18)
          | _enc(must_be_one, 19, 1, "must_be_one")
          | _enc(const_index, 20, 5, "const_index")
          | _enc(const_index_sel, 25, 2, "const_index_sel")
          | _enc(prefetch_count, 27, 3, "prefetch_count")
          | _enc(src_swizzle, 30, 2, "src_swizzle"))
    d1 = (_enc(dest_swizzle, 0, 12, "dest_swizzle")
          | _flag(is_signed, 12)
          | _flag(not is_normalized, 13)  # num_format_all: 0 == normalized
          | _enc(_enum(signed_rf_mode, SignedRepeatingFractionMode,
                       "signed_rf_mode"), 14, 1, "signed_rf_mode")
          | _flag(is_index_rounded, 15)
          | _enc(data_format, 16, 6, "data_format")
          | _enc_s(exp_adjust, 24, 6, "exp_adjust")
          # bit 30: is_mini_fetch = 0
          | _flag(is_predicated, 31))
    d2 = (_enc(stride, 0, 8, "stride")
          | _enc_s(offset, 8, 23, "offset")
          | _flag(predicate_condition, 31))
    return d0, d1, d2


def build_vfetch_mini(opcode=FetchOpcode.kVertexFetch, dest=0, dest_swizzle=0,
                      is_dest_relative=False, is_predicated=False,
                      predicate_condition=False):
    """VertexFetchInstruction, mini fetch (is_mini_fetch forced to 1).

    A mini fetch inherits src/constant/format/stride/offset state from the
    preceding full fetch; those fields are encoded as zero here and only the
    destination and predication fields are accepted.
    """
    op = _enum(opcode, FetchOpcode, "opcode")
    if FetchOpcode(op) is not FetchOpcode.kVertexFetch:
        raise ValueError("build_vfetch_mini: opcode must be kVertexFetch")
    d0 = (_enc(op, 0, 5, "opcode")
          | _enc(dest, 12, 6, "dest")
          | _flag(is_dest_relative, 18))
    d1 = (_enc(dest_swizzle, 0, 12, "dest_swizzle")
          | _flag(True, 30)  # is_mini_fetch = 1
          | _flag(is_predicated, 31))
    d2 = _flag(predicate_condition, 31)
    return d0, d1, d2


# =============================================================================
# Texture fetch builder.
# =============================================================================

_TFETCH_OPS = {
    FetchOpcode.kTextureFetch,
    FetchOpcode.kGetTextureBorderColorFrac,
    FetchOpcode.kGetTextureComputedLod,
    FetchOpcode.kGetTextureGradients,
    FetchOpcode.kGetTextureWeights,
    FetchOpcode.kSetTextureLod,
    FetchOpcode.kSetTextureGradientsHorz,
    FetchOpcode.kSetTextureGradientsVert,
}


def build_tfetch(opcode=FetchOpcode.kTextureFetch, src=0, src_swizzle=0,
                 is_src_relative=False, dest=0, dest_swizzle=0,
                 is_dest_relative=False, fetch_constant_index=0,
                 fetch_valid_only=False, unnormalized_coordinates=False,
                 mag_filter=TextureFilter.kUseFetchConst,
                 min_filter=TextureFilter.kUseFetchConst,
                 mip_filter=TextureFilter.kUseFetchConst,
                 aniso_filter=7, arbitrary_filter=7,
                 vol_mag_filter=TextureFilter.kUseFetchConst,
                 vol_min_filter=TextureFilter.kUseFetchConst,
                 use_computed_lod=True, use_register_lod=False,
                 use_register_gradients=False,
                 sample_location=SampleLocation.kCentroid, lod_bias=0.0,
                 dimension=FetchOpDimension.k2D, offset_x=0.0, offset_y=0.0,
                 offset_z=0.0, is_predicated=False, predicate_condition=False):
    """TextureFetchInstruction (all texture-family FetchOpcodes).

    lod_bias is in units of 1/16 (signed 7-bit raw, [-4.0, +3.9375]);
    offset_x/y/z are in units of 0.5 (signed 5-bit raw, [-8.0, +7.5]) —
    exactly the scaled floats the decoder properties return.
    aniso_filter / arbitrary_filter are raw 3-bit ints (decoder returns raw).
    """
    op = _enum(opcode, FetchOpcode, "opcode")
    if FetchOpcode(op) not in _TFETCH_OPS:
        raise ValueError(
            f"build_tfetch: {FetchOpcode(op).name} is not a texture fetch "
            f"opcode")
    d0 = (_enc(op, 0, 5, "opcode")
          | _enc(src, 5, 6, "src")
          | _flag(is_src_relative, 11)
          | _enc(dest, 12, 6, "dest")
          | _flag(is_dest_relative, 18)
          | _flag(fetch_valid_only, 19)
          | _enc(fetch_constant_index, 20, 5, "fetch_constant_index")
          | _flag(unnormalized_coordinates, 25)
          | _enc(src_swizzle, 26, 6, "src_swizzle"))
    d1 = (_enc(dest_swizzle, 0, 12, "dest_swizzle")
          | _enc(_enum(mag_filter, TextureFilter, "mag_filter"), 12, 2,
                 "mag_filter")
          | _enc(_enum(min_filter, TextureFilter, "min_filter"), 14, 2,
                 "min_filter")
          | _enc(_enum(mip_filter, TextureFilter, "mip_filter"), 16, 2,
                 "mip_filter")
          | _enc(aniso_filter, 18, 3, "aniso_filter")
          | _enc(arbitrary_filter, 21, 3, "arbitrary_filter")
          | _enc(_enum(vol_mag_filter, TextureFilter, "vol_mag_filter"), 24, 2,
                 "vol_mag_filter")
          | _enc(_enum(vol_min_filter, TextureFilter, "vol_min_filter"), 26, 2,
                 "vol_min_filter")
          | _flag(use_computed_lod, 28)
          | _flag(use_register_lod, 29)
          # bit 30: unk = 0
          | _flag(is_predicated, 31))
    d2 = (_flag(use_register_gradients, 0)
          | _enc(_enum(sample_location, SampleLocation, "sample_location"),
                 1, 1, "sample_location")
          | _scaled_signed(lod_bias, 16, 2, 7, "lod_bias")
          | _enc(_enum(dimension, FetchOpDimension, "dimension"), 14, 2,
                 "dimension")
          | _scaled_signed(offset_x, 2, 16, 5, "offset_x")
          | _scaled_signed(offset_y, 2, 21, 5, "offset_y")
          | _scaled_signed(offset_z, 2, 26, 5, "offset_z")
          | _flag(predicate_condition, 31))
    return d0, d1, d2


# =============================================================================
# ALU builder.
# =============================================================================

def build_alu(vector_opcode=AluVectorOpcode.kMax,
              scalar_opcode=AluScalarOpcode.kRetainPrev,
              vector_dest=0, is_vector_dest_relative=False,
              vector_write_mask=0, vector_clamp=False,
              scalar_dest=0, is_scalar_dest_relative=False,
              scalar_write_mask=0, scalar_clamp=False,
              is_export=False, abs_constants=False,
              is_predicated=False, predicate_condition=False,
              is_const_0_addressed=False, is_const_1_addressed=False,
              is_const_address_register_relative=False,
              src1_const_rel=False, src2_const_rel=False, src3_const_rel=False,
              src1_reg=0, src1_is_temp=True, src1_swizzle=0, src1_negate=False,
              src2_reg=0, src2_is_temp=True, src2_swizzle=0, src2_negate=False,
              src3_reg=0, src3_is_temp=True, src3_swizzle=0,
              src3_negate=False):
    """AluInstruction (co-issued vector + scalar op).

    Notes on the special cases the decoder documents:
      * mulsc/addsc/subsc (kMulsc0/1, kAddsc0/1, kSubsc0/1): the register
        operand's temp index is reassembled by the decoder from
        scalar_opcode bit 0, src3_is_temp (a.k.a. src3_sel) and src3_swizzle
        bits 2..5 — use scalar_const_reg_op_kwargs() to compute those kwargs.
        This builder packs the raw fields verbatim either way.
      * Export semantics (is_export=True): vector/scalar write masks gain the
        constant-0/constant-1 meanings described by GetConstant0WriteMask /
        GetConstant1WriteMask in the decoder; the bits encode identically.
      * srcN_reg is the full 8-bit operand field. For temp operands its low 6
        bits are the register index, bit 6 relative-addressing, bit 7
        absolute-value (see AluInstruction.src_temp_reg etc.). For constant
        operands all 8 bits are the constant index.
      * is_const_0_addressed / is_const_1_addressed are the raw
        const_0_rel_abs / const_1_rel_abs bits; which one applies to operand
        i is the derived selection rule in src_const_is_addressed(i).
      * srcN_const_rel requests aL-relative addressing (c[base + aL]) for
        constant operand N -- the high-level form needed for arrays indexed
        by a hardware-loop counter. The ISA has only two addressing bits, so
        this folds the request onto const_0/const_1 via the same
        src_const_is_addressed(i) rule the decoder uses. Only constant
        operands are addressable (temps carry their own relative bit in bit 6
        of srcN_reg); the request is ignored for temp operands.
        is_const_address_register_relative stays 0 (aL, not a0).
    """
    # Fold per-source aL-relative requests onto the two hardware addressing
    # bits. src_const_is_addressed(i): i=1 -> const_0; i=2 -> const_0 if src1
    # is temp else const_1; i=3 -> const_0 if src1 & src2 both temp else
    # const_1. (xost mirrors the decoder so c[base+aL] round-trips.)
    if src1_const_rel and not src1_is_temp:
        is_const_0_addressed = True
    if src2_const_rel and not src2_is_temp:
        if src1_is_temp:
            is_const_0_addressed = True
        else:
            is_const_1_addressed = True
    if src3_const_rel and not src3_is_temp:
        if src1_is_temp and src2_is_temp:
            is_const_0_addressed = True
        else:
            is_const_1_addressed = True
    d0 = (_enc(vector_dest, 0, 6, "vector_dest")
          | _flag(is_vector_dest_relative, 6)
          | _flag(abs_constants, 7)
          | _enc(scalar_dest, 8, 6, "scalar_dest")
          | _flag(is_scalar_dest_relative, 14)
          | _flag(is_export, 15)
          | _enc(vector_write_mask, 16, 4, "vector_write_mask")
          | _enc(scalar_write_mask, 20, 4, "scalar_write_mask")
          | _flag(vector_clamp, 24)
          | _flag(scalar_clamp, 25)
          | _enc(_enum(scalar_opcode, AluScalarOpcode, "scalar_opcode"),
                 26, 6, "scalar_opcode"))
    d1 = (_enc(src3_swizzle, 0, 8, "src3_swizzle")
          | _enc(src2_swizzle, 8, 8, "src2_swizzle")
          | _enc(src1_swizzle, 16, 8, "src1_swizzle")
          | _flag(src3_negate, 24)
          | _flag(src2_negate, 25)
          | _flag(src1_negate, 26)
          | _flag(predicate_condition, 27)
          | _flag(is_predicated, 28)
          | _flag(is_const_address_register_relative, 29)
          | _flag(is_const_1_addressed, 30)
          | _flag(is_const_0_addressed, 31))
    d2 = (_enc(src3_reg, 0, 8, "src3_reg")
          | _enc(src2_reg, 8, 8, "src2_reg")
          | _enc(src1_reg, 16, 8, "src1_reg")
          | _enc(_enum(vector_opcode, AluVectorOpcode, "vector_opcode"),
                 24, 5, "vector_opcode")
          | _flag(src3_is_temp, 29)
          | _flag(src2_is_temp, 30)
          | _flag(src1_is_temp, 31))
    return d0, d1, d2


def scalar_const_reg_op_kwargs(scalar_opcode, src_temp_reg,
                               src3_swizzle_low2=0):
    """For mulsc/addsc/subsc: scatter the register operand's temp index across
    the fields the decoder reassembles in scalar_const_reg_op_src_temp_reg().

    scalar_opcode: any of kMulsc0/1, kAddsc0/1, kSubsc0/1 (bit 0 is
        overwritten with bit 0 of src_temp_reg).
    src_temp_reg: temp register index [0, 63].
    src3_swizzle_low2: the 2 swizzle bits not consumed by the register index.

    Returns a dict of build_alu kwargs: scalar_opcode, src3_is_temp,
    src3_swizzle.
    """
    base = _enum(scalar_opcode, AluScalarOpcode, "scalar_opcode") & ~1
    if base not in (int(AluScalarOpcode.kMulsc0), int(AluScalarOpcode.kAddsc0),
                    int(AluScalarOpcode.kSubsc0)):
        raise ValueError(
            "scalar_const_reg_op_kwargs: scalar_opcode must be one of "
            "kMulsc0/1, kAddsc0/1, kSubsc0/1")
    src_temp_reg = int(src_temp_reg)
    if not 0 <= src_temp_reg <= 63:
        raise ValueError(f"src_temp_reg={src_temp_reg} out of range [0, 63]")
    if not 0 <= int(src3_swizzle_low2) <= 3:
        raise ValueError("src3_swizzle_low2 must be in [0, 3]")
    return {
        "scalar_opcode": AluScalarOpcode(base | (src_temp_reg & 1)),
        "src3_is_temp": bool((src_temp_reg >> 1) & 1),
        "src3_swizzle": (int(src3_swizzle_low2) & 0x3)
                        | (src_temp_reg & 0x3C),
    }
