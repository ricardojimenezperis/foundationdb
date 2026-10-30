// B+ node store
//
// Stores fixed-capacity B+ tree nodes in blocks allocated from a shared PagePool.
// Each node occupies one block; released blocks can be reused for nodes or keys.
//
// Manages node allocation, initialization, and release.
// Tracks nodes by age and exposes the oldest reclamation candidate.
// Node age can change after allocation, so allocation order is not age order.
//
// Tree navigation, splits, separators, and parent-child and leaf links are
// maintained by the caller. A node must be detached from the tree and removed
// from the age index before its block is returned to the pool.
//
// The caller determines the commit-version cutoff for reclamation.

#pragma once
#include "pagepool.h"
#include "common.h"
#include "keystore.h"
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <optional>
#include <type_traits>

 
enum class BNodeKind : std::uint8_t {
	Leaf,
	Internal
};

// Defined below; declared here so that a node can point to its parent.
struct InternalBlock;

struct BNodeHeader {
	std::uint64_t generation;
	Version maxCV = 0;
	BNodeHeader* agePrevious = nullptr;
	BNodeHeader* ageNext = nullptr;
	InternalBlock* parent = nullptr;
	BNodeKind kind;
	std::uint8_t count = 0;
	std::uint8_t parentSlot = 0;
	std::uint8_t childrenNeedingSplit = 0;
	// Rank of this node in its parent. Requires a parent.
	inline std::size_t parentRank() const;
};

// The free slots of a node of capacity C: bit i is set when slot i is free.
// Up to 64 slots it is a single word, and every operation is the one on a
// plain uint64_t; larger nodes take one word per 64 slots. setFree and
// unsetFree touch one word; any, count and first look at every word.
template<std::size_t C>
struct FreeSlots {
	static constexpr std::size_t Words = (C + 63) / 64;

	// Word w of a map with every slot free. Every shift is below 64, even in
	// the branch not taken.
	inline static constexpr std::uint64_t fullWord(std::size_t w) {
		return w + 1 < Words || C % 64 == 0 ? UINT64_MAX : (std::uint64_t{1} << (C % 64)) - 1;
	}

	// Every slot starts free.
	inline FreeSlots() {
		for (std::size_t w = 0; w < Words; ++w) {
			words[w] = fullWord(w);
		}
	}

	inline bool isFree(std::size_t slot) const {
		if constexpr (Words == 1) {
			return (words[0] >> slot) & 1;
		} else {
			return (words[slot / 64] >> (slot % 64)) & 1;
		}
	}
	inline void setFree(std::size_t slot) {
		if constexpr (Words == 1) {
			words[0] |= std::uint64_t{1} << slot;
		} else {
			words[slot / 64] |= std::uint64_t{1} << (slot % 64);
		}
	}
	inline void unsetFree(std::size_t slot) {
		if constexpr (Words == 1) {
			words[0] &= ~(std::uint64_t{1} << slot);
		} else {
			words[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
		}
	}
	inline bool any() const {
		if constexpr (Words == 1) {
			return words[0] != 0;
		} else {
			for (std::uint64_t w : words) {
				if (w != 0) {
					return true;
				}
			}
			return false;
		}
	}
	inline unsigned count() const {
		if constexpr (Words == 1) {
			return static_cast<unsigned>(std::popcount(words[0]));
		} else {
			unsigned n = 0;
			for (std::uint64_t w : words) {
				n += static_cast<unsigned>(std::popcount(w));
			}
			return n;
		}
	}
	// The lowest free slot. Requires a free slot.
	inline unsigned first() const {
		assert(any());
		if constexpr (Words == 1) {
			return static_cast<unsigned>(std::countr_zero(words[0]));
		} else {
			std::size_t w = 0;
			while (words[w] == 0) {
				++w;
			}
			return static_cast<unsigned>(64 * w + std::countr_zero(words[w]));
		}
	}

	std::uint64_t words[Words];
};

// count, parentSlot, childrenNeedingSplit and order hold slots and ranks in
// a std::uint8_t, so a node has at most 255 slots.
inline constexpr std::size_t MaxNodeCapacity = 255;

template<std::size_t C>
struct InternalBlockLayout {
	static_assert(C > 0 && C <= MaxNodeCapacity);

