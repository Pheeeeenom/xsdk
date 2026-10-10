#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>

#include "tests.h"

namespace
{

struct Shape {
	virtual ~Shape() = default;
};

struct Named {
	virtual ~Named() = default;
	std::string name = "named";
};

// Two polymorphic bases send a cross-cast down libsupc++'s general dynamic_cast path.
struct Circle : Shape, Named {
};

std::string message_of(const std::exception_ptr &error)
{
	try {
		std::rethrow_exception(error);
	} catch (const std::exception &caught) {
		return caught.what();
	} catch (...) {
		return {};
	}
}

} // namespace

extern "C" void test_cpp_exceptions(void)
{
	bool caught = false;
	try {
		throw std::runtime_error("thrown");
	} catch (const std::runtime_error &error) {
		caught = std::string(error.what()) == "thrown";
	}
	check("exception thrown and caught", caught);

	std::exception_ptr error;
	std::jthread([&] {
		try {
			throw std::out_of_range("from another thread");
		} catch (...) {
			error = std::current_exception();
		}
	}).join();
	check("exception passed between threads", message_of(error) == "from another thread");

	Circle circle;
	Shape *shape = &circle;
	const Named *named = dynamic_cast<Named *>(shape);
	check("dynamic_cast and typeid", named && named->name == "named" && typeid(*shape) == typeid(Circle));
}
