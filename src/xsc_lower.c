#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xsc_lower.h"

// Values lowered to instructions, as xenos-toolchain/xsc/lower.py's ValueLowerer emits them; each function names the
// method it follows.

enum {
	TEXT_BYTES = 128,
	MASK_BIT_LIMIT = 62,
	OP_U_DIV = 134,
	OP_U_MOD = 137,
	OP_S_MOD = 139,
	OP_ANY = 154,
	OP_ALL = 155,
	OP_LOGICAL_OR = 166,
	OP_LOGICAL_AND = 167,
	OP_LOGICAL_NOT = 168,
	OP_SHIFT_RIGHT_LOGICAL = 194,
	OP_NOT = 200,
	GLSL_MATRIX_INVERSE = 34,
};

bool xsdk_xsc_lower_refuse(XscLower *l, const char *format, ...)
{
	char text[XSDK_XSC_MESSAGE_BYTES];
	va_list args;
	va_start(args, format);
	(void)vsnprintf(text, sizeof text, format, args);
	va_end(args);
	return xsdk_xsc_fail(l->x, XSDK_XSC_REFUSED, "%s", text);
}

static bool fill_operands(Xsc *x, XscOperand **out, uint32_t count)
{
	*out = xsdk_xsc_alloc(x, (size_t)count * sizeof **out);
	return *out != NULL;
}

static bool fill_regs(Xsc *x, int64_t **out, uint32_t count)
{
	*out = xsdk_xsc_alloc(x, (size_t)count * sizeof **out);
	for (uint32_t i = 0; *out && i < count; i++)
		(*out)[i] = XSC_NONE;
	return *out != NULL;
}

bool xsdk_xsc_lower_init(Xsc *x, XscLower *l, const XscModule *m, const XscInterface *si, XscProgram *p,
	const XscType *types, bool vertex, int64_t first_reg, int64_t literal_base)
{
	memset(l, 0, sizeof *l);
	l->x = x;
	l->m = m;
	l->si = si;
	l->p = p;
	l->types = types;
	l->vertex = vertex;
	l->next_reg = first_reg;
	l->lit_base = literal_base;
	const uint32_t n = m->id_limit;
	l->matrix_cache = xsdk_xsc_alloc(x, (size_t)n * sizeof *l->matrix_cache);
	l->col_major = xsdk_xsc_alloc(x, n);
	l->local_store = xsdk_xsc_alloc(x, (size_t)n * sizeof *l->local_store);
	l->seen = xsdk_xsc_alloc(x, n);
	l->merge_branch = xsdk_xsc_alloc(x, (size_t)n * sizeof *l->merge_branch);
	for (uint32_t id = 0; l->merge_branch && id < n; id++)
		l->merge_branch[id] = -1;
	for (size_t i = 0; l->merge_branch && i < m->merge_count; i++) {
		if (m->merges[i].select < n)
			l->merge_branch[m->merges[i].select] = (int32_t)m->merges[i].branch;
	}
	const uint32_t branches = (uint32_t)m->branch_count + 1;
	l->branch_cnd = xsdk_xsc_alloc(x, branches * sizeof *l->branch_cnd);
	l->branch_swap = xsdk_xsc_alloc(x, branches);
	return fill_operands(x, &l->op_cache, n) && fill_operands(x, &l->local_values, n) && fill_regs(x, &l->inputs, n) &&
		fill_regs(x, &l->sample_reg, n) && fill_regs(x, &l->deriv_reg, n) && l->matrix_cache && l->col_major &&
		l->local_store && l->seen && l->merge_branch && fill_operands(x, &l->branch_test, branches) && l->branch_cnd &&
		l->branch_swap;
}

// An SSA id's instruction, refused as Python's by_result[ssa] would fail.
const XscInstr *xsdk_xsc_lower_result(XscLower *l, uint32_t ssa)
{
	const XscInstr *const ins = xsdk_xsc_by_result(l->m, ssa);
	if (!ins)
		xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "%u", (unsigned)ssa);
	return ins;
}

int64_t xsdk_xsc_fresh(XscLower *l)
{
	return l->next_reg++;
}

const XscType *xsdk_xsc_result_type(XscLower *l, uint32_t ssa)
{
	const XscInstr *const ins = xsdk_xsc_by_result(l->m, ssa);
	return ins ? xsdk_xsc_type(l->m, l->types, ins->result_type) : NULL;
}

int64_t xsdk_xsc_vec_size(XscLower *l, uint32_t ssa)
{
	if (!xsdk_xsc_lower_result(l, ssa))
		return 1;
	const XscType *const t = xsdk_xsc_result_type(l, ssa);
	return t && t->kind == XSC_TYPE_VECTOR ? t->count : 1;
}

double xsdk_xsc_scalar_const(XscLower *l, uint32_t ssa)
{
	const XscInstr *const ins = xsdk_xsc_lower_result(l, ssa);
	return ins ? xsdk_xsc_float_bits(xsdk_xsc_operand(ins, 0)) : 0;
}

static bool is_zero(double v)
{
	return v == 0.0;
}

static bool is_one(double v)
{
	return v == 1.0;
}

static XscTypeKind result_kind(XscLower *l, uint32_t ssa)
{
	const XscType *const t = xsdk_xsc_result_type(l, ssa);
	return t ? t->kind : XSC_TYPE_NONE;
}

// Marks an id visited, false if it was already: lower.py's seen set, shared down a fold's recursion.
static bool visit(XscLower *l, uint32_t ssa)
{
	if (ssa >= l->m->id_limit)
		return true;
	if (l->seen[ssa])
		return false;
	if (!xsdk_xsc_room(l->x, &l->seen_ids, l->seen_count, &l->seen_capacity, sizeof *l->seen_ids))
		return false;
	l->seen[ssa] = true;
	l->seen_ids[l->seen_count++] = ssa;
	return true;
}

static void forget_visits(XscLower *l)
{
	for (size_t i = 0; i < l->seen_count; i++)
		l->seen[l->seen_ids[i]] = false;
	l->seen_count = 0;
}

static int64_t signed_literal(uint32_t literal)
{
	return literal >= 0x80000000u ? (int64_t)literal - 0x100000000LL : literal;
}

static bool int_fold(XscLower *l, uint32_t ssa, int64_t *value);
static bool float_fold(XscLower *l, uint32_t ssa, double *value);

// A folded signed int, or false where it's data-dependent (lower.py int_const).
static bool int_fold_at(XscLower *l, uint32_t ssa, int64_t *value)
{
	if (!visit(l, ssa))
		return false;
	const XscInstr *const ins = xsdk_xsc_by_result(l->m, ssa);
	if (!ins)
		return false;
	const uint32_t op = ins->opcode;
	if (op == SPV_LOAD) {
		const uint32_t src = xsdk_xsc_operand(ins, 0);
		return src < l->m->id_limit && l->local_store[src] && int_fold(l, l->local_store[src], value);
	}
	if (op == SPV_CONSTANT && result_kind(l, ssa) == XSC_TYPE_INT) {
		*value = signed_literal(xsdk_xsc_operand(ins, 0));
		return true;
	}
	if (op == SPV_CONVERT_F_TO_S) {
		double v;
		if (!float_fold(l, xsdk_xsc_operand(ins, 0), &v))
			return false;
		// Python's int() of an infinity or NaN raises.
		if (!isfinite(v))
			return xsdk_xsc_fail(
				l->x, XSDK_XSC_MALFORMED, "cannot convert float %s to integer", isnan(v) ? "NaN" : "infinity");
		*value = (int64_t)v;
		return true;
	}
	if (op != SPV_I_ADD && op != SPV_I_SUB && op != SPV_I_MUL && op != SPV_S_DIV)
		return false;
	int64_t a, b;
	if (!int_fold(l, xsdk_xsc_operand(ins, 0), &a) || !int_fold(l, xsdk_xsc_operand(ins, 1), &b))
		return false;
	if (op == SPV_I_ADD)
		*value = a + b;
	else if (op == SPV_I_SUB)
		*value = a - b;
	else if (op == SPV_I_MUL)
		*value = a * b;
	else
		*value = b != 0 ? (int64_t)((double)a / (double)b) : 0;
	return true;
}

// A folded float, or false where it's data-dependent (lower.py float_const).
static bool float_fold_at(XscLower *l, uint32_t ssa, double *value)
{
	if (!visit(l, ssa))
		return false;
	const XscInstr *const ins = xsdk_xsc_by_result(l->m, ssa);
	if (!ins)
		return false;
	const uint32_t op = ins->opcode;
	if (op == SPV_LOAD) {
		const uint32_t src = xsdk_xsc_operand(ins, 0);
		return src < l->m->id_limit && l->local_store[src] && float_fold(l, l->local_store[src], value);
	}
	if (op == SPV_CONSTANT && result_kind(l, ssa) == XSC_TYPE_FLOAT) {
		*value = xsdk_xsc_scalar_const(l, ssa);
		return true;
	}
	if (op == SPV_CONVERT_S_TO_F) {
		int64_t v;
		if (!int_fold(l, xsdk_xsc_operand(ins, 0), &v))
			return false;
		*value = (double)v;
		return true;
	}
	if (op == SPV_F_NEGATE) {
		if (!float_fold(l, xsdk_xsc_operand(ins, 0), value))
			return false;
		*value = -*value;
		return true;
	}
	if (op != SPV_F_ADD && op != SPV_F_SUB && op != SPV_F_MUL && op != SPV_F_DIV)
		return false;
	double a, b;
	if (!float_fold(l, xsdk_xsc_operand(ins, 0), &a) || !float_fold(l, xsdk_xsc_operand(ins, 1), &b))
		return false;
	if (op == SPV_F_ADD)
		*value = a + b;
	else if (op == SPV_F_SUB)
		*value = a - b;
	else if (op == SPV_F_MUL)
		*value = a * b;
	else
		*value = b != 0.0 ? a / b : 0.0;
	return true;
}

