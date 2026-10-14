#ifndef XSDK_XSC_H
#define XSDK_XSC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// xost's compiler: Vulkan or OpenGL SPIR-V to a shader container (xenos-toolchain/docs/xsc.md).

typedef enum {
	XSDK_XSC_OK,
	// xost doesn't lower something the shader does.
	XSDK_XSC_REFUSED,
	// The shader needs more registers than its kind's share of the split.
	XSDK_XSC_REGISTERS,
	// A value doesn't fit the field an instruction or the SPIR-V gives it.
	XSDK_XSC_BAD_VALUE,
	// The SPIR-V isn't one xost understands well enough to say why.
	XSDK_XSC_MALFORMED,
	XSDK_XSC_OUT_OF_MEMORY,
} XsdkXscStatus;

enum {
	XSDK_XSC_MESSAGE_BYTES = 512,
};

typedef struct {
	// Each kind of shader's share of the split, from r0.
	uint32_t pixel_registers;
	uint32_t vertex_registers;
	// Rewrites the instructions before allocation (xenos-toolchain/xasm/optimize.py).
	bool optimize;
	// Lays a pixel shader's fetches out together, after their coordinates; off, as before milestone 36.
	bool group_fetches;
} XsdkXscOptions;

// What a shader needs the runtime to set besides its blocks and samplers.
typedef enum {
	// value in constant register reg.
	XSDK_XSC_LITERAL,
	// The uniform array name, regs registers from reg, that a hardware loop indexes.
	XSDK_XSC_ARRAY,
	// The loop count for loop constant reg, from the uniform name.
	XSDK_XSC_LOOP,
} XsdkXscNeedKind;

typedef struct {
	XsdkXscNeedKind kind;
	uint32_t reg;
	uint32_t regs;
	double value[4];
	const char *name;
} XsdkXscNeed;

typedef enum {
	XSDK_XSC_PUSH_CONSTANTS,
	XSDK_XSC_UNIFORM_BUFFER,
	XSDK_XSC_SAMPLER,
} XsdkXscResourceKind;

// Where a block's registers or a sampler's fetch constant landed.
typedef struct {
	XsdkXscResourceKind kind;
	uint32_t set;
	uint32_t binding;
	uint32_t reg;
	uint32_t regs;
} XsdkXscResource;

typedef struct {
	uint8_t *container;
	size_t container_bytes;
	XsdkXscNeed *needs;
	size_t need_count;
	XsdkXscResource *resources;
	size_t resource_count;
	// Why it failed, if it did.
	char message[XSDK_XSC_MESSAGE_BYTES];
} XsdkXscShader;

void xsdk_xsc_default_options(XsdkXscOptions *options);
// Compiles spirv; on success shader holds what xsdk_xsc_free releases.
XsdkXscStatus xsdk_xsc_compile(
	const uint32_t *spirv, size_t spirv_words, const XsdkXscOptions *options, XsdkXscShader *shader);
void xsdk_xsc_free(XsdkXscShader *shader);

#ifdef __cplusplus
}
#endif

#endif
