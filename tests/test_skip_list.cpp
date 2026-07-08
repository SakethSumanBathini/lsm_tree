#include "../src/db/lsm_engine.h"
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#include <filesystem>

void pass(const std::string& test) {
    std::cout << "  \033[32m✓\033[0m " << test << "\n";
}
void fail(const std::string& test, const std::string& msg) {
    std::cout << "  \033[31m✗\033[0m " << test << " — " << msg << "\n";
    std::exit(1);
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Running Unit Test: SkipList & Concurrency\n";
    std::cout << "===========================================\n";

    std::filesystem::remove_all("/tmp/lsm_test_skiplist");
    {
        LSMEngine db("/tmp/lsm_test_skiplist");

        // Basic read/write
        db.put("key1", "val1");
        db.put("key2", "val2");
        auto v = db.get("key1");
        if (!v || *v != "val1") fail("Basic read/write", "Expected val1");
        pass("Basic PUT/GET");

        // Overwrites
        db.put("key1", "val1_new");
        v = db.get("key1");
        if (!v || *v != "val1_new") fail("Overwrite key1", "Expected val1_new");
        pass("Overwrite returns latest value");

        // Deletion / Tombstone
        db.del("key2");
        v = db.get("key2");
        if (v.has_value()) fail("Delete key2", "Expected deleted key to return nullopt");
        pass("DELETE writes tombstone");
    }

    // Concurrent writes test
    std::filesystem::remove_all("/tmp/lsm_test_concurrent");
    {
        LSMEngine db("/tmp/lsm_test_concurrent");
        const int THREADS = 4;
        const int KEYS_PER = 100;

        std::vector<std::thread> threads;
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&db, t]() {
                for (int i = 0; i < KEYS_PER; ++i) {
                    db.put("thread_" + std::to_string(t) + "_key_" + std::to_string(i),
                           "val_" + std::to_string(t) + "_" + std::to_string(i));
                }
            });
        }
        for (auto& t : threads) t.join();

        int found = 0;
        for (int t = 0; t < THREADS; ++t) {
            for (int i = 0; i < KEYS_PER; ++i) {
                if (db.get("thread_" + std::to_string(t) + "_key_" + std::to_string(i)).has_value()) {
                    found++;
                }
            }
        }
        if (found != THREADS * KEYS_PER) {
            fail("Concurrent writes", "Expected " + std::to_string(THREADS * KEYS_PER) + " keys, found " + std::to_string(found));
        }
        pass("Concurrent writes thread-safe insertion (" + std::to_string(found) + " keys found)");
    }

    std::cout << "\033[32mSkipList & Concurrency tests passed successfully.\033[0m\n\n";
    return 0;
}
