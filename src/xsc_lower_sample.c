#include "xsc_lower.h"

// Texture samples: the coordinate, the bias, LOD or gradients the shader gives, then the fetch, and a depth
// comparison after it.

enum {
	// SPIR-V's image operands, each followed by its ids in this order (SPIR-V 1.0, section 3.14).
	IMAGE_BIAS = 0x1,
	IMAGE_LOD = 0x2,
	IMAGE_GRAD = 0x4,
	IMAGE_TAKEN = IMAGE_BIAS | IMAGE_LOD | IMAGE_GRAD,
	DIM_2D = 1,
	DIM_3D = 2,
	DIM_CUBE = 3,
	// A tfetch's source components, two bits each (xenia:src/xenia/gpu/ucode.h:851-852): x, y and z.
	SOURCE_XYZ = 0 | 1 << 2 | 2 << 4,
	WRITE_X = 0x1,
	WRITE_Y = 0x2,
	WRITE_Z = 0x4,
	// A fetch's own LOD bias: 7 signed bits in 1/16 steps (xenia:src/xenia/gpu/ucode.h:832-835, 871).
	LOD_BIAS_STEPS = 16,
	LOD_BIAS_LOW = -64,
	LOD_BIAS_HIGH = 63,
	// Every component kept, as a fetch that writes no register should (xenia:src/xenia/gpu/ucode.h:686-687).
	DEST_KEEP = 07777,
	// A gradient's x and y, then x again for the unused z (ucode.h:671-674).
	SRC_XY = 0 | 1 << 2 | 0 << 4,
};

static bool is_projective(const XscInstr *ins)
{
	return ins->opcode == SPV_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD || ins->opcode == SPV_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD;
}

static bool is_comparison(const XscInstr *ins)
{
	return ins->opcode == SPV_IMAGE_SAMPLE_DREF_IMPLICIT_LOD || ins->opcode == SPV_IMAGE_SAMPLE_DREF_EXPLICIT_LOD;
}

// A comparison's reference comes after the coordinate, before the image operands.
static uint32_t first_operand(const XscInstr *ins)
{
	return is_comparison(ins) ? 3 : 2;
}

uint32_t xsdk_xsc_sample_inputs(const XscInstr *ins, uint32_t ids[XSC_SAMPLE_INPUTS])
{
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	uint32_t n = 0;
	for (uint32_t i = 1; i < count && n < XSC_SAMPLE_INPUTS; i++) {
		if (i != first_operand(ins))
			ids[n++] = ops[i];
	}
	return n;
}

// Whether the fetch's own bias field holds value exactly.
static bool fits_bias_field(double value)
{
	const double steps = value * LOD_BIAS_STEPS;
	return steps >= LOD_BIAS_LOW && steps <= LOD_BIAS_HIGH && steps == (double)(int64_t)steps;
}

// setTexLOD, setGradientH and setGradientV write the state the next fetches read, and no register
// (xenia:src/xenia/gpu/ucode.h:669-674; spirv_shader_translator_fetch.cc:521-541).
static bool set_state(XscLower *l, int64_t opcode, int64_t reg, int64_t swizzle)
{
	XscFetch t = xsdk_xsc_template_tfetch();
	t.opcode = opcode;
	t.src = reg;
	t.src_swizzle = swizzle;
	t.dest_swizzle = DEST_KEEP;
	return xsdk_xsc_program_tfetch(l->x, l->p, &t) != NULL;
}

// The register LOD, from any temp's component, as fetches read temps alone.
static bool set_lod(XscLower *l, uint32_t lod)
{
	const XscOrigin origin = xsdk_xsc_scalar_origin(l, lod);
	if (xsc_failed(l))
		return false;
	const XscOperand reg = xsdk_xsc_to_reg(l, origin.o);
	if (xsc_failed(l))
		return false;
	return set_state(l, XSC_FETCH_SET_LOD, reg.value, origin.comp == XSC_BROADCAST ? 0 : origin.comp);
}

static bool set_gradients(XscLower *l, uint32_t horizontal, uint32_t vertical)
{
	const uint32_t gradients[2] = { horizontal, vertical };
	const int64_t opcodes[2] = { XSC_FETCH_SET_GRADIENTS_HORZ, XSC_FETCH_SET_GRADIENTS_VERT };
	for (int i = 0; i < 2; i++) {
		const XscOperand reg = xsdk_xsc_to_reg(l, xsdk_xsc_lower_operand(l, gradients[i]));
		if (xsc_failed(l) || !set_state(l, opcodes[i], reg.value, SRC_XY))
			return false;
	}
	return true;
}

