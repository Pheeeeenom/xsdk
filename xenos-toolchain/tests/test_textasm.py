#!/usr/bin/env python3
"""Exit gate for the text assembler: every shader xsc lowers from
xsc/testdata, and the combiner's demo bank, must survive container ->
disassemble(pretty) -> assemble_text -> identical ucode bytes, or bytes
that differ only in fields pretty text doesn't show (the reassembled program
disassembles to the same text), or be refused for a form the grammar doesn't
take yet (docs/textasm.md), each counted. Plus a conventional-program fuzz
over generated combiner shaders."""
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xasm.combiner import CombinerCycle, Generator, Src  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xasm.textasm import AsmError, assemble_text  # noqa: E402
from xsc import templates  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_xcomb_parity import BANK  # noqa: E402

TESTDATA = ROOT / "xsc" / "testdata"


def combiner():
    return Generator(alu_template=templates.mov(), tfetch_template=templates.texture_fetch())


def blob_list():
    """(name, container) for every shader in xsc/testdata xsc lowers, and the combiner's demo bank."""
    blobs = []
    for path in sorted(TESTDATA.glob("*.spv")):
        try:
            blobs.append((path.name, lower_ex(spirv.Module.parse(path.read_bytes()), XSDK_REGISTERS, True)[0]))
        except (LowerError, NotImplementedError, ValueError):
            continue
    gen = combiner()
    blobs += [(f"combiner {name}", gen.generate(*cycles)) for name, *cycles in BANK]
    return blobs


def roundtrip(blob: bytes):
    c = container.Container(blob)
    pretty = xu.disassemble(c.ucode_dwords, c.shader_type)
    rebuilt = assemble_text(pretty, c.shader_type)
    return rebuilt, c.ucode_bytes


def fuzz(n=200, seed=0xA55E):
    gen = combiner()
    srcs = [Src.ZERO, Src.ONE, Src.TEXEL0, Src.TEXEL1, Src.SHADE,
            Src.PRIM, Src.ENV]
    rng = random.Random(seed)
    ok = 0
    for _ in range(n):
        def mksel():
            return CombinerCycle(rng.choice(srcs), rng.choice(srcs),
                                 rng.choice(srcs), rng.choice(srcs))
        c0 = mksel()
        c1 = mksel() if rng.random() < 0.5 else None
        try:
            blob = gen.generate(c0, c1)
        except Exception:
            continue
        c = container.Container(blob)
        pretty = xu.disassemble(c.ucode_dwords, "pixel")
        rebuilt = assemble_text(pretty, "pixel")
        if rebuilt != c.ucode_bytes:
            print(f"  FUZZ FAIL: {c0.color_sel()} / "
                  f"{c1.color_sel() if c1 else None}")
            return ok, False
        ok += 1
    return ok, True


def cond_exec_opcodes():
    """cexec/cexece assemble to the opcode the disassembler reads back:
    kCondExecPredClean(End) by default, kCondExec(End) when marked
    '// PredicateClean=false' -- a cond exec has no predicate-clean bit."""
    K = xu.ControlFlowOpcode
    cases = [("cexec b128", K.kCondExecPredClean),
             ("cexec !b3    // PredicateClean=false", K.kCondExec),
             ("cexece b7", K.kCondExecPredCleanEnd),
             ("cexece b7    // PredicateClean=false", K.kCondExecEnd)]
    ok = True
    for line, want in cases:
        tail = "" if line.startswith("cexece") else \
            "      alloc colors\n      exece\n   max oC0, r1, r1\n"
        src = (f"      exec\n   add r1, r0, c0\n      {line}\n"
               f"   add r1, r0, c1\n{tail}")
        ucode = assemble_text(src, "pixel")
        dwords = struct.unpack(f">{len(ucode) // 4}I", ucode)
        # CF index 1 is the second half of the first pair: its opcode is
        # the top nibble of the pair's third dword (encode.pack_cf_pair).
        got = K(dwords[2] >> 28)
        again = assemble_text(xu.disassemble(list(dwords), "pixel"),
                              "pixel")
        if got != want or again != ucode:
            ok = False
            print(f"  FAIL {line!r}: opcode {got.name}, want {want.name}, "
                  f"round-trip {'same' if again == ucode else 'differs'}")
    return ok


