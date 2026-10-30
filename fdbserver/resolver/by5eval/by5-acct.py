#!/usr/bin/env python3
# Accounting build patch for by5resolvertest, applied to a copy of fdbserver/resolver (see build.sh).
# It writes BATCH_LOG.acct.csv, one line per batch, and a "by5acct" summary line:
# - FDB's skip list: history_bytes is the bytes of its live nodes, counted at each allocation and
#   release (64 and 128-byte FastAllocator nodes, malloc_usable_size of larger ones); every 100
#   batches a walk of the list checks the count (walk_checks, walk_mismatches).
# - btree-v2: history_bytes is its pool, PAGED_RESOLVER_POOL_PAGES x 65,536 bytes, allocated and
#   touched whole when the conflict set is built; btree-v2 keeps nothing outside it.
# It also samples, every 100 batches, VmRSS, HugetlbPages, jemalloc's statistics and the
# FastAllocator totals of 64 and 128-byte blocks. Accounting runs are not timed.
# usage: by5-acct.py DIR     (DIR holds the copies of ConflictSet.cpp and By5ResolverTest.inc)
import sys

D = sys.argv[1].rstrip('/') + '/'


def patch(name, pairs):
    p = D + name
    s = open(p).read()
    for a, b in pairs:
        assert s.count(a) == 1, (name, a[:80], s.count(a))
        s = s.replace(a, b)
    open(p, 'w').write(s)
    return s


cs = open(D + 'ConflictSet.cpp').read()
capacity_pool = ('(int64_t(SERVER_KNOBS->PAGED_RESOLVER_POOL_PAGES)*65536)'
                 if 'btree_v2::TempConflictBTree btreeV2;' in cs else 'int64_t(0)')

patch('ConflictSet.cpp', [
    ('#include "flow/UnitTest.h"\n', '''#include "flow/UnitTest.h"
#include <malloc.h>
static bool g_ageCount = false;
static int64_t g_ageSkipBytes = 0, g_ageSkipCreated = 0;
static int64_t g_by5Skip64 = 0, g_by5Skip128 = 0, g_by5SkipLarge = 0;
'''),
    ('''				n = (Node*)FastAllocator<64>::allocate();
				INSTRUMENT_ALLOCATE("SkipListNode64");
''', '''				n = (Node*)FastAllocator<64>::allocate();
				INSTRUMENT_ALLOCATE("SkipListNode64");
				if (g_ageCount) { g_ageSkipBytes += 64; g_ageSkipCreated += 64; g_by5Skip64 += 64; }
'''),
    ('''				n = (Node*)FastAllocator<128>::allocate();
				INSTRUMENT_ALLOCATE("SkipListNode128");
''', '''				n = (Node*)FastAllocator<128>::allocate();
				INSTRUMENT_ALLOCATE("SkipListNode128");
				if (g_ageCount) { g_ageSkipBytes += 128; g_ageSkipCreated += 128; g_by5Skip128 += 128; }
'''),
    ('''				n = (Node*)new char[nodeSize];
				INSTRUMENT_ALLOCATE("SkipListNodeLarge");
''', '''				n = (Node*)new char[nodeSize];
				INSTRUMENT_ALLOCATE("SkipListNodeLarge");
				if (g_ageCount) { const int64_t b = int64_t(malloc_usable_size(n)); g_ageSkipBytes += b; g_ageSkipCreated += b; g_by5SkipLarge += b; }
'''),
    ('''				FastAllocator<64>::release(this);
				INSTRUMENT_RELEASE("SkipListNode64");
''', '''				FastAllocator<64>::release(this);
				INSTRUMENT_RELEASE("SkipListNode64");
				if (g_ageCount) { g_ageSkipBytes -= 64; g_by5Skip64 -= 64; }
'''),
    ('''				FastAllocator<128>::release(this);
				INSTRUMENT_RELEASE("SkipListNode128");
''', '''				FastAllocator<128>::release(this);
				INSTRUMENT_RELEASE("SkipListNode128");
				if (g_ageCount) { g_ageSkipBytes -= 128; g_by5Skip128 -= 128; }
'''),
    ('''				delete[] (char*)this;
				INSTRUMENT_RELEASE("SkipListNodeLarge");
''', '''				if (g_ageCount) { const int64_t b = int64_t(malloc_usable_size(this)); g_ageSkipBytes -= b; g_by5SkipLarge -= b; }
				delete[] (char*)this;
				INSTRUMENT_RELEASE("SkipListNodeLarge");
'''),
    ('''	int count() const {
''', '''	int64_t ageAccountedBytes() const {
		int64_t bytes = 0;
		for (Node* x = header->getNext(0); x; x = x->getNext(0)) {
			const int nodeSize = int(sizeof(Node)) + x->length() + (x->level() + 1) * int(sizeof(Node*) + sizeof(Version));
			bytes += nodeSize <= 64 ? 64 : nodeSize <= 128 ? 128 : int64_t(malloc_usable_size(x));
		}
		return bytes;
	}

	int count() const {
'''),
])

