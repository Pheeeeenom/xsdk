#include "xsc_lower.h"

// Matrix values as a register a row, or a column where the matrix is marked so, as lower.py's ValueLowerer lowers them.

enum {
	GLSL_MATRIX_INVERSE = 34,
};

static int64_t done(bool ok, int64_t reg)
{
	return ok ? reg : XSC_NONE;
}

static bool square(size_t n)
{
	return n >= 2 && n <= XSC_MATRIX_ROWS;
}

static XscMatrix *new_matrix(XscLower *l, size_t count)
{
	XscMatrix *const mat = xsdk_xsc_alloc(l->x, sizeof *mat);
	if (mat)
		mat->count = count;
	return mat;
}

static bool rows_in_registers(XscLower *l, const XscMatrix *in, XscOperand *rows)
{
	for (size_t i = 0; i < in->count; i++) {
		rows[i] = xsdk_xsc_to_reg(l, in->rows[i]);
		if (xsc_failed(l))
			return false;
	}
	return true;
}

static bool is_col_major(XscLower *l, uint32_t mat, bool *col)
{
	if (!xsdk_xsc_matrix_operands(l, mat))
		return false;
	*col = l->col_major[mat];
	return true;
}

// The constant registers a uniform matrix sits in, a block's, or a local's value.
static const XscMatrix *load_matrix(XscLower *l, uint32_t ssa, const XscInstr *ins)
{
	const uint32_t src = xsdk_xsc_operand(ins, 0);
	XscBlockAccess access;
	bool found;
	if (!xsdk_xsc_block_access(l, src, &access, &found))
		return NULL;
	if (found)
		return xsdk_xsc_block_matrix(l, ssa, &access);
	if (src < l->m->id_limit && l->local_store[src])
		return xsdk_xsc_matrix_operands(l, l->local_store[src]);
	const XscRole *const role = xsdk_xsc_role(l->si, src);
	if (role->kind != XSC_ROLE_CONSTANT) {
		xsdk_xsc_lower_refuse(l, "matrix operand must be a uniform Load");
		return NULL;
	}
	if (!square((size_t)role->b)) {
		xsdk_xsc_lower_refuse(l, "matrix operand expects mat2/3/4, got %lld regs", (long long)role->b);
		return NULL;
	}
	XscMatrix *const mat = new_matrix(l, (size_t)role->b);
	for (size_t i = 0; mat && i < mat->count; i++)
		mat->rows[i] = xsc_const(role->a + (int64_t)i);
	return mat;
}

// Row i of the result is column i of M: component i of every row.
static const XscMatrix *emit_transpose(XscLower *l, uint32_t mat_ssa)
{
	const XscMatrix *const in = xsdk_xsc_matrix_operands(l, mat_ssa);
	if (!in)
		return NULL;
	const size_t n = in->count;
	if (!square(n)) {
		xsdk_xsc_lower_refuse(l, "transpose expects mat2/3/4, got %zu", n);
		return NULL;
	}
	XscOperand rows[XSC_MATRIX_ROWS];
	XscMatrix *const out = new_matrix(l, n);
	if (!rows_in_registers(l, in, rows) || !out)
		return NULL;
	for (size_t i = 0; i < n; i++) {
		const int64_t r = xsdk_xsc_fresh(l);
		for (size_t j = 0; j < n; j++) {
			const XscSrc s = xsc_src(rows[j], false, (((int64_t)i - (int64_t)j) & 3) << (2 * j));
			if (!xsc_emit2(l, XSC_VECTOR_MAX, s, s, r, false, (int64_t)1 << j))
				return NULL;
		}
		out->rows[i] = xsc_reg(r);
	}
	return out;
}

int64_t xsdk_xsc_cross(XscLower *l, XscOperand a, XscOperand b)
{
	static const int64_t yzx_idxs[] = { 1, 2, 0, 0 };
	static const int64_t zxy_idxs[] = { 2, 0, 1, 0 };
	const int64_t yzx = xsdk_xsc_swz(yzx_idxs, 4);
	const int64_t zxy = xsdk_xsc_swz(zxy_idxs, 4);
	const int64_t tc = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_MUL, xsc_src(a, false, zxy), xsc_src(b, false, yzx), tc, false, 0xF))
		return XSC_NONE;
	const int64_t out = xsdk_xsc_fresh(l);
	const XscSrc minus_tc = xsc_negated(xsc_reg(tc));
	return done(
		xsc_emit3(l, XSC_VECTOR_MAD, xsc_src(a, false, yzx), xsc_src(b, false, zxy), minus_tc, out, false, 0xF), out);
}

