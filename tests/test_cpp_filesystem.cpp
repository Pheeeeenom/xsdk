#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "tests.h"
#include "xsdk/log.h"

namespace fs = std::filesystem;

namespace
{

// game: is the launch folder on the console; Xenia mounts it read-only but lets titles write to cache:.
const char *const ROOTS[] = { "game:/xsdk_fs_test", "cache:/xsdk_fs_test" };

fs::path create_test_directory()
{
	for (const char *root : ROOTS) {
		std::error_code error;
		fs::remove_all(root, error);
		if (fs::create_directory(root, error))
			return root;
		xsdk_log_write("filesystem: create_directory(%s): %s", root, error.message().c_str());
	}
	return {};
}

} // namespace

extern "C" void test_cpp_filesystem(void)
{
	const fs::path directory = create_test_directory();
	check("filesystem creates a directory", !directory.empty());
	if (directory.empty())
		return;
	std::ofstream(directory / "file.txt") << "hello";

	std::error_code error;
	int entries = 0;
	bool found_file = false;
	for (const auto &entry : fs::directory_iterator(directory, error)) {
		entries++;
		found_file = found_file || (entry.path().filename() == "file.txt" && entry.is_regular_file());
	}
	if (error)
		xsdk_log_write("filesystem: directory_iterator: %s", error.message().c_str());
	check("filesystem lists a directory", !error && entries == 1 && found_file);

	const std::uintmax_t removed = fs::remove_all(directory, error);
	if (error)
		xsdk_log_write("filesystem: remove_all: %s", error.message().c_str());
	check("filesystem removes a directory", !error && removed == 2 && !fs::exists(directory));
}
