#include <stdint.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Finds which kernel exports hold the return addresses of Aurora's blocked start-up (docs/HARDWARE.md, "A title's
// interrupt callback stops XAM's device") by asking the kernel for every export's address. It touches no hardware.

enum {
	// Past the highest ordinal Xenia lists (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc).
	ORDINAL_LIMIT = 0x400,
};

typedef struct {
	uint32_t ordinal;
	const char *name;
} NamedExport;

// Kernel return addresses on Aurora's main thread, from the outermost (the export Aurora called) inwards.
static const uint32_t return_addresses[] = { 0x800C7E4C, 0x800C7DFC, 0x800BD7E8 };

// Graphics exports a D3D title may use to take the GPU (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc:446-490).
static const NamedExport graphics_exports[] = {
	{ 0x1B0, "VdBlockUntilGUIIdle" },
	{ 0x1B1, "VdCallGraphicsNotificationRoutines" },
	{ 0x1B6, "VdEnableRingBufferRPtrWriteBack" },
	{ 0x1C2, "VdInitializeEngines" },
	{ 0x1C3, "VdInitializeRingBuffer" },
	{ 0x1C7, "VdPersistDisplay" },
	{ 0x1CD, "VdRegisterGraphicsNotification" },
	{ 0x1CE, "VdRegisterXamGraphicsNotification" },
	{ 0x1D5, "VdSetGraphicsInterruptCallback" },
	{ 0x1D9, "VdSetSystemCommandBufferGpuIdentifierAddress" },
	{ 0x1DC, "VdShutdownEngines" },
	{ 0x25B, "VdSwap" },
};

static uint32_t addresses[ORDINAL_LIMIT];

int main(void)
{
	xsdk_log_write("exports probe");
	HANDLE kernel = 0;
	const uint32_t error = XexGetModuleHandle("xboxkrnl.exe", &kernel);
	if (error != 0) {
		xsdk_log_write("exports: XexGetModuleHandle failed: %08X", (unsigned)error);
		return 1;
	}

	uint32_t resolved = 0;
	for (uint32_t ordinal = 1; ordinal < ORDINAL_LIMIT; ordinal++) {
		void *address = 0;
		if (XexGetProcedureAddress(kernel, ordinal, &address) >= 0 && address) {
			addresses[ordinal] = (uint32_t)address;
			resolved++;
		}
	}
	xsdk_log_write("exports: %u of %u ordinals resolve", (unsigned)resolved, (unsigned)(ORDINAL_LIMIT - 1));

	for (uint32_t i = 0; i < sizeof graphics_exports / sizeof graphics_exports[0]; i++) {
		const NamedExport *named = &graphics_exports[i];
		xsdk_log_write(
			"exports: %03X %s at %08X", (unsigned)named->ordinal, named->name, (unsigned)addresses[named->ordinal]);
	}

	// The export just below a return address is the likeliest function holding it; the one just above bounds it.
	for (uint32_t i = 0; i < sizeof return_addresses / sizeof return_addresses[0]; i++) {
		const uint32_t target = return_addresses[i];
		uint32_t below = 0;
		uint32_t above = 0;
		for (uint32_t ordinal = 1; ordinal < ORDINAL_LIMIT; ordinal++) {
			const uint32_t address = addresses[ordinal];
			if (!address)
				continue;
			if (address <= target && (!below || address > addresses[below]))
				below = ordinal;
			if (address > target && (!above || address < addresses[above]))
				above = ordinal;
		}
		xsdk_log_write("exports: %08X: export below is %03X at %08X (+0x%X), export above is %03X at %08X",
			(unsigned)target, (unsigned)below, (unsigned)addresses[below], (unsigned)(target - addresses[below]),
			(unsigned)above, (unsigned)addresses[above]);
	}
	return 0;
}
