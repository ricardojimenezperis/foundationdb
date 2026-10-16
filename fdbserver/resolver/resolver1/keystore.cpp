#include "keystore.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>
#include <vector>

KeyStore::KeyStore(PagePool& pool)
	: pool_(pool), blockSize_(pool.blockSize()) {
	assert(blockSize_ > sizeof(KeyBlockHeader));
}

KeyStore::~KeyStore() {
	while (oldestKeyBlock_ != nullptr) {
		KeyBlockHeader* tmp= oldestKeyBlock_;
		oldestKeyBlock_=oldestKeyBlock_->nextBlock;
		pool_.releaseBlock(tmp);
	}
}

std::optional<KeyRef> KeyStore::appendKey(
	const std::byte* key,
	std::uint16_t length,
	Version cv) {
	if (length == 0) {
		return KeyRef{nullptr, 0, 0, 0};
	}
	const std::optional<RecordStart> start =
		appendRecord(key, length, nullptr, 0, cv);
	if (!start) {
		return std::nullopt;
	}
	return KeyRef{start->block, start->offset, length, start->generation};
}

std::optional<KeyStore::RecordStart> KeyStore::appendRecord(
	const std::byte* first,
	std::size_t firstLength,
	const std::byte* second,
	std::size_t secondLength,
	Version cv) {
	const std::size_t length = firstLength + secondLength;
	assert(length != 0);

	// Without an open block, the key starts in a new block.
	const bool needsOpenBlock = openKeyBlock_ == nullptr;
	const std::size_t usedBytes =
		needsOpenBlock ? sizeof(KeyBlockHeader) : openKeyBlockUsedBytes_;

	// Calculate how many additional blocks the key needs.
	const std::size_t availableBytes =
		blockSize_ - usedBytes;
	const std::size_t payloadBytes =
		blockSize_ - sizeof(KeyBlockHeader);
	const std::size_t missingBytes =
		length > availableBytes ? length - availableBytes : 0;
	const std::size_t additionalBlocks =
		missingBytes / payloadBytes +
		(missingBytes % payloadBytes != 0);
	const std::size_t neededBlocks = 1 + additionalBlocks;

	// Reserve all required blocks before modifying existing key storage.
	std::vector<KeyBlockHeader*> blocks(neededBlocks, nullptr);
	blocks[0] = openKeyBlock_;

	const std::size_t offset = usedBytes;
	const std::size_t firstNewBlock = needsOpenBlock ? 0 : 1;

	for (std::size_t i = firstNewBlock; i < neededBlocks; ++i) {
		std::byte* block = pool_.allocateBlock();
		if (block == nullptr) {
			for (std::size_t j = firstNewBlock; j < i; ++j) {
				pool_.releaseBlock(blocks[j]);
			}
			return std::nullopt;
		}

		blocks[i] = ::new (block) KeyBlockHeader{};
		blocks[i]->nextBlock = nullptr;
		blocks[i]->generation = nextGeneration_++;
	}

	if (needsOpenBlock) {
		if (newestKeyBlock_ != nullptr) {
			newestKeyBlock_->nextBlock = blocks[0];
		} else {
			oldestKeyBlock_ = blocks[0];
		}
	}

	const std::uint64_t gen = blocks[0]->generation;

	// Copy the record and link its blocks.
	std::size_t bytesToCopy = length;
	const std::byte* origin = first;
	std::size_t originBytes = firstLength;

	for (std::size_t i = 0; i < neededBlocks; ++i) {
		const std::size_t writeOffset =
			i == 0 ? offset : sizeof(KeyBlockHeader);
		const std::size_t bytesCanCopy =
			std::min(bytesToCopy, blockSize_ - writeOffset);

		auto* dest =
			reinterpret_cast<std::byte*>(blocks[i]) + writeOffset;

		// The record is the first source followed by the second one.
		for (std::size_t copied = 0; copied < bytesCanCopy;) {
			if (originBytes == 0) {
				origin = second;
				originBytes = secondLength;
			}
			const std::size_t chunk =
				std::min(bytesCanCopy - copied, originBytes);
			std::memcpy(dest + copied, origin, chunk);
			copied += chunk;
			origin += chunk;
			originBytes -= chunk;
		}
		blocks[i]->maxCV = std::max(blocks[i]->maxCV, cv);

		if (i + 1 < neededBlocks) {
			blocks[i]->nextBlock = blocks[i + 1];
		} else {
			openKeyBlockUsedBytes_ = writeOffset + bytesCanCopy;
		}

		bytesToCopy -= bytesCanCopy;
	}

	newestKeyBlock_ = blocks.back();
	openKeyBlock_ = newestKeyBlock_;

	// Keep an open block with free space whenever the pool allows it.
	if (openKeyBlockUsedBytes_ == blockSize_) {
		std::byte* block = pool_.allocateBlock();

		if (block == nullptr) {
			openKeyBlock_ = nullptr;
		} else {
			auto* header =
				::new (block) KeyBlockHeader{};

			header->nextBlock = nullptr;
			header->generation = nextGeneration_++;

			newestKeyBlock_->nextBlock = header;
			newestKeyBlock_ = header;
			openKeyBlock_ = header;
			openKeyBlockUsedBytes_ = sizeof(KeyBlockHeader);
		}
	}

	return RecordStart{
		blocks[0],
		static_cast<std::uint16_t>(offset),
		gen
	};
}

