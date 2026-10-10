#include "path_private.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Path calls on the kernel's file calls. Kernel paths are "game:\dir\file", relative to the drive-letter links, or
// absolute, "\Device\Harddisk0\Partition1\dir"; POSIX paths may use '/' and are relative to the current directory.

static pthread_mutex_t current_directory_lock = PTHREAD_MUTEX_INITIALIZER;
// game: is the folder the title was launched from (docs/HARDWARE.md).
static char current_directory[XSDK_PATH_BYTES] = "game:\\";

static bool is_separator(char c)
{
	return c == '/' || c == '\\';
}

// The bytes up to and including the separator after the drive: "game:\" or "\".
static size_t root_bytes(const char *resolved)
{
	return (size_t)(strchr(resolved, '\\') - resolved) + 1;
}

// Starts resolved at the path's own root, or at base for a relative path, and returns the rest of the path.
static const char *start_path(const char *path, const char *base, char resolved[XSDK_PATH_BYTES])
{
	const char *first_end = path;
	while (*first_end && !is_separator(*first_end))
		first_end++;
	const char *colon = memchr(path, ':', (size_t)(first_end - path));
	if (colon) {
		const size_t drive_bytes = (size_t)(colon - path) + 1;
		if (drive_bytes + 2 > XSDK_PATH_BYTES)
			return NULL;
		memcpy(resolved, path, drive_bytes);
		strcpy(resolved + drive_bytes, "\\");
		return colon + 1;
	}
	if (is_separator(path[0])) {
		strcpy(resolved, "\\");
		return path;
	}
	strcpy(resolved, base);
	return path;
}

// Appends the path's names to resolved, dropping "." and stepping back for "..".
static int append_names(char resolved[XSDK_PATH_BYTES], const char *path)
{
	const size_t root = root_bytes(resolved);
	size_t length = strlen(resolved);
	while (*path) {
		const char *end = path;
		while (*end && !is_separator(*end))
			end++;
		const size_t name_bytes = (size_t)(end - path);
		if (name_bytes == 2 && path[0] == '.' && path[1] == '.') {
			while (length > root && resolved[length - 1] != '\\')
				length--;
			if (length > root)
				length--;
			resolved[length] = '\0';
		} else if (name_bytes > 0 && !(name_bytes == 1 && path[0] == '.')) {
			const size_t separator_bytes = length > root ? 1 : 0;
			if (length + separator_bytes + name_bytes >= XSDK_PATH_BYTES)
				return ENAMETOOLONG;
			if (separator_bytes)
				resolved[length++] = '\\';
			memcpy(resolved + length, path, name_bytes);
			length += name_bytes;
			resolved[length] = '\0';
		}
		path = *end ? end + 1 : end;
	}
	return 0;
}

int xsdk_path_resolve(int dir_fd, const char *path, char resolved[XSDK_PATH_BYTES])
{
	if (!path[0])
		return ENOENT;
	char base[XSDK_PATH_BYTES];
	if (dir_fd == AT_FDCWD) {
		(void)pthread_mutex_lock(&current_directory_lock);
		memcpy(base, current_directory, sizeof(base));
		(void)pthread_mutex_unlock(&current_directory_lock);
	} else {
		const int error = xsdk_file_directory_path(dir_fd, base);
		if (error)
			return error;
	}
	const char *rest = start_path(path, base, resolved);
	if (!rest)
		return ENAMETOOLONG;
	return append_names(resolved, rest);
}

void xsdk_path_attributes(const char *resolved, ANSI_STRING *name, OBJECT_ATTRIBUTES *attributes)
{
	const size_t length = strlen(resolved);
	*name = (ANSI_STRING){ .length = (uint16_t)length, .maximum_length = (uint16_t)(length + 1), .pointer = resolved };
	// Drive-letter paths look the drive up among the DOS devices (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:129).
	*attributes = (OBJECT_ATTRIBUTES){ .root_directory = resolved[0] == '\\' ? 0 : OB_DOS_DEVICES, .name = name };
}

