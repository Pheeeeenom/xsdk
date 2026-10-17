#include <string.h>

#include "xsc_private.h"

// The program builder, as xenos-toolchain/xasm/program.py lays a program out and packs its registers.

enum {
	// An exec's count field is 3 bits; xost stays below the 7 limit.
	MAX_EXEC_COUNT = 6,
	// A temp's index is six bits in an instruction (xenia:src/xenia/gpu/ucode.h).
	ENCODABLE_REGS = 64,
	// Hand-built programs pack registers only past these; xsc's always (program.py ALLOCATE_PAST).
	PIXEL_ALLOCATE_PAST = 16,
	VERTEX_ALLOCATE_PAST = 64,
	// The pixel parameters' register is an interpolator's index (xenia:src/xenia/gpu/registers.h:159-161).
	MAX_INTERPOLATORS = 16,
	// SQ_PROGRAM_CNTL's ps_export_mode: depth in bit 0 and the colours exported counted from bit 1, as R600's
	// SQ_PGM_EXPORTS_PS has them (radeonhd:src/r600_exa.c:164, one colour 2). One colour is libxenon's 2
	// (libxenon:drivers/xenos/xe.c:419), with depth 3 (docs/HARDWARE.md).
	PS_EXPORT_MODE_SHIFT = 27,
	PS_EXPORT_DEPTH = 1,
	PS_EXPORT_COLORS_SHIFT = 1,
	// Its vs_export_mode for a second position vector, a point's size (xenia:src/xenia/gpu/registers.h:144;
	// xenos.h:790-800).
	VS_EXPORT_MODE_SHIFT = 24,
	VS_EXPORT_SPRITE = 2,
	USAGE_NIBBLE_TEXCOORD = 0x3,
	USAGE_NIBBLE_COLOR = 0xF,
	MAX_DEFS = 2,
	MAX_USES = 3,
};

bool xsdk_xsc_modifies_predicate(const XscOp *op)
{
	return op->kind == XSC_OP_ALU && op->alu.scalar_opcode >= XSC_SCALAR_SETP_EQ &&
		op->alu.scalar_opcode <= XSC_SCALAR_SETP_RSTR;
}

static bool add_stage(Xsc *x, XscProgram *p)
{
	if (!xsdk_xsc_room(x, &p->stages, p->stage_count, &p->stage_capacity, sizeof *p->stages))
		return false;
	p->stages[p->stage_count++] = (XscStage){ .alloc_after = XSC_NONE };
	return true;
}

bool xsdk_xsc_program_init(Xsc *x, XscProgram *p, bool pixel, uint32_t registers)
{
	memset(p, 0, sizeof *p);
	p->pixel = pixel;
	p->registers = registers;
	p->group_fetches = true;
	p->export_stage = XSC_NONE;
	p->param_gen = XSC_NONE;
	return add_stage(x, p);
}

static int64_t src_temp_index(const XscProgram *p, int64_t reg)
{
	return p->whole_sources ? reg & (XSC_ABS_SOURCE - 1) : reg & 0x3F;
}

// A register through the allocator's mapping, or itself.
static int64_t mapped(const int64_t *mapping, int64_t mapping_count, int64_t reg)
{
	return mapping && reg >= 0 && reg < mapping_count && mapping[reg] != XSC_NONE ? mapping[reg] : reg;
}

static int64_t encoded_src_temp(const XscProgram *p, int64_t reg, const int64_t *mapping, int64_t mapping_count)
{
	const int64_t idx = src_temp_index(p, reg);
	if (p->whole_sources)
		return mapped(mapping, mapping_count, idx) | (reg & XSC_ABS_SOURCE ? 0x80 : 0);
	return (reg & ~(int64_t)0x3F) | mapped(mapping, mapping_count, idx);
}

// An op's fields with each temp mapped, its sources as an instruction encodes them (program.py _remap_fields).
static void remap(const XscProgram *p, XscOp *op, const int64_t *mapping, int64_t mapping_count, XscOp *out)
{
	*out = *op;
	if (op->kind == XSC_OP_ALU) {
		XscAlu *const f = &out->alu;
		if (!f->is_export && f->vector_write_mask)
			f->vector_dest = mapped(mapping, mapping_count, f->vector_dest);
		if (f->scalar_write_mask && !f->is_export)
			f->scalar_dest = mapped(mapping, mapping_count, f->scalar_dest);
		for (int i = 0; i < 3; i++) {
			if (f->src_is_temp[i])
				f->src_reg[i] = encoded_src_temp(p, f->src_reg[i], mapping, mapping_count);
		}
		return;
	}
	out->fetch.dest = mapped(mapping, mapping_count, op->fetch.dest);
	out->fetch.src = mapped(mapping, mapping_count, op->fetch.src);
}

static int64_t highest(int64_t a, int64_t b)
{
	return a > b ? a : b;
}

// The highest register an op names (program.py _track_regs).
static int64_t tracked(const XscProgram *p, const XscOp *op, int64_t most)
{
	if (op->kind != XSC_OP_ALU)
		return highest(highest(most, op->fetch.dest), op->fetch.src);
	const XscAlu *const f = &op->alu;
	if (!f->is_export)
		most = highest(most, f->vector_dest);
	if (f->scalar_write_mask && !f->is_export)
		most = highest(most, f->scalar_dest);
	for (int i = 0; i < 3; i++) {
		if (f->src_is_temp[i])
			most = highest(most, src_temp_index(p, f->src_reg[i]));
	}
	return most;
}

static bool encode(Xsc *x, const XscOp *op, uint32_t triple[3])
{
	if (op->kind == XSC_OP_ALU)
		return xsdk_xsc_build_alu(x, &op->alu, triple);
	if (op->kind == XSC_OP_TFETCH)
		return xsdk_xsc_build_tfetch(x, &op->fetch, triple);
	return xsdk_xsc_build_vfetch(x, &op->fetch, triple);
}

// Encodes an op if its registers fit an instruction yet, as program.py's builders do on the way in.
static bool encode_if_encodable(Xsc *x, const XscProgram *p, XscOp *op)
{
	op->encoded = tracked(p, op, 0) < ENCODABLE_REGS;
	if (!op->encoded)
		return true;
	XscOp plain;
	remap(p, op, NULL, 0, &plain);
	return encode(x, &plain, op->triple);
}

static XscOp *add_op(Xsc *x, XscProgram *p, XscOpKind kind)
{
	XscStage *const stage = &p->stages[p->stage_count - 1];
	if (!xsdk_xsc_room(x, &stage->ops, stage->count, &stage->capacity, sizeof *stage->ops))
		return NULL;
	XscOp *const op = xsdk_xsc_alloc(x, sizeof *op);
	if (!op)
		return NULL;
	op->kind = kind;
	op->is_fetch = kind != XSC_OP_ALU;
	op->slot = -1;
	op->vertex_element = XSC_NONE;
	stage->ops[stage->count++] = op;
	return op;
}

