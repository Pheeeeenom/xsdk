#!/usr/bin/env python3
"""Declarative Xenos shader Program builder on top of xasm.encode.

The caller declares fetches, ALU instructions, and stage boundaries
(alloc_* markers); finish() performs:
  - instruction slot layout and control-flow program construction
    (exec/exec_end entries with addresses, counts, sequence bits),
  - register-usage scanning -> SQ_PROGRAM_CNTL,
  - VertexElement / Interpolator / export-slot metadata,
  - container emission via xdis.container_emit.

Instruction *encodings* are supplied as field dicts (matching the
xasm.encode builder kwargs); xsc/templates.py holds the field templates
xsc fills in.

Conventions baked in (validated on hardware by xost's first demo apps, in its
archived repo, and by the SDK's titles since):
exec entries use is_predicate_clean=True; fetch-bearing execs carry vc_hi=15;
sequence bit0 marks fetch slots; VS tail flags: POSITION=0x1, COLOR=0x3,
TEXCOORD=0x0; interpolator flags: VS (index<<4)|nibble, PS nibble
(nibble: TEXCOORD=0x3, COLOR=0xF).
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

import xdis.container_emit as ce  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xasm import encode as enc  # noqa: E402
from xasm.optimize import optimize as _optimize_instructions  # noqa: E402

K = xu.ControlFlowOpcode

MAX_EXEC_COUNT = 6  # exec count field is 3 bits; stay below the 7 limit

@dataclass(frozen=True)
class RegisterBudget:
    """The registers each kind of shader may use, from r0: its share of the
    shader unit's split, which the runtime sets in SQ_GPR_MANAGEMENT (pixel
    shaders' share in bits 4-11, vertex shaders' in bits 12-19,
    Linux:drivers/gpu/drm/msm/registers/adreno/a2xx.xml:1231-1235). A shader
    past its share hangs the GPU: under libxenon's split a pixel shader's r16
    hangs and r15 runs (observed on hardware). Past both, instructions name
    r63 at most and SQ_PROGRAM_CNTL counts 64 (xenia:src/xenia/gpu/
    registers.h:127-148)."""
    pixel: int
    vertex: int


# libxenon's set-up ends the split at 16 and 112 (libxenon:drivers/xenos/xe.c:
# 527-528); xsdk's stops at the kernel's 64 and 64 (xsdk:src/gpu_setup.c).
LIBXENON_REGISTERS = RegisterBudget(pixel=16, vertex=112)
XSDK_REGISTERS = RegisterBudget(pixel=64, vertex=64)


class RegisterBudgetError(ValueError):
    """A shader needs more registers than its kind's share of the split."""


def check_registers(shader_type: str, max_reg: int,
                    budget: RegisterBudget) -> None:
    """Raises RegisterBudgetError if r0..r<max_reg> is over the budget, or
    past r63, the last register an instruction names."""
    share = budget.pixel if shader_type == "pixel" else budget.vertex
    if max_reg + 1 > share:
        raise RegisterBudgetError(
            f"{shader_type} shader needs {max_reg + 1} registers "
            f"(r0..r{max_reg}), over the {share} it may use; it would hang "
            f"the GPU")
    if max_reg + 1 > 64:
        raise RegisterBudgetError(
            f"{shader_type} shader needs {max_reg + 1} registers "
            f"(r0..r{max_reg}); an instruction names r63 at most")

# Scalar ALU opcodes that write the predicate register p0. An exec block that
# contains one of these MUST be emitted with is_predicate_clean=False, or the
# sequencer treats the predicate as clean and won't wait for the write before
# a following predicated control-flow instruction (e.g. a predicated LOOP_END)
# reads it -- the data-dependent break would then never fire (ucode.h:
# "PredicateClean=false for the exec if the instructions inside it modify the
# predicate").
_PRED_WRITE_SCALAR_OPS = frozenset(int(o) for o in (
    xu.AluScalarOpcode.kSetpEq, xu.AluScalarOpcode.kSetpNe,
    xu.AluScalarOpcode.kSetpGt, xu.AluScalarOpcode.kSetpGe,
    xu.AluScalarOpcode.kSetpInv, xu.AluScalarOpcode.kSetpPop,
    xu.AluScalarOpcode.kSetpClr, xu.AluScalarOpcode.kSetpRstr))


