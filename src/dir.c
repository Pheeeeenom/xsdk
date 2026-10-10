#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "file_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Directory streams on NtQueryDirectoryFile, over a descriptor opened with O_DIRECTORY.

// Marks a stream whose last query has no entries left.
static const uint32_t ENTRIES_USED = UINT32_MAX;

struct __xsdk_dir {
	int fd;
	bool restart;
	// Where the next entry starts in buffer.
	uint32_t next_offset;
	struct dirent entry;
	// Room for an entry with a full-length name; uint64_t keeps the entry's 64-bit fields aligned.
	uint64_t buffer[(sizeof(FILE_DIRECTORY_INFORMATION) + sizeof(((struct dirent *)0)->d_name) + 7) / 8];
};

static bool query_entries(DIR *dir)
{
	const HANDLE handle = xsdk_file_handle(dir->fd);
	if (!handle) {
		errno = EBADF;
		return false;
	}
	// The pattern is read only when a scan starts.
	static const ANSI_STRING ALL_FILES = { .length = 1, .maximum_length = 2, .pointer = "*" };
	IO_STATUS_BLOCK io_status;
	const NTSTATUS status = NtQueryDirectoryFile(handle, 0, NULL, NULL, &io_status,
		(FILE_DIRECTORY_INFORMATION *)dir->buffer, sizeof(dir->buffer), dir->restart ? &ALL_FILES : NULL, dir->restart);
	dir->restart = false;
	// An empty directory answers the first query with STATUS_NO_SUCH_FILE (xenia:src/xenia/kernel/xfile.cc:58-61).
	if (status == STATUS_NO_MORE_FILES || status == STATUS_NO_SUCH_FILE)
		return false;
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("readdir: NtQueryDirectoryFile(fd %d) failed: 0x%08X", dir->fd, (unsigned)status);
		errno = xsdk_file_errno(status);
		return false;
	}
	dir->next_offset = 0;
	return true;
}

DIR *fdopendir(int fd)
{
	char path[XSDK_PATH_BYTES];
	const int error = xsdk_file_directory_path(fd, path);
	if (error) {
		errno = error;
		return NULL;
	}
	DIR *dir = malloc(sizeof(*dir));
	if (!dir) {
		errno = ENOMEM;
		return NULL;
	}
	dir->fd = fd;
	dir->restart = true;
	dir->next_offset = ENTRIES_USED;
	return dir;
}

DIR *opendir(const char *path)
{
	const int fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		return NULL;
	DIR *dir = fdopendir(fd);
	if (!dir) {
		const int error = errno;
		(void)close(fd);
		errno = error;
	}
	return dir;
}

// Returns NULL with errno untouched at the end of the directory, as POSIX asks.
struct dirent *readdir(DIR *dir)
{
	if (dir->next_offset == ENTRIES_USED && !query_entries(dir))
		return NULL;
	const FILE_DIRECTORY_INFORMATION *information =
		(const FILE_DIRECTORY_INFORMATION *)((const uint8_t *)dir->buffer + dir->next_offset);
	dir->next_offset =
		information->next_entry_offset ? dir->next_offset + information->next_entry_offset : ENTRIES_USED;

	const size_t name_bytes = information->file_name_length;
	if (name_bytes >= sizeof(dir->entry.d_name)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	memset(&dir->entry, 0, sizeof(dir->entry));
	dir->entry.d_type = information->attributes & FILE_ATTRIBUTE_DIRECTORY ? DT_DIR : DT_REG;
	memcpy(dir->entry.d_name, information->file_name, name_bytes);
	return &dir->entry;
}

void rewinddir(DIR *dir)
{
	dir->restart = true;
	dir->next_offset = ENTRIES_USED;
}

int dirfd(DIR *dir)
{
	return dir->fd;
}

int closedir(DIR *dir)
{
	const int result = close(dir->fd);
	free(dir);
	return result;
}
