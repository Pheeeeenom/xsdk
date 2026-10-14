#!/usr/bin/env python3
"""Loop unrolling + scalar mem2reg pre-pass for xsc.

glslang (-G --target-env opengl) lowers GLSL for/while loops to mutable-memory
form: an OpLoopMerge header, loop-carried values held in Function-storage
OpVariables mutated by OpStore, and an integer induction variable. xsc's
straight-line lowering (lower._store_map) is last-write-wins, so it cannot model
a variable stored more than once at meaningful program points -- which a loop
body does every iteration, and which read-modify-write (`c *= k;` after a loop)
does too.

When a module contains a loop this pass rewrites the whole function body into
flat, branch-free SSA by symbolically executing the structured control-flow
graph: every Function-local variable is threaded through an environment (full
mem2reg), counted loops with a compile-time trip count are expanded in place
(the induction variable constant-folded so `float(i)`, `i*k`, ... become
literals), and only stores to output variables survive. The result is consumed
by the existing (hardware-verified) lowering unchanged. A loop-free module gets
the same rewrite when it has no branches and stores a local more than once, as
`x = x * k + c;` written out does; other loop-free modules are returned
untouched, so they still lower byte-for-byte as before.

Out of scope for v1 -> raises UnrollError so the caller reports rather than
miscompiles:
  * data-dependent trip counts (init/bound/step not constant) -> needs real
    Xenos loop control flow (kLoopStart/kLoopEnd), the v2 path
  * if/else (OpSelectionMerge / BranchConditional) outside loop control
  * indexed access into a loop-carried local (partial vector update)
"""
from __future__ import annotations

import struct

from xsc.spirv import Instr, Module

MAX_TRIP = 4096

# opcodes
VARIABLE, LOAD, STORE, ACCESS_CHAIN = 59, 61, 62, 65
LABEL, BRANCH, BRANCH_COND = 248, 249, 250
LOOP_MERGE, SEL_MERGE = 246, 247
CONSTANT, FUNCTION, FUNCTION_END, EXT_INST = 43, 54, 56, 12
VEC_SHUFFLE, COMP_EXTRACT = 79, 81
TYPE_INT, TYPE_FLOAT = 21, 22
IADD, ISUB, IMUL, SDIV = 128, 130, 132, 135
FNEGATE, FADD, FSUB, FMUL, FDIV = 127, 129, 131, 133, 136
CONVERT_S_TO_F, CONVERT_F_TO_S = 111, 110
CMP_OPS = (170, 171, 172, 173, 174, 175, 176, 177, 178, 179)
RETURNS = (252, 253, 254)               # Kill, Return, ReturnValue
STORAGE_FUNCTION = 7


class UnrollError(Exception):
    pass


def unroll(m: Module) -> Module:
    """Expand constant-trip loops + mem2reg the function. Returns m (mutated).
    Also mem2regs a loop-free function without branches that stores a local
    more than once, or into one through an access chain, which the
    straight-line lowering would read as its last store. Any other module is
    returned unchanged, so it lowers byte-for-byte as before."""
    if any(i.opcode == LOOP_MERGE for i in m.instrs) or _stores_locals_again(m):
        _rewrite_function(m)
    return m


def _stores_locals_again(m: Module) -> bool:
    """One function, no branches, and a local stored more than once or into through an access chain."""
    if sum(1 for i in m.instrs if i.opcode == FUNCTION) != 1 or any(i.opcode == BRANCH_COND for i in m.instrs):
        return False
    local_vars = {i.result_id for i in m.instrs if i.opcode == VARIABLE and i.operands[0] == STORAGE_FUNCTION}
    chains = {i.result_id: i.operands[0] for i in m.instrs if i.opcode == ACCESS_CHAIN}
    targets = [i.operands[0] for i in m.instrs if i.opcode == STORE]
    stored = [t for t in targets if t in local_vars]
    return len(stored) != len(set(stored)) or any(chains.get(t) in local_vars for t in targets)


# ---------------------------------------------------------------- helpers

def _reindex(m: Module):
    for d in (m.by_result, m.names, m.member_names, m.decorations,
              m.member_decorations):
        d.clear()
    m.entry_points.clear()
    m._index()


def _signed(lit: int) -> int:
    return lit - (1 << 32) if lit >= (1 << 31) else lit


