"""Fuzz round-trip tests for xasm/encode.py against the verified decoder
xdis/xenos_ucode.py.

For every builder: >= 1000 iterations of random valid field vectors,
encode -> construct the corresponding decoder class from the produced dwords
-> assert EVERY field equals the input. Fixed seed for reproducibility.
Prints per-family pass counts; exits nonzero on any failure.
"""

import os
import random
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from xasm import encode as E  # noqa: E402
from xdis import xenos_ucode as D  # noqa: E402

SEED = 0xC0FFEE
ITERS = 1000

_FAILURES = []


def _norm(v):
    """Normalize decoder/builder values for comparison."""
    if isinstance(v, bool):
        return v
    if isinstance(v, float):
        return v
    return int(v)


def _check(family, it, kwargs, name, got, want):
    if isinstance(want, bool) or isinstance(got, bool):
        ok = bool(got) == bool(want)
    elif isinstance(want, float) or isinstance(got, float):
        ok = float(got) == float(want)
    else:
        ok = int(got) == int(want)
    if not ok:
        _FAILURES.append(
            f"{family}[iter {it}] field {name}: encoded {want!r}, "
            f"decoded {got!r}; kwargs={kwargs}")


def _run_family(name, fn):
    fail_before = len(_FAILURES)
    rng = random.Random(SEED)
    for it in range(ITERS):
        fn(name, it, rng)
    new = len(_FAILURES) - fail_before
    status = "PASS" if new == 0 else f"FAIL ({new} mismatches)"
    print(f"{name:<22} {ITERS} iterations  {status}")


def _flag(rng):
    return rng.random() < 0.5


# =============================================================================
# Control flow families.
# =============================================================================

def _check_cf_common(family, it, kwargs, inst, d1):
    assert 0 <= d1 <= 0xFFFF, f"{family}: dword_1 {d1:#x} exceeds 16 bits"
    for k, v in kwargs.items():
        _check(family, it, kwargs, k, getattr(inst, k)(), v)


def fuzz_cf_exec(family, it, rng):
    kwargs = dict(
        opcode=rng.choice([D.ControlFlowOpcode.kExec,
                           D.ControlFlowOpcode.kExecEnd]),
        address=rng.randrange(1 << 12),
        count=rng.randrange(1 << 3),
        is_yield=_flag(rng),
        sequence=rng.randrange(1 << 12),
        vc_hi=rng.randrange(1 << 4),
        vc_lo=rng.randrange(1 << 2),
        is_predicate_clean=_flag(rng),
        addressing_mode=D.AddressingMode(rng.randrange(2)),
    )
    d0, d1 = E.build_cf_exec(**kwargs)
    _check_cf_common(family, it, kwargs,
                     D.ControlFlowExecInstruction(d0, d1), d1)


def fuzz_cf_cond_exec(family, it, rng):
    kwargs = dict(
        opcode=rng.choice([D.ControlFlowOpcode.kCondExec,
                           D.ControlFlowOpcode.kCondExecEnd,
                           D.ControlFlowOpcode.kCondExecPredClean,
                           D.ControlFlowOpcode.kCondExecPredCleanEnd]),
        address=rng.randrange(1 << 12),
        count=rng.randrange(1 << 3),
        is_yield=_flag(rng),
        sequence=rng.randrange(1 << 12),
        vc_hi=rng.randrange(1 << 4),
        vc_lo=rng.randrange(1 << 2),
        bool_address=rng.randrange(1 << 8),
        condition=_flag(rng),
        addressing_mode=D.AddressingMode(rng.randrange(2)),
    )
    d0, d1 = E.build_cf_cond_exec(**kwargs)
    _check_cf_common(family, it, kwargs,
                     D.ControlFlowCondExecInstruction(d0, d1), d1)


def fuzz_cf_cond_exec_pred(family, it, rng):
    kwargs = dict(
        opcode=rng.choice([D.ControlFlowOpcode.kCondExecPred,
                           D.ControlFlowOpcode.kCondExecPredEnd]),
        address=rng.randrange(1 << 12),
        count=rng.randrange(1 << 3),
        is_yield=_flag(rng),
        sequence=rng.randrange(1 << 12),
        vc_hi=rng.randrange(1 << 4),
        vc_lo=rng.randrange(1 << 2),
        is_predicate_clean=_flag(rng),
        condition=_flag(rng),
        addressing_mode=D.AddressingMode(rng.randrange(2)),
    )
    d0, d1 = E.build_cf_cond_exec_pred(**kwargs)
    _check_cf_common(family, it, kwargs,
                     D.ControlFlowCondExecPredInstruction(d0, d1), d1)


