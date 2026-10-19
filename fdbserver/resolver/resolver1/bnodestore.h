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

struct BNodeHeader {
	std::uint64_t generation;
	Version maxCV = 0;
	BNodeHeader* agePrevious = nullptr;
	BNodeHeader* ageNext = nullptr;
	BNodeHeader* parent = nullptr;
	BNodeKind kind;
	std::uint8_t count = 0;
	std::uint8_t parentSlot = 0;
	std::uint8_t childrenNeedingSplit = 0;
	// Bit i is set when slot i is free. Set by BNodeStore on allocation.
	std::uint64_t freeSlots = 0;
	// Physical slot of each logical rank, sized for the largest capacity.
	std::uint8_t order[64]{};
	inline void setFree (std::size_t slot) {
		freeSlots |= std::uint64_t{1} << slot;
	}
	inline void unsetFree (std::size_t slot) {
		freeSlots &= ~(std::uint64_t{1} << slot);
	}
	inline bool hasSpace() const {
		return freeSlots != 0;
	}
	inline unsigned freeSlotCount() const {
		return static_cast<unsigned>(std::popcount(freeSlots));
	}
	// Returns the lowest free slot. Requires a free slot.
	inline unsigned firstFree() const {
		assert(freeSlots != 0);
		return static_cast<unsigned>(std::countr_zero(freeSlots));
	}
};

template<std::size_t C>
struct InternalBlockLayout {
	static_assert(C > 0 && C <= 64);

	BNodeHeader header;
	Version maxSubtreesCV[C];
	Version separatorsCV[C];
	KeyRef separators[C];
	BNodeHeader* children[C];
};

template<std::size_t C>
struct LeafBlockLayout {
	static_assert(C > 0 && C <= 64);

	BNodeHeader header;
	Version cv[C];
	RangeRef ranges[C];
	LeafBlockLayout<C>* left = nullptr;
	LeafBlockLayout<C>* right = nullptr;
};

template<template<std::size_t> class Layout, std::size_t C = 1>
constexpr std::size_t capacityForBlock() {
	if constexpr (sizeof(Layout<C>) > BlockSize) {
		return C - 1;
	} else if constexpr (C >= 64) {
		return 64;
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

using InternalBlock = InternalBlockLayout<InternalCapacity>;
using LeafBlock = LeafBlockLayout<LeafCapacity>;

inline constexpr std::uint64_t InternalSlotMask =
	UINT64_MAX >> (64 - InternalCapacity);

inline constexpr std::uint64_t LeafSlotMask =
	UINT64_MAX >> (64 - LeafCapacity);

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
	inline void updateCV(BNodeHeader* node, Version cv);

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

private:
	PagePool& pool_;
	KeyStore& keyStore_;
	std::uint64_t nextGeneration_ = 1;
	BNodeHeader* oldestNode_ = nullptr;
	BNodeHeader* newestNode_ = nullptr;

	inline void unlinkNode(BNodeHeader* node);
	inline void insertLast(BNodeHeader* node);
};

inline LeafBlock* BNodeStore::allocateLeaf(Version cv) {
	assert(newestNode_ == nullptr || cv >= newestNode_->maxCV);
	std::byte* block= pool_.allocateBlock();
	if (block==nullptr) return nullptr;
	LeafBlock* leaf = ::new(block) LeafBlock{};
	leaf->header.generation= nextGeneration_++;
	leaf->header.kind= BNodeKind::Leaf;
	leaf->header.freeSlots = LeafSlotMask;
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
	internal->header.freeSlots = InternalSlotMask;
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
	assert(node->header.freeSlots != 0);
	assert(rank <= node->header.count);

	const unsigned slot = node->header.firstFree();
	std::memmove(&node->header.order[rank+1], &node->header.order[rank], 
		(node->header.count - rank) * sizeof(node->header.order[0]));
	++node->header.count;
	node->header.order[rank]=static_cast<std::uint8_t>(slot);
	node->maxSubtreesCV[slot] = child->maxCV;
	node->separatorsCV[slot] = cv;
	node->separators[slot]=key;
	node->children[slot]=child;
	node->header.freeSlots&=~(std::uint64_t{1}<<slot);
	child->parent = &node->header;
	child->parentSlot = static_cast<std::uint8_t>(slot);
}

inline void BNodeStore::insertAt(std::size_t rank, 
							const RangeRef& range, 
							Version cv,
							LeafBlock* node) {
	assert(node != nullptr);
	assert(node->header.freeSlots != 0);
	assert(rank <= node->header.count);

	const unsigned slot = node->header.firstFree();
	std::memmove(&node->header.order[rank+1], &node->header.order[rank], 
		(node->header.count - rank) * sizeof(node->header.order[0]));
	++node->header.count;
	node->header.order[rank]=static_cast<std::uint8_t>(slot);
	node->header.freeSlots&=~(std::uint64_t{1}<<slot);
	node->cv[slot] = cv;
	node->ranges[slot] = range;
}

inline bool BNodeStore::findKey(const StringRef& key,
							std::size_t& rank,
							const InternalBlock* node) const {
	std::size_t lo=0;
	std::size_t hi= node->header.count;
	std::size_t mid= lo+(hi-lo)/2;
	int comp;
	while (lo < hi){
		comp= keyStore_.compareKeys(key,node->separators[node->header.order[mid]]);
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
		comp= keyStore_.compareBegin(key,node->ranges[node->header.order[mid]]);
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

	const unsigned slot = node->header.order[rank];
	std::memmove(&node->header.order[rank], &node->header.order[rank+1], 
		(node->header.count - rank - 1) * sizeof(node->header.order[0]));
	--node->header.count;
	node->header.freeSlots|= std::uint64_t{1}<<slot;
}

inline void BNodeStore::eraseAt(
		std::size_t rank,
		LeafBlock* node) {
	assert(node != nullptr);
	assert(rank < node->header.count);

	const unsigned slot = node->header.order[rank];
	std::memmove(&node->header.order[rank], &node->header.order[rank+1], 
		(node->header.count - rank - 1) * sizeof(node->header.order[0]));
	--node->header.count;
	node->header.freeSlots|= std::uint64_t{1}<<slot;
}