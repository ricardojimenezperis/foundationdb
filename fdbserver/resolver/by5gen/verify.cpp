#include "By5Workload.h"
#include <iostream>
int main(int argc,char** argv){try{
 by5::require(argc==2,"usage: by5-verify TRACE");std::ifstream in(argv[1],std::ios::binary);by5::require(bool(in),"cannot open trace");
 auto c=by5::readHeader(in);by5::History h;size_t n=c.longAgesUs.size()+1;
 std::vector<uint64_t> offered(n),conflicts(n),allOffered(n),allConflicts(n),mismatches(n);
 for(uint64_t b=0;b<c.totalBatches();++b){auto now=by5::commitVersion(c,b);bool measured=b>=c.warmupBatches();
  for(uint64_t i=0;i<c.batchSize;++i){auto t=by5::readTransaction(in,c,now);bool conflict=h.conflicts(t.read,t.readVersion);
   mismatches[t.ageClass]+=conflict!=t.expectedConflict;++allOffered[t.ageClass];allConflicts[t.ageClass]+=conflict;
   if(measured){++offered[t.ageClass];conflicts[t.ageClass]+=conflict;}
   if(!conflict)h.write(t.write,now);
  }
 }
 by5::require(in.peek()==EOF,"extra trace data");uint64_t bad=0;
 for(size_t i=0;i<n;++i){bad+=mismatches[i];std::cout<<"ageUs="<<(i?c.longAgesUs[i-1]:c.shortAgeUs)<<" measuredOffered="<<offered[i]<<" measuredConflicts="<<conflicts[i]<<" measuredPpm="<<(offered[i]?conflicts[i]*1e6/offered[i]:0)<<" allOffered="<<allOffered[i]<<" allConflicts="<<allConflicts[i]<<" allPpm="<<(allOffered[i]?allConflicts[i]*1e6/allOffered[i]:0)<<" mismatches="<<mismatches[i]<<"\n";}
 std::cout<<(bad?"FAIL":"PASS")<<" domain="<<c.domain<<" conflictPpm="<<c.conflictPpm<<" mismatches="<<bad<<"\n";return bad?2:0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
