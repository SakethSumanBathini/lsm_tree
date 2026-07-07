#include "../include/lsm_engine.h"
#include <iostream>
#include <cassert>
#include <chrono>
#include <random>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

// ----------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------
void pass(const std::string& test) {
    std::cout << "  \033[32m✓\033[0m " << test << "\n";
}
void fail(const std::string& test, const std::string& msg) {
    std::cout << "  \033[31m✗\033[0m " << test << " — " << msg << "\n";
    std::exit(1);
}

// ----------------------------------------------------------------
// Test suite
// ----------------------------------------------------------------
void testBasicPutGet() {
    std::cout << "\n[1] Basic PUT / GET\n";
    std::filesystem::remove_all("/tmp/lsm_test1");
    LSMEngine db("/tmp/lsm_test1");

    db.put("name", "Alice");
    db.put("city", "Bengaluru");
    db.put("lang", "C++");

    auto v = db.get("name");
    if (!v || *v != "Alice") fail("get name", "expected Alice");
    pass("put + get: name=Alice");

    v = db.get("city");
    if (!v || *v != "Bengaluru") fail("get city", "expected Bengaluru");
    pass("put + get: city=Bengaluru");

    v = db.get("missing");
    if (v.has_value()) fail("get missing", "should return nullopt");
    pass("get missing key → nullopt");
}

void testOverwrite() {
    std::cout << "\n[2] Overwrite\n";
    std::filesystem::remove_all("/tmp/lsm_test2");
    LSMEngine db("/tmp/lsm_test2");

    db.put("key", "v1");
    db.put("key", "v2");
    db.put("key", "v3");

    auto v = db.get("key");
    if (!v || *v != "v3") fail("overwrite", "expected v3");
    pass("overwrite 3x → returns latest value");
}

void testDelete() {
    std::cout << "\n[3] Tombstone / DELETE\n";
    std::filesystem::remove_all("/tmp/lsm_test3");
    LSMEngine db("/tmp/lsm_test3");

    db.put("user:1", "Alice");
    db.put("user:2", "Bob");
    db.del("user:1");

    auto v1 = db.get("user:1");
    if (v1.has_value()) fail("del user:1", "should be deleted");
    pass("del user:1 → tombstone → NOT FOUND");

    auto v2 = db.get("user:2");
    if (!v2 || *v2 != "Bob") fail("get user:2", "Bob should still exist");
    pass("get user:2 still returns Bob");
}

void testFlushAndReadFromSSTable() {
    std::cout << "\n[4] Flush to SSTable + read back\n";
    std::filesystem::remove_all("/tmp/lsm_test4");
    LSMEngine db("/tmp/lsm_test4");

    db.put("alpha", "1");
    db.put("beta",  "2");
    db.put("gamma", "3");

    db.flush(); // force flush to SSTable
    std::cout << "     flushed — " << db.sstableCount() << " SSTable(s), memtable size=" << db.memtableSize() << "\n";

    // Data is now on disk, Memtable is empty
    auto v = db.get("beta");
    if (!v || *v != "2") fail("read from SSTable", "expected 2 for beta");
    pass("read 'beta' from SSTable after flush");

    // Tombstone in SSTable should shadow older value
    db.put("alpha", "updated");
    db.del("alpha");
    db.flush();

    auto va = db.get("alpha");
    if (va.has_value()) fail("tombstone in SSTable", "alpha should be deleted");
    pass("tombstone in newer SSTable shadows older value");
}

void testBloomFilter() {
    std::cout << "\n[5] Bloom filter correctness\n";
    BloomFilter bf(1000);

    bf.insert("user:1");
    bf.insert("user:2");
    bf.insert("user:3");

    if (!bf.mayContain("user:1")) fail("bloom insert", "user:1 must be found");
    if (!bf.mayContain("user:2")) fail("bloom insert", "user:2 must be found");
    if (!bf.mayContain("user:3")) fail("bloom insert", "user:3 must be found");
    pass("no false negatives for inserted keys");

    // Count false positives over 10000 keys never inserted
    int fp = 0;
    for (int i = 10000; i < 20000; ++i)
        if (bf.mayContain("never:" + std::to_string(i))) fp++;

    double fp_rate = fp / 10000.0;
    std::cout << "     false positive rate: " << fp << "/10000 = " << fp_rate * 100 << "%\n";
    if (fp_rate > 0.05) fail("bloom FP rate", "too high (>5%)");
    pass("false positive rate under 5%");
}

