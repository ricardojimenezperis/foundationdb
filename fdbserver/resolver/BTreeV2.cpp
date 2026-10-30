/*
 * BTreeV2.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2026 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Compiles btree-v2, the sources of fdbserver/resolver/resolver1, as one unit inside
 * namespace btree_v2. ConflictSet.cpp declares the same types through resolver1's headers,
 * in the same namespace, and calls them directly. resolver1 is its own project with its own
 * tests. Its only change for FDB is a branch in fdbcompat.h, taken under RESOLVER1_IN_FDB,
 * that uses FDB's StringRef and the batch records of ResolverBatchTypes.h.
 */

#include "flow/Arena.h"
#include "fdbclient/FDBTypes.h"

// ResolverBatchTypes.h requires a free compare(StringRef, StringRef) declared before it
static force_inline int compare(const StringRef& a, const StringRef& b) {
	const size_t aSize = a.size();
	const size_t bSize = b.size();
	const size_t minSize = std::min(aSize, bSize);
	int c = memcmp(a.begin(), b.begin(), minSize);
	if (c)
		return (c > 0) - (c < 0); // normalize to +1/-1
	return (aSize > bSize) - (aSize < bSize);
}

#include "ResolverBatchTypes.h"

#ifndef BTREE_V2_TASKS
#error "BTREE_V2_TASKS, the task capacity of checkReads, is set by fdbserver/resolver/CMakeLists.txt"
#endif

// Every standard header resolver1 includes, first, so that its own includes, inside the
// namespace below, find them already included.
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#define RESOLVER1_IN_FDB 1
namespace btree_v2 {
#include "resolver1/pagepool.cpp"
#include "resolver1/keystore.cpp"
#include "resolver1/bnodestore.cpp"
#include "resolver1/tempconflictbtree.cpp"

// The configured task capacity, when it is not one tempconflictbtree.cpp instantiates
#if BTREE_V2_TASKS != 16 && BTREE_V2_TASKS != 32
template void TempConflictBTree::checkReads<BTREE_V2_TASKS>(const std::vector<KeyInfo>&,
                                                            TransactionInfo* const*,
                                                            const std::vector<ReadConflictRange>&,
                                                            const std::vector<int>&,
                                                            bool*,
                                                            std::vector<uint8_t>&,
                                                            int64_t&);
#endif
} // namespace btree_v2
#undef RESOLVER1_IN_FDB
