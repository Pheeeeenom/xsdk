#ifndef XSDK_KERNEL_H
#define XSDK_KERNEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

// One NtQueryDirectoryFile entry (xenia:src/xenia/kernel/xfile.h:36-49).
typedef struct {
	uint32_t next_entry_offset;
	uint32_t file_index;
	uint64_t creation_time;
	uint64_t last_access_time;
	uint64_t last_write_time;
	uint64_t change_time;
	uint64_t end_of_file;
	uint64_t allocation_size;
	uint32_t attributes;
	uint32_t file_name_length;
	char file_name[];
} FILE_DIRECTORY_INFORMATION;

// xenia:src/xenia/kernel/info/file.h:68-83
typedef struct {
	uint8_t delete_file;
} FILE_DISPOSITION_INFORMATION;

typedef struct {
	uint64_t end_of_file;
} FILE_END_OF_FILE_INFORMATION;

// INPUT_GAMEPAD's buttons (xenia:src/xenia/hid/input.h:32-46).
enum {
	INPUT_GAMEPAD_DPAD_UP = 0x0001,
	INPUT_GAMEPAD_DPAD_DOWN = 0x0002,
	INPUT_GAMEPAD_DPAD_LEFT = 0x0004,
	INPUT_GAMEPAD_DPAD_RIGHT = 0x0008,
	INPUT_GAMEPAD_START = 0x0010,
	INPUT_GAMEPAD_BACK = 0x0020,
	INPUT_GAMEPAD_LEFT_THUMB = 0x0040,
	INPUT_GAMEPAD_RIGHT_THUMB = 0x0080,
	INPUT_GAMEPAD_LEFT_SHOULDER = 0x0100,
	INPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200,
	INPUT_GAMEPAD_GUIDE = 0x0400,
	INPUT_GAMEPAD_A = 0x1000,
	INPUT_GAMEPAD_B = 0x2000,
	INPUT_GAMEPAD_X = 0x4000,
	INPUT_GAMEPAD_Y = 0x8000,
};

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

// xenia:src/xenia/hid/input.h:74-77
typedef struct {
	uint16_t left_motor_speed;
	uint16_t right_motor_speed;
} INPUT_VIBRATION;

// xenia:src/xenia/xbox.h:257-269
typedef struct {
	uint32_t display_width;
	uint32_t display_height;
	uint32_t is_interlaced;
	uint32_t is_widescreen;
	uint32_t is_hi_def;
	float refresh_rate;
	uint32_t video_standard;
	uint32_t reserved[5];
} VIDEO_MODE;

// xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:88-105
typedef struct {
	uint16_t front_buffer_width;
	uint16_t front_buffer_height;
	uint8_t front_buffer_color_format;
	uint8_t front_buffer_pixel_format;
	uint32_t scaler_parameters[14];
	uint16_t display_window_overscan[4];
	uint16_t display_width;
	uint16_t display_height;
	float display_refresh_rate;
	uint32_t display_interlaced;
	uint8_t display_color_format;
	uint16_t actual_display_width;
} DISPLAY_INFO;

// Pages are 4 KB here (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:451-473, 496).
typedef struct {
	uint32_t available_pages;
	uint32_t total_virtual_memory_bytes;
	uint32_t reserved_virtual_memory_bytes;
	uint32_t physical_pages;
	uint32_t pool_pages;
	uint32_t stack_pages;
	uint32_t image_pages;
	uint32_t heap_pages;
	uint32_t virtual_pages;
	uint32_t page_table_pages;
	uint32_t cache_pages;
} MM_QUERY_STATISTICS_SECTION;

typedef struct {
	uint32_t size;
	uint32_t total_physical_pages;
	uint32_t kernel_pages;
	MM_QUERY_STATISTICS_SECTION title;
	MM_QUERY_STATISTICS_SECTION system;
	uint32_t highest_physical_page;
} MM_QUERY_STATISTICS_RESULT;

