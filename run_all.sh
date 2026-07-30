#!/bin/bash
set -e

echo "==========================================="
echo "  LSM-Tree Key-Value Store — Test Runner  "
echo "==========================================="

echo "Running Unit Tests..."
./build/test_skip_list
./build/test_bloom_filter
./build/test_wal_recovery
./build/test_sstable
./build/test_compaction_streaming

echo "Running Benchmarks..."
./build/bench_write
./build/bench_read

echo "==========================================="
echo "  All tests and benchmarks completed!      "
echo "==========================================="
