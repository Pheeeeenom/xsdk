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

NTSTATUS xsdk_log_open(void)
{
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
	va_list args;
	va_start(args, format);
	int length = vsnprintf(log_line, sizeof(log_line) - 1, format, args);
	va_end(args);
	if (length < 0)
		return;
	if ((size_t)length > sizeof(log_line) - 2)
		length = sizeof(log_line) - 2;
	log_line[length] = '\n';
	log_line[length + 1] = '\0';

	(void)DbgPrint("%s", log_line);
	if (!log_file)
		return;
	IO_STATUS_BLOCK io_status;
	(void)NtWriteFile(log_file, 0, NULL, NULL, &io_status, log_line, (uint32_t)length + 1, NULL);
}

void xsdk_log_close(void)
{
	if (!log_file)
		return;
	(void)NtClose(log_file);
	log_file = 0;
}
