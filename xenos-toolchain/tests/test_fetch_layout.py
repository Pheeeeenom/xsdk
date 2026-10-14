#!/usr/bin/env python3
"""xsc lays a pixel shader's fetches out together, after the work their coordinates need (xasm/program.py,
_group_fetches).

Four fetches each right after its coordinate's add, the coordinates sharing a register, sampled 10 texels a clock on
the console; the adds first and the four fetches together in an exec of their own, 16, the texture units' rate (the
SDK's docs/HARDWARE.md, "The texel rate, milestone 36").

- vk_texel_offsets.frag: its coordinates' adds come before its fetches, the four fetches are an exec of their own, no
  fetch in it writes a register another in it reads, and the simulator gives what the GLSL says.
- vk_texel_dependent.frag: the fetch reading another's result comes after it, the other two go with the first, and the
  simulator gives what the GLSL says.
- fs_dsolid.frag, its one fetch reading the add before it, keeps them in one exec: nothing was measured to gain from
  an exec of its own.
- Every fragment shader in xsc/testdata whose fetches read only its inputs lowers byte for byte as without the layout.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xasm.program as program  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402
from test_xsc_lower import TD  # noqa: E402

UV = [0.75, 0.25]
DELTA = [0.125, 0.25, 0.0625, 0.5]
SCALE = [0.25, 0.5, 1.0, 2.0]
OPERAND = re.compile(r"\br(\d+)")
WRITE = re.compile(r"^\s*(?:\+\s*)?(?:\(!?p0\)\s*)?\w+\s+r(\d+)")


def module(name):
    return spirv.Module.parse((TD / f"{name}.spv").read_bytes())


def lower(name):
    return lower_ex(module(name), XSDK_REGISTERS, True)[0]


def lower_without_layout(name):
    grouped = program.PixelProgram._group_fetches
    program.PixelProgram._group_fetches = lambda self: None
    try:
        return lower(name)
    finally:
        program.PixelProgram._group_fetches = grouped


def blocks(blob):
    """The program's execs, each a list of its instructions as (whether it's a fetch, the registers it writes, those it
    reads)."""
    c = container.Container(blob)
    out, cf = [], None
    for ins in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if ins.kind == "cf":
            continue
        if ins.exec_cf_index != cf:
            out.append([])
            cf = ins.exec_cf_index
        fetch = ins.kind in ("tfetch", "vfetch")
        if fetch:
            regs = [int(r) for r in OPERAND.findall(ins.text)]
            writes, reads = (regs[:1], regs[1:2]) if len(regs) > 1 else ([], regs)
        else:
            writes = [int(m[1]) for line in ins.text.splitlines() for m in [WRITE.match(line)] if m]
            reads = []
        out[-1].append((fetch, writes, reads))
    return out


def sample(slot, coordinates):
    """What texture fetch constant slot holds at u, v: a value of both, and of the slot."""
    u, v = coordinates[0], coordinates[1]
    return [u + slot, 2.0 * v + slot, u * v, 1.0 + slot]


def add(*vectors):
    return [sum(c) for c in zip(*vectors)]


def slots(name):
    return {v.binding: slot for v, slot in resolve_interface(module(name)).samplers}


def push(name, values):
    block = next(b for b in resolve_interface(module(name)).blocks if b.kind == "push")
    return {block.reg + i: values[4 * i:4 * i + 4] for i in range(len(values) // 4)}


def simulated(name, values):
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS, True)
    constants = push(name, values)
    constants.update({r: list(v) for r, v in manifest})
    return simulate.run(blob, registers={0: UV + [0.0, 0.0]}, constants=constants, texture_fetch=sample).exports[0]


def close(got, want):
    return all(abs(g - w) <= 1e-4 * max(1.0, abs(w)) for g, w in zip(got, want))


def check(label, passed, detail=""):
    print(f"  {'PASS' if passed else 'FAIL'} {label}{': ' + detail if detail and not passed else ''}")
    return passed


def fetch_runs(blob):
    """Each exec holding a fetch, as (whether it holds only fetches, its fetches' destinations and sources)."""
    runs = []
    for block in blocks(blob):
        fetches = [(w[0], r[0]) for fetch, w, r in block if fetch]
        if fetches:
            runs.append((len(fetches) == len(block), fetches))
    return runs


def offsets():
    name = "vk_texel_offsets.frag"
    blob = lower(name)
    runs = fetch_runs(blob)
    ok = check(f"{name}: its four fetches are one exec of their own", len(runs) == 1 and runs[0][0]
               and len(runs[0][1]) == 4, str(runs))
    first = next(i for i, block in enumerate(blocks(blob)) if any(fetch for fetch, _, _ in block))
    work = sum(len(block) for block in blocks(blob)[:first])
    ok &= check(f"{name}: its coordinates' three adds come before them", work == 3, f"{work} instructions before")
    dests = {d for d, _ in runs[0][1]} if runs else set()
    sources = {s for _, s in runs[0][1]} if runs else set()
    ok &= check(f"{name}: no fetch in the exec writes a register another reads", not dests & sources,
                f"{sorted(dests & sources)} both")
    slot = slots(name)[0]
    uvs = [UV, [UV[0] + DELTA[0], UV[1] + DELTA[2]], [UV[0] + DELTA[2], UV[1] + DELTA[1]],
           [UV[0] + DELTA[0], UV[1] + DELTA[1]]]
    want = [s * k for s, k in zip(add(*(sample(slot, uv) for uv in uvs)), SCALE)]
    got = simulated(name, DELTA + SCALE)
    ok &= check(f"{name}: the simulator gives what the GLSL says", close(got, want), f"{got}, not {want}")
    return ok


def dependent():
    name = "vk_texel_dependent.frag"
    blob = lower(name)
    runs = fetch_runs(blob)
    ok = check(f"{name}: two fetch execs, the first with two fetches, the second with the dependent one",
               [len(r[1]) for r in runs] == [2, 1] and all(r[0] for r in runs), str(runs))
    s = slots(name)
    offset = sample(s[0], UV)
    moved = sample(s[1], [UV[0] + offset[0] * DELTA[3], UV[1] + offset[1] * DELTA[3]])
    across = sample(s[1], [UV[0] + DELTA[0], UV[1] + DELTA[1]])
    want = add(offset, moved, across)
    got = simulated(name, DELTA)
    ok &= check(f"{name}: the simulator gives what the GLSL says", close(got, want), f"{got}, not {want}")
    return ok


def reads_only_inputs(blob):
    """Whether every fetch reads a register nothing before it wrote: an interpolator or the pixel parameters."""
    written = set()
    for block in blocks(blob):
        for fetch, writes, reads in block:
            if fetch and written & set(reads):
                return False
            written |= set(writes)
    return True


def lone_fetch():
    name = "fs_dsolid.frag"
    return check(f"{name}: its one fetch stays in an exec with the add before it", lower(name) ==
                 lower_without_layout(name))


def inputs_only():
    ok, same, laid_out = True, 0, 0
    for path in sorted(TD.glob("*.frag.spv")):
        name = path.name[:-len(".spv")]
        try:
            before = lower_without_layout(name)
        except (LowerError, NotImplementedError, ValueError):
            continue
        if not reads_only_inputs(before):
            laid_out += 1
            continue
        if lower(name) != before:
            ok &= check(f"{name}: lowers as without the layout", False, "it changed")
        same += 1
    check(f"{same} fragment shaders that sample only their inputs lower byte for byte as before; {laid_out} others "
          f"are laid out", ok and same > 0)
    return ok and same > 0


def main():
    sys.setrecursionlimit(20000)
    print("== fetches laid out together ==")
    results = [offsets(), dependent(), lone_fetch(), inputs_only()]
    if not all(results):
        sys.exit(1)
    print("OK")


if __name__ == "__main__":
    main()
