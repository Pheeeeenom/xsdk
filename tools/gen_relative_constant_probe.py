#!/usr/bin/env python3
"""gen_relative_constant_probe.py <out.h>: the vertex shaders for probe_relative_constants, the console probe of which
shapes of constant reads at a run-time index, c[k+a0], the GPU reads otherwise than Xenia's model (milestone 45, step
4). Each case loads a0 with xost's maxasf from a vertex input, reads c[k+a0] in a shape of its own and exports what it
read as interpolator 0, packed with xost's encoders as tools/gen_shader_cost_probe.py packs its own shaders. The span
cases replay Dusklight's 93d36ce55e3f9bad.vert as xost compiled it before the milestone (34e6f58) and at step 4
(6e13c75), from its maxasf through its c[152+a0] read. The generator stops unless xost's simulator
(xenos-toolchain/xdis/simulate.py) runs each case to a whole constant in every cell; that constant is the one the
probe expects.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import gen_triangle_shaders as tri  # noqa: E402
import gen_xsc_probe as pack  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
from gen_triangle_shaders import ce, enc, xu  # noqa: E402

V = xu.AluVectorOpcode
S = xu.AluScalarOpcode
CONSTANTS = 256
# Each cell's a0, a column of the target each: all alike, then each its own, so the GPU indexes the vertices of one
# draw apart, as Dusklight's skinned meshes have it do.
MODES = (("uniform", (5, 5, 5, 5)), ("varying", (3, 7, 12, 18)))
# The rows 93d36ce55e3f9bad.vert reads at a0 before the interpolators' alloc and after it, and the constant it reads
# just before the second.
GEOMETRY_ROW = 120
NORMAL_ROW = 152
OTHER_ROW = 140
PLAIN = 230
# The constant 93d36's position export reads, whose w, 1, the span cases' export reads instead.
POSITION_SCALE = 229
# c[n]'s z, so a pixel decodes only from one whole constant.
CHECK = 0x5A
POSITION_REGISTER = 0
INDEX_REGISTER = 1
RESULT = 3
SCRATCH = 4
SCALAR_RESULT = 5
REGISTERS = SCALAR_RESULT + 1
# The vertex inputs: a position, and a0 in an index vector's x; each cell's quad has its own position and index.
POSITION_INPUT, INDEX_INPUT = 0, 1
INDEX_VECTOR = (0.5, 0.25, 1.0)
CHECK_POSITION = (-0.75, 0.5, 0.5, 1.0)
EXEC_MOST = 6
ALLOC_POSITION = "alloc position"
ALLOC_INTERPOLATORS = "alloc interpolators"
# Component-relative swizzles (xenia:src/xenia/gpu/ucode.h:1977-1982): all four w; and src3's as xost writes them for
# a scalar op, a and b both x for maxasf, a x for rcp.
WWWW = 0x1B
SCALAR_XX = 0x6C
SCALAR_X = 0x40

# Slots 25 to 70 of 93d36ce55e3f9bad.vert as xost at 34e6f58 compiles it: maxasf at 25, c[120..122+a0] at 26, 30 and
# 32, the position export at 47, the interpolators' alloc after it, c230 read at 66 and c[152+a0] into r6 at 67.
OLD_SPAN = (
    (0x60000000, 0x0000006C, 0xE2000001), (0xC80F0001, 0xE0000000, 0x22787800), (0xC8070005, 0x00000000, 0xE2000000),
    (0xC8080005, 0x00000000, 0x22E4E400), (0xC80F0006, 0x00000000, 0xEF010500), (0xC80F0001, 0xE0000000, 0x22797900),
    (0xC80F0007, 0x00000000, 0xEF010500), (0xC80F0001, 0xE0000000, 0x227A7A00), (0xC80F0008, 0x00000000, 0xEF010500),
    (0xC8010001, 0x00000000, 0xE2060600), (0xC8020001, 0x00000000, 0xE2070700), (0xC8040001, 0x00000000, 0xE2080800),
    (0xC8070005, 0x00000000, 0xE2010100), (0xC8080005, 0x00000000, 0x22E4E400), (0xC80F0001, 0x00000000, 0x6F060500),
    (0xC80F0006, 0x00000000, 0x6F070500), (0xC80F0007, 0x00000000, 0x6F080500), (0xC80F0008, 0x00000000, 0x6F090500),
    (0xC8010009, 0x00000000, 0xE2010100), (0xC8020009, 0x00000000, 0xE2060600), (0xC8040009, 0x00000000, 0xE2070700),
    (0xC8080009, 0x00000000, 0xE2080800), (0xC80F803E, 0x00000000, 0xA109E500), (0xC80F0001, 0x02000000, 0x60BD0500),
    (0xC80F0006, 0x00000000, 0xF0010100), (0xA0F00700, 0x00000000, 0xE2000006), (0x4CF00800, 0x00000040, 0xE2000007),
    (0xC80F000A, 0x00000000, 0xE1010800), (0xC80F0001, 0x00000000, 0xB00ABE00), (0xC90F0008, 0x00000000, 0xE2010100),
    (0xC80F0001, 0x00000000, 0xE1080800), (0xC801000B, 0x00000000, 0x22E4E400), (0xC802000B, 0x00000000, 0xE2080800),
    (0xC804000B, 0x00000000, 0xE2010100), (0xC80F0001, 0x00000000, 0x70C00B00), (0xC8010008, 0x00000000, 0x22E4E400),
    (0xC8020008, 0x00000000, 0xE2070700), (0xC8040008, 0x00000000, 0xE2060600), (0xC80F0006, 0x00000000, 0x70C10800),
    (0x4CF00700, 0x00000040, 0xE2000006), (0xC80F0006, 0x00000000, 0xE1010700), (0xC80F0001, 0x00000000, 0x62E60600),
    (0xC80F0006, 0xE0000000, 0x22989800), (0xC8070007, 0x00000000, 0xE2020200), (0xC8080007, 0x00000000, 0x22E6E600),
    (0xC80F0002, 0x00000000, 0xEF060700),
)
# Slots 24 to 63 as xost at 6e13c75 compiles it: the same to the position export at 46, then c[152+a0] into r8
# co-issued with rcp r7, r6.x at 61, a c193 read before it at 60.
STEP4_SPAN = (
    (0x60000000, 0x0000006C, 0xE2000001), (0xC80F0001, 0xE0000000, 0x22787800), (0xC8070005, 0x00000000, 0xE2000000),
    (0xC8080005, 0x00000000, 0x22E4E400), (0xC80F0006, 0x00000000, 0xEF010500), (0xC80F0001, 0xE0000000, 0x22797900),
    (0xC80F0007, 0x00000000, 0xEF010500), (0xC80F0001, 0xE0000000, 0x227A7A00), (0xC80F0008, 0x00000000, 0xEF010500),
    (0xC8010001, 0x00000000, 0xE2060600), (0xC8020001, 0x00000000, 0xE2070700), (0xC8040001, 0x00000000, 0xE2080800),
    (0xC8070005, 0x00000000, 0xE2010100), (0xC8080005, 0x00000000, 0x22E4E400), (0xC80F0001, 0x00000000, 0x6F060500),
    (0xC80F0006, 0x00000000, 0x6F070500), (0xC80F0007, 0x00000000, 0x6F080500), (0xC80F0008, 0x00000000, 0x6F090500),
    (0xC8010009, 0x00000000, 0xE2010100), (0xC8020009, 0x00000000, 0xE2060600), (0xC8040009, 0x00000000, 0xE2070700),
    (0xC8080009, 0x00000000, 0xE2080800), (0xC80F803E, 0x00000000, 0xA109E500), (0xC80F0001, 0x02000000, 0x60BD0500),
    (0xC80F0006, 0x00000000, 0xF0010100), (0xA0F10708, 0x00000000, 0x22E4E406), (0x4CF10A0B, 0x00000040, 0x22E4E407),
    (0xC80F000C, 0x00000000, 0xE1010A00), (0xC80F0001, 0x00000000, 0xB00CBE00), (0xC90F000A, 0x00000000, 0xE2010100),
    (0xC80F0001, 0x00000000, 0xE10A0A00), (0xC802000B, 0x00000000, 0xE20A0A00), (0xC804000B, 0x00000000, 0xE2010100),
    (0xC80F0001, 0x00000000, 0x70C00B00), (0xC8020008, 0x00000000, 0xE2070700), (0xC8040008, 0x00000000, 0xE2060600),
    (0xC80F0006, 0x00000000, 0x70C10800), (0x4CFF0708, 0xE0000040, 0x22989806), (0xC80F0006, 0x00000000, 0xE1010700),
    (0xC80F0001, 0x00000000, 0x62E60600),
)
# Each span's execs, before and after the interpolators' alloc, the last ending in the export of what was read, which
# takes the place of the old program's slot 71 and step 4's slot 64; the slot of its position export and of its
# c[152+a0] read in the span, the register that read writes, and its registers less one, as xost gave them.
SPANS = {
    "old": (OLD_SPAN, (6, 6, 6, 5), (6, 6, 6, 6), 22, 42, 6, 12),
    "step 4": (STEP4_SPAN, (6, 6, 6, 5), (6, 6, 6), 22, 37, 8, 13),
}
# 93d36's own fetches: r1 to r4, then r0 over the index the others read.
SPAN_FETCHES = ((1, INDEX_INPUT), (2, INDEX_INPUT), (3, INDEX_INPUT), (4, INDEX_INPUT), (0, POSITION_INPUT))
FETCHES = ((INDEX_REGISTER, INDEX_INPUT), (POSITION_REGISTER, POSITION_INPUT))


def fill(n):
    """c[n]: n, 255 - n and n ^ CHECK, each a quarter into its 8-bit step, so a target that rounds or truncates keeps
    them; and w 1."""
    return [simulate.single((v + 0.25) / 255) for v in (n, 255 - n, n ^ CHECK)] + [1.0]


def maxasf():
    """xost's a0 load, maxasf from the index's x (src/xsc_lower_block.c, set_a0)."""
    return enc.build_alu(scalar_opcode=S.kMaxAsf, src3_reg=INDEX_REGISTER, src3_swizzle=SCALAR_XX)


def read(row, dest=RESULT, **fields):
    """xost's c[row+a0] read, max of it with itself, src3 the template's r0 (src/xsc_lower_block.c,
    xsdk_xsc_block_register)."""
    return enc.build_alu(**{"vector_dest": dest, "vector_write_mask": 0xF, "is_const_0_addressed": True,
                            "is_const_1_addressed": True, "is_const_address_register_relative": True,
                            "src1_reg": row, "src1_is_temp": False, "src2_reg": row, "src2_is_temp": False, **fields})


def coissued(row, src3_reg=INDEX_REGISTER):
    """The read with rcp of a temp's x beside it, as step 4 co-issues it."""
    return read(row, scalar_opcode=S.kRcp, scalar_dest=SCALAR_RESULT, scalar_write_mask=0xF, src3_reg=src3_reg,
                src3_swizzle=SCALAR_X)


