#!/usr/bin/env python3
"""check_optimize.py <shader or folder>...: compiles each GLSL shader with and without xasm/optimize.py, at xsdk's
register split, and checks the two give the same exports, run on random inputs and constants by xdis/equivalence.py:
first the instructions just before and after the pass, whose undefined reads show, then the two programs' microcode,
on the export components the first run found defined. Prints each shader's ALU instructions and registers before and
after, and the totals; exits 1 if any differ.

    python tools/check_optimize.py ../local/m11-xenia-record/shaders --trials 20
"""
import argparse
import math
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xasm.program as program  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.equivalence as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xasm.program import ABS_SOURCE, XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402

GLSLANG = ROOT / "tools" / "glslang" / "bin" / "glslang.exe"

# Each program's instructions just before and after the pass, [(kind, fields)], from the last finish().
_captured = []
_optimize = program._Program._optimize


def _capture(self):
    def snapshot():
        return [(ins.kind, dict(ins.fields)) for stage in self._stages for ins in stage]
    before = snapshot()
    _optimize(self)
    _captured.append((before, snapshot()))


program._Program._optimize = _capture


def counts(blob):
    c = container.Container(blob)
    records = xu._decode_program(c.ucode_dwords, c.shader_type)[0]
    alu = sum(1 for r in records if r.kind == "alu")
    regs = c.program_control & 0x3F if c.shader_type == "vertex" else (c.program_control >> 8) & 0x3F
    return alu, regs + 1


def same(a, b):
    if a is None or b is None:
        return a is b
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    return a == b


def differs(want, got, defined=None):
    """What differs between two runs' exports, on the components defined holds (want's that aren't None)."""
    defined = want if defined is None else defined
    if set(want) != set(got):
        return f"exports {sorted(want)} became {sorted(got)}"
    for reg in want:
        for i in range(4):
            if defined[reg][i] is not None and not same(want[reg][i], got[reg][i]):
                return f"export {reg}.{'xyzw'[i]} {want[reg][i]} became {got[reg][i]}"
    return None


def check(spv_bytes, trials, seed):
    """(before, after, problem): the ALU and register counts each way, and what differed, if anything."""
    plain, manifest = lower_ex(spirv.Module.parse(spv_bytes), XSDK_REGISTERS)
    _captured.clear()
    optimized, manifest_opt = lower_ex(spirv.Module.parse(spv_bytes), XSDK_REGISTERS, optimize=True)
    before, after = _captured[-1]
    if manifest != manifest_opt:
        return counts(plain), counts(optimized), "the literal constants differ"
    rng = random.Random(seed)
    for trial in range(trials):
        registers = {r: [rng.uniform(-2.0, 2.0) for _ in range(4)] for r in simulate.inputs(before)}
        constants = {i: [rng.uniform(-2.0, 2.0) for _ in range(4)] for i in range(256)}
        for entry in manifest:
            if len(entry) == 2 and isinstance(entry[0], int):
                constants[entry[0]] = list(entry[1])
        try:
            want = simulate.run_fields(before, registers, constants, ABS_SOURCE)
            got = simulate.run_fields(after, registers, constants, ABS_SOURCE)
            problem = differs(want, got)
            if problem:
                return counts(plain), counts(optimized), f"trial {trial}, instructions: {problem}"
            problem = differs(simulate.run(plain, registers, constants),
                              simulate.run(optimized, registers, constants), want)
            if problem:
                return counts(plain), counts(optimized), f"trial {trial}, microcode: {problem}"
        except simulate.Unsupported as e:
            return counts(plain), counts(optimized), f"not simulated: {e}"
    return counts(plain), counts(optimized), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--trials", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    sources = []
    for p in map(Path, args.paths):
        sources += sorted(q for q in p.iterdir() if q.suffix in (".vert", ".frag")) if p.is_dir() else [p]
    totals = {"vert": [0, 0], "frag": [0, 0]}
    failed = 0
    skipped = 0
    with tempfile.TemporaryDirectory() as work:
        for source in sources:
            out = Path(work) / (source.name + ".spv")
            subprocess.run([str(GLSLANG), "-V", str(source), "-o", str(out)], check=True, capture_output=True)
            before, after, problem = check(out.read_bytes(), args.trials, args.seed)
            kind = source.suffix[1:]
            totals[kind][0] += before[0]
            totals[kind][1] += after[0]
            if problem and problem.startswith("not simulated"):
                skipped += 1
            elif problem:
                failed += 1
            print(f"{source.name}: {before[0]} -> {after[0]} ALU, {before[1]} -> {after[1]} registers; "
                  f"{problem or 'ok'}")
    for kind, (b, a) in totals.items():
        if b:
            print(f"{kind}: {b} -> {a} ALU instructions ({100.0 * (b - a) / b:.1f}% fewer)")
    print(f"{len(sources)} shaders, {failed} differ, {skipped} not simulated")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
