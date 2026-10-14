#ifndef XSDK_XSC_LOWER_H
#define XSDK_XSC_LOWER_H

#include "xsc_private.h"

// The lowering, shared by xsc_lower*.c: SPIR-V values to Xenos instructions, as xenos-toolchain/xsc/lower.py's
// ValueLowerer emits them.

typedef enum {
	XSC_OPERAND_NONE,
	XSC_OPERAND_REG,
	XSC_OPERAND_CONST,
	XSC_OPERAND_MAT,
} XscOperandKind;

typedef struct XscMatrix XscMatrix;

typedef struct {
	XscOperandKind kind;
	int64_t value;
	const XscMatrix *mat;
} XscOperand;

enum {
	XSC_MATRIX_ROWS = 4,
	// A component standing for every component: lower.py's "B".
	XSC_BROADCAST = -1,
	// Deeper than any shader goes (73 Python frames at most over Dusklight's); Python's own limit is 1000 frames.
	XSC_DEPTH_LIMIT = 512,
};

struct XscMatrix {
	size_t count;
	XscOperand rows[XSC_MATRIX_ROWS];
};

// One of an emitted instruction's sources (lower.py's ops values).
typedef struct {
	XscOperand o;
	bool negate;
	int64_t swizzle;
} XscSrc;

typedef struct {
	int64_t reg;
	double value[4];
} XscLiteral;

typedef struct {
	Xsc *x;
	const XscModule *m;
	const XscInterface *si;
	XscProgram *p;
	const XscType *types;
	bool vertex;
	int64_t next_reg;
	int64_t lit_base;
	XscLiteral *pool;
	double *pool_keys;
	size_t pool_count;
	size_t pool_capacity;
	size_t pool_key_capacity;
	// By SSA or variable id, below m->id_limit.
	XscOperand *op_cache;
	const XscMatrix **matrix_cache;
	XscOperand *local_values;
	bool *col_major;
	int64_t *inputs;
	uint32_t *local_store;
	int64_t *sample_reg;
	int64_t *deriv_reg;
	bool a0_set;
	uint32_t a0_part;
	int64_t a0_scale;
	// The ids a constant fold has visited, as lower.py's seen set: marked by id, listed to clear.
	bool *seen;
	uint32_t *seen_ids;
	size_t seen_count;
	size_t seen_capacity;
	// How deep the lowering has recursed, bounded where Python raises RecursionError.
	uint32_t depth;
} XscLower;

// Enters one level of recursion, false with the error set past the bound.
bool xsdk_xsc_enter(XscLower *l);

static inline bool xsc_failed(const XscLower *l)
{
	return l->x->status != XSDK_XSC_OK;
}

static inline const char *xsc_stage_name(const XscLower *l)
{
	return l->vertex ? "vertex" : "fragment";
}

static inline XscOperand xsc_reg(int64_t reg)
{
	return (XscOperand){ XSC_OPERAND_REG, reg, NULL };
}

static inline XscOperand xsc_const(int64_t reg)
{
	return (XscOperand){ XSC_OPERAND_CONST, reg, NULL };
}

static inline XscOperand xsc_mat(const XscMatrix *mat)
{
	return (XscOperand){ XSC_OPERAND_MAT, 0, mat };
}

static inline XscSrc xsc_src(XscOperand o, bool negate, int64_t swizzle)
{
	return (XscSrc){ o, negate, swizzle };
}

bool xsdk_xsc_emit(
	XscLower *l, int64_t opcode, const XscSrc *ops, size_t count, int64_t dst, bool export_, int64_t mask, bool clamp);

static inline bool xsc_emit2(XscLower *l, int64_t opcode, XscSrc a, XscSrc b, int64_t dst, bool export_, int64_t mask)
{
	const XscSrc ops[2] = { a, b };
	return xsdk_xsc_emit(l, opcode, ops, 2, dst, export_, mask, false);
}

