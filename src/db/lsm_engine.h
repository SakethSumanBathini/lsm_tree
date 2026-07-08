#pragma once
#include "../memtable/skip_list.h"
#include "../wal/wal.h"
#include "../sstable/sstable.h"
#include "../compaction/leveled_compactor.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>

class LSMEngine {
public:
    explicit LSMEngine(const std::string& data_dir, size_t memtable_max_bytes = Memtable::DEFAULT_MAX_BYTES);
    ~LSMEngine();

    static const std::string& ensureDir(const std::string& dir);

    void put(const std::string& key, const std::string& value);
    void del(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;
    void flush();

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
    LeveledCompactor compactor_;

    void flushMemtable();
    void recoverFromWAL();
    void loadSSTables();
    std::string sstPath(int level, int counter) const;
};
