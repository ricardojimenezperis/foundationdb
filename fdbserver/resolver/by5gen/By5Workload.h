#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace by5 {
using Version=int64_t;
constexpr uint64_t versionsPerSecond=1000000;
inline void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Config {
 uint64_t rate=65536,batchSize=2048,shortAgeUs=1562500,warmupSeconds=60,measureSeconds=120,seed=1,conflictPpm=7300,longPpm=1000;
 uint32_t domain=20000000,span=10,prefixBytes=12;
 std::vector<uint64_t> longAgesUs{10000000,20000000,30000000};
 uint64_t batchesPerSecond()const{return rate/batchSize;}
 uint64_t warmupBatches()const{return warmupSeconds*batchesPerSecond();}
 uint64_t totalBatches()const{return (warmupSeconds+measureSeconds)*batchesPerSecond();}
 uint64_t maxAge()const{return longPpm?longAgesUs.back():shortAgeUs;}
 void validate()const{
  require(batchSize>=2 && batchSize<=65536 && rate>=batchSize && rate<=1000000000 && rate%batchSize==0,"invalid rate/batch");
  require(warmupSeconds<=1000000 && measureSeconds>0 && measureSeconds<=1000000,"invalid durations");
  require(shortAgeUs>0 && shortAgeUs<=1000000000000ULL,"invalid short age");
  require(conflictPpm<=1000000 && longPpm<=1000000 && prefixBytes<=12 && span>0,"invalid rates/layout");
  require(!longPpm || !longAgesUs.empty(),"long ages required");
  uint64_t previous=shortAgeUs;
  for(auto a:longAgesUs){require(a>previous && a<=1000000000000ULL,"long ages must increase above short age");previous=a;}
  require(warmupSeconds*versionsPerSecond>=maxAge(),"warmup must cover largest age");
  require(uint64_t(span)<=(UINT64_MAX-shortAgeUs+1)/maxAge(),"span overflow");
  uint64_t largest=(uint64_t(span)*maxAge()+shortAgeUs-1)/shortAgeUs;
  require(domain>largest && uint64_t(domain)+largest<=UINT32_MAX,"domain too small / endpoint overflow");
 }
};
struct Random {
 uint64_t state;
 uint64_t next(){uint64_t z=(state+=0x9e3779b97f4a7c15ULL);z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;z=(z^(z>>27))*0x94d049bb133111ebULL;return z^(z>>31);}
 uint64_t below(uint64_t n){require(n>0,"empty domain");uint64_t x,threshold=(-n)%n;do{x=next();}while(x<threshold);return x%n;}
};
// Big-endian numeric rank preserves order for every key length.
inline std::string key(const Config& c,uint32_t rank){
 std::string out(16+8*(rank%7),'a');
 for(unsigned j=0;j<4;++j)out[c.prefixBytes+j]=char(rank>>(24-8*j));
 return out;
}
inline Version commitVersion(const Config& c,uint64_t b){return Version((b+1)*versionsPerSecond/c.batchesPerSecond());}
struct Range{uint32_t begin,end;};
struct Transaction{Range read,write;Version readVersion;uint32_t ageClass;bool expectedConflict;};
// Latest accepted write version per interval. Used only outside timed backends.
class History {
 std::map<uint32_t,Version> segments{{0,0}};
 std::map<uint32_t,Version>::iterator split(uint32_t at){
  auto lo=std::prev(segments.upper_bound(at));
  return lo->first==at?lo:segments.emplace_hint(std::next(lo),at,lo->second);
 }
public:
 bool conflicts(Range r,Version rv)const{
  auto it=std::prev(segments.upper_bound(r.begin));
  do{if(it->second>rv)return true;++it;}while(it!=segments.end() && it->first<r.end);
  return false;
 }
 void write(Range r,Version now){
  auto end=split(r.end),begin=split(r.begin);
  segments.erase(std::next(begin),end);begin->second=now;
  if(begin!=segments.begin() && std::prev(begin)->second==now)segments.erase(begin);
  if(end!=segments.begin() && std::prev(end)->second==end->second)segments.erase(end);
 }
 size_t size()const{return segments.size();}
 template<class F> void forEach(Range r,F f)const{
  for(auto it=std::prev(segments.upper_bound(r.begin));it!=segments.end() && it->first<r.end;++it){
   auto n=std::next(it);uint32_t e=n==segments.end()?UINT32_MAX:n->first;f(std::max(it->first,r.begin),std::min(e,r.end),it->second);
  }
 }
};
class ColdIndex {
 std::map<uint32_t,uint32_t> gaps;std::set<std::pair<uint32_t,uint32_t>> byLength;
 void erase(std::map<uint32_t,uint32_t>::iterator it){byLength.erase({it->second-it->first,it->first});gaps.erase(it);}
 void insert(uint32_t a,uint32_t b){gaps.emplace(a,b);byLength.emplace(b-a,a);}
public:
 static uint64_t starts(uint32_t g0,uint32_t g1,uint32_t w,uint32_t domain){
  return g1-g0<w || g0>=domain?0:std::min<uint64_t>(g1-w,domain-1)-g0+1;
 }
 explicit ColdIndex(uint32_t end){insert(0,end);}
 size_t size()const{return gaps.size();}
 uint32_t largestGap()const{return byLength.empty()?0:byLength.rbegin()->first;}
 const std::map<uint32_t,uint32_t>& intervals()const{return gaps;}
 void hot(uint32_t a,uint32_t b){
  auto it=gaps.upper_bound(a);if(it!=gaps.begin())--it;
  while(it!=gaps.end() && it->first<b){
   if(it->second<=a){++it;continue;}
   uint32_t g0=it->first,g1=it->second;auto next=std::next(it);erase(it);
   if(g0<a)insert(g0,a);
   if(g1>b)insert(b,g1);
   it=next;
  }
 }
 void cold(uint32_t a,uint32_t b){
  auto it=gaps.upper_bound(a);if(it!=gaps.begin() && std::prev(it)->second>=a)--it;
  while(it!=gaps.end() && it->first<=b){a=std::min(a,it->first);b=std::max(b,it->second);auto next=std::next(it);erase(it);it=next;}
  insert(a,b);
 }
 uint64_t freeStarts(uint32_t w,uint32_t domain)const{
  uint64_t total=0;for(auto it=byLength.lower_bound({w,0});it!=byLength.end();++it)total+=starts(it->second,it->second+it->first,w,domain);return total;
 }
 template<class R> bool sampleFree(uint32_t w,uint32_t domain,R& rng,uint32_t& start)const{
  uint64_t total=freeStarts(w,domain);if(!total)return false;uint64_t k=rng.below(total);
  for(auto it=byLength.lower_bound({w,0});it!=byLength.end();++it){
   uint64_t n=starts(it->second,it->second+it->first,w,domain);if(k<n){start=uint32_t(it->second+k);return true;}k-=n;
  }
  throw std::logic_error("free sampling overrun");
 }
 template<class R> bool sampleConflict(uint32_t w,uint32_t domain,R& rng,uint32_t& start)const{
  uint64_t free=0;for(const auto&[g0,g1]:gaps)free+=starts(g0,g1,w,domain);
  uint64_t total=domain-free;if(!total)return false;uint64_t k=rng.below(total),cursor=0;
  for(const auto&[g0,g1]:gaps){
   uint64_t n=starts(g0,g1,w,domain);if(!n)continue;
   if(g0>cursor){uint64_t len=g0-cursor;if(k<len){start=uint32_t(cursor+k);return true;}k-=len;}
   cursor=g0+n;
  }
  require(cursor+k<domain,"conflict sampling overrun");start=uint32_t(cursor+k);return true;
 }
};
inline uint32_t width(const Config& c,Random& rng,uint64_t age){
 uint64_t n=(1+rng.below(c.span))*age,result=n/c.shortAgeUs;
 if(rng.below(c.shortAgeUs)<n%c.shortAgeUs)++result;
 require(result>0 && result<=UINT32_MAX,"width overflow");return uint32_t(result);
}
class Generator {
 Config cfg;Random rng;History oracle;uint64_t longOrdinal=0,position=0;
 std::vector<uint64_t> classCount,pending,cursor;std::vector<ColdIndex> cold;
 std::deque<std::pair<Version,Range>> writes;uint64_t writesBase=0;
 uint64_t ageOf(uint32_t cls)const{return cls?cfg.longAgesUs[cls-1]:cfg.shortAgeUs;}
 void expire(uint32_t cls,Version rv){
  while(cursor[cls]<writesBase+writes.size() && writes[cursor[cls]-writesBase].first<=rv){
   const auto&[v,r]=writes[cursor[cls]-writesBase];
   oracle.forEach(r,[&](uint32_t b,uint32_t e,Version seen){if(seen==v)cold[cls].cold(b,e);});
   ++cursor[cls];
  }
 }
public:
 static constexpr unsigned rejectionTries=256;
 uint64_t candidateTrials=0,exactSamples=0,emptyConflictSets=0,deferredTargets=0,maxPending=0;
 explicit Generator(Config c):cfg(c),rng{c.seed},classCount(c.longAgesUs.size()+1,0),pending(classCount.size(),0),cursor(classCount.size(),0){
  cfg.validate();uint64_t largest=(uint64_t(cfg.span)*cfg.maxAge()+cfg.shortAgeUs-1)/cfg.shortAgeUs;
  cold.assign(classCount.size(),ColdIndex(uint32_t(cfg.domain+largest)));
 }
 size_t historySegments()const{return oracle.size();}
 uint64_t pendingTargets()const{uint64_t n=0;for(auto p:pending)n+=p;return n;}
 const ColdIndex& coldIndex(uint32_t cls)const{return cold[cls];}
 std::vector<Transaction> batch(uint64_t b){
  require(position==b*cfg.batchSize,"nonconsecutive generation");
  uint64_t nlong=((position+cfg.batchSize)*cfg.longPpm)/1000000-(position*cfg.longPpm)/1000000;
  std::vector<uint32_t> classes(cfg.batchSize,0);
  for(uint64_t i=0;i<nlong;++i)classes[i]=1+(longOrdinal++%cfg.longAgesUs.size());
  for(size_t i=classes.size();i>1;--i)std::swap(classes[i-1],classes[rng.below(i)]);
  std::vector<Transaction> out;out.reserve(cfg.batchSize);Version now=commitVersion(cfg,b);
  for(uint32_t cls=0;cls<classCount.size();++cls)expire(cls,std::max<Version>(0,now-Version(ageOf(cls))));
  uint64_t done=*std::min_element(cursor.begin(),cursor.end());
  while(writesBase<done){writes.pop_front();++writesBase;}
  for(auto cls:classes){
   uint64_t age=ageOf(cls);
   Version rv=std::max<Version>(0,now-Version(age));uint64_t count=classCount[cls]++;
   bool scheduled=((count+1)*cfg.conflictPpm)/1000000!=(count*cfg.conflictPpm)/1000000;
   bool fromPending=!scheduled && pending[cls]>0,target=scheduled || fromPending;
   uint32_t rw=width(cfg,rng,age),ww=width(cfg,rng,age);Range read{};bool found=false;
   const uint64_t budget=target?std::max<uint64_t>(rejectionTries,cold[cls].size()/4):rejectionTries;
   for(uint64_t tries=0;tries<budget;++tries){
    uint32_t start=uint32_t(rng.below(cfg.domain));read={start,start+rw};++candidateTrials;
    if(oracle.conflicts(read,rv)==target){found=true;break;}
   }
   if(!found){
    ++exactSamples;uint32_t start=0;
    bool ok=target?cold[cls].sampleConflict(rw,cfg.domain,rng,start):cold[cls].sampleFree(rw,cfg.domain,rng,start);
    if(!ok){
     if(!target)throw std::runtime_error("every admissible read of this class and width intersects a write after its RV: batch="+std::to_string(b)+" ageUs="+std::to_string(age)+" width="+std::to_string(rw)+" largestGap="+std::to_string(cold[cls].largestGap())+" gaps="+std::to_string(cold[cls].size()));
     if(b>=cfg.warmupBatches())throw std::runtime_error("no admissible read of this class and width intersects a write after its RV during measurement: batch="+std::to_string(b)+" ageUs="+std::to_string(age)+" width="+std::to_string(rw)+" coldIntervals="+std::to_string(cold[cls].size()));
     ++emptyConflictSets;if(scheduled){++pending[cls];++deferredTargets;maxPending=std::max(maxPending,pendingTargets());}
     target=false;fromPending=false;
     require(cold[cls].sampleFree(rw,cfg.domain,rng,start),"no read start exists");
    }
    read={start,start+rw};
    require(oracle.conflicts(read,rv)==target,"exact sampler disagrees with oracle");
   }
   if(fromPending && target)--pending[cls];
   uint32_t start=uint32_t(rng.below(cfg.domain));Range write{start,start+ww};
   out.push_back({read,write,rv,cls,target});
   if(!target){oracle.write(write,now);for(auto&c:cold)c.hot(write.begin,write.end);writes.push_back({now,write});}
   ++position;
  }
  return out;
 }
};
inline void hashWord(uint64_t& h,uint64_t v){for(unsigned i=0;i<8;++i){h^=uint8_t(v>>(8*i));h*=1099511628211ULL;}}
inline void put(std::ostream& s,uint64_t n,unsigned bytes=8){for(unsigned i=0;i<bytes;++i)s.put(char(n>>(8*i)));require(bool(s),"trace write failed");}
inline uint64_t get(std::istream& s,unsigned bytes=8){uint64_t n=0;for(unsigned i=0;i<bytes;++i){int c=s.get();require(c!=EOF,"truncated trace");n|=uint64_t(uint8_t(c))<<(8*i);}return n;}
inline void writeHeader(std::ostream& s,const Config& c){
 s.write("BY5RNG02",8);
 for(auto v:{c.rate,c.batchSize,c.shortAgeUs,c.warmupSeconds,c.measureSeconds,c.seed,c.conflictPpm,c.longPpm,uint64_t(c.domain),uint64_t(c.span),uint64_t(c.prefixBytes),uint64_t(c.longAgesUs.size())})put(s,v);
 for(auto a:c.longAgesUs)put(s,a);
}
inline Config readHeader(std::istream& s){
 char magic[8]{};s.read(magic,8);require(std::string(magic,8)=="BY5RNG02","wrong trace format");Config c;
 c.rate=get(s);c.batchSize=get(s);c.shortAgeUs=get(s);c.warmupSeconds=get(s);c.measureSeconds=get(s);c.seed=get(s);c.conflictPpm=get(s);c.longPpm=get(s);
 auto d=get(s),w=get(s),p=get(s),n=get(s);require(d<=UINT32_MAX && w<=UINT32_MAX && p<=12 && n<=128,"invalid header");
 c.domain=uint32_t(d);c.span=uint32_t(w);c.prefixBytes=uint32_t(p);c.longAgesUs.clear();for(uint64_t i=0;i<n;++i)c.longAgesUs.push_back(get(s));c.validate();return c;
}
inline void writeTransaction(std::ostream& s,const Transaction& t){
 put(s,t.read.begin,4);put(s,t.read.end,4);put(s,t.write.begin,4);put(s,t.write.end,4);put(s,uint64_t(t.readVersion));put(s,t.ageClass,4);put(s,t.expectedConflict,4);
}
inline Transaction readTransaction(std::istream& s,const Config& c,Version now){
 Transaction t;t.read={uint32_t(get(s,4)),uint32_t(get(s,4))};t.write={uint32_t(get(s,4)),uint32_t(get(s,4))};t.readVersion=Version(get(s));t.ageClass=uint32_t(get(s,4));auto target=get(s,4);require(target<=1,"invalid target");t.expectedConflict=target;
 require(t.ageClass<=c.longAgesUs.size() && t.read.begin<c.domain && t.write.begin<c.domain && t.read.end>t.read.begin && t.write.end>t.write.begin,"invalid range/class");
 uint64_t age=t.ageClass?c.longAgesUs[t.ageClass-1]:c.shortAgeUs;
 require(t.readVersion==std::max<Version>(0,now-Version(age)),"invalid RV");return t;
}
} // namespace by5