def times_one(first, second):
    """mul c[NORMAL_ROW+a0] by the w, 1, of a plain constant or of the position; each operand (reg, is_temp,
    swizzle, at a0)."""
    fields = {"vector_opcode": V.kMul, "vector_dest": RESULT, "vector_write_mask": 0xF,
              "is_const_address_register_relative": True}
    for n, (reg, is_temp, swizzle, at_a0) in ((1, first), (2, second)):
        fields.update({f"src{n}_reg": reg, f"src{n}_is_temp": is_temp, f"src{n}_swizzle": swizzle,
                       f"src{n}_const_rel": at_a0})
    # srcN_const_rel asks for aL, but sets the bits a0 reads through too (xenia:src/xenia/gpu/ucode.h:1945-1964).
    return enc.build_alu(**fields)


def filler():
    return enc.build_alu(vector_dest=SCRATCH, vector_write_mask=0xF)


def plain(twice=False):
    """max r4, c230, r0, as the old program's slot 66 reads c230 before c[152+a0]; or c230 in both operands."""
    return enc.build_alu(vector_dest=SCRATCH, vector_write_mask=0xF, src1_reg=PLAIN, src1_is_temp=False,
                         src2_reg=PLAIN if twice else POSITION_REGISTER, src2_is_temp=not twice)