static int fail_with_status(const char *call, const char *resolved, NTSTATUS status)
{
	xsdk_log_write("%s(%s) failed: 0x%08X", call, resolved, (unsigned)status);
	errno = xsdk_file_errno(status);
	return -1;
}

static int resolve_or_fail(int dir_fd, const char *path, char resolved[XSDK_PATH_BYTES])
{
	const int error = xsdk_path_resolve(dir_fd, path, resolved);
	if (error) {
		errno = error;
		return -1;
	}
	return 0;
}

static int stat_resolved(const char *resolved, struct stat *info)
{
	ANSI_STRING name;
	OBJECT_ATTRIBUTES attributes;
	xsdk_path_attributes(resolved, &name, &attributes);
	FILE_NETWORK_OPEN_INFORMATION information;
	const NTSTATUS status = NtQueryFullAttributesFile(&attributes, &information);
	if (!NT_SUCCESS(status)) {
		errno = xsdk_file_errno(status);
		// A missing file is the usual answer to a stat, not an error.
		if (errno != ENOENT)
			xsdk_log_write("stat: NtQueryFullAttributesFile(%s) failed: 0x%08X", resolved, (unsigned)status);
		return -1;
	}
	xsdk_file_stat(&information, info);
	return 0;
}

static NTSTATUS open_resolved(
	const char *resolved, uint32_t access, uint32_t disposition, uint32_t options, HANDLE *handle)
{
	ANSI_STRING name;
	OBJECT_ATTRIBUTES attributes;
	xsdk_path_attributes(resolved, &name, &attributes);
	IO_STATUS_BLOCK io_status;
	return NtCreateFile(handle, access | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL, 0,
		disposition, options | FILE_SYNCHRONOUS_IO_NONALERT);
}

int stat(const char *path, struct stat *info)
{
	char resolved[XSDK_PATH_BYTES];
	if (resolve_or_fail(AT_FDCWD, path, resolved) != 0)
		return -1;
	return stat_resolved(resolved, info);
}