std::optional<Version> KeyStore::reclaimOldest() {
	if (oldestKeyBlock_==nullptr){
		return std::nullopt;
	}
	const Version cv = oldestKeyBlock_-> maxCV;
	lastReclaimedKeyGeneration_=oldestKeyBlock_->generation;
	KeyBlockHeader* tmp= oldestKeyBlock_;
	oldestKeyBlock_= oldestKeyBlock_->nextBlock;
	pool_.releaseBlock(tmp);
	if (oldestKeyBlock_==nullptr){
		openKeyBlock_=nullptr;
		newestKeyBlock_=nullptr;
		openKeyBlockUsedBytes_=0;
	}
	return cv;
}

std::optional<Version> KeyStore::oldestCV() const {
	if (oldestKeyBlock_==nullptr) {
		return std::nullopt;
	}
	return oldestKeyBlock_->maxCV;
}

const std::byte* KeyStore::keyStart(KeyRef ref) const {
	if (ref.keyLength == 0) {
		return nullptr;
	}
	return reinterpret_cast<const std::byte*>(ref.firstBlock)+ref.keyOffset;
}

bool KeyStore::isValid(KeyRef ref) const {
	return ref.keyLength == 0 ||
		ref.blockGen > lastReclaimedKeyGeneration_;
}

const KeyBlockHeader* KeyStore::keyBlock(KeyRef ref) const {
	return ref.firstBlock;
}

std::size_t KeyStore::blockSize() const {
	return blockSize_;
}

int KeyStore::compareKeys(const KeyRef& kr1, const KeyRef& kr2) const {
	const KeyBlockHeader* block1=kr1.firstBlock;
	const KeyBlockHeader* block2=kr2.firstBlock;
	std::size_t bytes1 = std::min<std::size_t>(kr1.keyLength, blockSize_ - kr1.keyOffset);
	std::size_t bytes2 = std::min<std::size_t>(kr2.keyLength, blockSize_ - kr2.keyOffset);
	std::size_t rem1 = kr1.keyLength;
	std::size_t rem2 = kr2.keyLength;
	std::size_t usable = blockSize_-sizeof(KeyBlockHeader);
	const std::byte* start1=keyStart(kr1);
	const std::byte* start2=keyStart(kr2);
	int res=0;
	while (rem1!=0 && rem2!=0) {
		std::size_t adv= std::min(bytes1, bytes2);
		res=memcmp(start1, start2, adv);
		if (res != 0) {
			return res;
		}
		rem1-=adv;
		rem2-=adv;
		bytes1-=adv;
		bytes2-=adv;
		start1+=adv;
		start2+=adv;
		if (bytes1 == 0 && rem1 != 0) {
			block1=block1->nextBlock;
			start1=reinterpret_cast<const std::byte*> (block1)+sizeof(KeyBlockHeader);
			bytes1 = std::min(rem1, usable);
		}
		if (bytes2 == 0 && rem2 != 0) {
				block2=block2->nextBlock;
			start2=reinterpret_cast<const std::byte*> (block2)+sizeof(KeyBlockHeader);
			bytes2 = std::min(rem2, usable);
		}
	}
	if (kr1.keyLength==kr2.keyLength){
		return 0;
	}
	if (kr1.keyLength<kr2.keyLength){
		return -1;
	} else {
		return 1;
	}		
}