	BNodeHeader header;
	Version maxSubtreesCV[C];
	Version separatorsCV[C];
	KeyRef separators[C];
	BNodeHeader* children[C];
	FreeSlots<C> freeSlots;
	// Physical slot of each logical rank.
	std::uint8_t order[C]{};

	InternalBlock* parent() const {
		return header.parent;
	}
	std::size_t parentRank() const {
		return header.parentRank();
	}
	bool hasSpace() const {
		return freeSlots.any();
	}
	unsigned freeSlotCount() const {
		return freeSlots.count();
	}
	// The lowest free slot. Requires a free slot.
	unsigned firstFree() const {
		return freeSlots.first();
	}
	void setFree(std::size_t slot) {
		freeSlots.setFree(slot);
	}
	void unsetFree(std::size_t slot) {
		freeSlots.unsetFree(slot);
	}
};

template<std::size_t C>
struct LeafBlockLayout {
	static_assert(C > 0 && C <= MaxNodeCapacity);

	BNodeHeader header;
	Version cv[C];
	RangeRef ranges[C];
	LeafBlockLayout<C>* left = nullptr;
	LeafBlockLayout<C>* right = nullptr;
	FreeSlots<C> freeSlots;
	// Physical slot of each logical rank.
	std::uint8_t order[C]{};

	InternalBlock* parent() const {
		return header.parent;
	}
	std::size_t parentRank() const {
		return header.parentRank();
	}
	bool hasSpace() const {
		return freeSlots.any();
	}
	unsigned freeSlotCount() const {
		return freeSlots.count();
	}
	// The lowest free slot. Requires a free slot.
	unsigned firstFree() const {
		return freeSlots.first();
	}
	void setFree(std::size_t slot) {
		freeSlots.setFree(slot);
	}
	void unsetFree(std::size_t slot) {
		freeSlots.unsetFree(slot);
	}
};

template<template<std::size_t> class Layout, std::size_t C = 1>
constexpr std::size_t capacityForBlock() {
	if constexpr (sizeof(Layout<C>) > BlockSize) {
		return C - 1;
	} else if constexpr (C >= MaxNodeCapacity) {
		return MaxNodeCapacity;
	} else {
		return capacityForBlock<Layout, C + 1>();
	}
}

inline constexpr std::size_t InternalCapacity =
	capacityForBlock<InternalBlockLayout>();

inline constexpr std::size_t LeafCapacity =
	capacityForBlock<LeafBlockLayout>();

static_assert(InternalCapacity > 0);
static_assert(LeafCapacity > 0);

// A struct rather than an alias, so that BNodeHeader can declare it before
// its capacity is known.
struct InternalBlock : InternalBlockLayout<InternalCapacity> {};

inline std::size_t BNodeHeader::parentRank() const {
	assert(parent != nullptr);
	std::size_t rank = 0;
	while (parent->order[rank] != parentSlot) {
		++rank;
		assert(rank < parent->header.count);
	}
	return rank;
}
using LeafBlock = LeafBlockLayout<LeafCapacity>;

// The first word of a map with every slot free.
inline constexpr std::uint64_t InternalSlotMask = FreeSlots<InternalCapacity>::fullWord(0);
inline constexpr std::uint64_t LeafSlotMask = FreeSlots<LeafCapacity>::fullWord(0);

static_assert(sizeof(InternalBlock) <= BlockSize);
static_assert(sizeof(LeafBlock) <= BlockSize);

static_assert(std::is_standard_layout_v<InternalBlock>);
static_assert(std::is_standard_layout_v<LeafBlock>);

class BNodeStore {
public:
	explicit BNodeStore(PagePool& pool, KeyStore& keyStore);
	~BNodeStore();

	BNodeStore(const BNodeStore&) = delete;
	BNodeStore& operator=(const BNodeStore&) = delete;

