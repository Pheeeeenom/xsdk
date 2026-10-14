"""Compares the console's shader cache with Dusklight's pack, built on the PC: each program's SPIR-V, which glslang
compiled on the console, word for word; and each vulkan4xenon file's container, literals and resources, which xost
compiled there, with the pack's bundle. Usage: compare_cache.py <cache dir> <glsl_pack.c>."""

import re
import struct
import sys
from pathlib import Path

ARRAY = re.compile(r"(?:static )?const (uint32_t|V4xShaderLiteral|V4xShaderResource) (glsl_\w+)\[\] = \{(.*?)\};", re.S)
ENTRY = re.compile(r"\{ 0x([0-9A-F]+)ull, (\d+), (glsl_\w+)_container, (\d+), (?:(glsl_\w+)_literals|NULL), (\d+), "
                   r"(?:(glsl_\w+)_resources|NULL), (\d+) \}")
HEADER = struct.Struct(">IIQIIII")


def numbers(text):
    return [float(n) if "." in n or "e" in n.lower() else int(n, 0)
            for n in re.findall(r"-?[0-9][0-9A-Fa-fx.eE+-]*", text.replace("f", ""))]


def main():
    cache, pack = Path(sys.argv[1]), Path(sys.argv[2]).read_text()
    arrays = {name: (kind, body) for kind, name, body in ARRAY.findall(pack)}
    bundle = {}
    for m in ENTRY.finditer(pack):
        bundle[(int(m.group(1), 16), int(m.group(2)))] = m.group(3), int(m.group(4)), int(m.group(6)), int(m.group(8))

    same_spirv = 0
    spv = sorted((cache / "glslang-16.3.0").glob("*.spv"))
    for path in spv:
        stem = path.name[:-len(".spv")].replace(".", "_")
        words = list(struct.unpack(f">{path.stat().st_size // 4}I", path.read_bytes()))
        name = f"glsl_{stem}_spirv"
        if name not in arrays:
            print(f"{path.name}: not in the pack")
            continue
        packed = [int(n, 16) for n in re.findall(r"0x([0-9A-F]{8})", arrays[name][1])]
        if words == packed:
            same_spirv += 1
        else:
            print(f"{path.name}: SPIR-V differs from the pack's")
    print(f"SPIR-V: {same_spirv} of {len(spv)} as the pack's")

    same_container = 0
    files = sorted((cache / "xsc-82ec469a70a08b29").glob("*.v4xs"))
    for path in files:
        data = path.read_bytes()
        magic, layout, spirv_hash, spirv_size, container_bytes, literal_count, resource_count = HEADER.unpack_from(data)
        key = (spirv_hash, spirv_size)
        if key not in bundle:
            print(f"{path.name}: not in the bundle")
            continue
        stem, packed_bytes, packed_literals, packed_resources = bundle[key]
        container = data[HEADER.size:HEADER.size + container_bytes]
        packed = b"".join(struct.pack(">I", int(n, 16))
                          for n in re.findall(r"0x([0-9A-F]{8})", arrays[f"{stem}_container"][1]))[:packed_bytes]
        if (container == packed and literal_count == packed_literals and resource_count == packed_resources):
            same_container += 1
        else:
            print(f"{path.name}: container {container == packed}, literals {literal_count}/{packed_literals}, "
                  f"resources {resource_count}/{packed_resources}")
    print(f"Containers: {same_container} of {len(files)} as the bundle's, with as many literals and resources")


main()
