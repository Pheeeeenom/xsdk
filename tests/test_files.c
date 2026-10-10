#include <stdio.h>
#include <string.h>

#include "tests.h"
#include "xsdk/log.h"

enum { PATH_BYTES = 64 };

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

void test_files(void)
{
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
}
