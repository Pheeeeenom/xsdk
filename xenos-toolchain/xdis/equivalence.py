#!/usr/bin/env python3
"""Runs a straight-line Xenos program on the CPU, as Xenia's shader interpreter does
(xenia:src/xenia/gpu/shader_interpreter.cc:287-957), to compare two compilations of one shader: what xasm/optimize.py
gives against what it was given (tools/check_optimize.py, tests/test_optimize.py). xdis/simulate.py runs a program for
its numbers; this one tracks which values are undefined, and runs xasm's instructions before they're encoded too.

It runs either the program's microcode (run) or the instructions xasm built before they were encoded (run_fields),
whose registers aren't allocated yet: comparing those shows what a pass over the instructions changed, with every
temp that's read before anything writes it known to be undefined.

A register component nothing has written is None (undefined), and anything computed from one is too, so a comparison
can skip what the original program never defined. Fetches return values that depend only on what they read, so two
programs fetching the same coordinates get the same texels; the values are made up. Doubles stand in for the
hardware's floats: two programs run here compute the same thing or don't; what the GPU prints to the last bit isn't
modelled.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402

V = xu.AluVectorOpcode
S = xu.AluScalarOpcode
FLT_MAX = 3.4028234663852886e38
INF = math.inf
UNDEFINED = [None] * 4


class Unsupported(Exception):
    """The program does something this runner doesn't model (loops, predication, relative addressing, ...)."""


# ---- the operations ----

def _floor(x):
    return math.floor(x) if math.isfinite(x) else x


def _trunc(x):
    return float(math.trunc(x)) if math.isfinite(x) else x


def _recip(a):
    if a == 0.0:
        return math.copysign(INF, a)
    return 1.0 / a


def _exp2(x):
    try:
        return 2.0 ** x
    except OverflowError:
        return INF


def _log2(x):
    if math.isnan(x) or x < 0.0:
        return math.nan
    if x == 0.0:
        return -INF
    return math.log2(x) if math.isfinite(x) else x


def _sqrt(x):
    if math.isnan(x) or x < 0.0:
        return math.nan
    return math.sqrt(x) if math.isfinite(x) else x


def _trig(f, x):
    return f(x) if math.isfinite(x) else math.nan


def _mul(a, b):
    # Direct3D 9 behaviour: 0 times anything is +0 (shader_interpreter.cc:339-347).
    return a * b if a != 0.0 and b != 0.0 else 0.0


def _saturate(x):
    return x if x != x else min(max(x, 0.0), 1.0)


def _texel(constant, coords):
    """A made-up texel for a fetch from the given fetch constant at the given coordinates."""
    if any(c is None for c in coords):
        return UNDEFINED
    h = constant * 0.7311 + sum(c * k for c, k in zip(coords, (1.913, 2.377, 0.611)))
    return [math.sin(h * (i + 1.3)) for i in range(4)]


def _vertex_element(constant, offset, data_format):
    return [math.cos((constant + 1) * 0.5 + offset * 0.37 + data_format * 0.11 + i) for i in range(4)]


_ELEMENTWISE = {
    V.kAdd: lambda x, y, z: x + y,
    V.kMul: lambda x, y, z: _mul(x, y),
    V.kMax: lambda x, y, z: x if x >= y else y,
    V.kMin: lambda x, y, z: x if x < y else y,
    V.kSeq: lambda x, y, z: float(x == y),
    V.kSgt: lambda x, y, z: float(x > y),
    V.kSge: lambda x, y, z: float(x >= y),
    V.kSne: lambda x, y, z: float(x != y),
    V.kFrc: lambda x, y, z: x - _floor(x),
    V.kTrunc: lambda x, y, z: _trunc(x),
    V.kFloor: lambda x, y, z: _floor(x),
    V.kMad: lambda x, y, z: _mul(x, y) + z,
    V.kCndEq: lambda x, y, z: y if x == 0.0 else z,
    V.kCndGe: lambda x, y, z: y if x >= 0.0 else z,
    V.kCndGt: lambda x, y, z: y if x > 0.0 else z,
}
_OPERANDS = {V.kFrc: 1, V.kTrunc: 1, V.kFloor: 1, V.kMax4: 1, V.kMad: 3, V.kCndEq: 3, V.kCndGe: 3, V.kCndGt: 3,
             V.kDp2Add: 3}


