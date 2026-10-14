"""xost's compiler in C (xsdk:src/xsc_compile.c), called through ctypes.

make builds the library, build/host/libxsc.so, in WSL; the tools that compile shaders run there.
"""
from __future__ import annotations

import ctypes
from pathlib import Path

LIBRARY = Path(__file__).resolve().parents[2] / "build" / "host" / "libxsc.so"

OK, REFUSED, REGISTERS, BAD_VALUE, MALFORMED, OUT_OF_MEMORY = range(6)
STATUS_NAMES = ("ok", "refused", "registers", "bad value", "malformed", "out of memory")
LITERAL, ARRAY, LOOP = range(3)
RESOURCE_KINDS = ("push", "uniform", "sampler")
MESSAGE_BYTES = 512


class Options(ctypes.Structure):
    _fields_ = [("pixel_registers", ctypes.c_uint32), ("vertex_registers", ctypes.c_uint32),
                ("optimize", ctypes.c_bool), ("group_fetches", ctypes.c_bool)]


class Need(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("reg", ctypes.c_uint32), ("regs", ctypes.c_uint32),
                ("value", ctypes.c_double * 4), ("name", ctypes.c_char_p)]


class Resource(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("set", ctypes.c_uint32), ("binding", ctypes.c_uint32),
                ("reg", ctypes.c_uint32), ("regs", ctypes.c_uint32)]


class Shader(ctypes.Structure):
    _fields_ = [("container", ctypes.POINTER(ctypes.c_uint8)), ("container_bytes", ctypes.c_size_t),
                ("needs", ctypes.POINTER(Need)), ("need_count", ctypes.c_size_t),
                ("resources", ctypes.POINTER(Resource)), ("resource_count", ctypes.c_size_t),
                ("message", ctypes.c_char * MESSAGE_BYTES)]


class _Xsc(ctypes.Structure):
    _fields_ = [("chunks", ctypes.c_void_p), ("status", ctypes.c_int), ("message", ctypes.c_char * MESSAGE_BYTES)]


class _Var(ctypes.Structure):
    _fields_ = [("vid", ctypes.c_uint32), ("name", ctypes.c_char_p), ("storage_class", ctypes.c_uint32),
                ("pointee", ctypes.c_int64), ("location", ctypes.c_int64), ("binding", ctypes.c_int64),
                ("builtin", ctypes.c_int64), ("descriptor_set", ctypes.c_int64)]


class _VarUse(ctypes.Structure):
    _fields_ = [("var", ctypes.POINTER(_Var)), ("a", ctypes.c_uint32), ("b", ctypes.c_uint32)]


class _ConstantSlot(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("reg", ctypes.c_uint32), ("regs", ctypes.c_uint32),
                ("member", ctypes.c_int32)]


class _Block(ctypes.Structure):
    _fields_ = [("push", ctypes.c_bool), ("var", ctypes.POINTER(_Var)), ("struct_type", ctypes.c_uint32),
                ("reg", ctypes.c_uint32), ("regs", ctypes.c_uint32)]


# The interface xsdk:src/xsc_private.h's XscInterface holds; the compiler's own lookup, its roles, stays in C.
class _Interface(ctypes.Structure):
    _fields_ = [("pixel", ctypes.c_bool),
                ("vfetch", ctypes.POINTER(_VarUse)), ("vfetch_count", ctypes.c_size_t),
                ("interp_out", ctypes.POINTER(_VarUse)), ("interp_out_count", ctypes.c_size_t),
                ("interp_in", ctypes.POINTER(_VarUse)), ("interp_in_count", ctypes.c_size_t),
                ("color_out", ctypes.POINTER(_VarUse)), ("color_out_count", ctypes.c_size_t),
                ("samplers", ctypes.POINTER(_VarUse)), ("sampler_count", ctypes.c_size_t),
                ("position_out", ctypes.c_uint32), ("frag_coord", ctypes.POINTER(_Var)),
                ("constants", ctypes.POINTER(_ConstantSlot)), ("constant_count", ctypes.c_size_t),
                ("blocks", ctypes.POINTER(_Block)), ("block_count", ctypes.c_size_t),
                ("constant_end", ctypes.c_uint32),
                ("sampler_dims", ctypes.POINTER(ctypes.c_int64)), ("sampler_dim_count", ctypes.c_size_t),
                ("roles", ctypes.c_void_p), ("role_limit", ctypes.c_uint32)]


# Room for xsc_private.h's XscModule, which only the C reads.
MODULE_BYTES = 1024


class CompileError(Exception):
    """The compiler's status and message for a shader it didn't compile."""

    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


_library = None