// The fetch's LOD fields, with any state they read set first. The GPU adds the fetch constant's bias, the register
// LOD and the fetch's own bias, and works the LOD out from the coordinate's gradients, or the registers' if asked, only
// where it's told to compute it (xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:1498-1536).
static bool lod_fields(XscLower *l, uint32_t mask, const uint32_t *ids, XscFetch *t)
{
	uint32_t at = 0;
	const uint32_t bias = mask & IMAGE_BIAS ? ids[at++] : 0;
	const uint32_t lod = mask & IMAGE_LOD ? ids[at++] : 0;
	const uint32_t horizontal = mask & IMAGE_GRAD ? ids[at++] : 0;
	const uint32_t vertical = mask & IMAGE_GRAD ? ids[at++] : 0;
	t->use_computed_lod = !lod;
	const uint32_t level = bias ? bias : lod;
	double constant;
	if (level && xsdk_xsc_float_const(l, level, &constant) && fits_bias_field(constant)) {
		t->lod_bias = constant;
	} else if (level) {
		if (!set_lod(l, level))
			return false;
		t->use_register_lod = true;
	}
	if (!horizontal)
		return true;
	t->use_register_gradients = true;
	return set_gradients(l, horizontal, vertical);
}

static bool sample_cube(XscLower *l, int64_t coord, XscFetch *t)
{
	static const int64_t zzxy[] = { 2, 2, 0, 1 };
	static const int64_t yxzz[] = { 1, 0, 2, 2 };
	const int64_t cube = xsdk_xsc_fresh(l);
	const XscSrc a = xsc_src(xsc_reg(coord), false, xsdk_xsc_swz(zzxy, 4));
	const XscSrc b = xsc_src(xsc_reg(coord), false, xsdk_xsc_swz(yxzz, 4));
	if (!xsc_emit2(l, XSC_VECTOR_CUBE, a, b, cube, false, 0xF) ||
		xsdk_xsc_emit_scalar(l, XSC_SCALAR_RCP, cube, xsc_reg(cube | XSC_ABS_SOURCE), 2, 0x4) == XSC_NONE)
		return false;
	const int64_t bias = xsdk_xsc_literal_splat(l, 1.5);
	const XscSrc ma = xsc_src(xsc_reg(cube), false, xsdk_xsc_bsw(2));
	if (bias == XSC_NONE ||
		!xsc_emit3(l, XSC_VECTOR_MAD, xsc_temp(cube), ma, xsc_plain(xsc_const(bias)), cube, false, 0x3))
		return false;
	t->dimension = XSC_DIMENSION_CUBE;
	t->src_swizzle = 1 | 0 << 2 | 3 << 4;
	t->src = cube;
	return xsdk_xsc_program_tfetch(l->x, l->p, t) != NULL;
}

// A 2D or 3D sample; a projective one divides the coordinate by its last component first.
static bool sample_texture(XscLower *l, uint32_t coord, int64_t coord_reg, bool proj, XscFetch *t)
{
	if (proj) {
		const int64_t size = xsdk_xsc_vec_size(l, coord);
		const int64_t rw = xsdk_xsc_emit_scalar(
			l, XSC_SCALAR_RCP, xsdk_xsc_fresh(l), xsc_reg(coord_reg), size - 1 > 1 ? size - 1 : 1, 0xF);
		if (rw == XSC_NONE)
			return false;
		const int64_t uv = xsdk_xsc_fresh(l);
		if (!xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(coord_reg), xsc_temp(rw), uv, false, 0xF))
			return false;
		coord_reg = uv;
	}
	t->src = coord_reg;
	return xsdk_xsc_program_tfetch(l->x, l->p, t) != NULL;
}

