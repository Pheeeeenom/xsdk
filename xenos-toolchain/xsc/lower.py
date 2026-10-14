#!/usr/bin/env python3
"""xsc lowering — SPIR-V function body -> Xenos shader container.

v1 scope: branch-free vertex and (next) fragment shaders. The body is
lowered by evaluating each output's value expression through the SSA def
chain and emitting Program-builder calls, scheduled into the Xenos phase
structure (vfetch -> alloc position -> transform -> alloc interpolators ->
interpolator exports for VS).

Instruction templates (ALU/vfetch/tfetch field skeletons) come from Xenia's
definitions in xsc/templates.py; the corpus is only their test vector.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xdis.container as container  # noqa: E402
import xdis.container_emit as ce  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xsc.templates import templates  # noqa: E402
from xsc.unroll import UnrollError, unroll  # noqa: E402
from xsc.resolve import (DEC_ARRAY_STRIDE, DEC_MATRIX_STRIDE, DEC_OFFSET, DEC_ROW_MAJOR, _decos,  # noqa: E402
                         member_decos, resolve_interface, resolve_types)
from xasm.program import (ABS_SOURCE, LIBXENON_REGISTERS, PixelProgram, RegisterBudget,  # noqa: E402
                          VertexProgram)

OPOS = int(xu.ExportRegister.kVSPosition)  # 62
_V = xu.AluVectorOpcode
_S = xu.AluScalarOpcode


class LowerError(Exception):
    pass


def _templates():
    return templates()


def _store_map(m: spirv.Module):
    """target-id -> value-id for every OpStore in the body."""
    out = {}
    for ins in m.instrs:
        if ins.opcode == 62:  # Store target value
            out[ins.operands[0]] = ins.operands[1]
    return out


def _check_local_stores(m: spirv.Module, local_vars, merged_local=None, merge_label=None):
    """Refuses a local the lowering would read wrong: it reads a local as its last store (_store_map), and the
    unroller rewrites the modules where that isn't what each load sees, but not those with branches. Left to refuse:
    a store into part of a local, a local stored more than once beside an if/else, and the if/else's own variable,
    merged after it, read before the merge."""
    chains = {ins.result_id: ins.operands[0] for ins in m.instrs if ins.opcode == 65}
    stored = set()
    merged = False
    for ins in m.instrs:
        if ins.opcode == 248 and ins.result_id == merge_label:
            merged = True
        if ins.opcode == 61 and merged_local is not None and not merged and \
                merged_local in (ins.operands[0], chains.get(ins.operands[0])):
            raise LowerError(f"local %{merged_local} read before the if/else that assigns it ends")
        if ins.opcode != 62:
            continue
        target = ins.operands[0]
        if chains.get(target) in local_vars:
            raise LowerError(f"a store into part of local %{chains[target]}")
        if target in local_vars and target != merged_local:
            if target in stored:
                raise LowerError(f"local %{target} stored more than once where the unroller doesn't rewrite it")
            stored.add(target)


def _access_chain_base(m: spirv.Module, ptr_id: int):
    """If ptr_id is an AccessChain, return (base_var_id, [index consts])."""
    ins = m.by_result.get(ptr_id)
    if ins is None or ins.opcode != 65:  # AccessChain
        return None
    base = ins.operands[0]
    idx_consts = []
    for w in ins.operands[1:]:
        c = m.by_result.get(w)
        if c is not None and c.opcode == 43:  # Constant
            idx_consts.append(c.operands[0])
    return base, idx_consts


def _signed_int(lit: int) -> int:
    return lit - (1 << 32) if lit >= (1 << 31) else lit


def _frag_coord_reads(m: spirv.Module, types: dict, var_id: int) -> set:
    """The components of gl_FragCoord a shader reads: all four through any use but one component's."""
    loads, used = set(), set()
    for ins in m.instrs:
        if ins.opcode == 61 and ins.operands[0] == var_id:                # Load
            loads.add(ins.result_id)
        elif ins.opcode == 65 and ins.operands[0] == var_id:              # AccessChain(var, component)
            c = m.by_result.get(ins.operands[1])
            used.update((c.operands[0],) if c is not None and c.opcode == 43 else range(4))
    for ins in m.instrs:
        if ins.opcode == 81:                                              # CompositeExtract(v, component)
            if ins.operands[0] in loads:
                used.add(ins.operands[1])
        elif ins.opcode == 79:                                            # VectorShuffle(a, b, components...)
            first = types.get(m.by_result[ins.operands[0]].result_type)
            size = first.count if first is not None and first.kind == "vector" else 1
            for base, vec in ((0, ins.operands[0]), (size, ins.operands[1])):
                if vec in loads:
                    used.update(c - base for c in ins.operands[2:] if base <= c < base + 4)
        elif any(w in loads for w in ins.operands[2 if ins.opcode == 12 else 0:]):
            used.update(range(4))
    return used


