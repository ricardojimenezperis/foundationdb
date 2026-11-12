# Sanitization

The files in this directory were produced as listed in
[INVENTORY.txt](INVENTORY.txt). Before publication, local file system
paths were replaced. No result, count, time, trace or test name was
changed.

| Original | Published | Files |
|---|---|---|
| The local directory holding the LLVM 20.1.8 release archive (`InstalledDir` of `clang++ --version`) | `<redacted-local-path>/LLVM-20.1.8-macOS-ARM64/bin` | `toolchain.txt` |

One value was added after the run: in `matrix/mac/MANIFEST.txt`, the
SHA-256 of `mac-matrix.sh`, which the run recorded as empty because the
script was invoked by a relative path. The line says so.

[SANITIZATION-SHA256.tsv](SANITIZATION-SHA256.tsv) gives, for every file,
the SHA-256 before and after this step and the number of replacements.
[SHA256SUMS](SHA256SUMS) lists the SHA-256 of every file except itself.
