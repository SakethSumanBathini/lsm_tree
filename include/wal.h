#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <liburing.h>
#include <sys/uio.h>
#include <cstring>
#include <iostream>
#include <mutex>

// WAL (Write-Ahead Log) using Linux io_uring for Asynchronous Zero-Copy Logging.
//
// Uses O_DIRECT for zero-copy writes from memory-aligned buffers.
// Submits write requests to the submission queue (SQ) and reaps from completion queue (CQ).
// On destruction, fdatasync ensures all writes are durable before the WAL file is closed.

class WAL {
public:
    struct Entry {
        enum class Type { PUT = 0x01, DEL = 0x02 };
        Type        type;
        std::string key;
        std::string value;
    };

    explicit WAL(const std::string& path) : path_(path) {
        fd_ = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_DIRECT, 0644);
        if (fd_ < 0) throw std::runtime_error("WAL: cannot open file with O_DIRECT: " + path);

        if (io_uring_queue_init(8, &ring_, 0) < 0) {
            close(fd_);
            throw std::runtime_error("WAL: failed to init io_uring");
        }
    }

    ~WAL() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            reap();
        }
        // Ensure all writes are durable on disk before closing.
        // With O_DIRECT, data bypasses the page cache, but io_uring writes
        // may still be in the kernel's I/O queue. fdatasync forces them out.
        if (fd_ >= 0) fdatasync(fd_);
        io_uring_queue_exit(&ring_);
        if (fd_ >= 0) close(fd_);
    }

    void logPut(const std::string& key, const std::string& value) {
        writeEntry(Entry::Type::PUT, key, value);
    }

    void logDel(const std::string& key) {
        writeEntry(Entry::Type::DEL, key, "");
    }

    std::vector<Entry> recover() const {
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

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        reap();                     // drain any pending CQEs
        io_uring_queue_exit(&ring_);
        close(fd_);
        std::filesystem::remove(path_);
        fd_ = open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_DIRECT, 0644);
        io_uring_queue_init(8, &ring_, 0);
    }

private:
    int fd_;
    std::string path_;
    struct io_uring ring_;
    mutable std::mutex mu_;

    void writeEntry(Entry::Type type, const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mu_);
        // Allocate 4096-aligned buffer for O_DIRECT
        size_t size = 1 + 4 + 4 + key.size() + value.size() + 4;
        size_t aligned_size = (size + 511) & ~511; // Align to 512 for O_DIRECT
        
        void* buf;
        if (posix_memalign(&buf, 4096, aligned_size) != 0) throw std::runtime_error("WAL: memalign failed");
        std::memset(buf, 0, aligned_size);

        char* p = static_cast<char*>(buf);
        *p++ = static_cast<uint8_t>(type);
        uint32_t klen = key.size(), vlen = value.size();
        std::memcpy(p, &klen, 4); p += 4;
        std::memcpy(p, &vlen, 4); p += 4;
        std::memcpy(p, key.data(), klen); p += klen;
        std::memcpy(p, value.data(), vlen); p += vlen;
        
        uint32_t crc = crc32(static_cast<uint8_t>(type), key, value);
        std::memcpy(p, &crc, 4);

        // Prepare io_uring SQE
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_write(sqe, fd_, buf, aligned_size, -1); // -1 = current offset (append)
        io_uring_sqe_set_data(sqe, buf); // Tag with buffer pointer for freeing later
        
        io_uring_submit(&ring_);
        
        // Reap any completed CQEs (non-blocking) to free buffers promptly
        reap();
    }

    void reap() {
        struct io_uring_cqe* cqe;
        while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
            void* buf = io_uring_cqe_get_data(cqe);
            free(buf);
            io_uring_cqe_seen(&ring_, cqe);
        }
    }

    static uint32_t crc32(uint8_t type, const std::string& key, const std::string& value) {
        uint32_t crc = 0xFFFFFFFF;
        auto update = [&](uint8_t byte) {
            crc ^= byte;
            for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        };
        update(type);
        for (unsigned char c : key) update(c);
        for (unsigned char c : value) update(c);
        return crc ^ 0xFFFFFFFF;
    }
};
