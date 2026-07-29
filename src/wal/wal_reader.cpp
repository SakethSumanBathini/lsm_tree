#include "wal.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

std::vector<WAL::Entry> WAL::recover() const {
    // Held for the whole read.
    //
    // recover() is const and previously took no lock, so it could open and read
    // path_ while clear() was unlinking and recreating that same file, or while
    // writeEntry() was appending to it. Either overlap hands recovery a file
    // that is being replaced underneath it — a partial read, or a read of a log
    // that no longer represents the engine's state.
    //
    // mu_ is mutable, so a const method can take it, and it is the same mutex
    // writeEntry() and clear() already hold.
    std::lock_guard<std::mutex> lock(mu_);

    // Recovery reads the entire WAL file into memory and parses entries.
    // Uses regular O_RDONLY (not O_DIRECT) since recovery is a one-time startup cost
    // and avoids O_DIRECT alignment constraints that complicate cross-block reads.
    int rfd = open(path_.c_str(), O_RDONLY);
    if (rfd < 0) return {};

    // Read entire file into a contiguous buffer
    std::vector<char> buf;
    char tmp[4096];
    ssize_t bytes;
    while ((bytes = read(rfd, tmp, sizeof(tmp))) > 0) {
        buf.insert(buf.end(), tmp, tmp + bytes);
    }
    close(rfd);

    std::vector<Entry> entries;
    size_t offset = 0;
    size_t total = buf.size();

    while (offset < total) {
        // Each WAL entry is written as a 512-byte-aligned block (for O_DIRECT).
        // Record the start so we can skip padding after reading the entry.
        size_t entry_start = offset;

        if (offset + 1 > total) break;
        uint8_t type = static_cast<uint8_t>(buf[offset++]);
        if (type == 0) {
            // Hit padding — skip to next 512-byte boundary
            offset = ((entry_start / 512) + 1) * 512;
            continue;
        }

        if (offset + 8 > total) break;
        uint32_t klen, vlen;
        std::memcpy(&klen, buf.data() + offset, 4); offset += 4;
        std::memcpy(&vlen, buf.data() + offset, 4); offset += 4;

        if (offset + klen + vlen + 4 > total) break;
        std::string key(buf.data() + offset, klen); offset += klen;
        std::string value(buf.data() + offset, vlen); offset += vlen;

        uint32_t stored_crc;
        std::memcpy(&stored_crc, buf.data() + offset, 4); offset += 4;

        // Validate CRC before accepting the entry
        uint32_t expected_crc = crc32(type, key, value);
        if (stored_crc != expected_crc) {
            std::cerr << "WAL: CRC mismatch for key '" << key
                      << "' (stored=0x" << std::hex << stored_crc
                      << ", expected=0x" << expected_crc << std::dec
                      << "). Skipping corrupted entry.\n";
            // Skip to next aligned boundary
            offset = ((entry_start / 512) + 1) * 512;
            continue;
        }

        Entry e;
        e.type = (type == 0x01) ? Entry::Type::PUT : Entry::Type::DEL;
        e.key = key;
        e.value = value;
        entries.push_back(std::move(e));

        // Advance to next 512-byte boundary (skip O_DIRECT padding)
        size_t entry_size = 1 + 4 + 4 + klen + vlen + 4;
        size_t aligned_size = (entry_size + 511) & ~511;
        offset = entry_start + aligned_size;
    }
    return entries;
}
