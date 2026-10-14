#!/usr/bin/env python3
"""compare_pack.py <shader dir>: each pixel shader there lowered as Dusklight's pack lowers it (glslang, then xsc with
the optimiser), with milestone 36's fetch layout and without; for those that differ, both run on xost's simulator from
the same random interpolators, constants and texture, three times, their exports compared. Prints each changed
program's registers and its longest run of fetches, before and after."""
import math
import random
import subprocess
import sys
import tempfile
from pathlib import Path

XOST = Path(__file__).resolve().parents[2] / "xenos-toolchain"
sys.path.insert(0, str(XOST))

import xasm.program as program  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc import spirv  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402

GLSLANG = XOST / "tools" / "glslang" / "bin" / "glslang.exe"


def lowered(module, layout):
    grouped = program.PixelProgram._group_fetches
    if not layout:
        program.PixelProgram._group_fetches = lambda self: None
    try:
        return lower_ex(module, XSDK_REGISTERS, True)
    finally:
        program.PixelProgram._group_fetches = grouped


def registers(blob):
    return ((container.Container(blob).program_control >> 8) & 0x3F) + 1


def longest_run(blob):
    c = container.Container(blob)
    best = run = 0
    cf = None
    for ins in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if ins.kind == "cf":
            continue
        if ins.exec_cf_index != cf or ins.kind == "alu":
            run = 0
        cf = ins.exec_cf_index
        if ins.kind in ("tfetch", "vfetch"):
            run += 1
            best = max(best, run)
    return best


def texture(slot, coordinates):
    u, v, w = coordinates
    return [math.sin(3.1 * u + slot), math.cos(2.3 * v - slot), u * v + 0.5 * w, 1.0 / (1.0 + u * u + v * v)]


def run(blob, manifest, seed):
    rng = random.Random(seed)
    regs = {r: [rng.uniform(0.05, 0.95) for _ in range(4)] for r in range(16)}
    consts = {r: [rng.uniform(-1.0, 1.0) for _ in range(4)] for r in range(256)}
    consts.update({r: list(v) for r, v in manifest})
    result = simulate.run(blob, registers=regs, constants=consts, texture_fetch=texture)
    return result.exports, result.killed if hasattr(result, "killed") else None


def same(a, b):
    if a.keys() != b.keys():
        return False
    for k in a:
        for x, y in zip(a[k], b[k]):
            if math.isnan(x) and math.isnan(y):
                continue
            if abs(x - y) > 1e-4 * max(1.0, abs(x)):
                return False
    return True


def main():
    sys.setrecursionlimit(20000)
    shaders = sorted(Path(sys.argv[1]).glob("*.frag"))
    changed = wrong = 0
    with tempfile.TemporaryDirectory() as directory:
        for source in shaders:
            output = Path(directory) / (source.name + ".spv")
            subprocess.run([str(GLSLANG), "-V", str(source), "-o", str(output)], check=True, capture_output=True)
            module = spirv.Module.parse(output.read_bytes())
            before, before_manifest = lowered(module, False)
            after, after_manifest = lowered(module, True)
            if before == after:
                continue
            changed += 1
            results = []
            for seed in range(3):
                results.append(same(run(before, before_manifest, seed)[0], run(after, after_manifest, seed)[0]))
            ok = all(results)
            wrong += not ok
            print(f"{source.name}: registers {registers(before)} -> {registers(after)}, longest fetch run "
                  f"{longest_run(before)} -> {longest_run(after)}, exports {'same' if ok else 'DIFFER'}")
    print(f"{len(shaders)} pixel shaders, {changed} laid out anew, {wrong} with different exports")
    sys.exit(1 if wrong else 0)


if __name__ == "__main__":
    main()
