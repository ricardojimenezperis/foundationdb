// types.h
#pragma once

#include <cstddef>
#include <cstdint>

using Version = std::int64_t;

inline constexpr std::size_t KiB = 1024;
inline constexpr std::size_t PageSize = 64 * KiB;

// The size of a pool block, which holds one node or one block of keys. It is
// chosen when compiling, with -DRESOLVER1_BLOCK_SIZE=<bytes>, 2 KiB by default.
#ifndef RESOLVER1_BLOCK_SIZE
#define RESOLVER1_BLOCK_SIZE 2048
#endif
inline constexpr std::size_t BlockSize = RESOLVER1_BLOCK_SIZE;
static_assert(BlockSize >= 1 * KiB && BlockSize <= PageSize && (BlockSize & (BlockSize - 1)) == 0,
              "BlockSize must be a power of two between 1 KiB and a page");