static bool int_fold(XscLower *l, uint32_t ssa, int64_t *value)
{
	if (!xsdk_xsc_enter(l))
		return false;
	const bool folded = int_fold_at(l, ssa, value);
	l->depth--;
	return folded;
}

static bool float_fold(XscLower *l, uint32_t ssa, double *value)
{
	if (!xsdk_xsc_enter(l))
		return false;
	const bool folded = float_fold_at(l, ssa, value);
	l->depth--;
	return folded;
}

bool xsdk_xsc_int_const(XscLower *l, uint32_t ssa, int64_t *value)
{
	const bool folded = int_fold(l, ssa, value);
	forget_visits(l);
	return folded;
}

bool xsdk_xsc_float_const(XscLower *l, uint32_t ssa, double *value)
{
	const bool folded = float_fold(l, ssa, value);
	forget_visits(l);
	return folded;
}

bool xsdk_xsc_float_const01(XscLower *l, uint32_t ssa, double *value)
{
	return xsdk_xsc_float_const(l, ssa, value) && (is_zero(*value) || is_one(*value));
}

// A constant vector's components, at most 4 used (lower.py const_vector); its count, or -1 if it isn't one.
static int const_vector(XscLower *l, uint32_t ssa, double *vals, int capacity)
{
	const XscInstr *const ins = xsdk_xsc_by_result(l->m, ssa);
	if (!ins)
		return -1;
	if (ins->opcode == SPV_CONSTANT && result_kind(l, ssa) == XSC_TYPE_FLOAT) {
		vals[0] = xsdk_xsc_scalar_const(l, ssa);
		return 1;
	}
	if (ins->opcode != SPV_CONSTANT_COMPOSITE && ins->opcode != SPV_COMPOSITE_CONSTRUCT)
		return -1;
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	int n = 0;
	for (uint32_t i = 0; i < count; i++) {
		double v;
		if (ins->opcode == SPV_CONSTANT_COMPOSITE) {
			const XscInstr *const ci = xsdk_xsc_by_result(l->m, ops[i]);
			if (!ci || ci->opcode != SPV_CONSTANT)
				return -1;
			v = xsdk_xsc_scalar_const(l, ops[i]);
		} else if (!xsdk_xsc_float_const(l, ops[i], &v)) {
			return -1;
		}
		if (n < capacity)
			vals[n] = v;
		n++;
	}
	return n;
}

enum {
	CONST_VECTOR_CAPACITY = 16,
};

// The component a constant one-hot vector picks (lower.py axis_component), or -1.
static int64_t axis_component(XscLower *l, uint32_t ssa)
{
	double vals[CONST_VECTOR_CAPACITY];
	const int count = const_vector(l, ssa, vals, CONST_VECTOR_CAPACITY);
	if (count < 0)
		return -1;
	const int64_t n = xsdk_xsc_vec_size(l, ssa);
	if (count < n || n > CONST_VECTOR_CAPACITY)
		return -1;
	int64_t axis = -1;
	int ones = 0;
	for (int64_t i = 0; i < n; i++) {
		if (is_one(vals[i])) {
			axis = i;
			ones++;
		}
	}
	if (ones != 1)
		return -1;
	for (int64_t i = 0; i < n; i++) {
		if (i != axis && !is_zero(vals[i]))
			return -1;
	}
	return axis;
}

int64_t xsdk_xsc_swz(const int64_t *idxs, size_t count)
{
	int64_t out = 0;
	for (size_t p = 0; p < count; p++)
		out += ((idxs[p] - (int64_t)p) & 3) << (2 * p);
	return out;
}

int64_t xsdk_xsc_bsw(int64_t c)
{
	int64_t out = 0;
	for (int64_t p = 0; p < 4; p++)
		out += ((c - p) & 3) << (2 * p);
	return out;
}

// A register holding value4, pooled by its components to 9 places (lower.py literal_reg).
int64_t xsdk_xsc_literal_reg(XscLower *l, const double value[4])
{
	double key[4];
	for (int i = 0; i < 4; i++)
		key[i] = xsdk_xsc_round9(value[i]);
	for (size_t i = 0; i < l->pool_count; i++) {
		const double *const k = &l->pool_keys[4 * i];
		if (k[0] == key[0] && k[1] == key[1] && k[2] == key[2] && k[3] == key[3])
			return l->pool[i].reg;
	}
	size_t capacity = l->pool_capacity;
	if (!xsdk_xsc_room(l->x, &l->pool, l->pool_count, &capacity, sizeof *l->pool))
		return XSC_NONE;
	l->pool_capacity = capacity;
	if (!xsdk_xsc_room(l->x, &l->pool_keys, l->pool_count, &l->pool_key_capacity, 4 * sizeof *l->pool_keys))
		return XSC_NONE;
	const int64_t reg = l->lit_base++;
	memcpy(&l->pool_keys[4 * l->pool_count], key, sizeof key);
	l->pool[l->pool_count].reg = reg;
	memcpy(l->pool[l->pool_count].value, value, sizeof l->pool[l->pool_count].value);
	l->pool_count++;
	return reg;
}

int64_t xsdk_xsc_literal_splat(XscLower *l, double value)
{
	const double four[4] = { value, value, value, value };
	return xsdk_xsc_literal_reg(l, four);
}

static bool operand_equal(XscOperand a, XscOperand b)
{
	if (a.kind != b.kind)
		return false;
	if (a.kind != XSC_OPERAND_MAT)
		return a.value == b.value;
	if (a.mat->count != b.mat->count)
		return false;
	for (size_t i = 0; i < a.mat->count; i++) {
		if (!operand_equal(a.mat->rows[i], b.mat->rows[i]))
			return false;
	}
	return true;
}

static bool set_src(XscLower *l, XscAlu *f, int n, XscOperand o, bool negate, int64_t swizzle)
{
	if (o.kind == XSC_OPERAND_MAT || o.kind == XSC_OPERAND_NONE)
		return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "a matrix used as one register");
	f->src_reg[n - 1] = o.value;
	f->src_is_temp[n - 1] = o.kind == XSC_OPERAND_REG;
	f->src_swizzle[n - 1] = swizzle;
	f->src_negate[n - 1] = negate;
	return true;
}

bool xsdk_xsc_emit_alu(XscLower *l, const XscAlu *f)
{
	return xsdk_xsc_program_alu(l->x, l->p, f) != NULL;
}

// One instruction, one constant register at most: the others go through temps first (lower.py emit).
bool xsdk_xsc_emit(
	XscLower *l, int64_t opcode, const XscSrc *ops, size_t count, int64_t dst, bool export_, int64_t mask, bool clamp)
{
	XscSrc local[3];
	memcpy(local, ops, count * sizeof *ops);
	int64_t lowest = 0;
	size_t distinct = 0;
	int64_t seen[3];
	for (size_t i = 0; i < count; i++) {
		if (local[i].o.kind != XSC_OPERAND_CONST)
			continue;
		bool known = false;
		for (size_t j = 0; j < distinct; j++)
			known = known || seen[j] == local[i].o.value;
		if (known)
			continue;
		seen[distinct++] = local[i].o.value;
		if (distinct == 1 || local[i].o.value < lowest)
			lowest = local[i].o.value;
	}
	for (size_t i = 0; distinct > 1 && i < count; i++) {
		if (local[i].o.kind == XSC_OPERAND_CONST && local[i].o.value != lowest) {
			local[i].o = xsdk_xsc_to_reg(l, local[i].o);
			if (xsc_failed(l))
				return false;
		}
	}
	XscAlu f = xsdk_xsc_template_mov();
	f.vector_opcode = opcode;
	f.vector_dest = dst;
	f.is_export = export_;
	f.vector_write_mask = mask;
	f.vector_clamp = clamp;
	for (size_t i = 0; i < count; i++) {
		if (!set_src(l, &f, (int)i + 1, local[i].o, local[i].negate, local[i].swizzle))
			return false;
	}
	return xsdk_xsc_emit_alu(l, &f);
}

int64_t xsdk_xsc_emit_copy(
	XscLower *l, XscOperand opnd, int64_t dst, bool export_, bool negate, int64_t swizzle, int64_t mask, bool clamp)
{
	if (dst == XSC_NONE && !export_ && !negate && swizzle == 0 && !clamp && opnd.kind == XSC_OPERAND_REG)
		return opnd.value;
	if (dst == XSC_NONE)
		dst = xsdk_xsc_fresh(l);
	const XscSrc ops[2] = { xsc_src(opnd, negate, swizzle), xsc_src(opnd, negate, swizzle) };
	return xsdk_xsc_emit(l, XSC_VECTOR_MAX, ops, 2, dst, export_, mask, clamp) ? dst : XSC_NONE;
}

bool xsdk_xsc_emit_export_constants(XscLower *l, int64_t dst_reg, int64_t zero_mask, int64_t one_mask)
{
	if (!zero_mask && !one_mask)
		return true;
	XscAlu f = xsdk_xsc_template_mov();
	f.vector_opcode = XSC_VECTOR_MAX;
	f.vector_dest = dst_reg;
	f.is_export = true;
	f.vector_write_mask = one_mask;
	f.scalar_write_mask = one_mask;
	f.is_scalar_dest_relative = zero_mask != 0;
	f.vector_clamp = false;
	return xsdk_xsc_emit_alu(l, &f);
}

