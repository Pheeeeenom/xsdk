#!/usr/bin/env python3
"""SPIR-V type + interface resolution for xsc.

Turns a parsed `spirv.Module` into:
  - a type table (id -> ResolvedType, with vec/mat shapes), and
  - a ShaderInterface: each interface variable classified into the Xenos
    role its loads/stores lower to (vfetch / interpolator / constant /
    sampler / position / color export), plus the metadata the container
    emitter needs (vertex elements, interpolators, constants, samplers).

Conventions for the GLSL<->Xenos mapping (documented; v1):
  - VS inputs: assigned vfetch in Location order; Xenos vertex-element usage
    by location: 0->POSITION, 1->COLOR, 2->TEXCOORD, 3+->TEXCOORD(idx++).
    The host vertex format must match this order (same as our hand shaders).
  - VS outputs: BuiltIn Position -> oPos; Location N -> interpolator oN
    (usage TEXCOORD for vec2, COLOR otherwise — a heuristic; overridable).
  - FS inputs: Location N -> interpolator register rN (must match the VS).
  - Uniform and push constant blocks hold raw std140 bytes: push constants
    take the first registers after any bare uniforms, then each uniform block
    in (DescriptorSet, Binding) order. A member at byte offset o of a block at
    register b sits in b + o/16 from component (o%16)/4; a matrix keeps its
    memory layout, one column (column-major) or row a register (Block).
  - Samplers (UniformConstant sampled image): with a DescriptorSet decoration
    (Vulkan), fetch constants from 0 in (DescriptorSet, Binding) order;
    without (OpenGL), fetch-constant = Binding.
  - FS output Location 0 -> oC0.
"""
from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import xsc.spirv as spirv  # noqa: E402

# SPIR-V StorageClass
SC_UNIFORM_CONSTANT = 0
SC_INPUT = 1
SC_UNIFORM = 2
SC_OUTPUT = 3
SC_FUNCTION = 7
SC_PUSH_CONSTANT = 9

# SPIR-V Decoration
DEC_BLOCK = 2
DEC_ROW_MAJOR = 4
DEC_COL_MAJOR = 5
DEC_ARRAY_STRIDE = 6
DEC_MATRIX_STRIDE = 7
DEC_BUILTIN = 11
DEC_LOCATION = 30
DEC_BINDING = 33
DEC_DESCRIPTOR_SET = 34
DEC_OFFSET = 35

# SPIR-V BuiltIn
BUILTIN_POSITION = 0
BUILTIN_POINT_SIZE = 1
# input builtins we ignore as real vertex inputs
BUILTIN_VERTEX_ID = 5
BUILTIN_INSTANCE_ID = 6
BUILTIN_FRAG_COORD = 15
_IGNORED_INPUT_BUILTINS = {BUILTIN_VERTEX_ID, BUILTIN_INSTANCE_ID}


@dataclass
class RType:
    kind: str                 # void/bool/int/float/vector/matrix/pointer/
                              # struct/image/sampledimage/array/function/void
    comp: int | None = None   # element type id (vector/matrix/pointer/array)
    count: int | None = None  # vector size / matrix columns / array length
    members: tuple = ()        # struct member type ids
    storage_class: int | None = None  # pointer
    dim: int | None = None     # OpTypeImage Dim (0=1D, 1=2D, 2=3D, 3=Cube)

    def __repr__(self):
        if self.kind == "vector":
            return f"vec{self.count}"
        if self.kind == "matrix":
            return f"mat{self.count}"
        if self.kind == "float":
            return "float"
        if self.kind == "pointer":
            return f"ptr({self.storage_class})"
        return self.kind


def resolve_types(m: spirv.Module) -> dict:
    t: dict[int, RType] = {}
    for ins in m.instrs:
        op, rid, ops = ins.opcode, ins.result_id, ins.operands
        if op == 19:
            t[rid] = RType("void")
        elif op == 20:
            t[rid] = RType("bool")
        elif op == 21:
            t[rid] = RType("int")
        elif op == 22:
            t[rid] = RType("float")
        elif op == 23:  # TypeVector comp count
            t[rid] = RType("vector", comp=ops[0], count=ops[1])
        elif op == 24:  # TypeMatrix colType colCount
            t[rid] = RType("matrix", comp=ops[0], count=ops[1])
        elif op == 25:  # TypeImage sampledType dim ...
            t[rid] = RType("image", comp=ops[0], dim=ops[1])
        elif op == 26:
            t[rid] = RType("sampler")
        elif op == 27:  # TypeSampledImage imageType
            t[rid] = RType("sampledimage", comp=ops[0])
        elif op == 28:  # TypeArray elemType lengthConstId
            t[rid] = RType("array", comp=ops[0], count=ops[1])
        elif op == 30:  # TypeStruct members...
            t[rid] = RType("struct", members=tuple(ops))
        elif op == 32:  # TypePointer storageClass pointee
            t[rid] = RType("pointer", storage_class=ops[0], comp=ops[1])
        elif op == 33:
            t[rid] = RType("function")
    return t