// mat2: ad - bc; mat3: row0 . cross(row1, row2). A register with it in every component.
int64_t xsdk_xsc_emit_determinant(XscLower *l, uint32_t mat_ssa)
{
	const XscMatrix *const in = xsdk_xsc_matrix_operands(l, mat_ssa);
	XscOperand rows[XSC_MATRIX_ROWS];
	if (!in || !rows_in_registers(l, in, rows))
		return XSC_NONE;
	if (in->count == 2) {
		static const int64_t yx_idxs[] = { 1, 0, 0, 0 };
		const int64_t prod = xsdk_xsc_fresh(l);
		const XscSrc yx = xsc_src(rows[1], false, xsdk_xsc_swz(yx_idxs, 4));
		if (!xsc_emit2(l, XSC_VECTOR_MUL, xsc_plain(rows[0]), yx, prod, false, 0xF))
			return XSC_NONE;
		const int64_t r = xsdk_xsc_fresh(l);
		const XscSrc x = xsc_src(xsc_reg(prod), false, xsdk_xsc_bsw(0));
		const XscSrc minus_y = xsc_src(xsc_reg(prod), true, xsdk_xsc_bsw(1));
		return done(xsc_emit2(l, XSC_VECTOR_ADD, x, minus_y, r, false, 0xF), r);
	}
	if (in->count == 3) {
		const int64_t cr = xsdk_xsc_cross(l, rows[1], rows[2]);
		if (cr == XSC_NONE)
			return XSC_NONE;
		const int64_t r = xsdk_xsc_fresh(l);
		return done(xsc_emit2(l, XSC_VECTOR_DP3, xsc_plain(rows[0]), xsc_temp(cr), r, false, 0xF), r);
	}
	xsdk_xsc_lower_refuse(l, "determinant supports mat2/mat3");
	return XSC_NONE;
}

static bool scaled_into(XscLower *l, XscSrc src, int64_t idet, int64_t dst, int64_t mask)
{
	return xsc_emit2(l, XSC_VECTOR_MUL, src, xsc_temp(idet), dst, false, mask);
}

// (1/det) [[d, -b], [-c, a]].
static const XscMatrix *inverse2(XscLower *l, const XscOperand *rows, int64_t idet)
{
	XscMatrix *const out = new_matrix(l, 2);
	if (!out)
		return NULL;
	const int64_t r0 = xsdk_xsc_fresh(l);
	if (!scaled_into(l, xsc_src(rows[1], false, xsdk_xsc_bsw(1)), idet, r0, 0x1) ||
		!scaled_into(l, xsc_src(rows[0], true, xsdk_xsc_bsw(1)), idet, r0, 0x2))
		return NULL;
	const int64_t r1 = xsdk_xsc_fresh(l);
	if (!scaled_into(l, xsc_src(rows[1], true, xsdk_xsc_bsw(0)), idet, r1, 0x1) ||
		!scaled_into(l, xsc_src(rows[0], false, xsdk_xsc_bsw(0)), idet, r1, 0x2))
		return NULL;
	out->rows[0] = xsc_reg(r0);
	out->rows[1] = xsc_reg(r1);
	return out;
}

// The cofactors are the cross products of the other two rows; the inverse is their transpose over det.
static const XscMatrix *inverse3(XscLower *l, const XscOperand *rows, int64_t idet)
{
	XscMatrix *const out = new_matrix(l, 3);
	if (!out)
		return NULL;
	int64_t cof[3];
	for (int j = 0; j < 3; j++) {
		cof[j] = xsdk_xsc_cross(l, rows[(j + 1) % 3], rows[(j + 2) % 3]);
		if (cof[j] == XSC_NONE)
			return NULL;
	}
	for (int64_t i = 0; i < 3; i++) {
		const int64_t r = xsdk_xsc_fresh(l);
		for (int64_t j = 0; j < 3; j++) {
			if (!scaled_into(l, xsc_src(xsc_reg(cof[j]), false, ((i - j) & 3) << (2 * j)), idet, r, (int64_t)1 << j))
				return NULL;
		}
		out->rows[i] = xsc_reg(r);
	}
	return out;
}

