#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tests.h"
#include "xsdk/log.h"

enum {
	PATH_BYTES = 64,
	HOLD_MS = 50,
};

// game: is the launch folder on the console; Xenia mounts it read-only but lets titles write to cache:.
static const char *const directories[] = { "game:\\", "cache:\\" };
static const char FILE_NAME[] = "xsdk_file_test.txt";

// No printf here: Xenia mangles formatted text, and this test is about file IO.
static FILE *create_test_file(char *path)
{
	for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
		strcpy(path, directories[i]);
		strcat(path, FILE_NAME);
		FILE *file = fopen(path, "w");
		if (file)
			return file;
	}
	return NULL;
}

static long file_size(const char *path)
{
	FILE *file = fopen(path, "r");
	if (!file)
		return -1;
	long size = -1;
	if (fseek(file, 0, SEEK_END) == 0)
		size = ftell(file);
	fclose(file);
	return size;
}

// The test file holds "line one\nline 2\nline three\n": "line 2" starts at byte 9, and the file is 27 bytes.
static void check_pread(const char *path)
{
	char bytes[8] = "";
	const int fd = open(path, O_RDONLY);
	const bool read_at_offset = fd >= 0 && pread(fd, bytes, 6, 9) == 6 && memcmp(bytes, "line 2", 6) == 0;
	const bool position_kept = fd >= 0 && lseek(fd, 0, SEEK_CUR) == 0;
	const bool nothing_at_end = fd >= 0 && pread(fd, bytes, 1, 27) == 0;
	if (fd >= 0)
		close(fd);
	check("pread: reads at an offset and leaves the position", read_at_offset && position_kept);
	check("pread: reads nothing at the end", nothing_at_end);
}

static volatile bool thread_has_lock;

static void *lock_file(void *file)
{
	flockfile(file);
	thread_has_lock = true;
	funlockfile(file);
	return NULL;
}

static void hold(void)
{
	const struct timespec delay = { .tv_nsec = HOLD_MS * 1000000 };
	nanosleep(&delay, NULL);
}

// The standard streams are each thread's own, so the other thread is handed this one's stdout.
static void check_flockfile(void)
{
	FILE *file = stdout;
	flockfile(file);
	flockfile(file);
	pthread_t thread;
	if (pthread_create(&thread, NULL, lock_file, file) != 0) {
		funlockfile(file);
		funlockfile(file);
		check("flockfile: another thread waits while the lock is held, twice and then once", false);
		return;
	}
	hold();
	const bool waited_while_held_twice = !thread_has_lock;
	funlockfile(file);
	hold();
	const bool waited_while_held_once = !thread_has_lock;
	funlockfile(file);
	pthread_join(thread, NULL);
	check("flockfile: another thread waits while the lock is held, twice and then once",
		waited_while_held_twice && waited_while_held_once && thread_has_lock);
}

// The log, opened again for reading while the title has it open for writing, as a tool on the console reads it while a
// title runs. Only the console can fail this: Xenia ignores share access (xboxkrnl/xboxkrnl_io.cc:102-145).
static void check_log_readable(void)
{
	const char *const path = xsdk_log_path();
	if (!path) {
		check("log read while it's open", false);
		return;
	}
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(path),
		.maximum_length = (uint16_t)(strlen(path) + 1),
		.pointer = path,
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = path[0] == '\\' ? 0 : OB_DOS_DEVICES, .name = &name };
	IO_STATUS_BLOCK io_status;
	HANDLE handle;
	const NTSTATUS status =
		NtCreateFile(&handle, GENERIC_READ | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
			FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
	// The log's first line is "log: " and its path (src/log.c).
	char start[6] = "";
	if (NT_SUCCESS(status)) {
		const uint64_t offset = 0;
		(void)NtReadFile(handle, 0, NULL, NULL, &io_status, start, sizeof(start) - 1, &offset);
		(void)NtClose(handle);
	}
	xsdk_log_write(
		"files: the log opened for reading while it's open: 0x%08X, starting \"%s\"", (unsigned)status, start);
	check("log read while it's open", NT_SUCCESS(status) && strcmp(start, "log: ") == 0);
}

// POSIX lets a file be open more than once at a time, to write and to read.
static void check_open_twice(const char *path)
{
	const int writer = open(path, O_RDWR);
	const int reader = open(path, O_RDONLY);
	char bytes[8] = "";
	const bool both = writer >= 0 && reader >= 0 && pread(reader, bytes, 6, 9) == 6 && memcmp(bytes, "line 2", 6) == 0;
	if (reader >= 0)
		close(reader);
	if (writer >= 0)
		close(writer);
	check("open: a file opens twice, to write and to read", both);
}

// The test file, renamed over another, takes its place, and its own name is gone.
static void check_rename(const char *path)
{
	char other[PATH_BYTES];
	strcpy(other, path);
	strcpy(strrchr(other, '\\') + 1, "xsdk_rename_test.txt");
	FILE *file = fopen(other, "w");
	const bool made = file && fputs("old\n", file) >= 0 && fclose(file) == 0;
	const bool renamed = made && rename(path, other) == 0;
	check("rename: a file renamed over another takes its place",
		renamed && file_size(other) == 27 && file_size(path) < 0);
	(void)unlink(other);
}

void test_files(void)
{
	check_log_readable();

	char path[PATH_BYTES];
	FILE *file = create_test_file(path);
	if (!file) {
		check("file written", false);
		return;
	}
	xsdk_log_write("files: %s", path);
	const bool written = fputs("line one\nline 2\n", file) >= 0;
	check("file written", fclose(file) == 0 && written);

	char line_one[32] = "";
	char line_two[32] = "";
	file = fopen(path, "r");
	if (file) {
		fgets(line_one, sizeof(line_one), file);
		fgets(line_two, sizeof(line_two), file);
		fclose(file);
	}
	check("file read back", strcmp(line_one, "line one\n") == 0 && strcmp(line_two, "line 2\n") == 0);
	check("file size from seek", file_size(path) == 16);

	file = fopen(path, "a");
	const bool appended = file && fputs("line three\n", file) >= 0 && fclose(file) == 0;
	check("file append", appended && file_size(path) == 27);
	check_pread(path);
	check_open_twice(path);
	check_flockfile();
	check_rename(path);
}
