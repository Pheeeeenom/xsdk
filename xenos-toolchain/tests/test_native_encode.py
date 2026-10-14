#!/usr/bin/env python3
"""The C encoder (xsdk:src/xsc_encode.c) builds what xasm/encode.py builds: on random fields as test_encode_fuzz.py
draws them, the same words, and on fields out of range, the same error. Only the fields the compiler sets are the C's
to take. Runs in WSL, where make builds build/host/libxsc.so.
"""
import ctypes
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

from test_encode_fuzz import _flag, _random_alu_kwargs  # noqa: E402
from xsc_internals import DOUBLE, FETCH_FIELDS, LIB, Fetch, Xsc, alu_struct  # noqa: E402
from xasm import encode as E  # noqa: E402
from xdis import xenos_ucode as D  # noqa: E402

SEED = 0xC0FFEE
ITERS = 1000


def call_c(name, *args, words=3):
    """The words the C builder gives, or ('error', message)."""
    x = Xsc()
    out = (ctypes.c_uint32 * words)()
    ok = getattr(LIB, name)(ctypes.byref(x), *args, out)
    if not ok:
        return "error", x.message.decode()
    return tuple(out)


def call_python(fn, **kwargs):
    try:
        return tuple(int(w) for w in fn(**kwargs))
    except ValueError as e:
        return "error", str(e)


def fetch_struct(kwargs, defaults):
    f = Fetch()
    for name, kind in FETCH_FIELDS:
        value = kwargs.get(name, defaults.get(name, 0))
        setattr(f, name, float(value) if kind is DOUBLE else int(value))
    return f


TFETCH_DEFAULTS = dict(mag_filter=D.TextureFilter.kUseFetchConst, min_filter=D.TextureFilter.kUseFetchConst,
                       mip_filter=D.TextureFilter.kUseFetchConst, aniso_filter=7, arbitrary_filter=7,
                       vol_mag_filter=D.TextureFilter.kUseFetchConst, vol_min_filter=D.TextureFilter.kUseFetchConst,
                       use_computed_lod=True, sample_location=D.SampleLocation.kCentroid,
                       dimension=D.FetchOpDimension.k2D, opcode=D.FetchOpcode.kTextureFetch)
VFETCH_DEFAULTS = dict(must_be_one=1, is_normalized=True,
                       signed_rf_mode=D.SignedRepeatingFractionMode.kZeroClampMinusOne)


def alu_case(rng):
    kwargs = _random_alu_kwargs(rng)
    for n in (1, 2, 3):
        kwargs[f"src{n}_const_rel"] = _flag(rng)
    return kwargs


def tfetch_case(rng):
    return dict(
        opcode=rng.choice(sorted(E._TFETCH_OPS)), src=rng.randrange(1 << 6), src_swizzle=rng.randrange(1 << 6),
        is_src_relative=_flag(rng), dest=rng.randrange(1 << 6), dest_swizzle=rng.randrange(1 << 12),
        is_dest_relative=_flag(rng), fetch_constant_index=rng.randrange(1 << 5), fetch_valid_only=_flag(rng),
        unnormalized_coordinates=_flag(rng), mag_filter=D.TextureFilter(rng.randrange(4)),
        min_filter=D.TextureFilter(rng.randrange(4)), mip_filter=D.TextureFilter(rng.randrange(4)),
        aniso_filter=rng.randrange(1 << 3), arbitrary_filter=rng.randrange(1 << 3),
        vol_mag_filter=D.TextureFilter(rng.randrange(4)), vol_min_filter=D.TextureFilter(rng.randrange(4)),
        use_computed_lod=_flag(rng), use_register_lod=_flag(rng), use_register_gradients=_flag(rng),
        sample_location=D.SampleLocation(rng.randrange(2)), lod_bias=rng.randint(-64, 63) / 16.0,
        dimension=D.FetchOpDimension(rng.randrange(4)), offset_x=rng.randint(-16, 15) / 2.0,
        offset_y=rng.randint(-16, 15) / 2.0, offset_z=rng.randint(-16, 15) / 2.0, is_predicated=_flag(rng),
        predicate_condition=_flag(rng))