def _const_tables(m: Module):
    int_types, float_types = set(), set()
    int_tid = float_tid = None
    for ins in m.instrs:
        if ins.opcode == TYPE_INT:
            int_types.add(ins.result_id)
            if ins.operands[0] == 32 and ins.operands[1] == 1 and int_tid is None:
                int_tid = ins.result_id
        elif ins.opcode == TYPE_FLOAT:
            float_types.add(ins.result_id)
            if ins.operands[0] == 32 and float_tid is None:
                float_tid = ins.result_id
    iconst, fconst = {}, {}
    for ins in m.instrs:
        if ins.opcode == CONSTANT:
            t, lit = ins.result_type, ins.operands[0]
            if t in int_types:
                iconst[ins.result_id] = _signed(lit)
            elif t in float_types:
                fconst[ins.result_id] = struct.unpack(
                    "<f", struct.pack("<I", lit))[0]
    return iconst, fconst, int_tid, float_tid


def _ifold(op: int, a: int, b: int) -> int:
    if op == IADD:
        return a + b
    if op == ISUB:
        return a - b
    if op == IMUL:
        return a * b
    if op == SDIV:
        return int(a / b) if b != 0 else 0     # SPIR-V SDiv truncates to zero
    raise UnrollError(f"unhandled int op {op}")


def _icmp(op: int, a: int, b: int) -> bool:
    return {
        170: a == b, 171: a != b,
        172: a > b, 173: a > b,
        174: a >= b, 175: a >= b,
        176: a < b, 177: a < b,
        178: a <= b, 179: a <= b,
    }[op]


# --------------------------------------------------------------- main pass

