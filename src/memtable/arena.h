#pragma once
#include <vector>
#include <mutex>
#include <algorithm>

// Arena: simple thread-safe bump-pointer allocator for Lock-Free SkipList nodes.
// Memory is reclaimed entirely when the Arena (and Memtable) is destroyed.
class Arena {
public:
    explicit Arena(size_t block_size = 1024 * 1024) : block_size_(block_size), offset_(0) {
        current_block_ = new char[block_size_];
        blocks_.push_back(current_block_);
    }

    ~Arena() {
        for (char* b : blocks_) delete[] b;
    }

    void* allocate(size_t bytes) {
        std::lock_guard<std::mutex> lock(mu_);
        if (offset_ + bytes > block_size_) {
            size_t next_size = std::max(block_size_, bytes);
            current_block_ = new char[next_size];
            blocks_.push_back(current_block_);
            offset_ = 0;
        }
        void* result = current_block_ + offset_;
        offset_ += bytes;
        return result;
    }

private:
    size_t block_size_;
    size_t offset_;
    char* current_block_;
    std::vector<char*> blocks_;
    std::mutex mu_; // protects all allocations
};
