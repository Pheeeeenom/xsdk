#!/usr/bin/env python3
"""Minimal SPIR-V binary reader for the xsc GLSL->Xenos translator.

Parses the module structure we need to lower a branch-free shader: header,
the instruction stream, the type/constant table, decorations, interface
variables, and the single function body. No external grammar dependency —
operand layouts for the subset of opcodes we lower are known here; any other
opcode is retained as raw words so the stream always round-trips.

SPIR-V spec: 32-bit little-endian words. Header is 5 words
(magic 0x07230203, version, generator, id-bound, schema=0). Each instruction
word 0 = (word_count << 16) | opcode; remaining (word_count-1) words are
operands. Result-type / result-id presence is per-opcode.
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = 0x07230203

# Opcodes we name / interpret (core SPIR-V; numbers are stable spec values).
OP = {
    0: "Nop", 3: "Source", 5: "Name", 6: "MemberName",
    11: "ExtInstImport", 12: "ExtInst", 14: "MemoryModel",
    15: "EntryPoint", 16: "ExecutionMode", 17: "Capability",
    19: "TypeVoid", 20: "TypeBool", 21: "TypeInt", 22: "TypeFloat",
    23: "TypeVector", 24: "TypeMatrix", 25: "TypeImage",
    26: "TypeSampler", 27: "TypeSampledImage", 28: "TypeArray",
    30: "TypeStruct", 32: "TypePointer", 33: "TypeFunction",
    43: "Constant", 44: "ConstantComposite",
    54: "Function", 55: "FunctionParameter", 56: "FunctionEnd",
    57: "FunctionCall", 59: "Variable", 61: "Load", 62: "Store",
    65: "AccessChain", 71: "Decorate", 72: "MemberDecorate",
    79: "VectorShuffle", 80: "CompositeConstruct", 81: "CompositeExtract",
    82: "CompositeInsert", 84: "Transpose",
    87: "ImageSampleImplicitLod", 88: "ImageSampleExplicitLod",
    91: "ImageSampleProjImplicitLod", 92: "ImageSampleProjExplicitLod",
    110: "ConvertFToS", 111: "ConvertSToF",
    124: "Bitcast", 127: "FNegate", 128: "IAdd", 129: "FAdd", 130: "ISub",
    131: "FSub", 132: "IMul", 133: "FMul", 134: "UDiv", 135: "SDiv",
    136: "FDiv", 137: "UMod", 138: "SRem", 139: "SMod",
    142: "VectorTimesScalar",
    144: "VectorTimesMatrix", 145: "MatrixTimesVector",
    146: "MatrixTimesMatrix", 148: "Dot",
    154: "Any", 155: "All",
    166: "LogicalOr", 167: "LogicalAnd", 168: "LogicalNot",
    169: "Select",
    170: "IEqual", 171: "INotEqual", 172: "UGreaterThan",
    173: "SGreaterThan", 174: "UGreaterThanEqual", 175: "SGreaterThanEqual",
    176: "ULessThan", 177: "SLessThan", 178: "ULessThanEqual",
    179: "SLessThanEqual",
    180: "FOrdEqual", 182: "FOrdNotEqual", 184: "FOrdLessThan",
    186: "FOrdGreaterThan", 188: "FOrdLessThanEqual",
    190: "FOrdGreaterThanEqual",
    194: "ShiftRightLogical", 195: "ShiftRightArithmetic",
    196: "ShiftLeftLogical", 197: "BitwiseOr", 198: "BitwiseXor",
    199: "BitwiseAnd", 200: "Not",
    207: "DPdx", 208: "DPdy", 209: "Fwidth",
    246: "LoopMerge", 247: "SelectionMerge",
    248: "Label", 249: "Branch", 250: "BranchConditional",
    252: "Kill", 253: "Return", 254: "ReturnValue",
}

# Opcodes that carry (result_type_id, result_id) as their first two operands:
# value computations + Constant/Variable/Function (NOT type-defining ops,
# whose result *is* the type and have no result-type word).
_HAS_TYPE_AND_RESULT = {
    12, 43, 44, 54, 55, 57, 59, 61, 65, 79, 80, 81, 82, 84, 87, 88, 91, 92,
    124, 127,
    129, 130, 131, 133, 136, 141, 142, 144, 145, 146, 148,
    169, 180, 182, 184, 186, 188, 190,        # OpSelect + FOrd comparisons
    110, 111, 128, 132, 134, 135, 137, 138, 139,  # int arith/div/mod + convert
    154, 155, 166, 167, 168,                  # any/all + logical and/or/not
    170, 171, 172, 173, 174, 175, 176, 177, 178, 179,  # int comparisons
    194, 195, 196, 197, 198, 199, 200,        # shifts + bitwise (rejected)
    207, 208, 209,                            # derivatives (rejected for now)
}
# Opcodes that carry only result_id (no result type): every Type* op plus
# ExtInstImport and Label.
_HAS_RESULT_ONLY = {11, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 32,
                    33, 248}


@dataclass
class Instr:
    opcode: int
    words: tuple            # operand words (after the opcode word)
    result_type: int | None = None
    result_id: int | None = None

    @property
    def name(self) -> str:
        return OP.get(self.opcode, f"Op{self.opcode}")

    @property
    def operands(self) -> tuple:
        """Operand words excluding result-type/result-id."""
        skip = (2 if self.opcode in _HAS_TYPE_AND_RESULT else
                1 if self.opcode in _HAS_RESULT_ONLY else 0)
        return self.words[skip:]


@dataclass
class Module:
    version: int
    generator: int
    bound: int
    instrs: list = field(default_factory=list)

    # convenience indexes (filled by _index)
    names: dict = field(default_factory=dict)        # id -> str
    member_names: dict = field(default_factory=dict)  # (id,idx) -> str
    decorations: dict = field(default_factory=dict)   # id -> list[(deco,args)]
    member_decorations: dict = field(default_factory=dict)
    entry_points: list = field(default_factory=list)  # (model,id,name,iface)
    by_result: dict = field(default_factory=dict)     # id -> Instr

    def _index(self):
        for ins in self.instrs:
            if ins.result_id is not None:
                self.by_result[ins.result_id] = ins
            op = ins.opcode
            if op == 5:   # Name <id> "str"
                tid = ins.words[0]
                self.names[tid] = _decode_string(ins.words[1:])
            elif op == 6:  # MemberName <id> <member> "str"
                self.member_names[(ins.words[0], ins.words[1])] = \
                    _decode_string(ins.words[2:])
            elif op == 71:  # Decorate <id> <deco> args...
                self.decorations.setdefault(ins.words[0], []).append(
                    (ins.words[1], tuple(ins.words[2:])))
            elif op == 72:  # MemberDecorate <id> <member> <deco> args...
                self.member_decorations.setdefault(
                    (ins.words[0], ins.words[1]), []).append(
                    (ins.words[2], tuple(ins.words[3:])))
            elif op == 15:  # EntryPoint <model> <id> "name" iface...
                name, rest = _decode_string_at(ins.words, 2)
                self.entry_points.append(
                    (ins.words[0], ins.words[1], name, tuple(rest)))

    @staticmethod
    def parse(data: bytes) -> "Module":
        if len(data) < 20:
            raise ValueError("too short for a SPIR-V header")
        magic, version, generator, bound, schema = struct.unpack_from(
            "<5I", data, 0)
        if magic != MAGIC:
            # tolerate big-endian modules by byteswapping
            magic_be = struct.unpack_from(">I", data, 0)[0]
            if magic_be == MAGIC:
                words = struct.unpack(f">{len(data)//4}I", data)
                data = struct.pack(f"<{len(words)}I", *words)
                magic, version, generator, bound, schema = \
                    struct.unpack_from("<5I", data, 0)
            else:
                raise ValueError(f"bad SPIR-V magic {magic:#010x}")
        words = struct.unpack(f"<{len(data)//4}I", data)
        m = Module(version, generator, bound)
        i = 5
        while i < len(words):
            head = words[i]
            wc = head >> 16
            opcode = head & 0xFFFF
            if wc == 0:
                raise ValueError(f"zero word-count at {i}")
            ops = words[i + 1:i + wc]
            rt = rid = None
            if opcode in _HAS_TYPE_AND_RESULT:
                rt, rid = ops[0], ops[1]
            elif opcode in _HAS_RESULT_ONLY:
                rid = ops[0]
            m.instrs.append(Instr(opcode, ops, rt, rid))
            i += wc
        m._index()
        return m


def _decode_string(words) -> str:
    raw = struct.pack(f"<{len(words)}I", *words)
    end = raw.find(b"\0")
    return raw[:end if end >= 0 else len(raw)].decode("utf-8", "replace")


def _decode_string_at(words, start) -> tuple:
    """Decode a NUL-terminated string starting at word `start`; return
    (string, remaining_words_after_it)."""
    buf = bytearray()
    i = start
    done = False
    while i < len(words) and not done:
        for b in struct.pack("<I", words[i]):
            if b == 0:
                done = True
                break
            buf.append(b)
        i += 1
    return buf.decode("utf-8", "replace"), words[i:]


def main():
    for path in sys.argv[1:]:
        m = Module.parse(Path(path).read_bytes())
        print(f"=== {path}  (SPIR-V {m.version >> 16 & 0xFF}."
              f"{m.version >> 8 & 0xFF}, {len(m.instrs)} instrs, "
              f"id-bound {m.bound})")
        for ep in m.entry_points:
            models = {0: "Vertex", 4: "Fragment"}
            print(f"  EntryPoint {models.get(ep[0], ep[0])} '{ep[2]}' "
                  f"iface={list(ep[3])}")
        print("  -- function body --")
        in_fn = False
        for ins in m.instrs:
            if ins.opcode == 54:
                in_fn = True
            if in_fn and ins.opcode not in (54, 248, 56):
                rid = f"%{ins.result_id} = " if ins.result_id else ""
                print(f"    {rid}{ins.name} {list(ins.operands)}")
            if ins.opcode == 56:
                in_fn = False


if __name__ == "__main__":
    main()