def _decos(m: spirv.Module, vid: int) -> dict:
    """Flatten a variable's decorations into {name: args}."""
    out = {}
    for deco, args in m.decorations.get(vid, []):
        out[deco] = args
    return out


@dataclass
class Var:
    vid: int
    name: str
    storage_class: int
    pointee: int             # resolved type id behind the pointer
    location: int | None = None
    binding: int | None = None
    builtin: int | None = None
    descriptor_set: int | None = None


@dataclass
class ConstantSlot:
    name: str
    reg: int                 # base constant register
    regs: int                # how many (mat4 = 4)
    member: int


@dataclass
class Block:
    """A uniform or push constant block: its registers hold the block's bytes."""
    kind: str                # "uniform" or "push"
    var: Var
    descriptor_set: int | None
    binding: int | None
    struct: int              # struct type id
    reg: int                 # first constant register
    regs: int                # registers the block's bytes fill


@dataclass
class ShaderInterface:
    stage: str
    vfetch: list = field(default_factory=list)        # (Var, usage, uidx)
    interp_out: list = field(default_factory=list)    # (Var, o_index, usage)
    interp_in: list = field(default_factory=list)     # (Var, r_reg, usage)
    position_out: int | None = None                   # gl_Position var id
    frag_coord: Var | None = None                     # a pixel shader's gl_FragCoord
    color_out: list = field(default_factory=list)     # (Var, oC index)
    constants: list = field(default_factory=list)     # ConstantSlot
    samplers: list = field(default_factory=list)      # (Var, fetch_slot)
    blocks: list = field(default_factory=list)        # Block, by register
    # the first register past every constant and block, where literals start
    constant_end: int = 0
    # fetch_slot -> SPIR-V Dim
    sampler_dims: dict = field(default_factory=dict)
    # role lookup the lowering pass uses: var_id -> ("kind", payload)
    roles: dict = field(default_factory=dict)


def _usage_for_location(loc: int):
    import xdis.container_emit as ce
    table = {0: (ce.USAGE_POSITION, 0), 1: (ce.USAGE_COLOR, 0),
             2: (ce.USAGE_TEXCOORD, 0)}
    return table.get(loc, (ce.USAGE_TEXCOORD, max(0, loc - 2)))


def member_decos(m: spirv.Module, struct: int, member: int) -> dict:
    out = {}
    for deco, args in m.member_decorations.get((struct, member), []):
        out[deco] = args
    return out


def type_bytes(m: spirv.Module, types: dict, tid: int, member: dict) -> int:
    """Bytes a block member of this type spans, by its std140 decorations."""
    t = types.get(tid)
    if t is None or t.kind in ("float", "int", "bool"):
        return 4
    if t.kind == "vector":
        return 4 * t.count
    if t.kind == "matrix":
        stride = member.get(DEC_MATRIX_STRIDE, (16,))[0]
        col = types.get(t.comp)
        rows = col.count if col and col.kind == "vector" else 1
        lines = rows if DEC_ROW_MAJOR in member else t.count
        return stride * lines
    if t.kind == "array":
        stride = _decos(m, tid).get(DEC_ARRAY_STRIDE, (16,))[0]
        lc = m.by_result.get(t.count)
        length = lc.operands[0] if lc and lc.opcode == 43 else 1
        return stride * length
    if t.kind == "struct":
        return max((member_decos(m, tid, i).get(DEC_OFFSET, (0,))[0] +
                    type_bytes(m, types, mt, member_decos(m, tid, i))
                    for i, mt in enumerate(t.members)), default=0)
    raise ValueError(f"block member of type {t.kind}")


# The console loads float constants from memory only in whole groups of 4 from a multiple of 4
# (xsdk:docs/HARDWARE.md, "Constants loaded from memory go in groups of 4 too").
CONSTANT_GROUP = 4


def _round_up(reg, group):
    return (reg + group - 1) // group * group