def _load():
    global _library
    if _library is None:
        _library = ctypes.CDLL(str(LIBRARY))
        _library.xsdk_xsc_compile.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t,
                                              ctypes.POINTER(Options), ctypes.POINTER(Shader)]
        _library.xsdk_xsc_compile.restype = ctypes.c_int
        _library.xsdk_xsc_free.argtypes = [ctypes.POINTER(Shader)]
        _library.xsdk_xsc_free.restype = None
        _library.xsdk_xsc_spirv_parse.argtypes = [ctypes.POINTER(_Xsc), ctypes.POINTER(ctypes.c_uint32),
                                                  ctypes.c_size_t, ctypes.c_void_p]
        _library.xsdk_xsc_spirv_parse.restype = ctypes.c_bool
        _library.xsdk_xsc_resolve_interface.argtypes = [ctypes.POINTER(_Xsc), ctypes.c_void_p,
                                                        ctypes.POINTER(_Interface)]
        _library.xsdk_xsc_resolve_interface.restype = ctypes.c_bool
        _library.xsdk_xsc_release.argtypes = [ctypes.POINTER(_Xsc)]
        _library.xsdk_xsc_release.restype = None
    return _library


def _optional(value: int):
    return None if value == -1 else value


def _text(raw: bytes | None) -> str:
    return raw.decode("utf-8", "replace") if raw is not None else ""


def resolve_spirv(spirv: bytes):
    """The interface as xsdk:src/xsc_resolve.c lays it out, in plain values: a dict of lists of tuples and dicts, which
    xsc/resolve.py turns into its dataclasses. Raises CompileError for a module it can't lay out."""
    library = _load()
    words = (ctypes.c_uint32 * (len(spirv) // 4)).from_buffer_copy(spirv[:len(spirv) // 4 * 4])
    x = _Xsc()
    module = ctypes.create_string_buffer(MODULE_BYTES)
    si = _Interface()
    try:
        if not (library.xsdk_xsc_spirv_parse(ctypes.byref(x), words, len(words), module)
                and library.xsdk_xsc_resolve_interface(ctypes.byref(x), module, ctypes.byref(si))):
            raise CompileError(x.status, x.message.decode(errors="replace"))
        cache = {}

        def var(pointer):
            if not pointer:
                return None
            v = pointer.contents
            if v.vid not in cache:
                cache[v.vid] = dict(vid=v.vid, name=_text(v.name), storage_class=v.storage_class,
                                    pointee=_optional(v.pointee), location=_optional(v.location),
                                    binding=_optional(v.binding), builtin=_optional(v.builtin),
                                    descriptor_set=_optional(v.descriptor_set))
            return cache[v.vid]

        def uses(items, count, pair):
            return [(var(items[i].var), items[i].a) + (() if pair else (items[i].b,)) for i in range(count)]

        return dict(
            stage="pixel" if si.pixel else "vertex",
            vfetch=uses(si.vfetch, si.vfetch_count, False),
            interp_out=uses(si.interp_out, si.interp_out_count, False),
            interp_in=uses(si.interp_in, si.interp_in_count, False),
            color_out=uses(si.color_out, si.color_out_count, True),
            samplers=uses(si.samplers, si.sampler_count, True),
            position_out=si.position_out or None,
            frag_coord=var(si.frag_coord),
            constants=[(_text(c.name), c.reg, c.regs, c.member)
                       for c in (si.constants[i] for i in range(si.constant_count))],
            blocks=[("push" if b.push else "uniform", var(b.var), b.struct_type, b.reg, b.regs)
                    for b in (si.blocks[i] for i in range(si.block_count))],
            constant_end=si.constant_end,
            sampler_dims={si.sampler_dims[2 * i]: _optional(si.sampler_dims[2 * i + 1])
                          for i in range(si.sampler_dim_count)})
    finally:
        library.xsdk_xsc_release(ctypes.byref(x))


def _need(n: Need):
    if n.kind == LITERAL:
        return n.reg, tuple(n.value)
    if n.kind == ARRAY:
        return "array", n.reg, n.regs, n.name.decode()
    return "loop", n.reg, n.name.decode()


def compile_spirv(spirv: bytes, pixel_registers: int, vertex_registers: int, optimize: bool = False,
                  group_fetches: bool = True):
    """(container, manifest, resources) as lower.py's lower_ex gives the first two and build_shaders.py the last:
    the manifest [(register, (f0, f1, f2, f3))], with ("array", reg, regs, name) and ("loop", id, name) for a
    hardware loop; the resources [(kind, set, binding, register, registers)], blocks then samplers."""
    library = _load()
    words = (ctypes.c_uint32 * (len(spirv) // 4)).from_buffer_copy(spirv[:len(spirv) // 4 * 4])
    options = Options(pixel_registers, vertex_registers, optimize, group_fetches)
    shader = Shader()
    status = library.xsdk_xsc_compile(words, len(words), ctypes.byref(options), ctypes.byref(shader))
    if status != OK:
        raise CompileError(status, shader.message.decode(errors="replace"))
    try:
        container = ctypes.string_at(shader.container, shader.container_bytes)
        manifest = [_need(shader.needs[i]) for i in range(shader.need_count)]
        resources = [(RESOURCE_KINDS[r.kind], r.set, r.binding, r.reg, r.regs)
                     for r in (shader.resources[i] for i in range(shader.resource_count))]
    finally:
        library.xsdk_xsc_free(ctypes.byref(shader))
    return container, manifest, resources
