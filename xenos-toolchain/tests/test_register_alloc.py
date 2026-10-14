#!/usr/bin/env python3
"""Register allocation past r63, and dependent texture reads, as vulkan4xenon's milestone 11 needs for Aurora's
lighting, fog and indirect texturing (xasm/program.py, xsc/lower.py).

- vk_many_live.vert holds forty values live at once among more than sixty-four: it encodes only when the allocator
  recycles dead temporaries, which a vertex shader's now does past r63. Run on xdis.simulate, it gives the sums a
  reference gives, and the oracle accepts it. Without the allocator it's refused; with an allocation corrupted, the
  simulation's numbers are wrong, so the check would see a wrong allocation.
- vk_dependent_read.frag samples an image where an offsets texture says: the offsets are fetched first, and the
  simulation samples the image at the offset coordinates.
- A pixel shader whose hardware loop reads a value from before the loop: the allocator keeps that value's register
  through the loop, as each pass reads it again.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xasm.program as program  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.container_emit as ce  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xasm.program import PixelProgram, RegisterBudgetError, XSDK_REGISTERS  # noqa: E402
from xsc.lower import _templates, lower_ex  # noqa: E402
from test_xsc_lower import TD, oracle_hlsl  # noqa: E402

V = xu.AluVectorOpcode
VERTEX = 7.0                         # the vertex index in r0
POSITION = [0.75, -1.5, 2.0, 1.0]
SCALE = [0.5, -2.0, 0.0, 1.0]
WEIGHTS = [[(i + 1) * 0.25, -0.5 * (i % 3), 1.0 / (i + 2), float(i % 5) - 2.0] for i in range(40)]


def lowered(name):
    return lower_ex(spirv.Module.parse((TD / name).read_bytes()), XSDK_REGISTERS)


def single(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def close(got, want, eps=1e-4):
    return all(abs(g - w) <= eps * max(1.0, abs(w)) for g, w in zip(got, want))


def records(blob):
    c = container.Container(blob)
    return xu._decode_program(c.ucode_dwords, c.shader_type)[0]


def many_live_sums(blob, manifest):
    """The program's v_sum and position. Input 1 is fetched first and input 0, into r0, last (xsc/lower.py); a fetch
    that reads another vertex's index reads NaNs."""
    consts = {i: list(w) for i, w in enumerate(WEIGHTS)}
    consts.update({reg: list(values) for reg, values in manifest})

    def fetch(n, instruction, index):
        if index != VERTEX:
            return [float("nan")] * 4
        return SCALE if n == 0 else POSITION

    result = simulate.run(blob, registers={0: [VERTEX, 0.0, 0.0, 0.0]}, constants=consts, vertex_fetch=fetch)
    return result.exports.get(0), result.exports.get(int(xu.ExportRegister.kVSPosition))


def check_many_live():
    failures = []
    blob, manifest = lowered("vk_many_live.vert.spv")
    d = [sum(w[k] * POSITION[k] for k in range(4)) for w in WEIGHTS]
    want = [sum(reversed(d)) * SCALE[0], sum(x if i % 2 == 0 else -x for i, x in enumerate(d)), d[0], d[39] * SCALE[1]]
    got, position = many_live_sums(blob, manifest)
    if got is None or not close(got, want) or not close(position or [], POSITION):
        failures.append(f"vk_many_live.vert: v_sum {got}, position {position}; want {want}, {POSITION}")
    else:
        print("  PASS vk_many_live.vert: the simulated sums are the reference's")
    if oracle_hlsl(blob, "many_live") is None:
        failures.append("vk_many_live.vert: XenosRecomp rejected it")

    # Without the allocator, a vertex shader past r63 is refused.
    original = program._Program._allocate_registers
    program._Program._allocate_registers = lambda self: None
    try:
        lowered("vk_many_live.vert.spv")
        failures.append("vk_many_live.vert: lowered without the allocator, past r63")
    except RegisterBudgetError as error:
        print(f"  PASS without the allocator it's refused: {error}")
    finally:
        program._Program._allocate_registers = original

    # An allocation that gives one source the wrong register is seen by the numbers: the last add of two temps reads
    # its first operand twice.

    def corrupted(self):
        original(self)
        instrs = [ins for stage in self._stages for ins in stage if ins.kind == "alu"]
        for ins in reversed(instrs):
            f = dict(ins.fields)
            if f.get("vector_opcode") == V.kAdd and f.get("src1_is_temp") and f.get("src2_is_temp") and \
                    f["src1_reg"] != f["src2_reg"]:
                f["src2_reg"] = f["src1_reg"]
                ins.fields = f
                ins.triple = tuple(program.enc.build_alu(**f))
                break

    program._Program._allocate_registers = corrupted
    try:
        blob, manifest = lowered("vk_many_live.vert.spv")
        got, _ = many_live_sums(blob, manifest)
        if got is not None and close(got, want):
            failures.append("vk_many_live.vert: a corrupted allocation gave the reference's sums")
        else:
            print("  PASS a corrupted allocation gives other sums")
    finally:
        program._Program._allocate_registers = original
    return failures