def _assign_blocks(m, types, si, pending, first_reg):
    """Registers for each block, push constants first, then (set, binding). Each uniform block has whole groups of 4
    to itself, so a host can load it straight from its buffer without touching anything else."""
    order = sorted(pending, key=lambda b: (b[0] != "push", b[1].descriptor_set or 0, b[1].binding or 0))
    reg = first_reg
    for kind, v in order:
        if kind == "uniform":
            reg = _round_up(reg, CONSTANT_GROUP)
        regs = (type_bytes(m, types, v.pointee, {}) + 15) // 16
        si.roles[v.vid] = ("block", len(si.blocks))
        si.blocks.append(Block(kind, v, v.descriptor_set, v.binding, v.pointee, reg, regs))
        for mi, mtid in enumerate(types[v.pointee].members):
            md = member_decos(m, v.pointee, mi)
            off = md.get(DEC_OFFSET, (16 * mi,))[0]
            mregs = (type_bytes(m, types, mtid, md) + off % 16 + 15) // 16
            mname = m.member_names.get((v.pointee, mi), f"m{mi}")
            si.constants.append(ConstantSlot(mname, reg + off // 16, mregs, mi))
        reg += regs
        if kind == "uniform":
            reg = _round_up(reg, CONSTANT_GROUP)
    return reg


def resolve_interface(m: spirv.Module) -> ShaderInterface:
    import xdis.container_emit as ce
    types = resolve_types(m)
    ep = m.entry_points[0]
    stage = "vertex" if ep[0] == 0 else "pixel"
    si = ShaderInterface(stage)
    _next_const = [0]  # mutable cursor for default-uniform constant regs

    # collect all Variables
    variables = []
    for ins in m.instrs:
        if ins.opcode == 59:  # OpVariable result=ptr-type, sc
            ptr = types.get(ins.result_type)
            sc = ins.operands[0]
            d = _decos(m, ins.result_id)
            loc = d.get(DEC_LOCATION, (None,))[0]
            binding = d.get(DEC_BINDING, (None,))[0]
            builtin = d.get(DEC_BUILTIN, (None,))[0]
            dset = d.get(DEC_DESCRIPTOR_SET, (None,))[0]
            variables.append(Var(
                ins.result_id, m.names.get(ins.result_id, ""),
                sc, ptr.comp if ptr else None, loc, binding, builtin, dset))

    pending_blocks = []
    vulkan_samplers = []

    for v in variables:
        if v.storage_class == SC_INPUT:
            if v.builtin in _IGNORED_INPUT_BUILTINS:
                continue
            if stage == "vertex":
                usage, uidx = _usage_for_location(v.location or 0)
                si.vfetch.append((v, usage, uidx))
                si.roles[v.vid] = ("vfetch", len(si.vfetch) - 1)
            elif v.builtin == BUILTIN_FRAG_COORD:  # from the pixel parameters, not an interpolator
                si.frag_coord = v
                si.roles[v.vid] = ("frag_coord", None)
            else:  # pixel interpolator input
                r = v.location or 0
                shape = types.get(v.pointee)
                usage = (ce.USAGE_TEXCOORD
                         if shape and shape.kind == "vector"
                         and shape.count == 2 else ce.USAGE_COLOR)
                si.interp_in.append((v, r, usage))
                si.roles[v.vid] = ("interp_in", r)
        elif v.storage_class == SC_OUTPUT:
            st_out = types.get(v.pointee)
            if st_out and st_out.kind == "struct":
                # gl_PerVertex: the member decorated BuiltIn Position is
                # gl_Position; stores reach it via AccessChain(var, member).
                for mi in range(len(st_out.members)):
                    for deco, args in m.member_decorations.get(
                            (v.pointee, mi), []):
                        if deco == DEC_BUILTIN and args[0] == \
                                BUILTIN_POSITION:
                            si.position_out = v.vid
                            si.roles[v.vid] = ("position_struct", mi)
                continue
            if v.builtin == BUILTIN_POSITION:
                si.position_out = v.vid
                si.roles[v.vid] = ("position_struct", None)
            elif v.builtin is not None:
                continue  # PointSize etc. ignored in v1
            elif stage == "vertex":
                o = v.location or 0
                shape = types.get(v.pointee)
                usage = (ce.USAGE_TEXCOORD
                         if shape and shape.kind == "vector"
                         and shape.count == 2 else ce.USAGE_COLOR)
                si.interp_out.append((v, o, usage))
                si.roles[v.vid] = ("interp_out", o)
            else:  # pixel color output
                si.color_out.append((v, v.location or 0))
                si.roles[v.vid] = ("color", v.location or 0)
        elif v.storage_class in (SC_UNIFORM, SC_PUSH_CONSTANT):
            st = types.get(v.pointee)
            if st and st.kind == "struct":
                kind = "push" if v.storage_class == SC_PUSH_CONSTANT else "uniform"
                pending_blocks.append((kind, v))
        elif v.storage_class == SC_UNIFORM_CONSTANT:
            st = types.get(v.pointee)
            if st and st.kind in ("sampledimage", "image", "sampler") and v.descriptor_set is not None:
                vulkan_samplers.append(v)
            elif st and st.kind in ("sampledimage", "image", "sampler"):
                slot = v.binding if v.binding is not None else len(si.samplers)
                img = types.get(st.comp) if st.kind == "sampledimage" else st
                dim = img.dim if img and img.kind == "image" else None
                si.samplers.append((v, slot))
                si.sampler_dims[slot] = dim
                si.roles[v.vid] = ("sampler", slot, dim)
            elif st and st.kind in ("matrix", "vector", "float"):
                # default-block uniform (bare `uniform mat4 u_mvp;`):
                # constant register base = Location, else next free.
                regs = st.count if st.kind == "matrix" else 1
                reg = v.location if v.location is not None else _next_const[0]
                _next_const[0] = max(_next_const[0], reg + regs)
                si.constants.append(
                    ConstantSlot(v.name or f"u{reg}", reg, regs, -1))
                si.roles[v.vid] = ("constant", (reg, regs))
            elif st and st.kind == "array":
                # uniform array (`uniform vec4 w[N]`): N contiguous constant
                # registers, element i at base+i. Indexed by a loop counter it
                # becomes aL-relative addressing c[base+aL].
                elem = types.get(st.comp)
                elem_regs = (elem.count if elem and elem.kind == "matrix"
                             else 1)
                lc = m.by_result.get(st.count)
                length = lc.operands[0] if lc and lc.opcode == 43 else 1
                regs = max(int(length), 1) * elem_regs
                reg = v.location if v.location is not None else _next_const[0]
                _next_const[0] = max(_next_const[0], reg + regs)
                si.constants.append(
                    ConstantSlot(v.name or f"u{reg}", reg, regs, -1))
                si.roles[v.vid] = ("array", (reg, regs, elem_regs))

    si.constant_end = _assign_blocks(m, types, si, pending_blocks, _next_const[0])
    vulkan_samplers.sort(key=lambda v: (v.descriptor_set, v.binding or 0))
    for slot, v in enumerate(vulkan_samplers):
        st = types.get(v.pointee)
        img = types.get(st.comp) if st.kind == "sampledimage" else st
        dim = img.dim if img and img.kind == "image" else None
        si.samplers.append((v, slot))
        si.sampler_dims[slot] = dim
        si.roles[v.vid] = ("sampler", slot, dim)

    si.vfetch.sort(key=lambda x: x[0].location or 0)
    si.interp_in.sort(key=lambda x: x[1])
    si.interp_out.sort(key=lambda x: x[1])
    return si


def main():
    for path in sys.argv[1:]:
        m = spirv.Module.parse(Path(path).read_bytes())
        si = resolve_interface(m)
        print(f"=== {path}  [{si.stage}]")
        for v, usage, uidx in si.vfetch:
            print(f"  vfetch  {v.name:14} loc{v.location} -> "
                  f"usage{usage}.{uidx}")
        for v, r, usage in si.interp_in:
            print(f"  interp_in  {v.name:11} -> r{r} (usage{usage})")
        for v, o, usage in si.interp_out:
            print(f"  interp_out {v.name:11} -> o{o} (usage{usage})")
        for b in si.blocks:
            print(f"  {b.kind:7} {b.var.name or '(block)':11} set {b.descriptor_set} binding {b.binding} -> "
                  f"c{b.reg}..c{b.reg + b.regs - 1}")
        if si.position_out is not None:
            print(f"  position   %{si.position_out} -> oPos")
        for v, idx in si.color_out:
            print(f"  color_out  {v.name:11} -> oC{idx}")
        for cs in si.constants:
            print(f"  constant   {cs.name:11} -> c{cs.reg}"
                  f"{'' if cs.regs == 1 else f'..c{cs.reg+cs.regs-1}'}")
        for v, slot in si.samplers:
            print(f"  sampler    {v.name:11} -> tf{slot}")


if __name__ == "__main__":
    main()