int64_t xsdk_xsc_emit_scalar(XscLower *l, int64_t sop, int64_t dst, XscOperand src, int64_t src_comp, int64_t mask)
{
	XscAlu f = xsdk_xsc_template_mov();
	f.vector_opcode = XSC_VECTOR_MAX;
	f.vector_dest = 0;
	f.vector_write_mask = 0;
	f.vector_clamp = false;
	f.scalar_opcode = sop;
	f.scalar_dest = dst;
	f.scalar_write_mask = mask;
	f.scalar_clamp = false;
	f.is_export = false;
	// ((src_comp - 3) & 3) << 6 from a table: GCC makes that shift an rldic, which Xenia skips (tools/xenia_scan.py).
	static const uint8_t swizzles[4] = { 0x00, 0x40, 0x80, 0xC0 };
	if (!set_src(l, &f, 3, src, false, swizzles[(src_comp - 3) & 3]))
		return XSC_NONE;
	return xsdk_xsc_emit_alu(l, &f) ? dst : XSC_NONE;
}

XscOperand xsdk_xsc_to_reg(XscLower *l, XscOperand opnd)
{
	if (opnd.kind == XSC_OPERAND_REG)
		return opnd;
	const int64_t r = xsdk_xsc_fresh(l);
	const XscSrc ops[2] = { xsc_src(opnd, false, 0), xsc_src(opnd, false, 0) };
	if (!xsdk_xsc_emit(l, XSC_VECTOR_MAX, ops, 2, r, false, 0xF, false))
		return (XscOperand){ 0 };
	return xsc_reg(r);
}

int64_t xsdk_xsc_dotself(XscLower *l, XscOperand ov, int64_t n)
{
	const int64_t d = xsdk_xsc_fresh(l);
	const XscSrc ops[2] = { xsc_src(ov, false, 0), xsc_src(ov, false, 0) };
	return xsdk_xsc_emit(l, n == 3 ? XSC_VECTOR_DP3 : XSC_VECTOR_DP4, ops, 2, d, false, 0xF, false) ? d : XSC_NONE;
}

int64_t xsdk_xsc_recip(XscLower *l, XscOperand opnd, int64_t n)
{
	if (n == 1)
		return xsdk_xsc_emit_scalar(l, XSC_SCALAR_RCP, xsdk_xsc_fresh(l), opnd, 0, 0xF);
	const int64_t r = xsdk_xsc_fresh(l);
	for (int64_t i = 0; i < n; i++) {
		const int64_t ri = xsdk_xsc_emit_scalar(l, XSC_SCALAR_RCP, xsdk_xsc_fresh(l), opnd, i, 0xF);
		if (ri == XSC_NONE)
			return XSC_NONE;
		const XscSrc ops[2] = { xsc_src(xsc_reg(ri), false, 0), xsc_src(xsc_reg(ri), false, 0) };
		if (!xsdk_xsc_emit(l, XSC_VECTOR_MAX, ops, 2, r, false, (int64_t)1 << i, false))
			return XSC_NONE;
	}
	return r;
}

bool xsdk_xsc_const_scalar(XscLower *l, uint32_t ssa, double *value)
{
	const XscInstr *const ins = xsdk_xsc_lower_result(l, ssa);
	if (!ins)
		return false;
	if (ins->opcode == SPV_CONSTANT) {
		*value = xsdk_xsc_scalar_const(l, ssa);
		return true;
	}
	if (ins->opcode == SPV_COMPOSITE_CONSTRUCT) {
		uint32_t count;
		const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
		bool same = count > 0;
		for (uint32_t i = 1; i < count; i++)
			same = same && ops[i] == ops[0];
		const XscInstr *const part = same ? xsdk_xsc_lower_result(l, ops[0]) : NULL;
		if (xsc_failed(l))
			return false;
		if (part && part->opcode == SPV_CONSTANT) {
			*value = xsdk_xsc_scalar_const(l, ops[0]);
			return true;
		}
	}
	return xsdk_xsc_lower_refuse(l, "%%%u is not a constant scalar", (unsigned)ssa);
}

// An int constant's raw literal (lower.py const_val).
static bool const_val(XscLower *l, uint32_t ssa, int64_t *value)
{
	const XscInstr *const c = xsdk_xsc_by_result(l->m, ssa);
	if (!c || c->opcode != SPV_CONSTANT)
		return xsdk_xsc_lower_refuse(l, "%%%u is not an int constant", (unsigned)ssa);
	*value = xsdk_xsc_operand(c, 0);
	return true;
}

// ---- integers as floats (lower.py int_operand) ----

static XscOperand int_arith(XscLower *l, const XscInstr *ins)
{
	const uint32_t op = ins->opcode;
	const uint32_t a = xsdk_xsc_operand(ins, 0);
	const uint32_t b = xsdk_xsc_operand(ins, 1);
	int64_t ca, cb;
	const bool has_a = xsdk_xsc_int_const(l, a, &ca);
	const bool has_b = xsdk_xsc_int_const(l, b, &cb);
	if (op == SPV_I_ADD) {
		if (has_a && ca == 0)
			return xsdk_xsc_int_operand(l, b);
		if (has_b && cb == 0)
			return xsdk_xsc_int_operand(l, a);
	} else if (op == SPV_I_SUB && has_b && cb == 0) {
		return xsdk_xsc_int_operand(l, a);
	} else if (op == SPV_I_MUL) {
		if (has_a && ca == 1)
			return xsdk_xsc_int_operand(l, b);
		if (has_b && cb == 1)
			return xsdk_xsc_int_operand(l, a);
	}
	const XscOperand oa = xsdk_xsc_int_operand(l, a);
	if (xsc_failed(l))
		return oa;
	const XscOperand ob = xsdk_xsc_int_operand(l, b);
	if (xsc_failed(l))
		return ob;
	const int64_t r = xsdk_xsc_fresh(l);
	const XscSrc ops[2] = { xsc_src(oa, false, 0), xsc_src(ob, op == SPV_I_SUB, 0) };
	const int64_t vop = op == SPV_I_MUL ? XSC_VECTOR_MUL : XSC_VECTOR_ADD;
	if (!xsdk_xsc_emit(l, vop, ops, 2, r, false, 0xF, false))
		return (XscOperand){ 0 };
	return xsc_reg(r);
}

// Float emulation: q = trunc(a * rcp(b) + eps), the small bias restoring exact multiples rcp misses.
static XscOperand int_divide(XscLower *l, const XscInstr *ins)
{
	const uint32_t op = ins->opcode;
	const XscOperand oa = xsdk_xsc_int_operand(l, xsdk_xsc_operand(ins, 0));
	if (xsc_failed(l))
		return oa;
	const XscOperand ob = xsdk_xsc_int_operand(l, xsdk_xsc_operand(ins, 1));
	if (xsc_failed(l))
		return ob;
	const int64_t rb = xsdk_xsc_emit_scalar(l, XSC_SCALAR_RCP, xsdk_xsc_fresh(l), ob, 3, 0xF);
	if (rb == XSC_NONE)
		return (XscOperand){ 0 };
	const int64_t q = xsdk_xsc_fresh(l);
	const int64_t eps = xsdk_xsc_literal_splat(l, 1.0 / 1024);
	if (eps == XSC_NONE)
		return (XscOperand){ 0 };
	const XscSrc mad[3] = { xsc_src(oa, false, 0), xsc_src(xsc_reg(rb), false, 0), xsc_src(xsc_const(eps), false, 0) };
	if (!xsdk_xsc_emit(l, XSC_VECTOR_MAD, mad, 3, q, false, 0xF, false))
		return (XscOperand){ 0 };
	const int64_t t = xsdk_xsc_fresh(l);
	const XscSrc trunc_ops[2] = { xsc_src(xsc_reg(q), false, 0), xsc_src(xsc_reg(q), false, 0) };
	if (!xsdk_xsc_emit(l, XSC_VECTOR_TRUNC, trunc_ops, 2, t, false, 0xF, false))
		return (XscOperand){ 0 };
	if (op == OP_U_DIV || op == SPV_S_DIV)
		return xsc_reg(t);
	// The remainder: a - b * trunc(a / b).
	const int64_t r = xsdk_xsc_fresh(l);
	const XscSrc rem[3] = { xsc_src(ob, true, 0), xsc_src(xsc_reg(t), false, 0), xsc_src(oa, false, 0) };
	if (!xsdk_xsc_emit(l, XSC_VECTOR_MAD, rem, 3, r, false, 0xF, false))
		return (XscOperand){ 0 };
	return xsc_reg(r);
}

static XscOperand int_operand_of(XscLower *l, uint32_t ssa)
{
	int64_t cv;
	if (xsdk_xsc_int_const(l, ssa, &cv)) {
		const int64_t reg = xsdk_xsc_literal_splat(l, (double)cv);
		return reg == XSC_NONE ? (XscOperand){ 0 } : xsc_const(reg);
	}
	const XscInstr *const ins = xsdk_xsc_lower_result(l, ssa);
	if (!ins)
		return (XscOperand){ 0 };
	const uint32_t op = ins->opcode;
	if (op == SPV_LOAD) {
		const uint32_t src = xsdk_xsc_operand(ins, 0);
		if (src < l->m->id_limit && l->local_store[src])
			return xsdk_xsc_int_operand(l, l->local_store[src]);
		xsdk_xsc_lower_refuse(l, "integer Load must be a foldable local");
		return (XscOperand){ 0 };
	}
	if (op == SPV_CONVERT_F_TO_S) {
		const XscOperand ov = xsdk_xsc_lower_operand(l, xsdk_xsc_operand(ins, 0));
		if (xsc_failed(l))
			return ov;
		const int64_t r = xsdk_xsc_fresh(l);
		const XscSrc ops[2] = { xsc_src(ov, false, 0), xsc_src(ov, false, 0) };
		if (!xsdk_xsc_emit(l, XSC_VECTOR_TRUNC, ops, 2, r, false, 0xF, false))
			return (XscOperand){ 0 };
		return xsc_reg(r);
	}
	if (op == SPV_SELECT && xsdk_xsc_is_merge(l, ssa))
		return xsdk_xsc_lower_operand(l, ssa);
	if (op == SPV_I_ADD || op == SPV_I_SUB || op == SPV_I_MUL)
		return int_arith(l, ins);
	if (op == OP_U_DIV || op == SPV_S_DIV || (op >= OP_U_MOD && op <= OP_S_MOD))
		return int_divide(l, ins);
	char name[16];
	xsdk_xsc_lower_refuse(
		l, "v1 %s: unsupported integer op %s", xsc_stage_name(l), xsdk_xsc_op_name(op, name, sizeof name));
	return (XscOperand){ 0 };
}

