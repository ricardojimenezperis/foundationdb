#include "bnodestore.h"

#include <cassert>

BNodeStore::BNodeStore(PagePool& pool, KeyStore& keyStore)
	: pool_(pool) , keyStore_(keyStore){
	assert(pool_.blockSize() == BlockSize);
}

BNodeStore::~BNodeStore() {
	while (oldestNode_!=nullptr){
		BNodeHeader* tmp=oldestNode_;
		oldestNode_=oldestNode_->ageNext;
		pool_.releaseBlock(tmp);
	}
}