def _modifies_predicate(ins) -> bool:
    f = getattr(ins, "fields", None)
    if not f:
        return False
    op = f.get("scalar_opcode")
    return op is not None and int(op) in _PRED_WRITE_SCALAR_OPS

_USAGE_NIBBLE = {ce.USAGE_TEXCOORD: 0x3, ce.USAGE_COLOR: 0xF}
_VE_FLAGS = {ce.USAGE_POSITION: 0x1, ce.USAGE_COLOR: 0x3,
             ce.USAGE_TEXCOORD: 0x0}


def swizzle4(s: str) -> int:
    """ALU source swizzle text ('wzxy') -> 8-bit delta encoding."""
    assert len(s) == 4
    out = 0
    for i, ch in enumerate(s):
        out |= ((("xyzw".index(ch)) - i) & 3) << (2 * i)
    return out


def mask4(s: str) -> int:
    """write mask text ('xy', 'xyzw') -> 4-bit mask."""
    out = 0
    for ch in s:
        out |= 1 << "xyzw".index(ch)
    return out


ABS_SOURCE = 1 << 16                             # a whole temp source's absolute value


class _Instr:
    def __init__(self, kind: str, triple, is_fetch: bool):
        self.kind = kind
        self.triple = tuple(triple)
        self.is_fetch = is_fetch
        self.slot = -1


