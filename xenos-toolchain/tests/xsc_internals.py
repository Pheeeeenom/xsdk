"""The C compiler's internals (xsdk:src/xsc_private.h) through ctypes, for the tests that build instructions and programs
with them directly: its encoder, and its program builder with the paths only the compiler takes (whole sources, the
fetch layout, the optimiser). Each structure mirrors the C one; a program's tail is room the C alone reads.
"""
import ctypes
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xsc.native as native  # noqa: E402

I64, BOOL, DOUBLE = ctypes.c_int64, ctypes.c_bool, ctypes.c_double
# A whole temp source's absolute value, past its register (xsc_private.h XSC_ABS_SOURCE).
ABS_SOURCE = 1 << 16
ALLOC_COLORS = 2


class Xsc(ctypes.Structure):
    _fields_ = [("chunks", ctypes.c_void_p), ("status", ctypes.c_int),
                ("message", ctypes.c_char * native.MESSAGE_BYTES)]


class Alu(ctypes.Structure):
    _fields_ = [("vector_opcode", I64), ("scalar_opcode", I64), ("vector_dest", I64), ("is_vector_dest_relative", BOOL),
                ("vector_write_mask", I64), ("vector_clamp", BOOL), ("scalar_dest", I64),
                ("is_scalar_dest_relative", BOOL), ("scalar_write_mask", I64), ("scalar_clamp", BOOL),
                ("is_export", BOOL), ("abs_constants", BOOL), ("is_predicated", BOOL), ("predicate_condition", BOOL),
                ("is_const_0_addressed", BOOL), ("is_const_1_addressed", BOOL),
                ("is_const_address_register_relative", BOOL), ("src_const_rel", BOOL * 3), ("src_reg", I64 * 3),
                ("src_is_temp", BOOL * 3), ("src_swizzle", I64 * 3), ("src_negate", BOOL * 3)]


FETCH_FIELDS = [("opcode", I64), ("src", I64), ("src_swizzle", I64), ("is_src_relative", BOOL), ("dest", I64),
                ("dest_swizzle", I64), ("is_dest_relative", BOOL), ("fetch_constant_index", I64),
                ("fetch_valid_only", BOOL), ("unnormalized_coordinates", BOOL), ("mag_filter", I64),
                ("min_filter", I64), ("mip_filter", I64), ("aniso_filter", I64), ("arbitrary_filter", I64),
                ("vol_mag_filter", I64), ("vol_min_filter", I64), ("use_computed_lod", BOOL),
                ("use_register_lod", BOOL), ("use_register_gradients", BOOL), ("sample_location", I64),
                ("lod_bias", DOUBLE), ("dimension", I64), ("offset_x", DOUBLE), ("offset_y", DOUBLE),
                ("offset_z", DOUBLE), ("must_be_one", I64), ("const_index", I64), ("const_index_sel", I64),
                ("prefetch_count", I64), ("data_format", I64), ("exp_adjust", I64), ("is_signed", BOOL),
                ("is_normalized", BOOL), ("signed_rf_mode", I64), ("is_index_rounded", BOOL), ("stride", I64),
                ("offset", I64), ("is_predicated", BOOL), ("predicate_condition", BOOL)]


class Fetch(ctypes.Structure):
    _fields_ = FETCH_FIELDS


class Program(ctypes.Structure):
    """XscProgram's flags, then room for the rest of it."""
    _fields_ = [("pixel", BOOL), ("registers", ctypes.c_uint32), ("whole_sources", BOOL), ("optimize", BOOL),
                ("group_fetches", BOOL), ("clauses", BOOL), ("rest", ctypes.c_uint8 * 1024)]


