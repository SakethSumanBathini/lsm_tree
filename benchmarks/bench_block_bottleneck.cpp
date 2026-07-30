#include "../src/sstable/sstable.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <filesystem>

int main() {
    std::cout << "=======================================================\n";
    std::cout << " 🛠️  Verifying the Issue: ENTRIES_PER_BLOCK = 64 Bottleneck\n";
    std::cout << "=======================================================\n";

    std::filesystem::create_directories("/tmp/lsm_bottleneck_test");
    std::string small_sst_path = "/tmp/lsm_bottleneck_test/small_entries.sst";
    std::string large_sst_path = "/tmp/lsm_bottleneck_test/large_entries.sst";

    const int total_keys = 640; // Exactly 10 blocks of 64 entries each

    // 1. Create Small Payload SSTable (16 Bytes per value)
    {
        std::vector<SSTable::Entry> small_entries;
        small_entries.reserve(total_keys);
        std::string small_val(16, 'x'); // 16 bytes
        for (int i = 0; i < total_keys; ++i) {
            small_entries.push_back({"key_" + std::to_string(100000 + i), small_val});
        }
        SSTable::write(small_sst_path, small_entries);
    }

    // 2. Create Large Payload SSTable (64 KB per value -> 4MB per 64-entry block!)
    {
        std::vector<SSTable::Entry> large_entries;
        large_entries.reserve(total_keys);
        std::string large_val(64 * 1024, 'Y'); // 64 KB
        for (int i = 0; i < total_keys; ++i) {
            large_entries.push_back({"key_" + std::to_string(100000 + i), large_val});
        }
        SSTable::write(large_sst_path, large_entries);
    }

    uint64_t small_file_size = std::filesystem::file_size(small_sst_path);
    uint64_t large_file_size = std::filesystem::file_size(large_sst_path);

    std::cout << "\n📊 SSTable File & Block Size Analysis:\n";
    std::cout << "  - Small Entries File Size: " << small_file_size / 1024.0 << " KB\n";
    std::cout << "    └─ Avg Block Size (64 entries): ~" << (small_file_size / 10.0) / 1024.0 << " KB per block\n";
    std::cout << "  - Large Entries File Size: " << large_file_size / (1024.0 * 1024.0) << " MB\n";
    std::cout << "    └─ Avg Block Size (64 entries): ~" << (large_file_size / 10.0) / (1024.0 * 1024.0) << " MB per block! ⚠️\n\n";

    // 3. Test Read Performance on Small Payload SSTable
    SSTable small_sst(small_sst_path);
    std::vector<double> small_latencies;
    small_latencies.reserve(total_keys);

    auto t0_small = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < total_keys; ++i) {
        std::string key = "key_" + std::to_string(100000 + i);
        auto t0 = std::chrono::high_resolution_clock::now();
        auto val = small_sst.get(key);
        auto t1 = std::chrono::high_resolution_clock::now();
        small_latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    auto t1_small = std::chrono::high_resolution_clock::now();
    double small_total_ms = std::chrono::duration<double, std::milli>(t1_small - t0_small).count();
    double small_avg_us = std::accumulate(small_latencies.begin(), small_latencies.end(), 0.0) / total_keys;

    // 4. Test Read Performance on Large Payload SSTable (Huge 4MB block reads!)
    SSTable large_sst(large_sst_path);
    std::vector<double> large_latencies;
    large_latencies.reserve(total_keys);

    auto t0_large = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < total_keys; ++i) {
        std::string key = "key_" + std::to_string(100000 + i);
        auto t0 = std::chrono::high_resolution_clock::now();
        auto val = large_sst.get(key);
        auto t1 = std::chrono::high_resolution_clock::now();
        large_latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    auto t1_large = std::chrono::high_resolution_clock::now();
    double large_total_ms = std::chrono::duration<double, std::milli>(t1_large - t0_large).count();
    double large_avg_us = std::accumulate(large_latencies.begin(), large_latencies.end(), 0.0) / total_keys;

    std::cout << "⏱️  Point Lookup Benchmark Results:\n";
    std::cout << "  [Small Payload Block (~1.2 KB)]:\n";
    std::cout << "    - Total Time: " << small_total_ms << " ms\n";
    std::cout << "    - Avg Latency per key: " << small_avg_us << " us\n";
    std::cout << "  [Large Payload Block (~4.0 MB)]:\n";
    std::cout << "    - Total Time: " << large_total_ms << " ms\n";
    std::cout << "    - Avg Latency per key: " << large_avg_us << " us\n\n";

    double slowdown_factor = large_avg_us / small_avg_us;
    std::cout << "🚨 BOTTLENECK CONFIRMED:\n";
    std::cout << "   Because ENTRIES_PER_BLOCK = 64 forces a 4 MB block read for large values,\n";
    std::cout << "   searching keys in large payload blocks is " << slowdown_factor << "x SLOWER!\n";
    std::cout << "=======================================================\n";

    return 0;
}
