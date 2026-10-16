#include <math.h>
#include <stdio.h>

#include "xsc_private.h"

// Instructions encoded, as xenos-toolchain/xasm/encode.py encodes them, each field checked in its order there, and
// the templates xenos-toolchain/xsc/templates.py gives the lowering.

enum {
	ADDRESSING_RELATIVE = 0,
	SIGNED_RF_ZERO_CLAMP = 0,
	SAMPLE_CENTROID = 0,
	// Fetch destination swizzles are absolute, 3 bits a component (xenia:src/xenia/gpu/ucode.h:677-693).
	DEST_XYZW = 0 | 1 << 3 | 2 << 6 | 3 << 9,
	// A 2D texture fetch's source: x, y, then x again for the unused z (ucode.h:851-852).
	SRC_XY = 0 | 1 << 2 | 0 << 4,
	// Stream 0's vertex fetch constant, as libxenon numbers it (libxenon:drivers/xenos/xe.c:1965).
	VERTEX_FETCH_CONSTANT = 95,
};

// Places an unsigned value into bits [lo, lo + width) (encode.py _enc).
static bool field(Xsc *x, uint32_t *word, int64_t value, int lo, int width, const char *name)
{
	if (value < 0 || value >= (int64_t)1 << width)
		return xsdk_xsc_fail(
			x, XSDK_XSC_BAD_VALUE, "%s=%lld does not fit in %d unsigned bits", name, (long long)value, width);
	*word |= (uint32_t)value << lo;
	return true;
}

// A signed value, two's complement (encode.py _enc_s).
static bool signed_field(Xsc *x, uint32_t *word, int64_t value, int lo, int width, const char *name)
{
	const int64_t low = -((int64_t)1 << (width - 1));
	const int64_t high = ((int64_t)1 << (width - 1)) - 1;
	if (value < low || value > high)
		return xsdk_xsc_fail(x, XSDK_XSC_BAD_VALUE, "%s=%lld out of range [%lld, %lld] for %d signed bits", name,
			(long long)value, (long long)low, (long long)high, width);
	*word |= (uint32_t)(value & (((int64_t)1 << width) - 1)) << lo;
	return true;
}

static void flag(uint32_t *word, bool value, int lo)
{
	*word |= (uint32_t)value << lo;
}

// A value one of an enum's members has (encode.py _enum): the enum's members from 0 to count, less the gaps.
static bool member(Xsc *x, int64_t value, int64_t count, const char *name, const char *enum_name)
{
	const bool fetch_gap = count == 27 && ((value >= 2 && value <= 15) || (value >= 20 && value <= 23));
	if (value < 0 || value >= count || fetch_gap)
		return xsdk_xsc_fail(x, XSDK_XSC_BAD_VALUE, "%s=%lld is not a valid %s", name, (long long)value, enum_name);
	return true;
}

// A float the decoder gives as raw / scale (encode.py _scaled_signed).
static bool scaled_field(Xsc *x, uint32_t *word, double value, int scale, int lo, int width, const char *name)
{
	const double raw = value * scale;
	if (raw != trunc(raw)) {
		char text[40];
		xsdk_xsc_float_repr(value, text, sizeof text);
		return xsdk_xsc_fail(x, XSDK_XSC_BAD_VALUE, "%s=%s is not representable in steps of 1/%d", name, text, scale);
	}
	return signed_field(x, word, (int64_t)raw, lo, width, name);
}

// vc's first four bits are vc_hi, at word 0's top, and its last two vc_lo, at word 1's bottom
// (xenia:src/xenia/gpu/ucode.h:243-246).
bool xsdk_xsc_build_cf_exec(Xsc *x, uint32_t opcode, int64_t address, int64_t count, int64_t sequence, int64_t vc,
	bool is_predicate_clean, uint32_t pair[2])
{
	uint32_t d0 = 0, d1 = 0;
	if (!field(x, &d0, address, 0, 12, "address") || !field(x, &d0, count, 12, 3, "count"))
		return false;
	if (!field(x, &d0, sequence, 16, 12, "sequence") || !field(x, &d0, vc & 0xF, 28, 4, "vc_hi") ||
		!field(x, &d1, vc >> 4, 0, 2, "vc_lo"))
		return false;
	flag(&d1, is_predicate_clean, 9);
	d1 |= (uint32_t)ADDRESSING_RELATIVE << 11 | opcode << 12;
	pair[0] = d0;
	pair[1] = d1;
	return true;
}

