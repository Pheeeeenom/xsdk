#!/usr/bin/env python3
"""Byte-parity test: xcomb (C99) vs xasm.combiner.Generator (Python).

1. Regenerates xcomb/xcomb_templates.h (tools/gen_templates.py).
2. Builds tests/xcomb_parity/harness.c + xcomb/xcomb.c with MSVC
   (/W4 /WX /O2, warnings are errors).
3. Asserts byte-identical output on the 9-entry demo BANK below.
4. Fuzzes >= 300 seeded random valid combiner configurations (1-cycle and
   2-cycle, alpha differing from color, COMBINED only in cycle 1; the
   main batch avoids the A==ZERO,B!=ZERO subtract shape per the spec) and
   asserts byte parity on every case. An extra batch DOES include the
   A==ZERO,B!=ZERO shape with the rule: Python raises -> C must report
   error; Python succeeds -> bytes must match.

Run:  python tests/test_xcomb_parity.py
"""
import random
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from xasm.combiner import CombinerCycle, Generator, Src  # noqa: E402
from xsc import templates  # noqa: E402

VCVARS = (r"C:\Program Files\Microsoft Visual Studio\18\Community"
          r"\VC\Auxiliary\Build\vcvars64.bat")
HARNESS_DIR = ROOT / "tests" / "xcomb_parity"
BUILD_DIR = HARNESS_DIR / "build"
EXE = BUILD_DIR / "harness.exe"

# xcomb_src enum order in xcomb/xcomb.h
C_INDEX = {Src.ZERO: 0, Src.ONE: 1, Src.TEXEL0: 2, Src.TEXEL1: 3,
           Src.SHADE: 4, Src.PRIM: 5, Src.ENV: 6, Src.COMBINED: 7}

Z, O, T, S, P, E = (Src.ZERO, Src.ONE, Src.TEXEL0, Src.SHADE,
                    Src.PRIM, Src.ENV)
T1, C = Src.TEXEL1, Src.COMBINED

# The demo bank: six one-cycle modes and three two-cycle ones; out = (A-B)*C+D a cycle, and the second cycle sees the
# first's result as COMBINED.
BANK = [
    ("modulate",  CombinerCycle(T, Z, S, Z)),
    ("tex_prim",  CombinerCycle(T, Z, P, Z, aa=Z, ab=Z, ac=Z, ad=O)),
    ("lerp_pe_t", CombinerCycle(P, E, T, E, aa=Z, ab=Z, ac=Z, ad=O)),
    ("decal",     CombinerCycle(T, Z, O, Z)),
    ("shade",     CombinerCycle(S, Z, O, Z, aa=Z, ab=Z, ac=Z, ad=O)),
    ("inv_tex_p", CombinerCycle(O, T, P, Z, aa=Z, ab=Z, ac=Z, ad=O)),
    ("multitex",  CombinerCycle(T, Z, T1, Z),
                  CombinerCycle(C, Z, S, Z)),
    ("lerp2_p",   CombinerCycle(T, T1, S, T1),
                  CombinerCycle(C, Z, P, Z, aa=Z, ab=Z, ac=Z, ad=O)),
    ("add_glow",  CombinerCycle(T, Z, S, Z),
                  CombinerCycle(T1, Z, O, C, aa=Z, ab=Z, ac=Z, ad=O)),
]

FUZZ_MAIN = 300
FUZZ_ZERO_SUB = 60
SEED = 0xC0DE


def build_c() -> None:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    srcs = f'"{HARNESS_DIR / "harness.c"}" "{ROOT / "xcomb" / "xcomb.c"}"'
    cl = (f'cl /nologo /W4 /WX /O2 /I"{ROOT / "xcomb"}" '
          f'/Fe:"{EXE}" {srcs}')
    cmd = f'cmd /c ""{VCVARS}" >nul 2>&1 && {cl}"'
    r = subprocess.run(cmd, cwd=BUILD_DIR, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"MSVC build failed:\n{r.stdout}\n{r.stderr}")
    print(f"built {EXE.name} (MSVC /W4 /WX /O2)")


def cycle_args(cy: CombinerCycle) -> list:
    """Python cycle -> 8 explicit C ints (aa=None means 'same as color')."""
    color = cy.color_sel()
    alpha = cy.alpha_sel()
    return [str(C_INDEX[s]) for s in (*color, *alpha)]


