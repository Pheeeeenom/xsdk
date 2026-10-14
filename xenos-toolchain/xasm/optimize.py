#!/usr/bin/env python3
"""Optimisation of an xsc program's instructions before register allocation.

xsc lowers SPIR-V to Xenos instructions one value at a time, so its programs move values around, recompute what they
already have, keep multiplies apart from the adds that follow them and give scalar operations a whole instruction each.
On the console the pixel shaders' ALU work is most of a frame's GPU time (vulkan4xenon's Dusklight runs), so this pass
rewrites the instruction list xsc built, before the allocator packs its registers:

  - common subexpressions: an instruction computing what an earlier one already holds becomes a move of it;
  - copy propagation: a move's readers read its source instead, and the move goes;
  - mul + add -> mad, where the product has no other reader;
  - dead code: instructions whose results nothing reads go, and write masks shrink to what's read;
  - co-issue: a scalar operation joins a vector operation's instruction, as each ALU instruction has both pipes.

Every rewrite is checked per component against the program's data flow. The semantics are the hardware's as Xenia
models them (xenia:src/xenia/gpu/shader_interpreter.cc:287-957, ucode.cc:16-117): a vector op reads its operands'
swizzled lanes, a scalar op reads lanes w (a) and x (b) of the third, both pipes read before either writes, and an
export's overlapping masks mean the constant 1.

Programs it leaves alone, so they compile exactly as before: hardware loops, predication, anything reading the
previous scalar result, relative addressing of a temp or of a constant, and programs whose sources aren't xsc's whole
registers.
"""
from __future__ import annotations

import bisect
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

import xdis.xenos_ucode as xu  # noqa: E402

V = xu.AluVectorOpcode
S = xu.AluScalarOpcode

ALL = 0xF
OUTSIDE = -1                     # a value from before the program: an input, or a register nothing has written

# The lanes each vector op reads of its operands (xenia:src/xenia/gpu/ucode.cc:83-117). Element-wise ops read only the
# lanes they write.
_VECTOR_READS = {
    V.kAdd: (ALL, ALL), V.kMul: (ALL, ALL), V.kMax: (ALL, ALL), V.kMin: (ALL, ALL),
    V.kSeq: (ALL, ALL), V.kSgt: (ALL, ALL), V.kSge: (ALL, ALL), V.kSne: (ALL, ALL),
    V.kFrc: (ALL,), V.kTrunc: (ALL,), V.kFloor: (ALL,),
    V.kMad: (ALL, ALL, ALL), V.kCndEq: (ALL, ALL, ALL), V.kCndGe: (ALL, ALL, ALL), V.kCndGt: (ALL, ALL, ALL),
    V.kDp4: (ALL, ALL), V.kDp3: (0x7, 0x7), V.kDp2Add: (0x3, 0x3, 0x1), V.kCube: (ALL, ALL), V.kMax4: (ALL,),
    V.kDst: (0x6, 0xA),
}
_ELEMENTWISE = {V.kAdd, V.kMul, V.kMax, V.kMin, V.kSeq, V.kSgt, V.kSge, V.kSne, V.kFrc, V.kTrunc, V.kFloor,
                V.kMad, V.kCndEq, V.kCndGe, V.kCndGt}
# Vector ops that change state besides their result: never touched.
_VECTOR_STATE = {V.kSetpEqPush, V.kSetpNePush, V.kSetpGtPush, V.kSetpGePush, V.kKillEq, V.kKillGt, V.kKillGe,
                 V.kKillNe, V.kMaxA}

