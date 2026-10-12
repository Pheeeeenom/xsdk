#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
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

	// libstdc++ takes "game:/..." for a relative path, so these put the current directory, game:\, in front of it.
	std::error_code absolute_error;
	std::error_code canonical_error;
	const fs::path absolute = fs::absolute(directory / "file.txt", absolute_error);
	const fs::path canonical = fs::weakly_canonical(directory / "file.txt", canonical_error);
	xsdk_log_write("filesystem: absolute() gives %s, weakly_canonical() %s", absolute.c_str(), canonical.c_str());
	const auto reads_hello = [](const fs::path &path) {
		std::string word;
		std::ifstream(path) >> word;
		return word == "hello";
	};
	check("filesystem reaches a file through absolute() and weakly_canonical()",
		!absolute_error && !canonical_error && reads_hello(absolute) && reads_hello(canonical));

	const std::uintmax_t removed = fs::remove_all(directory, error);
	if (error)
		xsdk_log_write("filesystem: remove_all: %s", error.message().c_str());
	check("filesystem removes a directory", !error && removed == 2 && !fs::exists(directory));
}
