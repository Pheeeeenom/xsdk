#include "xsdk/kernel_entry.inc"

// The kernel calls it with the thread's start address and context (xenia:src/xenia/kernel/xthread.cc:518-521).
	XSDK_KERNEL_ENTRY xsdk_thread_entry, xsdk_thread_run
