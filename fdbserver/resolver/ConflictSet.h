/*
 * ConflictSet.h
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

#ifndef CONFLICTSET_H
#define CONFLICTSET_H
#pragma once

#include <utility>
#include <vector>

#include "fdbclient/CommitTransaction.h"
#include "fdbserver/core/ConflictBatch.h"
#include "fdbserver/resolver/ResolverBug.h"

struct ConflictSet;
ConflictSet* newConflictSet();
void clearConflictSet(ConflictSet*, Version);
void destroyConflictSet(ConflictSet*);

// What a conflict set reports to the by5 harness and to skiplisttest. Phase times are the
// process-wide totals of the batch phases, in seconds. The other fields describe a history kept by
// capacity rather than by the window; they stay zero for FDB's skip list.
struct ConflictSetStats {
	// True for a history kept by capacity, which has a boundary of its own
	bool paged = false;
	double checkReadSeconds = 0, checkIntraBatchSeconds = 0, combineSeconds = 0, mergeSeconds = 0,
	       cleanupSeconds = 0;
	// Oldest read version the history can still check
	Version validationMinRV = 0;
	int64_t poolLimitPages = 0, poolFreePages = 0, poolHeapPages = 0, poolIndexPages = 0;
	int64_t reclaimedHistoryPages = 0;
	// Transactions the external window admitted and the history's own boundary rejected
	int64_t tooOldWithinWindow = 0;
	// Largest distance by which the history's boundary has passed the external window
	int64_t boundaryPastWindow = 0;
	int64_t btreeNodes = 0, btreeLeaves = 0;
};
// The history's name, as the harness and skiplisttest print it
const char* conflictSetIndexName(const ConflictSet*);
ConflictSetStats getConflictSetStats(const ConflictSet*);

struct ConflictBatch : ConflictBatchStatus {
	explicit ConflictBatch(ConflictSet*,
	                       std::map<int, VectorRef<int>>* conflictingKeyRangeMap = nullptr,
	                       Arena* resolveBatchReplyArena = nullptr);
	~ConflictBatch();

	void addTransaction(const CommitTransactionRef& transaction, Version newOldestVersion);
	void detectConflicts(Version now,
	                     Version newOldestVersion,
	                     std::vector<int>& nonConflicting,
	                     std::vector<int>* tooOldTransactions = nullptr);
	void GetTooOldTransactions(std::vector<int>& tooOldTransactions);

private:
	ConflictSet* cs;
	Standalone<VectorRef<struct TransactionInfo*>> transactionInfo;
	std::vector<struct KeyInfo> points;
	int transactionCount;
	std::vector<std::pair<StringRef, StringRef>> combinedWriteConflictRanges;
	std::vector<struct ReadConflictRange> combinedReadConflictRanges;
	bool* transactionConflictStatus;
	// Stores the map: a transaction -> conflicted transactions' indices
	std::map<int, VectorRef<int>>* conflictingKeyRangeMap;
	Arena* resolveBatchReplyArena;
	std::shared_ptr<ResolverBug> bugs = SimBugInjector().get<ResolverBug>(ResolverBugID());

	// bug injection
	bool ignoreTooOld() const;
	bool ignoreWriteSet() const;
	bool ignoreReadSet() const;

	void checkIntraBatchConflicts();
	void combineWriteConflictRanges();
	void checkReadConflictRanges();
	void mergeWriteConflictRanges(Version now);
	void addConflictRanges(Version now,
	                       std::vector<std::pair<StringRef, StringRef>>::iterator begin,
	                       std::vector<std::pair<StringRef, StringRef>>::iterator end,
	                       class SkipList* part);
};

#endif
