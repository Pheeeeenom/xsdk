#!/usr/bin/env python3
"""xsc lowers Vulkan's gl_InstanceIndex to a float constant the runtime writes each instance's index into, as
vulkan4xenon's milestone 24 draws an instance at a time.

- vk_instance_index.vert reads it past a push block and a uniform block: the index takes the register past the
  uniform block's groups of 4, so writing it never sets a group the GPU loads, and the literals come after it. Its
  column is the index mod 4, its row the index over 4, its depth the index itself, and its colour a uniform array's
  element at the index. Run on the simulator for each index 0 to 7, with the index in every component of its register
  as the runtime writes it, each must give its own position and colour; the oracle must accept it.
- vk_instance_alone.vert has no block, so the index takes register 0 and the literals the ones after.
- Both name the register in the container's constant table, and the compiler lists it among the resources.
- vk_vertex_index.vert reads gl_VertexIndex, which xsc doesn't lower: it must be refused, where it used to be fetched
  as the position.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from test_xsc_lower import TD, oracle_hlsl  # noqa: E402

OPOS = int(xu.ExportRegister.kVSPosition)
POSITION = [0.5, -0.25, 0.125, 1.0]
SCALE = [2.0, 4.0, 1.0, 1.0]
COLORS = [[0.125 * k, 1.0 - 0.125 * k, 0.5, 1.0] for k in range(8)]
# The console loads constants in groups of 4 (xsdk:docs/HARDWARE.md, "Constants loaded from memory go in groups of 4
# too").
CONSTANT_GROUP = 4


def compiled(name):
    return native.compile_spirv((TD / f"{name}.spv").read_bytes(), XSDK_REGISTERS.pixel, XSDK_REGISTERS.vertex)


def literal_registers(manifest):
    return {reg: list(values) for reg, values in manifest}


def close(a, b):
    return len(a) == len(b) and all(abs(x - y) <= 1e-5 for x, y in zip(a, b))


def run(blob, constants):
    """The program's exports for one instance, every vertex fetch reading POSITION."""
    return simulate.run(blob, registers={0: [0.0] * 4}, constants=constants,
                        vertex_fetch=lambda n, fetch, vertex: POSITION).exports


def check_past_blocks():
    name = "vk_instance_index.vert"
    blob, manifest, resources = compiled(name)
    instance = [r for r in resources if r[0] == "instance"]
    blocks = [r for r in resources if r[0] in ("push", "uniform")]
    uniform_end = max(reg + regs for kind, _, _, reg, regs in blocks if kind == "uniform")
    if len(instance) != 1 or instance[0][3] != uniform_end or uniform_end % CONSTANT_GROUP:
        print(f"  FAIL {name}: resources {resources}: want the instance index alone, at the register past the "
              f"uniform block's groups of 4")
        return False
    reg = instance[0][3]
    literals = literal_registers(manifest)
    if any(r <= reg for r in literals):
        print(f"  FAIL {name}: literals at {sorted(literals)}, not all past the instance index's c{reg}")
        return False
    for index in range(8):
        constants = dict(literals)
        constants[0] = SCALE
        constants.update({4 + k: color for k, color in enumerate(COLORS)})
        constants[reg] = [float(index)] * 4
        exports = run(blob, constants)
        want = [POSITION[0] * SCALE[0] + 0.5 * (index % 4), POSITION[1] * SCALE[1] + 0.5 * (index // 4),
                POSITION[2] * SCALE[2] + index, POSITION[3] * SCALE[3]]
        if not close(exports.get(OPOS, []), want) or not close(exports.get(0, []), COLORS[index]):
            print(f"  FAIL {name}: instance {index}: position {exports.get(OPOS)} (want {want}), colour "
                  f"{exports.get(0)} (want {COLORS[index]})")
            return False
    if oracle_hlsl(blob, "vk_instance_index_vert") is None:
        print(f"  FAIL {name}: XenosRecomp rejected it")
        return False
    if b"gl_InstanceIndex" not in blob:
        print(f"  FAIL {name}: the constant table doesn't name gl_InstanceIndex")
        return False
    print(f"  PASS {name}: the index at c{reg}, past the blocks, literals after it; instances 0 to 7 each placed and "
          f"coloured by it ({len(blob)} B)")
    return True


def check_alone():
    name = "vk_instance_alone.vert"
    blob, manifest, resources = compiled(name)
    literals = literal_registers(manifest)
    if resources != [("instance", 0, 0, 0, 1)] or any(r == 0 for r in literals) or b"gl_InstanceIndex" not in blob:
        print(f"  FAIL {name}: resources {resources}, literals at {sorted(literals)}: want the index alone at c0")
        return False
    for index in (0, 3, 6):
        constants = dict(literals)
        constants[0] = [float(index)] * 4
        exports = run(blob, constants)
        want = [POSITION[0] + 0.25 * index, POSITION[1], 0.0, 1.0]
        if not close(exports.get(OPOS, []), want):
            print(f"  FAIL {name}: instance {index}: position {exports.get(OPOS)} (want {want})")
            return False
    print(f"  PASS {name}: the index at c0, literals after it, instances 0, 3 and 6 placed by it ({len(blob)} B)")
    return True


def check_vertex_index():
    name = "vk_vertex_index.vert"
    try:
        compiled(name)
    except native.CompileError as error:
        if error.status != native.REFUSED:
            print(f"  FAIL {name}: failed as {native.STATUS_NAMES[error.status]}, not refused: {error}")
            return False
        print(f"  PASS {name}: refused ({error})")
        return True
    print(f"  FAIL {name}: lowered, gl_VertexIndex fetched as a vertex input")
    return False


def main():
    results = [check_past_blocks(), check_alone(), check_vertex_index()]
    if not all(results):
        raise SystemExit("FAILED")
    print("OK")


if __name__ == "__main__":
    main()