XscOperand xsdk_xsc_int_operand(XscLower *l, uint32_t ssa)
{
	if (!xsdk_xsc_enter(l))
		return (XscOperand){ 0 };
	const XscOperand res = int_operand_of(l, ssa);
	l->depth--;
	return res;
}

// ---- comparisons and booleans as 0.0 and 1.0 ----

// A comparison opcode's set op, and whether its operands swap (lower.py _CMP_VALUE); false for other opcodes.
static bool compare_op(uint32_t opcode, int64_t *xop, bool *swap)
{
	static const struct {
		uint16_t opcode;
		uint8_t xop;
		bool swap;
	} table[] = { { 186, XSC_VECTOR_SGT, false }, { 184, XSC_VECTOR_SGT, true }, { 190, XSC_VECTOR_SGE, false },
		{ 188, XSC_VECTOR_SGE, true }, { 180, XSC_VECTOR_SEQ, false }, { 182, XSC_VECTOR_SNE, false },
		{ 173, XSC_VECTOR_SGT, false }, { 177, XSC_VECTOR_SGT, true }, { 175, XSC_VECTOR_SGE, false },
		{ 179, XSC_VECTOR_SGE, true }, { 172, XSC_VECTOR_SGT, false }, { 176, XSC_VECTOR_SGT, true },
		{ 174, XSC_VECTOR_SGE, false }, { 178, XSC_VECTOR_SGE, true }, { 170, XSC_VECTOR_SEQ, false },
		{ 171, XSC_VECTOR_SNE, false } };
	for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
		if (table[i].opcode == opcode) {
			*xop = table[i].xop;
			*swap = table[i].swap;
			return true;
		}
	}
	return false;
}

static bool compare_value(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	int64_t xop;
	bool swap;
	if (!compare_op(ins->opcode, &xop, &swap))
		return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "Op%u isn't a comparison", (unsigned)ins->opcode);
	XscOperand a = xsdk_xsc_lower_operand(l, xsdk_xsc_operand(ins, 0));
	if (xsc_failed(l))
		return false;
	XscOperand b = xsdk_xsc_lower_operand(l, xsdk_xsc_operand(ins, 1));
	if (xsc_failed(l))
		return false;
	if (swap) {
		const XscOperand t = a;
		a = b;
		b = t;
	}
	const XscSrc ops[2] = { xsc_src(a, false, 0), xsc_src(b, false, 0) };
	return xsdk_xsc_emit(l, xop, ops, 2, dst, export_, 0xF, false);
}

XscOperand xsdk_xsc_bool01(XscLower *l, uint32_t ssa)
{
	const XscInstr *const ins = xsdk_xsc_lower_result(l, ssa);
	if (!ins)
		return (XscOperand){ 0 };
	int64_t xop;
	bool swap;
	if (compare_op(ins->opcode, &xop, &swap)) {
		const int64_t r = xsdk_xsc_fresh(l);
		return compare_value(l, ins, r, false) ? xsc_reg(r) : (XscOperand){ 0 };
	}
	return xsdk_xsc_lower_operand(l, ssa);
}

// OpSelect(cond, t, f): f + (t - f) * cond, component-wise (lower.py select_value).
static bool select_value(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	const XscOperand c = xsdk_xsc_bool01(l, xsdk_xsc_operand(ins, 0));
	if (xsc_failed(l))
		return false;
	const XscOperand tr = xsdk_xsc_to_reg(l, xsdk_xsc_lower_operand(l, xsdk_xsc_operand(ins, 1)));
	if (xsc_failed(l))
		return false;
	const XscOperand fr = xsdk_xsc_to_reg(l, xsdk_xsc_lower_operand(l, xsdk_xsc_operand(ins, 2)));
	if (xsc_failed(l))
		return false;
	const int64_t diff = xsdk_xsc_fresh(l);
	const XscSrc sub[2] = { xsc_src(tr, false, 0), xsc_src(fr, true, 0) };
	if (!xsdk_xsc_emit(l, XSC_VECTOR_ADD, sub, 2, diff, false, 0xF, false))
		return false;
	const XscSrc mad[3] = { xsc_src(xsc_reg(diff), false, 0), xsc_src(c, false, 0), xsc_src(fr, false, 0) };
	return xsdk_xsc_emit(l, XSC_VECTOR_MAD, mad, 3, dst, export_, 0xF, false);
}

// any(bvec) as a horizontal max, all(bvec) a horizontal min (lower.py reduce_bool).
static bool reduce_bool(XscLower *l, const XscInstr *ins, int64_t dst, bool export_, bool is_all)
{
	const uint32_t v = xsdk_xsc_operand(ins, 0);
	const XscOperand vr = xsdk_xsc_to_reg(l, xsdk_xsc_bool01(l, v));
	if (xsc_failed(l))
		return false;
	const int64_t size = xsdk_xsc_vec_size(l, v);
	const int64_t n = size > 1 ? size : 1;
	const int64_t op = is_all ? XSC_VECTOR_MIN : XSC_VECTOR_MAX;
	const int64_t acc = xsdk_xsc_fresh(l);
	const XscSrc first[2] = { xsc_src(vr, false, xsdk_xsc_bsw(0)), xsc_src(vr, false, xsdk_xsc_bsw(0)) };
	if (!xsdk_xsc_emit(l, XSC_VECTOR_MAX, first, 2, acc, false, 0xF, false))
		return false;
	for (int64_t i = 1; i < n; i++) {
		const XscSrc step[2] = { xsc_src(xsc_reg(acc), false, 0), xsc_src(vr, false, xsdk_xsc_bsw(i)) };
		if (!xsdk_xsc_emit(l, op, step, 2, acc, false, 0xF, false))
			return false;
	}
	const XscSrc last[2] = { xsc_src(xsc_reg(acc), false, 0), xsc_src(xsc_reg(acc), false, 0) };
	return xsdk_xsc_emit(l, XSC_VECTOR_MAX, last, 2, dst, export_, 0xF, false);
}

static bool logical_not_value(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	const XscOperand v = xsdk_xsc_bool01(l, xsdk_xsc_operand(ins, 0));
	if (xsc_failed(l))
		return false;
	const int64_t one = xsdk_xsc_literal_splat(l, 1.0);
	if (one == XSC_NONE)
		return false;
	const XscSrc ops[2] = { xsc_src(xsc_const(one), false, 0), xsc_src(v, true, 0) };
	return xsdk_xsc_emit(l, XSC_VECTOR_ADD, ops, 2, dst, export_, 0xF, false);
}

static bool logical_binop_value(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	const XscOperand a = xsdk_xsc_bool01(l, xsdk_xsc_operand(ins, 0));
	if (xsc_failed(l))
		return false;
	const XscOperand b = xsdk_xsc_bool01(l, xsdk_xsc_operand(ins, 1));
	if (xsc_failed(l))
		return false;
	const int64_t op = ins->opcode == OP_LOGICAL_AND ? XSC_VECTOR_MIN : XSC_VECTOR_MAX;
	const XscSrc ops[2] = { xsc_src(a, false, 0), xsc_src(b, false, 0) };
	return xsdk_xsc_emit(l, op, ops, 2, dst, export_, 0xF, false);
}

// ---- arithmetic (lower.py binop, vts, dot, fdiv, fmod) ----

// A ConstantComposite or CompositeConstruct's parts with zero ones as 0 (lower.py zero_construct_parts); -1 if
// it's neither, or a part is a non-zero constant.
static int zero_construct_parts(XscLower *l, uint32_t ssa, uint32_t *parts, int capacity)
{
	const XscInstr *const ins = xsdk_xsc_by_result(l->m, ssa);
	if (!ins || (ins->opcode != SPV_CONSTANT_COMPOSITE && ins->opcode != SPV_COMPOSITE_CONSTRUCT))
		return -1;
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	if ((int)count > capacity)
		return -1;
	for (uint32_t i = 0; i < count; i++) {
		double v;
		if (!xsdk_xsc_float_const(l, ops[i], &v))
			parts[i] = ops[i];
		else if (is_zero(v))
			parts[i] = 0;
		else
			return -1;
	}
	return (int)count;
}

static int64_t component_swizzle(int64_t comp, int64_t i)
{
	return comp == XSC_BROADCAST ? 0 : ((comp - i) & 3) << (2 * i);
}