def _rewrite_function(m: Module):
    instrs = m.instrs
    func_idx = next(i for i, ins in enumerate(instrs) if ins.opcode == FUNCTION)
    end_idx = next(i for i, ins in enumerate(instrs) if ins.opcode == FUNCTION_END)
    entry_idx = next(i for i in range(func_idx, end_idx)
                     if instrs[i].opcode == LABEL)
    entry_label = instrs[entry_idx].result_id

    # block table: label id -> (middle instrs, terminator)
    blocks = {}
    i = entry_idx
    while i < end_idx:
        if instrs[i].opcode == LABEL:
            lbl = instrs[i].result_id
            mid, j = [], i + 1
            while j < end_idx and instrs[j].opcode not in (BRANCH, BRANCH_COND,
                                                           *RETURNS):
                mid.append(instrs[j])
                j += 1
            term = instrs[j] if j < end_idx else None
            blocks[lbl] = (mid, term)
            i = j + 1
        else:
            i += 1

    func_vars = {ins.result_id for ins in instrs[func_idx:end_idx]
                 if ins.opcode == VARIABLE and ins.operands[0] == STORAGE_FUNCTION}
    var_decls = [ins for ins in instrs[func_idx:end_idx]
                 if ins.opcode == VARIABLE and ins.operands[0] == STORAGE_FUNCTION]

    iconst, fconst, int_tid, float_tid = _const_tables(m)
    counter = [m.bound]
    minted, emitted = [], []
    env = {}                                 # Function var id -> value id

    def fresh():
        counter[0] += 1
        return counter[0] - 1

    def int_id(value):
        for cid, v in iconst.items():
            if v == value:
                return cid
        if int_tid is None:
            raise UnrollError("no 32-bit int type to materialise a constant")
        nid = fresh()
        minted.append(Instr(CONSTANT, (int_tid, nid, value & 0xFFFFFFFF),
                            int_tid, nid))
        iconst[nid] = value
        return nid

    def float_id(value):
        for cid, v in fconst.items():
            if v == value:
                return cid
        if float_tid is None:
            raise UnrollError("no 32-bit float type to materialise a constant")
        nid = fresh()
        bits = struct.unpack("<I", struct.pack("<f", value))[0]
        minted.append(Instr(CONSTANT, (float_tid, nid, bits), float_tid, nid))
        fconst[nid] = value
        return nid

    def remap(ins, lm):
        ops = list(ins.operands)
        rr = lambda x: lm.get(x, env.get(x, x))  # noqa: E731
        if ins.opcode == EXT_INST:        # set, instr#(literal), args...
            return [rr(ops[0]), ops[1]] + [rr(x) for x in ops[2:]]
        if ins.opcode == VEC_SHUFFLE:     # v1, v2, comps(literal)...
            return [rr(ops[0]), rr(ops[1])] + ops[2:]
        if ins.opcode == COMP_EXTRACT:    # composite, idx(literal)...
            return [rr(ops[0])] + ops[1:]
        return [rr(x) for x in ops]

    def clone_generic(ins, lm):
        nid = fresh()
        words = (ins.result_type, nid) + tuple(remap(ins, lm))
        emitted.append(Instr(ins.opcode, words, ins.result_type, nid))
        lm[ins.result_id] = nid

    def interpret(block_instrs, lm):
        """Symbolically execute a straight-line block; mutate env/emitted."""
        for ins in block_instrs:
            op = ins.opcode
            if op == VARIABLE:
                continue                              # declaration, kept apart
            if op == LOAD:
                ptr = ins.operands[0]
                if ptr in func_vars:
                    if ptr not in env:
                        raise UnrollError(f"local %{ptr} read before write")
                    lm[ins.result_id] = env[ptr]
                else:                                 # Load of AccessChain etc.
                    nid = fresh()
                    emitted.append(Instr(LOAD,
                                   (ins.result_type, nid, lm.get(ptr, ptr)),
                                   ins.result_type, nid))
                    lm[ins.result_id] = nid
            elif op == STORE:
                tgt, val = ins.operands[0], ins.operands[1]
                rval = lm.get(val, env.get(val, val))
                if tgt in func_vars:
                    env[tgt] = rval                   # mem2reg: stays in env
                else:                                 # store to an output, or
                    #                                   through the chain cloned
                    #                                   for gl_Position
                    emitted.append(Instr(STORE, (lm.get(tgt, tgt), rval),
                                         None, None))
            elif op == ACCESS_CHAIN and ins.operands[0] in func_vars:
                raise UnrollError("indexed access into a local not supported")
            elif op in (IADD, ISUB, IMUL, SDIV):
                a = lm.get(ins.operands[0], ins.operands[0])
                b = lm.get(ins.operands[1], ins.operands[1])
                if a not in iconst or b not in iconst:
                    raise UnrollError("non-constant integer op in loop")
                lm[ins.result_id] = int_id(_ifold(op, iconst[a], iconst[b]))
            elif op == CONVERT_S_TO_F:
                a = lm.get(ins.operands[0], ins.operands[0])
                if a not in iconst:
                    raise UnrollError("ConvertSToF of non-constant in loop")
                lm[ins.result_id] = float_id(float(iconst[a]))
            elif op == CONVERT_F_TO_S:
                a = lm.get(ins.operands[0], ins.operands[0])
                if a not in fconst:
                    raise UnrollError("ConvertFToS of non-constant in loop")
                lm[ins.result_id] = int_id(int(fconst[a]))
            elif op in (FNEGATE, FADD, FSUB, FMUL, FDIV):
                # fold scalar-constant float arithmetic (e.g. float(i)*0.2);
                # the lowerer can't split a binop with two constant operands.
                a = lm.get(ins.operands[0], ins.operands[0])
                if op == FNEGATE and a in fconst:
                    lm[ins.result_id] = float_id(-fconst[a])
                elif op != FNEGATE:
                    b = lm.get(ins.operands[1], ins.operands[1])
                    if a in fconst and b in fconst and not (op == FDIV
                                                            and fconst[b] == 0.0):
                        r = {FADD: fconst[a] + fconst[b],
                             FSUB: fconst[a] - fconst[b],
                             FMUL: fconst[a] * fconst[b],
                             FDIV: fconst[a] / fconst[b] if fconst[b] else 0.0}[op]
                        lm[ins.result_id] = float_id(float(r))
                    else:
                        clone_generic(ins, lm)
                else:
                    clone_generic(ins, lm)
            elif op in CMP_OPS:
                continue                              # if-condition: folded by
                #                                       _walk's _const_bool
            elif ins.result_id is None:
                continue                              # Nop / unmodelled
            else:                                     # value op: clone to SSA
                clone_generic(ins, lm)

    def expand_loop(header_lbl):
        """Unroll the loop whose header is header_lbl; return its merge label."""
        hmid, hterm = blocks[header_lbl]
        lm_ins = next(x for x in hmid if x.opcode == LOOP_MERGE)
        merge_lbl, continue_lbl = lm_ins.operands[0], lm_ins.operands[1]
        if hterm is None or hterm.opcode != BRANCH:
            raise UnrollError("unexpected loop header terminator")
        cond_lbl = hterm.operands[0]
        cmid, cterm = blocks[cond_lbl]
        if cterm.opcode != BRANCH_COND:
            raise UnrollError("loop condition is not a BranchConditional")
        cond_ssa, t_lbl, f_lbl = cterm.operands[:3]
        if merge_lbl not in (t_lbl, f_lbl):
            raise UnrollError("loop exit does not target the merge block")
        body_lbl = f_lbl if t_lbl == merge_lbl else t_lbl
        body_when_true = body_lbl == t_lbl

        cmp = m.by_result.get(cond_ssa)
        if cmp is None or cmp.opcode not in CMP_OPS:
            raise UnrollError("loop condition is not an integer comparison")
        a, b = cmp.operands[0], cmp.operands[1]
        la, lb = m.by_result.get(a), m.by_result.get(b)
        if la is not None and la.opcode == LOAD and la.operands[0] in func_vars \
                and b in iconst:
            iv, bound, iv_left = la.operands[0], iconst[b], True
        elif lb is not None and lb.opcode == LOAD and lb.operands[0] in func_vars \
                and a in iconst:
            iv, bound, iv_left = lb.operands[0], iconst[a], False
        else:
            raise UnrollError("condition not (induction var vs constant)")
        if iv not in env or env[iv] not in iconst:
            raise UnrollError("induction var not constant on loop entry")

        def runs():
            val = iconst[env[iv]]
            lhs, rhs = (val, bound) if iv_left else (bound, val)
            r = _icmp(cmp.opcode, lhs, rhs)
            return r if body_when_true else not r

        trips = 0
        while runs():
            trips += 1
            if trips > MAX_TRIP:
                raise UnrollError("loop trip count exceeds the unroll cap")
            before = env.get(iv)                      # may advance in body
            _walk(body_lbl, continue_lbl)             #   (while) or in the
            interpret(blocks[continue_lbl][0], {})    #   continue block (for)
            if env.get(iv) == before:
                raise UnrollError("loop induction variable does not advance")
        return merge_lbl

    def _const_bool(ssa):
        """Evaluate a comparison to a constant bool using the current env, or
        None if it is data-dependent."""
        c = m.by_result.get(ssa)
        if c is None or c.opcode not in CMP_OPS:
            return None

        def cint(x):
            if x in iconst:
                return iconst[x]
            lx = m.by_result.get(x)
            if lx is not None and lx.opcode == LOAD:
                v = env.get(lx.operands[0])
                if v in iconst:
                    return iconst[v]
            return None
        va, vb = cint(c.operands[0]), cint(c.operands[1])
        if va is None or vb is None:
            return None
        return _icmp(c.opcode, va, vb)

    def _walk(start_lbl, stop_lbl):
        cur = start_lbl
        while cur is not None and cur != stop_lbl:
            mid, term = blocks[cur]
            if any(x.opcode == LOOP_MERGE for x in mid):
                cur = expand_loop(cur)
                continue
            interpret(mid, {})
            if term is None or term.opcode in RETURNS:
                cur = None
            elif term.opcode == BRANCH:
                cur = term.operands[0]
            else:                                     # BranchConditional
                # Structured if/else with a CONSTANT condition (e.g. on the
                # induction variable): pick the taken arm, inline it, resume at
                # the selection merge. Data-dependent conditional stores can't
                # be represented in the env model -> raise (no miscompile).
                cval = _const_bool(term.operands[0])
                sel = next((x for x in mid if x.opcode == SEL_MERGE), None)
                if cval is None or sel is None:
                    raise UnrollError(
                        "data-dependent if/else in a loop body (v1)")
                merge = sel.operands[0]
                taken = term.operands[1] if cval else term.operands[2]
                if taken != merge:
                    _walk(taken, merge)
                cur = merge

    _walk(entry_label, None)

    # --- reassemble: head | minted consts | fn open | var decls | body ---
    new_instrs = (instrs[:func_idx] + minted + [instrs[func_idx],
                  instrs[entry_idx]] + var_decls + emitted + [instrs[end_idx]])
    m.instrs = new_instrs
    m.bound = counter[0]
    _reindex(m)