def fuzz_cf_loop_start(family, it, rng):
    kwargs = dict(
        opcode=D.ControlFlowOpcode.kLoopStart,
        address=rng.randrange(1 << 13),
        is_repeat=_flag(rng),
        loop_id=rng.randrange(1 << 5),
        addressing_mode=D.AddressingMode(rng.randrange(2)),
    )
    d0, d1 = E.build_cf_loop_start(**kwargs)
    _check_cf_common(family, it, kwargs,
                     D.ControlFlowLoopStartInstruction(d0, d1), d1)


def fuzz_cf_loop_end(family, it, rng):
    kwargs = dict(
        opcode=D.ControlFlowOpcode.kLoopEnd,
        address=rng.randrange(1 << 13),
        loop_id=rng.randrange(1 << 5),
        is_predicated_break=_flag(rng),
        condition=_flag(rng),
        addressing_mode=D.AddressingMode(rng.randrange(2)),
    )
    d0, d1 = E.build_cf_loop_end(**kwargs)
    _check_cf_common(family, it, kwargs,
                     D.ControlFlowLoopEndInstruction(d0, d1), d1)


def fuzz_cf_cond_call(family, it, rng):
    kwargs = dict(
        opcode=D.ControlFlowOpcode.kCondCall,
        address=rng.randrange(1 << 13),
        is_unconditional=_flag(rng),
        is_predicated=_flag(rng),
        bool_address=rng.randrange(1 << 8),
        condition=_flag(rng),
        addressing_mode=D.AddressingMode(rng.randrange(2)),
    )
    d0, d1 = E.build_cf_cond_call(**kwargs)
    _check_cf_common(family, it, kwargs,
                     D.ControlFlowCondCallInstruction(d0, d1), d1)


def fuzz_cf_return(family, it, rng):
    kwargs = dict(
        opcode=D.ControlFlowOpcode.kReturn,
        addressing_mode=D.AddressingMode(rng.randrange(2)),
    )
    d0, d1 = E.build_cf_return(**kwargs)
    _check_cf_common(family, it, kwargs,
                     D.ControlFlowReturnInstruction(d0, d1), d1)


def fuzz_cf_cond_jmp(family, it, rng):
    kwargs = dict(
        opcode=D.ControlFlowOpcode.kCondJmp,
        address=rng.randrange(1 << 13),
        is_unconditional=_flag(rng),
        is_predicated=_flag(rng),
        direction=rng.randrange(2),
        bool_address=rng.randrange(1 << 8),
        condition=_flag(rng),
        addressing_mode=D.AddressingMode(rng.randrange(2)),
    )
    d0, d1 = E.build_cf_cond_jmp(**kwargs)
    _check_cf_common(family, it, kwargs,
                     D.ControlFlowCondJmpInstruction(d0, d1), d1)


def fuzz_cf_alloc(family, it, rng):
    kwargs = dict(
        opcode=D.ControlFlowOpcode.kAlloc,
        size=rng.randrange(1 << 3),
        is_unserialized=_flag(rng),
        alloc_type=D.AllocType(rng.randrange(4)),
    )
    d0, d1 = E.build_cf_alloc(**kwargs)
    _check_cf_common(family, it, kwargs,
                     D.ControlFlowAllocInstruction(d0, d1), d1)


def fuzz_pack_cf_pair(family, it, rng):
    a = (rng.randrange(1 << 32), rng.randrange(1 << 16))
    b = (rng.randrange(1 << 32), rng.randrange(1 << 16))
    dwords = E.pack_cf_pair(a, b)
    assert len(dwords) == 3 and all(0 <= d <= 0xFFFFFFFF for d in dwords)
    cf_a, cf_b = D.UnpackControlFlowInstructions(dwords)
    ctx = {"a": a, "b": b}
    _check(family, it, ctx, "a.dword_0", cf_a.dword_0, a[0])
    _check(family, it, ctx, "a.dword_1", cf_a.dword_1, a[1])
    _check(family, it, ctx, "b.dword_0", cf_b.dword_0, b[0])
    _check(family, it, ctx, "b.dword_1", cf_b.dword_1, b[1])