// base + a construct whose parts are zero or scalars, lane by lane (lower.py add_zero_construct); XSC_NONE through
// folded false if it doesn't apply.
static int64_t add_zero_construct(XscLower *l, uint32_t base_ssa, uint32_t construct_ssa, int64_t dst, bool export_,
	bool negate_construct, bool clamp, bool *folded)
{
	uint32_t parts[CONST_VECTOR_CAPACITY];
	const int count = zero_construct_parts(l, construct_ssa, parts, CONST_VECTOR_CAPACITY);
	*folded = count >= 0;
	if (!*folded)
		return XSC_NONE;
	const XscOperand base = xsdk_xsc_lower_operand(l, base_ssa);
	if (xsc_failed(l))
		return XSC_NONE;
	int64_t zero_mask = 0;
	bool all_zero = true;
	for (int i = 0; i < count; i++) {
		if (!parts[i])
			zero_mask |= (int64_t)1 << i;
		else
			all_zero = false;
	}
	if (dst == XSC_NONE) {
		if (all_zero)
			return xsdk_xsc_emit_copy(l, base, XSC_NONE, export_, false, 0, 0xF, clamp);
		dst = xsdk_xsc_fresh(l);
	}
	if (zero_mask && xsdk_xsc_emit_copy(l, base, dst, export_, false, 0, zero_mask, clamp) == XSC_NONE)
		return XSC_NONE;
	for (int i = 0; i < count; i++) {
		if (!parts[i])
			continue;
		const XscOrigin origin = xsdk_xsc_scalar_origin(l, parts[i]);
		if (xsc_failed(l))
			return XSC_NONE;
		const XscSrc ops[2] = { xsc_src(base, false, 0),
			xsc_src(origin.o, negate_construct, component_swizzle(origin.comp, i)) };
		if (!xsdk_xsc_emit(l, XSC_VECTOR_ADD, ops, 2, dst, export_, (int64_t)1 << i, clamp))
			return XSC_NONE;
	}
	return dst;
}

static int64_t copy_of(XscLower *l, uint32_t ssa, int64_t dst, bool export_, bool negate, bool clamp)
{
	const XscOperand o = xsdk_xsc_lower_operand(l, ssa);
	if (xsc_failed(l))
		return XSC_NONE;
	return xsdk_xsc_emit_copy(l, o, dst, export_, negate, 0, 0xF, clamp);
}

int64_t xsdk_xsc_binop(XscLower *l, uint32_t op, uint32_t a, uint32_t b, int64_t dst, bool export_, bool clamp)
{
	double ca, cb;
	const bool has_a = xsdk_xsc_float_const01(l, a, &ca);
	const bool has_b = xsdk_xsc_float_const01(l, b, &cb);
	bool folded;
	int64_t r;
	if (op == SPV_F_ADD) {
		if (has_a && ca == 0.0)
			return copy_of(l, b, dst, export_, false, clamp);
		if (has_b && cb == 0.0)
			return copy_of(l, a, dst, export_, false, clamp);
		r = add_zero_construct(l, a, b, dst, export_, false, clamp, &folded);
		if (folded || xsc_failed(l))
			return r;
		r = add_zero_construct(l, b, a, dst, export_, false, clamp, &folded);
		if (folded || xsc_failed(l))
			return r;
	} else if (op == SPV_F_SUB) {
		if (has_b && cb == 0.0)
			return copy_of(l, a, dst, export_, false, clamp);
		if (has_a && ca == 0.0)
			return copy_of(l, b, dst, export_, true, clamp);
		r = add_zero_construct(l, a, b, dst, export_, true, clamp, &folded);
		if (folded || xsc_failed(l))
			return r;
	} else if (op == SPV_F_MUL) {
		if (has_a && ca == 1.0)
			return copy_of(l, b, dst, export_, false, clamp);
		if (has_b && cb == 1.0)
			return copy_of(l, a, dst, export_, false, clamp);
	}
	const XscOperand oa = xsdk_xsc_lower_operand(l, a);
	if (xsc_failed(l))
		return XSC_NONE;
	const XscOperand ob = xsdk_xsc_lower_operand(l, b);
	if (xsc_failed(l))
		return XSC_NONE;
	if (dst == XSC_NONE)
		dst = xsdk_xsc_fresh(l);
	const XscSrc ops[2] = { xsc_src(oa, false, 0), xsc_src(ob, op == SPV_F_SUB, 0) };
	const int64_t vop = op == SPV_F_MUL ? XSC_VECTOR_MUL : XSC_VECTOR_ADD;
	return xsdk_xsc_emit(l, vop, ops, 2, dst, export_, 0xF, clamp) ? dst : XSC_NONE;
}

int64_t xsdk_xsc_emit_clamped(XscLower *l, uint32_t ssa, int64_t dst, bool export_)
{
	const XscInstr *const vi = xsdk_xsc_lower_result(l, ssa);
	if (!vi)
		return XSC_NONE;
	if (vi->opcode == SPV_F_ADD || vi->opcode == SPV_F_SUB || vi->opcode == SPV_F_MUL)
		return xsdk_xsc_binop(l, vi->opcode, xsdk_xsc_operand(vi, 0), xsdk_xsc_operand(vi, 1), dst, export_, true);
	const XscOperand r = xsdk_xsc_lower_operand(l, ssa);
	if (xsc_failed(l))
		return XSC_NONE;
	const XscSrc ops[2] = { xsc_src(r, false, 0), xsc_src(r, false, 0) };
	return xsdk_xsc_emit(l, XSC_VECTOR_MAX, ops, 2, dst, export_, 0xF, true) ? dst : XSC_NONE;
}

static int64_t vts(XscLower *l, uint32_t vec, uint32_t scal, int64_t dst, bool export_)
{
	double one;
	if (xsdk_xsc_float_const01(l, scal, &one) && one == 1.0)
		return copy_of(l, vec, dst, export_, false, false);
	const XscOperand ov = xsdk_xsc_lower_operand(l, vec);
	if (xsc_failed(l))
		return XSC_NONE;
	const XscOperand os = xsdk_xsc_lower_operand(l, scal);
	if (xsc_failed(l))
		return XSC_NONE;
	if (dst == XSC_NONE)
		dst = xsdk_xsc_fresh(l);
	const XscSrc ops[2] = { xsc_src(ov, false, 0), xsc_src(os, false, xsdk_xsc_bsw(0)) };
	return xsdk_xsc_emit(l, XSC_VECTOR_MUL, ops, 2, dst, export_, 0xF, false) ? dst : XSC_NONE;
}

static int64_t dot(XscLower *l, uint32_t a, uint32_t b, int64_t dst, bool export_)
{
	const int64_t axis_b = axis_component(l, b);
	if (axis_b >= 0) {
		const XscOperand oa = xsdk_xsc_lower_operand(l, a);
		if (xsc_failed(l))
			return XSC_NONE;
		return xsdk_xsc_emit_copy(l, oa, dst, export_, false, xsdk_xsc_bsw(axis_b), 0xF, false);
	}
	const int64_t axis_a = axis_component(l, a);
	if (axis_a >= 0) {
		const XscOperand ob = xsdk_xsc_lower_operand(l, b);
		if (xsc_failed(l))
			return XSC_NONE;
		return xsdk_xsc_emit_copy(l, ob, dst, export_, false, xsdk_xsc_bsw(axis_a), 0xF, false);
	}
	const XscOperand oa = xsdk_xsc_lower_operand(l, a);
	if (xsc_failed(l))
		return XSC_NONE;
	const XscOperand ob = xsdk_xsc_lower_operand(l, b);
	if (xsc_failed(l))
		return XSC_NONE;
	const int64_t size = xsdk_xsc_vec_size(l, a);
	const int64_t opcode = size == 2 ? XSC_VECTOR_DP2_ADD : size == 3 ? XSC_VECTOR_DP3 : XSC_VECTOR_DP4;
	if (dst == XSC_NONE)
		dst = xsdk_xsc_fresh(l);
	XscSrc ops[3] = { xsc_src(oa, false, 0), xsc_src(ob, false, 0) };
	size_t count = 2;
	// dp2add adds src3: zero, as a mat2's rows do (xsc_lower_matrix.c). A dp4 would add what z and w hold.
	if (size == 2) {
		static const double zero[4] = { 0, 0, 0, 0 };
		const int64_t z = xsdk_xsc_literal_reg(l, zero);
		if (z == XSC_NONE)
			return XSC_NONE;
		ops[count++] = xsc_src(xsc_const(z), false, 0);
	}
	return xsdk_xsc_emit(l, opcode, ops, count, dst, export_, 0xF, false) ? dst : XSC_NONE;
}

// Whether b is a construct of one non-constant scalar (lower.py fdiv's scalar_b).
static bool scalar_construct(XscLower *l, uint32_t b)
{
	const XscInstr *const bi = xsdk_xsc_by_result(l->m, b);
	if (!bi || bi->opcode != SPV_COMPOSITE_CONSTRUCT)
		return false;
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(bi, &count);
	bool same = count > 0;
	for (uint32_t i = 1; i < count; i++)
		same = same && ops[i] == ops[0];
	if (!same)
		return false;
	const XscInstr *const part = xsdk_xsc_lower_result(l, ops[0]);
	return part && part->opcode != SPV_CONSTANT;
}