// Builds an op and records its registers; NULL with the error set if it doesn't encode. In a predicated stage the op is
// predicated as its exec is, as an exec on the predicate runs every vertex or pixel of 64 when any needs it
// (xenia:src/xenia/gpu/ucode.h:114-124).
static XscOp *declare(Xsc *x, XscProgram *p, XscOpKind kind, const XscAlu *alu, const XscFetch *fetch)
{
	XscOp candidate = { .kind = kind, .is_fetch = kind != XSC_OP_ALU, .slot = -1, .vertex_element = XSC_NONE };
	if (alu)
		candidate.alu = *alu;
	if (fetch)
		candidate.fetch = *fetch;
	const uint8_t predicate = p->stages[p->stage_count - 1].predicate;
	if (predicate && alu) {
		candidate.alu.is_predicated = true;
		candidate.alu.predicate_condition = predicate == XSC_PREDICATE_TRUE;
	} else if (predicate) {
		candidate.fetch.is_predicated = true;
		candidate.fetch.predicate_condition = predicate == XSC_PREDICATE_TRUE;
	}
	if (!encode_if_encodable(x, p, &candidate))
		return NULL;
	p->max_reg = tracked(p, &candidate, p->max_reg);
	XscOp *const op = add_op(x, p, kind);
	if (op)
		*op = candidate;
	return op;
}

XscOp *xsdk_xsc_program_alu(Xsc *x, XscProgram *p, const XscAlu *f)
{
	return declare(x, p, XSC_OP_ALU, f, NULL);
}

XscOp *xsdk_xsc_program_tfetch(Xsc *x, XscProgram *p, const XscFetch *f)
{
	return declare(x, p, XSC_OP_TFETCH, NULL, f);
}

static uint32_t element_flags(uint32_t usage)
{
	return usage == XSC_USAGE_POSITION ? 0x1 : usage == XSC_USAGE_COLOR ? 0x3 : 0x0;
}

static uint32_t usage_nibble(uint32_t usage)
{
	return usage == XSC_USAGE_TEXCOORD ? USAGE_NIBBLE_TEXCOORD : usage == XSC_USAGE_COLOR ? USAGE_NIBBLE_COLOR : 0;
}

// A template vfetch, patched at load (program.py VertexProgram.fetch).
XscOp *xsdk_xsc_program_vfetch(Xsc *x, XscProgram *p, uint32_t usage, uint32_t usage_index, const XscFetch *f)
{
	XscOp *const op = declare(x, p, XSC_OP_VFETCH, NULL, f);
	if (!op)
		return NULL;
	if (!xsdk_xsc_room(
			x, &p->vertex_elements, p->vertex_element_count, &p->vertex_element_capacity, sizeof *p->vertex_elements))
		return NULL;
	op->vertex_element = (int64_t)p->vertex_element_count;
	p->vertex_elements[p->vertex_element_count++] = (XscVertexElement){ 0, usage, usage_index, element_flags(usage) };
	return op;
}

static bool add_interpolator(Xsc *x, XscProgram *p, XscInterpolator it)
{
	if (!xsdk_xsc_room(
			x, &p->interpolators, p->interpolator_count, &p->interpolator_capacity, sizeof *p->interpolators))
		return false;
	p->interpolators[p->interpolator_count++] = it;
	return true;
}

XscOp *xsdk_xsc_program_export(Xsc *x, XscProgram *p, int64_t o, uint32_t usage, uint32_t usage_index, const XscAlu *f)
{
	XscAlu fields = *f;
	fields.vector_dest = o;
	XscOp *const op = xsdk_xsc_program_alu(x, p, &fields);
	if (!op)
		return NULL;
	const uint32_t flags = (uint32_t)p->interpolator_count << 4 | usage_nibble(usage);
	if (!add_interpolator(x, p, (XscInterpolator){ (uint32_t)o, usage, usage_index, flags }))
		return NULL;
	return op;
}

bool xsdk_xsc_program_alloc(Xsc *x, XscProgram *p, int64_t alloc_type)
{
	p->stages[p->stage_count - 1].alloc_after = alloc_type;
	p->alloc_count++;
	return add_stage(x, p);
}

bool xsdk_xsc_program_alloc_interpolators(Xsc *x, XscProgram *p)
{
	if (!xsdk_xsc_program_alloc(x, p, XSC_ALLOC_INTERPOLATORS))
		return false;
	p->export_stage = (int64_t)p->stage_count - 1;
	return true;
}

bool xsdk_xsc_program_input(Xsc *x, XscProgram *p, int64_t r, uint32_t usage)
{
	if (r != (int64_t)p->interpolator_count)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "declare inputs in r0,r1,... order");
	if (!add_interpolator(x, p, (XscInterpolator){ (uint32_t)r, usage, 0, usage_nibble(usage) }))
		return false;
	p->max_reg = highest(p->max_reg, r);
	return true;
}

bool xsdk_xsc_program_pixel_parameters(Xsc *x, XscProgram *p, int64_t r)
{
	if (r < (int64_t)p->interpolator_count || r >= MAX_INTERPOLATORS)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "the pixel parameters go past the inputs");
	p->param_gen = r;
	p->max_reg = highest(p->max_reg, r);
	return true;
}

bool xsdk_xsc_program_begin_loop(Xsc *x, XscProgram *p, uint32_t loop_id)
{
	if (p->stages[p->stage_count - 1].count && !add_stage(x, p))
		return false;
	p->loop_open = true;
	p->loop_open_stage = p->stage_count - 1;
	p->loop_open_id = loop_id;
	return true;
}

bool xsdk_xsc_program_predicate(Xsc *x, XscProgram *p, uint8_t predicate)
{
	const XscStage *const last = &p->stages[p->stage_count - 1];
	if ((last->count || last->alloc_after != XSC_NONE) && !add_stage(x, p))
		return false;
	p->stages[p->stage_count - 1].predicate = predicate;
	return true;
}

bool xsdk_xsc_program_end_loop(Xsc *x, XscProgram *p, bool predicated_break, bool break_condition)
{
	if (!xsdk_xsc_room(x, &p->loops, p->loop_count, &p->loop_capacity, sizeof *p->loops))
		return false;
	p->loops[p->loop_count++] =
		(XscLoop){ p->loop_open_stage, p->stage_count, p->loop_open_id, predicated_break, break_condition };
	p->loop_open = false;
	return add_stage(x, p);
}

// ---- layout ----

typedef struct {
	XscOp **ops;
	size_t count;
} Chunk;

// A stage's execs, MAX_EXEC_COUNT ops at most each; once the fetches are grouped, a stage whose ALU work runs into
// two fetches or more also splits where ALU and fetch runs meet (program.py _exec_chunks). Returns the count.
static size_t exec_chunks(const XscProgram *p, const XscStage *stage, Chunk *chunks)
{
	bool split = false;
	for (size_t i = 0; p->clauses && i + 2 < stage->count; i++) {
		if (!stage->ops[i]->is_fetch && stage->ops[i + 1]->is_fetch && stage->ops[i + 2]->is_fetch)
			split = true;
	}
	size_t count = 0;
	size_t run_start = 0;
	for (size_t i = 0; i <= stage->count; i++) {
		const bool run_ends =
			i == stage->count || (split && i > run_start && stage->ops[i]->is_fetch != stage->ops[i - 1]->is_fetch);
		if (!run_ends)
			continue;
		for (size_t at = run_start; at < i; at += MAX_EXEC_COUNT) {
			if (chunks)
				chunks[count] = (Chunk){ stage->ops + at, i - at < MAX_EXEC_COUNT ? i - at : MAX_EXEC_COUNT };
			count++;
		}
		run_start = i;
	}
	return count;
}