def vfetch_case(rng):
    return dict(
        opcode=D.FetchOpcode.kVertexFetch, src=rng.randrange(1 << 6), src_swizzle=rng.randrange(1 << 2),
        is_src_relative=_flag(rng), dest=rng.randrange(1 << 6), dest_swizzle=rng.randrange(1 << 12),
        is_dest_relative=_flag(rng), must_be_one=rng.randrange(2), const_index=rng.randrange(1 << 5),
        const_index_sel=rng.randrange(1 << 2), prefetch_count=rng.randrange(1 << 3),
        data_format=rng.randrange(1 << 6), exp_adjust=rng.randint(-32, 31), is_signed=_flag(rng),
        is_normalized=_flag(rng), signed_rf_mode=D.SignedRepeatingFractionMode(rng.randrange(2)),
        is_index_rounded=_flag(rng), stride=rng.randrange(1 << 8), offset=rng.randint(-(1 << 22), (1 << 22) - 1),
        is_predicated=_flag(rng), predicate_condition=_flag(rng))


def check_alu(kwargs):
    return call_python(E.build_alu, **kwargs), call_c("xsdk_xsc_build_alu", ctypes.byref(alu_struct(kwargs)))


def check_tfetch(kwargs):
    c = call_c("xsdk_xsc_build_tfetch", ctypes.byref(fetch_struct(kwargs, TFETCH_DEFAULTS)))
    return call_python(E.build_tfetch, **kwargs), c


def check_vfetch(kwargs):
    c = call_c("xsdk_xsc_build_vfetch", ctypes.byref(fetch_struct(kwargs, VFETCH_DEFAULTS)))
    return call_python(E.build_vfetch_full, **kwargs), c


def check_cf_exec(kwargs):
    py = call_python(E.build_cf_exec, **kwargs)
    c = call_c("xsdk_xsc_build_cf_exec", int(kwargs["opcode"]), kwargs["address"], kwargs["count"],
               kwargs["sequence"], kwargs["vc_hi"], kwargs["is_predicate_clean"], words=2)
    return py, c


def check_cf_alloc(kwargs):
    return call_python(E.build_cf_alloc, **kwargs), call_c("xsdk_xsc_build_cf_alloc", int(kwargs["alloc_type"]),
                                                           words=2)


def check_cf_loop_start(kwargs):
    py = call_python(E.build_cf_loop_start, **kwargs)
    return py, call_c("xsdk_xsc_build_cf_loop_start", kwargs["address"], kwargs["loop_id"], words=2)


def check_cf_loop_end(kwargs):
    py = call_python(E.build_cf_loop_end, **kwargs)
    c = call_c("xsdk_xsc_build_cf_loop_end", kwargs["address"], kwargs["loop_id"], kwargs["is_predicated_break"],
               kwargs["condition"], words=2)
    return py, c


def check_pack(kwargs):
    pair = ctypes.c_uint32 * 2
    out = (ctypes.c_uint32 * 3)()
    LIB.xsdk_xsc_pack_cf_pair(pair(*kwargs["a"]), pair(*kwargs["b"]), out)
    return call_python(E.pack_cf_pair, a_pair=kwargs["a"], b_pair=kwargs["b"]), tuple(out)