static int64_t fdiv(XscLower *l, uint32_t a, uint32_t b, int64_t dst, bool export_)
{
	double one;
	if (xsdk_xsc_float_const01(l, b, &one) && one == 1.0)
		return copy_of(l, a, dst, export_, false, false);
	const XscOperand oa = xsdk_xsc_lower_operand(l, a);
	if (xsc_failed(l))
		return XSC_NONE;
	const XscOperand ob = xsdk_xsc_lower_operand(l, b);
	if (xsc_failed(l))
		return XSC_NONE;
	const bool scalar_b = scalar_construct(l, b);
	if (xsc_failed(l))
		return XSC_NONE;
	if (dst == XSC_NONE)
		dst = xsdk_xsc_fresh(l);
	const int64_t size_a = xsdk_xsc_vec_size(l, a);
	const int64_t size_b = xsdk_xsc_vec_size(l, b);
	int64_t n = size_a > size_b ? size_a : size_b;
	n = n > 1 ? n : 1;
	if (scalar_b || n == 1) {
		const int64_t rb = xsdk_xsc_emit_scalar(l, XSC_SCALAR_RCP, xsdk_xsc_fresh(l), ob, 0, 0xF);
		if (rb == XSC_NONE)
			return XSC_NONE;
		const XscSrc ops[2] = { xsc_src(oa, false, 0), xsc_src(xsc_reg(rb), false, 0) };
		return xsdk_xsc_emit(l, XSC_VECTOR_MUL, ops, 2, dst, export_, 0xF, false) ? dst : XSC_NONE;
	}
	for (int64_t i = 0; i < n; i++) {
		const int64_t rb = xsdk_xsc_emit_scalar(l, XSC_SCALAR_RCP, xsdk_xsc_fresh(l), ob, i, 0xF);
		if (rb == XSC_NONE)
			return XSC_NONE;
		const XscSrc ops[2] = { xsc_src(oa, false, 0), xsc_src(xsc_reg(rb), false, 0) };
		if (!xsdk_xsc_emit(l, XSC_VECTOR_MUL, ops, 2, dst, export_, (int64_t)1 << i, false))
			return XSC_NONE;
	}
	return dst;
}

static int64_t fmod_value(XscLower *l, uint32_t a, uint32_t b, int64_t dst, bool export_)
{
	XscOperand oa = xsdk_xsc_lower_operand(l, a);
	if (xsc_failed(l))
		return XSC_NONE;
	const XscOperand ob = xsdk_xsc_lower_operand(l, b);
	if (xsc_failed(l))
		return XSC_NONE;
	const int64_t q = fdiv(l, a, b, XSC_NONE, false);
	if (q == XSC_NONE)
		return XSC_NONE;
	const int64_t fq = xsdk_xsc_fresh(l);
	const XscSrc floor_ops[2] = { xsc_src(xsc_reg(q), false, 0), xsc_src(xsc_reg(q), false, 0) };
	if (!xsdk_xsc_emit(l, XSC_VECTOR_FLOOR, floor_ops, 2, fq, false, 0xF, false))
		return XSC_NONE;
	if (dst == XSC_NONE)
		dst = xsdk_xsc_fresh(l);
	if (oa.kind == XSC_OPERAND_CONST && ob.kind == XSC_OPERAND_CONST) {
		oa = xsdk_xsc_to_reg(l, oa);
		if (xsc_failed(l))
			return XSC_NONE;
	}
	const XscSrc ops[3] = { xsc_src(ob, true, 0), xsc_src(xsc_reg(fq), false, 0), xsc_src(oa, false, 0) };
	return xsdk_xsc_emit(l, XSC_VECTOR_MAD, ops, 3, dst, export_, 0xF, false) ? dst : XSC_NONE;
}

// ---- operands (lower.py operand, scalar_origin) ----

// A scalar as a value of its own: its component, wherever it is, in every component of a fresh register, by a max of
// it with itself.
static XscOperand scalar_value(XscLower *l, uint32_t ssa)
{
	const XscOrigin origin = xsdk_xsc_scalar_origin(l, ssa);
	if (xsc_failed(l))
		return (XscOperand){ 0 };
	const int64_t swizzle = origin.comp == XSC_BROADCAST ? 0 : xsdk_xsc_bsw(origin.comp);
	const int64_t r = xsdk_xsc_fresh(l);
	const XscSrc ops[2] = { xsc_src(origin.o, false, swizzle), xsc_src(origin.o, false, swizzle) };
	return xsdk_xsc_emit(l, XSC_VECTOR_MAX, ops, 2, r, false, 0xF, false) ? xsc_reg(r) : (XscOperand){ 0 };
}

static XscOperand load_operand(XscLower *l, uint32_t ssa, const XscInstr *ins)
{
	const uint32_t src = xsdk_xsc_operand(ins, 0);
	const XscInstr *const ptr = xsdk_xsc_by_result(l->m, src);
	XscBlockAccess access;
	bool found;
	if (!xsdk_xsc_block_access(l, src, &access, &found))
		return (XscOperand){ 0 };
	if (found)
		return xsdk_xsc_block_value(l, ssa, &access);
	if (ptr && ptr->opcode == SPV_ACCESS_CHAIN)
		return scalar_value(l, ssa);
	const bool known = src < l->m->id_limit;
	if (known && l->inputs[src] != XSC_NONE)
		return xsc_reg(l->inputs[src]);
	if (known && l->local_values[src].kind != XSC_OPERAND_NONE)
		return l->local_values[src];
	if (known && l->local_store[src])
		return xsdk_xsc_lower_operand(l, l->local_store[src]);
	const XscRole *const role = xsdk_xsc_role(l->si, src);
	if (role->kind == XSC_ROLE_CONSTANT) {
		if (role->b > 1) {
			const XscMatrix *const mat = xsdk_xsc_matrix_operands(l, ssa);
			return mat ? (XscOperand){ XSC_OPERAND_MAT, 0, mat } : (XscOperand){ 0 };
		}
		return xsc_const(role->a);
	}
	xsdk_xsc_lower_refuse(l, "%s: unresolved Load %%%u", xsc_stage_name(l), (unsigned)src);
	return (XscOperand){ 0 };
}

static XscOperand composite_insert(XscLower *l, const XscInstr *ins)
{
	const XscOperand base = xsdk_xsc_to_reg(l, xsdk_xsc_lower_operand(l, xsdk_xsc_operand(ins, 1)));
	if (xsc_failed(l))
		return base;
	const int64_t r = xsdk_xsc_fresh(l);
	const XscSrc copy[2] = { xsc_src(base, false, 0), xsc_src(base, false, 0) };
	if (!xsdk_xsc_emit(l, XSC_VECTOR_MAX, copy, 2, r, false, 0xF, false))
		return (XscOperand){ 0 };
	const int64_t idx = xsdk_xsc_operand(ins, 2);
	// Python's 1 << idx has no limit; the encoder refuses any mask past 4 bits, but a shift past 62 can't be made.
	if (idx >= MASK_BIT_LIMIT) {
		xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "component %lld", (long long)idx);
		return (XscOperand){ 0 };
	}
	const XscOrigin origin = xsdk_xsc_scalar_origin(l, xsdk_xsc_operand(ins, 0));
	if (xsc_failed(l))
		return (XscOperand){ 0 };
	const int64_t swizzle = component_swizzle(origin.comp, idx);
	const XscSrc ops[2] = { xsc_src(origin.o, false, swizzle), xsc_src(origin.o, false, swizzle) };
	return xsdk_xsc_emit(l, XSC_VECTOR_MAX, ops, 2, r, false, (int64_t)1 << idx, false) ? xsc_reg(r)
																						: (XscOperand){ 0 };
}

static XscOperand vector_shuffle(XscLower *l, const XscInstr *ins)
{
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	const uint32_t n = count > 2 ? count - 2 : 0;
	bool identity = true;
	bool first_source = true;
	for (uint32_t i = 0; i < n; i++) {
		identity = identity && ops[2 + i] == i;
		first_source = first_source && ops[2 + i] < 4;
	}
	if (identity)
		return xsdk_xsc_lower_operand(l, ops[0]);
	if (!first_source || n > 4) {
		char text[TEXT_BYTES];
		size_t length = (size_t)snprintf(text, sizeof text, "[");
		for (uint32_t i = 0; i < n && length < sizeof text; i++)
			length += (size_t)snprintf(text + length, sizeof text - length, "%s%u", i ? ", " : "", ops[2 + i]);
		xsdk_xsc_lower_refuse(l, "two-source swizzle %s] (v1)", text);
		return (XscOperand){ 0 };
	}
	const XscOperand base = xsdk_xsc_lower_operand(l, ops[0]);
	if (xsc_failed(l))
		return base;
	int64_t idxs[4];
	for (uint32_t i = 0; i < n; i++)
		idxs[i] = ops[2 + i];
	const int64_t swizzle = xsdk_xsc_swz(idxs, n);
	const int64_t r = xsdk_xsc_fresh(l);
	const XscSrc src[2] = { xsc_src(base, false, swizzle), xsc_src(base, false, swizzle) };
	return xsdk_xsc_emit(l, XSC_VECTOR_MAX, src, 2, r, false, 0xF, false) ? xsc_reg(r) : (XscOperand){ 0 };
}

static XscOperand constant_operand(XscLower *l, const XscInstr *ins, uint32_t ssa)
{
	double values[4] = { 0, 0, 0, 0 };
	if (ins->opcode == SPV_CONSTANT) {
		const double v = xsdk_xsc_scalar_const(l, ssa);
		values[0] = values[1] = values[2] = values[3] = v;
	} else {
		uint32_t count;
		const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
		for (uint32_t i = 0; i < count; i++) {
			const double v = xsdk_xsc_scalar_const(l, ops[i]);
			if (xsc_failed(l))
				return (XscOperand){ 0 };
			if (i < 4)
				values[i] = v;
		}
	}
	const int64_t reg = xsdk_xsc_literal_reg(l, values);
	return reg == XSC_NONE ? (XscOperand){ 0 } : xsc_const(reg);
}

