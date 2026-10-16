// types.h
#pragma once

#include <cstddef>
#include <cstdint>

using Version = std::int64_t;

inline constexpr std::size_t KiB = 1024;
inline constexpr std::size_t PageSize = 64 * KiB;
inline constexpr std::size_t BlockSize = 2 * KiB;