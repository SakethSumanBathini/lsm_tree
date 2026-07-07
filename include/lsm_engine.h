#pragma once
#include "memtable.h"
#include "wal.h"
#include "sstable.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <map>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

// LSMEngine: the top-level key-value store.
class LSMEngine {
public:
    explicit LSMEngine(const std::string& data_dir, size_t memtable_max_bytes = Memtable::DEFAULT_MAX_BYTES)
        : data_dir_(data_dir)
        , memtable_max_bytes_(memtable_max_bytes)
        , wal_(ensureDir(data_dir) + "/wal.log")
        , memtable_(std::make_unique<Memtable>(memtable_max_bytes))
    {
        // Acquire exclusive file lock to prevent multiple engines on the same directory
        lock_fd_ = open((data_dir + "/LOCK").c_str(), O_CREAT | O_RDWR, 0644);
        if (lock_fd_ < 0 || flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            if (lock_fd_ >= 0) close(lock_fd_);
            throw std::runtime_error("LSMEngine: failed to acquire lock on " + data_dir);
        }

        recoverFromWAL();
        loadSSTables();
    }

    ~LSMEngine() {
        if (lock_fd_ >= 0) {
            flock(lock_fd_, LOCK_UN);
            close(lock_fd_);
        }
    }

    static const std::string& ensureDir(const std::string& dir) {
        std::filesystem::create_directories(dir);
        return dir;
    }

    void put(const std::string& key, const std::string& value) {
        wal_.logPut(key, value);
        memtable_->put(key, value);
        if (memtable_->isFull()) flushMemtable();
    }

    void del(const std::string& key) {
        wal_.logDel(key);
        memtable_->del(key);
        if (memtable_->isFull()) flushMemtable();
    }

    std::optional<std::string> get(const std::string& key) const {
        auto mem_result = memtable_->get(key);
        if (mem_result.has_value()) return mem_result.value();

        for (int i = static_cast<int>(sstables_.size()) - 1; i >= 0; --i) {
            auto sst_result = sstables_[i]->get(key);
            if (sst_result.has_value()) return sst_result.value();
        }
        return std::nullopt;
    }

    void flush() {
        if (!memtable_->empty()) flushMemtable();
    }

    size_t memtableSize()  const { return memtable_->size(); }
    size_t sstableCount()  const { return sstables_.size(); }

private:
    std::string data_dir_;
    size_t memtable_max_bytes_;
    WAL         wal_;
    std::unique_ptr<Memtable> memtable_;
    std::vector<std::unique_ptr<SSTable>> sstables_;
    int sst_counter_ = 0;
    int lock_fd_ = -1;

    void flushMemtable() {
        if (memtable_->empty()) return;

        std::vector<SSTable::Entry> entries;
        entries.reserve(memtable_->size());
        for (auto it = memtable_->begin(); it != memtable_->end(); ++it)
            entries.push_back({it->first, it->second});

        std::string path = sstPath(sst_counter_++);
        SSTable::write(path, entries);
        sstables_.push_back(std::make_unique<SSTable>(path));

        // SSTable::write() fsyncs the file, so it's safe to clear the WAL now.
        wal_.clear();
        memtable_ = std::make_unique<Memtable>(memtable_max_bytes_);
        if (sstables_.size() >= 4) compact();
    }

    void compact() {
        // Size-tiered compaction: merge all SSTables into one
        std::map<std::string, std::optional<std::string>> merged;
        for (auto& sst : sstables_) {
            for (auto& e : sst->readAll()) {
                merged[e.key] = e.value; // newer SSTables overwrite older
            }
        }
        // Remove tombstones and build entries
        std::vector<SSTable::Entry> entries;
        for (auto& [k, v] : merged) {
            if (v.has_value()) entries.push_back({k, v});
        }
        // Delete old files
        for (auto& sst : sstables_) {
            std::filesystem::remove(sst->path());
        }
        sstables_.clear();
        if (!entries.empty()) {
            sst_counter_ = 0;
            std::string path = sstPath(sst_counter_++);
            SSTable::write(path, entries);
            sstables_.push_back(std::make_unique<SSTable>(path));
        }
    }

    void recoverFromWAL() {
        auto entries = wal_.recover();
        for (auto& e : entries) {
            if (e.type == WAL::Entry::Type::PUT)
                memtable_->put(e.key, e.value);
            else
                memtable_->del(e.key);
        }
    }

    void loadSSTables() {
        std::vector<std::filesystem::path> sst_files;
        for (auto& entry : std::filesystem::directory_iterator(data_dir_)) {
            if (entry.path().extension() == ".sst")
                sst_files.push_back(entry.path());
        }
        std::sort(sst_files.begin(), sst_files.end());
        for (auto& path : sst_files) {
            sstables_.push_back(std::make_unique<SSTable>(path.string()));
            sst_counter_ = std::max(sst_counter_, std::stoi(path.stem().string().substr(4)) + 1);
        }
    }

    std::string sstPath(int n) const {
        std::ostringstream oss;
        oss << data_dir_ << "/sst-" << std::setw(6) << std::setfill('0') << n << ".sst";
        return oss.str();
    }
};
