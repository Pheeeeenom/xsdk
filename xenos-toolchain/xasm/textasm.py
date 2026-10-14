#!/usr/bin/env python3
"""xasm text assembler: parses this toolchain's own disassembly format
(xdis pretty output) back into byte-identical Xenos microcode, plus
directives for authoring complete shader containers by hand.

Round-trip contract: for programs following the toolchain's hardware-proven
conventions (see CONVENTIONS below), `assemble_text(disassemble(ucode)) ==
ucode` byte-for-byte. The 16-blob gate in tests/test_textasm.py enforces it
over the golden corpus, the authored xost shaders, and the combiner bank.

Grammar: the emitters in xdis/xenos_ucode.py are the normative spec
(_disassemble_result_operand / _disassemble_source_operand, the Parsed*
Disassemble methods). docs/textasm.md is the human-readable mirror.

CONVENTIONS (hidden fields not present in pretty text):
- exec: is_predicate_clean=True (unless '// PredicateClean=false'),
  is_yield from 'Yield=true', addressing_mode absolute,
  vc_hi=15/vc_lo=0 on vertex-shader execs containing a fetch, else 0.
- alloc: size from 'export = N' (else 0), is_unserialized=False.
- ALU scalar half without a '+' co-issue line: the kRetainPrev idiom
  (canonical field values extracted from the golden corpus; identical
  across every corpus vector-only instruction).
- vfetch signed_rf_mode is unprintable: kZeroClampMinusOne.

v1 limitations (assembly errors, all absent from the 16-blob gate):
relative/abs-value ALU operands ([N+a0], _abs), constant-0/1 dest write
masks, scalar-only ALU lines, vfetch_done.
"""
from __future__ import annotations

import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

import xdis.xenos_ucode as xu  # noqa: E402
from xasm import encode as enc  # noqa: E402

K = xu.ControlFlowOpcode

# canonical hidden ALU fields (extracted from corpus vs0 slots 7-13; every
# vector-only instruction in the golden corpus carries exactly these)
ALU_HIDDEN = {
    "scalar_opcode": xu.AluScalarOpcode.kRetainPrev,
    "scalar_dest": 0,
    "is_scalar_dest_relative": False,
    "scalar_write_mask": 0,
    "scalar_clamp": False,
    "src3_reg": 0,
    "src3_is_temp": True,
    "src3_swizzle": 0,
    "src3_negate": False,
    "is_const_0_addressed": False,
    "is_const_1_addressed": False,
    "is_const_address_register_relative": False,
    "abs_constants": False,
    "is_predicated": False,
    "predicate_condition": False,
}

_VEC_OPS = {info[0]: (xu.AluVectorOpcode(i), info[1])
            for i, info in enumerate(xu.kAluVectorOpcodeInfos)}
_SCA_OPS = {info[0]: (xu.AluScalarOpcode(i), info[1], info[2])
            for i, info in enumerate(xu.kAluScalarOpcodeInfos)}

_TFETCH_NAMES = {}
for _opc, (_names, _hd, _hc, _ha, _occ) in xu._TFETCH_OPCODE_INFO.items():
    for _dim, _n in enumerate(_names):
        _TFETCH_NAMES.setdefault(_n, (_opc, _dim, _hd, _hc))

_VF_FMT_BY_NAME = {v: k for k, v in
                   xu._VERTEX_FETCH_DATA_FORMAT_NAMES.items()}
_FILTER_BY_NAME = {n: i for i, n in enumerate(xu._TEXTURE_FILTER_NAMES)}
# NB: positional enumerate would be wrong — AnisoFilter value 6 is undefined
# in hardware, so "keep" (kUseFetchConst) is index 6 but VALUE 7.
_ANISO_BY_NAME = {n: i for i, n in enumerate(xu._ANISO_FILTER_NAMES[:-1])}
_ANISO_BY_NAME["keep"] = int(xu.AnisoFilter.kUseFetchConst)

_XYZW = "xyzw"


class AsmError(ValueError):
    def __init__(self, msg, lineno=None, line=""):
        super().__init__(
            f"line {lineno}: {msg}: {line.strip()!r}" if lineno else msg)


# ---------------------------------------------------------------------------
# operand parsing
# ---------------------------------------------------------------------------

_SRC_RE = re.compile(
    r"^(?P<neg>-)?(?P<bank>r|c)(?P<abs>_abs)?"
    r"(?:(?P<idx>\d+)|\[(?P<relidx>\d+)\+(?P<rel>a0|aL)\])"
    r"(?:\.(?P<swiz>[xyzw]+))?$")

_DEST_RE = re.compile(
    r"^(?P<name>oPos|oPts|oDepth|eA|eM|oC|o|r)(?P<idx>\d+)?"
    r"(?:\.(?P<mask>[xyzw_01]+))?$")