# =============================================================================
# Vertex fetch.
# =============================================================================

def fuzz_vfetch_full(family, it, rng):
    kwargs = dict(
        opcode=D.FetchOpcode.kVertexFetch,
        src=rng.randrange(1 << 6),
        src_swizzle=rng.randrange(1 << 2),
        is_src_relative=_flag(rng),
        dest=rng.randrange(1 << 6),
        dest_swizzle=rng.randrange(1 << 12),
        is_dest_relative=_flag(rng),
        must_be_one=rng.randrange(2),
        const_index=rng.randrange(1 << 5),
        const_index_sel=rng.randrange(1 << 2),
        prefetch_count=rng.randrange(1 << 3),
        data_format=rng.randrange(1 << 6),
        exp_adjust=rng.randint(-32, 31),
        is_signed=_flag(rng),
        is_normalized=_flag(rng),
        signed_rf_mode=D.SignedRepeatingFractionMode(rng.randrange(2)),
        is_index_rounded=_flag(rng),
        stride=rng.randrange(1 << 8),
        offset=rng.randint(-(1 << 22), (1 << 22) - 1),
        is_predicated=_flag(rng),
        predicate_condition=_flag(rng),
    )
    d0, d1, d2 = E.build_vfetch_full(**kwargs)
    inst = D.VertexFetchInstruction(d0, d1, d2)
    for k, v in kwargs.items():
        _check(family, it, kwargs, k, getattr(inst, k)(), v)
    _check(family, it, kwargs, "is_mini_fetch", inst.is_mini_fetch(), False)
    _check(family, it, kwargs, "fetch_constant_index",
           inst.fetch_constant_index(),
           kwargs["const_index"] * 3 + kwargs["const_index_sel"])


def fuzz_vfetch_mini(family, it, rng):
    kwargs = dict(
        opcode=D.FetchOpcode.kVertexFetch,
        dest=rng.randrange(1 << 6),
        dest_swizzle=rng.randrange(1 << 12),
        is_dest_relative=_flag(rng),
        is_predicated=_flag(rng),
        predicate_condition=_flag(rng),
    )
    d0, d1, d2 = E.build_vfetch_mini(**kwargs)
    inst = D.VertexFetchInstruction(d0, d1, d2)
    for k, v in kwargs.items():
        _check(family, it, kwargs, k, getattr(inst, k)(), v)
    _check(family, it, kwargs, "is_mini_fetch", inst.is_mini_fetch(), True)
    # Inherited fields must encode as zero.
    for k in ("src", "src_swizzle", "const_index", "const_index_sel",
              "prefetch_count", "data_format", "exp_adjust", "stride",
              "offset", "must_be_one"):
        _check(family, it, kwargs, f"zeroed:{k}", getattr(inst, k)(), 0)


# =============================================================================
# Texture fetch.
# =============================================================================

_TFETCH_OPCODES = sorted(E._TFETCH_OPS)


def fuzz_tfetch(family, it, rng):
    kwargs = dict(
        opcode=rng.choice(_TFETCH_OPCODES),
        src=rng.randrange(1 << 6),
        src_swizzle=rng.randrange(1 << 6),
        is_src_relative=_flag(rng),
        dest=rng.randrange(1 << 6),
        dest_swizzle=rng.randrange(1 << 12),
        is_dest_relative=_flag(rng),
        fetch_constant_index=rng.randrange(1 << 5),
        fetch_valid_only=_flag(rng),
        unnormalized_coordinates=_flag(rng),
        mag_filter=D.TextureFilter(rng.randrange(4)),
        min_filter=D.TextureFilter(rng.randrange(4)),
        mip_filter=D.TextureFilter(rng.randrange(4)),
        aniso_filter=rng.randrange(1 << 3),
        arbitrary_filter=rng.randrange(1 << 3),
        vol_mag_filter=D.TextureFilter(rng.randrange(4)),
        vol_min_filter=D.TextureFilter(rng.randrange(4)),
        use_computed_lod=_flag(rng),
        use_register_lod=_flag(rng),
        use_register_gradients=_flag(rng),
        sample_location=D.SampleLocation(rng.randrange(2)),
        lod_bias=rng.randint(-64, 63) / 16.0,
        dimension=D.FetchOpDimension(rng.randrange(4)),
        offset_x=rng.randint(-16, 15) / 2.0,
        offset_y=rng.randint(-16, 15) / 2.0,
        offset_z=rng.randint(-16, 15) / 2.0,
        is_predicated=_flag(rng),
        predicate_condition=_flag(rng),
    )
    d0, d1, d2 = E.build_tfetch(**kwargs)
    inst = D.TextureFetchInstruction(d0, d1, d2)
    for k, v in kwargs.items():
        _check(family, it, kwargs, k, getattr(inst, k)(), v)


