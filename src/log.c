#include "xsdk/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/kernel.h"

enum { LOG_LINE_BYTES = 512 };

typedef struct {
	HANDLE root_directory;
	const char *path;
} LogPath;

// game: is where the title was launched from; cache: is the folder Xenia lets titles write to.
static const LogPath log_paths[] = {
	{ OB_DOS_DEVICES, "game:\\xsdk.log" },
	{ 0, "\\Device\\Harddisk0\\Partition1\\xsdk.log" },
	{ 0, "\\Device\\Mass0\\xsdk.log" },
	{ OB_DOS_DEVICES, "cache:\\xsdk.log" },
};

// Guards log_line and the file; threads log concurrently.
static RTL_CRITICAL_SECTION log_lock;
static HANDLE log_file;
static char log_line[LOG_LINE_BYTES];

static NTSTATUS create_file(HANDLE *handle, const LogPath *log_path)
{
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(log_path->path),
		.maximum_length = (uint16_t)(strlen(log_path->path) + 1),
		.pointer = log_path->path,
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = log_path->root_directory, .name = &name };
	IO_STATUS_BLOCK io_status;
	return NtCreateFile(handle, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL, 0,
		FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
}

static void write_line(const char *line, size_t length_bytes)
{
	(void)DbgPrint("%s", line);
	if (!log_file)
		return;
	IO_STATUS_BLOCK io_status;
	(void)NtWriteFile(log_file, 0, NULL, NULL, &io_status, line, (uint32_t)length_bytes, NULL);
}

NTSTATUS xsdk_log_open(void)
{
	RtlInitializeCriticalSection(&log_lock);
	NTSTATUS status = STATUS_NOT_IMPLEMENTED;
	for (size_t i = 0; i < sizeof(log_paths) / sizeof(log_paths[0]); i++) {
		HANDLE handle;
		status = create_file(&handle, &log_paths[i]);
		if (NT_SUCCESS(status)) {
			log_file = handle;
			xsdk_log_write("log: %s", log_paths[i].path);
			return status;
		}
		xsdk_log_write("log: NtCreateFile(%s) failed: 0x%08X", log_paths[i].path, (unsigned)status);
	}
	return status;
}

void xsdk_log_write(const char *format, ...)
{
	RtlEnterCriticalSection(&log_lock);
	va_list args;
	va_start(args, format);
	int length = vsnprintf(log_line, sizeof(log_line) - 1, format, args);
	va_end(args);
	if (length >= 0) {
		if ((size_t)length > sizeof(log_line) - 2)
			length = sizeof(log_line) - 2;
		log_line[length] = '\n';
		log_line[length + 1] = '\0';
		write_line(log_line, (size_t)length + 1);
	}
	RtlLeaveCriticalSection(&log_lock);
}

void xsdk_log_write_bytes(const char *bytes, size_t length_bytes)
{
	RtlEnterCriticalSection(&log_lock);
	while (length_bytes) {
		size_t chunk = length_bytes < sizeof(log_line) - 1 ? length_bytes : sizeof(log_line) - 1;
		memcpy(log_line, bytes, chunk);
		log_line[chunk] = '\0';
		write_line(log_line, chunk);
		bytes += chunk;
		length_bytes -= chunk;
	}
	RtlLeaveCriticalSection(&log_lock);
}

void xsdk_log_close(void)
{
	RtlEnterCriticalSection(&log_lock);
	if (log_file)
		(void)NtClose(log_file);
	log_file = 0;
	RtlLeaveCriticalSection(&log_lock);
}