def _vector(op, ops):
    x, y, z = (ops + [UNDEFINED] * 3)[:3]
    f = _ELEMENTWISE.get(op)
    if f is not None:
        used = _OPERANDS.get(op, 2)
        out = []
        for i in range(4):
            lanes = (x[i], y[i], z[i])[:used]
            out.append(None if None in lanes else f(*(lanes + (0.0,) * (3 - used))))
        return out

    def whole(reads):
        return None in reads
    if op == V.kDp4:
        return [None if whole(x + y) else sum(_mul(x[i], y[i]) for i in range(4))] * 4
    if op == V.kDp3:
        return [None if whole(x[:3] + y[:3]) else sum(_mul(x[i], y[i]) for i in range(3))] * 4
    if op == V.kDp2Add:
        return [None if whole(x[:2] + y[:2] + z[:1]) else _mul(x[0], y[0]) + _mul(x[1], y[1]) + z[0]] * 4
    if op == V.kMax4:
        return [None if whole(x) else max(x)] * 4
    if op == V.kDst:
        return [1.0, None if None in (x[1], y[1]) else _mul(x[1], y[1]), x[2], y[3]]
    if op in (V.kKillEq, V.kKillGt, V.kKillGe, V.kKillNe):
        return [0.0] * 4
    raise Unsupported(f"vector {op.name}")


_SCALAR = {
    S.kAdds: lambda a, b: a + b,
    S.kMuls: lambda a, b: _mul(a, b),
    S.kMaxs: lambda a, b: a if a >= b else b,
    S.kMins: lambda a, b: a if a < b else b,
    S.kSubs: lambda a, b: a - b,
    S.kSeqs: lambda a, b: float(a == 0.0),
    S.kSgts: lambda a, b: float(a > 0.0),
    S.kSges: lambda a, b: float(a >= 0.0),
    S.kSnes: lambda a, b: float(a != 0.0),
    S.kFrcs: lambda a, b: a - _floor(a),
    S.kTruncs: lambda a, b: _trunc(a),
    S.kFloors: lambda a, b: _floor(a),
    S.kExp: lambda a, b: _exp2(a),
    S.kLogc: lambda a, b: -FLT_MAX if _log2(a) == -INF else _log2(a),
    S.kLog: lambda a, b: _log2(a),
    S.kRcpc: lambda a, b: math.copysign(FLT_MAX, _recip(a)) if math.isinf(_recip(a)) else _recip(a),
    S.kRcpf: lambda a, b: math.copysign(0.0, _recip(a)) if math.isinf(_recip(a)) else _recip(a),
    S.kRcp: lambda a, b: _recip(a),
    S.kRsqc: lambda a, b: math.copysign(FLT_MAX, _recip(_sqrt(a))) if math.isinf(_recip(_sqrt(a)))
    else _recip(_sqrt(a)),
    S.kRsqf: lambda a, b: math.copysign(0.0, _recip(_sqrt(a))) if math.isinf(_recip(_sqrt(a)))
    else _recip(_sqrt(a)),
    S.kRsq: lambda a, b: _recip(_sqrt(a)),
    S.kSqrt: lambda a, b: _sqrt(a),
    S.kSin: lambda a, b: _trig(math.sin, a),
    S.kCos: lambda a, b: _trig(math.cos, a),
}
_TWO_COMPONENT = {S.kAdds, S.kMuls, S.kMaxs, S.kMins, S.kSubs}


# ---- views of an instruction, from its microcode or from xasm's fields ----

class _Microcode:
    """An ALU instruction's microcode, as the executor reads it."""

    def __init__(self, ins):
        self.i = ins

    def __getattr__(self, name):
        return getattr(self.i, name)

    def temp(self, n):
        reg = self.i.src_reg(n)
        if reg & 0x40:
            raise Unsupported("a relative temp")
        return reg & 0x3F, bool(reg & 0x80)