def offsets(u, v):
    return [u * 0.5, v * 0.25 - 0.125, 0.0, 1.0]


def image(u, v):
    return [u, v, u + v, 1.0]


def check_dependent_read():
    failures = []
    blob, manifest = lowered("vk_dependent_read.frag.spv")
    fetches = [xu.TextureFetchInstruction(*r.raw) for r in records(blob) if r.kind == "tfetch"]
    slots = [f.fetch_constant_index() for f in fetches]
    if len(slots) != 2:
        return [f"vk_dependent_read.frag: {len(slots)} texture fetches"]
    first, second = slots
    uv = [0.375, 0.625]
    sampled = []

    def fetch(slot, coords):
        sampled.append((slot, coords[:2]))
        return (offsets if slot == first else image)(coords[0], coords[1])

    consts = {reg: list(values) for reg, values in manifest}
    result = simulate.run(blob, registers={0: uv + [0.0, 0.0]}, constants=consts, texture_fetch=fetch)
    o = offsets(*uv)
    want = image(single(uv[0] + o[0] * 0.25), single(uv[1] + o[1] * 0.25))
    got = result.exports.get(xu.kPSColor0)
    if sampled[0] != (first, uv) or got is None or not close(got, want):
        failures.append(f"vk_dependent_read.frag: fetched {sampled}, colour {got}; want offsets at {uv}, then {want}")
    else:
        print(f"  PASS vk_dependent_read.frag: the offsets are fetched first, and the image where they point")
    if oracle_hlsl(blob, "dependent_read") is None:
        failures.append("vk_dependent_read.frag: XenosRecomp rejected it")
    return failures


def check_loop():
    """r1 is read in the loop's body before r31 is written there: on the next pass the body reads r1 again, so r31
    mustn't take r1's register. Eighteen chained temps take the shader past 16 registers, so the allocator runs."""
    T = _templates()

    def alu(op, dest, srcs, export=False):
        f = dict(T["mov"], vector_opcode=op, vector_dest=dest, is_export=export, vector_write_mask=0xF,
                 vector_clamp=False, scalar_write_mask=0)
        for i, (reg, is_temp) in enumerate(srcs, start=1):
            f[f"src{i}_reg"] = reg
            f[f"src{i}_is_temp"] = is_temp
            f[f"src{i}_swizzle"] = 0
            f[f"src{i}_negate"] = False
        return f

    p = PixelProgram()
    p.input(0, 5)
    p.alu(**alu(V.kMax, 1, [(0, False), (0, False)]))                  # r1 = c0, read in the loop
    p.alu(**alu(V.kAdd, 2, [(0, True), (1, False)]))
    for r in range(3, 20):
        p.alu(**alu(V.kAdd, r, [(r - 1, True), (1, False)]))         # r2..r19, each dead after the next
    p.alu(**alu(V.kMax, 22, [(1, False), (1, False)]))                 # r22 = c1, the sum
    p.begin_loop(loop_id=0)
    p.alu(**alu(V.kAdd, 30, [(1, True), (2, False)]))                  # r30 = r1 + c2: r1's last read
    p.alu(**alu(V.kMul, 31, [(30, True), (3, False)]))                 # r31, written after it
    p.alu(**alu(V.kAdd, 22, [(22, True), (31, True)]))
    p.end_loop()
    p.alloc_colors()
    p.alu(**alu(V.kAdd, 0, [(22, True), (19, True)], export=True))
    consts = [ce.Constant(f"c{r}", ce.RS_FLOAT4, r, 1, ce.CLASS_VECTOR, ce.TYPE_FLOAT, rows=1, columns=4)
              for r in range(4)]
    blob = p.finish(constants=consts)
    alus = [xu.AluInstruction(*r.raw) for r in records(blob) if r.kind == "alu"]
    body = alus[20:23]                                                 # after r1, r2..r19 and r22
    held = body[0].src_reg(1) & 0x3F
    written = [ins.vector_dest() for ins in body]
    if held in written:
        return [f"loop: the body writes r{held}, the register of the value it reads from before the loop "
                f"(writes {written})"]
    print(f"  PASS loop: r{held}, read from before the loop, isn't written in it (writes {written})")
    return []


def main():
    failures = check_many_live() + check_dependent_read() + check_loop()
    for failure in failures:
        print(f"  FAIL {failure}")
    if failures:
        raise SystemExit("FAILED")
    print("OK")


if __name__ == "__main__":
    main()