	inline LeafBlock* allocateLeaf(Version cv);
	inline InternalBlock* allocateInternal(Version cv);

	// node should be previously unlinked from the tree
	inline void releaseNode(BNodeHeader* node);
	void releaseNode(LeafBlock* node) {
		releaseNode(&node->header);
	}
	void releaseNode(InternalBlock* node) {
		releaseNode(&node->header);
	}
	inline void updateCV(BNodeHeader* node, Version cv);
	void updateCV(LeafBlock* node, Version cv) {
		updateCV(&node->header, cv);
	}
	void updateCV(InternalBlock* node, Version cv) {
		updateCV(&node->header, cv);
	}

	inline std::optional<Version> oldestCV() const;
	inline BNodeHeader* oldestNode() const;

	inline bool findKey(
		const StringRef& key,
		std::size_t& rank,
		const InternalBlock* node) const;

	inline bool findKey(
		const StringRef& key,
		std::size_t& rank,
		const LeafBlock* node) const;

	inline void insertAt(
		std::size_t rank,
		const KeyRef& key,
		BNodeHeader* child,
		Version cv,
		InternalBlock* node);

	inline void insertAt(
		std::size_t rank,
		const RangeRef& range,
		Version cv,
		LeafBlock* node);

	inline void eraseAt(
		std::size_t rank,
		InternalBlock* node);

	inline void eraseAt(
		std::size_t rank,
		LeafBlock* node);

	inline void unlinkNode(BNodeHeader* node);

private:
	PagePool& pool_;
	KeyStore& keyStore_;
	std::uint64_t nextGeneration_ = 1;
	BNodeHeader* oldestNode_ = nullptr;
	BNodeHeader* newestNode_ = nullptr;