static const XscMatrix *emit_inverse(XscLower *l, uint32_t mat_ssa)
{
	const XscMatrix *const in = xsdk_xsc_matrix_operands(l, mat_ssa);
	XscOperand rows[XSC_MATRIX_ROWS];
	if (!in || !rows_in_registers(l, in, rows))
		return NULL;
	const int64_t det = xsdk_xsc_emit_determinant(l, mat_ssa);
	if (det == XSC_NONE)
		return NULL;
	const int64_t idet = xsdk_xsc_emit_scalar(l, XSC_SCALAR_RCP, xsdk_xsc_fresh(l), xsc_reg(det), 3, 0xF);
	if (idet == XSC_NONE)
		return NULL;
	if (in->count == 2)
		return inverse2(l, rows, idet);
	if (in->count == 3)
		return inverse3(l, rows, idet);
	xsdk_xsc_lower_refuse(l, "inverse supports mat2/mat3");
	return NULL;
}

// (M * v)[i] = dot(v, row i), a dot product a component.
static int64_t mat_vec_ops(XscLower *l, const XscMatrix *mat, XscOperand vec_op, int64_t dst, bool export_position)
{
	const size_t n = mat->count;
	if (!square(n)) {
		xsdk_xsc_lower_refuse(l, "matrix-vector expects mat2/3/4, got %zu rows", n);
		return XSC_NONE;
	}
	const int64_t dp = n == 4 ? XSC_VECTOR_DP4 : n == 3 ? XSC_VECTOR_DP3 : XSC_VECTOR_DP2_ADD;
	const XscOperand v = xsdk_xsc_to_reg(l, vec_op);
	if (xsc_failed(l))
		return XSC_NONE;
	const int64_t target = export_position ? XSC_EXPORT_POSITION : dst != XSC_NONE ? dst : xsdk_xsc_fresh(l);
	for (size_t i = 0; i < n; i++) {
		const XscOperand row = mat->rows[i];
		if (row.kind != XSC_OPERAND_REG && row.kind != XSC_OPERAND_CONST) {
			xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "a matrix as a matrix row");
			return XSC_NONE;
		}
		XscAlu f = xsdk_xsc_template_dp4();
		f.vector_opcode = dp;
		f.vector_dest = target;
		f.is_export = export_position;
		f.vector_write_mask = (int64_t)1 << i;
		f.src_reg[0] = v.value;
		f.src_is_temp[0] = true;
		f.src_reg[1] = row.value;
		f.src_is_temp[1] = row.kind == XSC_OPERAND_REG;
		// dp2add adds src3: zero.
		if (n == 2) {
			static const double zero[4] = { 0, 0, 0, 0 };
			const int64_t z = xsdk_xsc_literal_reg(l, zero);
			if (z == XSC_NONE)
				return XSC_NONE;
			f.src_reg[2] = z;
			f.src_is_temp[2] = false;
			f.src_swizzle[2] = 0;
		}
		if (!xsdk_xsc_emit_alu(l, &f))
			return XSC_NONE;
	}
	return target;
}

// v * M = sum(row i * v[i]), multiply-adds; the position goes out through a temp.
static int64_t vec_mat_ops(XscLower *l, XscOperand vec_op, const XscMatrix *mat, int64_t dst, bool export_position)
{
	const size_t n = mat->count;
	if (!square(n)) {
		xsdk_xsc_lower_refuse(l, "vector-matrix expects mat2/3/4, got %zu rows", n);
		return XSC_NONE;
	}
	const XscOperand v = xsdk_xsc_to_reg(l, vec_op);
	if (xsc_failed(l))
		return XSC_NONE;
	const int64_t target = export_position || dst == XSC_NONE ? xsdk_xsc_fresh(l) : dst;
	if (!xsc_emit2(l, XSC_VECTOR_MUL, xsc_plain(mat->rows[0]), xsc_src(v, false, xsdk_xsc_bsw(0)), target, false, 0xF))
		return XSC_NONE;
	for (size_t i = 1; i < n; i++) {
		const XscSrc vi = xsc_src(v, false, xsdk_xsc_bsw((int64_t)i));
		if (!xsc_emit3(l, XSC_VECTOR_MAD, xsc_plain(mat->rows[i]), vi, xsc_temp(target), target, false, 0xF))
			return XSC_NONE;
	}
	if (export_position &&
		xsdk_xsc_emit_copy(l, xsc_reg(target), XSC_EXPORT_POSITION, true, false, 0, 0xF, false) == XSC_NONE)
		return XSC_NONE;
	return target;
}

