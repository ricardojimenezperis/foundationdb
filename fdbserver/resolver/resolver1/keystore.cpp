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
		appendRecord(RecordSource{key, nullptr, 0, length}, RecordSource{}, cv);
	if (!start) {
		return std::nullopt;
	}
	return KeyRef{start->block, start->offset, length, start->generation};
}

KeyStore::RecordSource KeyStore::external(const StringRef& key) {
	return RecordSource{
		reinterpret_cast<const std::byte*>(key.begin()),
		nullptr,
		0,
		static_cast<std::size_t>(key.size())
	};
}

KeyStore::RecordSource KeyStore::stored(const KeyRef& key) {
	return RecordSource{nullptr, key.firstBlock, key.keyOffset, key.keyLength};
}

std::optional<KeyStore::RecordStart> KeyStore::appendRecord(
	const RecordSource& first,
	const RecordSource& second,
	Version cv) {
	const std::size_t length = first.length + second.length;
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

	// Copy the record and link its blocks. The record is the first source
	// followed by the second one; a stored source is read block by block.
	std::size_t bytesToCopy = length;
	const std::byte* contiguous = first.bytes;
	const KeyBlockHeader* sourceBlock = first.block;
	std::size_t sourceOffset = first.offset;
	std::size_t sourceLeft = first.length;

	for (std::size_t i = 0; i < neededBlocks; ++i) {
		const std::size_t writeOffset =
			i == 0 ? offset : sizeof(KeyBlockHeader);
		const std::size_t bytesCanCopy =
			std::min(bytesToCopy, blockSize_ - writeOffset);

		auto* dest =
			reinterpret_cast<std::byte*>(blocks[i]) + writeOffset;

		for (std::size_t copied = 0; copied < bytesCanCopy;) {
			if (sourceLeft == 0) {
				contiguous = second.bytes;
				sourceBlock = second.block;
				sourceOffset = second.offset;
				sourceLeft = second.length;
			}
			std::size_t chunk = std::min(bytesCanCopy - copied, sourceLeft);
			const std::byte* origin;
			if (contiguous != nullptr) {
				origin = contiguous;
				contiguous += chunk;
			} else {
				if (sourceOffset == blockSize_) {
					sourceBlock = sourceBlock->nextBlock;
					sourceOffset = sizeof(KeyBlockHeader);
				}
				chunk = std::min(chunk, blockSize_ - sourceOffset);
				origin = reinterpret_cast<const std::byte*>(sourceBlock) +
					sourceOffset;
				sourceOffset += chunk;
			}
			std::memcpy(dest + copied, origin, chunk);
			copied += chunk;
			sourceLeft -= chunk;
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
	assert(isValid(kr1) && isValid(kr2));
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
	assert(isValid(stored));
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

	const std::size_t beginLength = static_cast<std::size_t>(begin.size());
	const std::size_t endLength = static_cast<std::size_t>(end.size());

	// A point range [k, k\0) is stored once, as the bytes of its end.
	const bool point = endLength == beginLength + 1 &&
		end.begin()[beginLength] == 0 &&
		(beginLength == 0 ||
		 memcmp(begin.begin(), end.begin(), beginLength) == 0);

	return appendRange(external(begin), external(end), point, cv);
}

std::optional<RangeRef> KeyStore::appendRange(
	const RangeRef& source,
	const StringRef& end,
	Version cv) {
	assert(isValid(source));
	return appendWithStoredBegin(beginKey(source), end, cv);
}

std::optional<RangeRef> KeyStore::appendRange(
	const StringRef& begin,
	const RangeRef& source,
	Version cv) {
	assert(isValid(source));
	return appendWithStoredEnd(begin, endKey(source), cv);
}

std::optional<RangeRef> KeyStore::appendWithStoredBegin(
	const KeyRef& begin,
	const StringRef& end,
	Version cv) {
	assert(end.size() <= std::numeric_limits<std::uint16_t>::max());

	const std::size_t beginLength = begin.keyLength;
	const std::size_t endLength = static_cast<std::size_t>(end.size());

	const bool point = endLength == beginLength + 1 &&
		end.begin()[beginLength] == 0 &&
		(beginLength == 0 ||
		 compareFrom(StringRef(end.begin(), static_cast<int>(beginLength)),
					 begin.firstBlock, begin.keyOffset, beginLength) == 0);

	return appendRange(stored(begin), external(end), point, cv);
}

std::optional<RangeRef> KeyStore::appendWithStoredEnd(
	const StringRef& begin,
	const KeyRef& end,
	Version cv) {
	assert(begin.size() <= std::numeric_limits<std::uint16_t>::max());

	const std::size_t beginLength = static_cast<std::size_t>(begin.size());
	const std::size_t endLength = end.keyLength;

	bool point = false;
	if (endLength == beginLength + 1) {
		const KeyBlockHeader* block = end.firstBlock;
		std::size_t offset = end.keyOffset;
		locate(block, offset, beginLength);
		point = reinterpret_cast<const std::byte*>(block)[offset] == std::byte{0} &&
			(beginLength == 0 ||
			 compareFrom(begin, end.firstBlock, end.keyOffset, beginLength) == 0);
	}

	return appendRange(external(begin), stored(end), point, cv);
}

std::optional<RangeRef> KeyStore::appendRange(
	const RecordSource& begin,
	const RecordSource& end,
	bool point,
	Version cv) {
	const std::optional<RecordStart> start = point
		? appendRecord(end, RecordSource{}, cv)
		: appendRecord(begin, end, cv);
	if (!start) {
		return std::nullopt;
	}

	return RangeRef{
		start->block,
		start->offset,
		static_cast<std::uint16_t>(begin.length),
		static_cast<std::uint16_t>(point ? 0 : begin.length),
		static_cast<std::uint16_t>(end.length),
		start->generation
	};
}

bool KeyStore::isValid(const RangeRef& ref) const {
	return ref.blockGen > lastReclaimedKeyGeneration_;
}

KeyRef KeyStore::endKey(const RangeRef& ref) const {
	const KeyBlockHeader* block = ref.firstBlock;
	std::size_t offset = ref.keyOffset;
	locate(block, offset, ref.endStart);
	return KeyRef{
		block,
		static_cast<std::uint16_t>(offset),
		ref.endLength,
		block->generation
	};
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
	assert(isValid(stored));
	return compareAt(key, stored, 0, stored.beginLength);
}

int KeyStore::compareEnd(const StringRef& key, const RangeRef& stored) const {
	assert(isValid(stored));
	return compareAt(key, stored, stored.endStart, stored.endLength);
}

int KeyStore::compareAt(
	const StringRef& key,
	const RangeRef& stored,
	std::size_t start,
	std::size_t length) const {
	const KeyBlockHeader* block = stored.firstBlock;
	std::size_t offset = stored.keyOffset;
	locate(block, offset, start);
	return compareFrom(key, block, offset, length);
}

// Moves start bytes into stored data, skipping block headers.
void KeyStore::locate(
	const KeyBlockHeader*& block,
	std::size_t& offset,
	std::size_t start) const {
	while (start >= blockSize_ - offset) {
		start -= blockSize_ - offset;
		block = block->nextBlock;
		offset = sizeof(KeyBlockHeader);
	}
	offset += start;
}