static size_t cf_entry_count(const XscProgram *p)
{
	size_t n = 0;
	for (size_t i = 0; i < p->stage_count; i++) {
		n += exec_chunks(p, &p->stages[i], NULL);
		n += p->stages[i].alloc_after != XSC_NONE;
	}
	return n + 2 * p->loop_count;
}

typedef struct {
	enum { CF_EXEC, CF_ALLOC, CF_LOOP_START, CF_LOOP_END } type;
	uint32_t opcode;
	int64_t address;
	int64_t count;
	int64_t sequence;
	int64_t vc;
	bool pred_clean;
	// An exec on the predicate, and the predicate it runs on.
	bool predicated;
	bool condition;
	int64_t alloc_type;
	uint32_t loop_id;
	int64_t skip;
	int64_t body;
	bool brk;
	bool cond;
} CfEntry;

static bool add_cf(Xsc *x, CfEntry **cf, size_t *count, size_t *capacity, CfEntry entry)
{
	if (!xsdk_xsc_room(x, cf, *count, capacity, sizeof **cf))
		return false;
	(*cf)[(*count)++] = entry;
	return true;
}

// The temps the GPU has, and who wrote one since the last serialized instruction: the ALU or a fetch.
enum { XSC_TEMPS = 128, XSC_WRITTEN_ALU = 1, XSC_WRITTEN_FETCH = 2, XSC_WRITTEN_ANY = 3 };

static void defs_uses(
	const XscProgram *p, const XscOp *op, int64_t *defs, size_t *def_count, int64_t *uses, size_t *use_count);

static bool any_written(const uint8_t written[XSC_TEMPS], const int64_t *temps, size_t count, uint8_t by)
{
	for (size_t i = 0; i < count; i++) {
		if (temps[i] >= 0 && temps[i] < XSC_TEMPS && (written[temps[i]] & by))
			return true;
	}
	return false;
}

// Within an exec the ALU and the fetches run apart, and an instruction waits for the other's results only when
// serialized: an ALU instruction that reads, or writes, a temp a fetch in the exec wrote since its last serialized
// instruction, and a fetch that reads one the ALU wrote. Unserialized, the console's ALU read a fetched index before
// the fetch had written it; the next exec found it written (docs/HARDWARE.md, "Serialized fetches"). A fetch reading
// what another fetch wrote, as a vertex shader's texture fetch may, is serialized too, unchecked. A serialized
// instruction waits for everything before it, so it clears what's tracked.
static CfEntry exec_entry(const XscProgram *p, const XscStage *stage, const Chunk *chunk, bool last)
{
	uint8_t written[XSC_TEMPS] = { 0 };
	int64_t seq = 0;
	int vertex_fetches = 0;
	bool pred_clean = true;
	for (size_t k = 0; k < chunk->count; k++) {
		const XscOp *const op = chunk->ops[k];
		int64_t defs[2], uses[3];
		size_t def_count, use_count;
		defs_uses(p, op, defs, &def_count, uses, &use_count);
		const bool waits = op->is_fetch ? any_written(written, uses, use_count, XSC_WRITTEN_ANY)
										: any_written(written, uses, use_count, XSC_WRITTEN_FETCH) ||
				any_written(written, defs, def_count, XSC_WRITTEN_FETCH);
		if (waits) {
			seq |= (int64_t)2 << (2 * k);
			memset(written, 0, XSC_TEMPS);
		}
		if (op->is_fetch)
			seq |= (int64_t)1 << (2 * k);
		if (op->kind == XSC_OP_VFETCH)
			vertex_fetches++;
		for (size_t i = 0; i < def_count; i++) {
			if (defs[i] >= 0 && defs[i] < XSC_TEMPS)
				written[defs[i]] |= op->is_fetch ? XSC_WRITTEN_FETCH : XSC_WRITTEN_ALU;
		}
		if (xsdk_xsc_modifies_predicate(op))
			pred_clean = false;
	}
	// A vertex shader's fetch-bearing exec carries a mask of its fetches, all six bits of it: the console drew a fifth
	// fetch wrong without its bit (docs/HARDWARE.md, "A fifth vertex fetch"). Its vertex fetches lead the exec; the
	// texture fetches after them take none, unchecked, as what the bits mean past vertex fetches isn't known.
	const int64_t vc = vertex_fetches ? ((int64_t)1 << vertex_fetches) - 1 : 0;
	const bool predicated = stage->predicate != XSC_PREDICATE_NONE;
	const uint32_t opcode = predicated ? XSC_CF_COND_EXEC_PRED : last ? XSC_CF_EXEC_END : XSC_CF_EXEC;
	return (CfEntry){ .type = CF_EXEC,
		.opcode = opcode,
		.predicated = predicated,
		.condition = stage->predicate == XSC_PREDICATE_TRUE,
		.address = chunk->ops[0]->slot,
		.count = (int64_t)chunk->count,
		.sequence = seq,
		.vc = vc,
		.pred_clean = pred_clean };
}

// The control flow entries, loop addresses patched once their indices are known.
static bool control_flow(Xsc *x, const XscProgram *p, CfEntry **cf, size_t *cf_count)
{
	size_t capacity = 0;
	size_t last_stage = SIZE_MAX, last_chunk = 0;
	for (size_t si = 0; si < p->stage_count; si++) {
		const size_t n = exec_chunks(p, &p->stages[si], NULL);
		if (n) {
			last_stage = si;
			last_chunk = n - 1;
		}
	}
	// A predicated exec that ends the program ends it only where it runs (xenia:src/xenia/gpu/ucode.h:125-130).
	if (last_stage != SIZE_MAX && p->stages[last_stage].predicate != XSC_PREDICATE_NONE)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "a program ending in a predicated exec");
	int64_t *const open = xsdk_xsc_alloc(x, (p->loop_count + 1) * sizeof *open);
	if (!open)
		return false;
	for (size_t si = 0; si < p->stage_count; si++) {
		for (size_t l = 0; l < p->loop_count; l++) {
			if (p->loops[l].start == si) {
				open[l] = (int64_t)*cf_count;
				if (!add_cf(x, cf, cf_count, &capacity,
						(CfEntry){ .type = CF_LOOP_START, .loop_id = p->loops[l].loop_id, .skip = XSC_NONE }))
					return false;
			}
		}
		const XscStage *const stage = &p->stages[si];
		Chunk *const chunks = xsdk_xsc_alloc(x, (exec_chunks(p, stage, NULL) + 1) * sizeof *chunks);
		if (!chunks)
			return false;
		const size_t n = exec_chunks(p, stage, chunks);
		for (size_t ci = 0; ci < n; ci++) {
			if (!add_cf(x, cf, cf_count, &capacity,
					exec_entry(p, stage, &chunks[ci], si == last_stage && ci == last_chunk)))
				return false;
		}
		if (stage->alloc_after != XSC_NONE &&
			!add_cf(x, cf, cf_count, &capacity, (CfEntry){ .type = CF_ALLOC, .alloc_type = stage->alloc_after }))
			return false;
		for (size_t l = 0; l < p->loop_count; l++) {
			if (p->loops[l].end - 1 != si)
				continue;
			const int64_t le = (int64_t)*cf_count;
			if (!add_cf(x, cf, cf_count, &capacity,
					(CfEntry){ .type = CF_LOOP_END,
						.loop_id = p->loops[l].loop_id,
						.body = open[l] + 1,
						.brk = p->loops[l].predicated_break,
						.cond = p->loops[l].break_condition }))
				return false;
			(*cf)[open[l]].skip = le + 1;
		}
	}
	return true;
}

