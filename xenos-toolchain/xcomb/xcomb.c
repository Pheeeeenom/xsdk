/* xcomb.c — N64 combiner -> Xenos pixel-shader container generator.
 *
 * 1:1 C99 port of the pixel paths of:
 *   xasm/combiner.py        (algebra folding, instruction selection)
 *   xasm/program.py         (slot/CF layout, SQ_PROGRAM_CNTL, metadata)
 *   xasm/encode.py          (ALU / tfetch / CF field packing)
 *   xdis/container_emit.py  (container + D3DX constant table emission)
 *
 * Byte-identical to the Python Generator (proven by
 * tests/test_xcomb_parity.py). No host-endianness assumptions: every
 * output word goes through explicit big-endian byte writes.
 */
#include <stdint.h>
#include <string.h>

#include "xcomb.h"
#include "xcomb_templates.h"

/* ---- enum values mirrored from xdis/xenos_ucode.py ---------------------- */
#define VOP_ADD 0   /* AluVectorOpcode.kAdd */
#define VOP_MUL 1   /* AluVectorOpcode.kMul */
#define VOP_MAX 2   /* AluVectorOpcode.kMax */
#define VOP_MAD 11  /* AluVectorOpcode.kMad */

#define CF_EXEC     1   /* ControlFlowOpcode.kExec */
#define CF_EXEC_END 2   /* ControlFlowOpcode.kExecEnd */
#define CF_ALLOC    12  /* ControlFlowOpcode.kAlloc */

#define SCRATCH_REG  3
#define COMBINED_REG 4
#define TEXEL1_REG   5

#define ALPHA_SWIZZLE 0x1B  /* swizzle4("wwww") */
#define MASK_XYZW 0xF
#define MASK_XYZ  0x7       /* mask4("xyz") */
#define MASK_W    0x8       /* mask4("w") */

#define MAX_EXEC_COUNT 6
#define MAX_FETCH 2
#define MAX_ALU   32        /* worst valid case is 16; headroom is cheap */
#define MAX_CF    16

/* ---- big-endian byte writer --------------------------------------------- */

typedef struct {
    unsigned char *buf;
    size_t cap;
    size_t len;
    int err;
} writer_t;

static void w_u8(writer_t *w, unsigned v)
{
    if (w->len + 1 > w->cap) { w->err = 1; return; }
    w->buf[w->len++] = (unsigned char)(v & 0xFFu);
}

static void w_u16be(writer_t *w, unsigned v)
{
    w_u8(w, v >> 8);
    w_u8(w, v);
}

static void w_u32be(writer_t *w, uint32_t v)
{
    w_u8(w, (unsigned)(v >> 24));
    w_u8(w, (unsigned)(v >> 16));
    w_u8(w, (unsigned)(v >> 8));
    w_u8(w, (unsigned)v);
}

