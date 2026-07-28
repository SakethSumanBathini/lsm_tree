#pragma once
#include <vector>
#include <mutex>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Arena: simple thread-safe bump-pointer allocator for Lock-Free SkipList nodes.
// Memory is reclaimed entirely when the Arena (and Memtable) is destroyed.
class Arena {
public:
    explicit Arena(size_t block_size = 1024 * 1024)
        : block_size_(block_size), offset_(0), current_block_size_(block_size) {
        current_block_ = new char[block_size_];
        blocks_.push_back(current_block_);
    }

    ~Arena() {
        for (char* b : blocks_) delete[] b;
    }

    // Returns `bytes` of storage aligned to `alignment`.
    //
    // The bump pointer previously advanced by the raw request size with no
    // rounding, so the address handed back was only ever correctly aligned by
    // accident of the sizes involved. Callers placing types with an alignment
    // requirement — Node holds std::atomic<Node*> — depended on every prior
    // allocation happening to be a multiple of that alignment.
    void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
        std::lock_guard<std::mutex> lock(mu_);

        size_t pad = padFor(current_block_ + offset_, alignment);

        if (offset_ + pad + bytes > current_block_size_) {
            // A fresh block: size it for the payload plus worst-case padding.
            size_t next_size = std::max(block_size_, bytes + alignment);
            current_block_ = new char[next_size];
            blocks_.push_back(current_block_);
            // current_block_size_ tracks the block actually in hand. The old
            // code compared against block_size_ even after handing out an
            // oversized block, so the bound no longer described the block.
            current_block_size_ = next_size;
            offset_ = 0;
            pad = padFor(current_block_, alignment);
        }

        void* result = current_block_ + offset_ + pad;
        offset_ += pad + bytes;
        return result;
    }

private:
    static size_t padFor(const char* p, size_t alignment) {
        size_t rem = reinterpret_cast<uintptr_t>(p) % alignment;
        return rem ? alignment - rem : 0;
    }

    size_t block_size_;
    size_t offset_;
    size_t current_block_size_;
    char* current_block_;
    std::vector<char*> blocks_;
    std::mutex mu_; // protects all allocations
};