static bool cf_pair(Xsc *x, const CfEntry *d, uint32_t pair[2])
{
	switch (d->type) {
	case CF_EXEC:
		if (d->predicated)
			return xsdk_xsc_build_cf_exec_pred(
				x, d->opcode, d->address, d->count, d->sequence, d->vc, d->pred_clean, d->condition, pair);
		return xsdk_xsc_build_cf_exec(x, d->opcode, d->address, d->count, d->sequence, d->vc, d->pred_clean, pair);
	case CF_ALLOC:
		return xsdk_xsc_build_cf_alloc(x, d->alloc_type, pair);
	case CF_LOOP_START:
		return xsdk_xsc_build_cf_loop_start(x, d->skip, d->loop_id, pair);
	default:
		return xsdk_xsc_build_cf_loop_end(x, d->body, d->loop_id, d->brk, d->cond, pair);
	}
}

// The microcode, big-endian: control flow pairs, then each op's slot (program.py _build_ucode).
static bool build_ucode(Xsc *x, XscProgram *p, uint8_t **ucode, size_t *ucode_bytes)
{
	const size_t n_cf = cf_entry_count(p);
	const size_t first_slot = (n_cf + 1) / 2;
	int64_t slot = (int64_t)first_slot;
	for (size_t si = 0; si < p->stage_count; si++) {
		for (size_t i = 0; i < p->stages[si].count; i++)
			p->stages[si].ops[i]->slot = slot++;
	}
	CfEntry *cf = NULL;
	size_t cf_count = 0;
	if (!control_flow(x, p, &cf, &cf_count))
		return false;
	const size_t pair_count = (cf_count + 1) / 2 * 2;
	if (pair_count / 2 != first_slot)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "CF layout self-check failed");
	const size_t dword_count = (size_t)slot * 3;
	uint32_t *const dwords = xsdk_xsc_alloc(x, (dword_count + 1) * sizeof *dwords);
	uint8_t *const bytes = xsdk_xsc_alloc(x, dword_count * 4 + 1);
	if (!dwords || !bytes)
		return false;
	for (size_t i = 0; i < pair_count; i += 2) {
		uint32_t a[2] = { 0, 0 }, b[2] = { 0, 0 };
		if (!cf_pair(x, &cf[i], a))
			return false;
		if (i + 1 < cf_count && !cf_pair(x, &cf[i + 1], b))
			return false;
		xsdk_xsc_pack_cf_pair(a, b, dwords + i / 2 * 3);
	}
	size_t at = first_slot * 3;
	for (size_t si = 0; si < p->stage_count; si++) {
		for (size_t i = 0; i < p->stages[si].count; i++) {
			const XscOp *const op = p->stages[si].ops[i];
			if (!op->encoded)
				return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "an instruction left unencoded");
			memcpy(dwords + at, op->triple, sizeof op->triple);
			at += 3;
		}
	}
	for (size_t i = 0; i < dword_count; i++) {
		bytes[4 * i] = (uint8_t)(dwords[i] >> 24);
		bytes[4 * i + 1] = (uint8_t)(dwords[i] >> 16);
		bytes[4 * i + 2] = (uint8_t)(dwords[i] >> 8);
		bytes[4 * i + 3] = (uint8_t)dwords[i];
	}
	*ucode = bytes;
	*ucode_bytes = dword_count * 4;
	return true;
}

// ---- fetches grouped after the work their coordinates need (program.py _group_fetches) ----

static bool state_fetch(const XscOp *op)
{
	return op->is_fetch && op->fetch.opcode >= XSC_FETCH_SET_LOD && op->fetch.opcode <= XSC_FETCH_SET_GRADIENTS_VERT;
}

// setTexLOD and the gradients' setters write the fetches' state, not a temp.
static void defs_uses(
	const XscProgram *p, const XscOp *op, int64_t *defs, size_t *def_count, int64_t *uses, size_t *use_count)
{
	*def_count = *use_count = 0;
	if (op->kind != XSC_OP_ALU) {
		if (!state_fetch(op))
			defs[(*def_count)++] = op->fetch.dest;
		uses[(*use_count)++] = op->fetch.src;
		return;
	}
	const XscAlu *const f = &op->alu;
	if (!f->is_export && f->vector_write_mask)
		defs[(*def_count)++] = f->vector_dest;
	if (f->scalar_write_mask && !f->is_export)
		defs[(*def_count)++] = f->scalar_dest;
	for (int i = 0; i < 3; i++) {
		if (f->src_is_temp[i])
			uses[(*use_count)++] = src_temp_index(p, f->src_reg[i]);
	}
}

static bool is_predicated(const XscOp *op)
{
	return op->kind == XSC_OP_ALU ? op->alu.is_predicated : op->fetch.is_predicated;
}

// Whether b, after a, must stay after it for what their registers don't show (program.py _ordered).
static bool ordered(const XscOp *a, const XscOp *b)
{
	if (xsdk_xsc_modifies_predicate(a) && (is_predicated(b) || xsdk_xsc_modifies_predicate(b)))
		return true;
	if (is_predicated(a) && xsdk_xsc_modifies_predicate(b))
		return true;
	if (a->kind == XSC_OP_ALU && b->kind == XSC_OP_ALU && a->alu.scalar_opcode != XSC_SCALAR_RETAIN_PREV &&
		b->alu.scalar_opcode != XSC_SCALAR_RETAIN_PREV)
		return true;
	if (a->is_fetch && b->is_fetch && (state_fetch(a) || state_fetch(b)))
		return true;
	return a->kind == XSC_OP_ALU && b->kind == XSC_OP_ALU && a->alu.is_export && b->alu.is_export;
}

static bool shares(const int64_t *a, size_t a_count, const int64_t *b, size_t b_count)
{
	for (size_t i = 0; i < a_count; i++) {
		for (size_t j = 0; j < b_count; j++) {
			if (a[i] == b[j])
				return true;
		}
	}
	return false;
}

typedef struct {
	size_t n;
	// after[j * n + i]: op j must follow op i; needs likewise, through the chain.
	uint8_t *after;
	uint8_t *needs;
	uint8_t *done;
	size_t *order;
	size_t order_count;
} Grouping;

static bool ready(const Grouping *g, size_t k)
{
	if (g->done[k])
		return false;
	for (size_t i = 0; i < g->n; i++) {
		if (g->after[k * g->n + i] && !g->done[i])
			return false;
	}
	return true;
}

