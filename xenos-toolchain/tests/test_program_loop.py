#!/usr/bin/env python3
"""Exit gate for real hardware loops (LOOP_START/LOOP_END) emitted by
xasm.program. Builds a pixel shader with a hardware loop and checks:
  - the control-flow program has LOOP_START/LOOP_END with correct jump
    addresses (skip past LOOP_END; body = the exec after LOOP_START), and
  - XenosRecomp decompiles it to `for (aL = 0; aL < i0.x; aL++)` (the trip
    count comes from loop constant i0, uploaded by the host at draw time).
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xdis.container as container          # noqa: E402
import xdis.xenos_ucode as xu               # noqa: E402
import xdis.container_emit as ce            # noqa: E402
from xsc.lower import _templates            # noqa: E402
from xasm import encode as enc              # noqa: E402
from xasm.program import PixelProgram       # noqa: E402
from tests.test_xsc_lower import oracle_hlsl  # noqa: E402

_V = xu.AluVectorOpcode
_S = xu.AluScalarOpcode


def build_loop_shader():
    T = _templates()

    def alu(op, dest, srcs, export=False):
        f = dict(T["mov"], vector_opcode=op, vector_dest=dest, is_export=export,
                 vector_write_mask=0xF, vector_clamp=False, scalar_write_mask=0)
        for i, (reg, is_temp) in enumerate(srcs, start=1):
            f[f"src{i}_reg"] = reg
            f[f"src{i}_is_temp"] = is_temp
            f[f"src{i}_swizzle"] = 0
            f[f"src{i}_negate"] = False
        return f

    p = PixelProgram()
    p.input(0, 5)                                                 # r0 = v_texcoord (match VS)
    p.input(1, 10)                                                # r1 = v_color
    # accumulate in r2 — r0/r1 hold the VS-exported interpolators
    p.alu(**alu(_V.kMax, 2, [(1, False), (1, False)]))            # r2 = c1 (0)
    p.begin_loop(loop_id=0)
    p.alu(**alu(_V.kAdd, 2, [(2, True), (0, False)]))             # r2 += c0
    p.end_loop()
    p.alloc_colors()                                              # ALLOC pixel export
    p.alu(**alu(_V.kMax, 0, [(2, True), (2, True)], export=True))  # oC0 = r2
    consts = [ce.Constant(n, ce.RS_FLOAT4, r, 1, ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                          rows=1, columns=4) for n, r in (("c0", 0), ("c1", 1))]
    return p.finish(constants=consts)


def build_break_shader():
    """Pixel shader exercising both Phase C primitives in one loop:
      r2 = 0;
      for (aL = 0; aL < i0.x; aL++) {
          r2 += c[0 + aL];          # aL-relative constant addressing (C1)
          r3  = r2 - c2;
          setp_ge p0 <- r3.x;       # predicate set from a body value
      } break-if (p0 == true);      # data-dependent LOOP_END break (C2)
      oC0 = r2;
    """
    T = _templates()

    def vec(op, dest, srcs, export=False):
        f = dict(T["mov"], vector_opcode=op, vector_dest=dest, is_export=export,
                 vector_write_mask=0xF, vector_clamp=False, scalar_write_mask=0)
        for i, (reg, is_temp, neg, crel) in enumerate(srcs, start=1):
            f[f"src{i}_reg"] = reg
            f[f"src{i}_is_temp"] = is_temp
            f[f"src{i}_swizzle"] = 0
            f[f"src{i}_negate"] = neg
            f[f"src{i}_const_rel"] = crel
        return f

    p = PixelProgram()
    p.input(0, 5)
    p.input(1, 10)
    p.alu(**vec(_V.kMax, 2, [(1, False, False, False),
                             (1, False, False, False)]))            # r2 = c1 (0)
    p.begin_loop(loop_id=0)
    p.alu(**vec(_V.kAdd, 2, [(2, True, False, False),
                             (0, False, False, True)]))             # r2 += c[0+aL]
    p.alu(**vec(_V.kAdd, 3, [(2, True, False, False),
                             (2, False, True, False)]))             # r3 = r2 - c2
    p.alu(**dict(T["mov"], vector_opcode=_V.kMax, vector_dest=0,
                 vector_write_mask=0, vector_clamp=False,
                 scalar_opcode=_S.kSetpGe, scalar_dest=0, scalar_write_mask=0,
                 src3_reg=3, src3_is_temp=True, src3_swizzle=0x6c))  # setp_ge r3.x
    p.end_loop(predicated_break=True, break_condition=True)
    p.alloc_colors()
    p.alu(**vec(_V.kMax, 0, [(2, True, False, False),
                             (2, True, False, False)], export=True))  # oC0 = r2
    consts = [ce.Constant(n, ce.RS_FLOAT4, r, 1, ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                          rows=1, columns=4)
              for n, r in (("c0", 0), ("c1", 1), ("c2", 2))]
    return p.finish(constants=consts)


def cf_ops(blob):
    """(opcode, loop_id, address) per control-flow instruction, in order."""
    dwords = container.Container(blob).ucode_dwords
    out = []
    for i in range(len(dwords) // 3):
        for cf in xu.UnpackControlFlowInstructions(dwords[i * 3:i * 3 + 3]):
            op = cf.opcode()
            if op == xu.ControlFlowOpcode.kLoopStart:
                out.append(("loop_start", cf.loop_start.loop_id(),
                            cf.loop_start.address()))
            elif op == xu.ControlFlowOpcode.kLoopEnd:
                out.append(("loop_end", cf.loop_end.loop_id(),
                            cf.loop_end.address()))
            elif op == xu.ControlFlowOpcode.kAlloc:
                out.append(("alloc", None, None))
            elif xu.IsControlFlowOpcodeExec(op):
                out.append(("exec", None, cf.exec.address()))
    return out


def alu_records(blob):
    """Decoded AluInstruction objects in program order."""
    c = container.Container(blob)
    recs = xu._decode_program(c.ucode_dwords, c.shader_type)[0]
    return [xu.AluInstruction(*r.raw) for r in recs if r.kind == "alu"]


def loop_end_records(blob):
    """Decoded LOOP_END control-flow instructions in order."""
    dwords = container.Container(blob).ucode_dwords
    out = []
    for i in range(len(dwords) // 3):
        for cf in xu.UnpackControlFlowInstructions(dwords[i * 3:i * 3 + 3]):
            if cf.opcode() == xu.ControlFlowOpcode.kLoopEnd:
                out.append(cf.loop_end)
    return out


def exec_pred_cleans(blob):
    """is_predicate_clean for each non-empty exec, in order."""
    dwords = container.Container(blob).ucode_dwords
    out = []
    for i in range(len(dwords) // 3):
        for cf in xu.UnpackControlFlowInstructions(dwords[i * 3:i * 3 + 3]):
            if xu.IsControlFlowOpcodeExec(cf.opcode()) and cf.exec.count():
                out.append(cf.exec.is_predicate_clean())
    return out


def check_const_rel_encoding():
    """build_alu's srcN_const_rel must fold onto the correct hardware
    addressing bit (per src_const_is_addressed(i)) and stay aL-relative, NOT
    a0-relative. Validated through the independent decoder. Temp operands are
    never const-addressed regardless of the flag."""
    cases = []

    # C1's loop body shape: src1 temp (r2), src2 = aL-relative constant.
    # By the decoder rule, src1-temp routes operand 2 onto const_0.
    a = xu.AluInstruction(*enc.build_alu(
        vector_opcode=_V.kAdd, vector_dest=2, vector_write_mask=0xF,
        src1_reg=2, src1_is_temp=True,
        src2_reg=0, src2_is_temp=False, src2_const_rel=True))
    cases.append(("src2 const+rel (src1 temp)",
                  a.src_is_temp(1) and not a.src_is_temp(2)
                  and a.src_const_is_addressed(2)
                  and not a.is_const_address_register_relative()))

    # src1 = aL-relative constant -> operand 1 routes onto const_0.
    b = xu.AluInstruction(*enc.build_alu(
        vector_opcode=_V.kAdd, vector_dest=2, vector_write_mask=0xF,
        src1_reg=5, src1_is_temp=False, src1_const_rel=True,
        src2_reg=2, src2_is_temp=True))
    cases.append(("src1 const+rel",
                  not b.src_is_temp(1) and b.src_const_is_addressed(1)
                  and not b.is_const_address_register_relative()))

    # src3 = aL-relative constant with src1/src2 not both temp -> const_1.
    c = xu.AluInstruction(*enc.build_alu(
        vector_opcode=_V.kMad, vector_dest=2, vector_write_mask=0xF,
        src1_reg=0, src1_is_temp=False,
        src2_reg=2, src2_is_temp=True,
        src3_reg=1, src3_is_temp=False, src3_const_rel=True))
    cases.append(("src3 const+rel (not both temp -> const_1)",
                  not c.src_is_temp(3) and c.src_const_is_addressed(3)
                  and not c.is_const_address_register_relative()))

    # Baseline: no const_rel flag -> no addressing bits set anywhere. Guards
    # the byte-identity intent (existing shaders never set these).
    d = xu.AluInstruction(*enc.build_alu(
        vector_opcode=_V.kAdd, vector_dest=2, vector_write_mask=0xF,
        src1_reg=2, src1_is_temp=True, src2_reg=0, src2_is_temp=False))
    cases.append(("baseline: no addressing without the flag",
                  not any(d.src_const_is_addressed(i) for i in (1, 2, 3))
                  and not d.is_const_address_register_relative()))

    # Flag is ignored for temp operands (temps carry their own relative bit).
    e = xu.AluInstruction(*enc.build_alu(
        vector_opcode=_V.kAdd, vector_dest=2, vector_write_mask=0xF,
        src1_reg=2, src1_is_temp=True, src1_const_rel=True,
        src2_reg=3, src2_is_temp=True))
    cases.append(("flag ignored for temp operand",
                  not any(e.src_const_is_addressed(i) for i in (1, 2, 3))))

    ok = all(p for _, p in cases)
    if ok:
        print(f"  PASS encode srcN_const_rel -> aL-relative addressing "
              f"({len(cases)} cases)")
    else:
        for name, passed in cases:
            if not passed:
                print(f"  FAIL const_rel case: {name}")
    return ok


def main():
    ok = True
    blob = build_loop_shader()
    ops = cf_ops(blob)

    # CF must be: exec, loop_start(skip=Le+1), exec(body), loop_end(body=Ls+1), exec
    kinds = [o[0] for o in ops]
    try:
        ls = kinds.index("loop_start")
        le = kinds.index("loop_end")
    except ValueError:
        ls = le = -1
    structure_ok = (ls >= 0 and le == ls + 2 and kinds[ls + 1] == "exec"
                    and ops[ls][2] == le + 1        # skip past LOOP_END
                    and ops[le][2] == ls + 1)       # body = exec after LOOP_START
    if structure_ok:
        print(f"  PASS program loop CF: LOOP_START i{ops[ls][1]} skip->L{ops[ls][2]}"
              f", LOOP_END body->L{ops[le][2]} ({len(blob)} B)")
    else:
        ok = False
        print(f"  FAIL program loop CF structure: {ops}")

    hlsl = oracle_hlsl(blob, "program_loop")
    loop_ok = hlsl is not None and "for (aL = 0; aL < i0.x; aL++)" in hlsl
    if loop_ok:
        print("  PASS oracle: decompiles to for(aL=0; aL<i0.x; aL++) r0+=c0")
    else:
        ok = False
        print(f"  FAIL oracle loop: {'rejected' if hlsl is None else 'no for(aL) loop'}")

    # default end_loop() must leave LOOP_END unpredicated (byte-identity guard
    # for the existing counted-loop shaders).
    plain_le = loop_end_records(blob)
    plain_ok = (len(plain_le) == 1 and not plain_le[0].is_predicated_break()
                and not plain_le[0].condition())
    if plain_ok:
        print("  PASS counted loop LOOP_END stays unpredicated (brk=0, cond=0)")
    else:
        ok = False
        print(f"  FAIL counted LOOP_END predication: {plain_le}")

    # Phase C primitives.
    ok &= check_const_rel_encoding()

    bblob = build_break_shader()
    # C1 in the body: the accumulate instruction reads an aL-relative constant.
    body_alus = alu_records(bblob)
    accum = next((a for a in body_alus
                  if a.vector_opcode() == _V.kAdd and not a.src_is_temp(2)
                  and a.src_const_is_addressed(2)), None)
    c1_ok = (accum is not None
             and not accum.is_const_address_register_relative())
    if c1_ok:
        print("  PASS break shader body accumulates c[0+aL] (aL-relative)")
    else:
        ok = False
        print("  FAIL break shader: no aL-relative accumulate found")

    # C2: a setp_* sets the predicate and LOOP_END breaks on it.
    has_setp = any(a.scalar_opcode() == _S.kSetpGe for a in body_alus)
    ble = loop_end_records(bblob)
    c2_ok = (has_setp and len(ble) == 1 and ble[0].is_predicated_break()
             and ble[0].condition())
    if c2_ok:
        print("  PASS break shader: setp_ge + predicated LOOP_END (brk=1, "
              "cond=1)")
    else:
        ok = False
        print(f"  FAIL break shader predication: setp={has_setp}, end={ble}")

    # The setp-bearing body exec must NOT be predicate-clean, or the sequencer
    # won't wait for p0 before LOOP_END reads it (the loop would never break).
    bclean = exec_pred_cleans(bblob)
    pc_ok = bclean.count(False) == 1
    if pc_ok:
        print("  PASS predicate-setting exec is not predicate-clean")
    else:
        ok = False
        print(f"  FAIL predicate-clean flags wrong: {bclean}")

    # The plain counted loop has no setp, so every exec stays predicate-clean
    # (guards byte-identity of the existing loop shaders).
    if all(exec_pred_cleans(blob)):
        print("  PASS counted loop execs all stay predicate-clean")
    else:
        ok = False
        print(f"  FAIL counted loop pred-clean changed: {exec_pred_cleans(blob)}")

    # The break shader must also survive the decode oracle.
    bhlsl = oracle_hlsl(bblob, "program_break")
    if bhlsl is not None:
        print(f"  PASS oracle accepts the predicated-break loop "
              f"({len(bblob)} B)")
    else:
        ok = False
        print("  FAIL oracle: XenosRecomp rejected the predicated-break loop")

    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