bool xsdk_xsc_build_cf_alloc(Xsc *x, int64_t alloc_type, uint32_t pair[2])
{
	uint32_t d1 = 0;
	if (!member(x, alloc_type, 4, "alloc_type", "AllocType"))
		return false;
	d1 |= (uint32_t)alloc_type << 9 | (uint32_t)XSC_CF_ALLOC << 12;
	pair[0] = 0;
	pair[1] = d1;
	return true;
}

bool xsdk_xsc_build_cf_loop_start(Xsc *x, int64_t address, int64_t loop_id, uint32_t pair[2])
{
	uint32_t d0 = 0;
	if (!field(x, &d0, address, 0, 13, "address") || !field(x, &d0, loop_id, 16, 5, "loop_id"))
		return false;
	pair[0] = d0;
	pair[1] = (uint32_t)ADDRESSING_RELATIVE << 11 | (uint32_t)XSC_CF_LOOP_START << 12;
	return true;
}

bool xsdk_xsc_build_cf_loop_end(
	Xsc *x, int64_t address, int64_t loop_id, bool is_predicated_break, bool condition, uint32_t pair[2])
{
	uint32_t d0 = 0, d1 = 0;
	if (!field(x, &d0, address, 0, 13, "address") || !field(x, &d0, loop_id, 16, 5, "loop_id"))
		return false;
	flag(&d0, is_predicated_break, 21);
	flag(&d1, condition, 10);
	d1 |= (uint32_t)ADDRESSING_RELATIVE << 11 | (uint32_t)XSC_CF_LOOP_END << 12;
	pair[0] = d0;
	pair[1] = d1;
	return true;
}

// Two 48-bit control flow instructions in three dwords (xenos_ucode.py _pack_cf_pair).
void xsdk_xsc_pack_cf_pair(const uint32_t a[2], const uint32_t b[2], uint32_t out[3])
{
	out[0] = a[0];
	out[1] = a[1] | (b[0] & 0xFFFF) << 16;
	out[2] = b[0] >> 16 | b[1] << 16;
}

bool xsdk_xsc_build_vfetch(Xsc *x, const XscFetch *f, uint32_t triple[3])
{
	uint32_t d0 = 0, d1 = 0, d2 = 0;
	if (!member(x, f->opcode, 27, "opcode", "FetchOpcode"))
		return false;
	if (f->opcode != XSC_FETCH_VERTEX)
		return xsdk_xsc_fail(x, XSDK_XSC_BAD_VALUE, "build_vfetch_full: opcode must be kVertexFetch");
	bool ok = field(x, &d0, f->opcode, 0, 5, "opcode") && field(x, &d0, f->src, 5, 6, "src");
	flag(&d0, f->is_src_relative, 11);
	ok = ok && field(x, &d0, f->dest, 12, 6, "dest");
	flag(&d0, f->is_dest_relative, 18);
	ok = ok && field(x, &d0, f->must_be_one, 19, 1, "must_be_one") &&
		field(x, &d0, f->const_index, 20, 5, "const_index") &&
		field(x, &d0, f->const_index_sel, 25, 2, "const_index_sel") &&
		field(x, &d0, f->prefetch_count, 27, 3, "prefetch_count") &&
		field(x, &d0, f->src_swizzle, 30, 2, "src_swizzle");
	ok = ok && field(x, &d1, f->dest_swizzle, 0, 12, "dest_swizzle");
	flag(&d1, f->is_signed, 12);
	// num_format_all: 0 is normalized.
	flag(&d1, !f->is_normalized, 13);
	ok = ok && member(x, f->signed_rf_mode, 2, "signed_rf_mode", "SignedRepeatingFractionMode") &&
		field(x, &d1, f->signed_rf_mode, 14, 1, "signed_rf_mode");
	flag(&d1, f->is_index_rounded, 15);
	ok = ok && field(x, &d1, f->data_format, 16, 6, "data_format") &&
		signed_field(x, &d1, f->exp_adjust, 24, 6, "exp_adjust");
	flag(&d1, f->is_predicated, 31);
	ok = ok && field(x, &d2, f->stride, 0, 8, "stride") && signed_field(x, &d2, f->offset, 8, 23, "offset");
	flag(&d2, f->predicate_condition, 31);
	triple[0] = d0;
	triple[1] = d1;
	triple[2] = d2;
	return ok;
}

