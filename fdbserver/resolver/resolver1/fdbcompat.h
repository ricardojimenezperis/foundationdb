#pragma once

#include "common.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

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

template<class T>
class VectorRef;

struct ReadConflictRange {
	StringRef begin;
	StringRef end;
	Version version;
	int transaction;
	int indexInTx;
	VectorRef<int>* conflictingKeyRange;
	Arena* cKRArena;

	ReadConflictRange(
		StringRef begin,
		StringRef end,
		Version version,
		int transaction,
		int indexInTx,
		VectorRef<int>* conflictingKeyRange = nullptr,
		Arena* cKRArena = nullptr)
		: begin(begin),
		  end(end),
		  version(version),
		  transaction(transaction),
		  indexInTx(indexInTx),
		  conflictingKeyRange(conflictingKeyRange),
		  cKRArena(cKRArena) {
	}

	bool operator<(const ReadConflictRange& rhs) const {
		return compare(begin, rhs.begin) < 0;
	}
};
