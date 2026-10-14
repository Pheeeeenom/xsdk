#include <stdlib.h>
#include <string.h>

#include "xsc_lower.h"

// xost's compiler: SPIR-V in, a container and what it needs out, as xenos-toolchain/xsc/lower.py's lower_ex gives them.

enum {
	// xsdk's split of the registers (xenos-toolchain/xasm/program.py XSDK_REGISTERS).
	XSDK_PIXEL_REGISTERS = 64,
	XSDK_VERTEX_REGISTERS = 64,
	MODEL_VERTEX = 0,
};

// make works it out (Makefile, XSC_VERSION).
#ifndef XSDK_XSC_SOURCE_HASH
#error "XSDK_XSC_SOURCE_HASH names the compiler's version"
#endif

const char *xsdk_xsc_version(void)
{
	return XSDK_XSC_SOURCE_HASH;
}

void xsdk_xsc_default_options(XsdkXscOptions *options)
{
	*options = (XsdkXscOptions){ .pixel_registers = XSDK_PIXEL_REGISTERS,
		.vertex_registers = XSDK_VERTEX_REGISTERS,
		.optimize = false,
		.group_fetches = true };
}

static bool has_loop(const XscModule *m)
{
	for (size_t i = 0; i < m->instr_count; i++) {
		if (m->instrs[i].opcode == SPV_LOOP_MERGE)
			return true;
	}
	return false;
}

// Loops unrolled, or a pixel shader's loop that won't unroll as a hardware loop.
static bool lower(Xsc *x, XscModule *m, const XsdkXscOptions *options, XscInterface *si, XscLowered *out)
{
	if (!m->entry_point_count)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "list index out of range");
	const bool pixel = m->entry_points[0].model != MODEL_VERTEX;
	bool refused;
	if (!xsdk_xsc_unroll(x, m, &refused)) {
		if (!refused || !pixel || !has_loop(m))
			return false;
		x->status = XSDK_XSC_OK;
		x->message[0] = '\0';
		return xsdk_xsc_lower_real_loop(x, m, options->pixel_registers, si, out);
	}
	if (pixel)
		return xsdk_xsc_lower_fragment(x, m, options, si, out);
	return xsdk_xsc_lower_vertex(x, m, options, si, out);
}

static uint32_t or_zero(int64_t value)
{
	return value == XSC_NONE ? 0 : (uint32_t)value;
}

// Where each block's registers and each sampler's fetch constant landed: blocks first, then samplers.
static XsdkXscResource *resources(const XscInterface *si, size_t *count)
{
	*count = si->block_count + si->sampler_count;
	XsdkXscResource *const out = calloc(*count ? *count : 1, sizeof *out);
	if (!out)
		return NULL;
	for (size_t i = 0; i < si->block_count; i++) {
		const XscBlock *const b = &si->blocks[i];
		out[i] = (XsdkXscResource){ b->push ? XSDK_XSC_PUSH_CONSTANTS : XSDK_XSC_UNIFORM_BUFFER,
			or_zero(b->var->descriptor_set), or_zero(b->var->binding), b->reg, b->regs };
	}
	for (size_t i = 0; i < si->sampler_count; i++) {
		const XscVarUse *const s = &si->samplers[i];
		out[si->block_count + i] =
			(XsdkXscResource){ XSDK_XSC_SAMPLER, or_zero(s->var->descriptor_set), or_zero(s->var->binding), s->a, 1 };
	}
	return out;
}

// The needs and their names in one allocation, as the arena they're in goes.
static XsdkXscNeed *copy_needs(const XscLowered *lowered)
{
	size_t name_bytes = 0;
	for (size_t i = 0; i < lowered->need_count; i++)
		name_bytes += lowered->needs[i].name ? strlen(lowered->needs[i].name) + 1 : 0;
	const size_t need_bytes = lowered->need_count * sizeof *lowered->needs;
	XsdkXscNeed *const needs = malloc(need_bytes + name_bytes + 1);
	if (!needs)
		return NULL;
	if (need_bytes)
		memcpy(needs, lowered->needs, need_bytes);
	char *names = (char *)needs + need_bytes;
	for (size_t i = 0; i < lowered->need_count; i++) {
		if (!needs[i].name)
			continue;
		const size_t length = strlen(needs[i].name) + 1;
		memcpy(names, needs[i].name, length);
		needs[i].name = names;
		names += length;
	}
	return needs;
}

static bool copy_out(Xsc *x, const XscInterface *si, const XscLowered *lowered, XsdkXscShader *shader)
{
	shader->container = malloc(lowered->container_bytes ? lowered->container_bytes : 1);
	shader->needs = copy_needs(lowered);
	shader->resources = resources(si, &shader->resource_count);
	if (!shader->container || !shader->needs || !shader->resources)
		return xsdk_xsc_fail(x, XSDK_XSC_OUT_OF_MEMORY, "out of memory");
	memcpy(shader->container, lowered->container, lowered->container_bytes);
	shader->container_bytes = lowered->container_bytes;
	shader->need_count = lowered->need_count;
	return true;
}

XsdkXscStatus xsdk_xsc_compile(
	const uint32_t *spirv, size_t spirv_words, const XsdkXscOptions *options, XsdkXscShader *shader)
{
	memset(shader, 0, sizeof *shader);
	Xsc x = { .status = XSDK_XSC_OK };
	XscModule m;
	XscInterface si;
	XscLowered lowered = { 0 };
	const bool ok = xsdk_xsc_spirv_parse(&x, spirv, spirv_words, &m) && lower(&x, &m, options, &si, &lowered) &&
		copy_out(&x, &si, &lowered, shader);
	if (!ok && x.status == XSDK_XSC_OK)
		xsdk_xsc_fail(&x, XSDK_XSC_MALFORMED, "failed without an error");
	if (x.status != XSDK_XSC_OK)
		xsdk_xsc_free(shader);
	memcpy(shader->message, x.message, sizeof shader->message);
	xsdk_xsc_release(&x);
	return x.status;
}

void xsdk_xsc_free(XsdkXscShader *shader)
{
	free(shader->container);
	free(shader->needs);
	free(shader->resources);
	shader->container = NULL;
	shader->needs = NULL;
	shader->resources = NULL;
	shader->container_bytes = shader->need_count = shader->resource_count = 0;
}
