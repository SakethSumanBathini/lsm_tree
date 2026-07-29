#include "lsm_engine.h"
#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>

struct SSTFile {
    std::filesystem::path path;
    int level;
    int counter;
};

LSMEngine::LSMEngine(const std::string& data_dir, size_t memtable_max_bytes)
    : data_dir_(data_dir)
    , memtable_max_bytes_(memtable_max_bytes)
    , wal_(ensureDir(data_dir) + "/wal.log")
    , memtable_(std::make_unique<Memtable>(memtable_max_bytes))
    , compactor_(data_dir)
{
    // Acquire exclusive flock
    lock_fd_ = open((data_dir + "/LOCK").c_str(), O_CREAT | O_RDWR, 0644);
    if (lock_fd_ < 0 || flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        if (lock_fd_ >= 0) close(lock_fd_);
        throw std::runtime_error("LSMEngine: failed to acquire lock on " + data_dir);
    }

    recoverFromWAL();
    loadSSTables();
}

LSMEngine::~LSMEngine() {
    if (lock_fd_ >= 0) {
        flock(lock_fd_, LOCK_UN);
        close(lock_fd_);
    }
}

const std::string& LSMEngine::ensureDir(const std::string& dir) {
    std::filesystem::create_directories(dir);
    return dir;
}

void LSMEngine::put(const std::string& key, const std::string& value) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    wal_.logPut(key, value);
    memtable_->put(key, value);
    if (memtable_->isFull()) {
        flushMemtable();
    }
}

void LSMEngine::del(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    wal_.logDel(key);
    memtable_->del(key);
    if (memtable_->isFull()) {
        flushMemtable();
    }
}

std::optional<std::string> LSMEngine::get(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mu_);

    // 1. Search in MemTable
    auto mem_result = memtable_->get(key);
    if (mem_result.has_value()) {
        return mem_result.value();
    }

    // 2. Search in SSTables (newest to oldest: from back of vector to front)
    for (int i = static_cast<int>(sstables_.size()) - 1; i >= 0; --i) {
        auto sst_result = sstables_[i]->get(key);
        if (sst_result.has_value()) {
            return sst_result.value();
        }
    }
    return std::nullopt;
}

void LSMEngine::flush() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (!memtable_->empty()) {
        flushMemtable();
    }
}

void LSMEngine::flushMemtable() {
    if (memtable_->empty()) return;

    std::vector<SSTable::Entry> entries;
    entries.reserve(memtable_->size());
    for (auto it = memtable_->begin(); it != memtable_->end(); ++it) {
        entries.push_back({it->first, it->second});
    }

    // Level 0 files get sst-0-xxxxxx.sst path
    std::string path = sstPath(0, sst_counter_++);
    SSTable::write(path, entries);
    sstables_.push_back(std::make_unique<SSTable>(path));

    // Build the replacement memtable before discarding the log.
    //
    // The order used to be clear-then-construct. make_unique can throw
    // bad_alloc, and if it did the log was already gone while the old memtable
    // still held every entry written since the last flush — live in RAM, backed
    // by nothing. A crash at that point lost all of it, even though the data had
    // been durably logged moments earlier.
    //
    // Allocating first means the only step after wal_.clear() is a move
    // assignment of a unique_ptr, which cannot throw. The SSTable above is
    // already written and registered, so the entries are durable before the log
    // that describes them is dropped.
    auto replacement = std::make_unique<Memtable>(memtable_max_bytes_);
    wal_.clear();
    memtable_ = std::move(replacement);

    // Run leveled compaction if necessary
    compactor_.run(sstables_, sst_counter_);
}

void LSMEngine::recoverFromWAL() {
    auto entries = wal_.recover();
    for (auto& e : entries) {
        if (e.type == WAL::Entry::Type::PUT)
            memtable_->put(e.key, e.value);
        else
            memtable_->del(e.key);
    }
}

void LSMEngine::loadSSTables() {
    std::vector<SSTFile> sst_files;
    for (auto& entry : std::filesystem::directory_iterator(data_dir_)) {
        if (entry.path().extension() == ".sst") {
            std::string filename = entry.path().filename().string();
            // Parse filename sst-<level>-<counter>.sst
            if (filename.rfind("sst-", 0) == 0 && filename.size() >= 15) {
                try {
                    int level = std::stoi(filename.substr(4, 1));
                    int counter = std::stoi(filename.substr(6, 6));
                    sst_files.push_back({entry.path(), level, counter});
                } catch (...) {
                    // Ignore malformed files
                }
            }
        }
    }

    // Sort: Level 1 comes BEFORE Level 0; within levels sort by counter ascending.
    std::sort(sst_files.begin(), sst_files.end(), [](const SSTFile& a, const SSTFile& b) {
        if (a.level != b.level) {
            return a.level > b.level;
        }
        return a.counter < b.counter;
    });

    for (auto& f : sst_files) {
        sstables_.push_back(std::make_unique<SSTable>(f.path.string()));
        if (f.level == 0) {
            sst_counter_ = std::max(sst_counter_, f.counter + 1);
        }
    }
}

std::string LSMEngine::sstPath(int level, int counter) const {
    std::ostringstream oss;
    oss << data_dir_ << "/sst-" << level << "-" << std::setw(6) << std::setfill('0') << counter << ".sst";
    return oss.str();
}
