#include "xsc_lower.h"

// Uniform and push constant block members read through their registers, as lower.py's block_access and its helpers do.

enum {
	REGISTER_BYTES = 16,
	DEFAULT_STRIDE = 16,
	MEMBER_BYTES = 4,
};

// Python's // and %, rounding toward minus infinity, as an index below an array's start needs.
static int64_t floor_div(int64_t a, int64_t b)
{
	const int64_t q = a / b;
	return a % b != 0 && (a < 0) != (b < 0) ? q - 1 : q;
}

static int64_t floor_mod(int64_t a, int64_t b)
{
	return a - floor_div(a, b) * b;
}

static int64_t member_arg(const XscLower *l, const XscBlockAccess *a, uint32_t decoration, int64_t fallback)
{
	if (a->member_struct == XSC_NONE)
		return fallback;
	return xsdk_xsc_decoration_arg(
		xsdk_xsc_member_decoration(l->m, (uint32_t)a->member_struct, (uint32_t)a->member_index, decoration), fallback);
}

static bool row_major(const XscLower *l, const XscBlockAccess *a)
{
	return a->member_struct != XSC_NONE &&
		xsdk_xsc_member_decoration(l->m, (uint32_t)a->member_struct, (uint32_t)a->member_index, SPV_DEC_ROW_MAJOR);
}

// A run-time index as its run-time part (0 for none) plus a constant that can go in the register number.
static bool split_index(XscLower *l, uint32_t ssa, uint32_t *part, int64_t *k);

static bool split_index_at(XscLower *l, uint32_t ssa, uint32_t *part, int64_t *k)
{
	int64_t c;
	if (xsdk_xsc_int_const(l, ssa, &c)) {
		*part = 0;
		*k = c;
		return true;
	}
	const XscInstr *const ins = xsdk_xsc_lower_result(l, ssa);
	if (!ins || xsc_failed(l))
		return false;
	const uint32_t a = xsdk_xsc_operand(ins, 0);
	const uint32_t b = xsdk_xsc_operand(ins, 1);
	if (ins->opcode == SPV_LOAD && a < l->m->id_limit && l->local_store[a])
		return split_index(l, l->local_store[a], part, k);
	if (ins->opcode == SPV_I_ADD || ins->opcode == SPV_I_SUB) {
		int64_t cb, ca;
		if (xsdk_xsc_int_const(l, b, &cb)) {
			if (!split_index(l, a, part, k))
				return false;
			*k += ins->opcode == SPV_I_ADD ? cb : -cb;
			return true;
		}
		if (xsdk_xsc_int_const(l, a, &ca) && ins->opcode == SPV_I_ADD) {
			if (!split_index(l, b, part, k))
				return false;
			*k += ca;
			return true;
		}
	}
	*part = ssa;
	*k = 0;
	return !xsc_failed(l);
}

static bool split_index(XscLower *l, uint32_t ssa, uint32_t *part, int64_t *k)
{
	if (!xsdk_xsc_enter(l))
		return false;
	const bool ok = split_index_at(l, ssa, part, k);
	l->depth--;
	return ok;
}

static bool run_time_index(XscLower *l, XscBlockAccess *access, const XscType *t, uint32_t idx_id, int64_t *idx)
{
	if (t->kind != XSC_TYPE_ARRAY || access->dynamic)
		return xsdk_xsc_lower_refuse(l, "a run-time index other than one into a block's array (v1)");
	const XscDecoration *const d = xsdk_xsc_decoration(l->m, access->tid, SPV_DEC_ARRAY_STRIDE);
	const int64_t stride = xsdk_xsc_decoration_arg(d, DEFAULT_STRIDE);
	if (stride % REGISTER_BYTES)
		return xsdk_xsc_lower_refuse(l, "a run-time index into an array not on 16-byte lines (v1)");
	uint32_t part;
	if (!split_index(l, idx_id, &part, idx))
		return false;
	access->dynamic = true;
	access->dyn_part = part;
	access->dyn_scale = stride / REGISTER_BYTES;
	return true;
}