def library():
    lib = native._load()
    triple, pair = ctypes.c_uint32 * 3, ctypes.c_uint32 * 2
    x, p = ctypes.POINTER(Xsc), ctypes.POINTER(Program)
    signatures = {
        "xsdk_xsc_build_alu": ([x, ctypes.POINTER(Alu), triple], BOOL),
        "xsdk_xsc_build_tfetch": ([x, ctypes.POINTER(Fetch), triple], BOOL),
        "xsdk_xsc_build_vfetch": ([x, ctypes.POINTER(Fetch), triple], BOOL),
        "xsdk_xsc_build_cf_exec": ([x, ctypes.c_uint32, I64, I64, I64, I64, BOOL, pair], BOOL),
        "xsdk_xsc_build_cf_alloc": ([x, I64, pair], BOOL),
        "xsdk_xsc_build_cf_loop_start": ([x, I64, I64, pair], BOOL),
        "xsdk_xsc_build_cf_loop_end": ([x, I64, I64, BOOL, BOOL, pair], BOOL),
        "xsdk_xsc_pack_cf_pair": ([pair, pair, triple], None),
        "xsdk_xsc_program_init": ([x, p, BOOL, ctypes.c_uint32], BOOL),
        "xsdk_xsc_program_input": ([x, p, I64, ctypes.c_uint32], BOOL),
        "xsdk_xsc_program_alu": ([x, p, ctypes.POINTER(Alu)], ctypes.c_void_p),
        "xsdk_xsc_program_alloc": ([x, p, I64], BOOL),
        "xsdk_xsc_program_finish": ([x, p, ctypes.c_void_p, ctypes.c_size_t,
                                     ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)), ctypes.POINTER(ctypes.c_size_t)],
                                    BOOL),
        "xsdk_xsc_release": ([x], None),
    }
    for name, (argtypes, restype) in signatures.items():
        getattr(lib, name).argtypes = argtypes
        getattr(lib, name).restype = restype
    return lib


LIB = library()


def alu_struct(fields: dict) -> Alu:
    """An ALU instruction's fields as xasm.encode.build_alu names them, as the C's structure."""
    a = Alu()
    for name, _ in Alu._fields_:
        if not name.startswith("src_"):
            setattr(a, name, int(fields.get(name, 0)))
    for i in range(3):
        n = i + 1
        a.src_const_rel[i] = fields.get(f"src{n}_const_rel", False)
        a.src_reg[i] = fields.get(f"src{n}_reg", 0)
        a.src_is_temp[i] = fields.get(f"src{n}_is_temp", True)
        a.src_swizzle[i] = fields.get(f"src{n}_swizzle", 0)
        a.src_negate[i] = fields.get(f"src{n}_negate", False)
    return a


def pixel_program(inputs: int, usage: int, body, exports, registers: int, optimize: bool) -> bytes:
    """A pixel shader the C builds from ALU fields as the compiler would, temps whole till it packs them: inputs
    interpolators, the body, the colors' alloc, the exports. Raises CompileError where it refuses."""
    x, p = Xsc(), Program()
    container = ctypes.POINTER(ctypes.c_uint8)()
    size = ctypes.c_size_t()
    try:
        ok = LIB.xsdk_xsc_program_init(ctypes.byref(x), ctypes.byref(p), True, registers)
        p.whole_sources = True
        p.optimize = optimize
        for r in range(inputs):
            ok = ok and LIB.xsdk_xsc_program_input(ctypes.byref(x), ctypes.byref(p), r, usage)
        for f in body:
            ok = ok and bool(LIB.xsdk_xsc_program_alu(ctypes.byref(x), ctypes.byref(p), ctypes.byref(alu_struct(f))))
        ok = ok and LIB.xsdk_xsc_program_alloc(ctypes.byref(x), ctypes.byref(p), ALLOC_COLORS)
        for f in exports:
            ok = ok and bool(LIB.xsdk_xsc_program_alu(ctypes.byref(x), ctypes.byref(p), ctypes.byref(alu_struct(f))))
        ok = ok and LIB.xsdk_xsc_program_finish(ctypes.byref(x), ctypes.byref(p), None, 0, ctypes.byref(container),
                                                ctypes.byref(size))
        if not ok:
            raise native.CompileError(x.status, x.message.decode(errors="replace"))
        return ctypes.string_at(container, size.value)
    finally:
        LIB.xsdk_xsc_release(ctypes.byref(x))