POSITION_EXPORT = tri.export(tri.EXPORT_POSITION, POSITION_REGISTER)
RESULT_EXPORT = tri.export(tri.EXPORT_INTERPOLATOR_0, RESULT)
CONSTANT = (PLAIN, False, WWWW, False)
TEMP = (POSITION_REGISTER, True, WWWW, False)
AT_A0 = (NORMAL_ROW, False, 0, True)


def before_alloc(*execs):
    """The read before the interpolators' alloc, as 93d36 reads c[120..122+a0]: execs from maxasf on after the
    position's alloc, then the position export."""
    return [ALLOC_POSITION, *execs, [POSITION_EXPORT], ALLOC_INTERPOLATORS, [RESULT_EXPORT]]


def after_alloc(*execs):
    """The read after the interpolators' alloc, as 93d36 reads c[152+a0]: maxasf and the position export after the
    position's alloc."""
    return [ALLOC_POSITION, [maxasf(), POSITION_EXPORT], ALLOC_INTERPOLATORS, *execs, [RESULT_EXPORT]]


def span(name, registers=None, fillers=False):
    """A span as xost compiled it, its position export reading the probe's position, the exec of its c[152+a0] read
    ending in the export of what that read; with fillers, every instruction but maxasf, the reads at a0 and the
    exports a temp-only max."""
    words, before, after, position_at, read_at, dest, xost_registers = SPANS[name]
    words = list(words)
    words[position_at] = enc.build_alu(vector_opcode=V.kMul, vector_dest=tri.EXPORT_POSITION,
                                       vector_write_mask=0xF, is_export=True, src1_reg=POSITION_REGISTER,
                                       src2_reg=POSITION_SCALE, src2_is_temp=False, src2_swizzle=WWWW)
    if fillers:
        kept = {0, position_at, read_at} | {i for i, w in enumerate(words) if w[1] >> 29 & 1}
        words = [w if i in kept else filler() for i, w in enumerate(words)]
    words.append(tri.export(tri.EXPORT_INTERPOLATOR_0, dest))
    blocks, at = [ALLOC_POSITION], 0
    for size in before:
        blocks.append(words[at:at + size])
        at += size
    blocks.append(ALLOC_INTERPOLATORS)
    for size in after:
        blocks.append(words[at:at + size])
        at += size
    assert at == len(words)
    return blocks, SPAN_FETCHES, (registers or xost_registers + 1)


