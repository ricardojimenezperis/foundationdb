// Evidence harness, not part of resolver1: runs the one registered test whose
// name equals argv[1], so that a test whose name is a prefix of another name
// runs alone. Otherwise identical in behaviour to testmain.cpp.
#include "test.h"

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
	if (argc != 2) {
		std::cerr << "usage: " << argv[0] << " <exact test name>\n";
		return 2;
	}
	const std::string_view name = argv[1];
	for (const auto& entry : test::registry()) {
		if (std::string_view(entry.name) != name) continue;
		try {
			entry.run();
			std::cout << "PASS " << entry.name << "\n1 passed, 0 failed\n";
			return 0;
		} catch (const std::exception& error) {
			std::cerr << "FAIL " << entry.name << "\n  " << error.what() << '\n';
			return 1;
		}
	}
	std::cerr << "No matching test\n";
	return 1;
}
