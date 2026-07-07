#pragma once
#include "bloom_filter.h"
#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

// SSTable (Sorted String Table): immutable sorted file on disk.
class SSTable {
public:
    // ENTRIES_PER_BLOCK: number of entries per block for the sparse index.
    // Every ENTRIES_PER_BLOCK-th entry is recorded in the index for binary search.
    // (Named to avoid collision with the Linux kernel BLOCK_SIZE macro.)
    static constexpr size_t ENTRIES_PER_BLOCK = 64;

    struct Entry {
        std::string key;
        std::optional<std::string> value;
    };

    static void write(const std::string& path, const std::vector<Entry>& entries) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            throw std::runtime_error("SSTable: cannot create file: " + path);

        BloomFilter bloom(std::max(entries.size(), size_t(1)));
        std::vector<std::pair<std::string, uint64_t>> index;

        for (size_t i = 0; i < entries.size(); ++i) {
            if (i % ENTRIES_PER_BLOCK == 0)
                index.push_back({entries[i].key, static_cast<uint64_t>(out.tellp())});

            bloom.insert(entries[i].key);
            writeEntry(out, entries[i]);
        }

        uint64_t index_offset = static_cast<uint64_t>(out.tellp());
        uint32_t index_count  = static_cast<uint32_t>(index.size());
        writeUint32(out, index_count);
        for (auto& [key, offset] : index) {
            writeString(out, key);
            writeUint64(out, offset);
        }

        uint64_t bloom_offset = static_cast<uint64_t>(out.tellp());
        serializeBloom(out, bloom);

        writeUint64(out, index_offset);
        writeUint64(out, bloom_offset);
        out.flush();
        out.close();

        // fsync the file to ensure durability before the WAL is cleared.
        // Without this, a crash after WAL clear but before OS writeback
        // could lose the SSTable data.
        int fd = open(path.c_str(), O_RDONLY);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }

    explicit SSTable(const std::string& path) : path_(path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open())
            throw std::runtime_error("SSTable: cannot open: " + path);

        uint64_t file_size = in.tellg();
        if (file_size < 16)
            throw std::runtime_error("SSTable: file too small: " + path);

        in.seekg(-16, std::ios::end);
        uint64_t index_offset = readUint64(in);
        uint64_t bloom_offset = readUint64(in);

        in.seekg(index_offset);
        uint32_t index_count = readUint32(in);
        index_.resize(index_count);
        for (auto& [key, offset] : index_) {
            key    = readString(in);
            offset = readUint64(in);
        }

        in.seekg(bloom_offset);
        bloom_ = deserializeBloom(in);

        if (!index_.empty()) {
            smallest_key_ = index_.front().first;
            in.seekg(index_.back().second);
            Entry e;
            while (static_cast<uint64_t>(in.tellg()) < index_offset)
                e = readEntry(in);
            largest_key_ = e.key;
        }
    }

    std::optional<std::optional<std::string>> get(const std::string& key) const {
        if (!bloom_->mayContain(key))
            return std::nullopt;

        uint64_t block_offset = findBlock(key);
        std::ifstream in(path_, std::ios::binary);
        if (!in.is_open()) return std::nullopt;

        in.seekg(block_offset);
        for (size_t i = 0; i < ENTRIES_PER_BLOCK && in.peek() != EOF; ++i) {
            Entry e = readEntry(in);
            if (e.key == key) return e.value;
            if (e.key > key)  return std::nullopt;
        }
        return std::nullopt;
    }

    // Read all entries from the data section of this SSTable.
    // Used by compaction to merge SSTables.
    std::vector<Entry> readAll() const {
        std::ifstream in(path_, std::ios::binary);
        if (!in.is_open()) return {};
        std::vector<Entry> entries;
        // Read the index_offset from the footer
        in.seekg(-16, std::ios::end);
        uint64_t idx_off = readUint64(in);
        in.seekg(0);
        while (static_cast<uint64_t>(in.tellg()) < idx_off && in.peek() != EOF) {
            entries.push_back(readEntry(in));
        }
        return entries;
    }

    const std::string& smallestKey() const { return smallest_key_; }
    const std::string& largestKey()  const { return largest_key_; }
    const std::string& path()        const { return path_; }

private:
    std::string path_;
    std::vector<std::pair<std::string, uint64_t>> index_;
    std::unique_ptr<BloomFilter> bloom_;
    std::string smallest_key_;
    std::string largest_key_;

    uint64_t findBlock(const std::string& key) const {
        if (index_.empty()) return 0;
        int lo = 0, hi = static_cast<int>(index_.size()) - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (index_[mid].first <= key) lo = mid;
            else hi = mid - 1;
        }
        return index_[lo].second;
    }

    static void writeEntry(std::ofstream& out, const Entry& e) {
        uint8_t is_tombstone = e.value.has_value() ? 0 : 1;
        out.write(reinterpret_cast<const char*>(&is_tombstone), 1);
        writeString(out, e.key);
        writeString(out, e.value.value_or(""));
    }

    static Entry readEntry(std::ifstream& in) {
        uint8_t is_tombstone;
        in.read(reinterpret_cast<char*>(&is_tombstone), 1);
        Entry e;
        e.key = readString(in);
        std::string val = readString(in);
        e.value = is_tombstone ? std::nullopt : std::optional<std::string>(val);
        return e;
    }

    static void writeString(std::ofstream& out, const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        out.write(reinterpret_cast<const char*>(&len), 4);
        out.write(s.data(), len);
    }

    static std::string readString(std::ifstream& in) {
        uint32_t len = 0;
        in.read(reinterpret_cast<char*>(&len), 4);
        std::string s(len, '\0');
        in.read(s.data(), len);
        return s;
    }

    static void writeUint32(std::ofstream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }
    static uint32_t readUint32(std::ifstream& in) { uint32_t v = 0; in.read(reinterpret_cast<char*>(&v), 4); return v; }
    static void writeUint64(std::ofstream& out, uint64_t v) { out.write(reinterpret_cast<const char*>(&v), 8); }
    static uint64_t readUint64(std::ifstream& in) { uint64_t v = 0; in.read(reinterpret_cast<char*>(&v), 8); return v; }

    static void serializeBloom(std::ofstream& out, const BloomFilter& bf) {
        uint64_t num_hashes = static_cast<uint64_t>(bf.hashCount());
        uint64_t num_blocks = static_cast<uint64_t>(bf.blockCount());
        out.write(reinterpret_cast<const char*>(&num_hashes), 8);
        out.write(reinterpret_cast<const char*>(&num_blocks), 8);
        auto data = bf.serialize();
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    static std::unique_ptr<BloomFilter> deserializeBloom(std::ifstream& in) {
        uint64_t num_hashes, num_blocks;
        in.read(reinterpret_cast<char*>(&num_hashes), 8);
        in.read(reinterpret_cast<char*>(&num_blocks), 8);
        std::vector<uint8_t> data(num_blocks * 64);
        in.read(reinterpret_cast<char*>(data.data()), data.size());
        return std::make_unique<BloomFilter>(BloomFilter::deserialize(num_blocks, num_hashes, data));
    }
};
