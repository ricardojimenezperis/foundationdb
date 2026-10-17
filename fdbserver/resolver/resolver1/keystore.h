// Key store
//
// Stores variable-length keys in blocks allocated from a shared PagePool.
// Key bytes are appended sequentially and may span multiple linked blocks.
//
// Each key reference identifies its first block, offset, length, and generation.
// Key blocks receive increasing generations and are reclaimed in allocation order.
// References are checked against the last reclaimed generation without accessing
// block headers.
//
// The caller determines the commit-version cutoff for reclamation.
// No reclamation may occur while key bytes are being accessed.

#pragma once
#include "pagepool.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <optional>
#include "common.h"
#include "fdbcompat.h"

struct KeyBlockHeader {
    KeyBlockHeader* nextBlock = nullptr;
    std::uint64_t generation;
    Version maxCV;
};

struct KeyRef {
    const KeyBlockHeader* firstBlock;
    std::uint16_t keyOffset;
    std::uint16_t keyLength;
    std::uint64_t blockGen;
};

struct RangeRef {
	const KeyBlockHeader* firstBlock;
	std::uint16_t keyOffset;
	std::uint16_t beginLength;
	std::uint16_t endStart;
	std::uint16_t endLength;
	std::uint64_t blockGen;
};

static_assert(sizeof(RangeRef) == 24);

class KeyStore {
	public:
		explicit KeyStore(PagePool& pool);
		~KeyStore();

		KeyStore(const KeyStore&) = delete;
		KeyStore& operator=(const KeyStore&) = delete;

		std::optional<KeyRef> appendKey(const std::byte* key,
                 std::uint16_t length,
                 Version cv);
		std::optional<Version> oldestCV() const;
		std::optional<Version> reclaimOldest();
		const std::byte* keyStart(KeyRef ref) const;
		bool isValid(KeyRef ref) const;
		const KeyBlockHeader* keyBlock(KeyRef ref) const;
		std::size_t blockSize() const;
		int compareKeys(const KeyRef& kr1, const KeyRef& kr2) const;
		int compareKeys(const StringRef& key, const KeyRef& stored) const;

		// Precondition: begin < end. Neither key is longer than 65,535 bytes.
		std::optional<RangeRef> appendRange(
			const StringRef& begin,
			const StringRef& end,
			Version cv);
		// [source.begin, end) and [begin, source.end). Precondition: the
		// range is not empty; source is valid and is not reclaimed during
		// the call.
		std::optional<RangeRef> appendRange(
			const RangeRef& source,
			const StringRef& end,
			Version cv);
		std::optional<RangeRef> appendRange(
			const StringRef& begin,
			const RangeRef& source,
			Version cv);
		bool isValid(const RangeRef& ref) const;
		KeyRef beginKey(const RangeRef& ref) const;
		int compareBegin(const StringRef& key, const RangeRef& stored) const;
		int compareEnd(const StringRef& key, const RangeRef& stored) const;
	private:
		struct RecordStart {
			KeyBlockHeader* block;
			std::uint16_t offset;
			std::uint64_t generation;
		};

		// Bytes to copy into a record: contiguous bytes, or stored bytes
		// starting at offset in block when bytes is nullptr.
		struct RecordSource {
			const std::byte* bytes;
			const KeyBlockHeader* block;
			std::size_t offset;
			std::size_t length;
		};

		static RecordSource external(const StringRef& key);
		static RecordSource stored(const KeyRef& key);

		std::optional<RecordStart> appendRecord(
			const RecordSource& first,
			const RecordSource& second,
			Version cv);
		std::optional<RangeRef> appendRange(
			const RecordSource& begin,
			const RecordSource& end,
			bool point,
			Version cv);
		std::optional<RangeRef> appendWithStoredBegin(
			const KeyRef& begin,
			const StringRef& end,
			Version cv);
		std::optional<RangeRef> appendWithStoredEnd(
			const StringRef& begin,
			const KeyRef& end,
			Version cv);
		KeyRef endKey(const RangeRef& ref) const;
		void locate(
			const KeyBlockHeader*& block,
			std::size_t& offset,
			std::size_t start) const;
		int compareFrom(
			const StringRef& key,
			const KeyBlockHeader* block,
			std::size_t offset,
			std::size_t length) const;
		int compareAt(
			const StringRef& key,
			const RangeRef& stored,
			std::size_t start,
			std::size_t length) const;
		PagePool& pool_;
		std::size_t blockSize_;
		KeyBlockHeader* oldestKeyBlock_ = nullptr;
		KeyBlockHeader* openKeyBlock_ = nullptr;  	// nullptr if no available blocks
		std::size_t openKeyBlockUsedBytes_ = 0;
		KeyBlockHeader* newestKeyBlock_ = nullptr;  // used to link the last block to a new block 
													// when openKeyBlock_ is nullptr and a new block is allocated
		std::uint64_t nextGeneration_ = 1;
		std::uint64_t lastReclaimedKeyGeneration_ = 0;
};
