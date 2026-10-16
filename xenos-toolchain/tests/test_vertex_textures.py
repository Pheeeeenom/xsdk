#!/usr/bin/env python3
"""xsc samples textures in vertex shaders.

A vertex shader's sample reached the lowering ungathered, and xsc gave up. Now its samples go after the vertex fetches,
before the exports, as a pixel shader's go before its colours, at the explicit LOD glslang gives texture() in a vertex
shader, or one the shader works out; a vertex shader computes no LOD
(xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:639-641).

- vk_vertex_texture.vert's two fetches compute no LOD and fetch for every vertex (FetchValidOnly off), the first at LOD
  0, the second at the register LOD; the container declares both samplers.
- The fetch reading the coordinate a vertex fetch wrote in its exec is serialized, and the exec's vertex cache mask
  covers its vertex fetches, which lead it, and none of its texture fetches.
- On the simulator the height moves the position up and the colour comes from the LOD the vertex works out.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402
from test_local_stores import check, module  # noqa: E402

NAME = "vk_vertex_texture.vert"
OPOS = int(xu.ExportRegister.kVSPosition)
VERTEX = 3.0
POSITION = [0.25, -0.5, 0.75, 1.0]
UV = [0.375, 0.625, 0.0, 0.0]
HEIGHT = 0.125
EXEC_OPCODES = (1, 2)
FETCH_OPCODE_MASK = 0x1F


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def execs(words):
    """Each exec's instructions as (whether it's a fetch, whether it's a vertex fetch), and its vc bits."""
    out = []
    for slot in range(0, len(words) - 2, 3):
        a, b, c = words[slot:slot + 3]
        for low, high in ((a, b & 0xFFFF), (b >> 16 | (c & 0xFFFF) << 16, c >> 16)):
            if high >> 12 & 0xF not in EXEC_OPCODES:
                continue
            address, count, sequence = low & 0xFFF, low >> 12 & 0x7, low >> 16 & 0xFFF
            kinds = []
            for k in range(count):
                fetch = bool(sequence >> (2 * k) & 1)
                kinds.append((fetch, fetch and words[(address + k) * 3] & FETCH_OPCODE_MASK == 0))
            out.append((kinds, (low >> 28 & 0xF) | (high & 0x3) << 4))
    return out


def fields():
    blob, manifest = lower_ex(module(NAME), XSDK_REGISTERS)
    c = container.Container(blob)
    fetches = [xu.TextureFetchInstruction(*r.raw) for r in xu._decode_program(c.ucode_dwords, c.shader_type)[0]
               if r.kind == "tfetch"]
    samples = [f for f in fetches if f.opcode() == int(xu.FetchOpcode.kTextureFetch)]
    if len(samples) != 2 or any(f.use_computed_lod() or f.fetch_valid_only() for f in samples):
        fail(f"{NAME}: its fetches compute a LOD or fetch for valid pixels only")
    if samples[0].use_register_lod() == samples[1].use_register_lod():
        fail(f"{NAME}: one fetch should read the register LOD, the other not")
    print(f"  PASS {NAME}: two fetches, no LOD computed, every vertex fetched, one at the register LOD")
    names = [text for _, text in c.strings(0, len(blob)) if text.startswith("tex")]
    if names[:2] != ["tex0", "tex1"]:
        fail(f"{NAME}: the container declares {names}, not tex0 and tex1")
    print(f"  PASS {NAME}: the container declares tex0 and tex1")
    for kinds, vc in execs(c.ucode_dwords):
        vertex = sum(v for _, v in kinds)
        if kinds[:vertex] != [(True, True)] * vertex or vc != (1 << vertex) - 1:
            fail(f"{NAME}: an exec of {kinds} has vc {vc:#x}")
    print(f"  PASS {NAME}: each exec's vertex cache mask covers its leading vertex fetches alone")
    return blob, manifest


def serialized(blob):
    c = container.Container(blob)
    records = [r for r in xu._decode_program(c.ucode_dwords, c.shader_type)[0] if r.kind != "cf"]
    first = next(r for r in records if r.kind == "tfetch")
    if not first.serialize:
        fail(f"{NAME}: its first texture fetch, reading what a vertex fetch wrote, isn't serialized")
    print(f"  PASS {NAME}: its texture fetch reading a vertex fetch's result is serialized")


def simulated(blob, manifest):
    slots = {v.binding: slot for v, slot in resolve_interface(module(NAME)).samplers}
    constants = {r: list(v) for r, v in manifest}

    def vertex_fetch(n, fetch, index):
        # The texture coordinate is fetched first, the position, into r0, last.
        return UV if n == 0 else POSITION

    def sample(slot, coordinates, lod):
        if lod.computed or lod.gradients:
            fail(f"{NAME}: a fetch computes its LOD")
        if slot == slots[0]:
            return [HEIGHT + lod.bias + (lod.register or 0.0), 0.0, 0.0, 1.0]
        return [lod.register, coordinates[0], coordinates[1], 1.0]

    result = simulate.run(blob, registers={0: [VERTEX, 0.0, 0.0, 0.0]}, constants=constants,
                          vertex_fetch=vertex_fetch, texture_lod=sample)
    check(f"{NAME}, its position moved up by the height", result.exports[OPOS],
          [POSITION[0], POSITION[1] + HEIGHT, POSITION[2], POSITION[3]])
    check(f"{NAME}, its colour at the LOD it works out", result.exports[0], [POSITION[2] * 4.0, UV[0], UV[1], 1.0])


def main():
    blob, manifest = fields()
    serialized(blob)
    simulated(blob, manifest)
    print("OK")


if __name__ == "__main__":
    main()