FAMILIES = [
    ("alu", alu_case, check_alu),
    ("tfetch", tfetch_case, check_tfetch),
    ("vfetch", vfetch_case, check_vfetch),
    ("cf_exec", lambda rng: dict(opcode=rng.choice([D.ControlFlowOpcode.kExec, D.ControlFlowOpcode.kExecEnd]),
                                 address=rng.randrange(1 << 12), count=rng.randrange(1 << 3),
                                 sequence=rng.randrange(1 << 12), vc_hi=rng.randrange(1 << 4),
                                 is_predicate_clean=_flag(rng)), check_cf_exec),
    ("cf_alloc", lambda rng: dict(alloc_type=D.AllocType(rng.randrange(4))), check_cf_alloc),
    ("cf_loop_start", lambda rng: dict(address=rng.randrange(1 << 13), loop_id=rng.randrange(1 << 5)),
     check_cf_loop_start),
    ("cf_loop_end", lambda rng: dict(address=rng.randrange(1 << 13), loop_id=rng.randrange(1 << 5),
                                     is_predicated_break=_flag(rng), condition=_flag(rng)), check_cf_loop_end),
    ("pack_cf_pair", lambda rng: dict(a=(rng.randrange(1 << 32), rng.randrange(1 << 16)),
                                      b=(rng.randrange(1 << 32), rng.randrange(1 << 16))), check_pack),
]

# Fields out of range, and values the field can't hold: the C must refuse each as Python does.
ERRORS = [
    ("cf_exec address", check_cf_exec, dict(opcode=D.ControlFlowOpcode.kExec, address=1 << 12, count=0, sequence=0,
                                            vc_hi=0, is_predicate_clean=False)),
    ("cf_exec count", check_cf_exec, dict(opcode=D.ControlFlowOpcode.kExec, address=0, count=8, sequence=0, vc_hi=0,
                                          is_predicate_clean=False)),
    ("cf_alloc type", check_cf_alloc, dict(alloc_type=4)),
    ("cf_loop_start loop_id", check_cf_loop_start, dict(address=0, loop_id=32)),
    ("cf_loop_end address", check_cf_loop_end, dict(address=1 << 13, loop_id=0, is_predicated_break=False,
                                                    condition=False)),
    ("vfetch exp_adjust", check_vfetch, dict(exp_adjust=32)),
    ("vfetch offset", check_vfetch, dict(offset=1 << 22)),
    ("vfetch stride", check_vfetch, dict(stride=256)),
    ("tfetch opcode", check_tfetch, dict(opcode=D.FetchOpcode.kVertexFetch)),
    ("tfetch lod_bias step", check_tfetch, dict(lod_bias=0.01)),
    ("tfetch lod_bias range", check_tfetch, dict(lod_bias=4.0)),
    ("tfetch offset_x", check_tfetch, dict(offset_x=8.0)),
    ("tfetch filter", check_tfetch, dict(mag_filter=4)),
    ("alu vector_opcode", check_alu, dict(vector_opcode=32)),
    ("alu scalar_opcode", check_alu, dict(scalar_opcode=64)),
    ("alu src1_reg", check_alu, dict(src1_reg=256)),
    ("alu vector_dest", check_alu, dict(vector_dest=64)),
    ("alu write mask", check_alu, dict(vector_write_mask=16)),
    ("alu swizzle", check_alu, dict(src2_swizzle=256)),
    ("alu negative register", check_alu, dict(src3_reg=-1)),
]


def main():
    failures = []
    for name, case, check in FAMILIES:
        rng = random.Random(SEED)
        bad = 0
        for it in range(ITERS):
            kwargs = case(rng)
            py, c = check(kwargs)
            if py != c:
                bad += 1
                failures.append(f"{name}[{it}]: python {py}, c {c}; {kwargs}")
        print(f"{name:<16} {ITERS} iterations  {'PASS' if not bad else f'FAIL ({bad})'}")
    bad = 0
    for label, check, kwargs in ERRORS:
        py, c = check(kwargs)
        if py[0] != "error" or py != c:
            bad += 1
            failures.append(f"error {label}: python {py}, c {c}")
    print(f"{'errors':<16} {len(ERRORS)} cases        {'PASS' if not bad else f'FAIL ({bad})'}")
    for f in failures[:30]:
        print("  " + f)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
