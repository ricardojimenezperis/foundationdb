#!/usr/bin/env python3
# Native timing build patch for by5resolvertest, applied to a copy of fdbserver/resolver (see
# build.sh): the per-batch logs go to buffers in memory, of BY5_LOG_BUFFER_BYTES each (256 MiB by
# default), and are written when the run ends, so the timed run writes nothing to disk.
# usage: by5-membuf.py DIR
import sys
D = sys.argv[1].rstrip('/') + '/'
p = D + 'By5ResolverTest.inc'; s = open(p).read()
pairs = [
('''std::ofstream log(logPath),ages(std::string(logPath)+".ages.csv");by5::require(bool(log)&&bool(ages),"cannot open logs");''',
 '''const size_t by5LogCap=size_t(by5Env("BY5_LOG_BUFFER_BYTES",268435456));static std::vector<char> by5LogBuf(by5LogCap),by5AgesBuf(by5LogCap);std::ofstream log,ages;log.rdbuf()->pubsetbuf(by5LogBuf.data(),std::streamsize(by5LogCap));ages.rdbuf()->pubsetbuf(by5AgesBuf.data(),std::streamsize(by5LogCap));log.open(logPath);ages.open(std::string(logPath)+".ages.csv");by5::require(bool(log)&&bool(ages),"cannot open logs");'''),
(''' printf("by5lengths");''',
 ''' {const long long lb=(long long)log.tellp(),ab=(long long)ages.tellp();printf("by5logbuffer batches_bytes=%lld ages_bytes=%lld capacity=%zu\\n",lb,ab,by5LogCap);by5::require(lb>=0&&ab>=0&&size_t(lb)<by5LogCap&&size_t(ab)<by5LogCap,"log buffer too small");}
 printf("by5lengths");'''),
]
for a, b in pairs:
    assert s.count(a) == 1, (a[:60], s.count(a)); s = s.replace(a, b)
open(p, 'w').write(s)
