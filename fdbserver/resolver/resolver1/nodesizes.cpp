#include "bnodestore.h"

#include <cstdio>

namespace {

template<template<std::size_t> class Layout, std::size_t C>
void printLayout(const char* name) {
	constexpr std::size_t size = sizeof(Layout<C>);

	std::printf("%s\n", name);
	std::printf("  capacity           %zu\n", C);
	std::printf("  size               %zu bytes\n", size);
	std::printf("  unused in block    %zu bytes\n", BlockSize - size);

	if constexpr (C < 64) {
		std::printf("  size at capacity %zu %zu bytes\n",
					C + 1, sizeof(Layout<C + 1>));
	}
}

} // namespace

int main() {
	std::printf("BlockSize            %zu bytes\n", BlockSize);
	std::printf("BNodeBlockHeader     %zu bytes\n", sizeof(BNodeHeader));
	std::printf("KeyRef               %zu bytes\n", sizeof(KeyRef));
	std::printf("Version              %zu bytes\n", sizeof(Version));
	std::printf("pointer              %zu bytes\n", sizeof(void*));
	std::printf("\n");

	printLayout<InternalBlockLayout, InternalCapacity>("InternalBlock");
	std::printf("\n");
	printLayout<LeafBlockLayout, LeafCapacity>("LeafBlock");
}
