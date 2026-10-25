# by5gen

`by5-generate` writes the deterministic traces that `fdbserver -r by5resolvertest` replays, and
`by5-verify` replays a trace against an exact model of the history and checks that every
transaction marked as a conflict is one, and no other.

A trace is a header followed by batches of 2,048 transactions, 32 bytes each. Every transaction
has one read and one write over a key domain; its read version is a fixed age behind its batch's
commit version, and the generator marks a fixed share of reads (7,300 per million by default) to
conflict with a later write.

## Build

```sh
c++ -std=c++20 -O2 -o by5-generate generate.cpp
c++ -std=c++20 -O2 -o by5-verify verify.cpp
c++ -std=c++20 -O2 -o sampler-tests tests/SamplerTests.cpp && ./sampler-tests
```

## Traces of the resolver v1 evaluation

All use `--domain 2000000000 --long-ppm 0` and the defaults: 65,536 transactions per second of
model time, 2,048 per batch, reads 1.5625 s old, 7,300 conflicts per million, seed 1.

| Trace | Arguments | Size | SHA-256 |
|---|---|---|---|
| 40 s smoke | `--warmup 20 --measure 20` | | `edeb2f9ca3791f712912e353e293e4197f7e8c14ae724caddd5426119d9f7925` |
| 1,800 s | `--warmup 1500 --measure 300` | | `6fe019d1fcd68da1a573b2da16486de8dfb267e3ca7ed36fe191e80e12655a54` |
| 7,200 s | `--warmup 6900 --measure 300` | 15,099,494,528 bytes | `0c0d343969fe2e91ef95c09b765b74d078bc9ed48ccf2df15d9dd9db3fd4b6c2` |

For example:

```sh
./by5-generate --domain 2000000000 --long-ppm 0 --warmup 6900 --measure 300 --out pilot-2000M-l0-7200.trace
./by5-verify pilot-2000M-l0-7200.trace
```

The 7,200 s trace took 45,401 s to generate on an AMD Ryzen 7 7700, with a peak of about 30 GiB
of memory, and needs 15.1 GB of disk. Its first 57,600 batches are the 1,800 s trace.