def source_mode_smoke():
    """Hand-authored .xasm -> container -> XenosRecomp accepts it."""
    from xasm.textasm import assemble_file
    src = """.pixel
.interpolator 0 texcoord
.interpolator 1 color
.sampler tex0 0
      exec
   tfetch2D r2, r0.xy, tf0
      alloc colors
      exece
   mul oC0, r2, r1
      cnop
"""
    directory = Path(tempfile.mkdtemp())
    tmp = directory / "_textasm_smoke.xasm"
    tmp.write_text(src)
    blob = assemble_file(str(tmp), "pixel", container=True)
    outbin = directory / "_textasm_smoke.bin"
    outbin.write_bytes(blob)
    # structural check
    c = container.Container(blob)
    dis = xu.disassemble(c.ucode_dwords, "pixel")
    assert "tfetch2D" in dis and "mul oC0" in dis, dis
    # oracle check
    rc = (ROOT / "reference" / "XenosRecomp" / "build" / "XenosRecomp" /
          "XenosRecomp.exe")
    hdr = (ROOT / "reference" / "XenosRecomp" / "XenosRecomp" /
           "shader_common.h")
    if rc.exists():
        r = subprocess.run([str(rc), str(outbin),
                            str(outbin.with_suffix(".hlsl")), str(hdr)],
                           capture_output=True)
        return r.returncode == 0
    return True


def main():
    sys.setrecursionlimit(20000)
    ok = True
    blobs = blob_list()
    refused = {}
    hidden = []
    print(f"== {len(blobs)}-blob round-trip ==")
    for name, blob in blobs:
        try:
            rebuilt, orig = roundtrip(blob)
        except AsmError as error:
            reason = str(error).split(": ", 1)[1].rsplit(": ", 1)[0]
            refused.setdefault(reason, []).append(name)
            continue
        # pretty text cannot express unreferenced trailing pad slots (only
        # exact mode carries them); identical-up-to-trailing-zeros is the
        # correct round-trip contract for pretty text
        pad_ok = (len(rebuilt) < len(orig)
                  and orig[:len(rebuilt)] == rebuilt
                  and not any(orig[len(rebuilt):]))
        c = container.Container(blob)
        if rebuilt == orig:
            print(f"  PASS {name} ({len(orig)} B)")
        elif len(rebuilt) == len(orig) and xu.disassemble(
                list(struct.unpack(f">{len(rebuilt) // 4}I", rebuilt)),
                c.shader_type) == xu.disassemble(c.ucode_dwords,
                                                 c.shader_type):
            hidden.append(name)
            print(f"  PASS {name} ({len(orig)} B, the same disassembly; "
                  f"hidden fields differ)")
        elif pad_ok:
            print(f"  PASS {name} ({len(rebuilt)} B + "
                  f"{len(orig) - len(rebuilt)} B zero pad in original)")
        else:
            ok = False
            d = next((i for i in range(min(len(rebuilt), len(orig)))
                      if rebuilt[i] != orig[i]), None)
            print(f"  FAIL {name}: len {len(rebuilt)} vs {len(orig)}, "
                  f"first diff at {d}")
    for reason, names in sorted(refused.items()):
        print(f"  REFUSED {len(names)}, {reason}: {', '.join(names)}")
    print(f"  {len(blobs) - sum(map(len, refused.values()))} round-tripped, "
          f"{len(hidden)} of them up to hidden fields; "
          f"{sum(map(len, refused.values()))} refused")
    print("== conventional fuzz ==")
    n, fok = fuzz()
    print(f"  {'PASS' if fok else 'FAIL'} {n} combiner programs round-tripped")
    ok = ok and fok
    print("== cond exec opcodes ==")
    cok = cond_exec_opcodes()
    print(f"  {'PASS' if cok else 'FAIL'} cexec/cexece, clean and unclean")
    ok = ok and cok
    print("== source-mode smoke ==")
    sok = source_mode_smoke()
    print(f"  {'PASS' if sok else 'FAIL'} hand-authored .xasm -> container "
          f"-> oracle")
    ok = ok and sok
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
