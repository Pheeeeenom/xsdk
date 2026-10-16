#!/usr/bin/env python3
"""xsc serializes an instruction that would read across the ALU and the fetches within an exec.

Within an exec the console's ALU and fetches run apart: an ALU instruction that read a fetch's result in the same
exec, unserialized, read the temp before the fetch wrote it, and Aurora's minimap, a palette lookup at a fetched
index, came out as noise. The next exec found it written. Xenia runs each instruction to its end in turn
(xsdk:docs/HARDWARE.md, "Serialized fetches").

- In every shader of xsc's testdata, and vk_dependent_fetch.frag, an instruction is serialized exactly when, since
  its exec's last serialized instruction, it reads or writes a temp a fetch there wrote (an ALU instruction), or reads
  one an ALU instruction there wrote (a fetch).
- vk_dependent_fetch.frag has serialized ALU and fetch instructions both, and the simulator gives what the GLSL says.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from test_local_stores import UV, check, module, push_constants  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402
from test_xsc_lower import TD  # noqa: E402

SCALE = [1.0 / 204.0, 0.0, 0.5 / 204.0, 0.5]
INDEX = 188
TEMP = re.compile(r"\br(\d+)\b")


def instructions(blob):
    """Each ALU or fetch instruction: its exec, whether it's a fetch, its serialize bit, the temps it writes and
    reads."""
    c = container.Container(blob)
    out = []
    for r in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if r.kind not in ("alu", "tfetch", "vfetch"):
            continue
        # "op dest, sources" for each of the line's operations; an export's dest is no temp.
        writes, reads = set(), set()
        for part in r.text.split("+"):
            m = re.search(r"\b[a-zA-Z0-9_]+\s+([^,\s]+)\s*,(.*)", part)
            if not m:
                # setTexLOD and the gradients' setters name their source alone.
                if r.kind == "tfetch":
                    reads |= {int(t) for t in TEMP.findall(part)}
                continue
            writes |= {int(t) for t in TEMP.findall(m.group(1))}
            reads |= {int(t) for t in TEMP.findall(m.group(2))}
        out.append((r.exec_cf_index, r.kind != "alu", r.serialize, writes, reads))
    return out


def hazards(blob):
    """Each instruction's serialize bit, and whether the rule wants one."""
    result = []
    written, current = {}, None
    for exec_index, fetch, serialize, writes, reads in instructions(blob):
        if exec_index != current:
            written, current = {}, exec_index
        other = "alu" if fetch else "fetch"
        touched = reads if fetch else reads | writes
        wants = any(written.get(t) == other or written.get(t) == "both" for t in touched)
        result.append((serialize, wants))
        if wants:
            written = {}
        for t in writes:
            mine = "fetch" if fetch else "alu"
            written[t] = mine if written.get(t) in (None, mine) else "both"
    return result


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def main():
    checked = 0
    for spv in sorted(TD.glob("*.spv")):
        try:
            blob, _ = lower_ex(spirv.Module.parse(spv.read_bytes()), XSDK_REGISTERS)
        except LowerError:
            continue
        for i, (serialize, wants) in enumerate(hazards(blob)):
            if serialize != wants:
                fail(f"{spv.name}: instruction {i} serialized {serialize}, the rule says {wants}")
        checked += 1
    print(f"  PASS {checked} shaders: serialized exactly where an exec reads across the ALU and the fetches")

    blob, manifest = lower_ex(module("vk_dependent_fetch.frag"), XSDK_REGISTERS)
    kinds = [(fetch, serialize) for _, fetch, serialize, _, _ in instructions(blob)]
    if not any(s for f, s in kinds if f) or not any(s for f, s in kinds if not f):
        fail(f"vk_dependent_fetch.frag: serialized fetches and ALU {kinds}; want both kinds")
    print("  PASS vk_dependent_fetch.frag: serialized fetches and ALU instructions both")

    interface = resolve_interface(module("vk_dependent_fetch.frag"))
    slots = {slot: (var.descriptor_set, var.binding) for var, slot in interface.samplers}

    def fetch(slot, coordinates):
        if slots[slot] == (0, 0):
            return [INDEX / 255.0] * 4
        return [coordinates[0], coordinates[1], 0.0, 1.0]

    constants = push_constants("vk_dependent_fetch.frag", [SCALE])
    constants.update({r: list(v) for r, v in manifest})
    result = simulate.run(blob, registers={0: UV + [0.0, 0.0]}, constants=constants, texture_fetch=fetch)
    lookup = [(INDEX + 0.5) / 204.0, 0.5]
    check("vk_dependent_fetch.frag, the palette at the fetched index", result.exports[0],
          [lookup[0], lookup[1], 0.0, 1.0])
    print("OK")


if __name__ == "__main__":
    main()