class _Program:
    shader_type = None

    def __init__(self, registers: RegisterBudget = LIBXENON_REGISTERS):
        self.registers = registers               # finish() refuses past it
        # A temp source's value: its register in bits 0-5 with 0x40 relative
        # and 0x80 its absolute value, as an instruction encodes it; or, where
        # whole_sources is set (xsc's programs, whose temps run past r63 till
        # the allocator packs them), the whole register with ABS_SOURCE for its
        # absolute value, encoded when the instruction is.
        self.whole_sources = False
        # Whether finish() first rewrites the instructions (xasm/optimize.py); off, a program compiles as it always
        # has.
        self.optimize = False
        self._stages: list[list[_Instr]] = [[]]  # instr blocks
        self._alloc_after: dict = {}             # stage_idx -> alloc_type
        self._max_reg = 0
        self.interpolators: list[ce.Interpolator] = []
        self._export_stage_idx = None
        self._loops: list[tuple] = []            # (start, end, loop_id, brk, cond)
        self._loop_open = None
        # Set once _group_fetches has laid the stages out: a stage whose ALU work runs right into two fetches or more
        # then starts an exec wherever ALU and fetch runs meet (_exec_chunks), and what a run of fetches reads stays
        # in its register till the run ends (_allocate_registers).
        self._clauses = False

    def _src_temp_index(self, reg: int) -> int:
        return reg & (ABS_SOURCE - 1) if self.whole_sources else reg & 0x3F

    def _encoded_src_temp(self, reg: int, mapping: dict) -> int:
        """A temp source as an instruction encodes it, its register mapped."""
        idx = self._src_temp_index(reg)
        if self.whole_sources:
            return mapping.get(idx, idx) | (0x80 if reg & ABS_SOURCE else 0)
        return (reg & ~0x3F) | mapping.get(idx, idx)

    def _remap_fields(self, fields: dict, mapping: dict) -> dict:
        """The fields with each temp mapped (the allocator's mapping, or none),
        its sources as an instruction encodes them."""
        f = dict(fields)
        if "vector_opcode" in f or "scalar_opcode" in f:       # ALU
            if not f.get("is_export") and f.get("vector_write_mask", 0):
                f["vector_dest"] = mapping.get(f["vector_dest"], f["vector_dest"])
            if f.get("scalar_write_mask", 0) and not f.get("is_export"):
                f["scalar_dest"] = mapping.get(f.get("scalar_dest", 0), f.get("scalar_dest", 0))
            for i in (1, 2, 3):
                if f.get(f"src{i}_is_temp"):
                    f[f"src{i}_reg"] = self._encoded_src_temp(f[f"src{i}_reg"], mapping)
        else:                                                  # fetch
            f["dest"] = mapping.get(f["dest"], f["dest"])
            f["src"] = mapping.get(f.get("src", 0), f.get("src", 0))
        return f

    # ---- declarations ----

    def _track_regs(self, fields: dict):
        for key in ("dest", "src"):
            if key in fields:
                self._max_reg = max(self._max_reg, fields[key])
        if "vector_dest" in fields and not fields.get("is_export"):
            self._max_reg = max(self._max_reg, fields["vector_dest"])
        # scalar-pipe destination (rcp/sqrt/sin/cos/...) — a real register
        # write that must count toward the declared GPR total, else the GPU
        # reads/writes out of bounds (hang or garbage).
        if fields.get("scalar_write_mask") and not fields.get("is_export"):
            self._max_reg = max(self._max_reg, fields.get("scalar_dest", 0))
        for i in (1, 2, 3):
            if fields.get(f"src{i}_is_temp"):
                self._max_reg = max(
                    self._max_reg,
                    self._src_temp_index(fields[f"src{i}_reg"]))

    # A temp's index is six bits in an instruction (xenia:src/xenia/gpu/ucode.h); one past it is encoded once the
    # allocator has recycled the temps.
    ENCODABLE_REGS = 64

    def _encodable(self, fields: dict) -> bool:
        before = self._max_reg
        self._max_reg = 0
        self._track_regs(fields)
        fits = self._max_reg < self.ENCODABLE_REGS
        self._max_reg = before
        return fits

    def alu(self, **fields) -> _Instr:
        triple = enc.build_alu(**self._remap_fields(fields, {})) if self._encodable(fields) else ()
        self._track_regs(fields)
        ins = _Instr("alu", triple, is_fetch=False)
        ins.fields = dict(fields)
        self._stages[-1].append(ins)
        return ins

    def _alloc(self, alloc_type: int):
        self._alloc_after[len(self._stages) - 1] = alloc_type
        self._stages.append([])

    # ---- hardware loops (LOOP_START/LOOP_END) ----

    def begin_loop(self, loop_id: int = 0):
        """Open a hardware loop around every instruction added until end_loop().
        The trip count is taken from loop constant i[loop_id] at draw time
        (host uploads integer_constants[8+loop_id] = count | start<<8 | step<<16;
        aL = iteration*step + start runs the loop count times). Used for
        data-dependent / uniform-bounded loops that can't be unrolled."""
        if self._stages[-1]:
            self._stages.append([])              # body starts in a fresh stage
        self._loop_open = (len(self._stages) - 1, loop_id)

    def end_loop(self, predicated_break: bool = False,
                 break_condition: bool = False):
        """Close the loop opened by begin_loop().

        predicated_break=True turns the LOOP_END into a data-dependent early
        exit: the loop breaks when the predicate p0 (set by a setp_* ALU op in
        the body) equals break_condition. This is the `while(cond) break`
        primitive -- the trip count from the loop constant still acts as the
        upper bound. Defaults reproduce the plain counted loop (no break),
        keeping existing loop shaders byte-identical."""
        start_stage, loop_id = self._loop_open
        self._loops.append((start_stage, len(self._stages), loop_id,
                            predicated_break, break_condition))
        self._stages.append([])                  # fresh stage for post-loop code
        self._loop_open = None

    # ---- layout ----

    def _exec_chunks(self, stage) -> list:
        """A stage's execs, MAX_EXEC_COUNT instructions at most each. Once
        _group_fetches has laid it out, a stage whose ALU work runs right into
        two fetches or more also starts an exec wherever ALU and fetch runs
        meet, so each run of fetches is an exec of its own, as the layout
        measured fastest is (the SDK's docs/HARDWARE.md, "The texel rate,
        milestone 36"). Other stages keep plain chunks: those of shaders that
        sample only their inputs, and those whose fetches each wait on the work
        before them, as nothing was measured to gain from it there."""
        runs = [stage]
        if self._clauses and any(not a.is_fetch and b.is_fetch and c.is_fetch
                                 for a, b, c in zip(stage, stage[1:],
                                                    stage[2:])):
            runs = []
            for ins in stage:
                if runs and runs[-1][-1].is_fetch == ins.is_fetch:
                    runs[-1].append(ins)
                else:
                    runs.append([ins])
        return [run[i:i + MAX_EXEC_COUNT] for run in runs
                for i in range(0, len(run), MAX_EXEC_COUNT)]

    def _cf_entry_count(self) -> int:
        n = 0
        for i, stage in enumerate(self._stages):
            n += len(self._exec_chunks(stage))
            if i in self._alloc_after:
                n += 1
        return n + 2 * len(self._loops)  # LOOP_START + LOOP_END per loop

    def _build_ucode(self) -> bytes:
        n_cf = self._cf_entry_count()
        first_slot = (n_cf + 1) // 2  # cf pairs occupy one slot each
        # assign slots
        slot = first_slot
        for stage in self._stages:
            for ins in stage:
                ins.slot = slot
                slot += 1
        total_slots = slot

        # loop boundaries keyed by stage index
        loop_start_at = {s0: lid for (s0, s1, lid, *_) in self._loops}
        loop_end_at = {s1 - 1: (lid, brk, cond)
                       for (s0, s1, lid, brk, cond) in self._loops}
        # the final non-empty exec chunk gets kExecEnd (ends the shader)
        last_exec = None
        for si, stage in enumerate(self._stages):
            nch = len(self._exec_chunks(stage))
            if nch:
                last_exec = (si, nch - 1)

        cf = []            # descriptors; loop addresses patched once indices known
        open_loops = {}    # loop_id -> LOOP_START index in cf
        for si, stage in enumerate(self._stages):
            if si in loop_start_at:
                lid = loop_start_at[si]
                open_loops[lid] = len(cf)
                cf.append({"t": "loop_start", "loop_id": lid, "skip": None})
            chunks = self._exec_chunks(stage)
            for ci, chunk in enumerate(chunks):
                is_last = (si, ci) == last_exec
                seq = 0
                n_fetch = 0
                for k, ins in enumerate(chunk):
                    if ins.is_fetch:
                        seq |= 0b01 << (2 * k)
                        n_fetch += 1
                # XDK convention: vc_hi = per-fetch-count mask (VS only);
                # corpus: 4 fetches -> 0xF, 3 -> 0x7 (cube_vs)
                vc = ((1 << min(n_fetch, 4)) - 1) if (
                    n_fetch and self.shader_type == "vertex") else 0
                # An exec that sets the predicate is not predicate-clean.
                pred_clean = not any(_modifies_predicate(ins) for ins in chunk)
                cf.append({"t": "exec",
                           "opcode": K.kExecEnd if is_last else K.kExec,
                           "address": chunk[0].slot, "count": len(chunk),
                           "sequence": seq, "vc_hi": vc,
                           "pred_clean": pred_clean})
            if si in self._alloc_after:
                cf.append({"t": "alloc", "alloc_type": self._alloc_after[si]})
            if si in loop_end_at:
                lid, brk, cond = loop_end_at[si]
                ls = open_loops.pop(lid)
                le = len(cf)
                cf.append({"t": "loop_end", "loop_id": lid, "body": ls + 1,
                           "brk": brk, "cond": cond})
                cf[ls]["skip"] = le + 1          # skip past LOOP_END when done

        pairs = []
        for d in cf:
            if d["t"] == "exec":
                pairs.append(enc.build_cf_exec(
                    opcode=d["opcode"], address=d["address"], count=d["count"],
                    is_yield=False, sequence=d["sequence"],
                    vc_hi=d["vc_hi"], vc_lo=0,
                    is_predicate_clean=d["pred_clean"], addressing_mode=0))
            elif d["t"] == "alloc":
                pairs.append(enc.build_cf_alloc(
                    opcode=K.kAlloc, size=0, is_unserialized=False,
                    alloc_type=d["alloc_type"]))
            elif d["t"] == "loop_start":
                pairs.append(enc.build_cf_loop_start(
                    opcode=K.kLoopStart, address=d["skip"],
                    is_repeat=False, loop_id=d["loop_id"],
                    addressing_mode=xu.AddressingMode.kRelative))
            else:                                # loop_end
                pairs.append(enc.build_cf_loop_end(
                    opcode=K.kLoopEnd, address=d["body"], loop_id=d["loop_id"],
                    is_predicated_break=d["brk"], condition=d["cond"],
                    addressing_mode=xu.AddressingMode.kRelative))
        while len(pairs) % 2:
            pairs.append((0, 0))  # cnop
        assert len(pairs) // 2 == first_slot, "CF layout self-check failed"

        dwords = []
        for i in range(0, len(pairs), 2):
            dwords.extend(enc.pack_cf_pair(pairs[i], pairs[i + 1]))
        for stage in self._stages:
            for ins in stage:
                dwords.extend(ins.triple)
        assert len(dwords) == total_slots * 3
        return struct.pack(f">{len(dwords)}I", *dwords)

    def _optimize(self):
        """Rewrites the instructions if asked to and the program allows it, then encodes them again; the registers
        they name are a subset of those before, so _max_reg stays a bound."""
        if not self.optimize or not _optimize_instructions(self, ABS_SOURCE):
            return
        for stage in self._stages:
            for ins in stage:
                if ins.kind == "alu":
                    ins.triple = tuple(enc.build_alu(**self._remap_fields(ins.fields, {}))) \
                        if self._encodable(ins.fields) else ()

    def _group_fetches(self):
        """Lays each stage out with the work its fetches' coordinates need
        ahead of them and the fetches together: that work as soon as it's
        ready, then every fetch that's ready, again for fetches that read what
        others fetched, then the rest in its own order. Four fetches each
        after its own coordinate's add, the coordinates sharing a register,
        sampled 10 texels a clock on the console; the adds first and the
        fetches together, 16, the texture units' rate (the SDK's
        docs/HARDWARE.md, "The texel rate, milestone 36")."""
        for si, stage in enumerate(self._stages):
            if any(ins.is_fetch for ins in stage):
                self._stages[si] = self._fetches_grouped(stage)
        self._clauses = True

    _STATE_FETCHES = frozenset(int(o) for o in (
        xu.FetchOpcode.kSetTextureLod, xu.FetchOpcode.kSetTextureGradientsHorz,
        xu.FetchOpcode.kSetTextureGradientsVert))

    def _ordered(self, a, b) -> bool:
        """Whether b, after a, must stay after it for what their registers
        don't show: the predicate, the scalar unit's previous result, a fetch
        that sets state the next fetches read, and the exports' order."""
        fa, fb = a.fields, b.fields
        if _modifies_predicate(a) and (fb.get("is_predicated")
                                       or _modifies_predicate(b)):
            return True
        if fa.get("is_predicated") and _modifies_predicate(b):
            return True
        retain = int(xu.AluScalarOpcode.kRetainPrev)
        if a.kind == "alu" and b.kind == "alu" \
                and int(fa.get("scalar_opcode", retain)) != retain \
                and int(fb.get("scalar_opcode", retain)) != retain:
            return True
        if a.is_fetch and b.is_fetch and (
                int(fa.get("opcode", 0)) in self._STATE_FETCHES
                or int(fb.get("opcode", 0)) in self._STATE_FETCHES):
            return True
        return bool(fa.get("is_export") and fb.get("is_export"))

    def _fetches_grouped(self, stage) -> list:
        n = len(stage)
        regs = [tuple(map(set, self._defs_uses(ins))) for ins in stage]
        # the instructions each must follow: its registers' writers and
        # readers before it, and those _ordered keeps before it
        after = [set() for _ in range(n)]
        for j in range(n):
            dj, uj = regs[j]
            for i in range(j):
                di, ui = regs[i]
                if di & uj or ui & dj or di & dj or \
                        self._ordered(stage[i], stage[j]):
                    after[j].add(i)
        needs = [set() for _ in range(n)]
        for j in range(n):
            for i in after[j]:
                needs[j] |= needs[i] | {i}
        done, order = set(), []

        def take(k):
            done.add(k)
            order.append(k)

        def ready(k):
            return k not in done and after[k] <= done

        while any(ins.is_fetch and k not in done
                  for k, ins in enumerate(stage)):
            wanted = set().union(*(needs[k] for k, ins in enumerate(stage)
                                   if ins.is_fetch and k not in done))
            taken = len(order)
            more = True
            while more:                         # the work fetches need
                more = False
                for k in range(n):
                    if k in wanted and not stage[k].is_fetch and ready(k):
                        take(k)
                        more = True
            for k in range(n):                  # then the fetches ready
                if stage[k].is_fetch and ready(k):
                    take(k)
            if len(order) == taken:             # never, but never stall
                take(min(k for k in range(n) if ready(k)))
        order += [k for k in range(n) if k not in done]
        return [stage[k] for k in order]

    def _defs_uses(self, ins):
        """Temp registers an instruction writes (defs) and reads (uses).
        Constants, the oC export, and read-only inputs are excluded from defs;
        only is_temp sources count as uses."""
        f = ins.fields
        d, u = [], []
        if ins.kind == "alu":
            if not f.get("is_export") and f.get("vector_write_mask", 0):
                d.append(f["vector_dest"])
            if f.get("scalar_write_mask", 0) and not f.get("is_export"):
                d.append(f.get("scalar_dest", 0))
            for i in (1, 2, 3):
                if f.get(f"src{i}_is_temp"):
                    u.append(self._src_temp_index(f[f"src{i}_reg"]))
        else:                                         # tfetch / vfetch
            d.append(f["dest"])
            u.append(f.get("src", 0))                 # a vfetch's index, r0 unless named
        return d, u

    def _allocate_registers(self):
        """Linear-scan register allocation: recycle dead temporaries so the
        shader fits the GPR budget. Registers read but never written (a pixel
        shader's interpolator inputs and pixel parameters) stay pinned where
        they are, and so does one a vertex fetch reads its index from before
        it's written (a vertex shader's r0); every other written register is a
        'temp' whose live range [first def, last use] is packed into the
        smallest pool of physical registers above the pinned ones. A temp live
        across a hardware loop's back edge is kept for the whole loop, as the
        next pass reads it again. An ALU's unused source slots count as reads,
        so only a fetch's read pins a written register."""
        import heapq
        instrs = [ins for stage in self._stages for ins in stage]

        first_def, first_use, last_use, fetch_read = {}, {}, {}, {}
        for idx, ins in enumerate(instrs):
            d, u = self._defs_uses(ins)
            for r in u:
                first_use.setdefault(r, idx)
                last_use[r] = idx
                if ins.kind != "alu":
                    fetch_read.setdefault(r, idx)
            for r in d:
                first_def.setdefault(r, idx)
                last_use[r] = max(last_use.get(r, idx), idx)
        if self._clauses:
            # Once _group_fetches has laid them out, what a run of fetches
            # reads stays in its register till the run ends, as in the layout
            # measured fastest, so no fetch in it writes another's coordinates.
            stage_of = [si for si, stage in enumerate(self._stages)
                        for _ in stage]
            run_end = {}
            for idx in range(len(instrs) - 1, -1, -1):
                if instrs[idx].is_fetch:
                    nxt = idx + 1
                    run_end[idx] = run_end[nxt] if nxt < len(instrs) and \
                        instrs[nxt].is_fetch and \
                        stage_of[nxt] == stage_of[idx] else idx
            for idx, end in run_end.items():
                for r in self._defs_uses(instrs[idx])[1]:
                    last_use[r] = max(last_use[r], end)

        never = len(instrs)
        pinned = {r for r in last_use if r not in first_def} | \
            {r for r in fetch_read if fetch_read[r] < first_def.get(r, never)}
        npinned = (max(pinned) + 1) if pinned else 0
        temps = [r for r in first_def if r not in pinned]
        start = {r: first_def[r] for r in temps}
        end = {r: last_use[r] for r in temps}

        # A loop's instructions, by index; a temp defined in it before its
        # first use there lives within one pass.
        for s0, s1, *_ in self._loops:
            lo = sum(len(stage) for stage in self._stages[:s0])
            hi = sum(len(stage) for stage in self._stages[:s1]) - 1
            for r in temps:
                within = lo <= start[r] and end[r] <= hi and \
                    first_use.get(r, never) >= start[r]
                if not within and start[r] <= hi and end[r] >= lo:
                    start[r] = min(start[r], lo)
                    end[r] = max(end[r], hi)

        intervals = sorted((start[r], end[r], r) for r in temps)
        free = list(range(npinned, npinned + len(temps) + 1))
        heapq.heapify(free)
        active, mapping = [], {}
        for begin, finish, r in intervals:
            keep = []
            for e, ph, vr in active:
                if e < begin:                         # dead -> recycle its reg
                    heapq.heappush(free, ph)
                else:
                    keep.append((e, ph, vr))
            active = keep
            ph = heapq.heappop(free)
            mapping[r] = ph
            active.append((finish, ph, r))

        # self-check: temps sharing a physical reg must have disjoint live
        # ranges (the correctness invariant of the allocation).
        share = {}
        for r, ph in mapping.items():
            share.setdefault(ph, []).append(r)
        for ph, vrs in share.items():
            vrs.sort(key=lambda r: start[r])
            for a, b in zip(vrs, vrs[1:]):
                assert end[a] < start[b], \
                    f"register-allocation conflict on physical r{ph}"

        for ins in instrs:                            # remap + re-encode
            f = self._remap_fields(ins.fields, mapping)
            if ins.kind == "alu":
                ins.triple = tuple(enc.build_alu(**f))
            else:
                ins.triple = tuple(enc.build_tfetch(**f)
                                   if ins.kind == "tfetch"
                                   else enc.build_vfetch_full(**f))
            ins.fields = f
        self.whole_sources = False                    # the fields are encoded now

        self._max_reg = max([npinned - 1] + list(mapping.values()))