static bool texture_opcode(int64_t opcode)
{
	return opcode == XSC_FETCH_TEXTURE || (opcode >= XSC_FETCH_GET_BORDER_COLOR_FRAC && opcode <= 19) ||
		(opcode >= XSC_FETCH_SET_LOD && opcode <= XSC_FETCH_SET_GRADIENTS_VERT);
}

static bool filter(Xsc *x, uint32_t *word, int64_t value, int lo, const char *name)
{
	return member(x, value, 4, name, "TextureFilter") && field(x, word, value, lo, 2, name);
}

bool xsdk_xsc_build_tfetch(Xsc *x, const XscFetch *f, uint32_t triple[3])
{
	uint32_t d0 = 0, d1 = 0, d2 = 0;
	if (!member(x, f->opcode, 27, "opcode", "FetchOpcode"))
		return false;
	// The one fetch opcode left is the vertex fetch's.
	if (!texture_opcode(f->opcode))
		return xsdk_xsc_fail(x, XSDK_XSC_BAD_VALUE, "build_tfetch: kVertexFetch is not a texture fetch opcode");
	bool ok = field(x, &d0, f->opcode, 0, 5, "opcode") && field(x, &d0, f->src, 5, 6, "src");
	flag(&d0, f->is_src_relative, 11);
	ok = ok && field(x, &d0, f->dest, 12, 6, "dest");
	flag(&d0, f->is_dest_relative, 18);
	flag(&d0, f->fetch_valid_only, 19);
	ok = ok && field(x, &d0, f->fetch_constant_index, 20, 5, "fetch_constant_index");
	flag(&d0, f->unnormalized_coordinates, 25);
	ok = ok && field(x, &d0, f->src_swizzle, 26, 6, "src_swizzle");
	ok = ok && field(x, &d1, f->dest_swizzle, 0, 12, "dest_swizzle") &&
		filter(x, &d1, f->mag_filter, 12, "mag_filter") && filter(x, &d1, f->min_filter, 14, "min_filter") &&
		filter(x, &d1, f->mip_filter, 16, "mip_filter") && field(x, &d1, f->aniso_filter, 18, 3, "aniso_filter") &&
		field(x, &d1, f->arbitrary_filter, 21, 3, "arbitrary_filter") &&
		filter(x, &d1, f->vol_mag_filter, 24, "vol_mag_filter") &&
		filter(x, &d1, f->vol_min_filter, 26, "vol_min_filter");
	flag(&d1, f->use_computed_lod, 28);
	flag(&d1, f->use_register_lod, 29);
	flag(&d1, f->is_predicated, 31);
	flag(&d2, f->use_register_gradients, 0);
	ok = ok && member(x, f->sample_location, 2, "sample_location", "SampleLocation") &&
		field(x, &d2, f->sample_location, 1, 1, "sample_location") &&
		scaled_field(x, &d2, f->lod_bias, 16, 2, 7, "lod_bias") &&
		member(x, f->dimension, 4, "dimension", "FetchOpDimension") &&
		field(x, &d2, f->dimension, 14, 2, "dimension") && scaled_field(x, &d2, f->offset_x, 2, 16, 5, "offset_x") &&
		scaled_field(x, &d2, f->offset_y, 2, 21, 5, "offset_y") &&
		scaled_field(x, &d2, f->offset_z, 2, 26, 5, "offset_z");
	flag(&d2, f->predicate_condition, 31);
	triple[0] = d0;
	triple[1] = d1;
	triple[2] = d2;
	return ok;
}

