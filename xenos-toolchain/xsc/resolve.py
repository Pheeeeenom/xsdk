#!/usr/bin/env python3
"""SPIR-V interface resolution for xsc: the layout xost's compiler gives a shader's interface, which it does in C
(xsdk:src/xsc_resolve.c) and xsc/native.py calls, as Python dataclasses.

Each interface variable is classified into the Xenos role its loads and stores lower to (vfetch / interpolator /
constant / sampler / position / color export), with the metadata the container needs.

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
import xsc.native as native  # noqa: E402
import xsc.spirv as spirv  # noqa: E402


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
    front_facing: Var | None = None                   # and its gl_FrontFacing
    color_out: list = field(default_factory=list)     # (Var, oC index)
    constants: list = field(default_factory=list)     # ConstantSlot
    samplers: list = field(default_factory=list)      # (Var, fetch_slot)
    blocks: list = field(default_factory=list)        # Block, by register
    # the first register past every constant and block, where literals start
    constant_end: int = 0
    # fetch_slot -> SPIR-V Dim
    sampler_dims: dict = field(default_factory=dict)


def resolve_interface(m: spirv.Module) -> ShaderInterface:
    """The interface's layout; ValueError for a module the compiler can't lay out."""
    try:
        r = native.resolve_spirv(m.blob)
    except native.CompileError as e:
        raise ValueError(str(e)) from None
    vars_ = {}

    def var(d):
        if d is None:
            return None
        if d["vid"] not in vars_:
            vars_[d["vid"]] = Var(**d)
        return vars_[d["vid"]]

    return ShaderInterface(
        stage=r["stage"],
        vfetch=[(var(v), a, b) for v, a, b in r["vfetch"]],
        interp_out=[(var(v), a, b) for v, a, b in r["interp_out"]],
        interp_in=[(var(v), a, b) for v, a, b in r["interp_in"]],
        position_out=r["position_out"],
        frag_coord=var(r["frag_coord"]),
        front_facing=var(r["front_facing"]),
        color_out=[(var(v), a) for v, a in r["color_out"]],
        constants=[ConstantSlot(*c) for c in r["constants"]],
        samplers=[(var(v), a) for v, a in r["samplers"]],
        blocks=[Block(kind, var(v), var(v).descriptor_set, var(v).binding, struct, reg, regs)
                for kind, v, struct, reg, regs in r["blocks"]],
        constant_end=r["constant_end"],
        sampler_dims=r["sampler_dims"])


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