class VertexProgram(_Program):
    shader_type = "vertex"

    def __init__(self, registers: RegisterBudget = LIBXENON_REGISTERS):
        super().__init__(registers)
        self.vertex_elements: list[ce.VertexElement] = []

    def fetch(self, usage: int, template: dict, dest: int,
              usage_index: int = 0) -> _Instr:
        """Template vfetch (patched at load by Xe_ShaderApplyVFetchPatches)."""
        fields = dict(template, dest=dest)
        triple = enc.build_vfetch_full(**fields) if self._encodable(fields) else ()
        self._track_regs(fields)
        ins = _Instr("vfetch", triple, is_fetch=True)
        ins.fields = dict(fields)
        self._stages[-1].append(ins)
        self.vertex_elements.append(
            ce.VertexElement(0, usage, usage_index,
                             flags=_VE_FLAGS.get(usage, 0)))
        ins._ve = self.vertex_elements[-1]
        return ins

    def alloc_position(self):
        self._alloc(1)

    def alloc_interpolators(self):
        self._alloc(2)
        self._export_stage_idx = len(self._stages) - 1

    def export(self, o: int, usage: int, template: dict,
               usage_index: int = 0, **overrides) -> _Instr:
        """Interpolator export ALU (template + vector_dest=o)."""
        fields = dict(template, vector_dest=o, **overrides)
        ins = self.alu(**fields)
        self.interpolators.append(ce.Interpolator(
            o, usage, usage_index,
            flags=(len(self.interpolators) << 4)
                  | _USAGE_NIBBLE.get(usage, 0)))
        return ins

    # A hand-built vertex shader keeps its registers unless they run past r63,
    # so each compiles as it did; xsc's are always packed (whole_sources), as
    # fewer registers a thread let more threads run.
    ALLOCATE_PAST = 64

    def finish(self, constants: list) -> bytes:
        self._optimize()
        if self.whole_sources or self._max_reg + 1 > self.ALLOCATE_PAST:
            self._allocate_registers()                # recycle dead temps
        check_registers("vertex", self._max_reg, self.registers)
        ucode = self._build_ucode()
        for ins_list in self._stages:
            for ins in ins_list:
                if hasattr(ins, "_ve"):
                    ins._ve.slot = ins.slot
        export_slots = [ins.slot
                        for ins in self._stages[self._export_stage_idx]]
        return ce.build_container(
            "vertex", ucode, constants=constants,
            program_control=(
                self._max_reg |               # vs_num_reg (value-1 wants
                (1 << 16) |                   # max reg index; vs_resource
                (max(len(self.interpolators) - 1, 0) << 20)),  # vs_export_count; 0 with none, as xsdk's resolve shader
            context_misc=0,
            interpolators=self.interpolators,
            vertex_elements=self.vertex_elements,
            vs_tail_count=len(self.interpolators),
            export_slots=export_slots)


