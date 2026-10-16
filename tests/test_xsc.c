#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tests.h"
#include "xsc_shaders.h"
#include "xsdk/log.h"
#include "xsdk/xsc.h"

// xost's compiler on the console (docs/scope/shader-compiling.md, step 2): shaders compiled from their SPIR-V into what
// it makes from them on the PC (tools/gen_xsc_shaders.py).

enum {
	NAME_BYTES = 128,
};

static bool same_needs(const XscTestShader *want, const XsdkXscShader *got)
{
	if (got->need_count != want->need_count)
		return false;
	for (size_t i = 0; i < got->need_count; i++) {
		const XsdkXscNeed *const a = &got->needs[i];
		const XsdkXscNeed *const b = &want->needs[i];
		if (a->kind != b->kind || a->reg != b->reg || a->regs != b->regs || a->name != NULL || a->block != b->block ||
			a->set != b->set || a->binding != b->binding || a->offset_bytes != b->offset_bytes)
			return false;
		for (int c = 0; c < 4; c++) {
			if (a->value[c] != b->value[c])
				return false;
		}
	}
	return true;
}

static bool same_resources(const XscTestShader *want, const XsdkXscShader *got)
{
	if (got->resource_count != want->resource_count)
		return false;
	for (size_t i = 0; i < got->resource_count; i++) {
		const XsdkXscResource *const a = &got->resources[i];
		const XsdkXscResource *const b = &want->resources[i];
		if (a->kind != b->kind || a->set != b->set || a->binding != b->binding || a->reg != b->reg ||
			a->regs != b->regs)
			return false;
	}
	return true;
}

static void check_shader(const XscTestShader *want)
{
	XsdkXscOptions options;
	xsdk_xsc_default_options(&options);
	options.optimize = true;
	XsdkXscShader got;
	const XsdkXscStatus status = xsdk_xsc_compile(want->spirv, want->spirv_words, &options, &got);
	if (status != XSDK_XSC_OK)
		xsdk_log_write("xsc: %s: status %d, %s", want->name, status, got.message);
	const bool same_container = status == XSDK_XSC_OK &&
		got.container_bytes == want->container_words * sizeof *want->container &&
		memcmp(got.container, want->container, got.container_bytes) == 0;
	char name[NAME_BYTES];
	(void)snprintf(name, sizeof name, "xsc: %s compiles into the PC's container, literals and resources", want->name);
	check(name, same_container && same_needs(want, &got) && same_resources(want, &got));
	xsdk_xsc_free(&got);
}

void test_xsc(void)
{
	for (size_t i = 0; i < sizeof xsc_test_shaders / sizeof xsc_test_shaders[0]; i++)
		check_shader(&xsc_test_shaders[i]);
}