// The kernel calls back with a pointer to a word holding context (xenia:src/xenia/apu/audio_system.cc:189-192).
typedef struct {
	void (*callback)(const uint32_t *context);
	uint32_t context;
} XAUDIO_RENDER_DRIVER_CLIENT;

// xenia:src/xenia/xbox.h:39-68
enum {
	STATUS_SUCCESS = 0x00000000,
	STATUS_TIMEOUT = 0x00000102,
	STATUS_NO_MORE_FILES = (int32_t)0x80000006,
	STATUS_UNSUCCESSFUL = (int32_t)0xC0000001,
	STATUS_NOT_IMPLEMENTED = (int32_t)0xC0000002,
	STATUS_INVALID_HANDLE = (int32_t)0xC0000008,
	STATUS_INVALID_PARAMETER = (int32_t)0xC000000D,
	STATUS_NO_SUCH_FILE = (int32_t)0xC000000F,
	STATUS_END_OF_FILE = (int32_t)0xC0000011,
	STATUS_NO_MEMORY = (int32_t)0xC0000017,
	STATUS_ACCESS_DENIED = (int32_t)0xC0000022,
	STATUS_BUFFER_TOO_SMALL = (int32_t)0xC0000023,
	STATUS_OBJECT_NAME_INVALID = (int32_t)0xC0000033,
	STATUS_OBJECT_NAME_NOT_FOUND = (int32_t)0xC0000034,
	STATUS_OBJECT_NAME_COLLISION = (int32_t)0xC0000035,
	STATUS_FILE_IS_A_DIRECTORY = (int32_t)0xC00000BA,
	// NT's I/O timeout, an error: the SDK's waits return it, as STATUS_TIMEOUT would pass NT_SUCCESS.
	STATUS_IO_TIMEOUT = (int32_t)0xC00000B5,
	STATUS_NOT_SUPPORTED = (int32_t)0xC00000BB,
};

// File statuses Xenia doesn't list; the values are NT's.
enum {
	STATUS_OBJECT_PATH_NOT_FOUND = (int32_t)0xC000003A,
	STATUS_SHARING_VIOLATION = (int32_t)0xC0000043,
	STATUS_DIRECTORY_NOT_EMPTY = (int32_t)0xC0000101,
	STATUS_NOT_A_DIRECTORY = (int32_t)0xC0000103,
};

// Win32 errors that XAM returns (xenia:src/xenia/xbox.h:86-100).
enum {
	ERROR_SUCCESS = 0,
	ERROR_DEVICE_NOT_CONNECTED = 0x48F,
};

// xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:357
static const uint32_t TLS_OUT_OF_INDEXES = 0xFFFFFFFF;

// Root directory for paths like "game:\file" (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:129).
static const HANDLE OB_DOS_DEVICES = 0xFFFFFFFD;

// xenia:src/xenia/xbox.h:126-152
enum {
	MEM_COMMIT = 0x00001000,
	MEM_RESERVE = 0x00002000,
	// In MmAllocatePhysicalMemoryEx's protection, asks for 64 KB pages (xboxkrnl/xboxkrnl_memory.cc:356-361).
	MEM_LARGE_PAGES = 0x20000000,
	PAGE_READWRITE = 0x00000004,
	PAGE_NOCACHE = 0x00000200,
	PAGE_WRITECOMBINE = 0x00000400,
};

// xenia:src/xenia/base/filesystem.h:64-73, src/xenia/vfs/entry.h:47-73, src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:30-40
enum {
	GENERIC_READ = (int32_t)0x80000000,
	GENERIC_WRITE = 0x40000000,
	// Synchronous IO needs it (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:152); the value is NT's.
	SYNCHRONIZE = 0x00100000,
	// Needed to mark a file for deletion; the value is NT's.
	DELETE = 0x00010000,
	FILE_ATTRIBUTE_DIRECTORY = 0x00000010,
	FILE_ATTRIBUTE_NORMAL = 0x00000080,
	FILE_OPEN = 1,
	FILE_CREATE = 2,
	FILE_OPEN_IF = 3,
	FILE_OVERWRITE = 4,
	FILE_OVERWRITE_IF = 5,
	FILE_DIRECTORY_FILE = 0x00000001,
	FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020,
	FILE_NON_DIRECTORY_FILE = 0x00000040,
};