int chdir(const char *path)
{
	char resolved[XSDK_PATH_BYTES];
	if (resolve_or_fail(AT_FDCWD, path, resolved) != 0)
		return -1;
	struct stat info;
	if (stat_resolved(resolved, &info) != 0)
		return -1;
	if (!S_ISDIR(info.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}
	(void)pthread_mutex_lock(&current_directory_lock);
	memcpy(current_directory, resolved, sizeof(current_directory));
	(void)pthread_mutex_unlock(&current_directory_lock);
	return 0;
}

// With no buffer, allocates one, as glibc does; libstdc++ asks for that first.
char *getcwd(char *buffer, size_t size_bytes)
{
	char path[XSDK_PATH_BYTES];
	(void)pthread_mutex_lock(&current_directory_lock);
	memcpy(path, current_directory, sizeof(path));
	(void)pthread_mutex_unlock(&current_directory_lock);

	const size_t length = strlen(path);
	if (!buffer && size_bytes == 0)
		size_bytes = length + 1;
	if (size_bytes <= length) {
		errno = size_bytes == 0 ? EINVAL : ERANGE;
		return NULL;
	}
	char *const result = buffer ? buffer : malloc(size_bytes);
	if (!result) {
		errno = ENOMEM;
		return NULL;
	}
	for (size_t i = 0; i <= length; i++)
		result[i] = path[i] == '\\' ? '/' : path[i];
	return result;
}

// Permissions can't be stored (xsdk_file_stat), so mode is unused.
int mkdir(const char *path, mode_t mode)
{
	(void)mode;
	char resolved[XSDK_PATH_BYTES];
	if (resolve_or_fail(AT_FDCWD, path, resolved) != 0)
		return -1;
	HANDLE handle;
	const NTSTATUS status = open_resolved(resolved, GENERIC_READ, FILE_CREATE, FILE_DIRECTORY_FILE, &handle);
	if (!NT_SUCCESS(status))
		return fail_with_status("mkdir: NtCreateFile", resolved, status);
	const NTSTATUS close_status = NtClose(handle);
	if (!NT_SUCCESS(close_status))
		return fail_with_status("mkdir: NtClose", resolved, close_status);
	return 0;
}

// newlib's remove() calls unlink() for directories too, so without AT_REMOVEDIR either kind goes.
int unlinkat(int dir_fd, const char *path, int flags)
{
	char resolved[XSDK_PATH_BYTES];
	if (resolve_or_fail(dir_fd, path, resolved) != 0)
		return -1;
	const uint32_t options = flags & AT_REMOVEDIR ? FILE_DIRECTORY_FILE : 0;
	HANDLE handle;
	NTSTATUS status = open_resolved(resolved, DELETE, FILE_OPEN, options, &handle);
	if (!NT_SUCCESS(status))
		return fail_with_status("unlink: NtCreateFile", resolved, status);
	// The kernel deletes the file when its last handle closes; Xenia ignores this
	// (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc:203-210).
	const FILE_DISPOSITION_INFORMATION disposition = { .delete_file = 1 };
	IO_STATUS_BLOCK io_status;
	status =
		NtSetInformationFile(handle, &io_status, &disposition, sizeof(disposition), FILE_DISPOSITION_INFORMATION_CLASS);
	const NTSTATUS close_status = NtClose(handle);
	if (!NT_SUCCESS(status))
		return fail_with_status("unlink: NtSetInformationFile", resolved, status);
	if (!NT_SUCCESS(close_status))
		return fail_with_status("unlink: NtClose", resolved, close_status);
	return 0;
}

int unlink(const char *path)
{
	return unlinkat(AT_FDCWD, path, 0);
}

int truncate(const char *path, off_t length_bytes)
{
	if (length_bytes < 0) {
		errno = EINVAL;
		return -1;
	}
	char resolved[XSDK_PATH_BYTES];
	if (resolve_or_fail(AT_FDCWD, path, resolved) != 0)
		return -1;
	HANDLE handle;
	NTSTATUS status = open_resolved(resolved, GENERIC_WRITE, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &handle);
	if (!NT_SUCCESS(status))
		return fail_with_status("truncate: NtCreateFile", resolved, status);
	const FILE_END_OF_FILE_INFORMATION end = { .end_of_file = (uint64_t)length_bytes };
	IO_STATUS_BLOCK io_status;
	status = NtSetInformationFile(handle, &io_status, &end, sizeof(end), FILE_END_OF_FILE_INFORMATION_CLASS);
	const NTSTATUS close_status = NtClose(handle);
	if (!NT_SUCCESS(status))
		return fail_with_status("truncate: NtSetInformationFile", resolved, status);
	if (!NT_SUCCESS(close_status))
		return fail_with_status("truncate: NtClose", resolved, close_status);
	return 0;
}

// Permissions can't change (xsdk_file_stat), so only asking for the ones a file already has succeeds.
int fchmodat(int dir_fd, const char *path, mode_t mode, int flags)
{
	(void)flags;
	char resolved[XSDK_PATH_BYTES];
	if (resolve_or_fail(dir_fd, path, resolved) != 0)
		return -1;
	struct stat info;
	if (stat_resolved(resolved, &info) != 0)
		return -1;
	if ((mode & ALLPERMS) != (info.st_mode & ALLPERMS)) {
		errno = ENOTSUP;
		return -1;
	}
	return 0;
}

// The kernel's file calls have no links of either kind.
int link(const char *existing, const char *path)
{
	(void)existing;
	(void)path;
	errno = ENOSYS;
	return -1;
}

int symlink(const char *target, const char *path)
{
	(void)target;
	(void)path;
	errno = ENOSYS;
	return -1;
}

// No file is a symbolic link.
ssize_t readlink(const char *path, char *buffer, size_t size_bytes)
{
	(void)buffer;
	(void)size_bytes;
	struct stat info;
	if (stat(path, &info) != 0)
		return -1;
	errno = EINVAL;
	return -1;
}

long pathconf(const char *path, int name)
{
	(void)path;
	if (name == _PC_PATH_MAX)
		return XSDK_PATH_BYTES;
	errno = EINVAL;
	return -1;
}