p = D + 'By5ResolverTest.inc'
s = open(p).read()
s = ('extern "C" int mallctl(const char*, void*, size_t*, void*, size_t);\n'
     'struct By5T64 {};\nstruct By5T128 {};\n'
     'template <class Tag, auto F> struct By5Steal { friend auto by5Td(Tag) { return F; } };\n'
     'auto by5Td(By5T64);\nauto by5Td(By5T128);\n'
     'template struct By5Steal<By5T64, &FastAllocator<64>::threadData>;\n'
     'template struct By5Steal<By5T128, &FastAllocator<128>::threadData>;\n'
     'template <class Tag> int64_t by5ThreadFree(int64_t size) { auto& t = by5Td(Tag{})(); return int64_t(t.count) * size + (t.alternate ? int64_t(kFastAllocMagazineBytes) : 0); }\n'
     '#define BY5_CAPACITY_POOL_BYTES ' + capacity_pool + '\n') + s


def rep(a, b):
    global s
    assert s.count(a) == 1, (a[:80], s.count(a))
    s = s.replace(a, b)


rep(''' by5::History model;
 printf("by5keyprofile profile=%u\\n",cfg.keyProfile);
''', ''' by5::History model;
 printf("by5keyprofile profile=%u\\n",cfg.keyProfile);
 g_ageCount=true;std::ofstream acct(std::string(logPath)+".acct.csv");by5::require(bool(acct),"cannot open acct");
 acct<<"batch,measured,live_skip_bytes,walked_skip_bytes,pool_physical_bytes,pool_used_bytes,age_bytes,partition_bytes,page_table_bytes,metadata_bytes,read_scratch_bytes,write_scratch_bytes,history_bytes,history_plus_scratch_bytes,batch_arena_bytes,vm_rss_kib,hugetlb_kib,je_allocated_bytes,je_active_bytes,je_resident_bytes,je_mapped_bytes,je_retained_bytes,fast64_total_bytes,fast64_unused_bytes,fast128_total_bytes,fast128_unused_bytes,skip64_bytes,skip128_bytes,skip_large_bytes,fast64_thread_free_bytes,fast128_thread_free_bytes,fast64_active_threads,fast128_active_threads,fast64_other_threads_bound_bytes,fast64_use_high_bytes,fast64_use_low_bytes,fast64_other_high_bytes,fast64_other_low_bytes,fast128_other_threads_bound_bytes,fast128_use_high_bytes,fast128_use_low_bytes,fast128_other_high_bytes,fast128_other_low_bytes\\n";
 int64_t acctMaxLive=0,acctMaxAux=0,acctMaxScratch=0,acctMaxWriteScratch=0,acctMaxHistory=0,acctMaxMeasuredHistory=0,acctMaxHistoryScratch=0,acctPool=0,acctMaxRss=0,acctMaxHuge=0,acctMaxArena=0;uint64_t acctChecks=0,acctMismatches=0;
''')
rep('  double elapsed=timer()-start;auto after=getConflictSetStats(cs.get());\n',
    '''  double elapsed=timer()-start;auto after=getConflictSetStats(cs.get());
  {int64_t live=g_ageSkipBytes,walked=-1,pool=0,used=0,age=0,part=0,table=0,aux=0,scratch=0,wscratch=0,rssKib=-1,hugeKib=-1,jAlloc=-1,jActive=-1,jRes=-1,jMap=-1,jRet=-1,f64T=-1,f64U=-1,f128T=-1,f128U=-1,f64TF=-1,f128TF=-1,f64A=-1,f128A=-1;
   bool sample=b%100==99 || b+1==cfg.totalBatches();
   if(sample){walked=cs->versionHistory.ageAccountedBytes();++acctChecks;acctMismatches+=walked!=live;}
   if(capacity){pool=BY5_CAPACITY_POOL_BYTES;used=pool;}
   if(sample){
#ifdef __linux__
    std::ifstream st("/proc/self/status");std::string line;
    while(std::getline(st,line)){if(!line.compare(0,6,"VmRSS:"))rssKib=std::stoll(line.substr(6));else if(!line.compare(0,13,"HugetlbPages:"))hugeKib=std::stoll(line.substr(13));}
    {uint64_t epoch=1;size_t esz=sizeof(epoch);mallctl("epoch",&epoch,&esz,&epoch,esz);auto je=[](const char*n){size_t v=0,sz=sizeof(v);return mallctl(n,&v,&sz,nullptr,0)==0?int64_t(v):int64_t(-1);};
     jAlloc=je("stats.allocated");jActive=je("stats.active");jRes=je("stats.resident");jMap=je("stats.mapped");jRet=je("stats.retained");}
#endif
    f64T=FastAllocator<64>::getTotalMemory();f64U=FastAllocator<64>::getApproximateMemoryUnused();f128T=FastAllocator<128>::getTotalMemory();f128U=FastAllocator<128>::getApproximateMemoryUnused();
    f64TF=by5ThreadFree<By5T64>(64);f128TF=by5ThreadFree<By5T128>(128);f64A=FastAllocator<64>::getActiveThreads();f128A=FastAllocator<128>::getActiveThreads();
    acctMaxRss=std::max(acctMaxRss,rssKib);acctMaxHuge=std::max(acctMaxHuge,hugeKib);}
   int64_t history=capacity?pool+aux:live,historyScratch=history+scratch+wscratch,arenaBytes=int64_t(arena.getSize());acctPool=pool;
   acctMaxLive=std::max(acctMaxLive,live);acctMaxAux=std::max(acctMaxAux,aux);acctMaxScratch=std::max(acctMaxScratch,scratch);acctMaxWriteScratch=std::max(acctMaxWriteScratch,wscratch);
   acctMaxHistory=std::max(acctMaxHistory,history);acctMaxHistoryScratch=std::max(acctMaxHistoryScratch,historyScratch);acctMaxArena=std::max(acctMaxArena,arenaBytes);
   if(measured)acctMaxMeasuredHistory=std::max(acctMaxMeasuredHistory,history);
   acct<<b<<','<<measured<<','<<live<<','<<walked<<','<<pool<<','<<used<<','<<age<<','<<part<<','<<table<<','<<aux<<','<<scratch<<','<<wscratch<<','<<history<<','<<historyScratch<<','<<arenaBytes<<','<<rssKib<<','<<hugeKib<<','<<jAlloc<<','<<jActive<<','<<jRes<<','<<jMap<<','<<jRet<<','<<f64T<<','<<f64U<<','<<f128T<<','<<f128U<<','<<g_by5Skip64<<','<<g_by5Skip128<<','<<g_by5SkipLarge<<','<<f64TF<<','<<f128TF<<','<<f64A<<','<<f128A;
   for(int c=0;c<2;++c){int64_t T=c?f128T:f64T,G=c?f128U:f64U,R=c?f128TF:f64TF,A=c?f128A:f64A,L=c?g_by5Skip128:g_by5Skip64;if(T<0){acct<<",-1,-1,-1,-1,-1";continue;}int64_t Bd=std::max<int64_t>(0,A-1)*2*int64_t(kFastAllocMagazineBytes),U=T-G-R,lo=std::max<int64_t>(0,U-Bd);acct<<','<<Bd<<','<<U<<','<<lo<<','<<std::max<int64_t>(0,U-L)<<','<<std::max<int64_t>(0,lo-L);}
   acct<<'\\n';}
''')
rep(' printf("by5lengths");',
    ''' printf("by5acct backend=%s maxLiveSkipBytes=%lld poolPhysicalBytes=%lld maxMetadataBytes=%lld maxReadScratchBytes=%lld maxWriteScratchBytes=%lld maxHistoryBytes=%lld maxMeasuredHistoryBytes=%lld maxHistoryPlusScratchBytes=%lld maxBatchArenaBytes=%lld maxVmRssKiB=%lld maxHugetlbKiB=%lld walkChecks=%llu walkMismatches=%llu\\n",conflictSetIndexName(cs.get()),(long long)acctMaxLive,(long long)acctPool,(long long)acctMaxAux,(long long)acctMaxScratch,(long long)acctMaxWriteScratch,(long long)acctMaxHistory,(long long)acctMaxMeasuredHistory,(long long)acctMaxHistoryScratch,(long long)acctMaxArena,(long long)acctMaxRss,(long long)acctMaxHuge,(unsigned long long)acctChecks,(unsigned long long)acctMismatches);
 printf("by5lengths");''')
open(p, 'w').write(s)
print('by5-acct applied (%s)' % ('btree-v2' if capacity_pool != 'int64_t(0)' else 'skip list'))