class PixelProgram(_Program):
    shader_type = "pixel"

    # The pixel parameters' register is an interpolator's index (xenia:src/xenia/gpu/registers.h:159-161).
    MAX_INTERPOLATORS = 16

    def __init__(self, registers: RegisterBudget = LIBXENON_REGISTERS):
        super().__init__(registers)
        self._param_gen = None                   # the pixel parameters' register, if the shader reads them

    def input(self, r: int, usage: int, usage_index: int = 0):
        """Declare interpolator arriving in rN (must match VS export order)."""
        assert r == len(self.interpolators), "declare inputs in r0,r1,... order"
        self.interpolators.append(ce.Interpolator(
            r, usage, usage_index, flags=_USAGE_NIBBLE.get(usage, 0)))
        self._max_reg = max(self._max_reg, r)

    def pixel_parameters(self, r: int):
        """Have the pixel parameters written into rN, past the interpolators: |xy| the pixel's column and row (0, 1,
        2, ...), the sign bits flags (SQ_PROGRAM_CNTL.PARAM_GEN, SQ_CONTEXT_MISC.PARAM_GEN_POS;
        xenia:src/xenia/gpu/registers.h:140, 158-234)."""
        assert len(self.interpolators) <= r < self.MAX_INTERPOLATORS, "the pixel parameters go past the inputs"
        self._param_gen = r
        self._max_reg = max(self._max_reg, r)

    def tfetch(self, template: dict, dest: int, src: int) -> _Instr:
        fields = dict(template, dest=dest, src=src)
        triple = enc.build_tfetch(**fields) if self._encodable(fields) else ()
        self._track_regs(fields)
        ins = _Instr("tfetch", triple, is_fetch=True)
        ins.fields = dict(fields)
        self._stages[-1].append(ins)
        return ins

    def alloc_colors(self):
        self._alloc(2)

    # Past libxenon's 16 registers, dead temporaries are recycled first (see
    # docs/xsc.md "the frontier"), whatever the budget, so every hand-built
    # shader that fit 16 compiles as it did before budgets; xsc's are always
    # packed (whole_sources), as fewer registers a thread let more threads run.
    ALLOCATE_PAST = 16

    def finish(self, constants: list) -> bytes:
        self._optimize()
        if self.whole_sources:                        # xsc's: fetches together
            self._group_fetches()
        if self.whole_sources or self._max_reg + 1 > self.ALLOCATE_PAST:
            self._allocate_registers()                # recycle dead temps
        check_registers("pixel", self._max_reg, self.registers)
        ucode = self._build_ucode()
        param_gen = self._param_gen
        # The parameters' xy are the pixel's only with SC_OUTPUT_SCREEN_XY too; without it x read 0 on the console
        # (vulkan4xenon:results/2026-10-12-milestone-11-step-3/vulkan4xenon_diag.log, D2 and D4).
        return ce.build_container(
            "pixel", ucode, constants=constants,
            program_control=(self._max_reg << 8) | (2 << 27) | ((1 << 18) if param_gen is not None else 0),
            context_misc=0x8 | ((param_gen << 8 | 0x2) if param_gen is not None else 0),
            field10=0x20000,
            interpolators=self.interpolators,
            ps_field18=(len(self.interpolators) << 16) | 7,
            ps_outputs=0x1)
