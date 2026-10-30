// One write of long keys, then Writes short point writes, against the same
// short writes alone; prints both reclaimCVs. Mirrors the test
// AddWritesKeepsTheReserveOfALongKeyAfterReclaimingIt.
#include "tempconflictbtree.h"
#include "keystore.h"
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using Bytes = std::vector<uint8_t>;
static StringRef ref(const Bytes& b) { return StringRef(b.data(), static_cast<int>(b.size())); }
static void add(TempConflictBTree& t, const Bytes& b, const Bytes& e, Version cv) {
	const std::pair<StringRef, StringRef> w{ref(b), ref(e)};
	t.addWrites(std::span<const std::pair<StringRef, StringRef>>(&w, 1), cv);
}
static std::size_t writeBlocks(std::size_t length) {
	const std::size_t payload = BlockSize - sizeof(KeyBlockHeader);
	return 3 * ((2 * length + payload - 1) / payload) + 2;
}

int main(int argc, char** argv) {
	const std::size_t length = std::strtoul(argv[1], nullptr, 10);
	const std::size_t pages = std::strtoul(argv[2], nullptr, 10);
	const unsigned writes = 20000;
	TempConflictBTree plain(pages * PageSize), afterLong(pages * PageSize);
	Bytes lb(length, 'a'), le(length, 'z');
	lb[0] = le[0] = 0xff;
	add(afterLong, lb, le, 1);
	for (unsigned v = 0; v < writes; ++v) {
		Bytes k{'k', static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
		Bytes k1 = k;
		k1.push_back(0);
		add(plain, k, k1, 2 + v);
		add(afterLong, k, k1, 2 + v);
	}
	const Version last = 1 + writes;
	const std::size_t reserve = std::max<std::size_t>(35, writeBlocks(length));
	std::printf("block=%zu length=%zu pages=%zu poolBlocks=%zu reserve=%zu writes=%u "
				"reclaimCV plain=%lld afterLong=%lld retained plain=%lld afterLong=%lld\n",
				BlockSize, length, pages, pages * PageSize / BlockSize, reserve, writes,
				(long long)plain.reclaimCV(), (long long)afterLong.reclaimCV(),
				(long long)(last - plain.reclaimCV()), (long long)(last - afterLong.reclaimCV()));
}
