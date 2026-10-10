#!/usr/bin/env python3
"""Convert a title ELF linked with src/xsdk.ld into an unencrypted, uncompressed XEX2."""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

ELF_HEADER = struct.Struct(">16sHHIIIIIHHHHHH")
ELF_SECTION = struct.Struct(">IIIIIIIIII")
ET_EXEC = 2
EM_PPC = 20
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHT_NOBITS = 8

IMAGE_BASE = 0x82000000
PE_HEADER_BYTES = 0x1000
# Image pages are 64 KB unless the image asks for 4 KB (xenia:src/xenia/kernel/util/xex2_info.h:76).
PAGE_BYTES = 0x10000

# xenia:src/xenia/kernel/util/xex2_info.h:55-59
SECTION_CODE = 1
SECTION_DATA = 2
SECTION_READONLY = 3

# xenia:src/xenia/kernel/util/xex2_info.h:278-308
HEADER_FILE_FORMAT_INFO = 0x000003FF
HEADER_ENTRY_POINT = 0x00010100
HEADER_IMAGE_BASE_ADDRESS = 0x00010201
HEADER_IMPORT_LIBRARIES = 0x000103FF
HEADER_TLS_INFO = 0x00020104
HEADER_DEFAULT_STACK_SIZE = 0x00020200
HEADER_SYSTEM_FLAGS = 0x00030000
HEADER_EXECUTION_INFO = 0x00040006

MODULE_TITLE = 0x00000001
SYSTEM_NO_FORCED_REBOOT = 0x00000001
SYSTEM_HANDLE_GAMEPAD_DISCONNECT = 0x00000020
REGION_ALL = 0xFFFFFFFF
MEDIA_ALL = 0xFFFFFFFF
ENCRYPTION_NONE = 0
# Basic compression with one raw block is "not compressed"; homebrew packers emit it rather than type 0.
COMPRESSION_BASIC = 1
TLS_SLOTS = 64

SECURITY_INFO_BYTES = 0x184
PAGE_DESCRIPTOR_BYTES = 0x18
IMAGE_INFO_BYTES = 0x174
HEADER_ALIGN_BYTES = 0x1000

# xenia:third_party/pe/pe_image.h and the section flags at xenia:src/xenia/kernel/util/xex2_info.h:27-34
PE_MACHINE_POWERPCBE = 0x01F2
PE_EXECUTABLE_IMAGE = 0x0002
PE_RELOCS_STRIPPED = 0x0001
PE_32BIT_MACHINE = 0x0100
PE_OPTIONAL_HEADER_BYTES = 224
PE_OPTIONAL_MAGIC = 0x10B
PE_SUBSYSTEM_XBOX = 14
PE_SECTION_FLAGS = {
    SECTION_CODE: 0x60000020,
    SECTION_READONLY: 0x40000040,
    SECTION_DATA: 0xC0000040,
}
PE_SECTION_NAMES = {SECTION_CODE: b".text", SECTION_READONLY: b".rdata", SECTION_DATA: b".data"}

IMPORT_RECORD = struct.Struct(">IIII16s")
RECORD_THUNK = 0x01000000


def version(major, minor, build, qfe):
    return major << 28 | minor << 24 | build << 8 | qfe


# Per-library import IDs; how they're derived is unknown (SynthXEX aee2b8c6 src/setdata/optheaders.c:159-167).
LIBRARY_IDS = {"xboxkrnl.exe": 0x45DC17E0, "xam.xex": 0xFCA15C76}

# The versions a working XDK-built homebrew title carries (XexTool listing on the user's console files).
IMPORT_VERSION = version(2, 0, 21256, 0)
IMPORT_VERSION_MIN = version(2, 0, 1861, 0)


def fail(message):
    sys.exit(f"elf2xex: {message}")


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def sha1(*parts):
    digest = hashlib.sha1()
    for part in parts:
        digest.update(part)
    return digest.digest()