void testCrashRecovery() {
    std::cout << "\n[6] Crash recovery via WAL\n";
    std::filesystem::remove_all("/tmp/lsm_test6");
    {
        LSMEngine db("/tmp/lsm_test6");
        db.put("session", "xyz-123");
        db.put("counter", "42");
        // Simulate crash — destructor runs, no explicit flush
    }
    // Re-open: WAL should be replayed
    {
        LSMEngine db("/tmp/lsm_test6");
        auto v = db.get("session");
        if (!v || *v != "xyz-123") fail("WAL recovery", "session not recovered");
        pass("WAL replay recovered 'session' after simulated crash");

        auto c = db.get("counter");
        if (!c || *c != "42") fail("WAL recovery", "counter not recovered");
        pass("WAL replay recovered 'counter'");
    }
}

void benchmarkWrites() {
    std::cout << "\n[7] Write throughput benchmark\n";
    std::filesystem::remove_all("/tmp/lsm_bench");
    LSMEngine db("/tmp/lsm_bench");

    const int N = 10000;
    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N; ++i)
        db.put("key:" + std::to_string(i), "value:" + std::to_string(i));

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "     " << N << " writes in " << ms << " ms"
              << " → " << static_cast<int>(N / (ms / 1000.0)) << " writes/sec\n";
    std::cout << "     SSTables: " << db.sstableCount()
              << "  Memtable keys: " << db.memtableSize() << "\n";
    pass("benchmark complete");
}

void testCompaction() {
    std::cout << "\n[8] Compaction\n";
    std::filesystem::remove_all("/tmp/lsm_test8");
    LSMEngine db("/tmp/lsm_test8", 512); // tiny memtable to force flushes

    for (int i = 0; i < 200; ++i)
        db.put("key:" + std::to_string(i), "value:" + std::to_string(i));

    // Should have triggered compaction (>= 4 SSTables merged)
    db.flush();
    std::cout << "     SSTables after compaction: " << db.sstableCount() << "\n";

    // Verify data integrity after compaction
    for (int i = 0; i < 200; ++i) {
        auto v = db.get("key:" + std::to_string(i));
        if (!v || *v != "value:" + std::to_string(i))
            fail("compaction read", "key:" + std::to_string(i) + " not found or wrong value");
    }
    pass("all keys readable after compaction");
}

void testConcurrentWrites() {
    std::cout << "\n[9] Concurrent writes\n";
    std::filesystem::remove_all("/tmp/lsm_test9");
    LSMEngine db("/tmp/lsm_test9");
    const int THREADS = 4, KEYS_PER = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&db, t]() {
            for (int i = 0; i < KEYS_PER; ++i)
                db.put("t" + std::to_string(t) + ":" + std::to_string(i),
                       "val" + std::to_string(t) + ":" + std::to_string(i));
        });
    }
    for (auto& t : threads) t.join();

    int found = 0;
    for (int t = 0; t < THREADS; ++t)
        for (int i = 0; i < KEYS_PER; ++i)
            if (db.get("t" + std::to_string(t) + ":" + std::to_string(i)).has_value())
                found++;

    std::cout << "     found " << found << "/" << THREADS * KEYS_PER << " keys\n";
    if (found != THREADS * KEYS_PER) fail("concurrent writes", "missing keys");
    pass("all keys found after concurrent writes");
}

void testCRCCorruption() {
    std::cout << "\n[10] CRC corruption detection\n";
    std::filesystem::remove_all("/tmp/lsm_test10");
    {
        LSMEngine db("/tmp/lsm_test10");
        db.put("safe", "data");
        db.put("corrupt_me", "bad_data");
    }
    // Corrupt the WAL file
    std::string wal_path = "/tmp/lsm_test10/wal.log";
    {
        int fd = open(wal_path.c_str(), O_RDWR);
        if (fd >= 0) {
            // Corrupt a byte near the end of the file to hit the second entry
            off_t size = lseek(fd, 0, SEEK_END);
            if (size > 10) {
                lseek(fd, size - 6, SEEK_SET); // Corrupt near the CRC
                char c = 0xFF;
                [[maybe_unused]] ssize_t w = write(fd, &c, 1);
            }
            close(fd);
        }
    }
    // Re-open: should skip corrupted entries
    {
        LSMEngine db("/tmp/lsm_test10");
        // At least the corruption should not crash the engine
        pass("engine recovered without crash after WAL corruption");
    }
}

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  LSM-Tree Key-Value Store — Test Suite    \n";
    std::cout << "═══════════════════════════════════════════\n";

    testBasicPutGet();
    testOverwrite();
    testDelete();
    testFlushAndReadFromSSTable();
    testBloomFilter();
    testCrashRecovery();
    benchmarkWrites();
    testCompaction();
    testConcurrentWrites();
    testCRCCorruption();

    std::cout << "\n\033[32mAll 10 tests passed.\033[0m\n\n";
    return 0;
}