// File information classes (xenia:src/xenia/kernel/info/file.h:19-55).
enum {
	FILE_DISPOSITION_INFORMATION_CLASS = 13,
	FILE_END_OF_FILE_INFORMATION_CLASS = 20,
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
// Xenia returns it as 32 bits (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:312-315).
uint32_t KeQueryPerformanceFrequency(void);
// 100 ns units since 1601, as NT counts.
void KeQuerySystemTime(uint64_t *time);
// Returns TLS_OUT_OF_INDEXES when no slot is free.
uint32_t KeTlsAlloc(void);
uint32_t KeTlsFree(uint32_t slot);
void *KeTlsGetValue(uint32_t slot);
uint32_t KeTlsSetValue(uint32_t slot, void *value);
void *MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size_bytes, uint32_t protect, uint32_t min_address,
	uint32_t max_address, uint32_t alignment);
void MmFreePhysicalMemory(uint32_t type, void *base);
// The address the GPU uses for memory from MmAllocatePhysicalMemoryEx.
uint32_t MmGetPhysicalAddress(const void *address);
// size must be the struct's; Xenia refuses any other (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:483).
NTSTATUS MmQueryStatistics(MM_QUERY_STATISTICS_RESULT *statistics);
NTSTATUS NtAllocateVirtualMemory(
	void **base, uint32_t *size_bytes, uint32_t allocation_type, uint32_t protect, uint32_t debug_memory);
NTSTATUS NtClose(HANDLE handle);
NTSTATUS NtCreateEvent(HANDLE *event, const OBJECT_ATTRIBUTES *attributes, uint32_t event_type, uint32_t initial_state);
NTSTATUS NtCreateFile(HANDLE *handle, uint32_t desired_access, const OBJECT_ATTRIBUTES *attributes,
	IO_STATUS_BLOCK *io_status, const uint64_t *allocation_size, uint32_t file_attributes, uint32_t share_access,
	uint32_t creation_disposition, uint32_t create_options);
NTSTATUS NtCreateSemaphore(HANDLE *semaphore, const OBJECT_ATTRIBUTES *attributes, int32_t count, int32_t limit);
// file_name, a pattern, is read only when a scan starts (xenia:src/xenia/kernel/xfile.cc:50-71).
NTSTATUS NtQueryDirectoryFile(HANDLE file, HANDLE event, void *apc_routine, void *apc_context,
	IO_STATUS_BLOCK *io_status, FILE_DIRECTORY_INFORMATION *information, uint32_t length_bytes,
	const ANSI_STRING *file_name, uint32_t restart_scan);
NTSTATUS NtQueryFullAttributesFile(const OBJECT_ATTRIBUTES *attributes, FILE_NETWORK_OPEN_INFORMATION *information);
NTSTATUS NtQueryInformationFile(
	HANDLE file, IO_STATUS_BLOCK *io_status, void *information, uint32_t length_bytes, uint32_t information_class);
NTSTATUS NtReadFile(HANDLE file, HANDLE event, void *apc_routine, void *apc_context, IO_STATUS_BLOCK *io_status,
	void *buffer, uint32_t length_bytes, const uint64_t *byte_offset);
NTSTATUS NtReleaseSemaphore(HANDLE semaphore, int32_t release_count, int32_t *previous_count);
NTSTATUS NtSetEvent(HANDLE event, int32_t *previous_state);
NTSTATUS NtSetInformationFile(HANDLE file, IO_STATUS_BLOCK *io_status, const void *information, uint32_t length_bytes,
	uint32_t information_class);
