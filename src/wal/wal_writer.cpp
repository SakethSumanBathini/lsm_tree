#include "wal.h"
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <filesystem>

WAL::WAL(const std::string& path) : path_(path) {
    fd_ = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_DIRECT, 0644);
    if (fd_ < 0) throw std::runtime_error("WAL: cannot open file with O_DIRECT: " + path);

    if (io_uring_queue_init(8, &ring_, 0) < 0) {
        close(fd_);
        throw std::runtime_error("WAL: failed to init io_uring");
    }
}

WAL::~WAL() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        reap();
    }
    // Ensure all writes are durable on disk before closing.
    if (fd_ >= 0) fdatasync(fd_);
    io_uring_queue_exit(&ring_);
    if (fd_ >= 0) close(fd_);
}

void WAL::logPut(const std::string& key, const std::string& value) {
    writeEntry(Entry::Type::PUT, key, value);
}

void WAL::logDel(const std::string& key) {
    writeEntry(Entry::Type::DEL, key, "");
}

void WAL::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    reap();                     // drain any pending CQEs
    io_uring_queue_exit(&ring_);
    close(fd_);
    std::filesystem::remove(path_);
    fd_ = open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_DIRECT, 0644);
    io_uring_queue_init(8, &ring_, 0);
}

void WAL::writeEntry(Entry::Type type, const std::string& key, const std::string& value) {
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

    // Prepare io_uring SQE.
    //
    // io_uring_get_sqe() yields nullptr when the submission ring has no free
    // slot. The result was previously passed straight to io_uring_prep_write(),
    // which dereferences it. Reaching that state needs submissions to stop
    // draining the ring — see the unchecked io_uring_submit() below, whose
    // failures leave SQEs queued — but the pointer has to be checked either way.
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        // Flush what is queued to release slots, harvest completions, retry once.
        int flushed = io_uring_submit(&ring_);
        if (flushed < 0) {
            free(buf);
            throw std::runtime_error(std::string("WAL: io_uring_submit failed while draining: ")
                                     + std::strerror(-flushed));
        }
        reap();
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            free(buf);
            throw std::runtime_error("WAL: io_uring submission queue full");
        }
    }

    io_uring_prep_write(sqe, fd_, buf, aligned_size, -1); // -1 = current offset (append)
    io_uring_sqe_set_data(sqe, buf); // Tag with buffer pointer for freeing later
    
    io_uring_submit(&ring_);
    
    // Reap any completed CQEs (non-blocking) to free buffers promptly
    reap();
}

void WAL::reap() {
    struct io_uring_cqe* cqe;
    while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
        void* buf = io_uring_cqe_get_data(cqe);
        free(buf);
        io_uring_cqe_seen(&ring_, cqe);
    }
}

uint32_t WAL::crc32(uint8_t type, const std::string& key, const std::string& value) {
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
