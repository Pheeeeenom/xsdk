#!/usr/bin/env python3
"""Exit gate for the xsc GLSL->Xenos pipeline (Phase 3a).

Compiles the two canonical test shaders from SPIR-V through xsc.lower and
checks them against the hardware-proven hand-built equivalents:
  - modulate.frag must be BYTE-IDENTICAL to the modulate combiner
    (the demo bank's first mode, tests/test_xcomb_parity.py) — already
    photographed on console.
  - spincube.vert must be oracle-equivalent to xost_vs2, the hand-built
    shader in xost's archived examples (XenosRecomp accepts it and emits the
    mvp dp4 transform + passthrough interpolators).
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xsc.spirv as spirv  # noqa: E402
from xasm.combiner import CombinerCycle, Generator, Src  # noqa: E402
from xsc import templates  # noqa: E402
from xsc.lower import lower  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402

TD = ROOT / "xsc" / "testdata"
RECOMP = (ROOT / "reference" / "XenosRecomp" / "build" / "XenosRecomp" /
          "XenosRecomp.exe")
SHADER_COMMON = (ROOT / "reference" / "XenosRecomp" / "XenosRecomp" /
                 "shader_common.h")


def found(needle, text):
    """Whether text has needle, any register standing for each rN in it: the allocator numbers a program's registers
    as it packs them, so a check names the instructions, not their registers."""
    pattern = re.sub(r"\br\d+\b", lambda _: r"r\d+", re.escape(needle))
    return re.search(pattern, text) is not None


def compile_glsl(name):
    return lower(spirv.Module.parse((TD / f"{name}.spv").read_bytes()))


def oracle_hlsl(blob, tag):
    binp = TD / f"_{tag}.bin"
    hlsl = TD / f"_{tag}.hlsl"
    binp.write_bytes(blob)
    r = subprocess.run([str(RECOMP), str(binp), str(hlsl),
                        str(SHADER_COMMON)], capture_output=True)
    if r.returncode != 0:
        return None
    return hlsl.read_text()


def cf_ops(blob):
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


def shader_records(blob):
    c = container.Container(blob)
    return xu._decode_program(c.ucode_dwords, c.shader_type)[0]


def alu_swz(idxs):
    return sum(((idxs[p] - p) & 3) << (2 * p) for p in range(len(idxs)))


def fetch_swz(idxs):
    return sum(idxs[p] << (2 * p) for p in range(len(idxs)))


def vec_mat(v, m):
    return [sum(v[i] * m[i][j] for i in range(4)) for j in range(4)]


def mat_vec(m, v):
    return [sum(m[i][j] * v[j] for j in range(4)) for i in range(4)]


def mat_mat(a, b):
    return [vec_mat(row, b) for row in a]


def close4(a, b, eps=1e-5):
    return all(abs(a[i] - b[i]) <= eps for i in range(4))


def swizzled(src, swz):
    return [src[xu.AluInstruction.GetSwizzledComponentIndex(swz, i)]
            for i in range(4)]


def alu_src(op, n, regs, consts):
    reg = op.src_reg(n)
    if op.src_is_temp(n):
        val = list(regs.get(reg & 0x3F, (0.0, 0.0, 0.0, 0.0)))
        if reg & 0x80:
            val = [abs(x) for x in val]
    else:
        val = list(consts[reg])
    val = swizzled(val, op.src_swizzle(n))
    return [-x for x in val] if op.src_negate(n) else val


def write_masked(dst, src, mask):
    for i in range(4):
        if mask & (1 << i):
            dst[i] = src[i]


def eval_vertex_position(blob, regs, consts):
    regs = {k: list(v) for k, v in regs.items()}
    consts = {k: list(v) for k, v in consts.items()}
    opos = [0.0, 0.0, 0.0, 0.0]
    for rec in shader_records(blob):
        if rec.kind != "alu":
            continue
        op = xu.AluInstruction(*rec.raw)
        opcode = op.vector_opcode()
        mask = op.GetVectorOpResultWriteMask()
        if not mask:
            continue
        s1 = alu_src(op, 1, regs, consts)
        s2 = alu_src(op, 2, regs, consts)
        if opcode == xu.AluVectorOpcode.kMul:
            res = [s1[i] * s2[i] for i in range(4)]
        elif opcode == xu.AluVectorOpcode.kMad:
            s3 = alu_src(op, 3, regs, consts)
            res = [s1[i] * s2[i] + s3[i] for i in range(4)]
        elif opcode == xu.AluVectorOpcode.kAdd:
            res = [s1[i] + s2[i] for i in range(4)]
        elif opcode == xu.AluVectorOpcode.kMax:
            res = [max(s1[i], s2[i]) for i in range(4)]
        elif opcode == xu.AluVectorOpcode.kDp4:
            res = [sum(s1[i] * s2[i] for i in range(4))] * 4
        else:
            continue
        if op.is_export() and \
                op.vector_dest() == int(xu.ExportRegister.kVSPosition):
            write_masked(opos, res, mask)
        elif not op.is_export():
            dst = regs.setdefault(op.vector_dest(), [0.0, 0.0, 0.0, 0.0])
            write_masked(dst, res, mask)
    return opos


def main():
    ok = True

    # fragment: byte-identical to the photographed combiner
    frag = compile_glsl("modulate.frag")
    gold = Generator(alu_template=templates.mov(), tfetch_template=templates.texture_fetch()).generate(
        CombinerCycle(Src.TEXEL0, Src.ZERO, Src.SHADE, Src.ZERO))
    if frag == gold:
        print(f"  PASS modulate.frag byte-identical to combiner ({len(frag)} B)")
    else:
        ok = False
        print(f"  FAIL modulate.frag: {len(frag)} B vs combiner {len(gold)} B")

    # novel shader (never hand-built): mix() must decompose to the lerp
    # identity x + (y-x)*a and oracle-validate. The proof xsc *compiles*.
    blend = compile_glsl("blend.frag")
    bhlsl = oracle_hlsl(blend, "blend_frag")
    if bhlsl is None:
        ok = False
        print("  FAIL blend.frag: XenosRecomp rejected it")
    else:
        need = ["r1.xyzw + -r2.xyzw", "* u_blend.xyzw + r2.xyzw"]
        miss = [s for s in need if not found(s, bhlsl)]
        if miss:
            ok = False
            print(f"  FAIL blend.frag mix() decomposition wrong: missing "
                  f"{miss}")
        else:
            print(f"  PASS blend.frag: novel mix() -> correct lerp "
                  f"(sub+mad), oracle-validated ({len(blend)} B)")

    # swizzle / CompositeConstruct: vec4(tex.rgb * color.rgb, tex.a)
    tint = compile_glsl("tint.frag")
    thlsl = oracle_hlsl(tint, "tint_frag")
    if thlsl is None:
        ok = False
        print("  FAIL tint.frag: XenosRecomp rejected it")
    else:
        need = ["oC0.xyz = max(r3.xyz", "r2.xyzw * r1.xyzw",
                "oC0.w = max(r2.w"]
        miss = [s for s in need if not found(s, thlsl)]
        if miss:
            ok = False
            print(f"  FAIL tint.frag swizzle/construct wrong: missing {miss}")
        else:
            print(f"  PASS tint.frag: .rgb swizzle + vec4(rgb,a) construct "
                  f"-> masked exports ({len(tint)} B)")

    # literal constant: mix(tex, color, 0.5) -> 0.5 in a pooled constant reg
    from xsc.lower import lower_ex
    bl50, manifest = lower_ex(spirv.Module.parse(
        (TD / "blend50.frag.spv").read_bytes()))
    b50hl = oracle_hlsl(bl50, "blend50_frag")
    want_man = [(0, (0.5, 0.5, 0.5, 0.5))]
    if b50hl is None or not found("_lit0.xyzw + r2.xyzw", b50hl) or \
            manifest != want_man:
        ok = False
        print(f"  FAIL blend50.frag: literal pool wrong (manifest {manifest})")
    else:
        print(f"  PASS blend50.frag: literal 0.5 -> pooled const c0, "
              f"manifest {manifest} ({len(bl50)} B)")

    # 0/1 literals: identity operands fold away; export lanes use Xenos
    # constant-result masks instead of pooled constants.
    lit01, l01man = lower_ex(spirv.Module.parse(
        (TD / "lit01.frag.spv").read_bytes()))
    l01hl = oracle_hlsl(lit01, "lit01_frag")
    need = ["oC0.z = 0.0", "oC0.w = 1.0",
            "oC0.x = max(", "oC0.y = max("]
    miss = [s for s in need if not l01hl or not found(s, l01hl)]
    has_lit = l01hl and "_lit" in l01hl
    if l01hl is None or miss or l01man != [] or has_lit:
        ok = False
        print(f"  FAIL lit01.frag: 0/1 literal fold wrong "
              f"(missing {miss}, manifest {l01man}, "
              f"has_lit={bool(has_lit)})")
    else:
        print(f"  PASS lit01.frag: 0/1 literals -> identity folds + "
              f"export constants, empty manifest ({len(lit01)} B)")

    # scalar pipe: normalize + length (rsq/sqrt), and the full mathfns
    # stress shader (normalize+length+pow+floor) must compile + oracle-pass
    for name, needle, desc in [
        ("normlen", "rsqrt(r3.w)", "normalize (dp3+rsq) + length (dp3+sqrt)"),
        ("mathfns", "exp2(", "normalize+length+pow(log2/exp2)+floor stress"),
        ("explog", "sin(r1.y)", "exp/log natural + reflect(mad) + sin/cos"),
    ]:
        blob = compile_glsl(f"{name}.frag")
        h = oracle_hlsl(blob, f"{name}_frag")
        if h is None or not found(needle, h):
            ok = False
            print(f"  FAIL {name}.frag: {desc}")
        else:
            print(f"  PASS {name}.frag: {desc} ({len(blob)} B)")

    # control flow: if/else select -> branch-free cnd op (no jumps)
    cfif = compile_glsl("cf_if.frag")
    chl = oracle_hlsl(cfif, "cf_if_frag")
    if chl is None or not found("select(r3.xyzw > 0.0, r1.xyzw, r2.xyzw)", chl):
        ok = False
        print("  FAIL cf_if.frag: if/else cnd select wrong")
    else:
        print(f"  PASS cf_if.frag: if(a>b) A else B -> cndgt select, "
              f"branch-free ({len(cfif)} B)")

    # nested if/else -> chained cnd selects (inner select feeds the outer)
    nif = compile_glsl("nested_if.frag")
    nhl = oracle_hlsl(nif, "nested_if_frag")
    if nhl is None or not found("r4.xyzw = select(r5", nhl) or \
            not found("oC0.xyzw = select(r3", nhl):
        ok = False
        print("  FAIL nested_if.frag: nested if/else chained-select wrong")
    else:
        print(f"  PASS nested_if.frag: nested if/else -> chained cnd selects, "
              f"branch-free ({len(nif)} B)")

    # if/else assigning a local -> cnd select feeds post-merge output math
    fbl = compile_glsl("fs_branch_local.frag")
    fblh = oracle_hlsl(fbl, "fs_branch_local_frag")
    need = ["r3.xyzw = select(r4.xyzw > 0.0, r2.xyzw, r1.xyzw)",
            "oC0.xyzw = r3.xyzw * r1.xyzw"]
    miss = [s for s in need if not fblh or not found(s, fblh)]
    if fblh is None or miss:
        ok = False
        print(f"  FAIL fs_branch_local.frag: local select merge wrong "
              f"(missing {miss})")
    else:
        print(f"  PASS fs_branch_local.frag: if/else local -> select feeds "
              f"post-merge math ({len(fbl)} B)")

    # discard: if(v_uv.x < 0.5) discard -> kill_gt(0.5, uv.x)
    fdk, dkman = lower_ex(spirv.Module.parse(
        (TD / "fs_discard.frag.spv").read_bytes()))
    fdkh = oracle_hlsl(fdk, "fs_discard_frag")
    need = ["clip(any(_lit0.x > r0.x) ? -1 : 1)",
            "oC0.xyzw = r2.xyzw * r1.xyzw"]
    want_man = [(0, (0.5, 0.5, 0.5, 0.5))]
    miss = [s for s in need if not fdkh or not found(s, fdkh)]
    if fdkh is None or miss or dkman != want_man:
        ok = False
        print(f"  FAIL fs_discard.frag: discard kill wrong "
              f"(missing {miss}, manifest {dkman})")
    else:
        print(f"  PASS fs_discard.frag: if(uv.x<0.5) discard -> "
              f"kill_gt literal-vs-uv ({len(fdk)} B)")

    # cubemap: samplerCube must emit the hardware cube setup sequence, not
    # merely a decoder-accepted tfetchCube.
    fcube = compile_glsl("fs_cubemap.frag")
    fchl = oracle_hlsl(fcube, "fs_cubemap_frag")
    frec = shader_records(fcube)
    cube_ops = [xu.AluInstruction(*r.raw) for r in frec
                if r.kind == "alu" and
                xu.AluInstruction(*r.raw).vector_opcode() ==
                xu.AluVectorOpcode.kCube]
    rcp_ops = [xu.AluInstruction(*r.raw) for r in frec
               if r.kind == "alu" and
               xu.AluInstruction(*r.raw).scalar_opcode() ==
               xu.AluScalarOpcode.kRcp]
    tf_ops = [xu.TextureFetchInstruction(*r.raw) for r in frec
              if r.kind == "tfetch"]
    raw_ok = (len(cube_ops) == 1 and
              cube_ops[0].src_swizzle(1) == alu_swz([2, 2, 0, 1]) and
              cube_ops[0].src_swizzle(2) == alu_swz([1, 0, 2, 2]) and
              len(rcp_ops) == 1 and rcp_ops[0].scalar_write_mask() == 0x4 and
              rcp_ops[0].src_reg(3) & 0x80 and
              len(tf_ops) == 1 and
              tf_ops[0].dimension() == xu.FetchOpDimension.kCube and
              tf_ops[0].src_swizzle() == fetch_swz([1, 0, 3]))
    need = ["r2.xyzw = cube(r0, cubeMapData)",
            "rcp(abs(r2.z))",
            "r2.xy = r2.xy * r2.zz + _lit0.xy",
            "tfetchCube(tex0_TextureCubeDescriptorIndex",
            "r2.yxw, cubeMapData).xyzw",
            "oC0.xyzw = max(r1.xyzw, r1.xyzw)"]
    want_man = [(0, (1.5, 1.5, 1.5, 1.5))]
    miss = [s for s in need if not fchl or not found(s, fchl)]
    body_has_2d = fchl and \
        "tfetch2D(tex0_Texture2DDescriptorIndex" in fchl
    _fcube, fcman = lower_ex(spirv.Module.parse(
        (TD / "fs_cubemap.frag.spv").read_bytes()))
    if fchl is None or miss or body_has_2d or not raw_ok or fcman != want_man:
        ok = False
        print(f"  FAIL fs_cubemap.frag: cubemap fetch wrong "
              f"(missing {miss}, body_has_2d={bool(body_has_2d)}, "
              f"raw_ok={raw_ok}, manifest {fcman})")
    else:
        print(f"  PASS fs_cubemap.frag: samplerCube -> cube/rcp/mad + "
              f"tfetchCube .yxw ({len(fcube)} B)")

    # non-identity swizzle (.bgra) + arbitrary clamp + floor/abs
    for name, needle, desc in [
        ("sw_swizzle", "max(r2.zyxw", "non-identity swizzle .bgra"),
        ("clamprange", "min(r4.xyzw, _lit1.xyzw)",
         "clamp(x,0.2,0.8) -> max/min + pooled literals"),
        ("floorabs", "floor(r1.xyzw)", "floor() + abs() as max(x,-x)"),
    ]:
        blob = compile_glsl(f"{name}.frag")
        h = oracle_hlsl(blob, f"{name}_frag")
        if h is None or not found(needle, h):
            ok = False
            print(f"  FAIL {name}.frag: {desc}")
        else:
            print(f"  PASS {name}.frag: {desc} ({len(blob)} B)")

    # phase-3b breadth: float division, sign(), tan()
    for name, needles, desc in [
        ("divide", ["rcp(u_denom.x)", "rcp(u_denom.w)"],
         "vec/vec division -> per-component rcp + mul"),
        ("absign", ["max(u_offset.xyzw, -u_offset.xyzw)",
                     "r6.xyzw > -r6.xyzw", "-r6.xyzw > r6.xyzw"],
         "abs()=max(x,-x), sign()=sgt(x,-x)-sgt(-x,x)"),
        ("tangent", ["sin(u_phase.x)", "cos(u_phase.x)"],
         "tan(x)=sin(x)*rcp(cos(x))"),
    ]:
        blob = compile_glsl(f"{name}.frag")
        h = oracle_hlsl(blob, f"{name}_frag")
        miss = [s for s in needles if not h or not found(s, h)]
        if h is None or miss:
            ok = False
            print(f"  FAIL {name}.frag: {desc} (missing {miss})")
        else:
            print(f"  PASS {name}.frag: {desc} ({len(blob)} B)")

    # phase-3b breadth II: rounding family, angles, step/smoothstep, mod, cross,
    # two-source vector assembly
    for name, needles, desc in [
        ("rounding", ["frac(", "trunc(", "floor(-"],
         "fract/trunc/ceil=-floor(-x)/round=floor(x+.5)"),
        ("angles", ["* _lit0.xyzw", "* _lit1.xyzw"],
         "radians/degrees -> mul by pooled const"),
        ("stepsmooth", [">= _lit", "saturate("],
         "step (kSge) + smoothstep (clamp + hermite t*t*(3-2t))"),
        ("modcross", [".zxyw *", "-u_period.xyzw *"],
         "cross (yzx/zxy mad) + mod (x - y*floor(x/y))"),
        ("shuffle2", ["oC0.xy = max(r2", "oC0.z = max(r3"],
         "two-source vec assembly (t.xy + color.zw)"),
        ("invtrig", ["sqrt(", "> -"],
         "asin (A-S sqrt form) / acos=pi/2-asin / atan (odd poly)"),
    ]:
        blob = compile_glsl(f"{name}.frag")
        h = oracle_hlsl(blob, f"{name}_frag")
        miss = [s for s in needles if not h or not found(s, h)]
        if h is None or miss:
            ok = False
            print(f"  FAIL {name}.frag: {desc} (missing {miss})")
        else:
            print(f"  PASS {name}.frag: {desc} ({len(blob)} B)")

    # loops: constant-trip for/while unrolled to branch-free SSA (xsc.unroll),
    # then lowered + oracle-validated. Assert the loop was eliminated and the
    # body replicated the right number of times (the induction var is folded).
    from xsc.unroll import unroll as _unroll
    for name, want_adds, desc in [
        ("loop_accum", 4, "for i<4: s+=uv.x -> 4x unrolled accumulate"),
        ("loop_weight", 4, "for i<4: s+=uv.x*float(i) -> induction folded"),
        ("loop_while", 4, "while i<4 (increment in body) -> 4x unroll"),
        ("loop_nested", 9, "nested for 3x3 -> 9x (innermost-first)"),
        ("loop_stair", 5, "5x step() staircase + post-loop *0.2 (mem2reg RMW)"),
    ]:
        mu = spirv.Module.parse((TD / f"{name}.frag.spv").read_bytes())
        _unroll(mu)
        n_loop = sum(1 for i in mu.instrs if i.opcode == 246)  # OpLoopMerge
        n_add = sum(1 for i in mu.instrs if i.opcode == 129)   # OpFAdd
        blob = compile_glsl(f"{name}.frag")
        h = oracle_hlsl(blob, f"{name}_frag")
        if h is None or n_loop != 0 or n_add != want_adds:
            ok = False
            print(f"  FAIL {name}.frag: {desc} (loops={n_loop} adds={n_add} "
                  f"want {want_adds}, oracle={'ok' if h else 'REJECT'})")
        else:
            print(f"  PASS {name}.frag: {desc} ({len(blob)} B)")

    # uniform-bounded loop: falls back to real LOOP_START/END, not unrolling.
    lmu = spirv.Module.parse((TD / "loop_uniform.frag.spv").read_bytes())
    lblob, lman = lower_ex(lmu)
    lhlsl = oracle_hlsl(lblob, "loop_uniform_frag")
    ops = cf_ops(lblob)
    kinds = [o[0] for o in ops]
    try:
        ls, le = kinds.index("loop_start"), kinds.index("loop_end")
    except ValueError:
        ls = le = -1
    cf_ok = (ls >= 0 and le == ls + 2 and kinds[ls + 1] == "exec"
             and ops[ls][1] == 0 and ops[ls][2] == le + 1
             and ops[le][2] == ls + 1)
    want_step = (0.11999999731779099,) * 4
    want_man = [(0, want_step), ("loop", 0, "u_count")]
    # accumulator in r2 (r0/r1 are interpolator-interface padding so the PS
    # parameter-cache count matches the 2-export VS — see lower_fragment_real_loop)
    need = ["for (aL = 0; aL < i0.x; aL++)",
            "r2.xyzw = _lit0.xyzw + -_lit0.xyzw",
            "r2.xyzw = r2.xyzw + _lit0.xyzw",
            "oC0.xyzw = max(r2.xyzw, r2.xyzw)"]
    miss = [s for s in need if not lhlsl or not found(s, lhlsl)]
    if lhlsl is None or miss or lman != want_man or not cf_ok:
        ok = False
        print(f"  FAIL loop_uniform.frag: real loop wrong "
              f"(missing {miss}, manifest {lman}, cf={ops})")
    else:
        print(f"  PASS loop_uniform.frag: uniform bound -> real "
              f"LOOP_START/END ({len(lblob)} B)")

    # loop frontend: a uniform array indexed by the loop counter compiles to a
    # real loop whose body accumulates the aL-relative constant c[base+aL].
    la = spirv.Module.parse((TD / "loop_array.frag.spv").read_bytes())
    lablob, laman = lower_ex(la)
    lah = oracle_hlsl(lablob, "loop_array_frag")
    ladis = xu.disassemble(container.Container(lablob).ucode_dwords, "pixel")
    la_cf = [o[0] for o in cf_ops(lablob)]
    want_laman = [("array", 1, 4, "u_w"), ("loop", 0, "u_count")]
    if lah is None or not found("+aL]", ladis) or laman != want_laman or \
            "loop_start" not in la_cf:
        ok = False
        print(f"  FAIL loop_array.frag: aL-relative array loop wrong "
              f"(manifest {laman}, has_aL={'+aL]' in ladis})")
    else:
        print(f"  PASS loop_array.frag: for(i<n) sum += w[i] -> real loop "
              f"with c[base+aL] ({len(lablob)} B)")

    # while-break frontend: `for(i<n){ sum+=k; if(sum.x>thr) break; }` lowers
    # to a body setp + a predicated LOOP_END (the G30 primitive, from GLSL).
    lb = spirv.Module.parse((TD / "loop_break.frag.spv").read_bytes())
    lbblob, lbman = lower_ex(lb)
    lbh = oracle_hlsl(lbblob, "loop_break_frag")
    lbdis = xu.disassemble(container.Container(lbblob).ucode_dwords, "pixel")
    if lbh is None or not found("setp_gt", lbdis) or not found("(p0) endloop", lbdis):
        ok = False
        print(f"  FAIL loop_break.frag: predicated while-break wrong "
              f"(oracle={'OK' if lbh else 'REJECTED'})")
    else:
        print(f"  PASS loop_break.frag: if(cond) break -> setp + predicated "
              f"LOOP_END ({len(lbblob)} B)")

    # if/else inside a loop body with a constant (induction-var) condition:
    # the unroller picks the taken arm per iteration (4x .05 then 4x .10).
    lc = spirv.Module.parse((TD / "loop_cond.frag.spv").read_bytes())
    lcblob, lcman = lower_ex(lc)
    lch = oracle_hlsl(lcblob, "loop_cond_frag")
    want_lc = [(0, (0.05000000074505806,) * 4),
               (1, (0.10000000149011612,) * 4)]
    if lch is None or lcman != want_lc:
        ok = False
        print(f"  FAIL loop_cond.frag: if/else-in-loop wrong "
              f"(manifest {lcman})")
    else:
        print(f"  PASS loop_cond.frag: if(i<k) in loop -> per-iteration arm "
              f"select ({len(lcblob)} B)")

    # dot(): luminance desaturation (vec3 dot -> dp3, broadcast scalar)
    gray = compile_glsl("gray.frag")
    ghl = oracle_hlsl(gray, "gray_frag")
    if ghl is None or not found("dot(r2.xyz, _lit0.xyz)", ghl) or \
            not found("oC0.xyz = max(r3.xyz", ghl):
        ok = False
        print("  FAIL gray.frag: dot()/dp3 desaturation wrong")
    else:
        print(f"  PASS gray.frag: dot(rgb, luma) -> dp3 broadcast, "
              f"vec4(lum,lum,lum,a) ({len(gray)} B)")

    # clamp(x,0,1) -> free saturate modifier (no extra instruction)
    sat = compile_glsl("sat.frag")
    shl = oracle_hlsl(sat, "sat_frag")
    if shl is None or not found("saturate(r2.xyzw + r1.xyzw)", shl):
        ok = False
        print("  FAIL sat.frag: clamp->saturate wrong")
    else:
        print(f"  PASS sat.frag: clamp(x,0,1) -> add_sat fused, free "
              f"({len(sat)} B)")

    # VectorTimesScalar: modulate by a scalar uniform (.xxxx broadcast)
    bright = compile_glsl("bright.frag")
    bhl = oracle_hlsl(bright, "bright_frag")
    if bhl is None or not found("u_brightness.xxxx", bhl):
        ok = False
        print("  FAIL bright.frag: VectorTimesScalar broadcast wrong")
    else:
        print(f"  PASS bright.frag: vec * scalar uniform (.xxxx) "
              f"({len(bright)} B)")

    # integer scalar arithmetic: fold int locals through float() into literals.
    im = spirv.Module.parse((TD / "intmix.frag.spv").read_bytes())
    imix, imman = lower_ex(im)
    imhl = oracle_hlsl(imix, "intmix_frag")
    have_ops = {i.opcode for i in im.instrs}
    want_ops = {111, 128, 132}     # ConvertSToF, IAdd, IMul
    want_man = [(0, (0.875, 0.875, 0.875, 0.875))]
    need = ["oC0.xyzw = r0.xyzw * _lit0.xxxx"]
    miss = [s for s in need if not imhl or not found(s, imhl)]
    if imhl is None or miss or imman != want_man or not want_ops <= have_ops:
        ok = False
        print(f"  FAIL intmix.frag: int->float fold wrong "
              f"(missing {miss}, manifest {imman})")
    else:
        print(f"  PASS intmix.frag: int arithmetic -> float literal "
              f"manifest ({len(imix)} B)")

    # integer scalar arithmetic: float-backed int(float_expr) + const.
    ib = spirv.Module.parse((TD / "intbucket.frag.spv").read_bytes())
    iblob, ibman = lower_ex(ib)
    ibhl = oracle_hlsl(iblob, "intbucket_frag")
    have_ops = {i.opcode for i in ib.instrs}
    want_ops = {110, 111, 128}    # ConvertFToS, ConvertSToF, IAdd
    want_man = [(0, (4.0, 4.0, 4.0, 4.0)),
                (1, (1.0, 1.0, 1.0, 1.0)),
                (2, (0.25, 0.25, 0.25, 0.25))]
    need = ["r1.xyzw = max(r0.xxxx, r0.xxxx)",
            "r3.xyzw = trunc(r2.xyzw)",
            "r4.xyzw = r3.xyzw + _lit1.xyzw",
            "r5.xyzw = r4.xyzw * _lit2.xyzw",
            "oC0.w = 1.0", "oC0.xyz = max(r5.xyz, r5.xyz)"]
    miss = [s for s in need if not ibhl or not found(s, ibhl)]
    if ibhl is None or miss or ibman != want_man or not want_ops <= have_ops:
        ok = False
        print(f"  FAIL intbucket.frag: float-backed int wrong "
              f"(missing {miss}, manifest {ibman})")
    else:
        print(f"  PASS intbucket.frag: ConvertFToS/IAdd/ConvertSToF "
              f"float-backed path ({len(iblob)} B)")

    # Batch A frontend ops: each must oracle-decode AND emit the expected
    # Xenos ALU mnemonics (the disassembly is the direct check on what we
    # emit; the oracle confirms it decodes as a valid program).
    for name, needles, desc in [
        ("fs_ternary", ["sgt", "mad"],
         "ternary ?: -> f + (t-f)*cond select"),
        ("fs_vrelation", ["sgt", "min", "mad"],
         "greaterThan + any(max)/all(min)/not + select"),
        ("fs_refract", ["dp3", "sqrt"],
         "refract(): Snell vector with total-internal-reflection mask"),
        ("fs_intdiv", ["rcp", "trunc"],
         "runtime int / and % via rcp+trunc emulation"),
    ]:
        blob = compile_glsl(f"{name}.frag")
        h = oracle_hlsl(blob, name)
        dis = xu.disassemble(container.Container(blob).ucode_dwords, "pixel")
        miss = [s for s in needles if not found(s, dis)]
        if h is None or miss:
            ok = False
            print(f"  FAIL {name}.frag: {desc} "
                  f"(oracle={'OK' if h else 'REJECTED'}, missing {miss})")
        else:
            print(f"  PASS {name}.frag: {desc} ({len(blob)} B)")

    # Batch C texture sampling: explicit LOD -> tfetch LOD bias, projective
    # divide, and a computed (non-interpolator) coordinate.
    for name, needles, desc in [
        ("fs_texlod", ["LODBias"], "textureLod explicit LOD -> tfetch LOD bias"),
        ("fs_texproj", ["rcp", "tfetch2D"], "textureProj perspective divide"),
        ("fs_texscale", ["tfetch2D"], "texture2D with a computed coordinate"),
    ]:
        blob = compile_glsl(f"{name}.frag")
        h = oracle_hlsl(blob, name)
        dis = xu.disassemble(container.Container(blob).ucode_dwords, "pixel")
        miss = [s for s in needles if not found(s, dis)]
        if h is None or miss:
            ok = False
            print(f"  FAIL {name}.frag: {desc} "
                  f"(oracle={'OK' if h else 'REJECTED'}, missing {miss})")
        else:
            print(f"  PASS {name}.frag: {desc} ({len(blob)} B)")

    # screen-space derivatives via the getTextureGradients fetch -- hardware-
    # verified (DERIV visualizes them; DSOLID proved the fetch is pipeline-safe).
    for name in ("fs_deriv", "fs_dsolid"):
        blob = compile_glsl(f"{name}.frag")
        h = oracle_hlsl(blob, name)
        dis = xu.disassemble(container.Container(blob).ucode_dwords, "pixel")
        if h is None or not found("getGradients", dis):
            ok = False
            print(f"  FAIL {name}.frag: derivatives "
                  f"(oracle={'OK' if h else 'REJECTED'})")
        else:
            print(f"  PASS {name}.frag: getGradients fetch ({len(blob)} B)")

    # vertex: oracle-equivalent to the mvp transform
    vert = compile_glsl("spincube.vert")
    hlsl = oracle_hlsl(vert, "spincube_vert")
    if hlsl is None:
        ok = False
        print("  FAIL spincube.vert: XenosRecomp rejected it")
    else:
        need = ["dot(r0.xyzw, u_mvp(0)", "dot(r0.xyzw, u_mvp(3)",
                "oTexCoord0", "oColor0"]
        miss = [s for s in need if not found(s, hlsl)]
        if miss:
            ok = False
            print(f"  FAIL spincube.vert HLSL missing {miss}")
        else:
            print(f"  PASS spincube.vert oracle = mvp dp4 transform + "
                  f"passthrough ({len(vert)} B)")

    # vertex: chained matrix transforms (computed vector feeds mat4*vec4)
    chain = compile_glsl("chainmat.vert")
    chlsl = oracle_hlsl(chain, "chainmat_vert")
    if chlsl is None:
        ok = False
        print("  FAIL chainmat.vert: XenosRecomp rejected it")
    else:
        need = ["dot(r0.xyzw, u_model(0)", "r3.w = dot(r0.xyzw, u_model(3)",
                "dot(r3.xyzw, u_viewproj(0)",
                "dot(r3.xyzw, u_viewproj(3)", "oTexCoord0", "oColor0"]
        miss = [s for s in need if not found(s, chlsl)]
        if miss:
            ok = False
            print(f"  FAIL chainmat.vert HLSL missing {miss}")
        else:
            print(f"  PASS chainmat.vert: u_viewproj * (u_model * pos) "
                  f"-> temp dp4 chain + passthrough ({len(chain)} B)")

    # mat3 / mat2 (normal matrices, UV rotations): the matrix multiply uses a
    # dp3 / dp2add per row while the mat4 position transform stays dp4.
    for name, needle, desc in [
        ("vs_mat3", "dp3", "mat3 normal matrix (dp3 per row) + mat4 MVP"),
        ("vs_mat2", "dp2", "mat2 UV rotation (dp2add per row) + mat4 MVP"),
    ]:
        blob = compile_glsl(f"{name}.vert")
        h = oracle_hlsl(blob, name)
        dis = xu.disassemble(container.Container(blob).ucode_dwords, "vertex")
        if h is None or not found(needle, dis) or not found("dp4 oPos", dis):
            ok = False
            print(f"  FAIL {name}.vert: {desc} (oracle="
                  f"{'OK' if h else 'REJECTED'}, {needle}={needle in dis})")
        else:
            print(f"  PASS {name}.vert: {desc} ({len(blob)} B)")

    # matrix builtins (transpose / determinant / inverse): numerically gated by
    # tests/test_matrix.py; here confirm the oracle decodes them + key ops.
    for name, needle, desc in [
        ("fs_mtest", "dp3", "transpose + determinant(mat3)"),
        ("fs_minv", "rcp", "inverse(mat3) via adjugate/det"),
    ]:
        blob = compile_glsl(f"{name}.frag")
        h = oracle_hlsl(blob, name)
        dis = xu.disassemble(container.Container(blob).ucode_dwords, "pixel")
        if h is None or not found(needle, dis):
            ok = False
            print(f"  FAIL {name}.frag: {desc} "
                  f"(oracle={'OK' if h else 'REJECTED'})")
        else:
            print(f"  PASS {name}.frag: {desc} ({len(blob)} B)")

    # vertex: VectorTimesMatrix (vec4 * mat4) lowers as a row-vector
    # accumulation, and the emitted ALU numerically matches v*M.
    vmat = compile_glsl("vs_vecmat.vert")
    vmhl = oracle_hlsl(vmat, "vs_vecmat_vert")
    vpos = [1.25, -2.0, 0.5, 1.0]
    basis = [[2.0, -1.0, 0.5, 3.0],
             [0.25, 1.5, -2.0, 0.0],
             [-1.0, 0.75, 4.0, -0.5],
             [1.0, -3.0, 2.0, 0.5]]
    vgot = eval_vertex_position(vmat, {0: vpos},
                                {i: basis[i] for i in range(4)})
    vwant = vec_mat(vpos, basis)
    need = ["r3.xyzw = u_basis(0).xyzw * r0.xxxx",
            "r3.xyzw = u_basis(3).xyzw * r0.wwww + r3.xyzw",
            "oPos.xyzw = max(r3.xyzw, r3.xyzw)",
            "oTexCoord0", "oColor0"]
    miss = [s for s in need if not vmhl or not found(s, vmhl)]
    if vmhl is None or miss or not close4(vgot, vwant):
        ok = False
        print(f"  FAIL vs_vecmat.vert: vec*mat wrong "
              f"(missing {miss}, got {vgot}, want {vwant})")
    else:
        print(f"  PASS vs_vecmat.vert: vec4 * mat4 -> mul/mad row vector "
              f"({len(vmat)} B)")

    # vertex: MatrixTimesMatrix forms each product row with vec*mat, then the
    # existing row-form mat*vec path consumes the product matrix.
    mm = compile_glsl("vs_matmat.vert")
    mmhl = oracle_hlsl(mm, "vs_matmat_vert")
    model = [[1.0, 2.0, 0.0, -1.0],
             [0.5, -1.0, 3.0, 2.0],
             [-2.0, 0.0, 1.0, 0.25],
             [4.0, -0.5, 2.0, 1.0]]
    viewproj = [[0.75, -1.0, 2.0, 0.5],
                [1.5, 0.25, -0.75, 3.0],
                [-1.0, 2.0, 0.5, -2.0],
                [0.0, -3.0, 1.25, 1.0]]
    consts = {i: model[i] for i in range(4)}
    consts.update({4 + i: viewproj[i] for i in range(4)})
    mgot = eval_vertex_position(mm, {0: vpos}, consts)
    mwant = mat_vec(mat_mat(viewproj, model), vpos)
    need = ["r4.xyzw = u_model(0).xyzw * r3.xxxx",
            "r10.xyzw = u_model(3).xyzw * r9.wwww + r10.xyzw",
            "oPos.x = dot(r0.xyzw, r4.xyzw)",
            "oPos.w = dot(r0.xyzw, r10.xyzw)"]
    miss = [s for s in need if not mmhl or not found(s, mmhl)]
    if mmhl is None or miss or not close4(mgot, mwant):
        ok = False
        print(f"  FAIL vs_matmat.vert: mat*mat wrong "
              f"(missing {miss}, got {mgot}, want {mwant})")
    else:
        print(f"  PASS vs_matmat.vert: mat4 * mat4 rows feed mat4 * vec4 "
              f"({len(mm)} B)")

    # vertex: arithmetic values can feed both position and interpolators
    varith = compile_glsl("vtx_arith.vert")
    vahl = oracle_hlsl(varith, "vtx_arith_vert")
    if vahl is None:
        ok = False
        print("  FAIL vtx_arith.vert: XenosRecomp rejected it")
    else:
        need = ["r3.xyzw = r0.xyzw + u_offset.xyzw",
                "dot(r3.xyzw, u_mvp(0)", "dot(r3.xyzw, u_mvp(3)",
                "r4.xyzw = r1.xyzw * u_tint.xyzw",
                "oColor0.xyzw = max(r4.xyzw, r4.xyzw)"]
        miss = [s for s in need if not found(s, vahl)]
        if miss:
            ok = False
            print(f"  FAIL vtx_arith.vert HLSL missing {miss}")
        else:
            print(f"  PASS vtx_arith.vert: VS add/mul feed position + "
                  f"varying exports ({len(varith)} B)")

    # vertex: literals use the same manifest contract as fragments.
    vlight, vlman = lower_ex(spirv.Module.parse(
        (TD / "vs_lighting.vert.spv").read_bytes()))
    vlhl = oracle_hlsl(vlight, "vs_lighting_vert")
    want_vlman = [(4, (0.0, 0.0, 0.0, 0.0))]
    need = ["rsqrt(r7.w)", "r9.xyzw = max(r6.zzzz, r6.zzzz)",
            "max(r9.xyzw, _lit4.xyzw)",
            "oColor0.xyzw = max(r4.xyzw, r4.xyzw)"]
    miss = [s for s in need if not vlhl or not found(s, vlhl)]
    if vlhl is None or miss or vlman != want_vlman:
        ok = False
        print(f"  FAIL vs_lighting.vert: VS literal manifest wrong "
              f"(missing {miss}, manifest {vlman})")
    else:
        print(f"  PASS vs_lighting.vert: VS literal manifest feeds "
              f"normalize/dot/max lighting ({len(vlight)} B)")

    # vertex: prior literal crash path now lowers with manifest + scalar sin.
    vwave, vwman = lower_ex(spirv.Module.parse(
        (TD / "vs_wave.vert.spv").read_bytes()))
    vwhl = oracle_hlsl(vwave, "vs_wave_vert")
    want_vwman = [(4, (0.5, 0.5, 0.5, 0.5))]
    need = ["ps = sin(r6.w)", "r3.xzw = max(r0.xzw, r0.xzw)",
            "r3.y = r0.y + r4.y", "dot(r3.xyzw, u_mvp(0)"]
    miss = [s for s in need if not vwhl or not found(s, vwhl)]
    if vwhl is None or miss or vwman != want_vwman:
        ok = False
        print(f"  FAIL vs_wave.vert: VS literal/sin path wrong "
              f"(missing {miss}, manifest {vwman})")
    else:
        print(f"  PASS vs_wave.vert: VS literal + sin path lowers with "
              f"manifest ({len(vwave)} B)")

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
