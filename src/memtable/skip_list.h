#pragma once
#include "arena.h"
#include <atomic>
#include <string>
#include <optional>
#include <vector>
#include <random>
#include <cstring>
#include <mutex>

// Lock-Free SkipList Node
struct Node {
    std::string key;
    std::optional<std::string> value;
    int height;
    std::atomic<Node*> next[1]; // Flexible array member (at least 1)

    static Node* create(Arena& arena, const std::string& k, const std::optional<std::string>& v, int h) {
        size_t size = sizeof(Node) + (h - 1) * sizeof(std::atomic<Node*>);
        void* mem = arena.allocate(size);
        Node* n = new (mem) Node();
        n->key = k;
        n->value = v;
        n->height = h;
        for (int i = 0; i < h; ++i) n->next[i].store(nullptr, std::memory_order_relaxed);
        return n;
    }
};

// Simple Lock-Free SkipList (Insert-only for Memtable usage)
class SkipList {
public:
    static constexpr int MAX_HEIGHT = 12;

    explicit SkipList(Arena& arena) : arena_(arena), head_(Node::create(arena_, "", std::nullopt, MAX_HEIGHT)) {}

    void insert(const std::string& key, const std::optional<std::string>& value) {
        Node* preds[MAX_HEIGHT];
        Node* succs[MAX_HEIGHT];
        
        while (true) {
            if (find(key, preds, succs)) {
                // Key already exists — update the value in-place.
                // This is safe for MemTable usage: the engine is single-writer per key
                // in practice, and the memtable is swapped atomically on flush.
                // A direct store avoids inserting duplicate nodes, which would cause
                // find() to return stale data (the first/oldest match).
                succs[0]->value = value;
                return;
            }

            int height = randomHeight();
            Node* newNode = Node::create(arena_, key, value, height);

            for (int i = 0; i < height; ++i) {
                newNode->next[i].store(succs[i], std::memory_order_relaxed);
            }

            // Lock-free insertion at level 0
            if (preds[0]->next[0].compare_exchange_strong(succs[0], newNode)) {
                // Successfully inserted at level 0. Now link higher levels.
                for (int i = 1; i < height; ++i) {
                    while (true) {
                        Node* pred = preds[i];
                        Node* succ = succs[i];
                        newNode->next[i].store(succ, std::memory_order_relaxed);
                        if (pred->next[i].compare_exchange_strong(succ, newNode)) break;
                        // If CAS fails, we need to re-find neighbors for this level
                        find(key, preds, succs);
                    }
                }
                return;
            }
            // If level 0 CAS fails, someone else inserted. Retry entire operation.
        }
    }

    bool find(const std::string& key, Node** preds, Node** succs) const {
        Node* x = head_;
        for (int i = MAX_HEIGHT - 1; i >= 0; --i) {
            Node* next = x->next[i].load(std::memory_order_acquire);
            while (next != nullptr && next->key < key) {
                x = next;
                next = x->next[i].load(std::memory_order_acquire);
            }
            preds[i] = x;
            succs[i] = next;
        }
        return (succs[0] != nullptr && succs[0]->key == key);
    }

    Node* getHead() const { return head_; }

private:
    Arena& arena_;
    Node* head_;

    int randomHeight() {
        // Thread-safe: each thread gets its own RNG instance
        thread_local static std::mt19937 rng{std::random_device{}()};
        int h = 1;
        while (h < MAX_HEIGHT && (rng() % 4 == 0)) h++;
        return h;
    }
};

class Memtable {
public:
    static constexpr size_t DEFAULT_MAX_BYTES = 4 * 1024 * 1024; // 4 MiB

    explicit Memtable(size_t max_bytes = DEFAULT_MAX_BYTES)
        : max_bytes_(max_bytes), current_bytes_(0), list_(arena_) {}

    void put(const std::string& key, const std::string& value) {
        list_.insert(key, value);
        current_bytes_.fetch_add(key.size() + value.size() + 8, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    void del(const std::string& key) {
        list_.insert(key, std::nullopt);
        current_bytes_.fetch_add(key.size() + 8, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    std::optional<std::optional<std::string>> get(const std::string& key) const {
        Node* preds[SkipList::MAX_HEIGHT];
        Node* succs[SkipList::MAX_HEIGHT];
        if (list_.find(key, preds, succs)) {
            return succs[0]->value;
        }
        return std::nullopt;
    }

    bool isFull() const { return current_bytes_.load(std::memory_order_relaxed) >= max_bytes_; }
    size_t size() const { return count_.load(std::memory_order_relaxed); }
    size_t byteSize() const { return current_bytes_.load(std::memory_order_relaxed); }
    bool empty() const { return size() == 0; }

    // Iterator for flushing (sequential)
    class Iterator {
    public:
        using value_type = std::pair<std::string, std::optional<std::string>>;
        explicit Iterator(Node* n) : current_(n) { updateCache(); }
        bool operator!=(const Iterator& other) const { return current_ != other.current_; }
        void operator++() { 
            if (current_) current_ = current_->next[0].load(std::memory_order_acquire);
            updateCache();
        }
        const value_type& operator*() const { return cache_; }
        const value_type* operator->() const { return &cache_; }
    private:
        Node* current_;
        value_type cache_;
        void updateCache() {
            if (current_) cache_ = {current_->key, current_->value};
        }
    };

    Iterator begin() const { return Iterator(list_.getHead()->next[0].load(std::memory_order_acquire)); }
    Iterator end() const { return Iterator(nullptr); }

private:
    size_t max_bytes_;
    std::atomic<size_t> current_bytes_{0};
    std::atomic<size_t> count_{0};
    Arena arena_;
    SkipList list_;
};
