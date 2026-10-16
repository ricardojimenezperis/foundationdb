#include "test.h"

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
	const std::string_view prefix = argc > 1 ? argv[1] : "";

	std::size_t passed = 0;
	std::size_t failed = 0;

	for (const auto& entry : test::registry()) {
		if (!std::string_view(entry.name).starts_with(prefix)) {
			continue;
		}

		try {
			entry.run();
			++passed;
			std::cout << "PASS " << entry.name << '\n';
		} catch (const std::exception& error) {
			++failed;
			std::cerr << "FAIL " << entry.name
					  << "\n  " << error.what() << '\n';
		} catch (...) {
			++failed;
			std::cerr << "FAIL " << entry.name
					  << "\n  Unknown exception\n";
		}
	}

	if (passed + failed == 0) {
		std::cerr << "No matching tests\n";
		return 1;
	}

	std::cout << passed << " passed, "
			  << failed << " failed\n";

	return failed == 0 ? 0 : 1;
}