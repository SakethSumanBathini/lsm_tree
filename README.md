# LSM-Tree Key-Value Store (Production-Grade C++)

An advanced implementation of a Log-Structured Merge-Tree key-value store featuring production-grade optimizations.

## Advanced Features

1. **Asynchronous Zero-Copy Logging (io_uring):**
   The Write-Ahead Log (WAL) uses Linux `io_uring` with `O_DIRECT` for asynchronous, non-blocking disk I/O. This bypasses the kernel page cache and provides direct, zero-copy durability. CRC32 checksums protect every entry, and recovery validates them to skip corrupted entries.

2. **Vectorized Block Bloom Filters:**
   Implements 64-byte cache-aligned block Bloom filters. Checking for a key's existence incurs at most one cache-miss penalty, significantly speeding up reads by ensuring all bits for a key live on the same CPU cache line.

3. **Lock-Free SkipList MemTable:**
   The MemTable is built on a concurrent lock-free SkipList with atomic pointers. It uses a thread-safe bump-pointer Arena allocator (atomic CAS + mutex for new blocks) for high-performance memory management, allowing multiple threads to insert data without locking the entire table. Thread-local RNG ensures safe concurrent height generation.

4. **Size-Tiered Compaction:**
   When ≥4 SSTables accumulate, they are merged into a single SSTable — deduplicating keys and removing tombstones. This bounds read amplification and reclaims disk space.

5. **Durability Guarantees:**
   SSTables are `fsync`'d before the WAL is cleared, ensuring no data loss on crash. Exclusive file locking (`flock`) prevents multiple engine instances from corrupting the same data directory.

## Architecture

```
Write Path:
  PUT(key, value)
    → io_uring WAL append (async, direct I/O, CRC32 per entry)
    → Lock-Free SkipList insert (atomic CAS, in-place overwrite)
    → if full → flush MemTable to SSTable → fsync → clear WAL
    → if ≥4 SSTables → size-tiered compaction

Read Path:
  GET(key)
    → MemTable (SkipList traversal, O(log n))
    → For each SSTable (newest → oldest):
        → Blocked Bloom Filter (1 cache miss, skip if absent)
        → Binary search sparse index → scan block
```

## Build & Run

### Using Docker (Recommended for macOS/Windows)

Since this project uses Linux-specific `io_uring`, the easiest way to run it on macOS or Windows is via Docker:

```bash
# From the lsm_tree directory:
docker build -t lsm-advanced .
docker run --rm lsm-advanced
```

### Native Linux

This project requires **Linux** (kernel 5.1+) and **liburing**.

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install liburing-dev pkg-config

# Compile
mkdir build && cd build
cmake ..
make

# Run tests
./lsm_tests
```

## Verification

- **Correctness:** Passing all 10 tests:
  1. Basic PUT/GET
  2. Overwrite (returns latest value)
  3. Tombstone / DELETE
  4. Flush to SSTable + read back
  5. Bloom filter correctness (FPR < 5%)
  6. Crash recovery via WAL replay
  7. Write throughput benchmark
  8. Compaction (data integrity after merge)
  9. Concurrent multi-threaded writes
  10. CRC corruption detection (corrupted entries skipped)
- **Concurrency:** MemTable supports atomic multi-threaded insertions with lock-free SkipList, thread-safe Arena, and thread-local RNG.
- **Durability:** `fsync` on SSTable before WAL clear; CRC32 validation on recovery; exclusive file locking.
- **Performance:** Cache-aligned Bloom filters reduce read latency; `io_uring` reduces write stall times; Arena allocator minimizes allocation overhead.