// One index of the access chain: the bytes it moves the offset by, and the type it reaches.
static bool step_into(XscLower *l, XscBlockAccess *access, uint32_t idx_id, int64_t *offset)
{
	int64_t idx;
	const bool known = xsdk_xsc_int_const(l, idx_id, &idx);
	if (xsc_failed(l))
		return false;
	const XscType *const t = xsdk_xsc_type(l->m, l->types, access->tid);
	if (!t)
		return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "%u", (unsigned)access->tid);
	if (!known && !run_time_index(l, access, t, idx_id, &idx))
		return false;
	switch (t->kind) {
	case XSC_TYPE_STRUCT:
		if (idx < 0 || idx >= t->member_count)
			return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "member %lld past the struct's", (long long)idx);
		access->member_struct = access->tid;
		access->member_index = idx;
		*offset += member_arg(l, access, SPV_DEC_OFFSET, REGISTER_BYTES * idx);
		access->tid = t->members[idx];
		return true;
	case XSC_TYPE_MATRIX:
		if (row_major(l, access))
			return xsdk_xsc_lower_refuse(l, "a column of a row-major block matrix (v1)");
		*offset += idx * member_arg(l, access, SPV_DEC_MATRIX_STRIDE, DEFAULT_STRIDE);
		access->tid = t->comp;
		access->member_struct = XSC_NONE;
		return true;
	case XSC_TYPE_VECTOR:
		*offset += MEMBER_BYTES * idx;
		access->tid = t->comp;
		return true;
	case XSC_TYPE_ARRAY:
		*offset +=
			idx * xsdk_xsc_decoration_arg(xsdk_xsc_decoration(l->m, access->tid, SPV_DEC_ARRAY_STRIDE), DEFAULT_STRIDE);
		access->tid = t->comp;
		return true;
	default:
		return xsdk_xsc_lower_refuse(l, "index into a block %s (v1)", xsdk_xsc_type_kind_name(t->kind));
	}
}

bool xsdk_xsc_block_access(XscLower *l, uint32_t ptr_id, XscBlockAccess *access, bool *found)
{
	*found = false;
	const XscInstr *const ptr = xsdk_xsc_by_result(l->m, ptr_id);
	if (!ptr || (ptr->opcode != SPV_ACCESS_CHAIN && ptr->opcode != SPV_IN_BOUNDS_ACCESS_CHAIN))
		return true;
	const XscRole *const role = xsdk_xsc_role(l->si, xsdk_xsc_operand(ptr, 0));
	if (role->kind != XSC_ROLE_BLOCK)
		return true;
	const XscBlock *const block = &l->si->blocks[role->a];
	*access = (XscBlockAccess){ .tid = block->struct_type, .member_struct = XSC_NONE };
	int64_t offset = 0;
	uint32_t count;
	const uint32_t *const indices = xsdk_xsc_operands(ptr, &count);
	for (uint32_t i = 1; i < count; i++) {
		if (!step_into(l, access, indices[i], &offset))
			return false;
	}
	access->reg = block->reg + floor_div(offset, REGISTER_BYTES);
	access->comp = floor_mod(offset, REGISTER_BYTES) / MEMBER_BYTES;
	*found = true;
	return true;
}

// a0 = the run-time part times the registers an element; it's -256 to 255 (xenia:src/xenia/gpu/ucode.h:1138-1150).
static bool set_a0(XscLower *l, uint32_t part, int64_t scale)
{
	if (l->a0_set && l->a0_part == part && l->a0_scale == scale)
		return true;
	const XscInstr *const ins = xsdk_xsc_lower_result(l, part);
	if (!ins)
		return false;
	XscOrigin origin;
	int64_t sop;
	if (ins->opcode == SPV_CONVERT_F_TO_S && scale == 1) {
		origin = xsdk_xsc_scalar_origin(l, xsdk_xsc_operand(ins, 0));
		sop = XSC_SCALAR_MAX_ASF;
	} else {
		origin = (XscOrigin){ xsdk_xsc_int_operand(l, part), 0 };
		sop = XSC_SCALAR_MAX_AS;
	}
	if (xsc_failed(l))
		return false;
	if (sop == XSC_SCALAR_MAX_AS && scale != 1) {
		const int64_t r = xsdk_xsc_fresh(l);
		const int64_t k = xsdk_xsc_literal_splat(l, (double)scale);
		if (k == XSC_NONE || !xsc_emit2(l, XSC_VECTOR_MUL, xsc_plain(origin.o), xsc_plain(xsc_const(k)), r, false, 0xF))
			return false;
		origin.o = xsc_reg(r);
	}
	if (origin.o.kind != XSC_OPERAND_REG && origin.o.kind != XSC_OPERAND_CONST)
		return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "a matrix as an index");
	// The scalar op reads src3's w and x (xenia:src/xenia/gpu/shader_translator.cc:1246).
	XscAlu f = xsdk_xsc_template_mov();
	f.vector_opcode = XSC_VECTOR_MAX;
	f.vector_dest = 0;
	f.vector_write_mask = 0;
	f.vector_clamp = false;
	f.scalar_opcode = sop;
	f.scalar_dest = 0;
	f.scalar_write_mask = 0;
	f.scalar_clamp = false;
	f.is_export = false;
	f.src_reg[2] = origin.o.value;
	f.src_is_temp[2] = origin.o.kind == XSC_OPERAND_REG;
	f.src_swizzle[2] = xsdk_xsc_bsw(origin.comp == XSC_BROADCAST ? 0 : origin.comp);
	f.src_negate[2] = false;
	if (!xsdk_xsc_emit_alu(l, &f))
		return false;
	l->a0_set = true;
	l->a0_part = part;
	l->a0_scale = scale;
	return true;
}