class _Fields:
    """An ALU instruction's fields, as xasm built them (whole registers, ABS_SOURCE marking an absolute value)."""

    def __init__(self, f, abs_flag):
        self.f, self.abs_flag = f, abs_flag

    def vector_opcode(self):
        return V(self.f.get("vector_opcode", V.kMax))

    def scalar_opcode(self):
        return S(self.f.get("scalar_opcode", S.kRetainPrev))

    def is_export(self):
        return bool(self.f.get("is_export"))

    def is_predicated(self):
        return bool(self.f.get("is_predicated"))

    def vector_dest(self):
        return self.f.get("vector_dest", 0)

    def scalar_dest(self):
        return self.f.get("scalar_dest", 0)

    def vector_clamp(self):
        return bool(self.f.get("vector_clamp"))

    def scalar_clamp(self):
        return bool(self.f.get("scalar_clamp"))

    def is_vector_dest_relative(self):
        return bool(self.f.get("is_vector_dest_relative"))

    def is_scalar_dest_relative(self):
        return bool(self.f.get("is_scalar_dest_relative"))

    def GetVectorOpResultWriteMask(self):
        mask = self.f.get("vector_write_mask", 0)
        return mask & ~self.f.get("scalar_write_mask", 0) if self.is_export() else mask

    def GetScalarOpResultWriteMask(self):
        mask = self.f.get("scalar_write_mask", 0)
        return mask & ~self.f.get("vector_write_mask", 0) if self.is_export() else mask

    def GetConstant0WriteMask(self):
        if not self.is_export() or not self.is_scalar_dest_relative():
            return 0
        return 0xF & ~(self.f.get("vector_write_mask", 0) | self.f.get("scalar_write_mask", 0))

    def GetConstant1WriteMask(self):
        if not self.is_export():
            return 0
        return self.f.get("vector_write_mask", 0) & self.f.get("scalar_write_mask", 0)

    def src_reg(self, n):
        return self.f.get(f"src{n}_reg", 0)

    def src_is_temp(self, n):
        return bool(self.f.get(f"src{n}_is_temp", True))

    def src_swizzle(self, n):
        return self.f.get(f"src{n}_swizzle", 0)

    def src_negate(self, n):
        return bool(self.f.get(f"src{n}_negate"))

    def src_const_is_addressed(self, n):
        return bool(self.f.get(f"src{n}_const_rel") or self.f.get("is_const_0_addressed") or
                    self.f.get("is_const_1_addressed"))

    def temp(self, n):
        reg = self.src_reg(n)
        return reg & (self.abs_flag - 1), bool(reg & self.abs_flag)


# ---- the executor ----

class _State:
    def __init__(self, registers, constants):
        self.temps = {r: list(v) for r, v in registers.items()}
        self.constants = constants
        self.exports = {}
        self.previous = None

    def reg(self, r):
        return self.temps.setdefault(r, list(UNDEFINED))


def _operand(ins, n, state):
    if ins.src_is_temp(n):
        index, absolute = ins.temp(n)
        value = list(state.reg(index))
        if absolute:
            value = [None if x is None else abs(x) for x in value]
    else:
        if ins.src_const_is_addressed(n):
            raise Unsupported("a relative constant")
        value = list(state.constants.get(ins.src_reg(n), UNDEFINED))
    swizzle = ins.src_swizzle(n)
    value = [value[xu.AluInstruction.GetSwizzledComponentIndex(swizzle, i)] for i in range(4)]
    return [None if x is None else -x for x in value] if ins.src_negate(n) else value


def _execute_alu(ins, state):
    if ins.is_predicated():
        raise Unsupported("predication")
    vop, sop = ins.vector_opcode(), ins.scalar_opcode()
    vmask, smask = ins.GetVectorOpResultWriteMask(), ins.GetScalarOpResultWriteMask()
    result = list(UNDEFINED)
    if vmask or vop in (V.kKillEq, V.kKillGt, V.kKillGe, V.kKillNe):
        if vop in (V.kMaxA, V.kSetpEqPush, V.kSetpNePush, V.kSetpGtPush, V.kSetpGePush, V.kCube):
            raise Unsupported(f"vector {vop.name}")
        count = _OPERANDS.get(vop, 2)
        result = _vector(vop, [_operand(ins, n, state) for n in range(1, count + 1)])
    if sop != S.kRetainPrev:
        f = _SCALAR.get(sop)
        if f is None:
            raise Unsupported(f"scalar {sop.name}")
        src = _operand(ins, 3, state)
        a, b = src[3], src[0]
        state.previous = None if a is None or (sop in _TWO_COMPONENT and b is None) else f(a, b)
    if ins.vector_clamp():
        result = [None if x is None else _saturate(x) for x in result]
    scalar = state.previous
    if ins.scalar_clamp() and scalar is not None:
        scalar = _saturate(scalar)
    if ins.is_export():
        one, zero = ins.GetConstant1WriteMask(), ins.GetConstant0WriteMask()
        out = state.exports.setdefault(ins.vector_dest(), [None] * 4)
        for i in range(4):
            bit = 1 << i
            if vmask & bit:
                out[i] = result[i]
            elif smask & bit:
                out[i] = scalar
            elif one & bit:
                out[i] = 1.0
            elif zero & bit:
                out[i] = 0.0
        return
    if ins.is_vector_dest_relative() or ins.is_scalar_dest_relative():
        raise Unsupported("a relative destination")
    dest = state.reg(ins.vector_dest())
    for i in range(4):
        if vmask >> i & 1:
            dest[i] = result[i]
    dest = state.reg(ins.scalar_dest())
    for i in range(4):
        if smask >> i & 1:
            dest[i] = scalar


