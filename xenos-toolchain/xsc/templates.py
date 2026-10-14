"""Instruction field templates for xsc's lowering, from Xenia's definitions.

Each template is a full field set for an xasm.encode builder; the lowering then
overrides registers, opcodes, masks and fetch constants. Field meanings are
Xenia's (xenia:src/xenia/gpu/ucode.h, xenos.h); where a field has no effect on
the result, it takes the value Xenia's disassembler treats as the default
(xenia:src/xenia/gpu/shader_translator_disasm.cc:402-450) or its zero value.

Before xost joined xsdk, tests/test_templates.py checked these against the
instructions they first came from; that test and its test vectors stay in
xost's archived repo (github.com/xenosworks/xenos-toolchain).
"""

import xdis.xenos_ucode as xu

# Fetch destination swizzles are absolute, 3 bits a component (ucode.h:677-693).
DEST_XYZW = 0 | 1 << 3 | 2 << 6 | 3 << 9
# ALU source swizzles are relative to each component, 0 the component itself
# (ucode.h:1999-2003).
SRC_XYZW = 0
# Texture fetch sources are absolute, 2 bits a component, three components
# (ucode.h:851-852): x, y, then x again for a 2D fetch's unused z.
SRC_XY = 0 | 1 << 2 | 0 << 4
# Stream 0's vertex fetch constant, as libxenon numbers it (libxenon:drivers/xenos/xe.c:1965), split into the
# index and select fields (ucode.h:752-754); loaders patch it to the element's stream.
VERTEX_FETCH_CONSTANT = 95
OPOS = int(xu.ExportRegister.kVSPosition)


def vertex_fetch():
    """A full vertex fetch of r0.x, the vertex index, into every component; loaders patch the format and layout."""
    return {
        "opcode": int(xu.FetchOpcode.kVertexFetch), "src": 0, "src_swizzle": 0,
        "is_src_relative": False, "dest": 0, "dest_swizzle": DEST_XYZW,
        "is_dest_relative": False, "must_be_one": 1,
        "const_index": VERTEX_FETCH_CONSTANT // 3,
        "const_index_sel": VERTEX_FETCH_CONSTANT % 3, "prefetch_count": 0,
        "data_format": 0, "exp_adjust": 0, "is_signed": False,
        "is_normalized": True,
        "signed_rf_mode": xu.SignedRepeatingFractionMode.kZeroClampMinusOne,
        "is_index_rounded": False, "stride": 0, "offset": 0,
        "is_predicated": False, "predicate_condition": False,
    }


def _alu(vector_opcode, vector_dest, vector_write_mask, src2_is_temp):
    """A vector op whose scalar half writes nothing (kRetainPrev, mask 0; ucode.h:1330-1332)."""
    return {
        "vector_opcode": vector_opcode,
        "scalar_opcode": xu.AluScalarOpcode.kRetainPrev,
        "vector_dest": vector_dest, "is_vector_dest_relative": False,
        "vector_write_mask": vector_write_mask, "vector_clamp": False,
        "scalar_dest": 0, "is_scalar_dest_relative": False,
        "scalar_write_mask": 0, "scalar_clamp": False, "is_export": True,
        "abs_constants": False, "is_predicated": False,
        "predicate_condition": False, "is_const_0_addressed": False,
        "is_const_1_addressed": False,
        "is_const_address_register_relative": False,
        "src1_reg": 0, "src1_is_temp": True, "src1_swizzle": SRC_XYZW,
        "src1_negate": False,
        "src2_reg": 0, "src2_is_temp": src2_is_temp, "src2_swizzle": SRC_XYZW,
        "src2_negate": False,
        "src3_reg": 0, "src3_is_temp": True, "src3_swizzle": SRC_XYZW,
        "src3_negate": False,
    }


def dp4():
    """oPos.x = dot(temp, constant), the matrix row form the lowering writes one component at a time."""
    return _alu(xu.AluVectorOpcode.kDp4, OPOS, 0b0001, src2_is_temp=False)


def mov():
    """max(r, r): a move, exported to every component."""
    return _alu(xu.AluVectorOpcode.kMax, 0, 0b1111, src2_is_temp=True)


def texture_fetch():
    """A 2D fetch at r0.xy into every component, filtering and LOD from the fetch constant."""
    return {
        "opcode": int(xu.FetchOpcode.kTextureFetch), "src": 0,
        "src_swizzle": SRC_XY, "is_src_relative": False, "dest": 0,
        "dest_swizzle": DEST_XYZW, "is_dest_relative": False,
        "fetch_constant_index": 0, "fetch_valid_only": True,
        "unnormalized_coordinates": False,
        "mag_filter": xu.TextureFilter.kUseFetchConst,
        "min_filter": xu.TextureFilter.kUseFetchConst,
        "mip_filter": xu.TextureFilter.kUseFetchConst,
        # AnisoFilter::kUseFetchConst (xenos.h:140-148); the arbitrary filter's zero value, k2x4Sym (xenos.h:775-783).
        "aniso_filter": 7, "arbitrary_filter": 0,
        "vol_mag_filter": xu.TextureFilter.kUseFetchConst,
        "vol_min_filter": xu.TextureFilter.kUseFetchConst,
        "use_computed_lod": True, "use_register_lod": False,
        "use_register_gradients": False,
        "sample_location": xu.SampleLocation.kCentroid, "lod_bias": 0.0,
        "dimension": xu.FetchOpDimension.k2D, "offset_x": 0.0,
        "offset_y": 0.0, "offset_z": 0.0, "is_predicated": False,
        "predicate_condition": False,
    }


def templates():
    return {"vf": vertex_fetch(), "dp4": dp4(), "mov": mov(),
            "tf": texture_fetch()}
