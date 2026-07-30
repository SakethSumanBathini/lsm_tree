#pragma once
#include "sstable.h"
#include <unordered_map>
#include <list>
#include <mutex>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>

class BlockCache {
public:
    struct CacheKey {
        std::string sstable_path;
        uint64_t block_offset;

        bool operator==(const CacheKey& other) const {
            return block_offset == other.block_offset && sstable_path == other.sstable_path;
        }
    };

    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& k) const {
            return std::hash<std::string>()(k.sstable_path) ^ (std::hash<uint64_t>()(k.block_offset) << 1);
        }
    };

    explicit BlockCache(size_t capacity_blocks = 1024) : capacity_(capacity_blocks) {}

    std::optional<std::vector<SSTable::Entry>> get(const std::string& path, uint64_t block_offset) {
        std::lock_guard<std::mutex> lock(mu_);
        CacheKey key{path, block_offset};
        auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;
        }
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return it->second->value;
    }

    void put(const std::string& path, uint64_t block_offset, std::vector<SSTable::Entry> entries) {
        std::lock_guard<std::mutex> lock(mu_);
        CacheKey key{path, block_offset};
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->value = std::move(entries);
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return;
        }

        if (map_.size() >= capacity_) {
            auto last = lru_list_.end();
            --last;
            map_.erase(last->key);
            lru_list_.pop_back();
        }

        lru_list_.push_front({key, std::move(entries)});
        map_[key] = lru_list_.begin();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        map_.clear();
        lru_list_.clear();
    }

    static BlockCache& globalInstance() {
        static BlockCache instance(1024);
        return instance;
    }

private:
    struct Node {
        CacheKey key;
        std::vector<SSTable::Entry> value;
    };

    mutable std::mutex mu_;
    size_t capacity_;
    std::list<Node> lru_list_;
    std::unordered_map<CacheKey, std::list<Node>::iterator, CacheKeyHash> map_;
};
