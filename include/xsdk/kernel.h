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

// The kernel owns the layout (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc:369-376).
typedef struct {
	uint32_t opaque[7];
} RTL_CRITICAL_SECTION;

// xenia:src/xenia/kernel/info/file.h:98-108
typedef struct {
	uint64_t creation_time;
	uint64_t last_access_time;
	uint64_t last_write_time;
	uint64_t change_time;
	uint64_t allocation_size;
	uint64_t end_of_file;
	uint32_t attributes;
	uint32_t pad;
} FILE_NETWORK_OPEN_INFORMATION;

// xenia:src/xenia/hid/input.h:57-72
typedef struct {
	uint16_t buttons;
	uint8_t left_trigger;
	uint8_t right_trigger;
	int16_t thumb_lx;
	int16_t thumb_ly;
	int16_t thumb_rx;
	int16_t thumb_ry;
} INPUT_GAMEPAD;

typedef struct {
	uint32_t packet_number;
	INPUT_GAMEPAD gamepad;
} INPUT_STATE;

// The kernel calls back with a pointer to a word holding context (xenia:src/xenia/apu/audio_system.cc:189-192).
typedef struct {
	void (*callback)(const uint32_t *context);
	uint32_t context;
} XAUDIO_RENDER_DRIVER_CLIENT;

// xenia:src/xenia/xbox.h:39-61
enum {
	STATUS_SUCCESS = 0x00000000,
	STATUS_TIMEOUT = 0x00000102,
	STATUS_NOT_IMPLEMENTED = (int32_t)0xC0000002,
	STATUS_INVALID_HANDLE = (int32_t)0xC0000008,
	STATUS_INVALID_PARAMETER = (int32_t)0xC000000D,
	STATUS_NO_SUCH_FILE = (int32_t)0xC000000F,
	STATUS_END_OF_FILE = (int32_t)0xC0000011,
	STATUS_NO_MEMORY = (int32_t)0xC0000017,
	STATUS_ACCESS_DENIED = (int32_t)0xC0000022,
	STATUS_OBJECT_NAME_NOT_FOUND = (int32_t)0xC0000034,
	STATUS_OBJECT_NAME_COLLISION = (int32_t)0xC0000035,
};

// Win32 errors that XAM returns (xenia:src/xenia/xbox.h:86-100).
enum {
	ERROR_SUCCESS = 0,
	ERROR_DEVICE_NOT_CONNECTED = 0x48F,
};

// Root directory for paths like "game:\file" (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:129).
static const HANDLE OB_DOS_DEVICES = 0xFFFFFFFD;

// xenia:src/xenia/xbox.h:126-152
enum {
	MEM_COMMIT = 0x00001000,
	MEM_RESERVE = 0x00002000,
	PAGE_READWRITE = 0x00000004,
};

// xenia:src/xenia/base/filesystem.h:64-73, src/xenia/vfs/entry.h:47-60, src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:30-40
enum {
	GENERIC_READ = (int32_t)0x80000000,
	GENERIC_WRITE = 0x40000000,
	// Synchronous IO needs it (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:152); the value is NT's.
	SYNCHRONIZE = 0x00100000,
	FILE_ATTRIBUTE_NORMAL = 0x00000080,
	FILE_OPEN = 1,
	FILE_CREATE = 2,
	FILE_OPEN_IF = 3,
	FILE_OVERWRITE = 4,
	FILE_OVERWRITE_IF = 5,
	FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020,
	FILE_NON_DIRECTORY_FILE = 0x00000040,
	FILE_NETWORK_OPEN_INFORMATION_CLASS = 34,
};

// Event types (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:470) and processor modes; the values are NT's.
enum {
	NOTIFICATION_EVENT = 0,
	SYNCHRONIZATION_EVENT = 1,
	KERNEL_MODE = 0,
	USER_MODE = 1,
};

static inline bool NT_SUCCESS(NTSTATUS status)
{
	return status >= 0;
}

// xboxkrnl.exe
NTSTATUS DbgPrint(const char *format, ...);
NTSTATUS ExCreateThread(HANDLE *thread, uint32_t stack_bytes, uint32_t *thread_id, void *xapi_thread_startup,
	void *start_address, void *start_context, uint32_t creation_flags);
void ExTerminateThread(uint32_t exit_code);
NTSTATUS KeDelayExecutionThread(uint32_t processor_mode, uint32_t alertable, const int64_t *interval);
void *MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size_bytes, uint32_t protect, uint32_t min_address,
	uint32_t max_address, uint32_t alignment);
void MmFreePhysicalMemory(uint32_t type, void *base);
NTSTATUS NtAllocateVirtualMemory(
	void **base, uint32_t *size_bytes, uint32_t allocation_type, uint32_t protect, uint32_t debug_memory);
NTSTATUS NtClose(HANDLE handle);
NTSTATUS NtCreateEvent(HANDLE *event, const OBJECT_ATTRIBUTES *attributes, uint32_t event_type, uint32_t initial_state);
NTSTATUS NtCreateFile(HANDLE *handle, uint32_t desired_access, const OBJECT_ATTRIBUTES *attributes,
	IO_STATUS_BLOCK *io_status, const uint64_t *allocation_size, uint32_t file_attributes, uint32_t share_access,
	uint32_t creation_disposition, uint32_t create_options);
NTSTATUS NtCreateSemaphore(HANDLE *semaphore, const OBJECT_ATTRIBUTES *attributes, int32_t count, int32_t limit);
NTSTATUS NtQueryInformationFile(
	HANDLE file, IO_STATUS_BLOCK *io_status, void *information, uint32_t length_bytes, uint32_t information_class);
NTSTATUS NtReadFile(HANDLE file, HANDLE event, void *apc_routine, void *apc_context, IO_STATUS_BLOCK *io_status,
	void *buffer, uint32_t length_bytes, const uint64_t *byte_offset);
NTSTATUS NtReleaseSemaphore(HANDLE semaphore, int32_t release_count, int32_t *previous_count);
NTSTATUS NtSetEvent(HANDLE event, int32_t *previous_state);
NTSTATUS NtWaitForSingleObjectEx(HANDLE object, uint32_t wait_mode, uint32_t alertable, const int64_t *timeout);
NTSTATUS NtWriteFile(HANDLE file, HANDLE event, void *apc_routine, void *apc_context, IO_STATUS_BLOCK *io_status,
	const void *buffer, uint32_t length_bytes, const uint64_t *byte_offset);
void RtlEnterCriticalSection(RTL_CRITICAL_SECTION *section);
void RtlInitializeCriticalSection(RTL_CRITICAL_SECTION *section);
void RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *section);
uint32_t XAudioRegisterRenderDriverClient(const XAUDIO_RENDER_DRIVER_CLIENT *client, uint32_t *driver);
uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, const float *samples);
uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver);

// xam.xex
uint32_t XamInputGetState(uint32_t user_index, uint32_t flags, INPUT_STATE *state);
void XamLoaderTerminateTitle(void);

#endif
