# Classification of every instrumented condition without an independence pair,
# and of the constant-folded one. Line numbers are those of the tree of commit
# "Add MC/DC coverage tests for final Resolver v3"; tempconflictbtree.cpp unless
# another file is named.
A = 'assert condition'
U = 'unreachable (invariant)'
D = 'defensive'
L0 = 'LG=0 only'
N = 'reachability not established'
F = 'constant folded (not counted)'
def assert_just(expr):
	return ('Condition of `assert(%s)`. Its false outcome calls `__assert_rtn` and aborts the process; every '
		'execution is true. llvm-cov places all conditions of the macro at the position of the call. '
		'Counted in the 210 and not covered.' % expr)
TOOOLD = ('`readWriteBegins` is built only by `checkReads` (cleared at 1814, filled at 1861). A begin of a transaction '
	'already too old is skipped (1838); otherwise the transaction is marked too old and skipped (1839-1845). The test at '
	'1839 depends only on the transaction and on `reclaimCV_`, which `checkReads` does not change (tempconflictbtree.h:90), '
	'so all begins of a transaction take the same branch and none of a too-old transaction is pushed. resolver1 sets '
	'`tooOld` only at 1841; the caller sets it before the batch (ConflictSet.cpp:827-842) and, between `checkReads` and '
	'`addWrites`, changes only `transactionConflictStatus` (ConflictSet.cpp:942-963, 988-1000). Precondition: '
	'`addWrites` applies the batch that the preceding `checkReads` prepared (tempconflictbtree.h:88-109).')
