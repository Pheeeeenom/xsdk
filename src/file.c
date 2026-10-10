#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// newlib's file system calls on Nt*File; descriptors 0-2 are the console, and stdout and stderr go to the log.

enum {
	FILE_SLOTS = 32,
	FIRST_FILE_FD = 3,
	// NT's MAX_PATH.
	PATH_BYTES = 260,
};

// A slot whose handle is being opened.
static const HANDLE SLOT_CLAIMED = 0xFFFFFFFF;

typedef struct {
	HANDLE handle;
	uint64_t position;
	bool append;
} FileSlot;

static FileSlot files[FILE_SLOTS];

static int errno_from_status(NTSTATUS status)
{
	switch (status) {
	case STATUS_OBJECT_NAME_NOT_FOUND:
	case STATUS_NO_SUCH_FILE:
		return ENOENT;
	case STATUS_OBJECT_NAME_COLLISION:
		return EEXIST;
	case STATUS_ACCESS_DENIED:
		return EACCES;
	case STATUS_NO_MEMORY:
		return ENOMEM;
	case STATUS_INVALID_HANDLE:
		return EBADF;
	case STATUS_INVALID_PARAMETER:
		return EINVAL;
	default:
		return EIO;
	}
}

static int fail_with_status(const char *call, int fd, NTSTATUS status)
{
	xsdk_log_write("%s(fd %d) failed: 0x%08X", call, fd, (unsigned)status);
	errno = errno_from_status(status);
	return -1;
}