def _swizzle_delta(chars: str, positions) -> int:
    """Re-encode printed swizzle letters to the 8-bit delta encoding.
    positions: which component slot each printed char occupies."""
    out = 0
    for ch, pos in zip(chars, positions):
        out |= ((_XYZW.index(ch) - pos) & 3) << (2 * pos)
    return out


def parse_alu_source(tok: str, n: int, comp_count: int, lineno: int) -> dict:
    m = _SRC_RE.match(tok)
    if not m:
        raise AsmError(f"bad ALU source operand {tok!r}", lineno, tok)
    if m.group("abs") or m.group("rel"):
        raise AsmError("relative/_abs ALU operands not supported in v1",
                       lineno, tok)
    idx = int(m.group("idx"))
    swiz_text = m.group("swiz")
    if swiz_text is None:
        swiz = 0
    elif comp_count == 4:
        if len(swiz_text) != 4:
            raise AsmError("expected 4-component swizzle", lineno, tok)
        swiz = _swizzle_delta(swiz_text, range(4))
    elif comp_count == 1:
        if len(swiz_text) != 1:
            raise AsmError("expected 1-component swizzle", lineno, tok)
        swiz = _swizzle_delta(swiz_text, (3,))
    elif comp_count == 2:
        if len(swiz_text) != 2:
            raise AsmError("expected 2-component swizzle", lineno, tok)
        # printed chars live at components 3 (left) and 0 (right); the
        # unprinted components 1-2 are don't-cares that the XDK fills by
        # replicating the right-hand char (corpus cube_ps: '.ww' -> wwww=27)
        swiz = _swizzle_delta(swiz_text, (3, 0)) | \
            _swizzle_delta(swiz_text[1] * 2, (1, 2))
    else:
        raise AsmError("unsupported operand component count", lineno, tok)
    return {
        f"src{n}_reg": idx,
        f"src{n}_is_temp": m.group("bank") == "r",
        f"src{n}_swizzle": swiz,
        f"src{n}_negate": bool(m.group("neg")),
    }


def parse_alu_dest(tok: str, shader_type: str, lineno: int):
    """Returns (is_export, vector_dest, write_mask)."""
    m = _DEST_RE.match(tok)
    if not m:
        raise AsmError(f"bad ALU dest operand {tok!r}", lineno, tok)
    name, idx = m.group("name"), m.group("idx")
    idx = int(idx) if idx is not None else None
    E = xu.ExportRegister
    if name == "r":
        is_export, dest = False, idx
    elif name == "o":
        is_export, dest = True, int(E.kVSInterpolator0) + idx
    elif name == "oPos":
        is_export, dest = True, int(E.kVSPosition)
    elif name == "oPts":
        is_export, dest = True, int(E.kVSPointSizeEdgeFlagKillVertex)
    elif name == "oC":
        # PS color exports are module constants, not enum members
        is_export, dest = True, xu.kPSColor0 + idx
    elif name == "oDepth":
        is_export, dest = True, int(E.kPSDepth)
    elif name == "eA":
        is_export, dest = True, int(E.kExportAddress)
    elif name == "eM":
        is_export, dest = True, int(E.kExportData0) + idx
    else:
        raise AsmError(f"unknown dest {name!r}", lineno, tok)
    mask_text = m.group("mask")
    if mask_text is None:
        mask = 0b1111
    else:
        if len(mask_text) != 4:
            raise AsmError("dest mask must have 4 positions", lineno, tok)
        mask = 0
        for i, ch in enumerate(mask_text):
            if ch == "_":
                continue
            if ch in "01":
                raise AsmError("constant 0/1 dest masks not supported in v1",
                               lineno, tok)
            if ch != _XYZW[i]:
                raise AsmError(f"non-identity dest component {ch!r} at "
                               f"position {i}", lineno, tok)
            mask |= 1 << i
    return is_export, dest, mask


# ---------------------------------------------------------------------------
# line model
# ---------------------------------------------------------------------------

@dataclass
class CfLine:
    kind: str                  # exec|alloc|jmp|call|ret|loop|endloop|cnop
    opcode: K | None = None
    fields: dict = field(default_factory=dict)
    target_label: str | None = None
    block: list = field(default_factory=list)   # slot instrs for exec
    lineno: int = 0


@dataclass
class SlotInstr:
    kind: str                  # alu|vfetch|tfetch
    fields: dict = field(default_factory=dict)
    serialize: bool = False
    lineno: int = 0
    builder: str = ""


_COMMENT_RE = re.compile(r"/\*.*?\*/")
_ATTR_SPLIT = re.compile(r",\s*(?=[A-Z][A-Za-z]*=)")


def _split_attrs(rest: str):
    """Splits 'operand, operand, Key=v, Key=v' -> (operand tokens, attrs)."""
    parts = _ATTR_SPLIT.split(rest)
    head = parts[0]
    attrs = {}
    for p in parts[1:]:
        k, _, v = p.partition("=")
        attrs[k.strip()] = v.strip()
    toks = [t.strip() for t in head.split(",") if t.strip()]
    return toks, attrs


