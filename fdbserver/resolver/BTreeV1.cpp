/*
 * BTreeV1.cpp
 *
 * Compiles btree-v1, the sources of fdbserver/resolver/resolver1, as one unit inside
 * namespace btree_v1. ConflictSet.cpp declares the same types through resolver1's headers,
 * in the same namespace, and calls them directly. resolver1 is its own project with its own
 * tests. Its only change for FDB is a branch in fdbcompat.h, taken under RESOLVER1_IN_FDB,
 * that uses FDB's StringRef.
 */

#include "flow/Arena.h"

// Every standard header resolver1 includes, first, so that its own includes, inside the
// namespace below, find them already included.
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#define RESOLVER1_IN_FDB 1
namespace btree_v1 {
#include "resolver1/pagepool.cpp"
#include "resolver1/keystore.cpp"
#include "resolver1/bnodestore.cpp"
#include "resolver1/tempconflictbtree.cpp"
} // namespace btree_v1
#undef RESOLVER1_IN_FDB