def _store_fetch(dest, swizzle, value):
    for i in range(4):
        s = (swizzle >> (3 * i)) & 7
        if s < 4:
            dest[i] = value[s]
        elif s == 5:
            dest[i] = 1.0
        elif s != 7:
            dest[i] = 0.0


def _texture_fetch(state, dest, dest_swizzle, src, src_swizzle, constant):
    coords = [state.reg(src)[(src_swizzle >> (2 * i)) & 3] for i in range(3)]
    _store_fetch(state.reg(dest), dest_swizzle, _texel(constant, coords))


def run(blob: bytes, registers: dict, constants: dict) -> dict:
    """Runs the microcode with the given starting registers {index: [x, y, z, w]} (any other register undefined) and
    constants; returns its exports {export register: [x, y, z, w]}, with None for what wasn't exported or defined."""
    c = container.Container(blob)
    records = xu._decode_program(c.ucode_dwords, c.shader_type)[0]
    state = _State(registers, constants)
    for rec in records:
        if rec.kind == "cf":
            word = rec.text.split()[0] if rec.text else ""
            if not word.startswith(("exec", "alloc", "cnop")):
                raise Unsupported(f"control flow {rec.text!r}")
        elif rec.kind == "tfetch":
            ins = xu.TextureFetchInstruction(*rec.raw)
            if ins.is_predicated() or ins.is_dest_relative() or ins.is_src_relative():
                raise Unsupported("a predicated or relative fetch")
            _texture_fetch(state, ins.dest(), ins.dest_swizzle(), ins.src(), ins.src_swizzle(),
                           ins.fetch_constant_index())
        elif rec.kind == "vfetch":
            ins = xu.VertexFetchInstruction(*rec.raw)
            if ins.is_predicated() or ins.is_dest_relative():
                raise Unsupported("a predicated or relative fetch")
            _store_fetch(state.reg(ins.dest()), ins.dest_swizzle(),
                         _vertex_element(ins.fetch_constant_index(), ins.offset(), ins.data_format()))
        elif rec.kind == "alu":
            _execute_alu(_Microcode(xu.AluInstruction(*rec.raw)), state)
    return state.exports


def run_fields(instructions, registers: dict, constants: dict, abs_flag: int) -> dict:
    """Runs xasm instructions as built, before encoding: [(kind, fields)] in program order, registers whole."""
    state = _State(registers, constants)
    for kind, f in instructions:
        if kind == "alu":
            _execute_alu(_Fields(f, abs_flag), state)
        elif kind == "tfetch":
            if f.get("is_predicated") or f.get("is_dest_relative") or f.get("is_src_relative"):
                raise Unsupported("a predicated or relative fetch")
            _texture_fetch(state, f["dest"], f.get("dest_swizzle", 0), f.get("src", 0), f.get("src_swizzle", 0),
                           f.get("fetch_constant_index", 0))
        elif kind == "vfetch":
            if f.get("is_predicated") or f.get("is_dest_relative"):
                raise Unsupported("a predicated or relative fetch")
            constant = f.get("const_index", 0) * 3 + f.get("const_index_sel", 0)
            _store_fetch(state.reg(f["dest"]), f.get("dest_swizzle", 0),
                         _vertex_element(constant, f.get("offset", 0), int(f.get("data_format", 0))))
        else:
            raise Unsupported(f"instruction {kind}")
    return state.exports


def inputs(instructions) -> set:
    """The registers xasm instructions read whole before anything writes them: a pixel shader's interpolators and
    pixel parameters, a vertex shader's index."""
    written, read_first = set(), set()
    for kind, f in instructions:
        if kind == "alu":
            for n in (1, 2, 3):
                if f.get(f"src{n}_is_temp", True):
                    r = f.get(f"src{n}_reg", 0) & 0xFFFF
                    if r not in written:
                        read_first.add(r)
            if not f.get("is_export"):
                if f.get("vector_write_mask", 0):
                    written.add(f.get("vector_dest", 0))
                if f.get("scalar_write_mask", 0):
                    written.add(f.get("scalar_dest", 0))
        else:
            if f.get("src", 0) not in written:
                read_first.add(f.get("src", 0))
            written.add(f["dest"])
    return read_first