static void take(Grouping *g, size_t k)
{
	g->done[k] = 1;
	g->order[g->order_count++] = k;
}

static bool fetch_left(const Grouping *g, XscOp *const *ops)
{
	for (size_t k = 0; k < g->n; k++) {
		if (ops[k]->is_fetch && !g->done[k])
			return true;
	}
	return false;
}

static void take_ready_round(Grouping *g, XscOp *const *ops, uint8_t *wanted)
{
	const size_t n = g->n;
	memset(wanted, 0, n);
	for (size_t k = 0; k < n; k++) {
		if (!ops[k]->is_fetch || g->done[k])
			continue;
		for (size_t i = 0; i < n; i++)
			wanted[i] |= g->needs[k * n + i];
	}
	const size_t taken = g->order_count;
	bool more = true;
	while (more) {
		more = false;
		for (size_t k = 0; k < n; k++) {
			if (wanted[k] && !ops[k]->is_fetch && ready(g, k)) {
				take(g, k);
				more = true;
			}
		}
	}
	for (size_t k = 0; k < n; k++) {
		if (ops[k]->is_fetch && ready(g, k))
			take(g, k);
	}
	// Never, but never stall.
	for (size_t k = 0; g->order_count == taken && k < n; k++) {
		if (ready(g, k))
			take(g, k);
	}
}

static bool group_stage(Xsc *x, const XscProgram *p, XscStage *stage)
{
	const size_t n = stage->count;
	Grouping g = { .n = n };
	g.after = xsdk_xsc_alloc(x, n * n + 1);
	g.needs = xsdk_xsc_alloc(x, n * n + 1);
	g.done = xsdk_xsc_alloc(x, n + 1);
	g.order = xsdk_xsc_alloc(x, (n + 1) * sizeof *g.order);
	uint8_t *const wanted = xsdk_xsc_alloc(x, n + 1);
	XscOp **const ops = xsdk_xsc_alloc(x, (n + 1) * sizeof *ops);
	if (!g.after || !g.needs || !g.done || !g.order || !wanted || !ops)
		return false;
	memcpy(ops, stage->ops, n * sizeof *ops);
	for (size_t j = 0; j < n; j++) {
		int64_t dj[MAX_DEFS], uj[MAX_USES];
		size_t dj_count, uj_count;
		defs_uses(p, ops[j], dj, &dj_count, uj, &uj_count);
		for (size_t i = 0; i < j; i++) {
			int64_t di[MAX_DEFS], ui[MAX_USES];
			size_t di_count, ui_count;
			defs_uses(p, ops[i], di, &di_count, ui, &ui_count);
			if (shares(di, di_count, uj, uj_count) || shares(ui, ui_count, dj, dj_count) ||
				shares(di, di_count, dj, dj_count) || ordered(ops[i], ops[j]))
				g.after[j * n + i] = 1;
		}
	}
	for (size_t j = 0; j < n; j++) {
		for (size_t i = 0; i < n; i++) {
			if (!g.after[j * n + i])
				continue;
			for (size_t k = 0; k < n; k++)
				g.needs[j * n + k] |= g.needs[i * n + k];
			g.needs[j * n + i] = 1;
		}
	}
	while (fetch_left(&g, ops))
		take_ready_round(&g, ops, wanted);
	for (size_t k = 0; k < n; k++) {
		if (!g.done[k])
			g.order[g.order_count++] = k;
	}
	for (size_t k = 0; k < n; k++)
		stage->ops[k] = ops[g.order[k]];
	return true;
}

static bool group_fetches(Xsc *x, XscProgram *p)
{
	for (size_t si = 0; si < p->stage_count; si++) {
		bool any_fetch = false;
		for (size_t i = 0; i < p->stages[si].count; i++)
			any_fetch = any_fetch || p->stages[si].ops[i]->is_fetch;
		if (any_fetch && !group_stage(x, p, &p->stages[si]))
			return false;
	}
	p->clauses = true;
	return true;
}

// ---- registers packed (program.py _allocate_registers) ----

typedef struct {
	int64_t limit;
	int64_t *first_def;
	int64_t *first_use;
	int64_t *last_use;
	int64_t *fetch_read;
	int64_t *start;
	int64_t *end;
} Ranges;

static int64_t *filled(Xsc *x, int64_t count, int64_t value)
{
	int64_t *const array = xsdk_xsc_alloc(x, (size_t)(count + 1) * sizeof *array);
	for (int64_t i = 0; array && i <= count; i++)
		array[i] = value;
	return array;
}

static XscOp **flat_ops(Xsc *x, const XscProgram *p, size_t *count, size_t **stage_of)
{
	*count = 0;
	for (size_t si = 0; si < p->stage_count; si++)
		*count += p->stages[si].count;
	XscOp **const ops = xsdk_xsc_alloc(x, (*count + 1) * sizeof *ops);
	*stage_of = xsdk_xsc_alloc(x, (*count + 1) * sizeof **stage_of);
	if (!ops || !*stage_of)
		return NULL;
	size_t at = 0;
	for (size_t si = 0; si < p->stage_count; si++) {
		for (size_t i = 0; i < p->stages[si].count; i++) {
			(*stage_of)[at] = si;
			ops[at++] = p->stages[si].ops[i];
		}
	}
	return ops;
}

static bool live_ranges(Xsc *x, const XscProgram *p, XscOp **ops, size_t count, const size_t *stage_of, Ranges *r)
{
	int64_t limit = 0;
	for (size_t idx = 0; idx < count; idx++) {
		int64_t d[MAX_DEFS], u[MAX_USES];
		size_t dc, uc;
		defs_uses(p, ops[idx], d, &dc, u, &uc);
		for (size_t i = 0; i < dc; i++)
			limit = highest(limit, d[i] + 1);
		for (size_t i = 0; i < uc; i++)
			limit = highest(limit, u[i] + 1);
	}
	r->limit = limit;
	r->first_def = filled(x, limit, XSC_NONE);
	r->first_use = filled(x, limit, XSC_NONE);
	r->last_use = filled(x, limit, XSC_NONE);
	r->fetch_read = filled(x, limit, XSC_NONE);
	if (!r->first_def || !r->first_use || !r->last_use || !r->fetch_read)
		return false;
	for (size_t idx = 0; idx < count; idx++) {
		int64_t d[MAX_DEFS], u[MAX_USES];
		size_t dc, uc;
		defs_uses(p, ops[idx], d, &dc, u, &uc);
		for (size_t i = 0; i < uc; i++) {
			if (r->first_use[u[i]] == XSC_NONE)
				r->first_use[u[i]] = (int64_t)idx;
			r->last_use[u[i]] = (int64_t)idx;
			if (ops[idx]->kind != XSC_OP_ALU && r->fetch_read[u[i]] == XSC_NONE)
				r->fetch_read[u[i]] = (int64_t)idx;
		}
		for (size_t i = 0; i < dc; i++) {
			if (r->first_def[d[i]] == XSC_NONE)
				r->first_def[d[i]] = (int64_t)idx;
			r->last_use[d[i]] = highest(r->last_use[d[i]], (int64_t)idx);
		}
	}
	if (!p->clauses)
		return true;
	// What a run of fetches reads stays in its register till the run ends.
	size_t run_end = 0;
	for (size_t idx = count; idx-- > 0;) {
		if (!ops[idx]->is_fetch)
			continue;
		const size_t next = idx + 1;
		const bool runs_on = next < count && ops[next]->is_fetch && stage_of[next] == stage_of[idx];
		run_end = runs_on ? run_end : idx;
		int64_t d[MAX_DEFS], u[MAX_USES];
		size_t dc, uc;
		defs_uses(p, ops[idx], d, &dc, u, &uc);
		for (size_t i = 0; i < uc; i++)
			r->last_use[u[i]] = highest(r->last_use[u[i]], (int64_t)run_end);
	}
	return true;
}