// A sampler2DShadow's result: the fetched depth against the reference, clamped to 0 to 1 as a UNORM depth's is, below
// it, equal and above it, 1 or 0 each, weighed by the compare op's weights the runtime writes
// (xenos-toolchain/docs/xsc.md).
static bool compare_depth(XscLower *l, uint32_t reference, int64_t weights, int64_t texel, int64_t dst)
{
	const XscOrigin r = xsdk_xsc_scalar_origin(l, reference);
	if (xsc_failed(l))
		return false;
	const int64_t swizzle = r.comp == XSC_BROADCAST ? 0 : xsdk_xsc_bsw(r.comp);
	const int64_t ref = xsdk_xsc_emit_copy(l, r.o, xsdk_xsc_fresh(l), false, false, swizzle, WRITE_X, true);
	if (ref == XSC_NONE)
		return false;
	const XscSrc depth = xsc_src(xsc_reg(texel), false, xsdk_xsc_bsw(0));
	const XscSrc clamped = xsc_src(xsc_reg(ref), false, xsdk_xsc_bsw(0));
	const int64_t sides = xsdk_xsc_fresh(l);
	return xsc_emit2(l, XSC_VECTOR_SGT, depth, clamped, sides, false, WRITE_X) &&
		xsc_emit2(l, XSC_VECTOR_SEQ, clamped, depth, sides, false, WRITE_Y) &&
		xsc_emit2(l, XSC_VECTOR_SGT, clamped, depth, sides, false, WRITE_Z) &&
		xsc_emit2(l, XSC_VECTOR_DP3, xsc_temp(sides), xsc_plain(xsc_const(weights)), dst, false, 0xF);
}

bool xsdk_xsc_emit_sample(XscLower *l, uint32_t ssa, const XscInstr *ins, int64_t *slot)
{
	const bool proj = is_projective(ins);
	const bool dref = is_comparison(ins);
	const XscInstr *const sampled = xsdk_xsc_by_result(l->m, xsdk_xsc_operand(ins, 0));
	const XscRole *const role = sampled ? xsdk_xsc_role(l->si, xsdk_xsc_operand(sampled, 0)) : NULL;
	if (!role || role->kind != XSC_ROLE_SAMPLER)
		return xsdk_xsc_lower_refuse(l, "ImageSample needs a sampler");
	*slot = role->a;
	const int64_t dim = role->b;
	if (dim == XSC_NONE)
		return xsdk_xsc_lower_refuse(l, "unsupported sampler dimension None");
	if (dim != DIM_2D && dim != DIM_3D && dim != DIM_CUBE)
		return xsdk_xsc_lower_refuse(l, "unsupported sampler dimension %lld", (long long)dim);
	if (dref && (dim != DIM_2D || role->c == XSC_NONE))
		return xsdk_xsc_lower_refuse(l, "depth comparisons other than a sampler2DShadow's unsupported");
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	const uint32_t first = first_operand(ins);
	const uint32_t mask = count > first ? ops[first] : 0;
	if (mask & ~(uint32_t)IMAGE_TAKEN)
		return xsdk_xsc_lower_refuse(l, "image operands 0x%X: offsets, a sample index or a minimum LOD (v1)",
			(unsigned)(mask & ~(uint32_t)IMAGE_TAKEN));
	if (dim != DIM_2D && (mask & IMAGE_GRAD))
		return xsdk_xsc_lower_refuse(l, "gradients for a cube map or a 3D texture (v1)");
	// A vertex shader has no quad to work a LOD out over
	// (xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:639-641).
	if (l->vertex && !(mask & (IMAGE_LOD | IMAGE_GRAD)))
		return xsdk_xsc_lower_refuse(l, "a vertex shader's sample without a LOD or gradients");
	const uint32_t coord = xsdk_xsc_operand(ins, 1);
	const XscOperand coord_reg = xsdk_xsc_to_reg(l, xsdk_xsc_lower_operand(l, coord));
	if (xsc_failed(l))
		return false;
	XscFetch t = xsdk_xsc_template_tfetch();
	t.fetch_constant_index = *slot;
	// Valid only skips a quad's pixels outside the primitive (xenia:src/xenia/gpu/ucode.h:558-561); vertices have none.
	t.fetch_valid_only = !l->vertex;
	if (!lod_fields(l, mask, ops + first + 1, &t))
		return false;
	t.dest = xsdk_xsc_fresh(l);
	if (dim == DIM_CUBE && proj)
		return xsdk_xsc_lower_refuse(l, "projective cube sampling unsupported");
	if (dim == DIM_3D) {
		t.dimension = XSC_DIMENSION_3D;
		t.src_swizzle = SOURCE_XYZ;
	}
	const bool sampled_ok =
		dim == DIM_CUBE ? sample_cube(l, coord_reg.value, &t) : sample_texture(l, coord, coord_reg.value, proj, &t);
	if (!sampled_ok)
		return false;
	if (!dref) {
		l->sample_reg[ssa] = t.dest;
		return true;
	}
	// A Vulkan shader's compare op is its sampler's, bound at run time, so the ALU compares after the fetch.
	const int64_t compared = xsdk_xsc_fresh(l);
	if (!compare_depth(l, xsdk_xsc_operand(ins, 2), role->c, t.dest, compared))
		return false;
	l->sample_reg[ssa] = compared;
	return true;
}