def _parse_pred_prefix(line: str):
    s = line.strip()
    if s.startswith("(p0)"):
        return s[4:].strip(), True, True
    if s.startswith("(!p0)"):
        return s[5:].strip(), True, False
    return s, False, False


# ---------------------------------------------------------------------------
# the assembler
# ---------------------------------------------------------------------------

class TextAssembler:
    def __init__(self, shader_type: str):
        assert shader_type in ("vertex", "pixel")
        self.shader_type = shader_type
        self.cf: list[CfLine] = []
        self.labels: dict[str, int] = {}     # label name -> cf entry index
        self._pending_labels: list[str] = []
        self._pending_serialize = False
        self.directives: dict = {"interpolators": [], "elements": [],
                                 "samplers": [], "constants": []}

    # ---- parsing ----

    def parse(self, text: str):
        for lineno, raw in enumerate(text.splitlines(), 1):
            line = _COMMENT_RE.sub("", raw)
            pc = "// PredicateClean=false" in line
            line = line.split("//")[0] if not pc else \
                line.replace("// PredicateClean=false", "")
            if not line.strip():
                continue
            self._parse_line(line, lineno, predicate_clean=not pc)
        if self._pending_labels:
            # labels at end attach to a virtual entry == len(cf)
            for name in self._pending_labels:
                self.labels[name] = len(self.cf)
            self._pending_labels = []

    def _parse_line(self, line: str, lineno: int, predicate_clean: bool):
        s = line.strip()
        if s.startswith("."):
            self._parse_directive(s, lineno)
            return
        if s == "serialize":
            self._pending_serialize = True
            return
        if s.startswith("label "):
            self._pending_labels.append(s.split()[1])
            return
        if s.startswith("+"):
            self._parse_scalar_coissue(s[1:].strip(), lineno)
            return

        body, is_pred, pred_cond = _parse_pred_prefix(s)
        word = body.split()[0].rstrip()
        rest = body[len(word):].strip()

        if word in ("exec", "exece", "cexec", "cexece"):
            self._cf_exec(word, rest, lineno, predicate_clean,
                          is_pred, pred_cond)
        elif word == "alloc":
            self._cf_alloc(rest, lineno)
        elif word in ("jmp", "cjmp", "call", "ccall", "ret",
                      "loop", "endloop", "cnop", "nop"):
            if word == "nop":
                self._slot_alu_nop(is_pred, pred_cond, lineno)
            else:
                self._cf_flow(word, rest, lineno, is_pred, pred_cond)
        elif word in ("vfetch_full", "vfetch_mini"):
            self._slot_vfetch(word, rest, lineno, is_pred, pred_cond)
        elif word.split("_")[0] in _TFETCH_NAMES or word in _TFETCH_NAMES:
            self._slot_tfetch(word, rest, lineno, is_pred, pred_cond)
        else:
            self._slot_alu(body, lineno, is_pred, pred_cond)

    def _parse_directive(self, s: str, lineno: int):
        toks = s.split()
        d = toks[0]
        if d in (".pixel", ".vertex"):
            want = d[1:]
            if want != self.shader_type:
                raise AsmError(f"directive {d} conflicts with shader type "
                               f"{self.shader_type}", lineno, s)
        elif d == ".interpolator":
            self.directives["interpolators"].append(
                (int(toks[1]), toks[2],
                 int(toks[3]) if len(toks) > 3 else 0))
        elif d == ".element":
            self.directives["elements"].append(
                (toks[1], int(toks[2]) if len(toks) > 2 else 0))
        elif d == ".sampler":
            self.directives["samplers"].append((toks[1], int(toks[2])))
        elif d == ".constant":
            self.directives["constants"].append(
                ("vec", toks[1], int(toks[2].lstrip("c")),
                 int(toks[3]) if len(toks) > 3 else 1))
        elif d == ".matrix":
            self.directives["constants"].append(
                ("mat", toks[1], int(toks[2].lstrip("c")), 4))
        else:
            raise AsmError(f"unknown directive {d}", lineno, s)

    # ---- CF lines ----

    def _new_cf(self, cf: CfLine):
        for name in self._pending_labels:
            self.labels[name] = len(self.cf)
        self._pending_labels = []
        self.cf.append(cf)

    def _cf_exec(self, word, rest, lineno, predicate_clean, is_pred,
                 pred_cond):
        is_end = word.endswith("e") and word != "exec" and \
            word in ("exece", "cexece")
        fields = {"is_yield": "Yield=true" in rest,
                  "is_predicate_clean": predicate_clean}
        if word.startswith("cexec"):
            m = re.match(r"(!?)b(\d+)", rest.split(",")[0].strip())
            if not m:
                raise AsmError("cexec needs a bool condition", lineno, rest)
            fields["bool_address"] = int(m.group(2))
            fields["condition"] = m.group(1) != "!"
            # A cond exec has no predicate-clean bit: the opcode carries it,
            # kCondExec(End) unclean, kCondExecPredClean(End) clean, as
            # ParseControlFlowCondExec decodes (xdis/xenos_ucode.py).
            if fields.pop("is_predicate_clean"):
                opcode = (K.kCondExecPredCleanEnd if is_end
                          else K.kCondExecPredClean)
            else:
                opcode = K.kCondExecEnd if is_end else K.kCondExec
            kind = "cexec"
        elif is_pred:
            fields["condition"] = pred_cond
            opcode = (K.kCondExecPredEnd if word == "exece"
                      else K.kCondExecPred)
            kind = "pexec"
        else:
            opcode = K.kExecEnd if word == "exece" else K.kExec
            kind = "exec"
        self._new_cf(CfLine(kind, opcode, fields, lineno=lineno))

    def _cf_alloc(self, rest, lineno):
        rest = rest.strip()
        if rest == "position":
            t, size = 1, 0
        elif rest in ("interpolators", "colors"):
            t, size = 2, 0
        elif rest.startswith("export"):
            t, size = 3, int(rest.split("=")[1])
        else:
            raise AsmError(f"unknown alloc type {rest!r}", lineno, rest)
        self._new_cf(CfLine("alloc", K.kAlloc,
                            {"size": size, "alloc_type": t}, lineno=lineno))

    def _cf_flow(self, word, rest, lineno, is_pred, pred_cond):
        if word == "cnop":
            self._new_cf(CfLine("cnop", lineno=lineno))
            return
        if word == "ret":
            self._new_cf(CfLine("ret", K.kReturn, {}, lineno=lineno))
            return
        toks = [t.strip() for t in rest.split(",")]
        if word in ("jmp", "cjmp", "call", "ccall"):
            fields = {"is_unconditional": False, "is_predicated": False,
                      "bool_address": 0, "condition": False}
            if word in ("cjmp", "ccall"):
                m = re.match(r"(!?)b(\d+)", toks[0])
                fields["bool_address"] = int(m.group(2))
                fields["condition"] = m.group(1) != "!"
                target = toks[1]
            else:
                if is_pred:
                    fields["is_predicated"] = True
                    fields["condition"] = pred_cond
                else:
                    fields["is_unconditional"] = True
                target = toks[0]
            opcode = K.kCondJmp if word in ("jmp", "cjmp") else K.kCondCall
            self._new_cf(CfLine("jump", opcode, fields,
                                target_label=target, lineno=lineno))
        elif word == "loop":
            m = re.match(r"i(\d+)", toks[0])
            fields = {"loop_id": int(m.group(1)),
                      "is_repeat": any("Repeat=true" in t for t in toks)}
            self._new_cf(CfLine("loop", K.kLoopStart, fields,
                                target_label=toks[1], lineno=lineno))
        elif word == "endloop":
            m = re.match(r"i(\d+)", toks[0])
            fields = {"loop_id": int(m.group(1)),
                      "is_predicated_break": is_pred,
                      "condition": pred_cond}
            self._new_cf(CfLine("endloop", K.kLoopEnd, fields,
                                target_label=toks[1], lineno=lineno))

    # ---- slot instructions ----

    def _current_block(self, lineno):
        for cf in reversed(self.cf):
            if cf.kind in ("exec", "cexec", "pexec"):
                return cf.block
        raise AsmError("instruction outside any exec block", lineno, "")

    def _take_serialize(self):
        s = self._pending_serialize
        self._pending_serialize = False
        return s

    def _slot_alu(self, body, lineno, is_pred, pred_cond):
        toks, attrs = _split_attrs(body)
        if attrs:
            raise AsmError("unexpected attributes on ALU line", lineno, body)
        head = toks[0].split(None, 1)
        opname = head[0]
        sat = opname.endswith("_sat")
        if sat:
            opname = opname[:-4]
        if opname not in _VEC_OPS:
            raise AsmError(f"unknown vector opcode {opname!r}", lineno, body)
        opcode, operand_count = _VEC_OPS[opname]
        operands = [head[1].strip()] + [t for t in toks[1:]]
        dest_tok, src_toks = operands[0], operands[1:]
        if len(src_toks) != operand_count:
            raise AsmError(f"{opname} expects {operand_count} operands",
                           lineno, body)
        is_export, dest, mask = parse_alu_dest(dest_tok, self.shader_type,
                                               lineno)
        fields = dict(ALU_HIDDEN)
        fields.update(vector_opcode=opcode, vector_dest=dest,
                      is_vector_dest_relative=False,
                      vector_write_mask=mask, vector_clamp=sat,
                      is_export=is_export,
                      is_predicated=is_pred, predicate_condition=pred_cond,
                      src1_reg=0, src1_is_temp=True, src1_swizzle=0,
                      src1_negate=False,
                      src2_reg=0, src2_is_temp=True, src2_swizzle=0,
                      src2_negate=False)
        for n, tok in enumerate(src_toks, 1):
            fields.update(parse_alu_source(tok, n, 4, lineno))
        ins = SlotInstr("alu", fields, self._take_serialize(), lineno, "alu")
        self._current_block(lineno).append(ins)

    def _slot_alu_nop(self, is_pred, pred_cond, lineno):
        fields = dict(ALU_HIDDEN)
        fields.update(vector_opcode=xu.AluVectorOpcode.kMax,
                      vector_dest=0, is_vector_dest_relative=False,
                      vector_write_mask=0, vector_clamp=False,
                      is_export=False,
                      is_predicated=is_pred, predicate_condition=pred_cond,
                      src1_reg=0, src1_is_temp=True, src1_swizzle=0,
                      src1_negate=False,
                      src2_reg=0, src2_is_temp=True, src2_swizzle=0,
                      src2_negate=False)
        ins = SlotInstr("alu", fields, self._take_serialize(), lineno, "alu")
        self._current_block(lineno).append(ins)

    def _parse_scalar_coissue(self, body, lineno):
        block = self._current_block(lineno)
        if not block or block[-1].kind != "alu":
            raise AsmError("'+' scalar line without preceding vector op",
                           lineno, body)
        body, is_pred, pred_cond = _parse_pred_prefix(body)
        toks, _ = _split_attrs(body)
        head = toks[0].split(None, 1)
        opname = head[0]
        sat = opname.endswith("_sat")
        if sat:
            opname = opname[:-4]
        if opname not in _SCA_OPS:
            raise AsmError(f"unknown scalar opcode {opname!r}", lineno, body)
        opcode, operand_count, two_comp = _SCA_OPS[opname]
        operands = [head[1].strip()] + [t for t in toks[1:]]
        dest_tok, src_toks = operands[0], operands[1:]
        is_export, dest, mask = parse_alu_dest(dest_tok, self.shader_type,
                                               lineno)
        f = block[-1].fields
        f["scalar_opcode"] = opcode
        f["scalar_write_mask"] = mask
        f["scalar_clamp"] = sat
        if not is_export:
            f["scalar_dest"] = dest
            f["is_scalar_dest_relative"] = False
        if operand_count == 1:
            f.update({(k.replace("src1", "src3")): v for k, v in
                      parse_alu_source(src_toks[0], 1,
                                       2 if two_comp else 1,
                                       lineno).items()})
        elif operand_count == 2:
            # mulsc-family: const + temp operands scattered across fields
            cm = _SRC_RE.match(src_toks[0])
            tm = _SRC_RE.match(src_toks[1])
            if not cm or cm.group("bank") != "c" or \
               not tm or tm.group("bank") != "r":
                raise AsmError("mulsc-family expects cN.x, rN.x operands",
                               lineno, body)
            const_sw = cm.group("swiz") or "w"
            temp_sw = tm.group("swiz") or "x"
            swiz = _swizzle_delta(const_sw, (3,)) | \
                _swizzle_delta(temp_sw, (0,))
            extra = enc.scalar_const_reg_op_kwargs(
                scalar_opcode=opcode,
                src_temp_reg=int(tm.group("idx")),
                src3_swizzle_low2=swiz & 0b11)
            f["src3_reg"] = int(cm.group("idx"))
            f["src3_is_temp"] = False
            f["src3_swizzle"] = swiz
            f["src3_negate"] = bool(cm.group("neg")) or bool(tm.group("neg"))
            f.update(extra)

    def _slot_vfetch(self, word, rest, lineno, is_pred, pred_cond):
        toks, attrs = _split_attrs(rest)
        dest_swizzle, dest, dest_rel = self._parse_fetch_dest(toks[0], lineno)
        fields = {
            "opcode": int(xu.FetchOpcode.kVertexFetch),
            "dest": dest, "dest_swizzle": dest_swizzle,
            "is_dest_relative": dest_rel,
            "is_predicated": is_pred, "predicate_condition": pred_cond,
        }
        if word == "vfetch_full":
            sm = _SRC_RE.match(toks[1])
            if not sm:
                raise AsmError(f"bad vfetch source {toks[1]!r}", lineno, rest)
            vf = int(toks[2].lstrip("vf"))
            fci = 95 - vf
            fields.update(
                src=int(sm.group("idx")),
                src_swizzle=_XYZW.index((sm.group("swiz") or "x")),
                is_src_relative=sm.group("rel") is not None,
                must_be_one=1,
                const_index=fci // 3, const_index_sel=fci % 3,
                prefetch_count=int(attrs.get("PrefetchCount", 1)) - 1,
                data_format=_VF_FMT_BY_NAME.get(
                    attrs.get("DataFormat", ""), 0),
                exp_adjust=int(attrs.get("ExpAdjust", 0)),
                is_signed="Signed" in attrs,
                is_normalized=attrs.get("NumFormat", "") != "integer",
                signed_rf_mode=0,
                is_index_rounded="RoundIndex" in attrs,
                stride=int(attrs.get("Stride", 0)),
                offset=int(attrs.get("Offset", 0)),
            )
            triple = enc.build_vfetch_full(**fields)
        else:
            triple = enc.build_vfetch_mini(
                opcode=fields["opcode"], dest=dest,
                dest_swizzle=dest_swizzle, is_dest_relative=dest_rel,
                is_predicated=is_pred, predicate_condition=pred_cond)
        ins = SlotInstr("vfetch", {}, self._take_serialize(), lineno, "")
        ins.triple = triple
        self._current_block(lineno).append(ins)

    def _parse_fetch_dest(self, tok: str, lineno: int):
        """fetch dest operand -> (12-bit dest swizzle, reg, is_relative)."""
        m = _DEST_RE.match(tok) if "[" not in tok else None
        if not m or m.group("name") != "r":
            raise AsmError(f"bad fetch dest {tok!r}", lineno, tok)
        reg = int(m.group("idx"))
        mask_text = m.group("mask")
        codes = []
        if mask_text is None:
            codes = [0, 1, 2, 3]
        elif mask_text == "_":
            codes = [7, 7, 7, 7]
        else:
            if len(mask_text) != 4:
                raise AsmError("fetch dest swizzle must have 4 positions",
                               lineno, tok)
            for ch in mask_text:
                if ch == "_":
                    codes.append(7)        # kKeep
                elif ch == "1":
                    codes.append(5)        # k1
                elif ch == "0":
                    codes.append(4)        # k0 (prints as '0')
                else:
                    codes.append(_XYZW.index(ch))
        swiz = 0
        for i, c in enumerate(codes):
            swiz |= c << (3 * i)
        return swiz, reg, False

    def _slot_tfetch(self, word, rest, lineno, is_pred, pred_cond):
        if word not in _TFETCH_NAMES:
            raise AsmError(f"unknown fetch opcode {word!r}", lineno, rest)
        opcode, dim, has_dest, has_const = _TFETCH_NAMES[word]
        toks, attrs = _split_attrs(rest)
        i = 0
        if has_dest:
            dest_swizzle, dest, _ = self._parse_fetch_dest(toks[i], lineno)
            i += 1
        else:
            dest_swizzle, dest = 0x6D8, 0  # '._'-style keep-all
        sm = _SRC_RE.match(toks[i])
        if not sm:
            raise AsmError(f"bad tfetch source {toks[i]!r}", lineno, rest)
        src = int(sm.group("idx"))
        src_sw_text = sm.group("swiz") or "xyz"[:1]
        src_swizzle = 0
        for j, ch in enumerate(src_sw_text):
            src_swizzle |= _XYZW.index(ch) << (2 * j)
        i += 1
        tf = int(toks[i].lstrip("tf")) if has_const and i < len(toks) else 0

        F = _FILTER_BY_NAME
        fields = dict(
            opcode=opcode, src=src, src_swizzle=src_swizzle,
            is_src_relative=sm.group("rel") is not None,
            dest=dest, dest_swizzle=dest_swizzle, is_dest_relative=False,
            fetch_constant_index=tf,
            fetch_valid_only=attrs.get("FetchValidOnly", "true") != "false",
            unnormalized_coordinates="UnnormalizedTextureCoords" in attrs,
            mag_filter=F.get(attrs.get("MagFilter", "keep"), 3),
            min_filter=F.get(attrs.get("MinFilter", "keep"), 3),
            mip_filter=F.get(attrs.get("MipFilter", "keep"), 3),
            aniso_filter=_ANISO_BY_NAME.get(
                attrs.get("AnisoFilter", "keep"), 7),
            # never printed by the disassembler (deprecated in hardware);
            # corpus convention is 0 — both XDK vintages agree
            arbitrary_filter=0,
            vol_mag_filter=F.get(attrs.get("VolMagFilter", "keep"), 3),
            vol_min_filter=F.get(attrs.get("VolMinFilter", "keep"), 3),
            use_computed_lod=attrs.get("UseComputedLOD", "true") != "false",
            use_register_lod=attrs.get("UseRegisterLOD", "") == "true",
            use_register_gradients=attrs.get(
                "UseRegisterGradients", "") == "true",
            sample_location=0,
            lod_bias=float(attrs.get("LODBias", 0.0)),
            dimension=dim,
            offset_x=float(attrs.get("OffsetX", 0.0)),
            offset_y=float(attrs.get("OffsetY", 0.0)),
            offset_z=float(attrs.get("OffsetZ", 0.0)),
            is_predicated=is_pred, predicate_condition=pred_cond,
        )
        ins = SlotInstr("tfetch", {}, self._take_serialize(), lineno, "")
        ins.triple = enc.build_tfetch(**fields)
        self._current_block(lineno).append(ins)

    # ---- layout & encode ----

    def assemble(self) -> bytes:
        n_cf = len(self.cf)
        if n_cf % 2:
            self.cf.append(CfLine("cnop"))
            n_cf += 1
        first_slot = n_cf // 2

        slot = first_slot
        for cf in self.cf:
            if cf.block:
                cf.fields["address"] = slot
                cf.fields["count"] = len(cf.block)
                seq = 0
                for k, ins in enumerate(cf.block):
                    if ins.kind in ("vfetch", "tfetch"):
                        seq |= 0b01 << (2 * k)
                    if ins.serialize:
                        seq |= 0b10 << (2 * k)
                cf.fields["sequence"] = seq
                n_fetch = sum(1 for i in cf.block
                              if i.kind in ("vfetch", "tfetch"))
                # XDK convention (corpus-proven): per-fetch-count bit mask,
                # vertex shaders only (vs0: 4 fetches -> 0xF; cube_vs: 3 -> 7)
                cf.fields["vc_hi"] = ((1 << min(n_fetch, 4)) - 1) if (
                    n_fetch and self.shader_type == "vertex") else 0
                cf.fields["vc_lo"] = 0
                slot += len(cf.block)
            elif cf.kind in ("exec", "cexec", "pexec"):
                raise AsmError("empty exec block", cf.lineno, "")

        pairs = []
        for cf in self.cf:
            if cf.kind == "cnop":
                pairs.append((0, 0))
            elif cf.kind == "exec":
                pairs.append(enc.build_cf_exec(
                    opcode=cf.opcode, addressing_mode=0, **cf.fields))
            elif cf.kind == "cexec":
                pairs.append(enc.build_cf_cond_exec(
                    opcode=cf.opcode, addressing_mode=0, **cf.fields))
            elif cf.kind == "pexec":
                pairs.append(enc.build_cf_cond_exec_pred(
                    opcode=cf.opcode, addressing_mode=0,
                    is_predicate_clean=cf.fields.pop("is_predicate_clean"),
                    **cf.fields))
            elif cf.kind == "alloc":
                pairs.append(enc.build_cf_alloc(
                    opcode=K.kAlloc, is_unserialized=False, **cf.fields))
            elif cf.kind == "ret":
                pairs.append(enc.build_cf_return(
                    opcode=K.kReturn, addressing_mode=0))
            elif cf.kind == "jump":
                addr = self._resolve(cf.target_label, cf.lineno)
                builder = (enc.build_cf_cond_jmp
                           if cf.opcode == K.kCondJmp
                           else enc.build_cf_cond_call)
                pairs.append(builder(opcode=cf.opcode, address=addr,
                                     direction=0, addressing_mode=0,
                                     **cf.fields)
                             if cf.opcode == K.kCondJmp else
                             builder(opcode=cf.opcode, address=addr,
                                     addressing_mode=0, **cf.fields))
            elif cf.kind == "loop":
                pairs.append(enc.build_cf_loop_start(
                    opcode=K.kLoopStart, addressing_mode=0,
                    address=self._resolve(cf.target_label, cf.lineno),
                    **cf.fields))
            elif cf.kind == "endloop":
                pairs.append(enc.build_cf_loop_end(
                    opcode=K.kLoopEnd, addressing_mode=0,
                    address=self._resolve(cf.target_label, cf.lineno),
                    **cf.fields))
            else:
                raise AsmError(f"unhandled CF kind {cf.kind}", cf.lineno, "")

        dwords = []
        for i in range(0, len(pairs), 2):
            dwords.extend(enc.pack_cf_pair(pairs[i], pairs[i + 1]))
        for cf in self.cf:
            for ins in cf.block:
                if ins.kind == "alu":
                    dwords.extend(enc.build_alu(**ins.fields))
                else:
                    dwords.extend(ins.triple)
        return struct.pack(f">{len(dwords)}I", *dwords)

    def _resolve(self, label: str, lineno: int) -> int:
        if label not in self.labels:
            raise AsmError(f"undefined label {label!r}", lineno, label)
        return self.labels[label]