# Scalar ops with one operand, and whether it's two components (a and b) or one (a) (ucode.cc:16-80).
_SCALAR_ONE = {
    S.kAdds: True, S.kMuls: True, S.kMaxs: True, S.kMins: True, S.kSubs: True,
    S.kSeqs: False, S.kSgts: False, S.kSges: False, S.kSnes: False, S.kFrcs: False, S.kTruncs: False,
    S.kFloors: False, S.kExp: False, S.kLogc: False, S.kLog: False, S.kRcpc: False, S.kRcpf: False, S.kRcp: False,
    S.kRsqc: False, S.kRsqf: False, S.kRsq: False, S.kSqrt: False, S.kSin: False, S.kCos: False,
}
# Scalar ops that read the previous scalar result: a program with one is left alone, as the passes may remove or
# reorder scalar ops.
_SCALAR_PREVIOUS = {S.kAddsPrev, S.kMulsPrev, S.kMulsPrev2, S.kSubsPrev}

# At most this many different constants in an instruction. The hardware takes three, with only their addressing
# modes constrained (xenia:src/xenia/gpu/ucode.h:1945-1960); xsc itself uses one.
MAX_CONSTANTS = 2

_REFUSED_FLAGS = ("is_vector_dest_relative", "is_const_0_addressed", "is_const_1_addressed",
                  "is_const_address_register_relative", "src1_const_rel", "src2_const_rel", "src3_const_rel",
                  "is_predicated", "abs_constants")


def swizzle_components(swizzle: int) -> list:
    """An ALU source's 8-bit swizzle -> the component each lane reads."""
    return [((swizzle >> (2 * lane)) + lane) & 3 for lane in range(4)]


def encode_swizzle(components) -> int:
    return sum(((c - lane) & 3) << (2 * lane) for lane, c in enumerate(components))


def _lanes(mask: int):
    return [lane for lane in range(4) if mask >> lane & 1]


class _Source:
    """One of an instruction's three operands: a temp (whole register, possibly its absolute value) or a constant."""

    __slots__ = ("temp", "reg", "absolute", "components", "negate")

    def __init__(self, temp, reg, absolute, components, negate):
        self.temp, self.reg, self.absolute = temp, reg, absolute
        self.components, self.negate = list(components), negate

    def same_value(self, other) -> bool:
        return (self.temp, self.reg, self.absolute, self.components, self.negate) == \
            (other.temp, other.reg, other.absolute, other.components, other.negate)


class _Flow:
    """Which instruction's write each (register, component) holds at each instruction, and each write's readers."""

    def __init__(self, opt, instrs):
        self.instrs = instrs
        self.writes = {}                       # key -> sorted indices of the instructions writing it
        self.readers = {}                      # (writer index, key) -> indices of the instructions reading that write
        self.reads = [opt.temp_reads(ins) for ins in instrs]
        self.defs = [opt.defs(ins) for ins in instrs]
        for i, keys in enumerate(self.defs):
            for key in keys:
                self.writes.setdefault(key, []).append(i)
        for i, keys in enumerate(self.reads):
            for key in keys:
                self.readers.setdefault((self.at(i, key), key), []).append(i)

    def at(self, i, key):
        """The write of key that instruction i reads (OUTSIDE if none)."""
        writes = self.writes.get(key)
        if not writes:
            return OUTSIDE
        k = bisect.bisect_left(writes, i)
        return writes[k - 1] if k else OUTSIDE