NTSTATUS NtWaitForSingleObjectEx(HANDLE object, uint32_t wait_mode, uint32_t alertable, const int64_t *timeout);
NTSTATUS NtWriteFile(HANDLE file, HANDLE event, void *apc_routine, void *apc_context, IO_STATUS_BLOCK *io_status,
	const void *buffer, uint32_t length_bytes, const uint64_t *byte_offset);
NTSTATUS NtYieldExecution(void);
void RtlEnterCriticalSection(RTL_CRITICAL_SECTION *section);
void RtlInitializeCriticalSection(RTL_CRITICAL_SECTION *section);
void RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *section);
// Returns nonzero when it took the section.
uint32_t RtlTryEnterCriticalSection(RTL_CRITICAL_SECTION *section);
uint32_t XAudioRegisterRenderDriverClient(const XAUDIO_RENDER_DRIVER_CLIENT *client, uint32_t *driver);
uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, const float *samples);
uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver);
// The GPU writes its read pointer, in dwords, to this address (xenia:src/xenia/gpu/command_processor.cc:243-246).
void VdEnableRingBufferRPtrWriteBack(uint32_t physical_address, uint32_t block_size_log2);
void VdGetCurrentDisplayInformation(DISPLAY_INFO *info);
// Xenia fills 0x94 bytes of buffer and one dword of value (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:252-256).
void VdGetSystemCommandBuffer(void *buffer, uint32_t *value);
// Each points to a kernel variable holding a D3D device (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:442-457).
extern uint32_t *const VdGlobalDevice;
extern uint32_t *const VdGlobalXamDevice;
// Runs the graphics notification routines, through which XAM's D3D device gives up the GPU (docs/HARDWARE.md).
uint32_t VdInitializeEngines(
	uint32_t flags, void *callback, void *context, const uint32_t *pfp_microcode, const uint32_t *me_microcode);
// size_log2 is log2 of the ring's size in 8-byte units (xenia:src/xenia/gpu/command_processor.cc:312).
void VdInitializeRingBuffer(uint32_t physical_address, uint32_t size_log2);
void VdQueryVideoMode(VIDEO_MODE *mode);
// Bit 30 always set, bits 28-29 the colour space (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:166-183).
uint32_t VdSetDisplayMode(uint32_t flags);
// The kernel keeps one callback and calls it with source 0 at vblank (xenia:src/xenia/gpu/graphics_system.cc:234-272).
typedef void (*VdGraphicsInterruptCallback)(uint32_t source, void *context);
void VdSetGraphicsInterruptCallback(VdGraphicsInterruptCallback callback, void *context);
// D3D passes an address in its device (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:259-261).
void VdSetSystemCommandBufferGpuIdentifierAddress(volatile uint32_t *address);
void VdShutdownEngines(void);
// Writes only a fence, of system_value to system_writeback, into 64 dwords reserved in the ring (docs/HARDWARE.md).
void VdSwap(volatile uint32_t *ring_space, const uint32_t *fetch_constant, volatile uint32_t *system_writeback,
	void *system_buffer, uint32_t system_value, const uint32_t *front_buffer, const uint32_t *texture_format,
	const uint32_t *color_space, const uint32_t *width, const uint32_t *height);
void XeCryptRandom(void *buffer, uint32_t length_bytes);
// Returns 0 if a module like "xboxkrnl.exe" is loaded (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc:42-62).
uint32_t XexGetModuleHandle(const char *module_name, HANDLE *module);
NTSTATUS XexGetProcedureAddress(HANDLE module, uint32_t ordinal, void **address);

// xam.xex
uint32_t XamInputGetState(uint32_t user_index, uint32_t flags, INPUT_STATE *state);
uint32_t XamInputSetState(uint32_t user_index, uint32_t flags, const INPUT_VIBRATION *vibration);
void XamLoaderTerminateTitle(void);

#ifdef __cplusplus
}
#endif

#endif