int KeyStore::compareKeys(const StringRef& key, const KeyRef& stored) const {
	return compareFrom(key, stored.firstBlock, stored.keyOffset, stored.keyLength);
}

int KeyStore::compareFrom(
	const StringRef& key,
	const KeyBlockHeader* block,
	std::size_t offset,
	std::size_t length) const {

	const std::byte* start1=reinterpret_cast<const std::byte*>(key.begin());
	std::size_t rem1 = static_cast<std::size_t>(key.size());
	const KeyBlockHeader* block2=block;
	std::size_t bytes2 = std::min<std::size_t>(length, blockSize_ - offset);
	std::size_t rem2 = length;
	std::size_t usable = blockSize_-sizeof(KeyBlockHeader);
	const std::byte* start2=length == 0 ? nullptr :
		reinterpret_cast<const std::byte*>(block2)+offset;
	while (rem1!=0 && rem2!=0) {
		std::size_t adv= std::min(rem1, bytes2);
		int res=memcmp(start1, start2, adv);
		if (res != 0) {
			return res;
		}
		rem1-=adv;
		rem2-=adv;
		bytes2-=adv;
		start1+=adv;
		start2+=adv;
		if (bytes2 == 0 && rem2 != 0) {
			block2=block2->nextBlock;
			start2=reinterpret_cast<const std::byte*> (block2)+sizeof(KeyBlockHeader);
			bytes2 = std::min(rem2, usable);
		}
	}
	if (rem1==0 && rem2==0){
		return 0;
	}
	if (rem1==0){
		return -1;
	} else {
		return 1;
	}
}

std::optional<RangeRef> KeyStore::appendRange(
	const StringRef& begin,
	const StringRef& end,
	Version cv) {
	assert(begin.size() <= std::numeric_limits<std::uint16_t>::max());
	assert(end.size() <= std::numeric_limits<std::uint16_t>::max());

	const auto* beginBytes = reinterpret_cast<const std::byte*>(begin.begin());
	const auto* endBytes = reinterpret_cast<const std::byte*>(end.begin());
	const std::size_t beginLength = static_cast<std::size_t>(begin.size());
	const std::size_t endLength = static_cast<std::size_t>(end.size());

	// A point range [k, k\0) is stored once, as the bytes of its end.
	const bool point = endLength == beginLength + 1 &&
		end.begin()[beginLength] == 0 &&
		(beginLength == 0 ||
		 memcmp(begin.begin(), end.begin(), beginLength) == 0);

	const std::size_t endStart = point ? 0 : beginLength;
	assert(endStart == 0 || endStart == beginLength);

	const std::optional<RecordStart> start = point
		? appendRecord(endBytes, endLength, nullptr, 0, cv)
		: appendRecord(beginBytes, beginLength, endBytes, endLength, cv);
	if (!start) {
		return std::nullopt;
	}

	return RangeRef{
		start->block,
		start->offset,
		static_cast<std::uint16_t>(beginLength),
		static_cast<std::uint16_t>(endStart),
		static_cast<std::uint16_t>(endLength),
		start->generation
	};
}

bool KeyStore::isValid(const RangeRef& ref) const {
	return ref.blockGen > lastReclaimedKeyGeneration_;
}

KeyRef KeyStore::beginKey(const RangeRef& ref) const {
	return KeyRef{
		ref.firstBlock,
		ref.keyOffset,
		ref.beginLength,
		ref.blockGen
	};
}

int KeyStore::compareBegin(const StringRef& key, const RangeRef& stored) const {
	return compareAt(key, stored, 0, stored.beginLength);
}

int KeyStore::compareEnd(const StringRef& key, const RangeRef& stored) const {
	return compareAt(key, stored, stored.endStart, stored.endLength);
}

int KeyStore::compareAt(
	const StringRef& key,
	const RangeRef& stored,
	std::size_t start,
	std::size_t length) const {
	// Moves start bytes into the record, skipping block headers.
	const KeyBlockHeader* block = stored.firstBlock;
	std::size_t offset = stored.keyOffset;
	while (start >= blockSize_ - offset) {
		start -= blockSize_ - offset;
		block = block->nextBlock;
		offset = sizeof(KeyBlockHeader);
	}
	return compareFrom(key, block, offset + start, length);
}