// Keeps a temp live across a loop's back edge for the whole loop, unless it lives within one pass.
static void stretch_over_loops(const XscProgram *p, Ranges *r, const int64_t *temps, size_t temp_count)
{
	const int64_t never = INT64_MAX;
	for (size_t l = 0; l < p->loop_count; l++) {
		int64_t lo = 0, hi = -1;
		for (size_t si = 0; si < p->loops[l].start; si++)
			lo += (int64_t)p->stages[si].count;
		for (size_t si = 0; si < p->loops[l].end; si++)
			hi += (int64_t)p->stages[si].count;
		for (size_t t = 0; t < temp_count; t++) {
			const int64_t reg = temps[t];
			const int64_t first_use = r->first_use[reg] == XSC_NONE ? never : r->first_use[reg];
			const bool within = lo <= r->start[reg] && r->end[reg] <= hi && first_use >= r->start[reg];
			if (!within && r->start[reg] <= hi && r->end[reg] >= lo) {
				r->start[reg] = r->start[reg] < lo ? r->start[reg] : lo;
				r->end[reg] = highest(r->end[reg], hi);
			}
		}
	}
}

static bool interval_before(const Ranges *r, int64_t a, int64_t b)
{
	if (r->start[a] != r->start[b])
		return r->start[a] < r->start[b];
	if (r->end[a] != r->end[b])
		return r->end[a] < r->end[b];
	return a < b;
}

static bool assign_registers(Xsc *x, Ranges *r, int64_t *temps, size_t temp_count, int64_t npinned, int64_t **mapping)
{
	for (size_t i = 1; i < temp_count; i++) {
		const int64_t moving = temps[i];
		size_t j = i;
		for (; j > 0 && interval_before(r, moving, temps[j - 1]); j--)
			temps[j] = temps[j - 1];
		temps[j] = moving;
	}
	const int64_t pool = npinned + (int64_t)temp_count + 1;
	uint8_t *const free_reg = xsdk_xsc_alloc(x, (size_t)pool + 1);
	int64_t *const active = xsdk_xsc_alloc(x, (temp_count + 1) * sizeof *active);
	*mapping = filled(x, r->limit, XSC_NONE);
	if (!free_reg || !active || !*mapping)
		return false;
	for (int64_t ph = npinned; ph < pool; ph++)
		free_reg[ph] = 1;
	size_t active_count = 0;
	for (size_t t = 0; t < temp_count; t++) {
		const int64_t reg = temps[t];
		size_t kept = 0;
		for (size_t a = 0; a < active_count; a++) {
			if (r->end[active[a]] < r->start[reg])
				free_reg[(*mapping)[active[a]]] = 1;
			else
				active[kept++] = active[a];
		}
		active_count = kept;
		int64_t ph = npinned;
		while (!free_reg[ph])
			ph++;
		free_reg[ph] = 0;
		(*mapping)[reg] = ph;
		active[active_count++] = reg;
	}
	return true;
}

// Whether the hardware fills a register before the program writes it: an interpolator, the pixel parameters, or the
// vertex index a vertex shader's fetches read.
static bool is_input(const Ranges *r, int64_t reg, int64_t never)
{
	const bool read_unwritten = r->last_use[reg] != XSC_NONE && r->first_def[reg] == XSC_NONE;
	const int64_t first_def = r->first_def[reg] == XSC_NONE ? never : r->first_def[reg];
	return read_unwritten || (r->fetch_read[reg] != XSC_NONE && r->fetch_read[reg] < first_def);
}

// Each temp a register of its own while it lives, the inputs kept where they are throughout.
static bool assign_whole(
	Xsc *x, const XscProgram *p, Ranges *r, int64_t never, int64_t npinned, int64_t **mapping, int64_t *most)
{
	int64_t *const temps = xsdk_xsc_alloc(x, (size_t)(r->limit + 1) * sizeof *temps);
	r->start = filled(x, r->limit, 0);
	r->end = filled(x, r->limit, 0);
	if (!temps || !r->start || !r->end)
		return false;
	size_t temp_count = 0;
	for (int64_t reg = 0; reg < r->limit; reg++) {
		if (r->first_def[reg] == XSC_NONE || is_input(r, reg, never))
			continue;
		temps[temp_count++] = reg;
		r->start[reg] = r->first_def[reg];
		r->end[reg] = r->last_use[reg];
	}
	stretch_over_loops(p, r, temps, temp_count);
	if (!assign_registers(x, r, temps, temp_count, npinned, mapping))
		return false;
	*most = npinned - 1;
	for (size_t t = 0; t < temp_count; t++)
		*most = highest(*most, (*mapping)[temps[t]]);
	return true;
}

// ---- values packed into components, in programs the optimiser rewrote ----

// A temp's value: each component it holds, live from its first write, or the program's start for an input, to its last
// read or write; where the allocator puts it, and the next value put in that register.
typedef struct {
	int64_t reg;
	bool input;
	// It keeps its components' lanes.
	bool fixed;
	uint8_t held;
	int64_t start[4];
	int64_t end[4];
	int64_t first;
	int64_t last;
	int64_t phys;
	uint8_t lanes[4];
	int64_t next;
} Value;

enum { REG_FIXED = 1, REG_WHOLE = 2 };

// The lanes a value's components may take, its own first.
static const uint8_t PERMUTATIONS[24][4] = { { 0, 1, 2, 3 }, { 0, 1, 3, 2 }, { 0, 2, 1, 3 }, { 0, 2, 3, 1 },
	{ 0, 3, 1, 2 }, { 0, 3, 2, 1 }, { 1, 0, 2, 3 }, { 1, 0, 3, 2 }, { 1, 2, 0, 3 }, { 1, 2, 3, 0 }, { 1, 3, 0, 2 },
	{ 1, 3, 2, 0 }, { 2, 0, 1, 3 }, { 2, 0, 3, 1 }, { 2, 1, 0, 3 }, { 2, 1, 3, 0 }, { 2, 3, 0, 1 }, { 2, 3, 1, 0 },
	{ 3, 0, 1, 2 }, { 3, 0, 2, 1 }, { 3, 1, 0, 2 }, { 3, 1, 2, 0 }, { 3, 2, 0, 1 }, { 3, 2, 1, 0 } };