static void w_bytes(writer_t *w, const void *p, size_t n)
{
    if (w->len + n > w->cap) { w->err = 1; return; }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

/* NUL-terminated string + 0xAB padding to 4-byte alignment
 * (container_emit._pad4 with PAD=0xAB). */
static void w_str_pad(writer_t *w, const char *s)
{
    w_bytes(w, s, strlen(s) + 1);
    while (w->len % 4 && !w->err)
        w_u8(w, 0xAB);
}

/* ---- instruction packing (xasm/encode.py) ------------------------------- */

typedef struct {
    uint32_t d[3];
    int is_fetch;
} instr_t;

typedef struct {
    uint32_t d0;
    uint32_t d1;    /* fits in 16 bits */
} cf_t;

/* build_cf_exec: kExec/kExecEnd, is_predicate_clean=1, addressing_mode=0 */
static cf_t cf_exec(int opcode, int address, int count, unsigned sequence)
{
    cf_t c;
    c.d0 = (uint32_t)address          /* address [0,12) */
         | ((uint32_t)count << 12)    /* count [12,3) */
         | (sequence << 16);          /* sequence [16,12); vc_hi=0 (pixel) */
    c.d1 = (1u << 9)                  /* is_predicate_clean */
         | ((uint32_t)opcode << 12);
    return c;
}

/* build_cf_alloc: size=0, is_unserialized=0 */
static cf_t cf_alloc(int alloc_type)
{
    cf_t c;
    c.d0 = 0;
    c.d1 = ((uint32_t)alloc_type << 9) | ((uint32_t)CF_ALLOC << 12);
    return c;
}

/* pack_cf_pair: two 48-bit CF entries -> three dwords */
static void pack_cf_pair(cf_t a, cf_t b, uint32_t out[3])
{
    out[0] = a.d0;
    out[1] = a.d1 | ((b.d0 & 0xFFFFu) << 16);
    out[2] = (b.d0 >> 16) | (b.d1 << 16);
}

/* build_tfetch with everything from the corpus template except
 * dest / src / fetch_constant_index. Float-encoded fields come in as the
 * raw integer field values (zero in this template). */
static instr_t build_tfetch(int dest, int src, int fetch_constant_index)
{
    const xcomb_tfetch_template *t = &XCOMB_TFETCH_TEMPLATE;
    instr_t i;
    i.is_fetch = 1;
    i.d[0] = (uint32_t)t->opcode
           | ((uint32_t)src << 5)
           | ((uint32_t)t->is_src_relative << 11)
           | ((uint32_t)dest << 12)
           | ((uint32_t)t->is_dest_relative << 18)
           | ((uint32_t)t->fetch_valid_only << 19)
           | ((uint32_t)fetch_constant_index << 20)
           | ((uint32_t)t->unnormalized_coordinates << 25)
           | ((uint32_t)t->src_swizzle << 26);
    i.d[1] = (uint32_t)t->dest_swizzle
           | ((uint32_t)t->mag_filter << 12)
           | ((uint32_t)t->min_filter << 14)
           | ((uint32_t)t->mip_filter << 16)
           | ((uint32_t)t->aniso_filter << 18)
           | ((uint32_t)t->arbitrary_filter << 21)
           | ((uint32_t)t->vol_mag_filter << 24)
           | ((uint32_t)t->vol_min_filter << 26)
           | ((uint32_t)t->use_computed_lod << 28)
           | ((uint32_t)t->use_register_lod << 29)
           | ((uint32_t)t->is_predicated << 31);
    i.d[2] = (uint32_t)t->use_register_gradients
           | ((uint32_t)t->sample_location << 1)
           | (((uint32_t)t->lod_bias_raw & 0x7Fu) << 2)
           | ((uint32_t)t->dimension << 14)
           | (((uint32_t)t->offset_x_raw & 0x1Fu) << 16)
           | (((uint32_t)t->offset_y_raw & 0x1Fu) << 21)
           | (((uint32_t)t->offset_z_raw & 0x1Fu) << 26)
           | ((uint32_t)t->predicate_condition << 31);
    return i;
}

/* ---- combiner operands (xasm/combiner.py Op) ----------------------------- */

typedef struct {
    int reg;
    int is_temp;
    int negate;
} op_t;

static const op_t SCRATCH_OP = { SCRATCH_REG, 1, 0 };

/* Op.of: _TEMPS = {TEXEL0:2, SHADE:1, COMBINED:4, TEXEL1:5},
 *        _CONSTS = {PRIM:0, ENV:1, ONE:2}.  Never called with ZERO. */
static op_t op_of(xcomb_src s)
{
    op_t o;
    o.negate = 0;
    switch (s) {
    case XCOMB_TEXEL0:   o.reg = 2;            o.is_temp = 1; break;
    case XCOMB_SHADE:    o.reg = 1;            o.is_temp = 1; break;
    case XCOMB_COMBINED: o.reg = COMBINED_REG; o.is_temp = 1; break;
    case XCOMB_TEXEL1:   o.reg = TEXEL1_REG;   o.is_temp = 1; break;
    case XCOMB_PRIM:     o.reg = 0;            o.is_temp = 0; break;
    case XCOMB_ENV:      o.reg = 1;            o.is_temp = 0; break;
    case XCOMB_ONE:      o.reg = 2;            o.is_temp = 0; break;
    default:             o.reg = 0;            o.is_temp = 1; break;
    }
    return o;
}

static op_t op_negated(op_t o)
{
    o.negate = !o.negate;
    return o;
}

/* ---- program under construction (xasm/program.py PixelProgram) ----------- */

typedef struct {
    instr_t fetch[MAX_FETCH];
    int n_fetch;
    instr_t alu[MAX_ALU];
    int n_alu;
    int max_reg;
    int err;
} prog_t;

static void track_reg(prog_t *p, int reg)
{
    if (reg > p->max_reg)
        p->max_reg = reg;
}

static void prog_tfetch(prog_t *p, int dest, int src, int fci)
{
    if (p->n_fetch >= MAX_FETCH) { p->err = 1; return; }
    track_reg(p, dest);     /* _track_regs: "dest" */
    track_reg(p, src);      /* _track_regs: "src" */
    p->fetch[p->n_fetch++] = build_tfetch(dest, src, fci);
}

/* Generator._emit: template + overrides -> build_alu.
 * o1/o2/o3 may be NULL ("unused slot: benign r0 reference"). */
static void prog_alu(prog_t *p, int vop, int dest, int is_export, int wmask,
                     const op_t *o1, const op_t *o2, const op_t *o3,
                     int alpha)
{
    const xcomb_alu_template *t = &XCOMB_ALU_TEMPLATE;
    const op_t *src[3];
    op_t fill = { 0, 1, 0 };
    uint32_t swiz[3], regs[3];
    int is_temp[3], neg[3];
    int n, const_regs[3], n_consts = 0;
    instr_t ins;

    src[0] = o1; src[1] = o2; src[2] = o3;

    /* assert n_consts <= 1 ("split before emitting") */
    for (n = 0; n < 3; n++) {
        if (src[n] && !src[n]->is_temp) {
            int k, seen = 0;
            for (k = 0; k < n_consts; k++)
                if (const_regs[k] == src[n]->reg)
                    seen = 1;
            if (!seen)
                const_regs[n_consts++] = src[n]->reg;
        }
    }
    if (n_consts > 1) { p->err = 1; return; }

    for (n = 0; n < 3; n++) {
        const op_t *o = src[n] ? src[n] : &fill;
        regs[n] = (uint32_t)o->reg;
        is_temp[n] = o->is_temp;
        neg[n] = o->negate;
        swiz[n] = (src[n] && alpha) ? ALPHA_SWIZZLE : 0;
        if (is_temp[n])
            track_reg(p, o->reg);   /* _track_regs: srcN_is_temp */
    }
    if (!is_export)
        track_reg(p, dest);         /* _track_regs: vector_dest */

    if (p->n_alu >= MAX_ALU) { p->err = 1; return; }
    ins.is_fetch = 0;
    ins.d[0] = (uint32_t)dest
             | ((uint32_t)t->is_vector_dest_relative << 6)
             | ((uint32_t)t->abs_constants << 7)
             | ((uint32_t)t->scalar_dest << 8)
             | ((uint32_t)t->is_scalar_dest_relative << 14)
             | ((uint32_t)(is_export ? 1 : 0) << 15)
             | ((uint32_t)wmask << 16)
             | ((uint32_t)t->scalar_write_mask << 20)
             | ((uint32_t)t->vector_clamp << 24)
             | ((uint32_t)t->scalar_clamp << 25)
             | ((uint32_t)t->scalar_opcode << 26);
    ins.d[1] = swiz[2]
             | (swiz[1] << 8)
             | (swiz[0] << 16)
             | ((uint32_t)(neg[2] ? 1 : 0) << 24)
             | ((uint32_t)(neg[1] ? 1 : 0) << 25)
             | ((uint32_t)(neg[0] ? 1 : 0) << 26)
             | ((uint32_t)t->predicate_condition << 27)
             | ((uint32_t)t->is_predicated << 28)
             | ((uint32_t)t->is_const_address_register_relative << 29)
             | ((uint32_t)t->is_const_1_addressed << 30)
             | ((uint32_t)t->is_const_0_addressed << 31);
    ins.d[2] = regs[2]
             | (regs[1] << 8)
             | (regs[0] << 16)
             | ((uint32_t)vop << 24)
             | ((uint32_t)(is_temp[2] ? 1 : 0) << 29)
             | ((uint32_t)(is_temp[1] ? 1 : 0) << 30)
             | ((uint32_t)(is_temp[0] ? 1 : 0) << 31);
    p->alu[p->n_alu++] = ins;
}

static op_t mov_to_scratch(prog_t *p, op_t o, int alpha)
{
    prog_alu(p, VOP_MAX, SCRATCH_REG, 0, MASK_XYZW, &o, &o, NULL, alpha);
    return SCRATCH_OP;
}

/* ---- one (A-B)*C+D pipeline (Generator._pipeline) ------------------------ */

static void pipeline(prog_t *p, const xcomb_src sel[4], int alpha, int wmask,
                     int out_dest, int out_export)
{
    xcomb_src a = sel[0], b = sel[1], c = sel[2], d = sel[3];
    op_t diff = SCRATCH_OP;  /* value unused unless has_diff is set */
    int has_diff;

    /* difference term */
    if (a == b) {
        has_diff = 0;                       /* (A-B) == 0 */
    } else if (b == XCOMB_ZERO) {
        if (a == XCOMB_ZERO) {
            has_diff = 0;
        } else {
            diff = op_of(a);
            has_diff = 1;
        }
    } else if (a == XCOMB_ZERO) {
        diff = op_negated(op_of(b));        /* -(B) */
        has_diff = 1;
    } else {
        op_t oa = op_of(a), ob = op_negated(op_of(b));
        if (!oa.is_temp && !ob.is_temp && oa.reg != ob.reg)
            oa = mov_to_scratch(p, oa, alpha);
        prog_alu(p, VOP_ADD, SCRATCH_REG, 0, MASK_XYZW, &oa, &ob, NULL,
                 alpha);
        diff = SCRATCH_OP;
        has_diff = 1;
    }

    /* product & sum */
    if (!has_diff || c == XCOMB_ZERO) {
        if (d == XCOMB_ZERO) {
            /* true zero with no constant: SHADE - SHADE */
            op_t z = op_of(XCOMB_SHADE);
            op_t nz = op_negated(z);
            prog_alu(p, VOP_ADD, out_dest, out_export, wmask, &z, &nz, NULL,
                     alpha);
        } else {
            op_t od = op_of(d);
            prog_alu(p, VOP_MAX, out_dest, out_export, wmask, &od, &od,
                     NULL, alpha);
        }
        return;
    }

    if (c == XCOMB_ONE) {
        if (d == XCOMB_ZERO) {
            prog_alu(p, VOP_MAX, out_dest, out_export, wmask, &diff, &diff,
                     NULL, alpha);
        } else {
            op_t oc = diff, od = op_of(d);
            if (!oc.is_temp && !od.is_temp && oc.reg != od.reg)
                oc = mov_to_scratch(p, oc, alpha);
            prog_alu(p, VOP_ADD, out_dest, out_export, wmask, &oc, &od,
                     NULL, alpha);
        }
        return;
    }

    {
        op_t o1 = diff, o2 = op_of(c), o3;
        int has3 = (d != XCOMB_ZERO);
        int n_consts = 0, const_regs[3];
        int k, n;
        const op_t *all[3];

        if (has3)
            o3 = op_of(d);
        all[0] = &o1; all[1] = &o2; all[2] = has3 ? &o3 : NULL;
        for (n = 0; n < 3; n++) {
            if (all[n] && !all[n]->is_temp) {
                int seen = 0;
                for (k = 0; k < n_consts; k++)
                    if (const_regs[k] == all[n]->reg)
                        seen = 1;
                if (!seen)
                    const_regs[n_consts++] = all[n]->reg;
            }
        }
        if (n_consts > 1) {
            /* split: scratch = diff * C, then out = scratch + D */
            op_t s1 = o1;
            if (!s1.is_temp && !o2.is_temp)
                s1 = mov_to_scratch(p, s1, alpha);
            prog_alu(p, VOP_MUL, SCRATCH_REG, 0, MASK_XYZW, &s1, &o2, NULL,
                     alpha);
            if (has3) {
                prog_alu(p, VOP_ADD, out_dest, out_export, wmask,
                         &SCRATCH_OP, &o3, NULL, alpha);
            } else {
                prog_alu(p, VOP_MAX, out_dest, out_export, wmask,
                         &SCRATCH_OP, &SCRATCH_OP, NULL, alpha);
            }
        } else {
            prog_alu(p, has3 ? VOP_MAD : VOP_MUL, out_dest, out_export,
                     wmask, &o1, &o2, has3 ? &o3 : NULL, alpha);
        }
    }
}

static void emit_cycle(prog_t *p, const xcomb_cycle *cy, int out_dest,
                       int out_export)
{
    xcomb_src color[4], alpha[4];
    color[0] = cy->a;  color[1] = cy->b;  color[2] = cy->c;  color[3] = cy->d;
    alpha[0] = cy->aa; alpha[1] = cy->ab; alpha[2] = cy->ac; alpha[3] = cy->ad;

    if (color[0] == alpha[0] && color[1] == alpha[1] &&
        color[2] == alpha[2] && color[3] == alpha[3]) {
        pipeline(p, color, 0, MASK_XYZW, out_dest, out_export);
    } else {
        pipeline(p, color, 0, MASK_XYZ, out_dest, out_export);
        pipeline(p, alpha, 1, MASK_W, out_dest, out_export);
    }
}

/* ---- CF layout + ucode serialization (xasm/program.py _build_ucode) ------ */

/* Stage layout for the pixel combiner is fixed: stage0 = tfetches,
 * alloc(colors=2), stage1 = ALU instructions. */
static size_t build_ucode(const prog_t *p, writer_t *w, int *err)
{
    cf_t cf[MAX_CF];
    int n_cf = 0, i, ci;
    int fetch_chunks = p->n_fetch ? 1 : 0;     /* n_fetch <= 2 < 6 */
    int alu_chunks = (p->n_alu + MAX_EXEC_COUNT - 1) / MAX_EXEC_COUNT;
    int first_slot, total_slots;
    size_t start = w->len;

    if (p->n_alu == 0) { *err = 1; return 0; }

    first_slot = (fetch_chunks + 1 + alu_chunks + 1) / 2;

    if (fetch_chunks) {
        /* sequence bit0 marks fetch slots: 0b01 << (2*k) per fetch */
        unsigned seq = 0;
        for (i = 0; i < p->n_fetch; i++)
            seq |= 1u << (2 * i);
        cf[n_cf++] = cf_exec(CF_EXEC, first_slot, p->n_fetch, seq);
    }
    cf[n_cf++] = cf_alloc(2);                  /* alloc_colors */
    for (ci = 0; ci < alu_chunks; ci++) {
        int addr = first_slot + p->n_fetch + ci * MAX_EXEC_COUNT;
        int count = p->n_alu - ci * MAX_EXEC_COUNT;
        int is_last = (ci == alu_chunks - 1);
        if (count > MAX_EXEC_COUNT)
            count = MAX_EXEC_COUNT;
        cf[n_cf++] = cf_exec(is_last ? CF_EXEC_END : CF_EXEC, addr, count,
                             0);
    }
    if (n_cf % 2) {                            /* cnop pad */
        cf[n_cf].d0 = 0;
        cf[n_cf].d1 = 0;
        n_cf++;
    }
    if (n_cf / 2 != first_slot) { *err = 1; return 0; }  /* self-check */

    for (i = 0; i < n_cf; i += 2) {
        uint32_t d[3];
        pack_cf_pair(cf[i], cf[i + 1], d);
        w_u32be(w, d[0]);
        w_u32be(w, d[1]);
        w_u32be(w, d[2]);
    }
    for (i = 0; i < p->n_fetch; i++) {
        w_u32be(w, p->fetch[i].d[0]);
        w_u32be(w, p->fetch[i].d[1]);
        w_u32be(w, p->fetch[i].d[2]);
    }
    for (i = 0; i < p->n_alu; i++) {
        w_u32be(w, p->alu[i].d[0]);
        w_u32be(w, p->alu[i].d[1]);
        w_u32be(w, p->alu[i].d[2]);
    }
    total_slots = first_slot + p->n_fetch + p->n_alu;
    if (!w->err && w->len - start != (size_t)total_slots * 12) {
        *err = 1;
        return 0;
    }
    return w->len - start;
}

/* ---- D3DX constant table (container_emit.build_constant_table) ----------- */

/* D3DXPARAMETER / register-set values from container_emit.py */
#define RS_FLOAT4 2
#define RS_SAMPLER 3
#define CLASS_VECTOR 1
#define CLASS_OBJECT 4
#define TYPE_FLOAT 3

typedef struct {
    const char *name;
    int register_set;
    int register_index;
    int register_count;
    int param_class;
    int param_type;
    int rows;
    int columns;
    int elements;
} const_desc_t;

#define CTAB_HDR 0x1C   /* sizeof ConstantTable */
#define CTAB_CI  0x14   /* sizeof ConstantInfo */

/* writes the u32 size wrapper + table body; returns bytes written */
static void build_constant_table(const const_desc_t *cs, int n_cs,
                                 writer_t *out)
{
    unsigned char body[512];
    writer_t b;
    uint32_t name_off[5], ti_off[5];
    uint32_t target_off, creator_off;
    int i;

    b.buf = body;
    b.cap = sizeof(body);
    b.len = 0;
    b.err = 0;

    /* header + ConstantInfo placeholders */
    for (i = 0; i < CTAB_HDR + CTAB_CI * n_cs; i++)
        w_u8(&b, 0);

    for (i = 0; i < n_cs; i++) {
        name_off[i] = (uint32_t)b.len;
        w_str_pad(&b, cs[i].name);
        ti_off[i] = (uint32_t)b.len;
        /* TypeInfo: >4H2HI = class, type, rows, columns, elements, 0, u32 0 */
        w_u16be(&b, (unsigned)cs[i].param_class);
        w_u16be(&b, (unsigned)cs[i].param_type);
        w_u16be(&b, (unsigned)cs[i].rows);
        w_u16be(&b, (unsigned)cs[i].columns);
        w_u16be(&b, (unsigned)cs[i].elements);
        w_u16be(&b, 0);
        w_u32be(&b, 0);
    }
    target_off = (uint32_t)b.len;
    w_bytes(&b, "ps_3_0", 7);          /* NUL included, no pad yet */
    creator_off = (uint32_t)b.len;
    w_str_pad(&b, "2.0.11626.0");

    if (b.err) { out->err = 1; return; }

    /* patch header (>7I) */
    {
        writer_t h = b;
        h.len = 0;
        w_u32be(&h, CTAB_HDR);
        w_u32be(&h, creator_off);
        w_u32be(&h, 0xFFFF0300u);      /* VERSION_TOKEN pixel */
        w_u32be(&h, (uint32_t)n_cs);
        w_u32be(&h, n_cs ? CTAB_HDR : 0);
        w_u32be(&h, 0x00001000u);
        w_u32be(&h, target_off);
        /* ConstantInfo records (>I4H2I) */
        for (i = 0; i < n_cs; i++) {
            w_u32be(&h, name_off[i]);
            w_u16be(&h, (unsigned)cs[i].register_set);
            w_u16be(&h, (unsigned)cs[i].register_index);
            w_u16be(&h, (unsigned)cs[i].register_count);
            w_u16be(&h, 0);
            w_u32be(&h, ti_off[i]);
            w_u32be(&h, 0);
        }
    }

    w_u32be(out, (uint32_t)b.len);     /* size wrapper */
    w_bytes(out, body, b.len);
}

/* ---- whole shader (Generator.generate + PixelProgram.finish) ------------- */

static int src_valid(xcomb_src s, int allow_combined)
{
    if ((int)s < 0 || (int)s > (int)XCOMB_COMBINED)
        return 0;
    if (s == XCOMB_COMBINED && !allow_combined)
        return 0;
    return 1;
}

size_t xcomb_generate(const xcomb_cycle *cycle0, const xcomb_cycle *cycle1,
                      unsigned char *out, size_t out_cap)
{
    prog_t p;
    writer_t w;
    int used[8];
    int err = 0, i, n_cs = 0;
    const_desc_t cs[5];
    unsigned char ucode_buf[(MAX_CF / 2 + MAX_FETCH + MAX_ALU) * 12];
    writer_t uw;
    size_t ucode_len, ctab_len, shader_off, rec_len, virtual_size;
    uint32_t program_control, interp_info;

    if (!cycle0 || !out)
        return 0;

    /* validate sources; COMBINED is only meaningful in cycle 1 */
    {
        const xcomb_src *c0 = &cycle0->a;
        for (i = 0; i < 8; i++)
            if (!src_valid(c0[i], 0))
                return 0;
        if (cycle1) {
            const xcomb_src *c1 = &cycle1->a;
            for (i = 0; i < 8; i++)
                if (!src_valid(c1[i], 1))
                    return 0;
        }
    }

    memset(used, 0, sizeof(used));
    {
        const xcomb_src *c0 = &cycle0->a;
        for (i = 0; i < 8; i++)
            used[c0[i]] = 1;
        if (cycle1) {
            const xcomb_src *c1 = &cycle1->a;
            for (i = 0; i < 8; i++)
                used[c1[i]] = 1;
        }
    }

    memset(&p, 0, sizeof(p));
    /* p.input(0, TEXCOORD); p.input(1, COLOR) -> max_reg >= 1 */
    p.max_reg = 1;

    if (used[XCOMB_TEXEL0])
        prog_tfetch(&p, 2, 0, 0);
    if (used[XCOMB_TEXEL1])
        prog_tfetch(&p, TEXEL1_REG, 0, 1);

    if (!cycle1) {
        emit_cycle(&p, cycle0, 0, 1);
    } else {
        emit_cycle(&p, cycle0, COMBINED_REG, 0);
        emit_cycle(&p, cycle1, 0, 1);
    }
    if (p.err)
        return 0;

    /* ucode into scratch buffer first (header needs its length) */
    uw.buf = ucode_buf;
    uw.cap = sizeof(ucode_buf);
    uw.len = 0;
    uw.err = 0;
    ucode_len = build_ucode(&p, &uw, &err);
    if (err || uw.err)
        return 0;

    /* constant table contents, fixed order: tex0, tex1, prim, env, one */
    if (used[XCOMB_TEXEL0]) {
        const_desc_t c = { "tex0", RS_SAMPLER, 0, 1, CLASS_OBJECT, 0xC,
                           1, 1, 1 };
        cs[n_cs++] = c;
    }
    if (used[XCOMB_TEXEL1]) {
        const_desc_t c = { "tex1", RS_SAMPLER, 1, 1, CLASS_OBJECT, 0xC,
                           1, 1, 1 };
        cs[n_cs++] = c;
    }
    if (used[XCOMB_PRIM]) {
        const_desc_t c = { "prim", RS_FLOAT4, 0, 1, CLASS_VECTOR,
                           TYPE_FLOAT, 1, 4, 1 };
        cs[n_cs++] = c;
    }
    if (used[XCOMB_ENV]) {
        const_desc_t c = { "env", RS_FLOAT4, 1, 1, CLASS_VECTOR,
                           TYPE_FLOAT, 1, 4, 1 };
        cs[n_cs++] = c;
    }
    if (used[XCOMB_ONE]) {
        const_desc_t c = { "one", RS_FLOAT4, 2, 1, CLASS_VECTOR,
                           TYPE_FLOAT, 1, 4, 1 };
        cs[n_cs++] = c;
    }

    /* ---- container (container_emit.build_container, pixel path) ---- */
    w.buf = out;
    w.cap = out_cap;
    w.len = 0;
    w.err = 0;

    /* measure ctab by emitting after the 9-word header */
    for (i = 0; i < 9; i++)
        w_u32be(&w, 0);                /* placeholder header */
    build_constant_table(cs, n_cs, &w);
    if (w.err)
        return 0;
    ctab_len = w.len - 0x24;
    shader_off = 0x24 + ctab_len;

    program_control = ((uint32_t)p.max_reg << 8) | (2u << 27);
    /* interp_info_extra | (len(interpolators) << 5) | 0x3, 2 interpolators */
    interp_info = 0x2000u | (2u << 5) | 0x3u;
    rec_len = 6 * 4 + 2 * 4 + 2 * 4;   /* 6 words, ps pair, 2 interp words */

    /* shader record */
    w_u32be(&w, 0);
    w_u32be(&w, (uint32_t)ucode_len);
    w_u32be(&w, program_control);
    w_u32be(&w, 0x8);                  /* context_misc */
    w_u32be(&w, 0x20000);              /* field10 */
    w_u32be(&w, interp_info);
    w_u32be(&w, (2u << 16) | 7u);      /* ps_field18 */
    w_u32be(&w, 0x1);                  /* ps_outputs */
    /* interpolators: (flags<<12)|(reg<<8)|(usage<<4)|usage_index */
    w_u32be(&w, (0x3u << 12) | (0u << 8) | (5u << 4));   /* r0 TEXCOORD0 */
    w_u32be(&w, (0xFu << 12) | (1u << 8) | (10u << 4));  /* r1 COLOR0 */

    virtual_size = shader_off + rec_len;
    if (w.err || w.len != virtual_size)
        return 0;

    w_bytes(&w, ucode_buf, ucode_len);
    if (w.err)
        return 0;

    /* patch the real header */
    {
        writer_t h;
        h.buf = out;
        h.cap = out_cap;
        h.len = 0;
        h.err = 0;
        w_u32be(&h, 0x102A1100u);              /* MAGIC pixel */
        w_u32be(&h, (uint32_t)virtual_size);
        w_u32be(&h, (uint32_t)ucode_len);
        w_u32be(&h, 0);
        w_u32be(&h, 0x24);
        w_u32be(&h, 0);
        w_u32be(&h, (uint32_t)shader_off);
        w_u32be(&h, 0);
        w_u32be(&h, 0);
    }
    return w.len;
}