def read_elf(data):
    header = ELF_HEADER.unpack_from(data)
    ident, e_type, machine = header[0], header[1], header[2]
    if ident[:4] != b"\x7fELF" or ident[4] != 1 or ident[5] != 2:
        fail("not a 32-bit big-endian ELF")
    if e_type != ET_EXEC or machine != EM_PPC:
        fail("not a PowerPC executable")
    entry, section_offset = header[4], header[6]
    section_count, names_index = header[12], header[13]

    raw = [ELF_SECTION.unpack_from(data, section_offset + i * ELF_SECTION.size) for i in range(section_count)]
    names_offset = raw[names_index][4]
    sections = []
    for name, kind, flags, address, offset, size, *_ in raw:
        end = data.index(b"\0", names_offset + name)
        sections.append({
            "name": data[names_offset + name:end].decode(),
            "kind": kind,
            "flags": flags,
            "address": address,
            "data": b"" if kind == SHT_NOBITS else data[offset:offset + size],
            "size": size,
        })
    return entry, sections


def section_type(section):
    if section["flags"] & SHF_EXECINSTR:
        return SECTION_CODE
    if section["flags"] & SHF_WRITE:
        return SECTION_DATA
    return SECTION_READONLY


def build_image(sections):
    loaded = [s for s in sections if s["flags"] & SHF_ALLOC and s["size"]]
    for section in loaded:
        if section["address"] < IMAGE_BASE + PE_HEADER_BYTES:
            fail(f"section {section['name']} overlaps the PE headers")
    end = max(s["address"] + s["size"] for s in loaded)
    image = bytearray(align(end - IMAGE_BASE, PAGE_BYTES))
    page_types = [SECTION_READONLY] + [None] * (len(image) // PAGE_BYTES - 1)

    for section in loaded:
        start = section["address"] - IMAGE_BASE
        image[start:start + len(section["data"])] = section["data"]
        first_page = start // PAGE_BYTES
        last_page = (start + section["size"] - 1) // PAGE_BYTES
        for page in range(first_page, last_page + 1):
            if page_types[page] not in (None, section_type(section)):
                fail(f"section {section['name']} shares a 64 KB page with a section of another type")
            page_types[page] = section_type(section)

    runs = []
    for page, kind in enumerate(page_types):
        kind = kind or SECTION_READONLY
        if runs and runs[-1]["type"] == kind:
            runs[-1]["pages"] += 1
        else:
            runs.append({"type": kind, "first_page": page, "pages": 1})
    for run in runs:
        start = IMAGE_BASE + run["first_page"] * PAGE_BYTES
        end = start + run["pages"] * PAGE_BYTES
        inside = [s for s in loaded if start <= s["address"] < end]
        run["start"] = min(s["address"] for s in inside) - IMAGE_BASE if inside else None
        run["end"] = max(s["address"] + s["size"] for s in inside) - IMAGE_BASE if inside else None
        run["initialized"] = sum(len(s["data"]) for s in inside)
        run["uninitialized"] = sum(s["size"] - len(s["data"]) for s in inside)
    return image, runs


def read_imports(sections, image):
    found = [s for s in sections if s["name"] == ".xsdk.imports"]
    if not found:
        return {}
    data = found[0]["data"]
    libraries = {}
    for offset in range(0, len(data), IMPORT_RECORD.size):
        slot, thunk, ordinal, _, library = IMPORT_RECORD.unpack_from(data, offset)
        name = library.rstrip(b"\0").decode()
        libraries.setdefault(name, []).append({"slot": slot, "thunk": thunk, "ordinal": ordinal})
    for name, imports in libraries.items():
        imports.sort(key=lambda entry: entry["ordinal"])
        for entry in imports:
            for address in (entry["slot"], entry["thunk"]):
                word = struct.unpack_from(">I", image, address - IMAGE_BASE)[0]
                if word & 0xFFFF != entry["ordinal"]:
                    fail(f"{name} ordinal {entry['ordinal']:#x}: record at {address:#010x} reads {word:#010x}")
    return dict(sorted(libraries.items()))


def import_libraries_header(libraries):
    names = list(libraries)
    strings = b"".join(align_bytes(name.encode() + b"\0", 4) for name in names)

    records = []
    for index, name in enumerate(names):
        if name not in LIBRARY_IDS:
            fail(f"no import ID known for {name}")
        table = []
        for entry in libraries[name]:
            table.append(entry["slot"])
            if entry["thunk"]:
                table.append(entry["thunk"])
        body = struct.pack(">IIIHH", LIBRARY_IDS[name], IMPORT_VERSION, IMPORT_VERSION_MIN, index, len(table))
        body += struct.pack(f">{len(table)}I", *table)
        records.append(body)

    # Each record's digest covers the next record after its size; the security info's digest covers the first.
    next_digest = bytes(20)
    chained = []
    for body in reversed(records):
        chained.append(struct.pack(">I", 4 + 20 + len(body)) + next_digest + body)
        next_digest = sha1(next_digest, body)
    chained.reverse()

    block = strings + b"".join(chained)
    header = struct.pack(">III", 12 + len(block), len(strings), len(names)) + block
    return header, next_digest


def align_bytes(data, alignment):
    return data + bytes(align(len(data), alignment) - len(data))


def pe_headers(image, runs, entry, stack_bytes):
    sections = [run for run in runs if run["start"] is not None]
    code = [run for run in sections if run["type"] == SECTION_CODE]
    data = [run for run in sections if run["type"] != SECTION_CODE]
    file_header = struct.pack("<HHIIIHH", PE_MACHINE_POWERPCBE, len(sections), 0, 0, 0, PE_OPTIONAL_HEADER_BYTES,
        PE_EXECUTABLE_IMAGE | PE_RELOCS_STRIPPED | PE_32BIT_MACHINE)
    optional = struct.pack("<HBBIIIIIII", PE_OPTIONAL_MAGIC, 0, 0,
        sum(run["initialized"] for run in code),
        sum(run["initialized"] for run in data),
        sum(run["uninitialized"] for run in sections),
        entry - IMAGE_BASE,
        code[0]["start"] if code else 0,
        data[0]["start"] if data else 0,
        IMAGE_BASE)
    optional += struct.pack("<IIHHHHHHIIIIHHIIIIII", PE_HEADER_BYTES, PE_HEADER_BYTES, 0, 0, 0, 0, 0, 0, 0, len(image),
        PE_HEADER_BYTES, 0, PE_SUBSYSTEM_XBOX, 0, stack_bytes, stack_bytes, 0, 0, 0, 16)
    optional += bytes(16 * 8)

    section_headers = b""
    for run in sections:
        size = run["end"] - run["start"]
        section_headers += struct.pack("<8sIIIIIIHHI", PE_SECTION_NAMES[run["type"]], size, run["start"], size,
            run["start"], 0, 0, 0, 0, PE_SECTION_FLAGS[run["type"]])

    dos_header = bytearray(0x40)
    # The loader checks the first four bytes, not just "MZ" (xenia:src/xenia/cpu/xex_module.h:164).
    dos_header[0:4] = b"MZ\x90\x00"
    struct.pack_into("<I", dos_header, 0x3C, len(dos_header))
    headers = bytes(dos_header) + b"PE\0\0" + file_header + optional + section_headers
    if len(headers) > PE_HEADER_BYTES:
        fail("PE headers don't fit in their page")
    image[0:len(headers)] = headers


def page_descriptors(image, runs):
    # Each descriptor's digest covers the next run and its descriptor; the security info's digest covers the first.
    next_digest = bytes(20)
    descriptors = []
    for run in reversed(runs):
        value = struct.pack(">I", run["pages"] << 4 | run["type"])
        start = run["first_page"] * PAGE_BYTES
        descriptor = value + next_digest
        descriptors.append(descriptor)
        next_digest = sha1(image[start:start + run["pages"] * PAGE_BYTES], descriptor)
    descriptors.reverse()
    return b"".join(descriptors), next_digest


def optional_header_table(inline, blocks, data_offset, placed):
    keys = sorted(list(inline) + list(blocks) + list(placed))
    table = b""
    data = b""
    for key in keys:
        if key in inline:
            table += struct.pack(">II", key, inline[key])
        elif key in placed:
            table += struct.pack(">II", key, placed[key])
        else:
            table += struct.pack(">II", key, data_offset + len(data))
            data += align_bytes(blocks[key], 8)
    return table, data


def build_xex(image, runs, entry, libraries, args):
    imports, import_digest = import_libraries_header(libraries)
    descriptors, image_digest = page_descriptors(image, runs)

    inline = {
        HEADER_ENTRY_POINT: entry,
        HEADER_IMAGE_BASE_ADDRESS: IMAGE_BASE,
        HEADER_DEFAULT_STACK_SIZE: args.stack_size,
        HEADER_SYSTEM_FLAGS: SYSTEM_NO_FORCED_REBOOT | SYSTEM_HANDLE_GAMEPAD_DISCONNECT,
    }
    blocks = {
        HEADER_FILE_FORMAT_INFO: struct.pack(">IHHII", 16, ENCRYPTION_NONE, COMPRESSION_BASIC, len(image), 0),
        HEADER_TLS_INFO: struct.pack(">IIII", TLS_SLOTS, 0, 0, 0),
        HEADER_EXECUTION_INFO: struct.pack(">IIIIBBBBI", 0, 0, 0, args.title_id, 0, 0, 1, 1, 0),
    }

    header_count = len(inline) + len(blocks) + 1
    security_offset = align(0x18 + 8 * header_count, 8)
    security_bytes = SECURITY_INFO_BYTES + len(descriptors)
    data_offset = align(security_offset + security_bytes, 8)
    data_bytes = sum(align(len(block), 8) for block in blocks.values())
    header_size = align(data_offset + data_bytes + len(imports), HEADER_ALIGN_BYTES)
    # The import libraries end the headers exactly, as XexTool and SynthXEX (placer.c:72-106) place them.
    import_offset = header_size - len(imports)
    table, data = optional_header_table(inline, blocks, data_offset, {HEADER_IMPORT_LIBRARIES: import_offset})

    security = bytearray(SECURITY_INFO_BYTES)
    struct.pack_into(">II", security, 0, security_bytes, len(image))
    struct.pack_into(">III20sI20s", security, 0x108, IMAGE_INFO_BYTES, 0, IMAGE_BASE, image_digest, len(libraries),
        import_digest)
    struct.pack_into(">III", security, 0x178, REGION_ALL, MEDIA_ALL, len(runs))

    header = bytearray(header_size)
    struct.pack_into(">4sIIIII", header, 0, b"XEX2", MODULE_TITLE, header_size, 0, security_offset, header_count)
    header[0x18:0x18 + len(table)] = table
    header[security_offset:security_offset + SECURITY_INFO_BYTES] = security
    header[security_offset + SECURITY_INFO_BYTES:security_offset + security_bytes] = descriptors
    header[data_offset:data_offset + len(data)] = data
    header[import_offset:] = imports

    # The header digest covers everything the signature doesn't.
    header_digest = sha1(header[security_offset + 0x17C:], header[:security_offset + 8])
    header[security_offset + 0x164:security_offset + 0x178] = header_digest
    return bytes(header) + bytes(image)


def patch_import_records(image, libraries):
    for index, imports in enumerate(libraries.values()):
        for entry in imports:
            for address, kind in ((entry["slot"], 0), (entry["thunk"], RECORD_THUNK)):
                if address:
                    struct.pack_into(">I", image, address - IMAGE_BASE, kind | index << 16 | entry["ordinal"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("xex", type=Path)
    parser.add_argument("--title-id", type=lambda text: int(text, 16), default=0x5853444B)
    parser.add_argument("--stack-size", type=lambda text: int(text, 0), default=1024 * 1024)
    args = parser.parse_args()

    entry, sections = read_elf(args.elf.read_bytes())
    image, runs = build_image(sections)
    libraries = read_imports(sections, image)
    patch_import_records(image, libraries)
    pe_headers(image, runs, entry, args.stack_size)
    args.xex.write_bytes(build_xex(image, runs, entry, libraries, args))


if __name__ == "__main__":
    main()
