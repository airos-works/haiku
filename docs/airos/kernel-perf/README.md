# Kernel scalability measurements (airos-works/haiku #96-#101)

`kperf.c` is the benchmark used for the kernel lock and VM work of November
2026: mutex convoys (#96), per-CPU page caches (#97), file fault-around and
read-around (#98), anonymous multi-page faults and 2 MiB pages (#99) and the
page daemon (#100). Every result is one line `KP <test> <args> <value> <unit>`.

| Test | What it does |
|---|---|
| `faultthr N [MiB]` | N threads of one team each touch their own fresh area |
| `faultteam N [MiB]` | the same with N separate teams |
| `stat N [path]` | N threads `stat()` one path for a second |
| `areas N` | N threads create, touch and delete small areas |
| `malloc N` | N threads run a malloc/free mix |
| `anon MiB [N]` | touch fresh anonymous memory, walk it, unmap it |
| `filemap path` | map a cached file and read every page |
| `exec N` | fork + exec `/bin/true` |
| `daemon MiB s` | keep touching memory; CPU time of the page daemon |
| `verify MiB rounds` | anonymous memory keeps its contents |
| `mapwrite path MiB` | writes through a shared file mapping reach the file |
| `pressure anonMiB fileMiB path rounds` | anonymous memory plus a mapped file larger than what is left: page contents under heavy eviction, and how long it takes |
| `large MiB` | the same area with and without `B_LARGE_PAGES_AREA`: touch time, a random walk (TLB reach), fork with copy-on-write, shrinking, `mprotect()`, deletion |
| `lsteps MiB mode` | the large-page steps one at a time (mode bits: 1 fork, 2 child writes, 4 parent writes, 8 resize, 16 mprotect) |

## Running it in QEMU

`build.sh` cross-compiles it against a configured build directory's libroot.
The QEMU harness boots a copy of an image that runs a test script from a
fourth NVMe disk at boot (the `iotest` launch job of the #102/#103 work, see
`/data1/scratch/io-build/UserBuildConfig` on the build server) and powers off:

```
./build.sh
./mksuite.sh suite.sh               # suite-head.sh + kperf (base64) + suite-tail.sh
IMAGE=haiku-nightly.image SCRIPT=suite.sh ./qemu-run.sh <name>
```

`tail-check.sh` (correctness under pressure), `tail-press.sh` (pressure only)
and `tail-large.sh` (large pages) replace `suite-tail.sh` as the second
argument of `mksuite.sh`.

Run A and B images side by side (two `qemu-run.sh` at once) when the build
server is busy: timings of one run against an earlier one varied by up to 5x
on the shared host, side-by-side runs did not.