class _Optimizer:
    def __init__(self, program, abs_flag: int):
        self.p = program
        self.abs_flag = abs_flag

    # ---- an instruction's fields ----

    def source(self, ins, n) -> _Source:
        f = ins.fields
        reg = f.get(f"src{n}_reg", 0)
        temp = bool(f.get(f"src{n}_is_temp", True))
        absolute = temp and bool(reg & self.abs_flag)
        if temp:
            reg &= self.abs_flag - 1
        return _Source(temp, reg, absolute, swizzle_components(f.get(f"src{n}_swizzle", 0)),
                       bool(f.get(f"src{n}_negate", False)))

    def set_source(self, ins, n, src: _Source):
        f = ins.fields
        f[f"src{n}_reg"] = src.reg | (self.abs_flag if src.absolute else 0)
        f[f"src{n}_is_temp"] = src.temp
        f[f"src{n}_swizzle"] = encode_swizzle(src.components)
        f[f"src{n}_negate"] = src.negate

    @staticmethod
    def vector_op(ins):
        return V(ins.fields.get("vector_opcode", V.kMax))

    @staticmethod
    def scalar_op(ins):
        return S(ins.fields.get("scalar_opcode", S.kRetainPrev))

    @staticmethod
    def is_export(ins):
        return bool(ins.fields.get("is_export"))

    def vector_mask(self, ins):
        """The lanes the vector result goes to (an export's lanes both pipes name take the constant 1)."""
        f = ins.fields
        mask = f.get("vector_write_mask", 0)
        if self.is_export(ins):
            mask &= ~f.get("scalar_write_mask", 0)
        return mask

    def vector_active(self, ins):
        return self.vector_mask(ins) != 0 or self.vector_op(ins) in _VECTOR_STATE

    def scalar_active(self, ins):
        return self.scalar_op(ins) != S.kRetainPrev

    def vector_reads(self, ins):
        """{operand: lanes} the vector pipe reads."""
        if not self.vector_active(ins):
            return {}
        op = self.vector_op(ins)
        reads = _VECTOR_READS.get(op)
        if reads is None:
            return {1: ALL, 2: ALL, 3: ALL}
        if op in _ELEMENTWISE:
            mask = self.vector_mask(ins)
            return {n + 1: mask for n in range(len(reads))}
        return {n + 1: lanes for n, lanes in enumerate(reads)}

    def scalar_reads(self, ins):
        op = self.scalar_op(ins)
        if op == S.kRetainPrev:
            return {}
        two = _SCALAR_ONE.get(op)
        if two is None:
            return {3: ALL}
        return {3: 0x9 if two else 0x8}         # a is lane w, b lane x (xenia:src/xenia/gpu/ucode.h:966)

    def reads(self, ins):
        """{operand: lanes} the instruction reads, both pipes."""
        if ins.kind != "alu":
            return {}
        reads = dict(self.vector_reads(ins))
        for n, lanes in self.scalar_reads(ins).items():
            reads[n] = reads.get(n, 0) | lanes
        return reads

    def temp_reads(self, ins):
        """The (register, component) pairs an instruction reads; a fetch counts as reading its source whole."""
        if ins.kind != "alu":
            return {(ins.fields.get("src", 0), c) for c in range(4)}
        out = set()
        for n, lanes in self.reads(ins).items():
            src = self.source(ins, n)
            if src.temp:
                out |= {(src.reg, src.components[lane]) for lane in _lanes(lanes)}
        return out

    def defs(self, ins):
        """The (register, component) pairs an instruction writes; a fetch's kept components (swizzle 7) aren't."""
        f = ins.fields
        if ins.kind != "alu":
            swizzle = f.get("dest_swizzle", 0)
            return {(f["dest"], c) for c in range(4) if (swizzle >> (3 * c)) & 7 != 7}
        if self.is_export(ins):
            return set()
        out = set()
        if f.get("vector_write_mask", 0):
            out |= {(f["vector_dest"], c) for c in _lanes(f["vector_write_mask"])}
        if f.get("scalar_write_mask", 0):
            out |= {(f.get("scalar_dest", 0), c) for c in _lanes(f["scalar_write_mask"])}
        return out

    def constants_fit(self, ins) -> bool:
        consts = {n: self.source(ins, n).reg for n in self.reads(ins) if not self.source(ins, n).temp}
        if len(set(consts.values())) > MAX_CONSTANTS:
            return False
        return len(consts) < 3 or consts[2] == consts[3]

    def fixed(self, ins) -> bool:
        """An instruction the passes neither change nor remove (they still see its reads and writes)."""
        if ins.kind != "alu":
            return True
        return self.vector_op(ins) in _VECTOR_STATE or \
            (self.scalar_active(ins) and self.scalar_op(ins) not in _SCALAR_ONE)

    def is_move(self, ins) -> bool:
        f = ins.fields
        if self.fixed(ins) or self.is_export(ins) or self.scalar_active(ins):
            return False
        if self.vector_op(ins) != V.kMax or f.get("vector_clamp") or not f.get("vector_write_mask", 0):
            return False
        return self.source(ins, 1).same_value(self.source(ins, 2))

    def plain_vector(self, ins) -> bool:
        """A vector op alone, writing a temp."""
        return not self.fixed(ins) and not self.is_export(ins) and not self.scalar_active(ins) and \
            ins.fields.get("vector_write_mask", 0) != 0

    # ---- the program ----

    def instructions(self):
        return [ins for stage in self.p._stages for ins in stage]

    def remove(self, doomed):
        for stage in self.p._stages:
            stage[:] = [ins for ins in stage if id(ins) not in doomed]

    # ---- the passes; each returns whether it changed the program ----

    def remove_dead(self) -> bool:
        """Instructions writing nothing that's read go; the rest write only what's read."""
        live = set()
        doomed = set()
        changed = False
        for ins in reversed(self.instructions()):
            if ins.kind == "alu" and not self.is_export(ins) and not self.fixed(ins):
                f = ins.fields
                for dest_key, mask_key in (("vector_dest", "vector_write_mask"), ("scalar_dest", "scalar_write_mask")):
                    mask = f.get(mask_key, 0)
                    keep = sum(1 << c for c in _lanes(mask) if (f.get(dest_key, 0), c) in live)
                    if keep != mask:
                        f[mask_key] = keep
                        changed = True
                if not f.get("scalar_write_mask", 0) and self.scalar_active(ins):
                    f["scalar_opcode"] = S.kRetainPrev
                    changed = True
                if not self.vector_active(ins) and not self.scalar_active(ins):
                    doomed.add(id(ins))
                    continue
            live -= self.defs(ins)
            live |= self.temp_reads(ins)
        if doomed:
            self.remove(doomed)
        return changed or bool(doomed)

    def same_values(self, flow, src, lanes_components, i, j) -> bool:
        """Whether a temp source's given components hold the same writes at instructions i and j."""
        if not src.temp:
            return True
        return all(flow.at(i, (src.reg, c)) == flow.at(j, (src.reg, c)) for c in lanes_components)

    def common_subexpressions(self) -> bool:
        """A vector op computing what an earlier one holds, from the same values, becomes a move of it (or goes, if it
        writes the same register)."""
        instrs = self.instructions()
        flow = _Flow(self, instrs)
        for j, b in enumerate(instrs):
            if not self.plain_vector(b) or self.is_move(b):
                continue
            fb = b.fields
            for i in range(j - 1, -1, -1):
                a = instrs[i]
                if not self.plain_vector(a) or not self.same_computation(flow, a, b, i, j):
                    continue
                fa = a.fields
                if fb["vector_write_mask"] & ~fa["vector_write_mask"]:
                    continue
                # a's result must still be in its register when b runs.
                if any(flow.at(j, (fa["vector_dest"], c)) != i for c in _lanes(fb["vector_write_mask"])):
                    continue
                if fa["vector_dest"] == fb["vector_dest"]:
                    self.remove({id(b)})
                    return True
                move = _Source(True, fa["vector_dest"], False, [0, 1, 2, 3], False)
                fb["vector_opcode"] = V.kMax
                fb["vector_clamp"] = False
                self.set_source(b, 1, move)
                self.set_source(b, 2, move)
                return True
        return False

    def same_computation(self, flow, a, b, i, j) -> bool:
        fa, fb = a.fields, b.fields
        op = self.vector_op(b)
        if self.vector_op(a) != op or bool(fa.get("vector_clamp")) != bool(fb.get("vector_clamp")):
            return False
        reads_b = self.vector_reads(b)
        for n, lanes in reads_b.items():
            sa, sb = self.source(a, n), self.source(b, n)
            if (sa.temp, sa.reg, sa.absolute, sa.negate) != (sb.temp, sb.reg, sb.absolute, sb.negate):
                return False
            used = _lanes(lanes) if op in _ELEMENTWISE else range(4)
            if any(sa.components[lane] != sb.components[lane] for lane in used):
                return False
            if not self.same_values(flow, sa, {sa.components[lane] for lane in used}, i, j):
                return False
        return True

    def propagate_copies(self) -> bool:
        """A move's readers read its source instead, where every lane they read of it came from that move."""
        instrs = self.instructions()
        flow = _Flow(self, instrs)
        for i, mov in enumerate(instrs):
            if not self.is_move(mov):
                continue
            f = mov.fields
            dest, mask = f["vector_dest"], f["vector_write_mask"]
            users = sorted({r for c in _lanes(mask) for r in flow.readers.get((i, (dest, c)), [])})
            if any(self.rewrite_reader(flow, i, mov, r) for r in users):
                return True
        return False

    def rewrite_reader(self, flow, i, mov, r) -> bool:
        reader = flow.instrs[r]
        if self.fixed(reader):
            return False
        f = mov.fields
        src, dest, mask = self.source(mov, 1), f["vector_dest"], f["vector_write_mask"]
        new = {}
        for n, lanes in self.reads(reader).items():
            old = self.source(reader, n)
            if not old.temp or old.reg != dest:
                continue
            read = _lanes(lanes)
            # Every lane of this operand that's read must come from the move.
            if any(not (mask >> old.components[lane] & 1) or flow.at(r, (dest, old.components[lane])) != i
                   for lane in read):
                return False
            components = [src.components[old.components[lane] if lane in read else 0] for lane in range(4)]
            if old.absolute:
                if not src.temp:
                    return False
                absolute, negate = True, old.negate
            else:
                absolute, negate = src.absolute, old.negate ^ src.negate
            candidate = _Source(src.temp, src.reg, absolute, components, negate)
            # The move's source must hold at the reader what it held at the move.
            if not self.same_values(flow, candidate, {components[lane] for lane in read}, i, r):
                return False
            new[n] = candidate
        if not new:
            return False
        saved = dict(reader.fields)
        for n, s in new.items():
            self.set_source(reader, n, s)
        if not self.constants_fit(reader):
            reader.fields = saved
            return False
        return True

    def fuse_multiply_adds(self) -> bool:
        """mul t = a * b; add d = t + c, with nothing else reading t: mad d = a * b + c (both give +0 for a zero
        factor, xenia:src/xenia/gpu/shader_interpreter.cc:339-347, 394-403)."""
        instrs = self.instructions()
        flow = _Flow(self, instrs)
        for i, mul in enumerate(instrs):
            f = mul.fields
            if not self.plain_vector(mul) or self.vector_op(mul) != V.kMul or f.get("vector_clamp"):
                continue
            t, tmask = f["vector_dest"], f["vector_write_mask"]
            users = {r for c in _lanes(tmask) for r in flow.readers.get((i, (t, c)), [])}
            if len(users) != 1:
                continue
            r = users.pop()
            add = instrs[r]
            if self.fixed(add) or self.scalar_active(add) or self.vector_op(add) != V.kAdd:
                continue
            lanes = _lanes(self.vector_mask(add))
            ops = {n: self.source(add, n) for n in (1, 2)}
            hits = [n for n, s in ops.items() if s.temp and s.reg == t and
                    any(flow.at(r, (t, s.components[lane])) == i for lane in lanes)]
            if len(hits) != 1:
                continue
            tsrc, other = ops[hits[0]], ops[3 - hits[0]]
            if tsrc.absolute or any(not (tmask >> tsrc.components[lane] & 1) or
                                    flow.at(r, (t, tsrc.components[lane])) != i for lane in lanes):
                continue
            fused = []
            for n in (1, 2):
                s = self.source(mul, n)
                components = [s.components[tsrc.components[lane] if lane in lanes else 0] for lane in range(4)]
                fused.append(_Source(s.temp, s.reg, s.absolute, components, s.negate ^ (n == 1 and tsrc.negate)))
            # a and b must hold at the add what they held at the multiply.
            if not all(self.same_values(flow, s, {s.components[lane] for lane in lanes}, i, r) for s in fused):
                continue
            saved = dict(add.fields)
            add.fields["vector_opcode"] = V.kMad
            self.set_source(add, 1, fused[0])
            self.set_source(add, 2, fused[1])
            self.set_source(add, 3, other)
            if not self.constants_fit(add):
                add.fields = saved
                continue
            return True
        return False

    def vector_only(self, ins) -> bool:
        return self.plain_vector(ins) and 3 not in self.vector_reads(ins)

    def scalar_only(self, ins) -> bool:
        return not self.fixed(ins) and not self.is_export(ins) and self.scalar_active(ins) and \
            not self.vector_active(ins) and ins.fields.get("scalar_write_mask", 0) != 0

    def co_issue(self) -> bool:
        """An instruction with one pipe working joins the nearest earlier one with only the other pipe working, in the
        same stage, if nothing between stands in the way."""
        stage_of = {id(ins): s for s, stage in enumerate(self.p._stages) for ins in stage}
        instrs = self.instructions()
        for j, x in enumerate(instrs):
            vector, scalar = self.vector_only(x), self.scalar_only(x)
            if not vector and not scalar:
                continue
            x_reads, x_defs = self.temp_reads(x), self.defs(x)
            for i in range(j - 1, -1, -1):
                y = instrs[i]
                if stage_of[id(y)] != stage_of[id(x)]:
                    break
                y_defs = self.defs(y)
                # Joined, both pipes read before either writes: x mustn't read what y writes.
                if x_reads & y_defs:
                    break
                partner = self.scalar_only(y) if vector else self.vector_only(y)
                if partner and not (x_defs & y_defs) and self.merge(y, x):
                    self.remove({id(x)})
                    return True
                # Moving x above y: y mustn't read or write what x writes, nor change state.
                if x_defs & (self.temp_reads(y) | y_defs) or y.kind == "alu" and self.fixed(y):
                    break
        return False

    def merge(self, y, x) -> bool:
        """Puts x's working pipe into y, which keeps its own."""
        vec, sca = (x, y) if self.vector_only(x) else (y, x)
        merged = dict(vec.fields)
        for key in ("scalar_opcode", "scalar_dest", "scalar_write_mask", "scalar_clamp", "is_scalar_dest_relative",
                    "src3_reg", "src3_is_temp", "src3_swizzle", "src3_negate"):
            if key in sca.fields:
                merged[key] = sca.fields[key]
            else:
                merged.pop(key, None)
        saved = y.fields
        y.fields = merged
        if not self.constants_fit(y):
            y.fields = saved
            return False
        return True

    def run(self):
        while self.remove_dead() or self.common_subexpressions() or self.propagate_copies() or \
                self.fuse_multiply_adds():
            pass
        while self.co_issue():
            pass
        self.remove_dead()


def can_optimize(program) -> bool:
    if not program.whole_sources or program._loops:
        return False
    for stage in program._stages:
        for ins in stage:
            f = ins.fields
            if ins.kind != "alu":
                if f.get("is_src_relative") or f.get("is_dest_relative") or f.get("is_predicated"):
                    return False
                continue
            if any(f.get(flag) for flag in _REFUSED_FLAGS):
                return False
            sop = S(f.get("scalar_opcode", S.kRetainPrev))
            scalar_mask = f.get("scalar_write_mask", 0)
            if f.get("is_export"):
                scalar_mask &= ~f.get("vector_write_mask", 0)
            if sop in _SCALAR_PREVIOUS or sop == S.kRetainPrev and scalar_mask:
                return False
            if f.get("is_scalar_dest_relative") and not f.get("is_export"):
                return False
    return True


def optimize(program, abs_flag: int) -> bool:
    """Rewrites the program's instructions in place; returns whether it could (see the module's docstring)."""
    if not can_optimize(program):
        return False
    _Optimizer(program, abs_flag).run()
    return True