def cases():
    """Each case's name, blocks, fetches and registers, in the order they run."""
    g, n = GEOMETRY_ROW, NORMAL_ROW
    f = filler
    shapes = [
        ("1 right after maxasf", before_alloc([maxasf(), read(g)])),
        ("2 one after maxasf", before_alloc([maxasf(), f(), read(g)])),
        ("2 two after maxasf", before_alloc([maxasf(), f(), f(), read(g)])),
        ("2 four after maxasf", before_alloc([maxasf(), f(), f(), f(), f(), read(g)])),
        # An exec holds 6 (xenia:src/xenia/gpu/ucode.h:239-259), so 8 on is an exec on.
        ("2 eight after maxasf, an exec on", before_alloc([maxasf()] + [f()] * 5, [f()] * 3 + [read(g)])),
        ("3 an exec on", before_alloc([maxasf()], [read(g)])),
        ("3 three execs on", before_alloc([maxasf()], [f()], [f()], [read(g)])),
        ("4 after alloc interpolators", after_alloc([read(n)])),
        ("4 maxasf before alloc position",
         [[maxasf()], ALLOC_POSITION, [POSITION_EXPORT], ALLOC_INTERPOLATORS, [read(n)], [RESULT_EXPORT]]),
        ("5 co-issued with rcp of a temp", after_alloc([coissued(n)])),
        ("6 src3 temp r1", after_alloc([read(n, src3_reg=INDEX_REGISTER)])),
        # A third constant shares the second's relative bit (xenia:src/xenia/gpu/ucode.h:1944-1950).
        ("6 src3 c0", after_alloc([read(n, src3_is_temp=False)])),
        ("6 src3 c152", after_alloc([read(n, src3_is_temp=False, src3_reg=n)])),
        ("7 c[152+a0] * c230.w", after_alloc([times_one(AT_A0, CONSTANT)])),
        ("7 c230.w * c[152+a0]", after_alloc([times_one(CONSTANT, AT_A0)])),
        ("7 c[152+a0] * r0.w", after_alloc([times_one(AT_A0, TEMP)])),
        ("7 r0.w * c[152+a0]", after_alloc([times_one(TEMP, AT_A0)])),
    ]
    later = [
        ("9 right after a c230 read", after_alloc([plain(), read(n)])),
        ("9 right after a c230 read, co-issued", after_alloc([plain(), coissued(n)])),
        # As step 4's rcp reads the r6 the instruction before it writes.
        ("9 right after a c230 read, co-issued with rcp of what that wrote",
         after_alloc([plain(), coissued(n, src3_reg=SCRATCH)])),
        ("9 right after a c230 read, src3 what that wrote", after_alloc([plain(), read(n, src3_reg=SCRATCH)])),
        ("9 right after a c230 read, before alloc interpolators", before_alloc([maxasf(), plain(), read(g)])),
        ("9 one after a c230 read", after_alloc([plain(), f(), read(n)])),
        ("9 right after a read of c230 twice", after_alloc([plain(twice=True), read(n)])),
        ("10 right after a c[140+a0] read", after_alloc([read(OTHER_ROW, dest=SCRATCH), read(n)])),
        ("11 maxasf again after alloc interpolators", after_alloc([maxasf(), read(n)])),
        ("11 maxasf again after alloc interpolators, one between", after_alloc([maxasf(), f(), read(n)])),
        ("12 the read twice after alloc interpolators", after_alloc([read(n), read(n)])),
        # An alloc between the a0 load and the read, no export.
        ("13 maxasf before alloc position, read after it",
         [[maxasf()], ALLOC_POSITION, [read(g)], [POSITION_EXPORT], ALLOC_INTERPOLATORS, [RESULT_EXPORT]]),
        # The position export between the a0 load and the read, no alloc.
        ("14 read after the position export, before alloc interpolators",
         [ALLOC_POSITION, [maxasf(), POSITION_EXPORT], [read(g)], ALLOC_INTERPOLATORS, [RESULT_EXPORT]]),
        # Throwaway reads that write nothing, before the one that counts.
        ("15 after a throwaway c[152+a0] read, mask 0", after_alloc([read(n, vector_write_mask=0), read(n)])),
        ("15 after a throwaway c[140+a0] read, mask 0", after_alloc([read(OTHER_ROW, vector_write_mask=0), read(n)])),
        ("15 after a throwaway c[0+a0] read, mask 0", after_alloc([read(0, vector_write_mask=0), read(n)])),
    ]
    spans = [
        ("8 old span", *span("old")),
        ("8 old span, 14 registers", *span("old", registers=14)),
        ("8 step 4 span", *span("step 4")),
        ("8 step 4 span, 13 registers", *span("step 4", registers=13)),
        ("8 old span, light code as fillers", *span("old", fillers=True)),
        ("8 step 4 span, light code as fillers", *span("step 4", fillers=True)),
    ]
    return own(shapes) + spans + own(later)