// Each component's first write and last read or write, by key, a run of fetches keeping what it reads till the run
// ends, as live_ranges keeps registers.
static bool component_ranges(Xsc *x, const XscProgram *p, XscOp *const *ops, size_t count, const size_t *stage_of,
	int64_t limit, int64_t **written, int64_t **last)
{
	*written = filled(x, limit * 4, XSC_NONE);
	*last = filled(x, limit * 4, XSC_NONE);
	if (!*written || !*last)
		return false;
	size_t run_end = 0;
	for (size_t idx = count; idx-- > 0;) {
		const size_t next = idx + 1;
		const bool runs_on =
			ops[idx]->is_fetch && next < count && ops[next]->is_fetch && stage_of[next] == stage_of[idx];
		run_end = runs_on ? run_end : idx;
		int64_t keys[XSC_MAX_KEYS];
		const int64_t until = (int64_t)(ops[idx]->is_fetch && p->clauses ? run_end : idx);
		const size_t reads = xsdk_xsc_read_keys(ops[idx], keys);
		for (size_t k = 0; k < reads; k++)
			(*last)[keys[k]] = highest((*last)[keys[k]], until);
		const size_t writes = xsdk_xsc_write_keys(ops[idx], keys);
		for (size_t k = 0; k < writes; k++) {
			(*written)[keys[k]] = (int64_t)idx;
			(*last)[keys[k]] = highest((*last)[keys[k]], (int64_t)idx);
		}
	}
	return true;
}

// Which registers keep their lanes and which a fetch writes whole: a fetch's result and source stay as they are, and
// so does what an op that can't move its lanes writes.
static uint8_t *register_kinds(Xsc *x, XscOp *const *ops, size_t count, int64_t limit)
{
	uint8_t *const kinds = xsdk_xsc_alloc(x, (size_t)limit + 1);
	for (size_t idx = 0; kinds && idx < count; idx++) {
		const XscOp *const op = ops[idx];
		int64_t keys[XSC_MAX_KEYS];
		if (op->is_fetch) {
			kinds[op->fetch.src] |= REG_FIXED;
			if (xsdk_xsc_write_keys(op, keys))
				kinds[op->fetch.dest] |= REG_FIXED | REG_WHOLE;
		} else if (xsdk_xsc_lanes_fixed(op)) {
			kinds[op->alu.vector_dest] |= REG_FIXED;
		}
	}
	return kinds;
}

// A register's value, false if it holds none: a fetch's from its first write to its last read in all four components,
// an input's from the start in its own register.
static bool value_of(
	const Ranges *r, int64_t never, const int64_t *written, const int64_t *last, uint8_t kind, int64_t reg, Value *v)
{
	const bool input = is_input(r, reg, never);
	*v = (Value){ .reg = reg,
		.input = input,
		.fixed = input || (kind & REG_FIXED),
		.phys = XSC_NONE,
		.lanes = { 0, 1, 2, 3 },
		.first = INT64_MAX,
		.last = -1,
		.next = XSC_NONE };
	for (int c = 0; c < 4; c++) {
		const int64_t key = reg * 4 + c;
		if (input ? last[key] == XSC_NONE : written[key] == XSC_NONE)
			continue;
		v->held |= (uint8_t)(1 << c);
		v->start[c] = input ? -1 : written[key];
		v->end[c] = last[key];
		v->first = v->start[c] < v->first ? v->start[c] : v->first;
		v->last = highest(v->last, v->end[c]);
	}
	if (v->held && (kind & REG_WHOLE)) {
		v->held = 0xF;
		for (int c = 0; c < 4; c++) {
			v->start[c] = v->first;
			v->end[c] = v->last;
		}
	}
	return v->held != 0;
}

static bool overlap(const Value *a, int ca, const Value *b, int cb)
{
	return a->start[ca] <= b->end[cb] && b->start[cb] <= a->end[ca];
}

// Whether value v fits register phys beside the values already there, giving the lanes its components take.
static bool fits(const Value *values, const int64_t *heads, const Value *v, int64_t phys, uint8_t lanes[4])
{
	bool taken[4][4] = { { false } };
	for (int64_t w = heads[phys]; w != XSC_NONE; w = values[w].next) {
		for (int d = 0; d < 4; d++) {
			for (int c = 0; (values[w].held >> d & 1) && c < 4; c++)
				taken[c][values[w].lanes[d]] |= (v->held >> c & 1) && overlap(v, c, &values[w], d);
		}
	}
	for (size_t i = 0; i < (v->fixed ? 1 : 24); i++) {
		bool free = true;
		for (int c = 0; c < 4; c++)
			free = free && !((v->held >> c & 1) && taken[c][PERMUTATIONS[i][c]]);
		if (free) {
			memcpy(lanes, PERMUTATIONS[i], 4);
			return true;
		}
	}
	return false;
}

static bool value_before(const Value *a, const Value *b)
{
	if (a->first != b->first)
		return a->first < b->first;
	if (a->last != b->last)
		return a->last < b->last;
	return a->reg < b->reg;
}

// Puts each value, in the order they start, in the lowest register it fits, an input in its own.
static bool place_values(Xsc *x, Value *values, size_t value_count, int64_t limit)
{
	for (size_t i = 1; i < value_count; i++) {
		const Value moving = values[i];
		size_t j = i;
		for (; j > 0 && value_before(&moving, &values[j - 1]); j--)
			values[j] = values[j - 1];
		values[j] = moving;
	}
	int64_t *const heads = filled(x, limit + (int64_t)value_count, XSC_NONE);
	if (!heads)
		return false;
	for (size_t i = 0; i < value_count; i++) {
		Value *const v = &values[i];
		v->phys = v->input ? v->reg : 0;
		while (!v->input && !fits(values, heads, v, v->phys, v->lanes))
			v->phys++;
		v->next = heads[v->phys];
		heads[v->phys] = (int64_t)i;
	}
	return true;
}

// Packs values whose components don't overlap in time into one register's components, moving a value's components to
// other lanes where its writers and readers allow, and maps each register to where its value went.
static bool pack_registers(Xsc *x, const XscProgram *p, XscOp *const *ops, size_t count, const size_t *stage_of,
	const Ranges *r, int64_t npinned, int64_t **mapping, int64_t *most)
{
	const int64_t never = (int64_t)count;
	int64_t *written, *last;
	uint8_t *const kinds = register_kinds(x, ops, count, r->limit);
	Value *const values = xsdk_xsc_alloc(x, (size_t)(r->limit + 1) * sizeof *values);
	uint8_t (*const lanes)[4] = xsdk_xsc_alloc(x, (size_t)(r->limit + 1) * sizeof *lanes);
	*mapping = filled(x, r->limit, XSC_NONE);
	if (!kinds || !values || !lanes || !*mapping ||
		!component_ranges(x, p, ops, count, stage_of, r->limit, &written, &last))
		return false;
	size_t value_count = 0;
	for (int64_t reg = 0; reg < r->limit; reg++)
		value_count += value_of(r, never, written, last, kinds[reg], reg, &values[value_count]);
	if (!place_values(x, values, value_count, r->limit))
		return false;
	// Covers every interpolator and the pixel parameters, which arrive read or not, as the count always has.
	*most = highest(npinned - 1, p->pixel ? highest((int64_t)p->interpolator_count - 1, p->param_gen) : -1);
	for (int64_t reg = 0; reg < r->limit; reg++)
		memcpy(lanes[reg], PERMUTATIONS[0], 4);
	for (size_t i = 0; i < value_count; i++) {
		if (values[i].input)
			continue;
		(*mapping)[values[i].reg] = values[i].phys;
		memcpy(lanes[values[i].reg], values[i].lanes, 4);
		*most = highest(*most, values[i].phys);
	}
	for (size_t idx = 0; idx < count; idx++)
		xsdk_xsc_move_lanes(ops[idx], lanes, r->limit);
	return true;
}

