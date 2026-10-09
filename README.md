# B+ Tree in C++: Memory-only vs Disk-only vs Memory + Disk

Course assignment: implement a B+ tree for **10 million** randomly shuffled records
(`key` = unique alphanumeric, `payload` = random data) and compare three storage designs on
insertion time, search time, memory usage, disk usage, tree height and disk I/O.

**Author:** Ranvir Kumar Singh (P25CS0007)

Everything is written in **C++17** with no external libraries: the B+ tree, the storage engines,
the benchmark, the report generator and the charts (SVG).

## Highlights

* **One B+ tree algorithm, three pluggable storage engines.** `BPlusTree<Store>` is a template, so
  the three variants differ only in storage and the comparison isolates the storage strategy.
* **Real page-based disk format:** fixed 4 KB pages, binary layout, meta page, separate
  append-only payload heap file. The index can be closed and reopened (persistence is tested).
* **Memory + Disk is a real buffer pool:** internal nodes are pinned in RAM, leaves use an LRU
  cache, dirty pages are written back on eviction, payload writes are buffered. An optional
  sweep compares pool sizes.
* **Honest measurement.** Each variant runs in its own process. Only `insert()` time is counted;
  CSV parsing is excluded. Searches are measured cold (right after reopening from disk) and
  warm. Every logical page read/write is counted, and latencies are reported as p50/p95/p99.
* **Verified correctness:**
  * 100k existing-key lookups are checked against the CSV payload.
  * 10k guaranteed-missing keys must return "not found".
  * 1k range scans must produce an identical digest across all variants.
  * A full invariant check (`validate()`) walks the whole tree.
  * The unit tests run 43 checks and are clean under AddressSanitizer/UBSan.
* **Auto-generated report** (`results/REPORT.md`) with tables and 7 SVG charts.

## Project layout

```
src/bptree.hpp      B+ tree algorithm: insert with splits, search, range scan, validate
src/storage.hpp     PageCodec, MemoryStore, DiskStore, BufferPoolStore, HeapFile, MemArena
src/trees.hpp       MemoryTree, DiskOnlyTree, HybridTree (common API)
src/util.hpp        CSV reader, RSS measurement, latency statistics, formatting
src/json.hpp        small JSON writer/parser for result files
src/benchmark.cpp   benchmark one variant      -> results/<variant>.json
src/report.cpp      tables + SVG charts        -> results/REPORT.md, results/plots/*.svg
src/gen_sample.cpp  synthetic dataset for a quick demo
tests/test_bptree.cpp  randomized tests against std::map
run_all.sh          build, run every variant, generate the report
Makefile
```

## Design and assumptions

| Item | Choice |
|---|---|
| Node / page size | 4096 B; one node = one page (`--page-size`) |
| Key storage | fixed slot = longest key in the dataset, zero-padded (byte order = string order) |
| Page header | 16 B: node type, key count, next-leaf page id, reserved |
| Leaf order | `⌊(4096 − 16) / (key_size + 8)⌋` keys; value = 8 B record pointer (40-bit offset + 24-bit length) |
| Internal order | `⌊(4096 − 20) / (key_size + 4)⌋` keys plus one more child; child = 4 B page id |
| Example (16 B keys) | leaf 170 keys, internal 203 keys / 204 children, so height 4 for 10M records |
| Index file | `index.db`; page 0 = meta page (root, height, counters); page *i* at offset *i* × 4096 |
| Payload file | `heap.db`, append-only. Leaves hold only (key, pointer), so fan-out does not depend on payload size |
| Split policy | split at the middle. A leaf split copies the separator up; an internal split moves it up |
| Memory-only | same capacities as the disk pages; payloads in an in-RAM arena with the same pointer format |
| Disk-only | no caching: every node visit = 1 `pread`, every change = 1 `pwrite`, every payload = 1 write |
| Memory + Disk | buffer pool (default 128 MB): internal nodes pinned, LRU leaves, write-back, 1 MB payload buffer; internal levels loaded on open |
| Insert order | CSV order (already shuffled); a duplicate key would overwrite its value |
| I/O metric | logical page reads/writes issued by the program. The OS page cache stays on unless `--direct-io` (macOS `F_NOCACHE`) is given |

Expected cost per operation, for tree height *h*:

* **Memory-only:** *h* node visits.
* **Disk-only:** *h* page reads, plus 1–2 page writes per insert and 1 heap read per search.
* **Memory + Disk:** at most 1 page read (the leaf), because the internal levels are pinned in RAM.

## Setup (macOS)

```bash
xcode-select --install        # installs the C++ compiler (skip if already installed)

git clone https://github.com/<your-username>/bplus-tree-cpp.git
cd bplus-tree-cpp
make                          # builds build/bptree_bench, build/bptree_report, build/gen_sample
make test                     # unit tests
```

## Running

```bash
# demo on 500k synthetic rows (~10 s, no dataset needed)
make demo

# quick run: first 1M records
./run_all.sh --csv /Users/nileshmishra/Downloads/bplus_tree_data_10m.csv --limit 1000000 --validate

# full 10M run + buffer pool sweep (use this for the submission)
./run_all.sh --csv /Users/nileshmishra/Downloads/bplus_tree_data_10m.csv --validate --pool-sweep 32,128,512

open results/REPORT.md
```

The same runs are available as `make quick` and `make full`.

To run a single variant by hand:

```bash
./build/bptree_bench --tree memory --csv /Users/nileshmishra/Downloads/bplus_tree_data_10m.csv
./build/bptree_bench --tree disk   --csv /Users/nileshmishra/Downloads/bplus_tree_data_10m.csv
./build/bptree_bench --tree hybrid --csv /Users/nileshmishra/Downloads/bplus_tree_data_10m.csv --pool-mb 128
./build/bptree_report results
```

Options: `--limit N`, `--page-size B`, `--pool-mb M`, `--no-pin-internal`, `--direct-io`,
`--validate`, `--hits / --misses / --ranges / --range-len`, `--keep-files`.

## Expected resources for 10M records

* Memory-only needs about 1.5–2.5 GB of RAM, depending on payload size.
* Disk usage is the index (≈ 350–600 MB) plus the payload file (≈ total payload size).
* The full run with the sweep takes a few minutes. Disk-only is the slowest variant, and
  progress with an ETA is printed every 1M records.

## Results

See [`results/REPORT.md`](results/REPORT.md) and `results/plots/` (both generated by the run).

## Submitting

```bash
git init && git add . && git commit -m "B+ tree in C++: memory, disk and hybrid + benchmark"
git branch -M main
git remote add origin https://github.com/<your-username>/bplus-tree-cpp.git
git push -u origin main
```

`build/`, `storage/`, `data/` and the CSV are git-ignored. The `results/` folder (report,
charts and JSON) is committed.