static XscOperand construct_operand(XscLower *l, const XscInstr *ins)
{
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	bool same = count > 0;
	for (uint32_t i = 1; i < count; i++)
		same = same && ops[i] == ops[0];
	if (same) {
		const XscInstr *const part = xsdk_xsc_lower_result(l, ops[0]);
		if (!part)
			return (XscOperand){ 0 };
		if (part->opcode != SPV_CONSTANT)
			return xsdk_xsc_lower_operand(l, ops[0]);
		const int64_t reg = xsdk_xsc_literal_splat(l, xsdk_xsc_scalar_const(l, ops[0]));
		return reg == XSC_NONE ? (XscOperand){ 0 } : xsc_const(reg);
	}
	const int64_t r = xsdk_xsc_fresh(l);
	return xsdk_xsc_emit_construct(l, ins, r, false) ? xsc_reg(r) : (XscOperand){ 0 };
}

static XscOperand from_reg(int64_t reg)
{
	return reg == XSC_NONE ? (XscOperand){ 0 } : xsc_reg(reg);
}

static XscOperand matrix_operand(XscLower *l, uint32_t ssa)
{
	const XscMatrix *const mat = xsdk_xsc_matrix_operands(l, ssa);
	return mat ? (XscOperand){ XSC_OPERAND_MAT, 0, mat } : (XscOperand){ 0 };
}

// The value ops operand() resolves through the arithmetic it emits.
static XscOperand value_operand(XscLower *l, uint32_t ssa, const XscInstr *ins)
{
	const uint32_t op = ins->opcode;
	const uint32_t a = xsdk_xsc_operand(ins, 0);
	const uint32_t b = xsdk_xsc_operand(ins, 1);
	switch (op) {
	case SPV_MATRIX_TIMES_VECTOR:
		return from_reg(xsdk_xsc_emit_mat_vec(l, a, b, XSC_NONE, false));
	case SPV_VECTOR_TIMES_MATRIX:
		return from_reg(xsdk_xsc_emit_vec_mat(l, a, b, XSC_NONE, false));
	case SPV_MATRIX_TIMES_MATRIX:
	case SPV_TRANSPOSE:
		return matrix_operand(l, ssa);
	case SPV_F_ADD:
	case SPV_F_SUB:
	case SPV_F_MUL:
		return from_reg(xsdk_xsc_binop(l, op, a, b, XSC_NONE, false, false));
	case SPV_VECTOR_TIMES_SCALAR:
		return from_reg(vts(l, a, b, XSC_NONE, false));
	case SPV_DOT:
		return from_reg(dot(l, a, b, XSC_NONE, false));
	case SPV_F_DIV:
		return from_reg(fdiv(l, a, b, XSC_NONE, false));
	case SPV_F_MOD:
		return from_reg(fmod_value(l, a, b, XSC_NONE, false));
	case SPV_CONSTANT:
	case SPV_CONSTANT_COMPOSITE:
		return constant_operand(l, ins, ssa);
	case SPV_COMPOSITE_CONSTRUCT:
		return construct_operand(l, ins);
	case SPV_EXT_INST:
		if (b == GLSL_MATRIX_INVERSE)
			return matrix_operand(l, ssa);
		return from_reg(xsdk_xsc_extinst(l, ins, XSC_NONE, false));
	case SPV_CONVERT_S_TO_F:
		return xsdk_xsc_int_operand(l, a);
	case SPV_COMPOSITE_EXTRACT:
		// A float out of a vector that's a value rather than a variable, as glslang makes of `vec3(x).r`: its
		// component, as a scalar loaded from a block is. Anything else extracted stays refused below.
		if (result_kind(l, ssa) == XSC_TYPE_FLOAT)
			return scalar_value(l, ssa);
		break;
	default:
		break;
	}
	char name[16];
	if (op >= OP_SHIFT_RIGHT_LOGICAL && op <= OP_NOT)
		xsdk_xsc_lower_refuse(l,
			"%s: bitwise/shift operators have no Xenos float-ALU equivalent (the GPU has no bit ops); restructure "
			"with arithmetic, or precompute on the host",
			xsdk_xsc_op_name(op, name, sizeof name));
	else
		xsdk_xsc_lower_refuse(l, "v1 %s: unhandled op %s", xsc_stage_name(l), xsdk_xsc_op_name(op, name, sizeof name));
	return (XscOperand){ 0 };
}

// A comparison or boolean op's value, 1.0 where it holds and 0.0 elsewhere.
static XscOperand boolean_operand(XscLower *l, const XscInstr *ins)
{
	const uint32_t op = ins->opcode;
	int64_t xop;
	bool swap;
	const int64_t r = xsdk_xsc_fresh(l);
	bool ok;
	if (compare_op(op, &xop, &swap))
		ok = compare_value(l, ins, r, false);
	else if (op == SPV_SELECT)
		ok = select_value(l, ins, r, false);
	else if (op == OP_ANY || op == OP_ALL)
		ok = reduce_bool(l, ins, r, false, op == OP_ALL);
	else if (op == OP_LOGICAL_NOT)
		ok = logical_not_value(l, ins, r, false);
	else
		ok = logical_binop_value(l, ins, r, false);
	return ok ? xsc_reg(r) : (XscOperand){ 0 };
}

static bool boolean_op(uint32_t op)
{
	int64_t xop;
	bool swap;
	return compare_op(op, &xop, &swap) || op == SPV_SELECT || op == OP_ANY || op == OP_ALL || op == OP_LOGICAL_NOT ||
		op == OP_LOGICAL_OR || op == OP_LOGICAL_AND;
}

static XscOperand regs_by_id(XscLower *l, const int64_t *regs, uint32_t ssa, const char *message)
{
	if (ssa >= l->m->id_limit || regs[ssa] == XSC_NONE) {
		xsdk_xsc_lower_refuse(l, "%s", message);
		return (XscOperand){ 0 };
	}
	return xsc_reg(regs[ssa]);
}

static XscOperand operand_of(XscLower *l, uint32_t ssa, const XscInstr *ins)
{
	const uint32_t op = ins->opcode;
	if (op == SPV_SELECT && xsdk_xsc_is_merge(l, ssa))
		return xsdk_xsc_merge_operand(l, ssa);
	if (op == SPV_CONSTANT_TRUE || op == SPV_CONSTANT_FALSE) {
		const int64_t reg = xsdk_xsc_literal_splat(l, op == SPV_CONSTANT_TRUE ? 1.0 : 0.0);
		return reg == XSC_NONE ? (XscOperand){ 0 } : xsc_const(reg);
	}
	if (boolean_op(op))
		return boolean_operand(l, ins);
	const XscTypeKind kind = result_kind(l, ssa);
	if (kind == XSC_TYPE_INT)
		return xsdk_xsc_int_operand(l, ssa);
	double v;
	if (kind == XSC_TYPE_FLOAT && xsdk_xsc_float_const(l, ssa, &v)) {
		const int64_t reg = xsdk_xsc_literal_splat(l, v);
		return reg == XSC_NONE ? (XscOperand){ 0 } : xsc_const(reg);
	}
	if (op == SPV_LOAD)
		return load_operand(l, ssa, ins);
	if (op == SPV_IMAGE_SAMPLE_IMPLICIT_LOD || op == SPV_IMAGE_SAMPLE_EXPLICIT_LOD ||
		op == SPV_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD || op == SPV_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD)
		return regs_by_id(l, l->sample_reg, ssa, "ImageSample was not gathered before lowering");
	if (op == SPV_COMPOSITE_INSERT)
		return composite_insert(l, ins);
	if (op == SPV_VECTOR_SHUFFLE)
		return vector_shuffle(l, ins);
	if (op >= SPV_DPDX && op <= SPV_FWIDTH)
		return regs_by_id(l, l->deriv_reg, ssa, "derivative was not gathered before lowering");
	return value_operand(l, ssa, ins);
}

XscOperand xsdk_xsc_lower_operand(XscLower *l, uint32_t ssa)
{
	if (ssa < l->m->id_limit && l->op_cache[ssa].kind != XSC_OPERAND_NONE)
		return l->op_cache[ssa];
	const XscInstr *const ins = xsdk_xsc_lower_result(l, ssa);
	if (!ins)
		return (XscOperand){ 0 };
	if (!xsdk_xsc_enter(l))
		return (XscOperand){ 0 };
	const XscOperand res = operand_of(l, ssa, ins);
	l->depth--;
	if (!xsc_failed(l))
		l->op_cache[ssa] = res;
	return res;
}

static XscOrigin origin(XscOperand o, int64_t comp)
{
	return (XscOrigin){ o, comp };
}

static XscOrigin load_origin(XscLower *l, uint32_t ssa, const XscInstr *ins, bool *resolved)
{
	*resolved = true;
	const uint32_t src = xsdk_xsc_operand(ins, 0);
	const XscInstr *const ptr = xsdk_xsc_by_result(l->m, src);
	XscBlockAccess access;
	bool found;
	if (!xsdk_xsc_block_access(l, src, &access, &found))
		return origin((XscOperand){ 0 }, 0);
	if (found) {
		const XscType *const t = xsdk_xsc_type(l->m, l->types, access.tid);
		if (!t) {
			xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "%u", (unsigned)access.tid);
			return origin((XscOperand){ 0 }, 0);
		}
		if (t->kind != XSC_TYPE_VECTOR)
			return origin(xsdk_xsc_block_register(l, &access, 0), access.comp);
		return origin(xsdk_xsc_block_value(l, ssa, &access), XSC_BROADCAST);
	}
	const uint32_t limit = l->m->id_limit;
	if (ptr && ptr->opcode == SPV_ACCESS_CHAIN) {
		const uint32_t base = xsdk_xsc_operand(ptr, 0);
		int64_t comp = 0;
		if (!const_val(l, xsdk_xsc_operand(ptr, 1), &comp))
			return origin((XscOperand){ 0 }, 0);
		if (base < limit && l->local_values[base].kind != XSC_OPERAND_NONE)
			return origin(l->local_values[base], comp);
		if (base < limit && l->local_store[base])
			return origin(xsdk_xsc_lower_operand(l, l->local_store[base]), comp);
		if (base < limit && l->inputs[base] != XSC_NONE)
			return origin(xsc_reg(l->inputs[base]), comp);
		const XscRole *const role = xsdk_xsc_role(l->si, base);
		if (role->kind == XSC_ROLE_CONSTANT)
			return origin(xsc_const(role->a), comp);
	}
	if (src < limit && l->local_values[src].kind != XSC_OPERAND_NONE)
		return origin(l->local_values[src], XSC_BROADCAST);
	if (src < limit && l->local_store[src])
		return origin(xsdk_xsc_lower_operand(l, l->local_store[src]), XSC_BROADCAST);
	const XscRole *const role = xsdk_xsc_role(l->si, src);
	if (role->kind == XSC_ROLE_CONSTANT)
		return origin(xsc_const(role->a), 0);
	if (src < limit && l->inputs[src] != XSC_NONE)
		return origin(xsc_reg(l->inputs[src]), 0);
	*resolved = false;
	return origin((XscOperand){ 0 }, 0);
}