# =============================================================================
# ALU.
# =============================================================================

def _random_alu_kwargs(rng):
    return dict(
        vector_opcode=D.AluVectorOpcode(rng.randrange(1 << 5)),
        scalar_opcode=D.AluScalarOpcode(rng.randrange(1 << 6)),
        vector_dest=rng.randrange(1 << 6),
        is_vector_dest_relative=_flag(rng),
        vector_write_mask=rng.randrange(1 << 4),
        vector_clamp=_flag(rng),
        scalar_dest=rng.randrange(1 << 6),
        is_scalar_dest_relative=_flag(rng),
        scalar_write_mask=rng.randrange(1 << 4),
        scalar_clamp=_flag(rng),
        is_export=_flag(rng),
        abs_constants=_flag(rng),
        is_predicated=_flag(rng),
        predicate_condition=_flag(rng),
        is_const_0_addressed=_flag(rng),
        is_const_1_addressed=_flag(rng),
        is_const_address_register_relative=_flag(rng),
        src1_reg=rng.randrange(1 << 8),
        src1_is_temp=_flag(rng),
        src1_swizzle=rng.randrange(1 << 8),
        src1_negate=_flag(rng),
        src2_reg=rng.randrange(1 << 8),
        src2_is_temp=_flag(rng),
        src2_swizzle=rng.randrange(1 << 8),
        src2_negate=_flag(rng),
        src3_reg=rng.randrange(1 << 8),
        src3_is_temp=_flag(rng),
        src3_swizzle=rng.randrange(1 << 8),
        src3_negate=_flag(rng),
    )


def _check_alu(family, it, kwargs, inst):
    direct = ("vector_opcode", "scalar_opcode", "vector_dest",
              "is_vector_dest_relative", "vector_write_mask", "vector_clamp",
              "scalar_dest", "is_scalar_dest_relative", "scalar_write_mask",
              "scalar_clamp", "is_export", "abs_constants", "is_predicated",
              "predicate_condition", "is_const_0_addressed",
              "is_const_1_addressed", "is_const_address_register_relative")
    for k in direct:
        _check(family, it, kwargs, k, getattr(inst, k)(), kwargs[k])
    for i in (1, 2, 3):
        _check(family, it, kwargs, f"src{i}_reg",
               inst.src_reg(i), kwargs[f"src{i}_reg"])
        _check(family, it, kwargs, f"src{i}_is_temp",
               inst.src_is_temp(i), kwargs[f"src{i}_is_temp"])
        _check(family, it, kwargs, f"src{i}_swizzle",
               inst.src_swizzle(i), kwargs[f"src{i}_swizzle"])
        _check(family, it, kwargs, f"src{i}_negate",
               inst.src_negate(i), kwargs[f"src{i}_negate"])
    _check(family, it, kwargs, "src3_sel",
           inst.src3_sel(), 1 if kwargs["src3_is_temp"] else 0)


def fuzz_alu(family, it, rng):
    kwargs = _random_alu_kwargs(rng)
    d0, d1, d2 = E.build_alu(**kwargs)
    _check_alu(family, it, kwargs, D.AluInstruction(d0, d1, d2))