static bool check_registers(Xsc *x, const XscProgram *p);

static bool allocate_registers(Xsc *x, XscProgram *p)
{
	size_t count;
	size_t *stage_of;
	XscOp **const ops = flat_ops(x, p, &count, &stage_of);
	Ranges r;
	if (!ops || !live_ranges(x, p, ops, count, stage_of, &r))
		return false;
	const int64_t never = (int64_t)count;
	int64_t npinned = 0;
	for (int64_t reg = 0; reg < r.limit; reg++) {
		if (is_input(&r, reg, never))
			npinned = reg + 1;
	}
	int64_t *mapping;
	int64_t most;
	if (p->optimized ? !pack_registers(x, p, ops, count, stage_of, &r, npinned, &mapping, &most)
					 : !assign_whole(x, p, &r, never, npinned, &mapping, &most))
		return false;
	// Past the budget, refused as such before an instruction is encoded with a register it can't name.
	if (most + 1 > p->registers || most + 1 > ENCODABLE_REGS) {
		p->max_reg = most;
		return check_registers(x, p);
	}
	for (size_t idx = 0; idx < count; idx++) {
		XscOp remapped;
		remap(p, ops[idx], mapping, r.limit, &remapped);
		*ops[idx] = remapped;
		if (!encode(x, ops[idx], ops[idx]->triple))
			return false;
		ops[idx]->encoded = true;
	}
	p->whole_sources = false;
	p->max_reg = most;
	return true;
}

static bool check_registers(Xsc *x, const XscProgram *p)
{
	const char *const kind = p->pixel ? "pixel" : "vertex";
	const int64_t need = p->max_reg + 1;
	if (need > p->registers)
		return xsdk_xsc_fail(x, XSDK_XSC_REGISTERS,
			"%s shader needs %lld registers (r0..r%lld), over the %u it may use; it would hang the GPU", kind,
			(long long)need, (long long)p->max_reg, (unsigned)p->registers);
	if (need > ENCODABLE_REGS)
		return xsdk_xsc_fail(x, XSDK_XSC_REGISTERS,
			"%s shader needs %lld registers (r0..r%lld); an instruction names r63 at most", kind, (long long)need,
			(long long)p->max_reg);
	return true;
}

// Rewrites the instructions if asked to and the program allows it, then encodes them again (program.py _optimize).
static bool optimize(Xsc *x, XscProgram *p)
{
	if (!p->optimize)
		return true;
	bool ran;
	if (!xsdk_xsc_optimize(x, p, &ran))
		return false;
	p->optimized = ran;
	for (size_t si = 0; ran && si < p->stage_count; si++) {
		for (size_t i = 0; i < p->stages[si].count; i++) {
			XscOp *const op = p->stages[si].ops[i];
			if (op->kind == XSC_OP_ALU && !encode_if_encodable(x, p, op))
				return false;
		}
	}
	return true;
}

static bool finish_vertex(Xsc *x, XscProgram *p, XscContainerParts *parts)
{
	parts->program_control = (uint32_t)p->max_reg | 1u << 16 |
		(uint32_t)(p->interpolator_count ? p->interpolator_count - 1 : 0) << 20 |
		(p->exports_point_size ? (uint32_t)VS_EXPORT_SPRITE << VS_EXPORT_MODE_SHIFT : 0);
	for (size_t si = 0; si < p->stage_count; si++) {
		for (size_t i = 0; i < p->stages[si].count; i++) {
			const XscOp *const op = p->stages[si].ops[i];
			if (op->vertex_element != XSC_NONE)
				p->vertex_elements[op->vertex_element].slot = (uint32_t)op->slot;
		}
	}
	if (p->export_stage == XSC_NONE)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "a vertex shader without interpolators allocated");
	const XscStage *const stage = &p->stages[p->export_stage];
	int64_t *const slots = xsdk_xsc_alloc(x, (stage->count + 1) * sizeof *slots);
	if (!slots)
		return false;
	for (size_t i = 0; i < stage->count; i++)
		slots[i] = stage->ops[i]->slot;
	parts->vertex_elements = p->vertex_elements;
	parts->vertex_element_count = p->vertex_element_count;
	parts->export_slots = slots;
	parts->export_slot_count = stage->count;
	return true;
}

bool xsdk_xsc_program_finish(Xsc *x, XscProgram *p, const XscConstant *constants, size_t constant_count,
	uint8_t **container, size_t *container_bytes)
{
	if (!optimize(x, p))
		return false;
	if (p->pixel && p->whole_sources && p->group_fetches && !group_fetches(x, p))
		return false;
	const int64_t past = p->pixel ? PIXEL_ALLOCATE_PAST : VERTEX_ALLOCATE_PAST;
	if ((p->whole_sources || p->max_reg + 1 > past) && !allocate_registers(x, p))
		return false;
	if (!check_registers(x, p))
		return false;
	XscContainerParts parts = { .pixel = p->pixel,
		.constants = constants,
		.constant_count = constant_count,
		.interpolators = p->interpolators,
		.interpolator_count = p->interpolator_count };
	if (!build_ucode(x, p, (uint8_t **)&parts.ucode, &parts.ucode_bytes))
		return false;
	if (!p->pixel) {
		if (!finish_vertex(x, p, &parts))
			return false;
		return xsdk_xsc_container(x, &parts, container, container_bytes);
	}
	// The parameters' xy are the pixel's only with SC_OUTPUT_SCREEN_XY too
	// (vulkan4xenon:results/2026-10-12-milestone-11-step-3/vulkan4xenon_diag.log, D2 and D4).
	const bool params = p->param_gen != XSC_NONE;
	// oC0 to the highest colour output, as targets from 0 take them.
	uint32_t colors = 0;
	while (p->color_outputs >> colors)
		colors++;
	const uint32_t export_mode = colors << PS_EXPORT_COLORS_SHIFT | (p->exports_depth ? PS_EXPORT_DEPTH : 0);
	parts.program_control = (uint32_t)p->max_reg << 8 | export_mode << PS_EXPORT_MODE_SHIFT | (params ? 1u << 18 : 0);
	parts.context_misc = 0x8 | (params ? (uint32_t)p->param_gen << 8 | 0x2 : 0);
	parts.field10 = 0x20000;
	parts.ps_field18 = (uint32_t)p->interpolator_count << 16 | 7;
	parts.pixel_outputs = p->color_outputs;
	return xsdk_xsc_container(x, &parts, container, container_bytes);
}
