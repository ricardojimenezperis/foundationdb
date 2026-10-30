#pragma once

#include "common.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#ifdef RESOLVER1_IN_FDB
// Built inside FDB: FDB's own StringRef, VectorRef and Arena, and its batch records from
// ResolverBatchTypes.h. That header must be included before the namespace resolver1 is compiled
// in, so that resolver1 uses the same types as ConflictSet.cpp.
#ifndef FDBSERVER_RESOLVER_BATCH_TYPES_H
#error "include ResolverBatchTypes.h before the namespace that resolver1 is compiled in"
#endif
using StringRef = ::StringRef;
using ReadConflictRange = ::ReadConflictRange;
using KeyInfo = ::KeyInfo;
using TransactionInfo = ::TransactionInfo;
#else
class StringRef {
public:
	StringRef() : data(nullptr), length(0) {}
	StringRef(const uint8_t* data, int length) : data(data), length(length) {}

	const uint8_t* begin() const { return data; }
	const uint8_t* end() const { return data + length; }
	int size() const { return length; }
	bool empty() const { return length == 0; }

	int compare(const StringRef& other) const {
		int minSize = std::min(size(), other.size());
		if (minSize != 0) {
			int c = memcmp(begin(), other.begin(), minSize);
			if (c != 0)
				return c;
		}
		return (size() > other.size()) - (size() < other.size());
	}

private:
	const uint8_t* data;
	int length;
};

inline int compare(const StringRef& l, const StringRef& r) {
	return l.compare(r);
}

class Arena;

// The members of FDB's VectorRef that the batch records and their readers use: a view of size
// elements from data.
template<class T>
class VectorRef {
public:
	VectorRef() = default;
	VectorRef(T* data, int size) : data_(data), size_(size) {}

	T* begin() const { return data_; }
	T* end() const { return data_ + size_; }
	int size() const { return size_; }
	T& operator[](int i) const { return data_[i]; }

private:
	T* data_ = nullptr;
	int size_ = 0;
};

// ReadConflictRange, KeyInfo and TransactionInfo, as FDB's ConflictSet.cpp defines its batch
#include "../ResolverBatchTypes.h"
#endif
