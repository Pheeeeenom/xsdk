#include <cstdio>
#include <expected>
#include <format>
#include <print>
#include <string>
#include <string_view>

#include "tests.h"

namespace
{

enum class ParseError { empty, not_a_number };

std::expected<int, ParseError> parse_number(std::string_view text)
{
	if (text.empty())
		return std::unexpected(ParseError::empty);
	int value = 0;
	for (const char digit : text) {
		if (digit < '0' || digit > '9')
			return std::unexpected(ParseError::not_a_number);
		value = value * 10 + (digit - '0');
	}
	return value;
}

} // namespace

extern "C" void test_cpp_format(void)
{
	const std::string text = std::format("{} {:#x} {:.2f} {:>4}", 42, 255, 3.14159, "ok");
	check("std::format", text == "42 0xff 3.14   ok");

	const auto good = parse_number("1234");
	const auto bad = parse_number("12a");
	check("std::expected", good && *good == 1234 && !bad && bad.error() == ParseError::not_a_number);

	std::println("println: {} reaches the log through stdout", "std::println");
	check("std::println", std::fflush(stdout) == 0 && !std::ferror(stdout));
}
