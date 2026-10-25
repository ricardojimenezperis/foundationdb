#include "../By5Workload.h"
#include <cassert>
#include <cmath>
#include <iostream>
static bool hit(const std::vector<by5::Version>& v,uint32_t s,uint32_t w,by5::Version rv){for(uint32_t i=s;i<s+w;++i)if(v[i]>rv)return true;return false;}
int main(){
 const uint32_t domain=400,end=460;by5::Random r{9};
 for(int round=0;round<200;++round){
  by5::ColdIndex idx(end);std::vector<bool> hot(end,false);
  for(int op=0;op<60;++op){uint32_t a=r.below(end),b=a+1+r.below(std::min<uint32_t>(40,end-a));
   if(r.below(3)){idx.hot(a,b);for(uint32_t i=a;i<b;++i)hot[i]=true;}else{idx.cold(a,b);for(uint32_t i=a;i<b;++i)hot[i]=false;}
   uint32_t prev=0;bool first=true;
   for(const auto&[g0,g1]:idx.intervals()){assert(g0<g1);if(!first)assert(g0>prev);prev=g1;first=false;for(uint32_t i=g0;i<g1;++i)assert(!hot[i]);if(g0)assert(hot[g0-1]);if(g1<end)assert(hot[g1]);}
   uint64_t cold=0;for(uint32_t i=0;i<end;++i)cold+=!hot[i];uint64_t sum=0;for(const auto&[g0,g1]:idx.intervals())sum+=g1-g0;assert(sum==cold);
   for(uint32_t w:{1u,3u,17u,60u}){uint64_t brute=0;for(uint32_t s=0;s<domain;++s){bool any=false;for(uint32_t i=s;i<s+w;++i)any|=hot[i];brute+=!any;}assert(idx.freeStarts(w,domain)==brute);}
  }
 }
 uint64_t uniformCases=0;
 for(int round=0;round<6;++round){
  by5::ColdIndex idx(end);std::vector<bool> hot(end,false);
  for(int op=0;op<25+round*10;++op){uint32_t a=r.below(end),b=a+1+r.below(std::min<uint32_t>(12,end-a));idx.hot(a,b);for(uint32_t i=a;i<b;++i)hot[i]=true;}
  for(uint32_t w:{1u,5u,9u}){
   std::vector<int> eligibleFree,eligibleConflict;
   for(uint32_t s=0;s<domain;++s){bool any=false;for(uint32_t i=s;i<s+w;++i)any|=hot[i];(any?eligibleConflict:eligibleFree).push_back(int(s));}
   for(int want=0;want<2;++want){
    auto& eligible=want?eligibleConflict:eligibleFree;std::vector<uint64_t> seen(domain,0);const uint64_t draws=200000;uint32_t s=0;
    for(uint64_t d=0;d<draws;++d){bool ok=want?idx.sampleConflict(w,domain,r,s):idx.sampleFree(w,domain,r,s);assert(ok==!eligible.empty());if(!ok)break;++seen[s];}
    if(eligible.empty())continue;
    std::vector<bool> allowed(domain,false);for(int e:eligible)allowed[e]=true;
    double expected=double(draws)/eligible.size(),chi=0;for(uint32_t i=0;i<domain;++i){if(!allowed[i]){assert(!seen[i]);continue;}chi+=(seen[i]-expected)*(seen[i]-expected)/expected;}
    double dof=eligible.size()-1;assert(chi<dof+6*std::sqrt(2*dof)+10);++uniformCases;
   }
  }
 }
 by5::ColdIndex full(end);full.hot(0,end);uint32_t s=0;assert(!full.sampleFree(1,domain,r,s));assert(full.sampleConflict(1,domain,r,s) && s<domain);
 by5::ColdIndex empty(end);assert(!empty.sampleConflict(1,domain,r,s));
 uint64_t checked=0;
 struct Case{const char* name;uint64_t rate,batch;uint32_t domain;uint64_t warmup,measure,longPpm,conflictPpm;std::vector<uint64_t> ages;};
 for(const Case& k:{Case{"sparse",256,16,4000000,8,8,100000,100000,{2000000,4000000,6000000}},Case{"dense",4096,128,150000,8,8,100000,7300,{2000000,4000000,6000000}},Case{"dense-long",4096,128,4000000,32,8,100000,7300,{10000000,20000000,30000000}}}){
  by5::Config c;c.rate=k.rate;c.batchSize=k.batch;c.domain=k.domain;c.warmupSeconds=k.warmup;c.measureSeconds=k.measure;c.longPpm=k.longPpm;c.conflictPpm=k.conflictPpm;c.longAgesUs=k.ages;c.validate();
  by5::Generator g(c);std::vector<by5::Version> oracle(c.domain+2000);std::vector<uint64_t> counts(4),targets(4);
  for(uint64_t b=0;b<c.totalBatches();++b){auto now=by5::commitVersion(c,b);
   for(const auto&t:g.batch(b)){bool conflict=hit(oracle,t.read.begin,t.read.end-t.read.begin,t.readVersion);assert(conflict==t.expectedConflict);
    if(!conflict)std::fill(oracle.begin()+t.write.begin,oracle.begin()+t.write.end,now);
    ++counts[t.ageClass];targets[t.ageClass]+=conflict;++checked;}
  }
  for(unsigned i=0;i<4;++i){uint64_t want=counts[i]*c.conflictPpm/1000000;assert(targets[i]+g.pendingTargets()>=want && targets[i]<=want);}
  std::cout<<"PASS "<<k.name<<" transactions="<<c.totalBatches()*c.batchSize<<" exactSamples="<<g.exactSamples<<" emptyConflictSets="<<g.emptyConflictSets<<" deferred="<<g.deferredTargets<<" pendingAtEnd="<<g.pendingTargets();
  for(unsigned i=0;i<4;++i)std::cout<<" class"<<i<<"="<<targets[i]<<"/"<<counts[i];
  std::cout<<std::endl;
 }
 {by5::Config c;c.rate=256;c.batchSize=16;c.domain=100000;c.warmupSeconds=2;c.measureSeconds=2;c.longPpm=0;c.conflictPpm=1000000;c.validate();
  by5::Generator g(c);std::string why;uint64_t failed=0;
  try{for(uint64_t b=0;b<c.totalBatches();++b){failed=b;g.batch(b);}}catch(const std::runtime_error& e){why=e.what();}
  assert(why.rfind("no admissible read of this class and width intersects a write after its RV during measurement: batch=50 ",0)==0 && failed==50 && g.emptyConflictSets==2 && g.deferredTargets==2);
  std::cout<<"PASS startup: conflicting targets with no eligible start deferred in warmup (batches 0 and 25), explicit error in measurement (batch 50)"<<std::endl;}
 std::cout<<"PASS cold index vs brute force: 200 rounds; uniformity: "<<uniformCases<<" cases; generator vs dense oracle: "<<checked<<" transactions\n";
}