bool xsdk_xsc_build_alu(Xsc *x, const XscAlu *f, uint32_t triple[3])
{
	bool const0 = f->is_const_0_addressed;
	bool const1 = f->is_const_1_addressed;
	// aL-relative requests fold onto the two addressing bits as the decoder reads them back (src_const_is_addressed).
	if (f->src_const_rel[0] && !f->src_is_temp[0])
		const0 = true;
	if (f->src_const_rel[1] && !f->src_is_temp[1]) {
		if (f->src_is_temp[0])
			const0 = true;
		else
			const1 = true;
	}
	if (f->src_const_rel[2] && !f->src_is_temp[2]) {
		if (f->src_is_temp[0] && f->src_is_temp[1])
			const0 = true;
		else
			const1 = true;
	}
	uint32_t d0 = 0, d1 = 0, d2 = 0;
	bool ok = field(x, &d0, f->vector_dest, 0, 6, "vector_dest");
	flag(&d0, f->is_vector_dest_relative, 6);
	flag(&d0, f->abs_constants, 7);
	ok = ok && field(x, &d0, f->scalar_dest, 8, 6, "scalar_dest");
	flag(&d0, f->is_scalar_dest_relative, 14);
	flag(&d0, f->is_export, 15);
	ok = ok && field(x, &d0, f->vector_write_mask, 16, 4, "vector_write_mask") &&
		field(x, &d0, f->scalar_write_mask, 20, 4, "scalar_write_mask");
	flag(&d0, f->vector_clamp, 24);
	flag(&d0, f->scalar_clamp, 25);
	ok = ok && member(x, f->scalar_opcode, 64, "scalar_opcode", "AluScalarOpcode") &&
		field(x, &d0, f->scalar_opcode, 26, 6, "scalar_opcode");
	ok = ok && field(x, &d1, f->src_swizzle[2], 0, 8, "src3_swizzle") &&
		field(x, &d1, f->src_swizzle[1], 8, 8, "src2_swizzle") &&
		field(x, &d1, f->src_swizzle[0], 16, 8, "src1_swizzle");
	flag(&d1, f->src_negate[2], 24);
	flag(&d1, f->src_negate[1], 25);
	flag(&d1, f->src_negate[0], 26);
	flag(&d1, f->predicate_condition, 27);
	flag(&d1, f->is_predicated, 28);
	flag(&d1, f->is_const_address_register_relative, 29);
	flag(&d1, const1, 30);
	flag(&d1, const0, 31);
	ok = ok && field(x, &d2, f->src_reg[2], 0, 8, "src3_reg") && field(x, &d2, f->src_reg[1], 8, 8, "src2_reg") &&
		field(x, &d2, f->src_reg[0], 16, 8, "src1_reg") &&
		member(x, f->vector_opcode, 32, "vector_opcode", "AluVectorOpcode") &&
		field(x, &d2, f->vector_opcode, 24, 5, "vector_opcode");
	flag(&d2, f->src_is_temp[2], 29);
	flag(&d2, f->src_is_temp[1], 30);
	flag(&d2, f->src_is_temp[0], 31);
	triple[0] = d0;
	triple[1] = d1;
	triple[2] = d2;
	return ok;
}

// A vector op whose scalar half writes nothing (kRetainPrev, mask 0; xenia:src/xenia/gpu/ucode.h:1330-1332).
static XscAlu alu_template(int64_t vector_opcode, int64_t vector_dest, int64_t vector_write_mask, bool src2_is_temp)
{
	return (XscAlu){ .vector_opcode = vector_opcode,
		.scalar_opcode = XSC_SCALAR_RETAIN_PREV,
		.vector_dest = vector_dest,
		.vector_write_mask = vector_write_mask,
		.is_export = true,
		.src_is_temp = { true, src2_is_temp, true } };
}

XscAlu xsdk_xsc_template_mov(void)
{
	return alu_template(XSC_VECTOR_MAX, 0, 0xF, true);
}

XscAlu xsdk_xsc_template_dp4(void)
{
	return alu_template(XSC_VECTOR_DP4, XSC_EXPORT_POSITION, 0x1, false);
}

XscFetch xsdk_xsc_template_vfetch(void)
{
	return (XscFetch){ .opcode = XSC_FETCH_VERTEX,
		.dest_swizzle = DEST_XYZW,
		.must_be_one = 1,
		.const_index = VERTEX_FETCH_CONSTANT / 3,
		.const_index_sel = VERTEX_FETCH_CONSTANT % 3,
		.is_normalized = true,
		.signed_rf_mode = SIGNED_RF_ZERO_CLAMP };
}

XscFetch xsdk_xsc_template_tfetch(void)
{
	// The aniso filter's kUseFetchConst (xenos.h:140-148); the arbitrary filter's zero value, k2x4Sym
	// (xenos.h:775-783).
	return (XscFetch){ .opcode = XSC_FETCH_TEXTURE,
		.src_swizzle = SRC_XY,
		.dest_swizzle = DEST_XYZW,
		.fetch_valid_only = true,
		.mag_filter = XSC_FILTER_USE_FETCH_CONST,
		.min_filter = XSC_FILTER_USE_FETCH_CONST,
		.mip_filter = XSC_FILTER_USE_FETCH_CONST,
		.aniso_filter = 7,
		.vol_mag_filter = XSC_FILTER_USE_FETCH_CONST,
		.vol_min_filter = XSC_FILTER_USE_FETCH_CONST,
		.use_computed_lod = true,
		.sample_location = SAMPLE_CENTROID,
		.dimension = XSC_DIMENSION_2D };
}
