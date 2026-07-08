#include "leveled_compactor.h"
#include <map>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <iostream>

std::string LeveledCompactor::makeSSTPath(int level, int counter) const {
    std::ostringstream oss;
    oss << data_dir_ << "/sst-" << level << "-" << std::setw(6) << std::setfill('0') << counter << ".sst";
    return oss.str();
}

void LeveledCompactor::run(std::vector<std::unique_ptr<SSTable>>& sstables, int& sst_counter) {
    // Count Level 0 tables
    std::vector<std::string> l0_paths;
    std::vector<std::string> l1_paths;
    
    for (const auto& sst : sstables) {
        std::string filename = std::filesystem::path(sst->path()).filename().string();
        if (filename.rfind("sst-0-", 0) == 0) {
            l0_paths.push_back(sst->path());
        } else if (filename.rfind("sst-1-", 0) == 0) {
            l1_paths.push_back(sst->path());
        }
    }

    if (l0_paths.size() < L0_THRESHOLD) {
        return; // Compaction threshold not met
    }

    std::cout << "[Compaction] Merging " << l0_paths.size() << " L0 tables with "
              << l1_paths.size() << " L1 tables...\n";

    // Merge all active SSTable entries
    // Since sstables are stored in oldest-to-newest order, iterating through them
    // and overwriting keys guarantees that newer updates shadow older ones.
    std::map<std::string, std::optional<std::string>> merged;
    for (const auto& sst : sstables) {
        for (const auto& e : sst->readAll()) {
            merged[e.key] = e.value;
        }
    }

    // Filter out tombstones
    std::vector<SSTable::Entry> entries;
    for (const auto& [k, v] : merged) {
        if (v.has_value()) {
            entries.push_back({k, v});
        }
    }

    // Delete old SSTables physically
    for (const auto& sst : sstables) {
        std::filesystem::remove(sst->path());
    }
    sstables.clear();

    // Split merged entries into non-overlapping partitioned Level 1 tables
    int l1_counter = 0;
    if (!entries.empty()) {
        for (size_t i = 0; i < entries.size(); i += L1_MAX_FILE_ENTRIES) {
            size_t end_idx = std::min(i + L1_MAX_FILE_ENTRIES, entries.size());
            std::vector<SSTable::Entry> sub_entries(entries.begin() + i, entries.begin() + end_idx);

            std::string path = makeSSTPath(1, l1_counter++);
            SSTable::write(path, sub_entries);
            sstables.push_back(std::make_unique<SSTable>(path));
        }
    }

    std::cout << "[Compaction] Compaction finished. " << l1_counter << " Level 1 tables written.\n";
    sst_counter = 0; // Reset L0 counter for subsequent flushes
}
