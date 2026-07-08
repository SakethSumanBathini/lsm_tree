#pragma once
#include "../sstable/sstable.h"
#include <vector>
#include <string>
#include <memory>

class LeveledCompactor {
public:
    // Compacter configuration
    static constexpr size_t L0_THRESHOLD = 4;
    static constexpr size_t L1_MAX_FILE_ENTRIES = 1000;

    explicit LeveledCompactor(const std::string& data_dir) : data_dir_(data_dir) {}

    // Checks if compaction is needed and performs it if L0 size >= threshold.
    // Updates the active sstables vector.
    void run(std::vector<std::unique_ptr<SSTable>>& sstables, int& sst_counter);

private:
    std::string data_dir_;

    std::string makeSSTPath(int level, int counter) const;
};