static inline bool xsc_emit3(
	XscLower *l, int64_t opcode, XscSrc a, XscSrc b, XscSrc c, int64_t dst, bool export_, int64_t mask)
{
	const XscSrc ops[3] = { a, b, c };
	return xsdk_xsc_emit(l, opcode, ops, 3, dst, export_, mask, false);
}

// A source reading a register as it is, and negated.
static inline XscSrc xsc_temp(int64_t reg)
{
	return xsc_src(xsc_reg(reg), false, 0);
}

static inline XscSrc xsc_plain(XscOperand o)
{
	return xsc_src(o, false, 0);
}

static inline XscSrc xsc_negated(XscOperand o)
{
	return xsc_src(o, true, 0);
}

// The (operand, component) a scalar resolves to; component XSC_BROADCAST for "B".
typedef struct {
	XscOperand o;
	int64_t comp;
} XscOrigin;

bool xsdk_xsc_lower_init(Xsc *x, XscLower *l, const XscModule *m, const XscInterface *si, XscProgram *p,
	const XscType *types, bool vertex, int64_t first_reg, int64_t literal_base);
bool xsdk_xsc_lower_refuse(XscLower *l, const char *format, ...) __attribute__((format(printf, 2, 3)));
const XscInstr *xsdk_xsc_lower_result(XscLower *l, uint32_t ssa);
int64_t xsdk_xsc_fresh(XscLower *l);
int64_t xsdk_xsc_vec_size(XscLower *l, uint32_t ssa);
const XscType *xsdk_xsc_result_type(XscLower *l, uint32_t ssa);
int64_t xsdk_xsc_swz(const int64_t *idxs, size_t count);
int64_t xsdk_xsc_bsw(int64_t c);
double xsdk_xsc_scalar_const(XscLower *l, uint32_t ssa);
bool xsdk_xsc_float_const(XscLower *l, uint32_t ssa, double *value);
bool xsdk_xsc_float_const01(XscLower *l, uint32_t ssa, double *value);
bool xsdk_xsc_int_const(XscLower *l, uint32_t ssa, int64_t *value);
bool xsdk_xsc_const_scalar(XscLower *l, uint32_t ssa, double *value);
int64_t xsdk_xsc_literal_reg(XscLower *l, const double value[4]);
int64_t xsdk_xsc_literal_splat(XscLower *l, double value);
bool xsdk_xsc_emit_alu(XscLower *l, const XscAlu *f);
int64_t xsdk_xsc_emit_copy(
	XscLower *l, XscOperand opnd, int64_t dst, bool export_, bool negate, int64_t swizzle, int64_t mask, bool clamp);
bool xsdk_xsc_emit_export_constants(XscLower *l, int64_t dst_reg, int64_t zero_mask, int64_t one_mask);
int64_t xsdk_xsc_emit_scalar(XscLower *l, int64_t sop, int64_t dst, XscOperand src, int64_t src_comp, int64_t mask);
XscOperand xsdk_xsc_to_reg(XscLower *l, XscOperand opnd);
XscOperand xsdk_xsc_lower_operand(XscLower *l, uint32_t ssa);
XscOrigin xsdk_xsc_scalar_origin(XscLower *l, uint32_t ssa);
bool xsdk_xsc_emit_value(XscLower *l, uint32_t ssa, int64_t dst, bool export_, bool export_position);
bool xsdk_xsc_emit_construct(XscLower *l, const XscInstr *ins, int64_t dst_reg, bool export_);
int64_t xsdk_xsc_binop(XscLower *l, uint32_t op, uint32_t a, uint32_t b, int64_t dst, bool export_, bool clamp);
int64_t xsdk_xsc_extinst(XscLower *l, const XscInstr *ins, int64_t dst, bool export_);
int64_t xsdk_xsc_dotself(XscLower *l, XscOperand ov, int64_t n);
int64_t xsdk_xsc_recip(XscLower *l, XscOperand opnd, int64_t n);
int64_t xsdk_xsc_emit_clamped(XscLower *l, uint32_t ssa, int64_t dst, bool export_);