def own(named):
    """Named cases with the probe's own fetches and registers."""
    return [(name, blocks, FETCHES, REGISTERS) for name, blocks in named]


def vertex_shader(blocks, fetches, registers):
    """The fetch exec, then blocks: allocs and execs, the last exec ending the program."""
    control_flow_count = 1 + len(blocks)
    address = (control_flow_count + 1) // 2
    fetch_bits = (1 << len(fetches)) - 1
    # Two sequence bits an instruction, the low one a fetch; a vertex cache bit a fetch (ucode.h:239-259).
    control_flow = [enc.build_cf_exec(xu.ControlFlowOpcode.kExec, address=address, count=len(fetches),
                                      sequence=sum(1 << 2 * i for i in range(len(fetches))), vc_hi=fetch_bits & 0xF,
                                      vc_lo=fetch_bits >> 4, is_predicate_clean=True)]
    elements = [ce.VertexElement(address + i, tri.USAGE_POSITION if source == POSITION_INPUT else ce.USAGE_TEXCOORD, 0)
                for i, (_, source) in enumerate(fetches)]
    instructions = [tri.fetch(dest) for dest, _ in fetches]
    address += len(fetches)
    for i, block in enumerate(blocks):
        if block == ALLOC_POSITION:
            control_flow.append(enc.build_cf_alloc(alloc_type=tri.ALLOC_POSITION))
        elif block == ALLOC_INTERPOLATORS:
            control_flow.append(enc.build_cf_alloc(alloc_type=tri.ALLOC_INTERPOLATORS))
        else:
            assert 1 <= len(block) <= EXEC_MOST
            control_flow.append(tri.execute(address, len(block), end=i == len(blocks) - 1))
            instructions += block
            address += len(block)
    dwords = tri.pack(control_flow, instructions)
    return ce.build_container(
        "vertex", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(vs_num_reg=registers - 1, vs_resource=1, vs_export_count=0),
        context_misc=0, interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        vertex_elements=elements, creator=tri.CREATOR)


