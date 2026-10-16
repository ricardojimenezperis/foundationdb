#pragma once

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {

struct Case {
	const char* name;
	void (*run)();
};

inline std::vector<Case>& registry() {
	static std::vector<Case> cases;
	return cases;
}

struct Register {
	Register(const char* name, void (*run)()) {
		registry().push_back({name, run});
	}
};

inline void check(bool passed,
				  const char* expression,
				  const char* file,
				  int line) {
	if (!passed) {
		std::ostringstream message;
		message << file << ':' << line
				<< ": check failed: " << expression;
		throw std::runtime_error(message.str());
	}
}

} // namespace test

#define TEST_JOIN_IMPL(a, b) a##b
#define TEST_JOIN(a, b) TEST_JOIN_IMPL(a, b)

#define TEST_CASE_IMPL(name, line)                                      \
	static void TEST_JOIN(testCase_, line)();                           \
	static test::Register TEST_JOIN(testRegistration_, line)(           \
		name, &TEST_JOIN(testCase_, line));                             \
	static void TEST_JOIN(testCase_, line)()

#define TEST_CASE(name) TEST_CASE_IMPL(name, __LINE__)

#define ASSERT(expression)                                             \
	test::check(static_cast<bool>(expression),                          \
				#expression, __FILE__, __LINE__)

#define ASSERT_EQ(actual, expected) ASSERT((actual) == (expected))