// Matrices (xsc_lower_matrix.c).
const XscMatrix *xsdk_xsc_matrix_operands(XscLower *l, uint32_t ssa);
int64_t xsdk_xsc_emit_mat_vec(XscLower *l, uint32_t mat, uint32_t vec, int64_t dst, bool export_position);
int64_t xsdk_xsc_emit_vec_mat(XscLower *l, uint32_t vec, uint32_t mat, int64_t dst, bool export_position);
int64_t xsdk_xsc_emit_determinant(XscLower *l, uint32_t mat);
int64_t xsdk_xsc_cross(XscLower *l, XscOperand a, XscOperand b);

// Block members (xsc_lower_block.c): an access through a uniform or push constant block, found false for others.
typedef struct {
	int64_t reg;
	int64_t comp;
	uint32_t tid;
	// The member's struct and index whose decorations hold, or XSC_NONE.
	int64_t member_struct;
	int64_t member_index;
	bool dynamic;
	uint32_t dyn_part;
	int64_t dyn_scale;
} XscBlockAccess;
bool xsdk_xsc_block_access(XscLower *l, uint32_t ptr_id, XscBlockAccess *access, bool *found);
XscOperand xsdk_xsc_block_register(XscLower *l, const XscBlockAccess *access, int64_t extra);
XscOperand xsdk_xsc_block_value(XscLower *l, uint32_t ssa, const XscBlockAccess *access);
const XscMatrix *xsdk_xsc_block_matrix(XscLower *l, uint32_t ssa, const XscBlockAccess *access);
XscOperand xsdk_xsc_int_operand(XscLower *l, uint32_t ssa);

// Stages (xsc_lower_stage.c and a file each): a container and what it needs, lower.py's literal manifest.
typedef struct {
	uint8_t *container;
	size_t container_bytes;
	XsdkXscNeed *needs;
	size_t need_count;
	size_t need_capacity;
} XscLowered;

// Every OpStore's value by its target, the last store's (lower.py _store_map); the targets in first-store order.
typedef struct {
	uint32_t *value;
	uint32_t *targets;
	size_t target_count;
} XscStores;

typedef struct {
	XscConstant *items;
	size_t count;
	size_t capacity;
} XscConstants;

bool xsdk_xsc_store_map(Xsc *x, const XscModule *m, XscStores *stores);
// The function-local variables declared in instructions first to end, marked by id.
bool *xsdk_xsc_local_vars(Xsc *x, const XscModule *m, size_t first, size_t end);
void xsdk_xsc_fill_local_store(XscLower *l, const XscStores *stores, const bool *locals);
bool xsdk_xsc_check_local_stores(
	Xsc *x, const XscModule *m, const bool *locals, uint32_t merged_local, uint32_t merge_label);
int64_t xsdk_xsc_literal_base(const XscInterface *si);
const char *xsdk_xsc_format(Xsc *x, const char *format, ...) __attribute__((format(printf, 2, 3)));
XscConstant xsdk_xsc_vector_constant(const char *name, int64_t reg, int64_t regs);
bool xsdk_xsc_add_constant(Xsc *x, XscConstants *constants, XscConstant c);
bool xsdk_xsc_add_need(Xsc *x, XscLowered *out, XsdkXscNeed need);
// The literal pool as constants named _lit<register>, and as needs.
bool xsdk_xsc_literal_constants(XscLower *l, XscConstants *constants);
bool xsdk_xsc_literal_needs(XscLower *l, XscLowered *out);

bool xsdk_xsc_lower_vertex(
	Xsc *x, const XscModule *m, const XsdkXscOptions *options, XscInterface *si, XscLowered *out);
bool xsdk_xsc_lower_fragment(
	Xsc *x, const XscModule *m, const XsdkXscOptions *options, XscInterface *si, XscLowered *out);
// The hardware loop lowering's one shape: for (i = 0; i < u_count; i++) sum += k (lower.py lower_fragment_real_loop).
bool xsdk_xsc_lower_real_loop(Xsc *x, const XscModule *m, uint32_t registers, XscInterface *si, XscLowered *out);

#endif