static FileSlot *claim_slot(int *fd)
{
	for (int i = 0; i < FILE_SLOTS; i++) {
		HANDLE expected = 0;
		if (__atomic_compare_exchange_n(
				&files[i].handle, &expected, SLOT_CLAIMED, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			*fd = FIRST_FILE_FD + i;
			return &files[i];
		}
	}
	return NULL;
}

static void release_slot(FileSlot *file)
{
	__atomic_store_n(&file->handle, 0, __ATOMIC_RELEASE);
}

static FileSlot *find_slot(int fd)
{
	if (fd < FIRST_FILE_FD || fd >= FIRST_FILE_FD + FILE_SLOTS)
		return NULL;
	FileSlot *file = &files[fd - FIRST_FILE_FD];
	const HANDLE handle = __atomic_load_n(&file->handle, __ATOMIC_ACQUIRE);
	return handle && handle != SLOT_CLAIMED ? file : NULL;
}

static bool is_console(int fd)
{
	return fd >= 0 && fd < FIRST_FILE_FD;
}

static uint32_t access_from_flags(int flags)
{
	switch (flags & O_ACCMODE) {
	case O_WRONLY:
		return GENERIC_WRITE;
	case O_RDWR:
		return GENERIC_READ | GENERIC_WRITE;
	default:
		return GENERIC_READ;
	}
}

static uint32_t disposition_from_flags(int flags)
{
	if (flags & O_CREAT) {
		if (flags & O_EXCL)
			return FILE_CREATE;
		return flags & O_TRUNC ? FILE_OVERWRITE_IF : FILE_OPEN_IF;
	}
	return flags & O_TRUNC ? FILE_OVERWRITE : FILE_OPEN;
}

static NTSTATUS query_size(HANDLE handle, uint64_t *size_bytes)
{
	FILE_NETWORK_OPEN_INFORMATION info;
	IO_STATUS_BLOCK io_status;
	const NTSTATUS status =
		NtQueryInformationFile(handle, &io_status, &info, sizeof(info), FILE_NETWORK_OPEN_INFORMATION_CLASS);
	if (NT_SUCCESS(status))
		*size_bytes = info.end_of_file;
	return status;
}

int open(const char *path, int flags, ...)
{
	char nt_path[PATH_BYTES];
	const size_t length = strlen(path);
	if (length >= sizeof(nt_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	for (size_t i = 0; i <= length; i++)
		nt_path[i] = path[i] == '/' ? '\\' : path[i];

	int fd;
	FileSlot *file = claim_slot(&fd);
	if (!file) {
		errno = EMFILE;
		return -1;
	}

	const ANSI_STRING name = {
		.length = (uint16_t)length, .maximum_length = (uint16_t)(length + 1), .pointer = nt_path
	};
	// "game:\file" names a drive-letter link; "\Device\..." is absolute.
	const OBJECT_ATTRIBUTES attributes = { .root_directory = nt_path[0] == '\\' ? 0 : OB_DOS_DEVICES, .name = &name };
	IO_STATUS_BLOCK io_status;
	HANDLE handle;
	const NTSTATUS status = NtCreateFile(&handle, access_from_flags(flags) | SYNCHRONIZE, &attributes, &io_status, NULL,
		FILE_ATTRIBUTE_NORMAL, 0, disposition_from_flags(flags),
		FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
	if (!NT_SUCCESS(status)) {
		release_slot(file);
		xsdk_log_write("open: NtCreateFile(%s) failed: 0x%08X", path, (unsigned)status);
		errno = errno_from_status(status);
		return -1;
	}
	file->position = 0;
	file->append = (flags & O_APPEND) != 0;
	__atomic_store_n(&file->handle, handle, __ATOMIC_RELEASE);
	return fd;
}

int close(int fd)
{
	if (is_console(fd))
		return 0;
	FileSlot *file = find_slot(fd);
	if (!file) {
		errno = EBADF;
		return -1;
	}
	const NTSTATUS status = NtClose(file->handle);
	release_slot(file);
	return NT_SUCCESS(status) ? 0 : fail_with_status("close", fd, status);
}

ssize_t read(int fd, void *buffer, size_t length_bytes)
{
	FileSlot *file = find_slot(fd);
	if (!file) {
		errno = EBADF;
		return -1;
	}
	IO_STATUS_BLOCK io_status;
	const NTSTATUS status =
		NtReadFile(file->handle, 0, NULL, NULL, &io_status, buffer, (uint32_t)length_bytes, &file->position);
	if (status == STATUS_END_OF_FILE)
		return 0;
	if (!NT_SUCCESS(status))
		return fail_with_status("read", fd, status);
	file->position += io_status.information;
	return (ssize_t)io_status.information;
}

ssize_t write(int fd, const void *buffer, size_t length_bytes)
{
	if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
		xsdk_log_write_bytes(buffer, length_bytes);
		return (ssize_t)length_bytes;
	}
	FileSlot *file = find_slot(fd);
	if (!file) {
		errno = EBADF;
		return -1;
	}
	if (file->append) {
		const NTSTATUS status = query_size(file->handle, &file->position);
		if (!NT_SUCCESS(status))
			return fail_with_status("write", fd, status);
	}
	IO_STATUS_BLOCK io_status;
	const NTSTATUS status =
		NtWriteFile(file->handle, 0, NULL, NULL, &io_status, buffer, (uint32_t)length_bytes, &file->position);
	if (!NT_SUCCESS(status))
		return fail_with_status("write", fd, status);
	file->position += io_status.information;
	return (ssize_t)io_status.information;
}

off_t lseek(int fd, off_t offset, int whence)
{
	FileSlot *file = find_slot(fd);
	if (!file) {
		errno = is_console(fd) ? ESPIPE : EBADF;
		return -1;
	}
	uint64_t base = 0;
	if (whence == SEEK_CUR) {
		base = file->position;
	} else if (whence == SEEK_END) {
		const NTSTATUS status = query_size(file->handle, &base);
		if (!NT_SUCCESS(status))
			return fail_with_status("lseek", fd, status);
	} else if (whence != SEEK_SET) {
		errno = EINVAL;
		return -1;
	}
	const int64_t position = (int64_t)base + offset;
	if (position < 0) {
		errno = EINVAL;
		return -1;
	}
	file->position = (uint64_t)position;
	return (off_t)position;
}

int fstat(int fd, struct stat *info)
{
	memset(info, 0, sizeof(*info));
	if (is_console(fd)) {
		info->st_mode = S_IFCHR;
		return 0;
	}
	FileSlot *file = find_slot(fd);
	if (!file) {
		errno = EBADF;
		return -1;
	}
	uint64_t size_bytes;
	const NTSTATUS status = query_size(file->handle, &size_bytes);
	if (!NT_SUCCESS(status))
		return fail_with_status("fstat", fd, status);
	info->st_mode = S_IFREG;
	info->st_size = (off_t)size_bytes;
	return 0;
}

int isatty(int fd)
{
	if (is_console(fd))
		return 1;
	errno = find_slot(fd) ? ENOTTY : EBADF;
	return 0;
}