def register_read(name, blob, fetches, index):
    """The constant Xenia's model has the case export for a0 index; stops unless it exports the position and one whole
    constant."""
    inputs = {POSITION_INPUT: list(CHECK_POSITION), INDEX_INPUT: [float(index), *INDEX_VECTOR]}
    constants = {r: fill(r) for r in range(CONSTANTS)}
    result = simulate.run(blob, {0: [0.0] * 4}, constants,
                          vertex_fetch=lambda n, fetch, vertex: inputs[fetches[n][1]])
    if result.exports.get(tri.EXPORT_POSITION) != list(CHECK_POSITION):
        sys.exit(f"gen_relative_constant_probe: {name}: the position export is "
                 f"{result.exports.get(tri.EXPORT_POSITION)}")
    value = result.exports.get(tri.EXPORT_INTERPOLATOR_0, [0.0] * 4)
    reg = round(value[0] * 255 - 0.25)
    if not 0 <= reg < CONSTANTS or value != fill(reg):
        sys.exit(f"gen_relative_constant_probe: {name}: a0 {index} exports {value}, no whole constant")
    return reg


def check_encodings():
    """Stops unless the builders write xost's words: maxasf, the old read and step 4's co-issued one."""
    old, step4 = SPANS["old"], SPANS["step 4"]
    if maxasf() != OLD_SPAN[0] or read(NORMAL_ROW, dest=old[5]) != OLD_SPAN[old[4]] or \
            read(NORMAL_ROW, dest=step4[5], scalar_opcode=S.kRcp, scalar_dest=7, scalar_write_mask=0xF, src3_reg=6,
                 src3_swizzle=SCALAR_X) != STEP4_SPAN[step4[4]]:
        sys.exit("gen_relative_constant_probe: the builders don't write xost's words")


def words_of(blob):
    return list(struct.unpack(f">{len(blob) // 4}I", blob))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    check_encodings()
    lines = ["// Generated by tools/gen_relative_constant_probe.py; don't edit.", "",
             "static const char *const probe_mode_names[] = { " + ", ".join(f'"{m}"' for m, _ in MODES) + " };",
             "static const float probe_cell_index[][CELLS] = {"]
    lines += ["\t{ " + ", ".join(f"{i}.0f" for i in cells) + " }," for _, cells in MODES] + ["};"]
    lines += ["static const float probe_index_rest[] = { " + ", ".join(f"{v}f" for v in INDEX_VECTOR) + " };", ""]
    table = []
    for i, (name, blocks, fetches, registers) in enumerate(cases()):
        blob = vertex_shader(blocks, fetches, registers)
        expected = [[register_read(name, blob, fetches, index) for index in cells] for _, cells in MODES]
        lines += [f"static const uint32_t probe_case_{i}[] = {{", pack.word_lines(words_of(blob)), "};", ""]
        rows = ", ".join("{ " + ", ".join(str(r) for r in row) + " }" for row in expected)
        table.append(f"\t{{ \"{name}\", probe_case_{i}, sizeof probe_case_{i}, {{ {rows} }} }},")
    lines += ["static const ProbeCase probe_cases[] = {"] + table + ["};", ""]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