# ---------------------------------------------------------------------------
# public API
# ---------------------------------------------------------------------------

def assemble_text(text: str, shader_type: str) -> bytes:
    """Assembles pretty-format disassembly text to raw big-endian ucode."""
    a = TextAssembler(shader_type)
    a.parse(text)
    return a.assemble()


def assemble_file(path: str, shader_type: str, container: bool = False,
                  registers=None):
    """registers, a xasm.program.RegisterBudget (libxenon's by default), is
    the runtime's register split; a container past its kind's share raises
    RegisterBudgetError."""
    text = Path(path).read_text()
    if not container:
        return assemble_text(text, shader_type)
    return assemble_container(text, shader_type, registers)


def assemble_container(text: str, shader_type: str, registers=None) -> bytes:
    """Assembles source-mode text, directives included, to a container;
    registers as assemble_file takes them."""
    a = TextAssembler(shader_type)
    a.parse(text)
    return _emit_container(a, a.assemble(), registers)


def _emit_container(a: TextAssembler, ucode: bytes, registers=None) -> bytes:
    import xdis.container_emit as ce
    from xasm.program import LIBXENON_REGISTERS, check_registers
    usage_by_name = {"position": ce.USAGE_POSITION, "color": ce.USAGE_COLOR,
                     "texcoord": ce.USAGE_TEXCOORD}
    nibble = {"texcoord": 0x3, "color": 0xF}
    interps = []
    for idx, uname, uidx in a.directives["interpolators"]:
        flags = ((len(interps) << 4) if a.shader_type == "vertex" else 0) \
            | nibble.get(uname, 0)
        interps.append(ce.Interpolator(idx, usage_by_name[uname], uidx,
                                       flags=flags))
    constants = []
    for name, slot in a.directives["samplers"]:
        constants.append(ce.Constant(name, ce.RS_SAMPLER, slot, 1,
                                     ce.CLASS_OBJECT, 0xC,
                                     rows=1, columns=1))
    for kind, name, reg, count in a.directives["constants"]:
        if kind == "mat":
            constants.append(ce.Constant(name, ce.RS_FLOAT4, reg, count,
                                         ce.CLASS_MATRIX_COLS, ce.TYPE_FLOAT,
                                         rows=4, columns=4))
        else:
            constants.append(ce.Constant(name, ce.RS_FLOAT4, reg, count,
                                         ce.CLASS_VECTOR, ce.TYPE_FLOAT,
                                         rows=1, columns=4))

    max_reg = _scan_max_reg(a)
    check_registers(a.shader_type, max_reg, registers or LIBXENON_REGISTERS)
    if a.shader_type == "pixel":
        return ce.build_container(
            "pixel", ucode, constants=constants,
            program_control=(max_reg << 8) | (2 << 27),
            context_misc=0x8, field10=0x20000,
            interpolators=interps,
            ps_field18=(len(interps) << 16) | 7, ps_outputs=0x1)
    # vertex
    ve_flags = {ce.USAGE_POSITION: 0x1, ce.USAGE_COLOR: 0x3,
                ce.USAGE_TEXCOORD: 0x0}
    elements = []
    fetch_slots = _vertex_fetch_slots(a)
    for (uname, uidx), slot in zip(a.directives["elements"], fetch_slots):
        u = usage_by_name[uname]
        elements.append(ce.VertexElement(slot, u, uidx,
                                         flags=ve_flags.get(u, 0)))
    export_slots = _post_interpolator_slots(a)
    return ce.build_container(
        "vertex", ucode, constants=constants,
        program_control=(max_reg | (1 << 16) |
                         ((len(interps) - 1) << 20)),
        context_misc=0, interpolators=interps,
        vertex_elements=elements, vs_tail_count=len(interps),
        export_slots=export_slots)