// The constant itself, or at a run-time index c[a0 + register] copied to a temp, so a later index can't change it.
XscOperand xsdk_xsc_block_register(XscLower *l, const XscBlockAccess *access, int64_t extra)
{
	if (!access->dynamic)
		return xsc_const(access->reg + extra);
	if (!set_a0(l, access->dyn_part, access->dyn_scale))
		return (XscOperand){ 0 };
	const int64_t r = xsdk_xsc_fresh(l);
	XscAlu f = xsdk_xsc_template_mov();
	f.vector_opcode = XSC_VECTOR_MAX;
	f.vector_dest = r;
	f.is_export = false;
	f.vector_write_mask = 0xF;
	f.vector_clamp = false;
	f.is_const_address_register_relative = true;
	for (int n = 0; n < 2; n++) {
		f.src_const_rel[n] = true;
		f.src_reg[n] = access->reg + extra;
		f.src_is_temp[n] = false;
		f.src_swizzle[n] = 0;
		f.src_negate[n] = false;
	}
	return xsdk_xsc_emit_alu(l, &f) ? xsc_reg(r) : (XscOperand){ 0 };
}

XscOperand xsdk_xsc_block_value(XscLower *l, uint32_t ssa, const XscBlockAccess *access)
{
	const XscType *const t = xsdk_xsc_type(l->m, l->types, access->tid);
	if (!t) {
		xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "%u", (unsigned)access->tid);
		return (XscOperand){ 0 };
	}
	if (t->kind == XSC_TYPE_MATRIX) {
		const XscMatrix *const mat = xsdk_xsc_matrix_operands(l, ssa);
		return mat ? xsc_mat(mat) : (XscOperand){ 0 };
	}
	const int64_t n = t->kind == XSC_TYPE_VECTOR ? t->count : 1;
	if (t->kind == XSC_TYPE_VECTOR && access->comp == 0)
		return xsdk_xsc_block_register(l, access, 0);
	if (access->comp + n > 4) {
		xsdk_xsc_lower_refuse(l, "a block member across registers (v1)");
		return (XscOperand){ 0 };
	}
	int64_t idxs[4];
	for (int64_t i = 0; i < n; i++)
		idxs[i] = access->comp + i;
	const int64_t swizzle = n == 1 ? xsdk_xsc_bsw(access->comp) : xsdk_xsc_swz(idxs, (size_t)n);
	const XscOperand src = xsdk_xsc_block_register(l, access, 0);
	if (xsc_failed(l))
		return src;
	const int64_t r = xsdk_xsc_fresh(l);
	const XscSrc s = xsc_src(src, false, swizzle);
	return xsc_emit2(l, XSC_VECTOR_MAX, s, s, r, false, 0xF) ? xsc_reg(r) : (XscOperand){ 0 };
}

// A block matrix's registers: its columns, marked so, unless it's row-major.
const XscMatrix *xsdk_xsc_block_matrix(XscLower *l, uint32_t ssa, const XscBlockAccess *access)
{
	const XscType *const t = xsdk_xsc_type(l->m, l->types, access->tid);
	if (!t) {
		xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "%u", (unsigned)access->tid);
		return NULL;
	}
	const XscType *const col = xsdk_xsc_type(l->m, l->types, t->comp);
	const int64_t rows = col && col->kind == XSC_TYPE_VECTOR ? col->count : 1;
	if (access->comp || member_arg(l, access, SPV_DEC_MATRIX_STRIDE, DEFAULT_STRIDE) != REGISTER_BYTES) {
		xsdk_xsc_lower_refuse(l, "a block matrix not on 16-byte lines (v1)");
		return NULL;
	}
	const bool by_rows = row_major(l, access);
	const int64_t count = by_rows ? rows : t->count;
	if (!by_rows)
		l->col_major[ssa] = true;
	if (count > XSC_MATRIX_ROWS) {
		xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "a matrix of %lld registers", (long long)count);
		return NULL;
	}
	XscMatrix *const mat = xsdk_xsc_alloc(l->x, sizeof *mat);
	if (!mat)
		return NULL;
	for (int64_t i = 0; i < count; i++) {
		mat->rows[i] = xsdk_xsc_block_register(l, access, i);
		if (xsc_failed(l))
			return NULL;
	}
	mat->count = (size_t)count;
	return mat;
}
