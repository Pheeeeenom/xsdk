#!/usr/bin/env python3
"""xost's compiler in C gives what the Python one gives: the same container bytes, literals and resources, or the same
kind of error with the same message, for every SPIR-V module under xsc/ and in the C files named on the command line
(arrays that start with SPIR-V's magic, as vulkan4xenon's and Dusklight's generated shader files hold them), over both
register splits with the optimiser off and on. Runs in WSL, where make builds build/host/libxsc.so.

usage: test_native_parity.py [--quiet] [generated.c ...]
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xsc.native as native  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xasm.program import LIBXENON_REGISTERS, XSDK_REGISTERS, RegisterBudgetError  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402

SPIRV_MAGIC = 0x07230203
CONFIGS = [(name, budget, optimize) for name, budget in (("libxenon", LIBXENON_REGISTERS), ("xsdk", XSDK_REGISTERS))
           for optimize in (False, True)]


def corpus(extra):
    for path in sorted((ROOT / "xsc").glob("*/*.spv")):
        yield path.relative_to(ROOT).as_posix(), path.read_bytes()
    for c_file in extra:
        text = Path(c_file).read_text(encoding="utf-8")
        for name, body in re.findall(r"(\w+)\[\] = \{([^}]*)\}", text):
            words = [int(w, 16) for w in re.findall(r"0x[0-9A-Fa-f]{1,8}", body)]
            if words and words[0] == SPIRV_MAGIC:
                yield f"{Path(c_file).name}:{name}", struct.pack(f"<{len(words)}I", *words)


def python_result(blob, budget, optimize):
    """('ok', container, manifest, resources) or (kind, message)."""
    try:
        container, manifest = lower_ex(spirv.Module.parse(blob), budget, optimize)
    except RegisterBudgetError as e:
        return "registers", str(e)
    except LowerError as e:
        return "refused", str(e)
    except ValueError as e:
        return "value", str(e)
    except Exception as e:  # noqa: BLE001 - every other exception is the C's malformed
        return "malformed", f"{type(e).__name__}: {e}"
    si = resolve_interface(spirv.Module.parse(blob))
    resources = [(b.kind, b.descriptor_set or 0, b.binding or 0, b.reg, b.regs) for b in si.blocks]
    resources += [("sampler", v.descriptor_set or 0, v.binding or 0, slot, 1) for v, slot in si.samplers]
    return "ok", container, [_exact(entry) for entry in manifest], resources


def _exact(entry):
    """A literal's values as their bits, so -0.0 and 0.0 differ."""
    if isinstance(entry[0], int):
        return entry[0], struct.pack("<4d", *entry[1])
    return tuple(entry)


def native_result(blob, budget, optimize):
    try:
        container, manifest, resources = native.compile_spirv(blob, budget.pixel, budget.vertex, optimize)
    except native.CompileError as e:
        kind = {native.REFUSED: "refused", native.REGISTERS: "registers", native.BAD_VALUE: "value"}
        return kind.get(e.status, native.STATUS_NAMES[e.status]), str(e)
    return "ok", container, [_exact(entry) for entry in manifest], resources


def same(py, c):
    """Whether the results match: a Python ValueError the C can't tell from bad SPIR-V may come back malformed."""
    if py[0] == "malformed" or (py[0] == "value" and c[0] == "malformed"):
        return c[0] == "malformed"
    return py == c


def describe(result):
    if result[0] != "ok":
        return f"{result[0]}: {result[1]}"
    return f"ok, {len(result[1])} bytes, {len(result[2])} literals, {len(result[3])} resources"


def first_difference(a, b):
    return next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))


def main():
    args = sys.argv[1:]
    quiet = "--quiet" in args
    extra = [a for a in args if a != "--quiet"]
    passed = failed = 0
    kinds = {}
    for name, blob in corpus(extra):
        for config, budget, optimize in CONFIGS:
            label = f"{name} [{config}{', optimize' if optimize else ''}]"
            py = python_result(blob, budget, optimize)
            c = native_result(blob, budget, optimize)
            kinds[py[0]] = kinds.get(py[0], 0) + 1
            if same(py, c):
                passed += 1
                if not quiet:
                    print(f"  PASS {label}: {describe(py)}")
                continue
            failed += 1
            print(f"  FAIL {label}\n    python: {describe(py)}\n    c:      {describe(c)}")
            if py[0] == c[0] == "ok" and py[1] != c[1]:
                print(f"    containers differ from byte {first_difference(py[1], c[1])}")
            elif py[0] == c[0] == "ok" and py[2] != c[2]:
                print(f"    literals: python {py[2]}\n              c      {c[2]}")
            elif py[0] == c[0] == "ok":
                print(f"    resources: python {py[3]}\n               c      {c[3]}")
    print(f"{passed} of {passed + failed} match; python gave {', '.join(f'{v} {k}' for k, v in sorted(kinds.items()))}")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
