#include "wal.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

std::vector<WAL::Entry> WAL::recover() const {
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

        // A truncated or corrupted length header is skipped, not fatal.
        //
        // Both checks below used to `break`, abandoning the remainder of the
        // log and discarding every valid entry after the damaged one — the
        // opposite of what recovery is for. It was also inconsistent with how
        // this same loop already handles its two other corruption cases: the
        // padding branch above and the CRC mismatch below both advance to the
        // next 512-byte boundary and continue.
        //
        // Entries are written as 512-byte aligned blocks, so that boundary is
        // where the next record begins. Advancing there resynchronises with the
        // record stream rather than giving up on it, and always moves forward,
        // so the loop still terminates.
        if (offset + 8 > total) {
            offset = ((entry_start / 512) + 1) * 512;
            continue;
        }

        uint32_t klen, vlen;
        std::memcpy(&klen, buf.data() + offset, 4); offset += 4;
        std::memcpy(&vlen, buf.data() + offset, 4); offset += 4;

        if (offset + klen + vlen + 4 > total) {
            std::cerr << "WAL: entry at offset " << entry_start
                      << " declares a length past the end of the log (klen=" << klen
                      << ", vlen=" << vlen << "). Skipping corrupted entry.\n";
            offset = ((entry_start / 512) + 1) * 512;
            continue;
        }

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