def run_c(cycles) -> bytes | None:
    """None means the C side reported an error."""
    args = [str(EXE), str(len(cycles))]
    for cy in cycles:
        args += cycle_args(cy)
    r = subprocess.run(args, capture_output=True, text=True)
    txt = r.stdout.strip()
    if r.returncode == 1 and txt == "ERR":
        return None
    if r.returncode != 0:
        sys.exit(f"harness failed ({r.returncode}): {r.stderr}\nargs={args}")
    return bytes.fromhex(txt)


def describe(cycles) -> str:
    return " | ".join(
        f"({c.a.name},{c.b.name},{c.c.name},{c.d.name} / "
        f"{','.join(s.name for s in c.alpha_sel())})" for c in cycles)


def check(gen, cycles, label: str) -> None:
    try:
        py = gen.generate(*cycles)
        py_err = None
    except Exception as exc:  # noqa: BLE001 — parity on rejection too
        py, py_err = None, exc
    cb = run_c(cycles)
    if py is None:
        assert cb is None, (
            f"{label}: Python rejected ({py_err!r}) but C produced "
            f"{len(cb)} bytes\n  {describe(cycles)}")
        return
    assert cb is not None, (
        f"{label}: C reported error but Python produced {len(py)} bytes\n"
        f"  {describe(cycles)}")
    assert py == cb, (
        f"{label}: byte mismatch (py {len(py)} B vs c {len(cb)} B)\n"
        f"  {describe(cycles)}\n"
        f"  py: {py.hex()}\n  c : {cb.hex()}")


def rand_sel(rng, srcs, avoid_zero_sub: bool):
    while True:
        sel = tuple(rng.choice(srcs) for _ in range(4))
        if avoid_zero_sub and sel[0] == Z and sel[1] != Z:
            continue
        return sel


def rand_cycle(rng, srcs, avoid_zero_sub: bool) -> CombinerCycle:
    color = rand_sel(rng, srcs, avoid_zero_sub)
    if rng.random() < 0.5:
        return CombinerCycle(*color)
    alpha = rand_sel(rng, srcs, avoid_zero_sub)
    return CombinerCycle(*color, aa=alpha[0], ab=alpha[1],
                         ac=alpha[2], ad=alpha[3])


def fuzz_case(rng, avoid_zero_sub: bool):
    srcs0 = [Z, O, T, T1, S, P, E]            # COMBINED only in cycle 1
    c0 = rand_cycle(rng, srcs0, avoid_zero_sub)
    if rng.random() < 0.5:
        return (c0,)
    srcs1 = srcs0 + [C]
    return (c0, rand_cycle(rng, srcs1, avoid_zero_sub))


def main() -> None:
    # 0. templates header must be fresh
    r = subprocess.run([sys.executable,
                        str(ROOT / "tools" / "gen_templates.py")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"gen_templates.py failed:\n{r.stdout}\n{r.stderr}")

    build_c()

    gen = Generator(alu_template=templates.mov(), tfetch_template=templates.texture_fetch())

    max_len = 0
    n_bank = 0
    for i, entry in enumerate(BANK):
        name, cycles = entry[0], entry[1:]
        check(gen, cycles, f"bank[{i}] {name}")
        max_len = max(max_len, len(gen.generate(*cycles)))
        n_bank += 1
        print(f"  bank[{i}] {name}: OK "
              f"({len(gen.generate(*cycles))} B)")
    print(f"BANK parity: {n_bank}/{len(BANK)}")

    rng = random.Random(SEED)
    for i in range(FUZZ_MAIN):
        cycles = fuzz_case(rng, avoid_zero_sub=True)
        check(gen, cycles, f"fuzz[{i}]")
        max_len = max(max_len, len(gen.generate(*cycles)))
    print(f"fuzz (main, A-B shape restricted): {FUZZ_MAIN}/{FUZZ_MAIN}")

    n_rejected = 0
    for i in range(FUZZ_ZERO_SUB):
        cycles = fuzz_case(rng, avoid_zero_sub=False)
        try:
            blob = gen.generate(*cycles)
            max_len = max(max_len, len(blob))
        except Exception:
            n_rejected += 1
        check(gen, cycles, f"fuzz-zsub[{i}]")
    print(f"fuzz (incl. A==ZERO subtract shape): "
          f"{FUZZ_ZERO_SUB}/{FUZZ_ZERO_SUB} "
          f"({n_rejected} rejected identically on both sides)")

    print(f"max container size observed: {max_len} B "
          f"(XCOMB_MAX_CONTAINER = 1024)")
    print(f"ALL GREEN: {n_bank} bank + {FUZZ_MAIN + FUZZ_ZERO_SUB} fuzz "
          f"cases byte-identical")


if __name__ == "__main__":
    main()