ROWS = [
 ('bnodestore.h','74:10-74:38','C2','C % 64 == 0',F,'`C` is the template parameter of `FreeSlots<C>`, so the condition is a compile-time constant in each instantiation. llvm-cov reports it as constant folded and leaves it out of the 210 conditions.'),
 ('bnodestore.h','356:2-356:2','C1','newestNode_ == nullptr',A,assert_just('newestNode_ == nullptr || cv >= newestNode_->maxCV')),
 ('bnodestore.h','356:2-356:2','C2','cv >= newestNode_->maxCV',A,assert_just('newestNode_ == nullptr || cv >= newestNode_->maxCV')),
 ('bnodestore.h','372:2-372:2','C1','newestNode_ == nullptr',A,assert_just('newestNode_ == nullptr || cv >= newestNode_->maxCV')),
 ('bnodestore.h','372:2-372:2','C2','cv >= newestNode_->maxCV',A,assert_just('newestNode_ == nullptr || cv >= newestNode_->maxCV')),
 ('keystore.cpp','247:2-247:2','C1','isKeyValid(kr1)',A,assert_just('isKeyValid(kr1) && isKeyValid(kr2)')),
 ('keystore.cpp','247:2-247:2','C2','isKeyValid(kr2)',A,assert_just('isKeyValid(kr1) && isKeyValid(kr2)')),
 ('pagepool.cpp','20:2-20:2','C1','blockSize >= sizeof(FreeBlock)',A,assert_just('blockSize >= sizeof(FreeBlock) && blockSize <= PageSize && blockSize % alignof(FreeBlock) == 0')),
 ('pagepool.cpp','20:2-20:2','C2','blockSize <= PageSize',A,assert_just('blockSize >= sizeof(FreeBlock) && blockSize <= PageSize && blockSize % alignof(FreeBlock) == 0')),
 ('pagepool.cpp','20:2-20:2','C3','blockSize % alignof(FreeBlock) == 0',A,assert_just('blockSize >= sizeof(FreeBlock) && blockSize <= PageSize && blockSize % alignof(FreeBlock) == 0')),
 ('tempconflictbtree.cpp','59:8-59:69','C1','!info.tooOld',U,'Always true. '+TOOOLD),
 ('tempconflictbtree.cpp','134:10-134:65','C1','t.write.first.size() != 0',U,'Both pairs need `{F F = F}`, an accepted interval whose begin and end keys are both empty. An interval is accepted only with begin index < end index (65), and merging only raises the end (66-68). `sortPoints` puts every end before every begin at equal keys (ConflictSet.cpp:98-100, 126-142), so begin index < end index implies begin key < end key, and the end key is not empty: whenever C1 is false, C2 is true.'),
 ('tempconflictbtree.cpp','134:10-134:65','C2','t.write.second.size() != 0',U,'As C1 of this decision: the end key of an accepted interval is never empty, so the outcome is never false.'),
 ('tempconflictbtree.cpp','143:7-143:48','C1','!t.hasSavedLeaf',U,'Always false. `hasSavedLeaf` is false only in a default `WriteTask` (102), which exists only between 170 and `resetSearch` at 174, which sets it (159, 140). `savedLeafIsCurrent` is called only inside `advance` (247, 287; assertions 388, 396, 429), after `fill`.'),
 ('tempconflictbtree.cpp','145:10-146:50','C1','(generation & 1) != 0',N,'False only if the block of the saved leaf was released (even generation) and not reused when the task checks it. A leaf can be released while another task has saved it (reclamation in a head task\'s reservation, or removal of an emptied leaf); no run produced an even generation here, and no invariant excluding it was established. C2 of this decision has a pair: a reused block (another generation) was observed.'),
 ('tempconflictbtree.cpp','231:10-231:61','C2','hintLeaf(t.scan->left)',U,'Evaluated only when the scan leaf has one entry and no right neighbour (229-231), and false only when its left link is null. The scan starts in the cursor leaf at rank + 1 after the insertion (442-443), so that leaf holds the inserted entry and the scanned one (two or more entries). A later scan leaf is reached through `right` (455), and the cursor leaf, which keeps the inserted entry, stays linked on its left. Always true.'),
 ('tempconflictbtree.cpp','236:3-236:3','C1','!t.owned',A,assert_just('!t.owned || head')),
 ('tempconflictbtree.cpp','236:3-236:3','C2','head',A,assert_just('!t.owned || head')),
 ('tempconflictbtree.cpp','275:5-275:5','C1','head',A,assert_just('head && t.owned')),
 ('tempconflictbtree.cpp','275:5-275:5','C2','t.owned',A,assert_just('head && t.owned')),
 ('tempconflictbtree.cpp','286:9-287:28','C3','!savedLeafIsCurrent(t)',N,'Needs the reservation at 283 to change the saved leaf (generation or mutation epoch) while the cursor leaf (C1) and the cut (C2) stay the same. No run produced it, and it was not shown that a reservation changes leaves only by raising the cut.'),
 ('tempconflictbtree.cpp','304:9-305:76','C2','cleanLeafCV_ != reclaimCV_',N,'Needs a task that is not the head to inspect a leaf after the cut was raised and before any `cleanExpired` call resynchronized `cleanLeafCV_` (1367-1369). No run produced it, and no invariant excluding it was established.'),
 ('tempconflictbtree.cpp','329:12-330:57','C1','t.scanRank < t.scan->header.count',N,'False only when every entry of the right neighbour inspected at `InspectRight` is expired; that neighbour is not compacted before this loop. No run produced it, and no invariant excluding it was established.'),
 ('tempconflictbtree.cpp','404:10-404:95','C1','t.scan->cv[slot] > reclaimCV_',U,'Always true. With rank > 0 the scan is the cursor leaf, compacted under the current cut: a head task cleans it at `Inspect` (308) or after moving right (348); a task that is not the head searches only leaves already clean under the current cut (304-306) and parks at `SearchReady` (386); a change of the cut resets the search (245-252, and 286-292 after the task\'s own reservation). `cleanExpired` removes every entry with CV <= `reclaimCV_` (1365-1391), and an entry inserted later carries the batch\'s CV, above the cut.'),
 ('tempconflictbtree.cpp','404:10-404:95','C2','hintKey(t, t.scan->ranges[slot], true, Step::Insert)',U,'`hintKey` returns false only for length 0 (196). With `wholeRange` the length is at least `endStart + endLength`, the length of the stored end key. Every stored range has begin < end: a write is inserted (430) only after 276 skips begin >= end, and an exterior keeps the part of an existing range on one side of a key of the write. So the end key is not empty and the condition is always true.'),
 ('tempconflictbtree.cpp','422:10-422:95','C2','hintKey(t, t.scan->ranges[slot], true, Step::Insert)',U,'As 404 C2: the stored end key is never empty, so `hintKey` returns true whenever it is evaluated.'),
 ('tempconflictbtree.cpp','518:7-518:7','C1','which == 0',A,assert_just('which == 0 && i == group.first')),
 ('tempconflictbtree.cpp','518:7-518:7','C2','i == group.first',A,assert_just('which == 0 && i == group.first')),
 ('tempconflictbtree.cpp','617:8-617:67','C1','info.tooOld',L0,'In `addWritesSequential`, called only when `RESOLVER1_WRITE_LG` is 0 (`if constexpr`, 29-31); no vector in the 2x8 build. In the separate LG=0 measurement it has no pair either, for the reason given at 59 C1: no entry of `readWriteBegins` belongs to a too-old transaction.'),
 ('tempconflictbtree.cpp','617:8-617:67','C2','transactionConflictStatus[entry.transaction]',L0,'In `addWritesSequential`, called only when `RESOLVER1_WRITE_LG` is 0 (29-31); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement.'),
 ('tempconflictbtree.cpp','627:8-627:45','C1','hasPending',L0,'In `addWritesSequential`, called only when `RESOLVER1_WRITE_LG` is 0 (29-31); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement.'),
 ('tempconflictbtree.cpp','627:8-627:45','C2','beginIndex > pendingEnd',L0,'In `addWritesSequential`, called only when `RESOLVER1_WRITE_LG` is 0 (29-31); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement.'),
 ('tempconflictbtree.cpp','661:2-661:2','C1','writeDestinationsActive_',A,assert_just('writeDestinationsActive_ && group < writeLeaves.size()')),
 ('tempconflictbtree.cpp','661:2-661:2','C2','group < writeLeaves.size()',A,assert_just('writeDestinationsActive_ && group < writeLeaves.size()')),
 ('tempconflictbtree.cpp','712:6-712:64','C2','activeWriteCursor_ == nullptr',U,'Its pair needs `{T F = F}`: destinations inactive while a cursor is active. `activeWriteCursor_` is set only at 267 (inside `addWrites`) and 816 (inside `addWritesSequential`), and reset by guards (258, 819) before those functions return. Both activate the destinations first (39, 550; `writeLeaves` is not empty by 35-37 and 544-548, so 646 sets the flag) and clear them only when they return (42, 553); the only other clear is at the start of `checkReads` (1812). Unreachable in every build.'),
 ('tempconflictbtree.cpp','745:2-745:2','C1','ticket >= writeLeafBufferFirst_',A,assert_just('ticket >= writeLeafBufferFirst_ && ticket < writeLeafBufferNext_')),
 ('tempconflictbtree.cpp','745:2-745:2','C2','ticket < writeLeafBufferNext_',A,assert_just('ticket >= writeLeafBufferFirst_ && ticket < writeLeafBufferNext_')),
 ('tempconflictbtree.cpp','753:3-753:3','C1','writeLeaves.empty()',A,assert_just('writeLeaves.empty() || writeLeaves.back().firstWrite < buffered.group.firstWrite')),
 ('tempconflictbtree.cpp','753:3-753:3','C2','writeLeaves.back().firstWrite < buffered.group.firstWrite',A,assert_just('writeLeaves.empty() || writeLeaves.back().firstWrite < buffered.group.firstWrite')),
 ('tempconflictbtree.cpp','803:6-803:41','C1','leaf == nullptr',D,'Both pairs need `{T T = T}`, which throws `std::logic_error("missing prepared write destination")`: a cursor without a leaf in a non-empty tree. Before `prepareWriteSplit` (389), a null cursor leaf is replaced by `rightmostLeaf_` (284; `resetSearch` anchors on it at 151), and `rightmostLeaf_` is null only when `root_` is null (1187-1189, 1424-1427). The check guards that invariant; no fault was injected to violate it.'),
 ('tempconflictbtree.cpp','803:6-803:41','C2','root_ != nullptr',D,'As C1 of this decision: the true outcome is the defensive `std::logic_error` for a cursor without a leaf in a non-empty tree.'),
 ('tempconflictbtree.cpp','838:12-839:53','C1','first < right->header.count',L0,'In `addWrite`, called only from `addWritesSequential` (576), only when `RESOLVER1_WRITE_LG` is 0 (29-31); no vector in the 2x8 build. In the separate LG=0 measurement it has no pair: false only when every entry of a right neighbour is expired, as at 329 C1.'),
 ('tempconflictbtree.cpp','838:12-839:53','C2','right->cv[right->order[first]] <= reclaimCV_',L0,'In `addWrite`, called only when `RESOLVER1_WRITE_LG` is 0 (29-31, 576); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement.'),
 ('tempconflictbtree.cpp','926:7-927:54','C1','leaf->cv[slot] > reclaimCV_',L0,'In `eraseCoveredRanges`, called only from `addWrite` (863), only when `RESOLVER1_WRITE_LG` is 0 (29-31); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement.'),
 ('tempconflictbtree.cpp','926:7-927:54','C2','keyStore_.compareEnd(end, leaf->ranges[slot]) < 0',L0,'In `eraseCoveredRanges`, only when `RESOLVER1_WRITE_LG` is 0 (29-31, 863); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement.'),
 ('tempconflictbtree.cpp','1414:6-1414:40','C2','!needsSplit(parent)',U,'Its pair needs a parent that needed a split before (C1) and still needs one after an empty child is erased. An internal node needs a split when `childrenNeedingSplit == freeSlotCount()` (tempconflictbtree.h:318-320). The erased child is empty (1398), so it does not need a split and is not counted (comment at 1409); erasing it (1410) adds a free slot and leaves the count unchanged. Always true when evaluated.'),
 ('tempconflictbtree.cpp','1519:6-1519:74','C2','activeWriteCursor_->leaf == oldNode',U,'Its pair needs an active cursor and a different leaf being split. `splitLeaf` is called only by `split` (1461), only from `prepareWriteSplit` (807), with `leaf` bound to the cursor\'s leaf (783) and asserted equal to the split node (1460); a leaf candidate other than the cursor\'s is discarded when the cursor moves (349, 788-799). `prepareWriteSplit` runs at 389 with the head task\'s cursor, the active one (267), and at 851 with `addWrite`\'s cursor, the active one (816). Unreachable in every build.'),
 ('tempconflictbtree.cpp','1611:2-1611:2','C1','splitDelta >= -1',A,assert_just('splitDelta >= -1 && splitDelta <= 1')),
 ('tempconflictbtree.cpp','1611:2-1611:2','C2','splitDelta <= 1',A,assert_just('splitDelta >= -1 && splitDelta <= 1')),
 ('tempconflictbtree.cpp','1629:4-1629:4','C1','splitDelta > 0',A,assert_just('splitDelta > 0 || parent->header.childrenNeedingSplit > 0')),
 ('tempconflictbtree.cpp','1629:4-1629:4','C2','parent->header.childrenNeedingSplit > 0',A,assert_just('splitDelta > 0 || parent->header.childrenNeedingSplit > 0')),
 ('tempconflictbtree.cpp','1904:7-1905:31','C1','transactionInfo[entry.transaction]->tooOld',U,'Always false: no entry of `readWriteBegins` belongs to a too-old transaction. '+TOOOLD.split('. resolver1 sets')[0]+'.'),
 ('tempconflictbtree.cpp','1961:7-1961:47','C1','length == 0',U,'Always false. `prefetchStoredKeySlow` is called only from `prefetchStoredKey` (2009), after it returns for length 0 (1993).'),
 ('tempconflictbtree.cpp','1961:7-1961:47','C2','!keyStore_.isValid(range)',U,'Always false. The ranges come from 2022, a separator hinted only when its CV is above the cut after `refreshSeparatorDeferred` (2051-2058); from 2068, an entry of a leaf compacted by `cleanReadLeaf` (2172); and from 2076, an entry of a left leaf compacted at 2132 whose CV is above the cut (2139). `checkReads` does not change `reclaimCV_` (tempconflictbtree.h:90), and a key block is reclaimed only when its CV is at or below `reclaimCV_` (tempconflictbtree.h:290-291), so these key bytes are valid.'),
 ('tempconflictbtree.cpp','2160:7-2160:76','C2','leaf == nullptr',U,'`bufferTaskLeaf` receives `nullptr` only from 2204, reached only for an internal node without children (2199-2200). Only the root leaf may be empty when an operation returns, and an internal node left without children is removed before (tempconflictbtree.h:18-20; 1419-1443). During `checkReads` emptied leaves are only queued (1933), separators are refreshed without removal (1276-1300), and the cleanup runs after the descent (2525). Always false.'),
 ('tempconflictbtree.cpp','2425:4-2425:4','C1','next > writeFrontier',A,assert_just('next > writeFrontier && next <= operationCount')),
 ('tempconflictbtree.cpp','2425:4-2425:4','C2','next <= operationCount',A,assert_just('next > writeFrontier && next <= operationCount')),
 ('tempconflictbtree.cpp','2444:8-2444:73','C1','tasks[slot].step == Step::Empty',U,'A task in `order` has step `Empty` only after 2205, which needs the internal node without children of 2160 C2. Lines 2445 and 2465 set `Empty` on slots that are then freed and left out of `order` (2442-2449, 2461-2466, 2471-2480). Always false.'),
]