def _scan_max_reg(a: TextAssembler) -> int:
    mx = 0
    slot = (len(a.cf) + 1) // 2
    for cf in a.cf:
        for ins in cf.block:
            if ins.kind == "alu":
                f = ins.fields
                if not f.get("is_export"):
                    mx = max(mx, f["vector_dest"])
                for n in (1, 2, 3):
                    if f.get(f"src{n}_is_temp"):
                        mx = max(mx, f[f"src{n}_reg"])
    return mx


def _vertex_fetch_slots(a: TextAssembler):
    out, slot = [], len(a.cf) // 2 + (len(a.cf) % 2)
    slot = (len(a.cf) + 1) // 2
    for cf in a.cf:
        for ins in cf.block:
            if ins.kind == "vfetch":
                out.append(slot)
            slot += 1
    return out


def _post_interpolator_slots(a: TextAssembler):
    out, slot = [], (len(a.cf) + 1) // 2
    seen_interp_alloc = False
    for cf in a.cf:
        if cf.kind == "alloc" and cf.fields.get("alloc_type") == 2:
            seen_interp_alloc = True
        for ins in cf.block:
            if seen_interp_alloc:
                out.append(slot)
            slot += 1
    return out


def main(argv=None):
    import argparse
    from xasm.program import LIBXENON_REGISTERS, RegisterBudget
    ap = argparse.ArgumentParser(prog="xasm.textasm")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--type", choices=["vertex", "pixel"], required=True)
    ap.add_argument("--container", action="store_true")
    ap.add_argument("--pixel-registers", type=int, default=LIBXENON_REGISTERS.pixel,
                    help="pixel shaders' share of the register split (libxenon's by default)")
    ap.add_argument("--vertex-registers", type=int, default=LIBXENON_REGISTERS.vertex,
                    help="vertex shaders' share of the register split (libxenon's by default)")
    args = ap.parse_args(argv)
    blob = assemble_file(args.input, args.type, container=args.container,
                         registers=RegisterBudget(pixel=args.pixel_registers,
                                                  vertex=args.vertex_registers))
    Path(args.output).write_bytes(blob)
    print(f"wrote {args.output} ({len(blob)} B"
          f"{', container' if args.container else ', raw ucode'})")


if __name__ == "__main__":
    main()