	inline void insertLast(BNodeHeader* node);
};

inline LeafBlock* BNodeStore::allocateLeaf(Version cv) {
	assert(newestNode_ == nullptr || cv >= newestNode_->maxCV);
	std::byte* block= pool_.allocateBlock();
	if (block==nullptr) return nullptr;
	LeafBlock* leaf = ::new(block) LeafBlock{};
	leaf->header.generation= nextGeneration_++;
	leaf->header.kind= BNodeKind::Leaf;
	leaf->header.maxCV=cv;
	insertLast(&leaf->header);
	return leaf;
}

inline InternalBlock* BNodeStore::allocateInternal(Version cv) {
	assert(newestNode_ == nullptr || cv >= newestNode_->maxCV);
	std::byte* block= pool_.allocateBlock();
	if (block==nullptr) return nullptr;
	InternalBlock* internal = ::new(block) InternalBlock{};
	internal->header.generation= nextGeneration_++;
	internal->header.kind= BNodeKind::Internal;
	internal->header.maxCV=cv;
	insertLast(&internal->header);
	return internal;
}

inline void BNodeStore::insertLast(BNodeHeader* node) {
	node->agePrevious = newestNode_;
	node->ageNext = nullptr;
	if (newestNode_ == nullptr) {
		oldestNode_ = node;
	} else {
		newestNode_->ageNext = node;
	}
	newestNode_ = node;
}

inline void BNodeStore::unlinkNode(BNodeHeader* node) {
	if (node->ageNext == nullptr){
		newestNode_=node->agePrevious;
	} else {
		node->ageNext->agePrevious=node->agePrevious;
	}
	if (node->agePrevious == nullptr){
		oldestNode_=node->ageNext;
	} else {
		node->agePrevious->ageNext=node->ageNext;
	}
}

inline void BNodeStore::releaseNode(BNodeHeader* node) {
	assert(node!=nullptr);
	unlinkNode(node);
	pool_.releaseBlock(node);
}

inline void BNodeStore::updateCV(BNodeHeader* node, Version cv) {
	assert(node != nullptr);
	assert(cv >= node->maxCV);
	if (cv == node->maxCV) {
		return;
	}
	assert(cv >= newestNode_->maxCV);
	node->maxCV = cv;
	if (node != newestNode_) {
		unlinkNode(node);
		insertLast(node);
	}
}

inline std::optional<Version> BNodeStore::oldestCV() const {
	if (oldestNode_ == nullptr) {
		return std::nullopt;
	}
	return oldestNode_->maxCV;
}

inline BNodeHeader* BNodeStore::oldestNode() const {
	return oldestNode_;
}

inline void BNodeStore::insertAt(std::size_t rank, 
							const KeyRef& key, 
							BNodeHeader* child,
							Version cv,
							InternalBlock* node) {
	assert(node != nullptr);
	assert(child != nullptr);	
	assert(node->hasSpace());
	assert(rank <= node->header.count);

	const unsigned slot = node->firstFree();
	std::memmove(&node->order[rank+1], &node->order[rank], 
		(node->header.count - rank) * sizeof(node->order[0]));
	++node->header.count;
	node->order[rank]=static_cast<std::uint8_t>(slot);
	node->maxSubtreesCV[slot] = child->maxCV;
	node->separatorsCV[slot] = cv;
	node->separators[slot]=key;
	node->children[slot]=child;
	node->unsetFree(slot);
	child->parent = node;
	child->parentSlot = static_cast<std::uint8_t>(slot);
}

inline void BNodeStore::insertAt(std::size_t rank, 
							const RangeRef& range, 
							Version cv,
							LeafBlock* node) {
	assert(node != nullptr);
	assert(node->hasSpace());
	assert(rank <= node->header.count);

	const unsigned slot = node->firstFree();
	std::memmove(&node->order[rank+1], &node->order[rank], 
		(node->header.count - rank) * sizeof(node->order[0]));
	++node->header.count;
	node->order[rank]=static_cast<std::uint8_t>(slot);
	node->unsetFree(slot);
	node->cv[slot] = cv;
	node->ranges[slot] = range;
	if (cv > node->header.maxCV) {
		updateCV(node, cv);
	}
}

inline bool BNodeStore::findKey(const StringRef& key,
							std::size_t& rank,
							const InternalBlock* node) const {
	std::size_t lo=0;
	std::size_t hi= node->header.count;
	std::size_t mid= lo+(hi-lo)/2;
	int comp;
	while (lo < hi){
		comp= keyStore_.compareKeys(key,node->separators[node->order[mid]]);
		if (comp==0) {
			rank=mid;
			return true;
		}
		if (comp<0) {
			hi=mid;
		} else {
			lo=mid+1;
		}
		mid=lo+(hi-lo)/2;
	}
	rank=lo;
	return false;
}

inline bool BNodeStore::findKey(const StringRef& key,
							std::size_t& rank,
							const LeafBlock* node) const {
	assert(node != nullptr);

	std::size_t lo=0;
	std::size_t hi= node->header.count;
	std::size_t mid= lo+(hi-lo)/2;
	int comp;
	while (lo < hi){
		comp= keyStore_.compareBegin(key,node->ranges[node->order[mid]]);
		if (comp==0) {
			rank=mid;
			return true;
		}
		if (comp<0) {
			hi=mid;
		} else {
			lo=mid+1;
		}
		mid=lo+(hi-lo)/2;
	}
	rank=lo;
	return false;
}

inline void BNodeStore::eraseAt(
		std::size_t rank,
		InternalBlock* node) {
	assert(node != nullptr);
	assert(rank < node->header.count);

	const unsigned slot = node->order[rank];
	std::memmove(&node->order[rank], &node->order[rank+1], 
		(node->header.count - rank - 1) * sizeof(node->order[0]));
	--node->header.count;
	node->setFree(slot);
}

inline void BNodeStore::eraseAt(
		std::size_t rank,
		LeafBlock* node) {
	assert(node != nullptr);
	assert(rank < node->header.count);

	const unsigned slot = node->order[rank];
	std::memmove(&node->order[rank], &node->order[rank+1], 
		(node->header.count - rank - 1) * sizeof(node->order[0]));
	--node->header.count;
	node->setFree(slot);
}