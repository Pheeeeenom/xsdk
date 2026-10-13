#include "xsdk/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/kernel.h"

enum {
	LOG_LINE_BYTES = 512,
	// The log's name, the title's with .log, and the folder before it.
	LOG_NAME_BYTES = 64,
	LOG_PATH_BYTES = 128,
};

typedef struct {
	HANDLE root_directory;
	const char *folder;
} LogFolder;

// game: is where the title was launched from; cache: is the folder Xenia lets titles write to.
static const LogFolder log_folders[] = {
	{ OB_DOS_DEVICES, "game:\\" },
	{ 0, "\\Device\\Harddisk0\\Partition1\\" },
	{ 0, "\\Device\\Mass0\\" },
	{ OB_DOS_DEVICES, "cache:\\" },
};

// Guards log_line and the file; threads log concurrently.
static RTL_CRITICAL_SECTION log_lock;
static HANDLE log_file;
static char log_line[LOG_LINE_BYTES];
static char log_path[LOG_PATH_BYTES];

// The running .xex's name, from the path the loader leaves in ExLoadedImageName, with .log for its extension; xsdk.log
// if there's no name or it doesn't fit.
static void log_name(char *name, size_t name_bytes)
{
	const char *const image = ExLoadedImageName ? ExLoadedImageName : "";
	const char *base = image;
	for (const char *c = image; *c; c++) {
		if (*c == '\\' || *c == '/' || *c == ':')
			base = c + 1;
	}
	const char *const dot = strrchr(base, '.');
	const size_t stem_bytes = dot ? (size_t)(dot - base) : strlen(base);
	if (stem_bytes == 0 || stem_bytes + sizeof ".log" > name_bytes) {
		(void)snprintf(name, name_bytes, "xsdk.log");
		return;
	}
	(void)snprintf(name, name_bytes, "%.*s.log", (int)stem_bytes, base);
}

static NTSTATUS create_file(HANDLE *handle, HANDLE root_directory, const char *path)
{
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(path),
		.maximum_length = (uint16_t)(strlen(path) + 1),
		.pointer = path,
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = root_directory, .name = &name };
	IO_STATUS_BLOCK io_status;
	// Shared for reading, so the log can be read while the title runs.
	return NtCreateFile(handle, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
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
	char name[LOG_NAME_BYTES];
	log_name(name, sizeof name);
	NTSTATUS status = STATUS_NOT_IMPLEMENTED;
	for (size_t i = 0; i < sizeof(log_folders) / sizeof(log_folders[0]); i++) {
		char path[LOG_PATH_BYTES];
		(void)snprintf(path, sizeof path, "%s%s", log_folders[i].folder, name);
		HANDLE handle;
		status = create_file(&handle, log_folders[i].root_directory, path);
		if (NT_SUCCESS(status)) {
			log_file = handle;
			strcpy(log_path, path);
			xsdk_log_write("log: %s", path);
			xsdk_log_write("log: named after %s", ExLoadedImageName ? ExLoadedImageName : "nothing");
			return status;
		}
		xsdk_log_write("log: NtCreateFile(%s) failed: 0x%08X", path, (unsigned)status);
	}
	return status;
}

const char *xsdk_log_path(void)
{
	return log_path[0] ? log_path : NULL;
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
