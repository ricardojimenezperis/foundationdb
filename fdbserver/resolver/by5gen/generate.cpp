#include "By5Workload.h"
#include <iostream>
#include <filesystem>
#include <sstream>
uint64_t number(const std::string& s){by5::require(!s.empty() && s[0]!='-',"invalid unsigned value");size_t used=0;auto n=std::stoull(s,&used);by5::require(used==s.size(),"invalid number");return n;}
int main(int argc,char** argv){try{
 by5::Config c;std::string path;
 for(int i=1;i<argc;i+=2){by5::require(i+1<argc,"option requires value");std::string a=argv[i],v=argv[i+1];
  if(a=="--out")path=v;
  else if(a=="--long-ages-us"){c.longAgesUs.clear();std::stringstream ss(v);std::string t;while(std::getline(ss,t,','))c.longAgesUs.push_back(number(t));}
  else {auto n=number(v);
   if(a=="--rate")c.rate=n;else if(a=="--batch")c.batchSize=n;else if(a=="--short-age-us")c.shortAgeUs=n;
   else if(a=="--warmup")c.warmupSeconds=n;else if(a=="--measure")c.measureSeconds=n;
   else if(a=="--long-ppm")c.longPpm=n;else if(a=="--conflict-ppm")c.conflictPpm=n;else if(a=="--seed")c.seed=n;
   else if(a=="--domain"){by5::require(n<=UINT32_MAX,"domain overflow");c.domain=uint32_t(n);}
   else if(a=="--span"){by5::require(n<=UINT32_MAX,"span overflow");c.span=uint32_t(n);}
   else if(a=="--prefix"){by5::require(n<=12,"prefix overflow");c.prefixBytes=uint32_t(n);}
   else throw std::runtime_error("unknown option "+a);
  }
 }
 c.validate();by5::require(!path.empty(),"--out required");
 by5::require(!std::filesystem::exists(path) && !std::filesystem::exists(path+".partial"),"output exists");
 uint64_t bytes=104+c.longAgesUs.size()*8+c.totalBatches()*c.batchSize*32;
 auto parent=std::filesystem::path(path).parent_path();if(parent.empty())parent=".";
 by5::require(std::filesystem::space(parent).available>bytes+64*1024*1024,"insufficient disk space for trace");
 std::ofstream out(path+".partial",std::ios::binary);by5::writeHeader(out,c);by5::Generator gen(c);
 uint64_t hash=14695981039346656037ULL;
 std::vector<uint64_t> counts(c.longAgesUs.size()+1),targets(counts.size()),readWidth(counts.size()),writeWidth(counts.size());
 for(uint64_t b=0;b<c.totalBatches();++b){
  for(const auto&t:gen.batch(b)){
   by5::writeTransaction(out,t);
   if(b>=c.warmupBatches()){++counts[t.ageClass];targets[t.ageClass]+=t.expectedConflict;readWidth[t.ageClass]+=t.read.end-t.read.begin;writeWidth[t.ageClass]+=t.write.end-t.write.begin;}
   for(auto v:{uint64_t(t.read.begin),uint64_t(t.read.end),uint64_t(t.write.begin),uint64_t(t.write.end),uint64_t(t.readVersion),uint64_t(t.ageClass),uint64_t(t.expectedConflict)})by5::hashWord(hash,v);
  }
  if(b && b%1024==0)std::cerr<<"batch="<<b<<" oracleSegments="<<gen.historySegments()<<"\n";
 }
 out.close();by5::require(bool(out),"trace close failed");std::filesystem::rename(path+".partial",path);
 std::cout<<"trace="<<path<<" bytes="<<bytes<<" hash="<<std::hex<<hash<<std::dec<<" trials="<<gen.candidateTrials<<" exactSamples="<<gen.exactSamples<<" emptyConflictSets="<<gen.emptyConflictSets<<" deferredTargets="<<gen.deferredTargets<<" maxPending="<<gen.maxPending<<" pendingAtEnd="<<gen.pendingTargets()<<"\n";
 for(size_t i=0;i<counts.size();++i)std::cout<<"ageUs="<<(i?c.longAgesUs[i-1]:c.shortAgeUs)<<" offered="<<counts[i]<<" target="<<targets[i]<<" targetPpm="<<(counts[i]?targets[i]*1000000.0/counts[i]:0)<<" readWidth="<<readWidth[i]<<" writeWidth="<<writeWidth[i]<<"\n";
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
