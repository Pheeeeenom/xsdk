#ifndef XSDK_KERNEL_H
#define XSDK_KERNEL_H

#include <stdbool.h>
#include <stdint.h>

// Types, constants and imports as xenia:src/xenia/xbox.h and the shims in src/xenia/kernel/ define them.

typedef int32_t NTSTATUS;
typedef uint32_t HANDLE;

typedef struct {
	uint16_t length;
	uint16_t maximum_length;
	const char *pointer;
} ANSI_STRING;

typedef struct {
	HANDLE root_directory;
	const ANSI_STRING *name;
	uint32_t attributes;
} OBJECT_ATTRIBUTES;

typedef struct {
	NTSTATUS status;
	uint32_t information;
} IO_STATUS_BLOCK;

enum {
	STATUS_SUCCESS = 0x00000000,
	STATUS_NOT_IMPLEMENTED = (int32_t)0xC0000002,
};

// Root directory for paths like "game:\file" (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:129).
static const HANDLE OB_DOS_DEVICES = 0xFFFFFFFD;

enum {
	MEM_COMMIT = 0x00001000,
	MEM_RESERVE = 0x00002000,
	PAGE_READWRITE = 0x00000004,
};

enum {
	GENERIC_WRITE = 0x40000000,
	// Synchronous IO needs it (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:152); the value is NT's.
	SYNCHRONIZE = 0x00100000,
	FILE_ATTRIBUTE_NORMAL = 0x00000080,
	FILE_OVERWRITE_IF = 5,
	FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020,
	FILE_NON_DIRECTORY_FILE = 0x00000040,
};

static inline bool NT_SUCCESS(NTSTATUS status)
{
	return status >= 0;
}

// xboxkrnl.exe
NTSTATUS DbgPrint(const char *format, ...);
NTSTATUS NtAllocateVirtualMemory(
	void **base, uint32_t *size_bytes, uint32_t allocation_type, uint32_t protect, uint32_t debug_memory);
NTSTATUS NtClose(HANDLE handle);
NTSTATUS NtCreateFile(HANDLE *handle, uint32_t desired_access, const OBJECT_ATTRIBUTES *attributes,
	IO_STATUS_BLOCK *io_status, const uint64_t *allocation_size, uint32_t file_attributes, uint32_t share_access,
	uint32_t creation_disposition, uint32_t create_options);
NTSTATUS NtWriteFile(HANDLE file, HANDLE event, void *apc_routine, void *apc_context, IO_STATUS_BLOCK *io_status,
	const void *buffer, uint32_t length_bytes, const uint64_t *byte_offset);

// xam.xex
void XamLoaderTerminateTitle(void);

#endif