static XscOrigin scalar_origin_of(XscLower *l, uint32_t ssa)
{
	const XscInstr *const ins = xsdk_xsc_lower_result(l, ssa);
	if (!ins)
		return origin((XscOperand){ 0 }, 0);
	const uint32_t op = ins->opcode;
	if (op == SPV_COMPOSITE_EXTRACT)
		return origin(xsdk_xsc_lower_operand(l, xsdk_xsc_operand(ins, 0)), xsdk_xsc_operand(ins, 1));
	if (op == SPV_LOAD) {
		bool resolved;
		const XscOrigin found = load_origin(l, ssa, ins, &resolved);
		if (resolved || xsc_failed(l))
			return found;
	}
	const bool broadcast = op == SPV_DOT || op == SPV_CONSTANT || op == SPV_CONSTANT_TRUE || op == SPV_CONSTANT_FALSE ||
		op == SPV_F_ADD || op == SPV_F_SUB || op == SPV_F_MUL || op == SPV_F_DIV || op == SPV_F_MOD ||
		op == SPV_EXT_INST || boolean_op(op);
	if (broadcast)
		return origin(xsdk_xsc_lower_operand(l, ssa), XSC_BROADCAST);
	xsdk_xsc_lower_refuse(l, "cannot resolve scalar %%%u", (unsigned)ssa);
	return origin((XscOperand){ 0 }, 0);
}

XscOrigin xsdk_xsc_scalar_origin(XscLower *l, uint32_t ssa)
{
	if (!xsdk_xsc_enter(l))
		return origin((XscOperand){ 0 }, 0);
	const XscOrigin res = scalar_origin_of(l, ssa);
	l->depth--;
	return res;
}

// ---- constructs and values (lower.py emit_construct, emit_value) ----

typedef struct {
	bool literal;
	double value;
	XscOrigin origin;
} Part;

bool xsdk_xsc_emit_construct(XscLower *l, const XscInstr *ins, int64_t dst_reg, bool export_)
{
	uint32_t n;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &n);
	Part *const parts = xsdk_xsc_alloc(l->x, ((size_t)n + 1) * sizeof *parts);
	if (!parts)
		return false;
	for (uint32_t i = 0; i < n; i++) {
		if (export_ && xsdk_xsc_float_const01(l, ops[i], &parts[i].value)) {
			parts[i].literal = true;
			continue;
		}
		parts[i].origin = xsdk_xsc_scalar_origin(l, ops[i]);
		if (xsc_failed(l))
			return false;
	}
	if (export_) {
		int64_t zero_mask = 0, one_mask = 0;
		for (uint32_t i = 0; i < n; i++) {
			if (parts[i].literal && is_zero(parts[i].value))
				zero_mask |= (int64_t)1 << i;
			if (parts[i].literal && is_one(parts[i].value))
				one_mask |= (int64_t)1 << i;
		}
		if (!xsdk_xsc_emit_export_constants(l, dst_reg, zero_mask, one_mask))
			return false;
	}
	uint32_t j = 0;
	while (j < n) {
		if (parts[j].literal) {
			j++;
			continue;
		}
		const XscOperand op_j = parts[j].origin.o;
		uint32_t k = j;
		while (k < n && !parts[k].literal && operand_equal(parts[k].origin.o, op_j) &&
			(parts[k].origin.comp == (int64_t)k || parts[k].origin.comp == XSC_BROADCAST))
			k++;
		if (k > j) {
			int64_t mask = 0;
			for (uint32_t t = j; t < k; t++)
				mask |= (int64_t)1 << t;
			const XscSrc copy[2] = { xsc_src(op_j, false, 0), xsc_src(op_j, false, 0) };
			if (!xsdk_xsc_emit(l, XSC_VECTOR_MAX, copy, 2, dst_reg, export_, mask, false))
				return false;
			j = k;
			continue;
		}
		const int64_t swizzle = component_swizzle(parts[j].origin.comp, j);
		const XscSrc one[2] = { xsc_src(op_j, false, swizzle), xsc_src(op_j, false, swizzle) };
		if (!xsdk_xsc_emit(l, XSC_VECTOR_MAX, one, 2, dst_reg, export_, (int64_t)1 << j, false))
			return false;
		j++;
	}
	return true;
}

static bool export_constant_vector(XscLower *l, uint32_t ssa, int64_t dst, bool *done)
{
	*done = false;
	double vals[CONST_VECTOR_CAPACITY];
	const int count = const_vector(l, ssa, vals, CONST_VECTOR_CAPACITY);
	if (count < 0 || count > CONST_VECTOR_CAPACITY)
		return true;
	int64_t zero_mask = 0, one_mask = 0;
	for (int i = 0; i < count; i++) {
		if (!is_zero(vals[i]) && !is_one(vals[i]))
			return true;
		if (is_zero(vals[i]))
			zero_mask |= (int64_t)1 << i;
		else
			one_mask |= (int64_t)1 << i;
	}
	*done = true;
	return xsdk_xsc_emit_export_constants(l, dst, zero_mask, one_mask);
}

static bool emit_arithmetic_value(XscLower *l, const XscInstr *vi, int64_t dst, bool export_)
{
	const uint32_t a = xsdk_xsc_operand(vi, 0);
	const uint32_t b = xsdk_xsc_operand(vi, 1);
	switch (vi->opcode) {
	case SPV_F_ADD:
	case SPV_F_SUB:
	case SPV_F_MUL:
		return xsdk_xsc_binop(l, vi->opcode, a, b, dst, export_, false) != XSC_NONE || !xsc_failed(l);
	case SPV_VECTOR_TIMES_SCALAR:
		return vts(l, a, b, dst, export_) != XSC_NONE || !xsc_failed(l);
	case SPV_DOT:
		return dot(l, a, b, dst, export_) != XSC_NONE || !xsc_failed(l);
	case SPV_F_DIV:
		return fdiv(l, a, b, dst, export_) != XSC_NONE || !xsc_failed(l);
	case SPV_F_MOD:
		return fmod_value(l, a, b, dst, export_) != XSC_NONE || !xsc_failed(l);
	default:
		return xsdk_xsc_extinst(l, vi, dst, export_) != XSC_NONE || !xsc_failed(l);
	}
}

bool xsdk_xsc_emit_value(XscLower *l, uint32_t ssa, int64_t dst, bool export_, bool export_position)
{
	const XscInstr *const vi = xsdk_xsc_lower_result(l, ssa);
	if (!vi)
		return false;
	const uint32_t op = vi->opcode;
	if (export_ && (op == SPV_CONSTANT || op == SPV_CONSTANT_COMPOSITE)) {
		bool done;
		if (!export_constant_vector(l, ssa, dst, &done))
			return false;
		if (done)
			return true;
	}
	const uint32_t a = xsdk_xsc_operand(vi, 0);
	const uint32_t b = xsdk_xsc_operand(vi, 1);
	if (op == SPV_SELECT && xsdk_xsc_is_merge(l, ssa))
		return xsdk_xsc_emit_merge(l, ssa, dst, export_);
	if (op == SPV_COMPOSITE_CONSTRUCT)
		return xsdk_xsc_emit_construct(l, vi, dst, export_);
	if (op == SPV_MATRIX_TIMES_VECTOR)
		return xsdk_xsc_emit_mat_vec(l, a, b, dst, export_position) != XSC_NONE || !xsc_failed(l);
	if (op == SPV_VECTOR_TIMES_MATRIX)
		return xsdk_xsc_emit_vec_mat(l, a, b, dst, export_position) != XSC_NONE || !xsc_failed(l);
	if (op == SPV_MATRIX_TIMES_MATRIX)
		return xsdk_xsc_lower_refuse(l, "cannot export a matrix value");
	if (op == SPV_F_ADD || op == SPV_F_SUB || op == SPV_F_MUL || op == SPV_VECTOR_TIMES_SCALAR || op == SPV_DOT ||
		op == SPV_F_DIV || op == SPV_F_MOD || op == SPV_EXT_INST)
		return emit_arithmetic_value(l, vi, dst, export_);
	// A constant goes straight to the export, as xsdk's constant shaders do (tools/gen_constant_shaders.py).
	const XscOperand r = xsdk_xsc_lower_operand(l, ssa);
	if (xsc_failed(l))
		return false;
	const XscSrc ops[2] = { xsc_src(r, false, 0), xsc_src(r, false, 0) };
	return xsdk_xsc_emit(l, XSC_VECTOR_MAX, ops, 2, dst, export_, 0xF, false);
}