class ValueLowerer:
    """Shared SPIR-V value lowering for vertex and fragment stages.

    Stage setup still lives in lower_vertex/lower_fragment (vfetches, tfetch
    gather, exports, constants). This context owns the single source of truth
    for ALU source resolution and value-expression emission.
    """

    BCAST_X = sum(((0 - p) & 3) << (2 * p) for p in range(4))

    def __init__(self, *, m, si, program, templates, types, stores, first_reg,
                 inputs, local_store, stage, sample_reg=None, deriv_reg=None,
                 literal_base=0, allow_literals=True):
        self.m = m
        self.si = si
        self.p = program
        self.T = templates
        self.types = types
        self.stores = stores
        self.next_reg = first_reg
        self.inputs = inputs
        self.local_store = local_store
        self.stage = stage
        self.sample_reg = sample_reg if sample_reg is not None else {}
        self.deriv_reg = deriv_reg if deriv_reg is not None else {}
        self.allow_literals = allow_literals
        self.lit_base = literal_base
        self.lit_pool = {}
        self.lit_manifest = []
        self.op_cache = {}
        self.matrix_cache = {}
        self.local_values = {}
        # Matrix values whose registers hold columns, as a block's std140 bytes do.
        self.col_major = set()
        # The run-time index a0 holds, as set_a0's key, so reads at the same index share one MOVA.
        self.a0_key = None

    def fresh(self):
        r = self.next_reg
        self.next_reg += 1
        return r

    def vec_size(self, ssa):
        t = self.types.get(self.m.by_result[ssa].result_type)
        return t.count if t and t.kind == "vector" else 1

    def scalar_const(self, ssa):
        bits = self.m.by_result[ssa].operands[0] & 0xFFFFFFFF
        return struct.unpack("<f", struct.pack("<I", bits))[0]

    @staticmethod
    def is_zero(v):
        return v == 0.0

    @staticmethod
    def is_one(v):
        return v == 1.0

    def float_const01(self, ssa):
        v = self.float_const(ssa)
        if v is not None and (self.is_zero(v) or self.is_one(v)):
            return v
        return None

    def const_vector(self, ssa):
        ins = self.m.by_result.get(ssa)
        if ins is None:
            return None
        if ins.opcode == 43 and self.result_kind(ssa) == "float":
            return (self.scalar_const(ssa),)
        if ins.opcode == 44:                      # ConstantComposite
            vals = []
            for comp in ins.operands:
                ci = self.m.by_result.get(comp)
                if ci is None or ci.opcode != 43:
                    return None
                vals.append(self.scalar_const(comp))
            return tuple(vals)
        if ins.opcode == 80:                      # CompositeConstruct
            vals = []
            for part in ins.operands:
                v = self.float_const(part)
                if v is None:
                    return None
                vals.append(v)
            return tuple(vals)
        return None

    def axis_component(self, ssa):
        vals = self.const_vector(ssa)
        if vals is None:
            return None
        n = self.vec_size(ssa)
        vals = vals[:n]
        if len(vals) != n:
            return None
        axis = [i for i, v in enumerate(vals) if self.is_one(v)]
        if len(axis) != 1:
            return None
        if any(not self.is_zero(v) for i, v in enumerate(vals)
               if i != axis[0]):
            return None
        return axis[0]

    @staticmethod
    def signed_int(lit: int) -> int:
        return _signed_int(lit)

    def result_kind(self, ssa):
        ins = self.m.by_result.get(ssa)
        t = self.types.get(ins.result_type) if ins is not None else None
        return t.kind if t is not None else None

    def int_const(self, ssa, seen=None):
        """Return a folded signed-int scalar, or None when data-dependent."""
        seen = set() if seen is None else seen
        if ssa in seen:
            return None
        seen.add(ssa)
        ins = self.m.by_result.get(ssa)
        if ins is None:
            return None
        op = ins.opcode
        if op == 61:                              # Load
            src = ins.operands[0]
            if src in self.local_store:
                return self.int_const(self.local_store[src], seen)
            return None
        if op == 43 and self.result_kind(ssa) == "int":
            return self.signed_int(ins.operands[0] & 0xFFFFFFFF)
        if op == 110:                             # ConvertFToS
            v = self.float_const(ins.operands[0], seen)
            return int(v) if v is not None else None
        if op not in (128, 130, 132, 135):        # IAdd ISub IMul SDiv
            return None
        a = self.int_const(ins.operands[0], seen)
        b = self.int_const(ins.operands[1], seen)
        if a is None or b is None:
            return None
        if op == 128:
            return a + b
        if op == 130:
            return a - b
        if op == 132:
            return a * b
        return int(a / b) if b != 0 else 0

    def float_const(self, ssa, seen=None):
        """Return a folded scalar float, or None when data-dependent."""
        seen = set() if seen is None else seen
        if ssa in seen:
            return None
        seen.add(ssa)
        ins = self.m.by_result.get(ssa)
        if ins is None:
            return None
        op = ins.opcode
        if op == 61:                              # Load
            src = ins.operands[0]
            if src in self.local_store:
                return self.float_const(self.local_store[src], seen)
            return None
        if op == 43 and self.result_kind(ssa) == "float":
            return self.scalar_const(ssa)
        if op == 111:                             # ConvertSToF
            v = self.int_const(ins.operands[0], seen)
            return float(v) if v is not None else None
        if op == 127:                             # FNegate
            v = self.float_const(ins.operands[0], seen)
            return -v if v is not None else None
        if op not in (129, 131, 133, 136):        # FAdd FSub FMul FDiv
            return None
        a = self.float_const(ins.operands[0], seen)
        b = self.float_const(ins.operands[1], seen)
        if a is None or b is None:
            return None
        if op == 129:
            return a + b
        if op == 131:
            return a - b
        if op == 133:
            return a * b
        return a / b if b != 0.0 else 0.0

    def literal_reg(self, value4):
        if not self.allow_literals:
            raise LowerError("vertex literal constants need a host manifest")
        key = tuple(round(x, 9) for x in value4)
        if key in self.lit_pool:
            return self.lit_pool[key]
        reg = self.lit_base
        self.lit_base += 1
        self.lit_pool[key] = reg
        self.lit_manifest.append((reg, tuple(value4)))
        return reg

    def const_base(self, ssa_id: int) -> tuple:
        """Resolve a Load of a uniform -> (constant base reg, reg count)."""
        ins = self.m.by_result.get(ssa_id)
        if ins is None or ins.opcode != 61:
            raise LowerError("matrix operand must be a uniform Load")
        role = self.si.roles.get(ins.operands[0])
        if not role or role[0] != "constant":
            raise LowerError("expected a uniform constant")
        return role[1]

    def src(self, fields, n, opnd, negate=False, swizzle=0):
        kind, val = opnd
        fields[f"src{n}_reg"] = val
        fields[f"src{n}_is_temp"] = (kind == "reg")
        fields[f"src{n}_swizzle"] = swizzle
        fields[f"src{n}_negate"] = negate

    def emit(self, opcode, ops, dst, export, mask=0xF, clamp=False):
        consts = sorted({v[0][1] for v in ops.values() if v[0][0] == "const"})
        if len(consts) > 1:
            # One constant register an instruction (v1): the others are moved into temps first.
            ops = {n: (self.to_reg(v[0]),) + tuple(v[1:]) if v[0][0] == "const" and v[0][1] != consts[0] else v
                   for n, v in ops.items()}
        f = dict(self.T["mov"], vector_opcode=opcode, vector_dest=dst,
                 is_export=export, vector_write_mask=mask,
                 vector_clamp=clamp)
        for n, v in ops.items():
            opnd, neg = v[0], v[1]
            swz = v[2] if len(v) > 2 else 0
            self.src(f, n, opnd, neg, swz)
        self.p.alu(**f)

    def emit_copy(self, opnd, dst=None, export=False, negate=False,
                  swizzle=0, mask=0xF, clamp=False):
        if dst is None and not export and not negate and swizzle == 0 and \
                not clamp and opnd[0] == "reg":
            return opnd[1]
        if dst is None:
            dst = self.fresh()
        self.emit(_V.kMax, {1: (opnd, negate, swizzle),
                            2: (opnd, negate, swizzle)},
                  dst, export, mask=mask, clamp=clamp)
        return dst

    def emit_export_constants(self, dst_reg, zero_mask=0, one_mask=0):
        if not zero_mask and not one_mask:
            return
        f = dict(self.T["mov"], vector_opcode=_V.kMax, vector_dest=dst_reg,
                 is_export=True, vector_write_mask=one_mask,
                 scalar_write_mask=one_mask,
                 is_scalar_dest_relative=bool(zero_mask),
                 vector_clamp=False)
        self.p.alu(**f)

    def split_index(self, ssa):
        """A run-time int index -> (the part only known at run time, the constant added to it), so the constant
        part can go in the register number: rows[i + 1] reads c[a0 + base + 1] with a0 = i."""
        c = self.int_const(ssa)
        if c is not None:
            return None, c
        ins = self.m.by_result[ssa]
        if ins.opcode == 61 and ins.operands[0] in self.local_store:      # Load of a local
            return self.split_index(self.local_store[ins.operands[0]])
        if ins.opcode in (128, 130):                                      # IAdd ISub
            cb = self.int_const(ins.operands[1])
            if cb is not None:
                part, k = self.split_index(ins.operands[0])
                return part, k + (cb if ins.opcode == 128 else -cb)
            ca = self.int_const(ins.operands[0])
            if ca is not None and ins.opcode == 128:
                part, k = self.split_index(ins.operands[1])
                return part, k + ca
        return ssa, 0

    def block_access(self, ptr_id):
        """An AccessChain into a uniform or push constant block -> (register,
        first component, type id, member decorations, run-time index); None for
        other pointers. One array index may be known only at run time: the
        run-time index is then (its run-time part, registers an element), and
        the register is the one its run-time part 0 would read."""
        ptr = self.m.by_result.get(ptr_id)
        if ptr is None or ptr.opcode not in (65, 66):
            return None
        role = self.si.roles.get(ptr.operands[0])
        if not role or role[0] != "block":
            return None
        block = self.si.blocks[role[1]]
        tid, offset, member, dyn = block.struct, 0, {}, None
        for idx_id in ptr.operands[1:]:
            idx = self.int_const(idx_id)
            t = self.types[tid]
            if idx is None:
                if t.kind != "array" or dyn is not None:
                    raise LowerError("a run-time index other than one into a block's array (v1)")
                stride = _decos(self.m, tid).get(DEC_ARRAY_STRIDE, (16,))[0]
                if stride % 16:
                    raise LowerError("a run-time index into an array not on 16-byte lines (v1)")
                part, idx = self.split_index(idx_id)
                dyn = (part, stride // 16)
            if t.kind == "struct":
                member = member_decos(self.m, tid, idx)
                offset += member.get(DEC_OFFSET, (16 * idx,))[0]
                tid = t.members[idx]
            elif t.kind == "matrix":
                if DEC_ROW_MAJOR in member:
                    raise LowerError("a column of a row-major block matrix (v1)")
                offset += idx * member.get(DEC_MATRIX_STRIDE, (16,))[0]
                tid, member = t.comp, {}
            elif t.kind == "vector":
                offset += 4 * idx
                tid = t.comp
            elif t.kind == "array":
                offset += idx * _decos(self.m, tid).get(DEC_ARRAY_STRIDE, (16,))[0]
                tid = t.comp
            else:
                raise LowerError(f"index into a block {t.kind} (v1)")
        return block.reg + offset // 16, (offset % 16) // 4, tid, member, dyn

    def set_a0(self, part, scale):
        """a0 = a run-time index's run-time part times scale, the registers an element, unless a0 holds it already.
        int(f) sets it by MOVA_FLOOR from f, which is the same for f >= 0, as indices are; any other index is a whole
        number by then, which MOVA rounds to itself. a0 is -256 to 255 (xenia:src/xenia/gpu/ucode.h:1138-1150)."""
        if self.a0_key == (part, scale):
            return
        ins = self.m.by_result[part]
        if ins.opcode == 110 and scale == 1:                              # ConvertFToS
            opnd, comp = self.scalar_origin(ins.operands[0])
            sop = _S.kMaxAsf
        else:
            opnd, comp, sop = self.int_operand(part), 0, _S.kMaxAs
            if scale != 1:
                r = self.fresh()
                self.emit(_V.kMul, {1: (opnd, False), 2: (("const", self.literal_reg((float(scale),) * 4)), False)},
                          r, False)
                opnd = ("reg", r)
        kind, reg = opnd
        # The scalar op reads its operands a and b from components w and x of src3, both the index's here
        # (xenia:src/xenia/gpu/shader_translator.cc:1246).
        f = dict(self.T["mov"], vector_opcode=_V.kMax, vector_dest=0, vector_write_mask=0, vector_clamp=False,
                 scalar_opcode=sop, scalar_dest=0, scalar_write_mask=0, scalar_clamp=False, is_export=False,
                 src3_reg=reg, src3_is_temp=(kind == "reg"), src3_swizzle=self.bsw(0 if comp == "B" else comp),
                 src3_negate=False)
        self.p.alu(**f)
        self.a0_key = (part, scale)

    def block_register(self, access, extra=0):
        """The operand holding a block access's register, plus extra registers: the constant itself, or at a run-time
        index c[a0 + register] copied into a temp, so a later index can't change what it read."""
        reg, _comp, _tid, _member, dyn = access
        if dyn is None:
            return ("const", reg + extra)
        self.set_a0(*dyn)
        r = self.fresh()
        f = dict(self.T["mov"], vector_opcode=_V.kMax, vector_dest=r, is_export=False, vector_write_mask=0xF,
                 vector_clamp=False, src1_const_rel=True, src2_const_rel=True,
                 is_const_address_register_relative=True)
        self.src(f, 1, ("const", reg + extra))
        self.src(f, 2, ("const", reg + extra))
        self.p.alu(**f)
        return ("reg", r)

    def block_value(self, ssa_id, access):
        """A Load of a block member as an operand."""
        _reg, comp, tid, _member, _dyn = access
        t = self.types[tid]
        if t.kind == "matrix":
            return ("mat", self.matrix_operands(ssa_id))
        n = t.count if t.kind == "vector" else 1
        if t.kind == "vector" and comp == 0:
            return self.block_register(access)
        if comp + n > 4:
            raise LowerError("a block member across registers (v1)")
        swz = self.bsw(comp) if n == 1 else self.swz([comp + i for i in range(n)])
        src = self.block_register(access)
        r = self.fresh()
        self.emit(_V.kMax, {1: (src, False, swz), 2: (src, False, swz)}, r, False)
        return ("reg", r)

    def block_matrix(self, ssa_id, access):
        """A block matrix's registers: columns (marked) unless it's row-major."""
        _reg, comp, tid, member, _dyn = access
        t = self.types[tid]
        col = self.types.get(t.comp)
        rows = col.count if col and col.kind == "vector" else 1
        if comp or member.get(DEC_MATRIX_STRIDE, (16,))[0] != 16:
            raise LowerError("a block matrix not on 16-byte lines (v1)")
        if DEC_ROW_MAJOR in member:
            return tuple(self.block_register(access, i) for i in range(rows))
        self.col_major.add(ssa_id)
        return tuple(self.block_register(access, i) for i in range(t.count))

    def matrix_operands(self, ssa_id):
        """Resolve a mat4 value to four vector operands (one per register)."""
        if ssa_id in self.matrix_cache:
            return self.matrix_cache[ssa_id]
        ins = self.m.by_result.get(ssa_id)
        if ins is None:
            raise LowerError(f"%{ssa_id} is not a matrix value")
        if ins.opcode == 61:                       # Load
            src = ins.operands[0]
            access = self.block_access(src)
            if access is not None:
                res = self.block_matrix(ssa_id, access)
            elif src in self.local_store:
                res = self.matrix_operands(self.local_store[src])
            else:
                role = self.si.roles.get(src)
                if not role or role[0] != "constant":
                    raise LowerError("matrix operand must be a uniform Load")
                base, regs = role[1]
                if regs not in self._MAT_DP:
                    raise LowerError(
                        f"matrix operand expects mat2/3/4, got {regs} regs")
                res = tuple(("const", base + i) for i in range(regs))
        elif ins.opcode == 146:                    # MatrixTimesMatrix
            res = self.emit_mat_mat(ins.operands[0], ins.operands[1])
            if ins.operands[0] in self.col_major:
                self.col_major.add(ssa_id)
        elif ins.opcode == 84 and self.is_col_major(ins.operands[0]):
            # A transpose of columns is the same registers read as rows.
            res = self.matrix_operands(ins.operands[0])
        elif ins.opcode == 84:                     # OpTranspose
            res = self.emit_transpose(ins.operands[0])
        elif ins.opcode == 12 and ins.operands[1] == 34:   # MatrixInverse
            res = self.emit_inverse(ins.operands[2])
        else:
            raise LowerError(
                f"v1 {self.stage}: unhandled matrix op {ins.name}")
        self.matrix_cache[ssa_id] = res
        return res

    def emit_transpose(self, mat_ssa):
        """transpose(M): row i of the result is column i of M -- gather
        component i of every input row."""
        rows = self.matrix_operands(mat_ssa)
        n = len(rows)
        if n not in self._MAT_DP:
            raise LowerError(f"transpose expects mat2/3/4, got {n}")
        rows = [self.to_reg(r) for r in rows]
        out = []
        for i in range(n):                         # new row i = column i
            r = self.fresh()
            for j in range(n):                     # gather rows[j][i] -> .j
                swz = ((i - j) & 3) << (2 * j)
                self.emit(_V.kMax, {1: (rows[j], False, swz),
                                    2: (rows[j], False, swz)}, r, False,
                          mask=(1 << j))
            out.append(("reg", r))
        return tuple(out)

    def emit_determinant(self, mat_ssa):
        """determinant(M) -> a broadcast scalar reg. mat2 = ad-bc; mat3 =
        row0 . cross(row1, row2)."""
        rows = [self.to_reg(r) for r in self.matrix_operands(mat_ssa)]
        n = len(rows)
        if n == 2:                                 # det = r0.x*r1.y - r0.y*r1.x
            yx = self.swz([1, 0, 0, 0])
            prod = self.fresh()
            self.emit(_V.kMul, {1: (rows[0], False),
                                2: (rows[1], False, yx)}, prod, False)
            r = self.fresh()                       # prod.x - prod.y
            self.emit(_V.kAdd, {1: (("reg", prod), False, self.bsw(0)),
                                2: (("reg", prod), True, self.bsw(1))},
                      r, False)
            return r
        if n == 3:                                 # r0 . cross(r1, r2)
            cr = self._cross(rows[1], rows[2])
            r = self.fresh()
            self.emit(_V.kDp3, {1: (rows[0], False),
                                2: (("reg", cr), False)}, r, False, mask=0xF)
            return r
        raise LowerError("determinant supports mat2/mat3")

    def _cross(self, a_op, b_op):
        """cross(a, b) -> a fresh register (vec3 in xyz)."""
        yzx, zxy = self.swz([1, 2, 0, 0]), self.swz([2, 0, 1, 0])
        tc = self.fresh()
        self.emit(_V.kMul, {1: (a_op, False, zxy), 2: (b_op, False, yzx)},
                  tc, False)
        out = self.fresh()
        self.emit(_V.kMad, {1: (a_op, False, yzx), 2: (b_op, False, zxy),
                            3: (("reg", tc), True)}, out, False)
        return out

    def emit_inverse(self, mat_ssa):
        """inverse(M) for mat2/mat3 -> tuple of row operands. mat2 uses the
        closed form; mat3 uses adjugate(rows of cofactor cross-products) / det.
        Register-heavy for mat3 -- intended for vertex shaders."""
        rows = [self.to_reg(r) for r in self.matrix_operands(mat_ssa)]
        n = len(rows)
        det = self.emit_determinant(mat_ssa)
        idet = self.emit_scalar(_S.kRcp, self.fresh(), ("reg", det))
        if n == 2:                                 # (1/det) [[d,-b],[-c,a]]
            r0 = self.fresh()                      # [r1.y, -r0.y]
            self.emit(_V.kMul, {1: (rows[1], False, self.bsw(1)),
                                2: (("reg", idet), False)}, r0, False,
                      mask=0x1)
            self.emit(_V.kMul, {1: (rows[0], True, self.bsw(1)),
                                2: (("reg", idet), False)}, r0, False,
                      mask=0x2)
            r1 = self.fresh()                      # [-r1.x, r0.x]
            self.emit(_V.kMul, {1: (rows[1], True, self.bsw(0)),
                                2: (("reg", idet), False)}, r1, False,
                      mask=0x1)
            self.emit(_V.kMul, {1: (rows[0], False, self.bsw(0)),
                                2: (("reg", idet), False)}, r1, False,
                      mask=0x2)
            return (("reg", r0), ("reg", r1))
        if n == 3:
            # inverse = transpose(cofactor) / det; the cofactor rows are the
            # cross products of the OTHER two rows, and transposing them is the
            # column-gather below.
            c0 = self._cross(rows[1], rows[2])
            c1 = self._cross(rows[2], rows[0])
            c2 = self._cross(rows[0], rows[1])
            cof = [("reg", c0), ("reg", c1), ("reg", c2)]
            out = []
            for i in range(3):                     # transpose + scale by 1/det
                r = self.fresh()
                for j in range(3):
                    swz = ((i - j) & 3) << (2 * j)
                    self.emit(_V.kMul, {1: (cof[j], False, swz),
                                        2: (("reg", idet), False)},
                              r, False, mask=(1 << j))
                out.append(("reg", r))
            return tuple(out)
        raise LowerError("inverse supports mat2/mat3")

    # square-matrix dimension -> per-row dot opcode (rows uploaded as the
    # matrix's constant registers; (M*v)[i] = dot(v, row_i)).
    _MAT_DP = {4: _V.kDp4, 3: _V.kDp3, 2: _V.kDp2Add}

    def emit_mat_vec_ops(self, mat_ops, vec_op, dst=None,
                         export_position=False):
        n = len(mat_ops)
        if n not in self._MAT_DP:
            raise LowerError(f"matrix-vector expects mat2/3/4, got {n} rows")
        dpop = self._MAT_DP[n]
        vec_op = self.to_reg(vec_op)
        target = OPOS if export_position else (
            dst if dst is not None else self.fresh())
        for i, mat_op in enumerate(mat_ops):
            kw = dict(self.T["dp4"], vector_opcode=dpop, vector_dest=target,
                      is_export=export_position, vector_write_mask=(1 << i),
                      src1_reg=vec_op[1], src1_is_temp=True,
                      src2_reg=mat_op[1], src2_is_temp=(mat_op[0] == "reg"))
            if n == 2:                            # dp2add: zero the addend
                z = self.literal_reg((0.0, 0.0, 0.0, 0.0))
                kw.update(src3_reg=z, src3_is_temp=False, src3_swizzle=0)
            self.p.alu(**kw)
        return target

    def is_col_major(self, mat_ssa):
        self.matrix_operands(mat_ssa)
        return mat_ssa in self.col_major

    def emit_mat_vec(self, mat_ssa, vec_ssa, dst=None, export_position=False):
        # M * v over columns is sum(column_j * v_j), the row-vector form's multiply-add.
        if self.is_col_major(mat_ssa):
            return self.emit_vec_mat_ops(self.operand(vec_ssa), self.matrix_operands(mat_ssa), dst,
                                         export_position)
        return self.emit_mat_vec_ops(self.matrix_operands(mat_ssa),
                                     self.operand(vec_ssa), dst,
                                     export_position)

    def emit_vec_mat_ops(self, vec_op, mat_ops, dst=None,
                         export_position=False):
        n = len(mat_ops)
        if n not in self._MAT_DP:
            raise LowerError(f"vector-matrix expects mat2/3/4, got {n} rows")
        vec_op = self.to_reg(vec_op)
        target = self.fresh() if export_position else (
            dst if dst is not None else self.fresh())
        self.emit(_V.kMul, {1: (mat_ops[0], False),
                            2: (vec_op, False, self.bsw(0))},
                  target, False)
        for i in range(1, n):
            self.emit(_V.kMad, {1: (mat_ops[i], False),
                                2: (vec_op, False, self.bsw(i)),
                                3: (("reg", target), False)},
                      target, False)
        if export_position:
            self.emit_copy(("reg", target), OPOS, True)
        return target

    def emit_vec_mat(self, vec_ssa, mat_ssa, dst=None, export_position=False):
        # v * M over columns is dot(v, column_j) for each j.
        if self.is_col_major(mat_ssa):
            return self.emit_mat_vec_ops(self.matrix_operands(mat_ssa), self.operand(vec_ssa), dst,
                                         export_position)
        return self.emit_vec_mat_ops(self.operand(vec_ssa),
                                     self.matrix_operands(mat_ssa), dst,
                                     export_position)

    def emit_mat_mat(self, a_ssa, b_ssa):
        lhs = self.matrix_operands(a_ssa)
        rhs = self.matrix_operands(b_ssa)
        if self.is_col_major(a_ssa) != self.is_col_major(b_ssa):
            raise LowerError("a product of row and column matrices (v1)")
        if self.is_col_major(a_ssa):
            # Column k of A * B is A times column k of B.
            return tuple(("reg", self.emit_vec_mat_ops(col, lhs)) for col in rhs)
        rows = []
        for row in lhs:
            rows.append(("reg", self.emit_vec_mat_ops(row, rhs)))
        return tuple(rows)

    def int_operand(self, ssa_id):
        """Integer scalar represented as a float operand."""
        cv = self.int_const(ssa_id)
        if cv is not None:
            fv = float(cv)
            return ("const", self.literal_reg((fv, fv, fv, fv)))
        ins = self.m.by_result[ssa_id]
        op = ins.opcode
        if op == 61:                              # Load
            src = ins.operands[0]
            if src in self.local_store:
                return self.int_operand(self.local_store[src])
            raise LowerError("integer Load must be a foldable local")
        if op == 110:                             # ConvertFToS
            ov = self.operand(ins.operands[0])
            r = self.fresh()
            self.emit(_V.kTrunc, {1: (ov, False), 2: (ov, False)}, r, False)
            return ("reg", r)
        if op in (128, 130, 132):                 # IAdd ISub IMul
            ca = self.int_const(ins.operands[0])
            cb = self.int_const(ins.operands[1])
            if op == 128:
                if ca == 0:
                    return self.int_operand(ins.operands[1])
                if cb == 0:
                    return self.int_operand(ins.operands[0])
            elif op == 130 and cb == 0:
                return self.int_operand(ins.operands[0])
            elif op == 132:
                if ca == 1:
                    return self.int_operand(ins.operands[1])
                if cb == 1:
                    return self.int_operand(ins.operands[0])
            oa = self.int_operand(ins.operands[0])
            ob = self.int_operand(ins.operands[1])
            r = self.fresh()
            if op == 132:
                self.emit(_V.kMul, {1: (oa, False), 2: (ob, False)}, r, False)
            elif op == 128:
                self.emit(_V.kAdd, {1: (oa, False), 2: (ob, False)}, r, False)
            else:
                self.emit(_V.kAdd, {1: (oa, False), 2: (ob, True)}, r, False)
            return ("reg", r)
        if op in (134, 135, 137, 138, 139):       # UDiv/SDiv/UMod/SRem/SMod
            # Float emulation: q = trunc(a*rcp(b) + eps). rcp is approximate, so
            # an exact multiple can come out as x.9998 and truncate to x-1; the
            # small positive bias restores it. Correct for the non-negative
            # integers shaders actually use (indices, counts); negative operands
            # are approximate. eps < 1/256 so it never crosses a real boundary
            # for divisors up to 256.
            oa = self.int_operand(ins.operands[0])
            ob = self.int_operand(ins.operands[1])
            rb = self.emit_scalar(_S.kRcp, self.fresh(), ob)
            q = self.fresh()
            self.emit(_V.kMad, {1: (oa, False), 2: (("reg", rb), False),
                                3: (("const", self.literal_reg(
                                    (1.0 / 1024,) * 4)), False)}, q, False)
            t = self.fresh()
            self.emit(_V.kTrunc, {1: (("reg", q), False),
                                  2: (("reg", q), False)}, t, False)
            if op in (134, 135):                  # division -> quotient
                return ("reg", t)
            r = self.fresh()                      # remainder: a - b*trunc(a/b)
            self.emit(_V.kMad, {1: (ob, True), 2: (("reg", t), False),
                                3: (oa, False)}, r, False)
            return ("reg", r)
        raise LowerError(f"v1 {self.stage}: unsupported integer op {ins.name}")

    # Comparison opcode -> (Xenos set op, swap operands). These yield 1.0 where
    # the comparison is true and 0.0 elsewhere, component-wise -- the float
    # representation of a GLSL bool / bvec. Unsigned compares fold onto the
    # signed set ops (exact for the small non-negative ints shaders use).
    _CMP_VALUE = {
        186: (_V.kSgt, False), 184: (_V.kSgt, True),    # FOrd >, <
        190: (_V.kSge, False), 188: (_V.kSge, True),    # FOrd >=, <=
        180: (_V.kSeq, False), 182: (_V.kSne, False),   # FOrd ==, !=
        173: (_V.kSgt, False), 177: (_V.kSgt, True),    # S >, <
        175: (_V.kSge, False), 179: (_V.kSge, True),    # S >=, <=
        172: (_V.kSgt, False), 176: (_V.kSgt, True),    # U >, <
        174: (_V.kSge, False), 178: (_V.kSge, True),    # U >=, <=
        170: (_V.kSeq, False), 171: (_V.kSne, False),   # I ==, !=
    }

    def compare_value(self, ins, dst, export=False):
        """A relational op used as a value: 1.0 where true else 0.0,
        component-wise (the float form of a GLSL bool/bvec)."""
        xop, swap = self._CMP_VALUE[ins.opcode]
        a, b = self.operand(ins.operands[0]), self.operand(ins.operands[1])
        if swap:
            a, b = b, a
        self.emit(xop, {1: (a, False), 2: (b, False)}, dst, export)
        return dst

    def bool01(self, ssa):
        """Operand holding a bool/bvec as 0.0/1.0 floats."""
        ins = self.m.by_result[ssa]
        if ins.opcode in self._CMP_VALUE:
            r = self.fresh()
            self.compare_value(ins, r, False)
            return ("reg", r)
        return self.operand(ssa)              # any/all/not/logical already 0/1

    def select_value(self, ins, dst, export=False):
        """OpSelect(cond, t, f) -> f + (t - f)*cond, component-wise. Works for
        a scalar cond broadcast over a vector and for a component-wise bvec."""
        c = self.bool01(ins.operands[0])
        tr = self.to_reg(self.operand(ins.operands[1]))
        fr = self.to_reg(self.operand(ins.operands[2]))
        diff = self.fresh()
        self.emit(_V.kAdd, {1: (tr, False), 2: (fr, True)}, diff, False)
        self.emit(_V.kMad, {1: (("reg", diff), False), 2: (c, False),
                            3: (fr, False)}, dst, export)
        return dst

    def reduce_bool(self, ins, dst, export=False, is_all=False):
        """any(bvec) = horizontal max, all(bvec) = horizontal min, over the
        0/1 components."""
        vr = self.to_reg(self.bool01(ins.operands[0]))
        n = max(self.vec_size(ins.operands[0]), 1)
        op = _V.kMin if is_all else _V.kMax
        acc = self.fresh()
        self.emit(_V.kMax, {1: (vr, False, self.bsw(0)),
                            2: (vr, False, self.bsw(0))}, acc, False)
        for i in range(1, n):
            self.emit(op, {1: (("reg", acc), False),
                           2: (vr, False, self.bsw(i))}, acc, False)
        self.emit(_V.kMax, {1: (("reg", acc), False),
                            2: (("reg", acc), False)}, dst, export)
        return dst

    def logical_not_value(self, ins, dst, export=False):
        """!bool / not(bvec) = 1.0 - v over 0/1 components."""
        v = self.bool01(ins.operands[0])
        one = ("const", self.literal_reg((1.0, 1.0, 1.0, 1.0)))
        self.emit(_V.kAdd, {1: (one, False), 2: (v, True)}, dst, export)
        return dst

    def logical_binop_value(self, ins, dst, export=False):
        """&& = min, || = max over 0/1 bool operands."""
        a, b = self.bool01(ins.operands[0]), self.bool01(ins.operands[1])
        op = _V.kMin if ins.opcode == 167 else _V.kMax
        self.emit(op, {1: (a, False), 2: (b, False)}, dst, export)
        return dst

    def operand(self, ssa_id: int):
        """Resolve an SSA value to an operand, emitting once."""
        if ssa_id in self.op_cache:
            return self.op_cache[ssa_id]
        ins = self.m.by_result[ssa_id]
        op = ins.opcode
        if op in self._CMP_VALUE:
            r = self.fresh()
            self.compare_value(ins, r, False)
            res = ("reg", r)
        elif op == 169:                           # OpSelect (ternary ?:)
            r = self.fresh()
            self.select_value(ins, r, False)
            res = ("reg", r)
        elif op in (154, 155):                    # any / all
            r = self.fresh()
            self.reduce_bool(ins, r, False, is_all=(op == 155))
            res = ("reg", r)
        elif op == 168:                           # logical not / not(bvec)
            r = self.fresh()
            self.logical_not_value(ins, r, False)
            res = ("reg", r)
        elif op in (166, 167):                    # logical or / and
            r = self.fresh()
            self.logical_binop_value(ins, r, False)
            res = ("reg", r)
        elif self.result_kind(ssa_id) == "int":
            res = self.int_operand(ssa_id)
        elif self.result_kind(ssa_id) == "float" and \
                self.float_const(ssa_id) is not None:
            v = self.float_const(ssa_id)
            res = ("const", self.literal_reg((v, v, v, v)))
        elif op == 61:                            # Load
            src = ins.operands[0]
            ptr = self.m.by_result.get(src)
            access = self.block_access(src)
            if access is not None:
                res = self.block_value(ssa_id, access)
            elif ptr is not None and ptr.opcode == 65:
                o, comp = self.scalar_origin(ssa_id)
                bsw = 0 if comp == "B" else self.bsw(comp)
                r = self.fresh()
                self.emit(_V.kMax, {1: (o, False, bsw),
                                    2: (o, False, bsw)}, r, False)
                res = ("reg", r)
            elif src in self.inputs:
                res = ("reg", self.inputs[src])
            elif src in self.local_values:
                res = self.local_values[src]
            elif src in self.local_store:
                res = self.operand(self.local_store[src])
            else:
                role = self.si.roles.get(src)
                if role and role[0] == "constant":
                    if role[1][1] > 1:
                        res = ("mat", self.matrix_operands(ssa_id))
                    else:
                        res = ("const", role[1][0])
                else:
                    raise LowerError(f"{self.stage}: unresolved Load %{src}")
        elif op in (87, 88, 91, 92):
            if ssa_id not in self.sample_reg:
                raise LowerError(
                    "ImageSample was not gathered before lowering")
            res = ("reg", self.sample_reg[ssa_id])
        elif op == 82:                            # CompositeInsert(obj,comp,i)
            base = self.to_reg(self.operand(ins.operands[1]))
            r = self.fresh()
            self.emit(_V.kMax, {1: (base, False), 2: (base, False)}, r, False)
            idx = ins.operands[2]
            o, c = self.scalar_origin(ins.operands[0])
            swz = 0 if c == "B" else ((c - idx) & 3) << (2 * idx)
            self.emit(_V.kMax, {1: (o, False, swz), 2: (o, False, swz)},
                      r, False, mask=(1 << idx))
            res = ("reg", r)
        elif op == 79:                            # VectorShuffle
            idxs = list(ins.operands[2:])
            if idxs == list(range(len(idxs))):
                res = self.operand(ins.operands[0])
            elif all(i < 4 for i in idxs):
                base = self.operand(ins.operands[0])
                swz = self.swz(idxs)
                r = self.fresh()
                self.emit(_V.kMax, {1: (base, False, swz),
                                    2: (base, False, swz)}, r, False)
                res = ("reg", r)
            else:
                raise LowerError(f"two-source swizzle {idxs} (v1)")
        elif op == 145:                           # MatrixTimesVector
            res = ("reg", self.emit_mat_vec(ins.operands[0], ins.operands[1]))
        elif op == 144:                           # VectorTimesMatrix
            res = ("reg", self.emit_vec_mat(ins.operands[0], ins.operands[1]))
        elif op == 146:                           # MatrixTimesMatrix
            res = ("mat", self.matrix_operands(ssa_id))
        elif op == 84:                            # OpTranspose
            res = ("mat", self.matrix_operands(ssa_id))
        elif op in (129, 131, 133):               # FAdd FSub FMul
            res = ("reg", self.binop(op, ins.operands[0], ins.operands[1]))
        elif op == 142:                           # VectorTimesScalar
            res = ("reg", self.vts(ins.operands[0], ins.operands[1]))
        elif op == 148:                           # OpDot
            res = ("reg", self.dot(ins.operands[0], ins.operands[1]))
        elif op == 136:                           # OpFDiv
            res = ("reg", self.fdiv(ins.operands[0], ins.operands[1]))
        elif op == 141:                           # OpFMod
            res = ("reg", self.fmod(ins.operands[0], ins.operands[1]))
        elif op == 43:                            # OpConstant
            v = self.scalar_const(ssa_id)
            res = ("const", self.literal_reg((v, v, v, v)))
        elif op == 44:                            # OpConstantComposite
            comps = [self.scalar_const(c) for c in ins.operands]
            comps = (comps + [0.0, 0.0, 0.0, 0.0])[:4]
            res = ("const", self.literal_reg(tuple(comps)))
        elif op == 80:                            # CompositeConstruct
            parts = ins.operands
            if len(set(parts)) == 1:
                if self.m.by_result[parts[0]].opcode == 43:
                    v = self.scalar_const(parts[0])
                    res = ("const", self.literal_reg((v, v, v, v)))
                else:
                    res = self.operand(parts[0])
            else:
                r = self.fresh()
                self.emit_construct(ins, r, export=False)
                res = ("reg", r)
        elif op == 12 and ins.operands[1] == 34:  # MatrixInverse -> matrix
            res = ("mat", self.matrix_operands(ssa_id))
        elif op == 12:                            # ExtInst (scalar/vector)
            res = ("reg", self.extinst(ins))
        elif op == 111:                           # ConvertSToF
            res = self.int_operand(ins.operands[0])
        elif op in (194, 195, 196, 197, 198, 199, 200):
            raise LowerError(
                f"{ins.name}: bitwise/shift operators have no Xenos float-ALU "
                "equivalent (the GPU has no bit ops); restructure with "
                "arithmetic, or precompute on the host")
        elif op in (207, 208, 209):               # dFdx / dFdy / fwidth
            if ssa_id not in self.deriv_reg:
                raise LowerError(
                    "derivative was not gathered before lowering")
            res = ("reg", self.deriv_reg[ssa_id])
        else:
            raise LowerError(f"v1 {self.stage}: unhandled op {ins.name}")
        self.op_cache[ssa_id] = res
        return res

    def const_scalar(self, ssa):
        """Scalar constant, possibly wrapped in a broadcast construct."""
        ins = self.m.by_result[ssa]
        if ins.opcode == 43:
            return self.scalar_const(ssa)
        if ins.opcode == 80 and len(set(ins.operands)) == 1 and \
                self.m.by_result[ins.operands[0]].opcode == 43:
            return self.scalar_const(ins.operands[0])
        raise LowerError(f"%{ssa} is not a constant scalar")

    def const_val(self, ssa_id):
        c = self.m.by_result.get(ssa_id)
        if c is None or c.opcode != 43:
            raise LowerError(f"%{ssa_id} is not an int constant")
        return c.operands[0]

    def scalar_origin(self, ssa_id):
        """Resolve a scalar SSA to (vector_operand, component). Component is an
        int, or "B" when the value is broadcast across all components."""
        ins = self.m.by_result[ssa_id]
        if ins.opcode == 81:                      # CompositeExtract(v, idx)
            return self.operand(ins.operands[0]), ins.operands[1]
        if ins.opcode == 61:                      # Load
            src = ins.operands[0]
            ptr = self.m.by_result.get(src)
            access = self.block_access(src)
            if access is not None and self.types[access[2]].kind != "vector":
                return self.block_register(access), access[1]
            if access is not None:
                return self.block_value(ssa_id, access), "B"
            if ptr is not None and ptr.opcode == 65:
                base, comp = ptr.operands[0], self.const_val(ptr.operands[1])
                if base in self.local_values:
                    return self.local_values[base], comp
                if base in self.local_store:
                    return self.operand(self.local_store[base]), comp
                if base in self.inputs:
                    return ("reg", self.inputs[base]), comp
                role = self.si.roles.get(base)
                if role and role[0] == "constant":
                    return ("const", role[1][0]), comp
            if src in self.local_values:
                return self.local_values[src], "B"
            if src in self.local_store:
                return self.operand(self.local_store[src]), "B"
            role = self.si.roles.get(src)
            if role and role[0] == "constant":
                return ("const", role[1][0]), 0
            if src in self.inputs:
                return ("reg", self.inputs[src]), 0
        if ins.opcode in (148, 43):
            return self.operand(ssa_id), "B"
        if ins.opcode in (129, 131, 133, 136, 141, 12):
            return self.operand(ssa_id), "B"
        # select / any / all / not / logical / relational results: produced by
        # operand() as a (broadcast) value.
        if ins.opcode in (169, 154, 155, 168, 166, 167) or \
                ins.opcode in self._CMP_VALUE:
            return self.operand(ssa_id), "B"
        raise LowerError(f"cannot resolve scalar %{ssa_id}")

    def emit_construct(self, ins, dst_reg, export=True):
        origins = []
        for part in ins.operands:
            v = self.float_const01(part) if export else None
            if v is not None:
                origins.append(("lit01", v))
            else:
                origins.append(("dyn", self.scalar_origin(part)))
        n = len(origins)
        if export:
            zero_mask = sum(1 << i for i, o in enumerate(origins)
                            if o[0] == "lit01" and self.is_zero(o[1]))
            one_mask = sum(1 << i for i, o in enumerate(origins)
                           if o[0] == "lit01" and self.is_one(o[1]))
            self.emit_export_constants(dst_reg, zero_mask, one_mask)
        j = 0
        while j < n:
            if origins[j][0] == "lit01":
                j += 1
                continue
            op_j = origins[j][1][0]
            k = j
            while k < n and origins[k][0] == "dyn" and \
                    origins[k][1][0] == op_j and \
                    origins[k][1][1] in (k, "B"):
                k += 1
            if k > j:
                mask = sum(1 << t for t in range(j, k))
                self.emit(_V.kMax, {1: (op_j, False), 2: (op_j, False)},
                          dst_reg, export, mask=mask)
                j = k
            else:
                op, comp = origins[j][1]
                swz = 0 if comp == "B" else ((comp - j) & 3) << (2 * j)
                self.emit(_V.kMax, {1: (op, False, swz),
                                    2: (op, False, swz)},
                          dst_reg, export, mask=(1 << j))
                j += 1

    def zero_construct_parts(self, ssa):
        ins = self.m.by_result.get(ssa)
        if ins is None:
            return None
        if ins.opcode == 44:                      # ConstantComposite
            parts = ins.operands
        elif ins.opcode == 80:                    # CompositeConstruct
            parts = ins.operands
        else:
            return None
        out = []
        for part in parts:
            v = self.float_const(part)
            if v is None:
                out.append(part)
            elif self.is_zero(v):
                out.append(None)
            else:
                return None
        return out

    def add_zero_construct(self, base_ssa, construct_ssa, dst=None,
                           export=False, negate_construct=False,
                           clamp=False):
        parts = self.zero_construct_parts(construct_ssa)
        if parts is None:
            return None
        base = self.operand(base_ssa)
        if dst is None:
            if all(part is None for part in parts):
                return self.emit_copy(base, export=export, clamp=clamp)
            dst = self.fresh()
        zero_mask = sum(1 << i for i, part in enumerate(parts)
                        if part is None)
        if zero_mask:
            self.emit_copy(base, dst, export, mask=zero_mask, clamp=clamp)
        for i, part in enumerate(parts):
            if part is None:
                continue
            op, comp = self.scalar_origin(part)
            swz = 0 if comp == "B" else ((comp - i) & 3) << (2 * i)
            self.emit(_V.kAdd, {1: (base, False),
                                2: (op, negate_construct, swz)},
                      dst, export, mask=(1 << i), clamp=clamp)
        return dst

    def binop(self, op, a, b, dst=None, export=False, clamp=False):
        ca, cb = self.float_const01(a), self.float_const01(b)
        if op == 129:                             # FAdd
            if ca == 0.0:
                return self.emit_copy(self.operand(b), dst, export,
                                      clamp=clamp)
            if cb == 0.0:
                return self.emit_copy(self.operand(a), dst, export,
                                      clamp=clamp)
            folded = self.add_zero_construct(a, b, dst, export,
                                             clamp=clamp)
            if folded is not None:
                return folded
            folded = self.add_zero_construct(b, a, dst, export,
                                             clamp=clamp)
            if folded is not None:
                return folded
        elif op == 131:                           # FSub
            if cb == 0.0:
                return self.emit_copy(self.operand(a), dst, export,
                                      clamp=clamp)
            if ca == 0.0:
                return self.emit_copy(self.operand(b), dst, export,
                                      negate=True, clamp=clamp)
            folded = self.add_zero_construct(a, b, dst, export,
                                             negate_construct=True,
                                             clamp=clamp)
            if folded is not None:
                return folded
        elif op == 133:                           # FMul
            if ca == 1.0:
                return self.emit_copy(self.operand(b), dst, export,
                                      clamp=clamp)
            if cb == 1.0:
                return self.emit_copy(self.operand(a), dst, export,
                                      clamp=clamp)
        oa, ob = self.operand(a), self.operand(b)
        if dst is None:
            dst = self.fresh()
        if op == 133:
            self.emit(_V.kMul, {1: (oa, False), 2: (ob, False)}, dst,
                      export, clamp=clamp)
        elif op == 129:
            self.emit(_V.kAdd, {1: (oa, False), 2: (ob, False)}, dst,
                      export, clamp=clamp)
        else:
            self.emit(_V.kAdd, {1: (oa, False), 2: (ob, True)}, dst,
                      export, clamp=clamp)
        return dst

    def emit_clamped(self, ssa, dst, export):
        vi = self.m.by_result[ssa]
        if vi.opcode in (129, 131, 133):
            return self.binop(vi.opcode, vi.operands[0], vi.operands[1],
                              dst=dst, export=export, clamp=True)
        r = self.operand(ssa)
        self.emit(_V.kMax, {1: (r, False), 2: (r, False)}, dst, export,
                  clamp=True)
        return dst

    def vts(self, vec, scal, dst=None, export=False):
        if self.float_const01(scal) == 1.0:
            return self.emit_copy(self.operand(vec), dst, export)
        ov, os = self.operand(vec), self.operand(scal)
        if dst is None:
            dst = self.fresh()
        self.emit(_V.kMul, {1: (ov, False),
                            2: (os, False, self.BCAST_X)}, dst, export)
        return dst

    def dot(self, a, b, dst=None, export=False):
        axis_b = self.axis_component(b)
        if axis_b is not None:
            return self.emit_copy(self.operand(a), dst, export,
                                  swizzle=self.bsw(axis_b))
        axis_a = self.axis_component(a)
        if axis_a is not None:
            return self.emit_copy(self.operand(b), dst, export,
                                  swizzle=self.bsw(axis_a))
        oa, ob = self.operand(a), self.operand(b)
        opcode = _V.kDp3 if self.vec_size(a) == 3 else _V.kDp4
        if dst is None:
            dst = self.fresh()
        self.emit(opcode, {1: (oa, False), 2: (ob, False)}, dst, export,
                  mask=0xF)
        return dst

    def emit_scalar(self, sop, dst, src_op, src_comp=3, mask=0xF):
        kind, reg = src_op
        swz = ((src_comp - 3) & 3) << 6
        f = dict(self.T["mov"], vector_opcode=_V.kMax, vector_dest=0,
                 vector_write_mask=0, vector_clamp=False,
                 scalar_opcode=sop, scalar_dest=dst, scalar_write_mask=mask,
                 scalar_clamp=False, is_export=False,
                 src3_reg=reg, src3_is_temp=(kind == "reg"),
                 src3_swizzle=swz, src3_negate=False)
        self.p.alu(**f)
        return dst

    def dotself(self, ov, n):
        d = self.fresh()
        self.emit(_V.kDp3 if n == 3 else _V.kDp4,
                  {1: (ov, False), 2: (ov, False)}, d, False, mask=0xF)
        return d

    def fdiv(self, a, b, dst=None, export=False):
        if self.float_const01(b) == 1.0:
            return self.emit_copy(self.operand(a), dst, export)
        oa, ob = self.operand(a), self.operand(b)
        bi = self.m.by_result.get(b)
        scalar_b = (bi is not None and bi.opcode == 80 and
                    len(set(bi.operands)) == 1 and
                    self.m.by_result[bi.operands[0]].opcode != 43)
        if dst is None:
            dst = self.fresh()
        n = max(self.vec_size(a), self.vec_size(b), 1)
        if scalar_b or n == 1:
            rb = self.emit_scalar(_S.kRcp, self.fresh(), ob, src_comp=0)
            self.emit(_V.kMul, {1: (oa, False), 2: (("reg", rb), False)},
                      dst, export)
        else:
            for i in range(n):
                rb = self.emit_scalar(_S.kRcp, self.fresh(), ob, src_comp=i)
                self.emit(_V.kMul, {1: (oa, False),
                                    2: (("reg", rb), False)},
                          dst, export, mask=(1 << i))
        return dst

    def to_reg(self, opnd):
        if opnd[0] == "reg":
            return opnd
        r = self.fresh()
        self.emit(_V.kMax, {1: (opnd, False), 2: (opnd, False)}, r, False)
        return ("reg", r)

    @staticmethod
    def swz(idxs):
        return sum(((idxs[p] - p) & 3) << (2 * p) for p in range(len(idxs)))

    @staticmethod
    def bsw(c):
        return sum(((c - p) & 3) << (2 * p) for p in range(4))

    def recip(self, opnd, n):
        if n == 1:
            return self.emit_scalar(_S.kRcp, self.fresh(), opnd, src_comp=0)
        r = self.fresh()
        for i in range(n):
            ri = self.emit_scalar(_S.kRcp, self.fresh(), opnd, src_comp=i)
            self.emit(_V.kMax, {1: (("reg", ri), False),
                                2: (("reg", ri), False)},
                      r, False, mask=(1 << i))
        return r

    def fmod(self, a, b, dst=None, export=False):
        oa, ob = self.operand(a), self.operand(b)
        q = self.fdiv(a, b)
        fq = self.fresh()
        self.emit(_V.kFloor, {1: (("reg", q), False),
                              2: (("reg", q), False)}, fq, False)
        if dst is None:
            dst = self.fresh()
        if oa[0] == "const" and ob[0] == "const":
            oa = self.to_reg(oa)
        self.emit(_V.kMad, {1: (ob, True), 2: (("reg", fq), False),
                            3: (oa, False)}, dst, export)
        return dst

    def extinst(self, ins, dst=None, export=False):
        inst = ins.operands[1]
        args = ins.operands[2:]
        if dst is None:
            dst = self.fresh()
        if inst == 46:                            # FMix(x,y,a)=x+(y-x)*a
            ox, oy, oa = (self.operand(args[0]), self.operand(args[1]),
                          self.operand(args[2]))
            tmp = self.fresh()
            self.emit(_V.kAdd, {1: (oy, False), 2: (ox, True)}, tmp, False)
            self.emit(_V.kMad, {1: (("reg", tmp), False), 2: (oa, False),
                                3: (ox, False)}, dst, export)
            return dst
        if inst in (40, 37):                      # FMax / FMin
            self.emit(_V.kMax if inst == 40 else _V.kMin,
                      {1: (self.operand(args[0]), False),
                       2: (self.operand(args[1]), False)}, dst, export)
            return dst

        def scalar_result(sop, src_op, src_comp=3):
            if export:
                r = self.emit_scalar(sop, self.fresh(), src_op, src_comp)
                self.emit(_V.kMax, {1: (("reg", r), False),
                                    2: (("reg", r), False)}, dst, True)
            else:
                self.emit_scalar(sop, dst, src_op, src_comp)
            return dst

        if inst == 8:                             # Floor
            ox = self.operand(args[0])
            self.emit(_V.kFloor, {1: (ox, False), 2: (ox, False)},
                      dst, export)
            return dst
        if inst == 69:                            # Normalize
            ov = self.operand(args[0])
            d = self.dotself(ov, self.vec_size(args[0]))
            s = self.emit_scalar(_S.kRsq, self.fresh(), ("reg", d))
            self.emit(_V.kMul, {1: (ov, False), 2: (("reg", s), False)},
                      dst, export)
            return dst
        if inst == 66:                            # Length
            ov = self.operand(args[0])
            d = self.dotself(ov, self.vec_size(args[0]))
            return scalar_result(_S.kSqrt, ("reg", d))
        if inst == 67:                            # Distance
            oa, ob = self.operand(args[0]), self.operand(args[1])
            diff = self.fresh()
            self.emit(_V.kAdd, {1: (oa, False), 2: (ob, True)}, diff, False)
            d = self.dotself(("reg", diff), self.vec_size(args[0]))
            return scalar_result(_S.kSqrt, ("reg", d))
        if inst == 31:
            return scalar_result(_S.kSqrt, self.operand(args[0]))
        if inst == 32:
            return scalar_result(_S.kRsq, self.operand(args[0]))
        if inst == 29:
            return scalar_result(_S.kExp, self.operand(args[0]))
        if inst == 30:
            return scalar_result(_S.kLog, self.operand(args[0]))

        def sbcast(c):
            return sum(((c - pp) & 3) << (2 * pp) for pp in range(4))

        def scalar_src(ssa):
            o, c = self.scalar_origin(ssa)
            return o, (0 if c == "B" else sbcast(c)), (3 if c == "B" else c)

        def bcast(ssa):
            o, sw, _c = scalar_src(ssa)
            r = self.fresh()
            self.emit(_V.kMax, {1: (o, False, sw), 2: (o, False, sw)},
                      r, False)
            return ("reg", r)

        def horner(xop, coeffs):
            p = self.to_reg(("const", self.literal_reg((coeffs[0],) * 4)))
            pr = p[1]
            for ck in coeffs[1:]:
                ckop = ("const", self.literal_reg((ck,) * 4))
                self.emit(_V.kMad, {1: (xop, False),
                                    2: (("reg", pr), False),
                                    3: (ckop, False)}, pr, False)
            return ("reg", pr)

        def asin_reg(ssa):
            xr = bcast(ssa)
            ax = self.fresh()
            self.emit(_V.kMax, {1: (xr, False), 2: (xr, True)}, ax, False)
            p = horner(("reg", ax),
                       [-0.0187293, 0.0742610, -0.2121144, 1.5707288])
            one = ("const", self.literal_reg((1.0,) * 4))
            om = self.fresh()
            self.emit(_V.kAdd, {1: (one, False),
                                2: (("reg", ax), True)}, om, False)
            s = self.emit_scalar(_S.kSqrt, self.fresh(), ("reg", om))
            halfpi = ("const", self.literal_reg((1.5707963,) * 4))
            inner = self.fresh()
            self.emit(_V.kMad, {1: (("reg", s), True), 2: (p, False),
                                3: (halfpi, False)}, inner, False)
            gt = self.fresh()
            self.emit(_V.kSgt, {1: (xr, False), 2: (xr, True)}, gt, False)
            lt = self.fresh()
            self.emit(_V.kSgt, {1: (xr, True), 2: (xr, False)}, lt, False)
            sgn = self.fresh()
            self.emit(_V.kAdd, {1: (("reg", gt), False),
                                2: (("reg", lt), True)}, sgn, False)
            out = self.fresh()
            self.emit(_V.kMul, {1: (("reg", sgn), False),
                                2: (("reg", inner), False)}, out, False)
            return out

        def atan_reg(ssa):
            xr = bcast(ssa)
            x2 = self.fresh()
            self.emit(_V.kMul, {1: (xr, False), 2: (xr, False)}, x2, False)
            p = horner(("reg", x2), [0.0208351, -0.0851330, 0.1801410,
                                     -0.3302995, 0.9998660])
            out = self.fresh()
            self.emit(_V.kMul, {1: (xr, False), 2: (p, False)}, out, False)
            return out

        if inst == 27:                            # Exp
            o, sw, _c = scalar_src(args[0])
            ck = ("const", self.literal_reg((1.4426950408,) * 4))
            mm = self.fresh()
            self.emit(_V.kMul, {1: (o, False, sw), 2: (ck, False)},
                      mm, False)
            return scalar_result(_S.kExp, ("reg", mm))
        if inst == 28:                            # Log
            o, _sw, c = scalar_src(args[0])
            lg = self.emit_scalar(_S.kLog, self.fresh(), o, src_comp=c)
            ck = ("const", self.literal_reg((0.6931471805,) * 4))
            self.emit(_V.kMul, {1: (("reg", lg), False), 2: (ck, False)},
                      dst, export)
            return dst
        if inst in (13, 14):                      # Sin / Cos
            o, _sw, c = scalar_src(args[0])
            return scalar_result(_S.kSin if inst == 13 else _S.kCos, o, c)
        if inst == 71:                            # Reflect
            oi, on = self.operand(args[0]), self.operand(args[1])
            d = self.dotself(on, self.vec_size(args[0])) \
                if args[0] == args[1] else None
            if d is None:
                d = self.fresh()
                self.emit(_V.kDp3 if self.vec_size(args[0]) == 3 else _V.kDp4,
                          {1: (on, False), 2: (oi, False)}, d, False,
                          mask=0xF)
            d2 = self.fresh()
            self.emit(_V.kAdd, {1: (("reg", d), False),
                                2: (("reg", d), False)}, d2, False)
            self.emit(_V.kMad, {1: (("reg", d2), True), 2: (on, False),
                                3: (oi, False)}, dst, export)
            return dst
        if inst == 26:                            # Pow
            ox, oy = self.operand(args[0]), self.operand(args[1])
            n = max(self.vec_size(args[0]), 1)
            for i in range(n):
                li = self.emit_scalar(_S.kLog, self.fresh(), ox, src_comp=i)
                mi = self.fresh()
                self.emit(_V.kMul, {1: (("reg", li), False),
                                    2: (oy, False, self.BCAST_X)}, mi, False)
                ri = self.emit_scalar(_S.kExp, self.fresh(), ("reg", mi))
                self.emit(_V.kMax, {1: (("reg", ri), False),
                                    2: (("reg", ri), False)}, dst, export,
                          mask=(1 << i))
            return dst
        if inst == 4:                             # FAbs
            ox = self.operand(args[0])
            self.emit(_V.kMax, {1: (ox, False), 2: (ox, True)}, dst, export)
            return dst
        if inst == 6:                             # FSign
            ox = self.operand(args[0])
            if ox[0] == "const":
                xr = self.fresh()
                self.emit(_V.kMax, {1: (ox, False), 2: (ox, False)},
                          xr, False)
                ox = ("reg", xr)
            gt = self.fresh()
            self.emit(_V.kSgt, {1: (ox, False), 2: (ox, True)}, gt, False)
            lt = self.fresh()
            self.emit(_V.kSgt, {1: (ox, True), 2: (ox, False)}, lt, False)
            self.emit(_V.kAdd, {1: (("reg", gt), False),
                                2: (("reg", lt), True)}, dst, export)
            return dst
        if inst == 15:                            # Tan
            o, _sw, c = scalar_src(args[0])
            s = self.emit_scalar(_S.kSin, self.fresh(), o, src_comp=c)
            co = self.emit_scalar(_S.kCos, self.fresh(), o, src_comp=c)
            rc = self.emit_scalar(_S.kRcp, self.fresh(), ("reg", co))
            self.emit(_V.kMul, {1: (("reg", s), False),
                                2: (("reg", rc), False)}, dst, export)
            return dst
        if inst == 10:                            # Fract
            ox = self.operand(args[0])
            self.emit(_V.kFrc, {1: (ox, False), 2: (ox, False)}, dst, export)
            return dst
        if inst == 3:                             # Trunc
            ox = self.operand(args[0])
            self.emit(_V.kTrunc, {1: (ox, False), 2: (ox, False)}, dst, export)
            return dst
        if inst == 9:                             # Ceil
            ox = self.operand(args[0])
            tf = self.fresh()
            self.emit(_V.kFloor, {1: (ox, True), 2: (ox, True)}, tf, False)
            self.emit(_V.kMax, {1: (("reg", tf), True),
                                2: (("reg", tf), True)}, dst, export)
            return dst
        if inst in (1, 2):                        # Round/RoundEven
            ox = self.to_reg(self.operand(args[0]))
            half = ("const", self.literal_reg((0.5, 0.5, 0.5, 0.5)))
            s = self.fresh()
            self.emit(_V.kAdd, {1: (ox, False), 2: (half, False)}, s, False)
            self.emit(_V.kFloor, {1: (("reg", s), False),
                                  2: (("reg", s), False)}, dst, export)
            return dst
        if inst in (11, 12):                      # Radians/Degrees
            ox = self.to_reg(self.operand(args[0]))
            k = 0.0174532925 if inst == 11 else 57.2957795
            ck = ("const", self.literal_reg((k, k, k, k)))
            self.emit(_V.kMul, {1: (ox, False), 2: (ck, False)}, dst, export)
            return dst
        if inst == 48:                            # Step
            oe = self.operand(args[0])
            ox = self.to_reg(self.operand(args[1]))
            self.emit(_V.kSge, {1: (ox, False), 2: (oe, False)}, dst, export)
            return dst
        if inst == 49:                            # SmoothStep
            oe0, oe1, ox = (self.operand(args[0]), self.operand(args[1]),
                            self.operand(args[2]))
            e0r = self.to_reg(oe0)
            n = max(self.vec_size(args[2]), 1)
            num = self.fresh()
            self.emit(_V.kAdd, {1: (ox, False), 2: (e0r, True)}, num, False)
            den = self.fresh()
            self.emit(_V.kAdd, {1: (oe1, False), 2: (e0r, True)}, den, False)
            rden = self.recip(("reg", den), n)
            tt = self.fresh()
            self.emit(_V.kMul, {1: (("reg", num), False),
                                2: (("reg", rden), False)}, tt, False,
                      clamp=True)
            two_t = self.fresh()
            self.emit(_V.kAdd, {1: (("reg", tt), False),
                                2: (("reg", tt), False)}, two_t, False)
            three = ("const", self.literal_reg((3.0, 3.0, 3.0, 3.0)))
            h = self.fresh()
            self.emit(_V.kAdd, {1: (three, False),
                                2: (("reg", two_t), True)}, h, False)
            t2 = self.fresh()
            self.emit(_V.kMul, {1: (("reg", tt), False),
                                2: (("reg", tt), False)}, t2, False)
            self.emit(_V.kMul, {1: (("reg", t2), False),
                                2: (("reg", h), False)}, dst, export)
            return dst
        if inst == 68:                            # Cross
            oa, ob = self.operand(args[0]), self.operand(args[1])
            yzx, zxy = self.swz([1, 2, 0]), self.swz([2, 0, 1])
            tc = self.fresh()
            self.emit(_V.kMul, {1: (oa, False, zxy),
                                2: (ob, False, yzx)}, tc, False)
            self.emit(_V.kMad, {1: (oa, False, yzx),
                                2: (ob, False, zxy),
                                3: (("reg", tc), True)}, dst, export)
            return dst
        if inst == 33:                            # Determinant -> scalar
            r = self.emit_determinant(args[0])
            self.emit(_V.kMax, {1: (("reg", r), False),
                                2: (("reg", r), False)}, dst, export)
            return dst
        if inst == 72:                            # Refract(I, N, eta)
            oi, on = self.operand(args[0]), self.operand(args[1])
            n = max(self.vec_size(args[0]), 1)
            eta = bcast(args[2])
            one = ("const", self.literal_reg((1.0,) * 4))
            d = self.fresh()                      # d = dot(N, I)
            self.emit(_V.kDp3 if n == 3 else _V.kDp4,
                      {1: (on, False), 2: (oi, False)}, d, False, mask=0xF)
            d2 = self.fresh()
            self.emit(_V.kMul, {1: (("reg", d), False),
                                2: (("reg", d), False)}, d2, False)
            om = self.fresh()                     # 1 - d^2
            self.emit(_V.kAdd, {1: (one, False),
                                2: (("reg", d2), True)}, om, False)
            e2 = self.fresh()                     # eta^2
            self.emit(_V.kMul, {1: (eta, False), 2: (eta, False)}, e2, False)
            t = self.fresh()                      # eta^2 * (1 - d^2)
            self.emit(_V.kMul, {1: (("reg", e2), False),
                                2: (("reg", om), False)}, t, False)
            k = self.fresh()                      # k = 1 - eta^2*(1-d^2)
            self.emit(_V.kAdd, {1: (one, False),
                                2: (("reg", t), True)}, k, False)
            sk = self.emit_scalar(_S.kSqrt, self.fresh(), ("reg", k))
            coef = self.fresh()                   # eta*d + sqrt(k)
            self.emit(_V.kMad, {1: (eta, False), 2: (("reg", d), False),
                                3: (("reg", sk), False)}, coef, False)
            base = self.fresh()                   # eta*I
            self.emit(_V.kMul, {1: (eta, False), 2: (oi, False)}, base, False)
            res = self.fresh()                    # eta*I - coef*N
            self.emit(_V.kMad, {1: (on, True), 2: (("reg", coef), False),
                                3: (("reg", base), False)}, res, False)
            zero = ("const", self.literal_reg((0.0,) * 4))
            msk = self.fresh()                    # k >= 0 ? 1 : 0
            self.emit(_V.kSge, {1: (("reg", k), False),
                                2: (zero, False)}, msk, False)
            self.emit(_V.kMul, {1: (("reg", res), False),
                                2: (("reg", msk), False)}, dst, export)
            return dst
        if inst == 43:                            # FClamp
            lo, hi = self.const_scalar(args[1]), self.const_scalar(args[2])
            if lo == 0.0 and hi == 1.0:
                return self.emit_clamped(args[0], dst, export)
            ox = self.operand(args[0])
            lo_op = ("const", self.literal_reg((lo, lo, lo, lo)))
            hi_op = ("const", self.literal_reg((hi, hi, hi, hi)))
            t = self.fresh()
            self.emit(_V.kMax, {1: (ox, False), 2: (lo_op, False)}, t, False)
            self.emit(_V.kMin, {1: (("reg", t), False),
                                2: (hi_op, False)}, dst, export)
            return dst
        if inst == 16:                            # Asin
            r = asin_reg(args[0])
            self.emit(_V.kMax, {1: (("reg", r), False),
                                2: (("reg", r), False)}, dst, export)
            return dst
        if inst == 17:                            # Acos
            r = asin_reg(args[0])
            halfpi = ("const", self.literal_reg((1.5707963, 1.5707963,
                                                 1.5707963, 1.5707963)))
            self.emit(_V.kAdd, {1: (halfpi, False),
                                2: (("reg", r), True)}, dst, export)
            return dst
        if inst == 18:                            # Atan
            r = atan_reg(args[0])
            self.emit(_V.kMax, {1: (("reg", r), False),
                                2: (("reg", r), False)}, dst, export)
            return dst
        raise LowerError(f"v1 {self.stage}: unhandled GLSL.std.450 #{inst}")

    def emit_value(self, ssa, dst, export, export_position=False):
        """Emit a value expression to `dst` (export-fused when export)."""
        vi = self.m.by_result[ssa]
        cv = self.const_vector(ssa) if export and vi.opcode in (43, 44) \
            else None
        if cv is not None and all(self.is_zero(v) or self.is_one(v)
                                  for v in cv):
            zero_mask = sum(1 << i for i, v in enumerate(cv)
                            if self.is_zero(v))
            one_mask = sum(1 << i for i, v in enumerate(cv)
                           if self.is_one(v))
            self.emit_export_constants(dst, zero_mask, one_mask)
        elif vi.opcode == 80:
            self.emit_construct(vi, dst, export)
        elif vi.opcode == 145:
            self.emit_mat_vec(vi.operands[0], vi.operands[1], dst=dst,
                              export_position=export_position)
        elif vi.opcode == 144:
            self.emit_vec_mat(vi.operands[0], vi.operands[1], dst=dst,
                              export_position=export_position)
        elif vi.opcode == 146:
            raise LowerError("cannot export a matrix value")
        elif vi.opcode in (129, 131, 133):
            self.binop(vi.opcode, vi.operands[0], vi.operands[1], dst=dst,
                       export=export)
        elif vi.opcode == 142:
            self.vts(vi.operands[0], vi.operands[1], dst=dst, export=export)
        elif vi.opcode == 148:
            self.dot(vi.operands[0], vi.operands[1], dst=dst, export=export)
        elif vi.opcode == 136:
            self.fdiv(vi.operands[0], vi.operands[1], dst=dst, export=export)
        elif vi.opcode == 141:
            self.fmod(vi.operands[0], vi.operands[1], dst=dst, export=export)
        elif vi.opcode == 12:
            self.extinst(vi, dst=dst, export=export)
        else:
            # A constant goes straight to the export, as xsdk's hardware-proven constant shaders do
            # (xsdk:tools/gen_constant_shaders.py, max o0, c2, c2).
            r = self.operand(ssa)
            self.emit(_V.kMax, {1: (r, False), 2: (r, False)}, dst, export)


def lower_vertex(m: spirv.Module, registers: RegisterBudget = LIBXENON_REGISTERS, optimize: bool = False):
    si = resolve_interface(m)
    if si.stage != "vertex":
        raise LowerError("lower_vertex called on non-vertex module")
    # A vertex shader lowers as straight-line code, a local's value its last store. A branch left after unrolling, an
    # if/else or a ?: that glslang makes one, would lower as its last arm alone, silently: refuse it.
    if any(ins.opcode in (250, 251) for ins in m.instrs):  # OpBranchConditional, OpSwitch
        raise LowerError("vertex shader control flow (an if/else, a ?: glslang branches for, or a switch) "
                         "isn't lowered")
    T = _templates()
    types = resolve_types(m)
    stores = _store_map(m)
    p = VertexProgram(registers)
    p.optimize = optimize
    p.whole_sources = True                        # temps run past r63 till the allocator packs them

    # --- phase 1: vfetch every input, input n into rn ---
    # r0 also holds the vertex index every fetch reads, so the fetch that overwrites it goes last: Xenia reads the index
    # as each fetch runs (xenia:src/xenia/gpu/dxbc_shader_translator_fetch.cc:80-88), so the rest would index by r0's
    # new value.
    input_reg = {}
    for n, (v, usage, uidx) in reversed(list(enumerate(si.vfetch))):
        p.fetch(usage, T["vf"], dest=n, usage_index=uidx)
        input_reg[v.vid] = n
    reg = len(si.vfetch)

    local_vars = {ins.result_id for ins in m.instrs
                  if ins.opcode == 59 and ins.operands[0] == 7}
    _check_local_stores(m, local_vars)
    local_store = {tgt: val for tgt, val in stores.items()
                   if tgt in local_vars}
    lit_base = max(max((cs.reg + cs.regs for cs in si.constants), default=0), si.constant_end)
    ctx = ValueLowerer(m=m, si=si, program=p, templates=T, types=types,
                       stores=stores, first_reg=reg, inputs=input_reg,
                       local_store=local_store, stage="vertex",
                       literal_base=lit_base, allow_literals=True)

    # --- phase 2: position ---
    # Without varyings, interpolators are allocated ahead of the position, as xsdk's hardware-proven resolve shader
    # does (xsdk:tools/gen_resolve_vs.py), so no alloc trails the end of the program.
    if not si.interp_out:
        p.alloc_interpolators()
    p.alloc_position()
    pos_value = None
    for tgt, val in stores.items():
        ac = _access_chain_base(m, tgt)
        if tgt == si.position_out or (ac and ac[0] == si.position_out):
            pos_value = val
    if pos_value is None:
        raise LowerError("no write to gl_Position")
    ctx.emit_value(pos_value, OPOS, True, export_position=True)

    # --- phase 3: interpolator exports ---
    if si.interp_out:
        p.alloc_interpolators()
    for v, o, usage in si.interp_out:
        if v.vid not in stores:
            raise LowerError(f"interpolator {v.name} never written")
        r = ctx.operand(stores[v.vid])
        temp = r[0] == "reg"
        p.export(o, usage, T["mov"], src1_reg=r[1], src2_reg=r[1], src1_is_temp=temp, src2_is_temp=temp)

    constants = []
    for cs in si.constants:
        if cs.regs > 1:
            constants.append(ce.Constant(cs.name, ce.RS_FLOAT4, cs.reg,
                                         cs.regs, ce.CLASS_MATRIX_COLS,
                                         ce.TYPE_FLOAT, rows=4, columns=4))
        else:
            constants.append(ce.Constant(cs.name, ce.RS_FLOAT4, cs.reg, 1,
                                         ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                                         rows=1, columns=4))
    for reg, _val in ctx.lit_manifest:
        constants.append(ce.Constant(f"_lit{reg}", ce.RS_FLOAT4, reg, 1,
                                     ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                                     rows=1, columns=4))
    return p.finish(constants=constants), ctx.lit_manifest


def lower_fragment_real_loop(m: spirv.Module, reason: Exception | None = None,
                             registers: RegisterBudget = LIBXENON_REGISTERS):
    """Minimal real-loop lowering for `for(i=0;i<u_count;i++) sum += k`.

    This intentionally bypasses the SSA value DAG: the loop-carried `sum` is a
    persistent GPR, and the uniform bound is a hardware loop constant uploaded
    by the host as `integer_constants[8+loop_id]`.
    """
    si = resolve_interface(m)
    if si.stage != "pixel":
        raise LowerError("real-loop lowering is pixel-only") from reason
    if len(si.color_out) != 1:
        raise LowerError("real-loop path expects one color output") from reason
    if si.frag_coord is not None:
        raise LowerError("gl_FragCoord in a hardware loop (v1)") from reason

    instrs = m.instrs
    func_idx = next(i for i, ins in enumerate(instrs) if ins.opcode == 54)
    end_idx = next(i for i, ins in enumerate(instrs) if ins.opcode == 56)
    entry_idx = next(i for i in range(func_idx, end_idx)
                     if instrs[i].opcode == 248)
    entry_label = instrs[entry_idx].result_id

    blocks = {}
    i = entry_idx
    while i < end_idx:
        if instrs[i].opcode == 248:               # Label
            lbl = instrs[i].result_id
            mid, j = [], i + 1
            while j < end_idx and instrs[j].opcode not in (249, 250, 253):
                mid.append(instrs[j])
                j += 1
            term = instrs[j] if j < end_idx else None
            blocks[lbl] = (mid, term)
            i = j + 1
        else:
            i += 1

    local_vars = {ins.result_id for ins in instrs[func_idx:end_idx]
                  if ins.opcode == 59 and ins.operands[0] == 7}

    def fail(msg):
        raise LowerError(f"real-loop v1: {msg}") from reason

    def const_int(ssa):
        ins = m.by_result.get(ssa)
        if ins is None or ins.opcode != 43:
            fail(f"%{ssa} is not an int constant")
        return _signed_int(ins.operands[0] & 0xFFFFFFFF)

    def const_float(ssa):
        ins = m.by_result.get(ssa)
        if ins is None or ins.opcode != 43:
            fail(f"%{ssa} is not a float constant")
        bits = ins.operands[0] & 0xFFFFFFFF
        return struct.unpack("<f", struct.pack("<I", bits))[0]

    def const_vec4(ssa):
        ins = m.by_result.get(ssa)
        if ins is None:
            fail(f"%{ssa} is not a constant vector")
        if ins.opcode == 44:
            vals = [const_float(c) for c in ins.operands]
        elif ins.opcode == 43:
            vals = [const_float(ssa)]
        else:
            fail(f"%{ssa} is not a constant vector")
        return tuple((vals + [0.0, 0.0, 0.0, 0.0])[:4])

    def load_var(ssa):
        ins = m.by_result.get(ssa)
        if ins is not None and ins.opcode == 61:
            return ins.operands[0]
        return None

    entry_mid, entry_term = blocks[entry_label]
    if entry_term is None or entry_term.opcode != 249:
        fail("entry must branch to a loop header")
    header_lbl = entry_term.operands[0]
    init_stores = [(ins.operands[0], ins.operands[1])
                   for ins in entry_mid if ins.opcode == 62]
    header_mid, header_term = blocks.get(header_lbl, (None, None))
    loop_merge = next((ins for ins in header_mid or []
                       if ins.opcode == 246), None)
    if loop_merge is None or header_term is None or header_term.opcode != 249:
        fail("missing structured loop header")
    merge_lbl, cont_lbl = loop_merge.operands[:2]
    cond_lbl = header_term.operands[0]

    cond_mid, cond_term = blocks.get(cond_lbl, (None, None))
    if cond_term is None or cond_term.opcode != 250:
        fail("loop condition must branch conditionally")
    cmp = m.by_result.get(cond_term.operands[0])
    if cmp is None or cmp.opcode != 177:           # SLessThan
        fail("loop condition must be signed i < bound")
    body_lbl = cond_term.operands[1]
    if cond_term.operands[2] != merge_lbl:
        fail("loop false branch must exit")

    iv_var = load_var(cmp.operands[0])
    bound_var = load_var(cmp.operands[1])
    if iv_var not in local_vars or bound_var is None:
        fail("condition must compare local induction var to a uniform")
    bound_decl = m.by_result.get(bound_var)
    if bound_decl is None or bound_decl.opcode != 59 or \
            bound_decl.operands[0] != 0:
        fail("loop bound must be a UniformConstant int")
    loop_id = 0
    for deco, args in m.decorations.get(bound_var, []):
        if deco == 30:                             # Location
            loop_id = args[0]
    bound_name = m.names.get(bound_var, f"u_loop{loop_id}")

    init_map = {t: v for t, v in init_stores}
    if const_int(init_map.get(iv_var)) != 0:
        fail("induction variable must start at zero")

    cont_mid, cont_term = blocks.get(cont_lbl, (None, None))
    if cont_term is None or cont_term.opcode != 249 or \
            cont_term.operands[0] != header_lbl:
        fail("continue block must branch back to the header")
    cont_store = next((ins for ins in cont_mid or [] if ins.opcode == 62
                       and ins.operands[0] == iv_var), None)
    if cont_store is None:
        fail("continue block must update the induction variable")
    inc = m.by_result.get(cont_store.operands[1])
    if inc is None or inc.opcode != 128:           # IAdd
        fail("induction update must be i + 1")
    inc_loads = [load_var(w) for w in inc.operands]
    inc_consts = [w for w in inc.operands if w in m.by_result and
                  m.by_result[w].opcode == 43]
    if iv_var not in inc_loads or not inc_consts or \
            const_int(inc_consts[0]) != 1:
        fail("induction update must be i + 1")

    body_mid, body_term = blocks.get(body_lbl, (None, None))
    plain_body = (body_term is not None and body_term.opcode == 249 and
                  body_term.operands[0] == cont_lbl)
    # `if(cond) break;` makes the body end in SelectionMerge + BranchConditional
    # (one arm jumps to the loop merge); handled below.
    break_body = body_term is not None and body_term.opcode == 250
    if not (plain_body or break_body):
        fail("loop body must branch to continue (or conditionally break)")
    body_store = next((ins for ins in body_mid or [] if ins.opcode == 62),
                      None)
    if body_store is None or body_store.operands[0] not in local_vars:
        fail("loop body must store one local accumulator")
    acc_var = body_store.operands[0]
    acc_init = init_map.get(acc_var)
    if acc_init is None:
        fail("accumulator must be initialized before the loop")
    add = m.by_result.get(body_store.operands[1])
    if add is None or add.opcode != 129:           # FAdd
        fail("loop body must be sum += constant")
    acc_load_idx = None
    for idx, operand in enumerate(add.operands):
        if load_var(operand) == acc_var:
            acc_load_idx = idx
    if acc_load_idx is None:
        fail("loop body must add to the accumulator")
    step_ssa = add.operands[1 - acc_load_idx]

    def al_array(ssa):
        """(base_reg, n_regs) if ssa is a Load of arr[induction_var] for a
        uniform array -- the aL-relative addressing case c[base+aL]."""
        ld = m.by_result.get(ssa)
        if ld is None or ld.opcode != 61:          # Load
            return None
        ac = m.by_result.get(ld.operands[0])
        if ac is None or ac.opcode != 65:          # AccessChain
            return None
        il = m.by_result.get(ac.operands[1])       # index == Load(induction)?
        if il is None or il.opcode != 61 or il.operands[0] != iv_var:
            return None
        role = si.roles.get(ac.operands[0])
        if role and role[0] == "array":
            return role[1][0], role[1][1], ac.operands[0]
        return None

    arr = al_array(step_ssa)
    if arr is None:
        step_val = const_vec4(step_ssa)            # acc += constant
    init_val = const_vec4(acc_init)
    if arr is not None and not all(v == 0.0 for v in init_val):
        fail("array-accumulating loop must start the accumulator at zero")

    # `if(acc.c CMP const) break;` -> a setp on the body value + a predicated
    # LOOP_END (the data-dependent while-break, ISA-proven as G30).
    has_break = break_setp = break_comp = break_thresh = None
    break_negate = break_cond_val = False
    if break_body:
        def acc_component(ssa):
            ld = m.by_result.get(ssa)
            if ld is None or ld.opcode != 61:
                return None
            ac = m.by_result.get(ld.operands[0])
            if ac is None or ac.opcode != 65 or ac.operands[0] != acc_var:
                return None
            ci = m.by_result.get(ac.operands[1])
            return ci.operands[0] if ci and ci.opcode == 43 else None

        def jumps_to(lbl, target):
            mm, tt = blocks.get(lbl, (None, None))
            return (not (mm or [])) and tt is not None and \
                tt.opcode == 249 and tt.operands[0] == target

        t_lbl, f_lbl = body_term.operands[1], body_term.operands[2]
        if jumps_to(t_lbl, merge_lbl):
            break_cond_val = True
        elif jumps_to(f_lbl, merge_lbl):
            break_cond_val = False
        else:
            fail("loop body conditional must be a plain `break`")
        bcmp = m.by_result.get(body_term.operands[0])
        if bcmp is None or bcmp.opcode not in (180, 182, 184, 186, 188, 190):
            fail("break condition must be a float comparison")
        comp = acc_component(bcmp.operands[0])
        if comp is not None:
            thr_ssa, cop = bcmp.operands[1], bcmp.opcode
        else:                                      # accumulator on the rhs
            comp = acc_component(bcmp.operands[1])
            _flip = {186: 184, 184: 186, 190: 188, 188: 190, 180: 180,
                     182: 182}
            thr_ssa, cop = bcmp.operands[0], _flip[bcmp.opcode]
        if comp is None:
            fail("break must compare the accumulator to a constant")
        # condition `acc CMP thr` -> setp on (acc-thr); for < / <= negate diff.
        _setp = {186: (xu.AluScalarOpcode.kSetpGt, False),
                 190: (xu.AluScalarOpcode.kSetpGe, False),
                 184: (xu.AluScalarOpcode.kSetpGt, True),
                 188: (xu.AluScalarOpcode.kSetpGe, True),
                 180: (xu.AluScalarOpcode.kSetpEq, False),
                 182: (xu.AluScalarOpcode.kSetpNe, False)}
        break_setp, break_negate = _setp[cop]
        break_comp, break_thresh = comp, const_float(thr_ssa)
        has_break = True

    merge_mid, merge_term = blocks.get(merge_lbl, (None, None))
    if merge_term is None or merge_term.opcode != 253:
        fail("merge block must return")
    out_var = si.color_out[0][0].vid
    out_store = next((ins for ins in merge_mid or [] if ins.opcode == 62
                      and ins.operands[0] == out_var), None)
    if out_store is None or load_var(out_store.operands[1]) != acc_var:
        fail("merge block must export the accumulator")

    T = _templates()
    p = PixelProgram(registers)

    def emit(opcode, dst, srcs, export=False):
        f = dict(T["mov"], vector_opcode=opcode, vector_dest=dst,
                 is_export=export, vector_write_mask=0xF,
                 vector_clamp=False, scalar_write_mask=0)
        for n, (kind, reg, neg) in enumerate(srcs, start=1):
            f[f"src{n}_reg"] = reg
            f[f"src{n}_is_temp"] = (kind == "reg")
            f[f"src{n}_swizzle"] = 0
            f[f"src{n}_negate"] = neg
        p.alu(**f)

    # Match the gallery's standard vertex shader, which exports two
    # interpolators (texcoord, color). The PS parameter-cache count MUST match
    # the VS export count, or the GPU leaks the varying and corrupts
    # interpolation (magenta hue + distorted geometry). The loop body uses
    # neither; they are interface padding (same fix as the hand-built LOOPHW).
    # The accumulator therefore lives in r2, clear of the interpolators r0/r1.
    # (v1: a general solution would derive this from the VS pairing.)
    p.input(0, ce.USAGE_TEXCOORD)
    p.input(1, ce.USAGE_COLOR)
    acc_reg, step_reg = 2, 0
    init_is_zero = all(v == 0.0 for v in init_val)
    if arr is not None:
        arr_base, arr_regs, arr_var = arr
        arr_name = m.names.get(arr_var) or f"u_arr{arr_base}"
        # acc = c[base] - c[base] = 0 (zero without spending a literal slot)
        emit(_V.kAdd, acc_reg, [("const", arr_base, False),
                                ("const", arr_base, True)])
    elif init_is_zero:
        emit(_V.kAdd, acc_reg, [("const", step_reg, False),
                                ("const", step_reg, True)])
    else:
        init_reg = 1
        emit(_V.kMax, acc_reg, [("const", init_reg, False),
                                ("const", init_reg, False)])
    if has_break:
        if arr is not None:
            thresh_reg = arr_base + arr_regs
        elif not init_is_zero:
            thresh_reg = init_reg + 1
        else:
            thresh_reg = step_reg + 1
    p.begin_loop(loop_id=loop_id)
    if arr is not None:
        # acc += c[base + aL]: the array element selected by the loop counter.
        p.alu(**dict(T["mov"], vector_opcode=_V.kAdd, vector_dest=acc_reg,
                     is_export=False, vector_write_mask=0xF,
                     vector_clamp=False, scalar_write_mask=0,
                     src1_reg=acc_reg, src1_is_temp=True, src1_swizzle=0,
                     src1_negate=False,
                     src2_reg=arr_base, src2_is_temp=False, src2_swizzle=0,
                     src2_negate=False, src2_const_rel=True))
    else:
        emit(_V.kAdd, acc_reg, [("reg", acc_reg, False),
                                ("const", step_reg, False)])
    if has_break:
        # p0 = (acc.comp CMP thresh); break the loop when p0 == break_cond_val.
        diff_reg = acc_reg + 1
        if break_negate:                          # thresh - acc (for < / <=)
            emit(_V.kAdd, diff_reg, [("const", thresh_reg, False),
                                     ("reg", acc_reg, True)])
        else:                                     # acc - thresh
            emit(_V.kAdd, diff_reg, [("reg", acc_reg, False),
                                     ("const", thresh_reg, True)])
        bsw = sum(((break_comp - pp) & 3) << (2 * pp) for pp in range(4))
        p.alu(**dict(T["mov"], vector_opcode=_V.kMax, vector_dest=0,
                     is_export=False, vector_write_mask=0, vector_clamp=False,
                     scalar_opcode=break_setp, scalar_dest=0,
                     scalar_write_mask=0,
                     src3_reg=diff_reg, src3_is_temp=True, src3_swizzle=bsw))
        p.end_loop(predicated_break=True, break_condition=break_cond_val)
    else:
        p.end_loop()
    p.alloc_colors()
    emit(_V.kMax, 0, [("reg", acc_reg, False), ("reg", acc_reg, False)],
         export=True)

    constants = []
    manifest = []
    if arr is not None:
        constants.append(ce.Constant(arr_name, ce.RS_FLOAT4, arr_base,
                                     arr_regs, ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                                     rows=1, columns=4))
        manifest.append(("array", arr_base, arr_regs, arr_name))
    else:
        constants.append(ce.Constant("_lit0", ce.RS_FLOAT4, step_reg, 1,
                                     ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                                     rows=1, columns=4))
        manifest.append((step_reg, step_val))
        if not init_is_zero:
            constants.append(ce.Constant("_lit1", ce.RS_FLOAT4, init_reg, 1,
                                         ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                                         rows=1, columns=4))
            manifest.append((init_reg, init_val))
    if has_break:
        constants.append(ce.Constant("_thr", ce.RS_FLOAT4, thresh_reg, 1,
                                     ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                                     rows=1, columns=4))
        manifest.append((thresh_reg, (break_thresh,) * 4))
    manifest.append(("loop", loop_id, bound_name))
    return p.finish(constants=constants), manifest


def lower_fragment(m: spirv.Module, registers: RegisterBudget = LIBXENON_REGISTERS,
                   optimize: bool = False) -> bytes:
    si = resolve_interface(m)
    if si.stage != "pixel":
        raise LowerError("lower_fragment called on non-pixel module")
    T = _templates()
    types = resolve_types(m)
    stores = _store_map(m)
    p = PixelProgram(registers)
    p.optimize = optimize
    p.whole_sources = True                        # temps run past r63 till the allocator packs them

    interp_reg = {}
    maxr = -1
    for v, r, usage in si.interp_in:
        p.input(r, usage)
        interp_reg[v.vid] = r
        maxr = max(maxr, r)
    # gl_FragCoord.xy from the pixel parameters, written into the register past the interpolators.
    param_reg = None
    if si.frag_coord is not None:
        if _frag_coord_reads(m, types, si.frag_coord.vid) & {2, 3}:
            raise LowerError("gl_FragCoord.zw: the pixel parameters hold no depth (v1)")
        param_reg = maxr + 1
        p.pixel_parameters(param_reg)
        maxr = param_reg
    sample_reg = {}       # ImageSample ssa-id -> register (phase 1)
    deriv_reg = {}        # dFdx/dFdy/fwidth ssa-id -> register (phase 1)
    used_samplers = set()

    # Literal pool starts after uniform-declared constants.
    lit_base = max(max((cs.reg + cs.regs for cs in si.constants), default=0), si.constant_end)

    # Function-local variables (vec4 tmp = ...): store-forward in branch-free.
    local_vars = {ins.result_id for ins in m.instrs
                  if ins.opcode == 59 and ins.operands[0] == 7}
    local_store = {tgt: val for tgt, val in stores.items()
                   if tgt in local_vars}

    ctx = ValueLowerer(m=m, si=si, program=p, templates=T, types=types,
                       stores=stores, first_reg=maxr + 1,
                       inputs=interp_reg, local_store=local_store,
                       stage="fragment", sample_reg=sample_reg,
                       deriv_reg=deriv_reg,
                       literal_base=lit_base, allow_literals=True)
    if param_reg is not None:
        # Vulkan's gl_FragCoord is the pixel's centre, the column and row plus 0.5 (|xy|: the sign bits are flags).
        coord = ctx.fresh()
        ctx.emit(_V.kAdd, {1: (("reg", param_reg | ABS_SOURCE), False),
                           2: (("const", ctx.literal_reg((0.5, 0.5, 0.5, 0.5))), False)}, coord, False, mask=0x3)
        interp_reg[si.frag_coord.vid] = coord

    # phase 1 (before alloc_colors): emit a tfetch for every ImageSample
    # reachable from the output expression (walk the whole DAG).
    seen = set()

    def gather(ssa_id: int):
        if ssa_id in seen:
            return
        seen.add(ssa_id)
        ins = m.by_result.get(ssa_id)
        if ins is None:
            return
        if ins.opcode in (207, 208, 209):         # dFdx / dFdy / fwidth
            # getGradients is a fetch-pipe op (like tfetch) -> emit in phase 1.
            # It returns grad.xz = ddx(src.xy), grad.yw = ddy(src.xy).
            val_ssa = ins.operands[0]
            gather(val_ssa)
            if ssa_id not in deriv_reg:
                src = ctx.to_reg(ctx.operand(val_ssa))[1]
                grad = ctx.fresh()
                p.tfetch(dict(T["tf"],
                              opcode=xu.FetchOpcode.kGetTextureGradients,
                              src_swizzle=(0 | (1 << 2))), dest=grad, src=src)
                XZ, YW = 0xE4, 0x39               # .xzxz (ddx), .ywyw (ddy)
                if ins.opcode == 207:             # dFdx
                    r = ctx.fresh()
                    ctx.emit(_V.kMax, {1: (("reg", grad), False, XZ),
                                       2: (("reg", grad), False, XZ)}, r, False)
                elif ins.opcode == 208:           # dFdy
                    r = ctx.fresh()
                    ctx.emit(_V.kMax, {1: (("reg", grad), False, YW),
                                       2: (("reg", grad), False, YW)}, r, False)
                else:                             # fwidth = |ddx| + |ddy|
                    ag = ctx.fresh()
                    ctx.emit(_V.kMax, {1: (("reg", grad), False),
                                       2: (("reg", grad), True)}, ag, False)
                    r = ctx.fresh()
                    ctx.emit(_V.kAdd, {1: (("reg", ag), False, XZ),
                                       2: (("reg", ag), False, YW)}, r, False)
                deriv_reg[ssa_id] = r
            return
        if ins.opcode in (87, 88, 91, 92):        # ImageSample [Proj][Lod]
            proj = ins.opcode in (91, 92)         # projective (divide by last)
            samp = m.by_result.get(ins.operands[0])
            srole = si.roles.get(samp.operands[0]) if samp else None
            if not srole or srole[0] != "sampler":
                raise LowerError("ImageSample needs a sampler")
            slot = srole[1]
            dim = srole[2] if len(srole) > 2 else si.sampler_dims.get(slot)
            if dim not in (1, 3):
                raise LowerError(f"unsupported sampler dimension {dim}")
            coord = ins.operands[1]
            # Optional image operand: Bias (0x1) or explicit Lod (0x2). Xenos
            # tfetch carries a constant LOD bias (7-bit, 1/16 units), so a
            # constant bias / LOD folds in; a register LOD is rejected.
            lod_bias = 0.0
            rest = list(ins.operands[2:])
            if rest and (rest[0] & 0x3) and len(rest) > 1:
                lod_bias = ctx.const_scalar(rest[1])
            # A coord computed from another sample (a dependent read, as GX's
            # indirect texturing makes) has that sample fetched first.
            gather(coord)
            # Coord resolves through operand(): an interpolator Load returns its
            # register with no extra ALU (texture2D stays byte-identical), while
            # a computed coord (e.g. uv*2, or the projective divide) is emitted.
            coord_reg = ctx.to_reg(ctx.operand(coord))[1]
            dst = ctx.fresh()
            if dim == 3:                          # SPIR-V Dim Cube
                if proj:
                    raise LowerError("projective cube sampling unsupported")
                cube = ctx.fresh()
                ctx.emit(_V.kCube, {
                    1: (("reg", coord_reg), False,
                        ctx.swz([2, 2, 0, 1])),   # .zzxy
                    2: (("reg", coord_reg), False,
                        ctx.swz([1, 0, 2, 2])),   # .yxzz
                },
                         cube, False)
                ctx.emit_scalar(_S.kRcp, cube, ("reg", cube | ABS_SOURCE),
                                src_comp=2, mask=0x4)
                bias = ("const", ctx.literal_reg((1.5, 1.5, 1.5, 1.5)))
                ctx.emit(_V.kMad, {1: (("reg", cube), False),
                                   2: (("reg", cube), False, ctx.bsw(2)),
                                   3: (bias, False)},
                         cube, False, mask=0x3)
                p.tfetch(dict(T["tf"], fetch_constant_index=slot,
                              dimension=xu.FetchOpDimension.kCube,
                              lod_bias=lod_bias,
                              src_swizzle=(1 | (0 << 2) | (3 << 4))),
                         dest=dst, src=cube)
            else:
                if proj:                          # uv = coord.xy / coord.last
                    wcomp = max(ctx.vec_size(coord) - 1, 1)
                    rw = ctx.emit_scalar(_S.kRcp, ctx.fresh(),
                                         ("reg", coord_reg), src_comp=wcomp)
                    uv = ctx.fresh()
                    ctx.emit(_V.kMul, {1: (("reg", coord_reg), False),
                                       2: (("reg", rw), False)}, uv, False)
                    coord_reg = uv
                p.tfetch(dict(T["tf"], fetch_constant_index=slot,
                              lod_bias=lod_bias), dest=dst, src=coord_reg)
            sample_reg[ssa_id] = dst
            used_samplers.add(slot)
            return
        if ins.opcode == 61 and ins.operands[0] == merged_local:
            return
        if ins.opcode == 61 and ins.operands[0] in local_store:
            gather(local_store[ins.operands[0]])  # Load of a local
            return
        if ins.opcode == 65:                      # AccessChain: recurse base
            if ins.operands[0] == merged_local:
                return
            if ins.operands[0] in local_store:
                gather(local_store[ins.operands[0]])
            return
        # generic: recurse into any operand that is an SSA result
        skip = 2 if ins.opcode == 12 else 0       # ExtInst: set + inst num
        for w in ins.operands[skip:]:
            if w in m.by_result:
                gather(w)

    out_var = si.color_out[0][0].vid

    # control flow: structured if/else, possibly NESTED, that assigns one
    # common variable in every arm lowers branch-free to chained Xenos cnd*
    # selects (no jumps). The common variable can be o_color directly, or a
    # Function-local later loaded by the post-merge output expression.
    def _block(label):
        """Basic block starting at `label`: instrs after the Label through the
        terminator (Branch / BranchConditional / Kill / Return)."""
        out, inb = [], False
        for ins in m.instrs:
            if ins.opcode == 248 and ins.result_id == label:
                inb = True
                continue
            if inb:
                out.append(ins)
                if ins.opcode in (249, 250, 252, 253, 254):
                    break
        return out

    def _leaf_store_region(blk, target):
        stores_here = [(i.operands[0], i.operands[1])
                       for i in blk if i.opcode == 62]
        if target is not None:
            vals = [v for t, v in stores_here if t == target]
            if not vals:
                name = "o_color" if target == out_var else f"%{target}"
                raise LowerError(f"if/else arm does not assign {name}")
            return ("val", vals[-1]), target

        # v1: accept arms whose meaningful assignment is either the output or
        # a Function-local. Ignore stores to unrelated storage classes.
        candidates = [(t, v) for t, v in stores_here
                      if t == out_var or t in local_vars]
        targets = []
        for t, _v in candidates:
            if t not in targets:
                targets.append(t)
        if len(targets) != 1:
            raise LowerError("if/else arm does not assign o_color")
        target = targets[0]
        vals = [v for t, v in candidates if t == target]
        return ("val", vals[-1]), target

    def _region(label, target=None):
        """A structured region -> ('val', ssa) leaf, or
        ('sel', cmp_instr, true_region, false_region) for a nested if/else,
        plus the variable all leaves assign."""
        blk = _block(label)
        brc = next((i for i in blk if i.opcode == 250), None)
        if brc is not None:                       # nested selection header
            cond, t_lbl, f_lbl = brc.operands
            treg, t_target = _region(t_lbl, target)
            freg, f_target = _region(f_lbl, target or t_target)
            if t_target != f_target:
                raise LowerError("if/else arms assign different variables")
            return ("sel", m.by_result[cond], treg, freg), t_target
        return _leaf_store_region(blk, target)

    def _selection_region(brc):
        cond, t_lbl, f_lbl = brc.operands
        treg, target = _region(t_lbl)
        freg, f_target = _region(f_lbl, target)
        if target != f_target:
            raise LowerError("if/else arms assign different variables")
        return ("sel", m.by_result[cond], treg, freg), target

    def _gather_region(region):
        if region[0] == "val":
            gather(region[1])
        else:
            gather(region[1].operands[0])
            gather(region[1].operands[1])
            _gather_region(region[2])
            _gather_region(region[3])

    def _has_kill(label):
        return any(i.opcode == 252 for i in _block(label))

    top_brc = next((i for i in m.instrs if i.opcode == 250), None)
    region = None
    region_target = None
    merged_local = None
    kill_cmp = None
    kill_on_true = True
    out_val = None
    if top_brc is not None:
        cond_ssa, t_lbl, f_lbl = top_brc.operands
        t_kill, f_kill = _has_kill(t_lbl), _has_kill(f_lbl)
        if t_kill or f_kill:
            if t_kill and f_kill:
                raise LowerError("if/discard kills both arms")
            kill_cmp = m.by_result[cond_ssa]
            kill_on_true = t_kill
            gather(kill_cmp.operands[0])
            gather(kill_cmp.operands[1])
            if out_var not in stores:
                raise LowerError("o_color never written")
            out_val = stores[out_var]
            gather(out_val)
        else:
            region, region_target = _selection_region(top_brc)
            _gather_region(region)
            if region_target == out_var:
                pass
            elif region_target in local_vars:
                merged_local = region_target
                if out_var not in stores:
                    raise LowerError("o_color never written")
                out_val = stores[out_var]
                gather(out_val)
            else:
                raise LowerError("if/else arms assign unsupported variable")
    else:
        if out_var not in stores:
            raise LowerError("o_color never written")
        out_val = stores[out_var]
        gather(out_val)
    # The if/else's merge block, named by the OpSelectionMerge just before its branch.
    merge_label = None
    if top_brc is not None:
        before = m.instrs[m.instrs.index(top_brc) - 1]
        merge_label = before.operands[0] if before.opcode == 247 else None
    _check_local_stores(m, local_vars, merged_local, merge_label)

    # phase 2 (after alloc_colors): arithmetic, operand-typed.
    p.alloc_colors()

    # comparison opcode -> (cnd vector op, swap operands, swap branches)
    _CMP = {
        186: (_V.kCndGt, False, False),   # a >  b : cndgt(a-b)
        184: (_V.kCndGt, True, False),    # a <  b : cndgt(b-a)
        190: (_V.kCndGe, False, False),   # a >= b : cndge(a-b)
        188: (_V.kCndGe, True, False),    # a <= b : cndge(b-a)
        180: (_V.kCndEq, False, False),   # a == b : cndeq(a-b)
        182: (_V.kCndEq, False, True),    # a != b : cndeq(a-b), swap arms
    }
    # comparison opcode -> (kill vector op, swap operands) for true/false arm
    _KILL_TRUE = {
        186: (_V.kKillGt, False),          # a >  b
        184: (_V.kKillGt, True),           # a <  b -> b > a
        190: (_V.kKillGe, False),          # a >= b
        188: (_V.kKillGe, True),           # a <= b -> b >= a
        180: (_V.kKillEq, False),          # a == b
        182: (_V.kKillNe, False),          # a != b
    }
    _KILL_FALSE = {
        186: (_V.kKillGe, True),           # !(a > b) -> b >= a
        184: (_V.kKillGe, False),          # !(a < b) -> a >= b
        190: (_V.kKillGt, True),           # !(a >= b) -> b > a
        188: (_V.kKillGt, False),          # !(a <= b) -> a > b
        180: (_V.kKillNe, False),          # !(a == b) -> a != b
        182: (_V.kKillEq, False),          # !(a != b) -> a == b
    }

    def _bsw(c):
        return 0 if c == "B" else ctx.bsw(c)

    def _emit_kill(cmp, on_true):
        table = _KILL_TRUE if on_true else _KILL_FALSE
        if cmp.opcode not in table:
            raise LowerError(f"v1: unsupported discard comparison {cmp.name}")
        killop, swap_ops = table[cmp.opcode]
        oa, ca = ctx.scalar_origin(cmp.operands[0])
        ob, cb = ctx.scalar_origin(cmp.operands[1])
        if swap_ops:
            (oa, ca), (ob, cb) = (ob, cb), (oa, ca)
        ctx.emit(killop, {1: (oa, False, _bsw(ca)),
                          2: (ob, False, _bsw(cb))},
                 0, False, mask=0)

    def _region_value(region):
        """Operand holding a region's value: a leaf resolves through operand()
        directly (no extra mov); a nested select / general construct spills to
        a temp."""
        if region[0] == "sel":
            r = ctx.fresh()
            _emit_region(region, r, False)
            return ("reg", r)
        try:
            return ctx.operand(region[1])
        except LowerError:                        # e.g. general construct
            r = ctx.fresh()
            ctx.emit_value(region[1], r, False)
            return ("reg", r)

    def _emit_region(region, dst, export):
        """Emit a region tree: a leaf value, or chained cnd* selects for
        nested if/else."""
        if region[0] == "val":
            ctx.emit_value(region[1], dst, export)
            return
        _, cmp, treg, freg = region
        if cmp.opcode not in _CMP:
            raise LowerError(f"v1: unsupported comparison {cmp.name}")
        cndop, swap_ops, swap_br = _CMP[cmp.opcode]
        oa, ca = ctx.scalar_origin(cmp.operands[0])
        ob, cb = ctx.scalar_origin(cmp.operands[1])
        if swap_ops:
            (oa, ca), (ob, cb) = (ob, cb), (oa, ca)
        if swap_br:
            treg, freg = freg, treg
        d = ctx.fresh()                           # d = a - b
        ctx.emit(_V.kAdd, {1: (oa, False, _bsw(ca)),
                           2: (ob, True, _bsw(cb))}, d, False)
        tv = _region_value(treg)
        fv = _region_value(freg)
        if tv[0] == "const" and fv[0] == "const":
            fv = ctx.to_reg(fv)                   # cnd allows <=1 const/op
        ctx.emit(cndop, {1: (("reg", d), False), 2: (tv, False),
                         3: (fv, False)}, dst, export)

    if kill_cmp is not None:
        _emit_kill(kill_cmp, kill_on_true)

    if region is not None and region_target == out_var:
        _emit_region(region, 0, True)
    elif region is not None:
        r = ctx.fresh()
        _emit_region(region, r, False)
        ctx.local_values[merged_local] = ("reg", r)
        ctx.emit_value(out_val, 0, True)
    else:
        ctx.emit_value(out_val, 0, True)

    constants = []
    for slot in sorted(used_samplers):
        constants.append(ce.Constant(f"tex{slot}", ce.RS_SAMPLER, slot, 1,
                                     ce.CLASS_OBJECT, 0xC, rows=1, columns=1))
    # fragment uniform constants (e.g. u_blend vec4)
    for cs in si.constants:
        constants.append(ce.Constant(cs.name, ce.RS_FLOAT4, cs.reg, cs.regs,
                                     ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                                     rows=1, columns=4))
    # compiler-allocated literal constants (host uploads per lit_manifest)
    for reg, _val in ctx.lit_manifest:
        constants.append(ce.Constant(f"_lit{reg}", ce.RS_FLOAT4, reg, 1,
                                     ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                                     rows=1, columns=4))
    return p.finish(constants=constants), ctx.lit_manifest


def lower_ex(m: spirv.Module, registers: RegisterBudget = LIBXENON_REGISTERS, optimize: bool = False):
    """Returns (container_bytes, literal_manifest). The manifest is
    [(constant_register, (f0,f1,f2,f3))] for GLSL literals, optionally plus
    ("loop", loop_id, uniform_name) for a real-loop trip count the host uploads
    to integer_constants[8+loop_id]. registers is the runtime's register
    split; a shader past its kind's share raises RegisterBudgetError. optimize
    has xasm/optimize.py rewrite the instructions before they're allocated."""
    stage = "vertex" if m.entry_points[0][0] == 0 else "pixel"
    try:
        # expand constant-trip loops to straight-line SPIR-V (no-op if none)
        unroll(m)
    except UnrollError as e:
        if stage == "pixel" and any(i.opcode == 246 for i in m.instrs):
            return lower_fragment_real_loop(m, e, registers)
        raise LowerError(str(e)) from e
    if stage == "vertex":
        return lower_vertex(m, registers, optimize)
    return lower_fragment(m, registers, optimize)


def lower(m: spirv.Module, registers: RegisterBudget = LIBXENON_REGISTERS, optimize: bool = False) -> bytes:
    return lower_ex(m, registers, optimize)[0]


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("spv")
    ap.add_argument("-o", "--out")
    ap.add_argument("--pixel-registers", type=int, default=LIBXENON_REGISTERS.pixel,
                    help="pixel shaders' share of the register split (libxenon's by default)")
    ap.add_argument("--vertex-registers", type=int, default=LIBXENON_REGISTERS.vertex,
                    help="vertex shaders' share of the register split (libxenon's by default)")
    ap.add_argument("--optimize", action="store_true", help="rewrite the instructions first (xasm/optimize.py)")
    args = ap.parse_args()
    m = spirv.Module.parse(Path(args.spv).read_bytes())
    blob = lower(m, RegisterBudget(pixel=args.pixel_registers, vertex=args.vertex_registers), args.optimize)
    if args.out:
        Path(args.out).write_bytes(blob)
        print(f"wrote {args.out} ({len(blob)} B)")
    c = container.Container(blob)
    print(xu.disassemble(c.ucode_dwords, c.shader_type))


if __name__ == "__main__":
    main()