def fuzz_alu_scalar_const_reg(family, it, rng):
    """mulsc/addsc/subsc: temp register index reassembly round trip."""
    kwargs = _random_alu_kwargs(rng)
    base_op = rng.choice([D.AluScalarOpcode.kMulsc0, D.AluScalarOpcode.kMulsc1,
                          D.AluScalarOpcode.kAddsc0, D.AluScalarOpcode.kAddsc1,
                          D.AluScalarOpcode.kSubsc0,
                          D.AluScalarOpcode.kSubsc1])
    temp_reg = rng.randrange(1 << 6)
    low2 = rng.randrange(1 << 2)
    kwargs.update(E.scalar_const_reg_op_kwargs(base_op, temp_reg, low2))
    d0, d1, d2 = E.build_alu(**kwargs)
    inst = D.AluInstruction(d0, d1, d2)
    _check_alu(family, it, kwargs, inst)
    _check(family, it, kwargs, "scalar_const_reg_op_src_temp_reg",
           inst.scalar_const_reg_op_src_temp_reg(), temp_reg)
    _check(family, it, kwargs, "scalar_opcode_base",
           inst.scalar_opcode_raw() & ~1, int(base_op) & ~1)
    _check(family, it, kwargs, "src3_swizzle_low2",
           inst.src_swizzle(3) & 0x3, low2)


# =============================================================================
# Error-path sanity checks (not fuzzed, just a handful of asserts).
# =============================================================================

def _expect_value_error(fn, label):
    try:
        fn()
    except ValueError:
        return
    _FAILURES.append(f"error-paths: {label} did not raise ValueError")


def run_error_paths():
    _expect_value_error(
        lambda: E.build_cf_exec(D.ControlFlowOpcode.kCondExec),
        "build_cf_exec(kCondExec)")
    _expect_value_error(
        lambda: E.build_cf_exec(D.ControlFlowOpcode.kExec, address=1 << 12),
        "build_cf_exec(address overflow)")
    _expect_value_error(
        lambda: E.build_vfetch_full(exp_adjust=32),
        "build_vfetch_full(exp_adjust=32)")
    _expect_value_error(
        lambda: E.build_vfetch_full(offset=1 << 22),
        "build_vfetch_full(offset overflow)")
    _expect_value_error(
        lambda: E.build_tfetch(opcode=D.FetchOpcode.kVertexFetch),
        "build_tfetch(kVertexFetch)")
    _expect_value_error(
        lambda: E.build_tfetch(lod_bias=0.01),
        "build_tfetch(lod_bias not 1/16 step)")
    _expect_value_error(
        lambda: E.build_tfetch(offset_x=8.0),
        "build_tfetch(offset_x=8.0)")
    _expect_value_error(
        lambda: E.build_alu(vector_opcode=32),
        "build_alu(vector_opcode=32)")
    _expect_value_error(
        lambda: E.build_alu(src1_reg=256),
        "build_alu(src1_reg=256)")
    _expect_value_error(
        lambda: E.scalar_const_reg_op_kwargs(D.AluScalarOpcode.kAdds, 0),
        "scalar_const_reg_op_kwargs(kAdds)")
    _expect_value_error(
        lambda: E.pack_cf_pair((0, 1 << 16), (0, 0)),
        "pack_cf_pair(a.dword_1 overflow)")
    print(f"{'error-paths':<22} 11 checks         "
          f"{'PASS' if not _FAILURES else 'see failures'}")


def main():
    families = [
        ("cf_exec", fuzz_cf_exec),
        ("cf_cond_exec", fuzz_cf_cond_exec),
        ("cf_cond_exec_pred", fuzz_cf_cond_exec_pred),
        ("cf_loop_start", fuzz_cf_loop_start),
        ("cf_loop_end", fuzz_cf_loop_end),
        ("cf_cond_call", fuzz_cf_cond_call),
        ("cf_return", fuzz_cf_return),
        ("cf_cond_jmp", fuzz_cf_cond_jmp),
        ("cf_alloc", fuzz_cf_alloc),
        ("pack_cf_pair", fuzz_pack_cf_pair),
        ("vfetch_full", fuzz_vfetch_full),
        ("vfetch_mini", fuzz_vfetch_mini),
        ("tfetch", fuzz_tfetch),
        ("alu", fuzz_alu),
        ("alu_scalar_const_reg", fuzz_alu_scalar_const_reg),
    ]
    for name, fn in families:
        _run_family(name, fn)
    run_error_paths()
    if _FAILURES:
        print(f"\n{len(_FAILURES)} FAILURE(S):")
        for f in _FAILURES[:50]:
            print("  " + f)
        if len(_FAILURES) > 50:
            print(f"  ... and {len(_FAILURES) - 50} more")
        sys.exit(1)
    print("\nAll encoder fuzz tests passed.")
    sys.exit(0)


if __name__ == "__main__":
    main()
