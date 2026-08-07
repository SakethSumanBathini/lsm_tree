#include "../src/db/lsm_engine.h"
#include "../src/wal/wal.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

void pass(const std::string& test) {
    std::cout << "  \033[32m✓\033[0m " << test << "\n";
}
void fail(const std::string& test, const std::string& msg) {
    std::cout << "  \033[31m✗\033[0m " << test << " — " << msg << "\n";
    std::exit(1);
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Running Unit Test: WAL Recovery & Checks\n";
    std::cout << "===========================================\n";

    // Write the log directly rather than through the engine.
    //
    // This block and the corruption block below both need a WAL that still
    // holds its records. Destroying an LSMEngine no longer leaves one, because
    // shutdown now flushes — so producing the log through the WAL itself is
    // what actually models a process that died before flushing.
    std::filesystem::remove_all("/tmp/lsm_test_wal");
    std::filesystem::create_directories("/tmp/lsm_test_wal");
    {
        WAL wal("/tmp/lsm_test_wal/wal.log");
        wal.logPut("keyA", "dataA");
        wal.logPut("keyB", "dataB");
    }

    // Re-open and verify recovered entries
    {
        LSMEngine db("/tmp/lsm_test_wal");
        auto v = db.get("keyA");
        if (!v || *v != "dataA") fail("WAL recovery", "Expected keyA to hold 'dataA'");
        
        v = db.get("keyB");
        if (!v || *v != "dataB") fail("WAL recovery", "Expected keyB to hold 'dataB'");
        pass("Durable Log Replay");
    }

    // CRC Corruption Test
    std::filesystem::remove_all("/tmp/lsm_test_wal_corrupt");
    std::filesystem::create_directories("/tmp/lsm_test_wal_corrupt");
    {
        WAL wal("/tmp/lsm_test_wal_corrupt/wal.log");
        wal.logPut("safe_key", "good_data");
        wal.logPut("corrupt_key", "corrupt_data");
    }

    // Corrupt a byte in the second entry of the WAL file
    std::string wal_path = "/tmp/lsm_test_wal_corrupt/wal.log";
    int fd = open(wal_path.c_str(), O_RDWR);
    if (fd >= 0) {
        off_t size = lseek(fd, 0, SEEK_END);
        if (size >= 512) {
            // Write a corrupt type byte at the start of the second block (offset 512)
            lseek(fd, 512, SEEK_SET);
            char corrupt_byte = 0x03; // Invalid type to cause CRC mismatch
            [[maybe_unused]] ssize_t w = write(fd, &corrupt_byte, 1);
        }
        close(fd);
    }

    // Reopen. The engine should skip the corrupt second entry but keep the safe one
    {
        LSMEngine db("/tmp/lsm_test_wal_corrupt");
        auto v = db.get("safe_key");
        if (!v || *v != "good_data") {
            fail("CRC Corruption", "Expected safe_key to still be recovered correctly");
        }
        
        v = db.get("corrupt_key");
        if (v.has_value()) {
            fail("CRC Corruption", "Expected corrupted key to be skipped during recovery");
        }
        pass("Checksum Verification: corrupted entries successfully skipped");
    }

    std::cout << "\033[32mWAL Recovery tests passed successfully.\033[0m\n\n";
    return 0;
}