// Over columns, M * v is v * M's multiply-adds and v * M is M * v's dot products.
int64_t xsdk_xsc_emit_mat_vec(XscLower *l, uint32_t mat, uint32_t vec, int64_t dst, bool export_position)
{
	bool col;
	if (!is_col_major(l, mat, &col))
		return XSC_NONE;
	const XscMatrix *const m = xsdk_xsc_matrix_operands(l, mat);
	const XscOperand v = xsdk_xsc_lower_operand(l, vec);
	if (xsc_failed(l))
		return XSC_NONE;
	return col ? vec_mat_ops(l, v, m, dst, export_position) : mat_vec_ops(l, m, v, dst, export_position);
}

int64_t xsdk_xsc_emit_vec_mat(XscLower *l, uint32_t vec, uint32_t mat, int64_t dst, bool export_position)
{
	bool col;
	if (!is_col_major(l, mat, &col))
		return XSC_NONE;
	const XscMatrix *const m = xsdk_xsc_matrix_operands(l, mat);
	const XscOperand v = xsdk_xsc_lower_operand(l, vec);
	if (xsc_failed(l))
		return XSC_NONE;
	return col ? mat_vec_ops(l, m, v, dst, export_position) : vec_mat_ops(l, v, m, dst, export_position);
}

// Rows of A times B; over columns, column k of A * B is A times column k of B.
static const XscMatrix *emit_mat_mat(XscLower *l, uint32_t a, uint32_t b)
{
	const XscMatrix *const lhs = xsdk_xsc_matrix_operands(l, a);
	const XscMatrix *const rhs = lhs ? xsdk_xsc_matrix_operands(l, b) : NULL;
	bool col_a, col_b;
	if (!rhs || !is_col_major(l, a, &col_a) || !is_col_major(l, b, &col_b))
		return NULL;
	if (col_a != col_b) {
		xsdk_xsc_lower_refuse(l, "a product of row and column matrices (v1)");
		return NULL;
	}
	const XscMatrix *const over = col_a ? rhs : lhs;
	const XscMatrix *const by = col_a ? lhs : rhs;
	XscMatrix *const out = new_matrix(l, over->count);
	if (!out)
		return NULL;
	for (size_t i = 0; i < over->count; i++) {
		const int64_t r = vec_mat_ops(l, over->rows[i], by, XSC_NONE, false);
		if (r == XSC_NONE)
			return NULL;
		out->rows[i] = xsc_reg(r);
	}
	return out;
}

static const XscMatrix *resolve_matrix(XscLower *l, uint32_t ssa, const XscInstr *ins)
{
	const uint32_t a = xsdk_xsc_operand(ins, 0);
	if (ins->opcode == SPV_LOAD)
		return load_matrix(l, ssa, ins);
	if (ins->opcode == SPV_MATRIX_TIMES_MATRIX) {
		const XscMatrix *const res = emit_mat_mat(l, a, xsdk_xsc_operand(ins, 1));
		if (res && l->col_major[a])
			l->col_major[ssa] = true;
		return res;
	}
	if (ins->opcode == SPV_TRANSPOSE) {
		bool col;
		if (!is_col_major(l, a, &col))
			return NULL;
		// A transpose of columns is the same registers read as rows.
		return col ? xsdk_xsc_matrix_operands(l, a) : emit_transpose(l, a);
	}
	if (ins->opcode == SPV_EXT_INST && xsdk_xsc_operand(ins, 1) == GLSL_MATRIX_INVERSE)
		return emit_inverse(l, xsdk_xsc_operand(ins, 2));
	char name[16];
	xsdk_xsc_lower_refuse(
		l, "v1 %s: unhandled matrix op %s", xsc_stage_name(l), xsdk_xsc_op_name(ins->opcode, name, sizeof name));
	return NULL;
}

const XscMatrix *xsdk_xsc_matrix_operands(XscLower *l, uint32_t ssa)
{
	if (ssa < l->m->id_limit && l->matrix_cache[ssa])
		return l->matrix_cache[ssa];
	const XscInstr *const ins = xsdk_xsc_by_result(l->m, ssa);
	if (!ins) {
		xsdk_xsc_lower_refuse(l, "%%%u is not a matrix value", (unsigned)ssa);
		return NULL;
	}
	if (!xsdk_xsc_enter(l))
		return NULL;
	const XscMatrix *const res = resolve_matrix(l, ssa, ins);
	l->depth--;
	if (res)
		l->matrix_cache[ssa] = res;
	return res;
}
