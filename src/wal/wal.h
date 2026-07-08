#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <liburing.h>

// WAL (Write-Ahead Log) using Linux io_uring for Asynchronous Zero-Copy Logging.
class WAL {
public:
    struct Entry {
        enum class Type { PUT = 0x01, DEL = 0x02 };
        Type        type;
        std::string key;
        std::string value;
    };

    explicit WAL(const std::string& path);
    ~WAL();

    void logPut(const std::string& key, const std::string& value);
    void logDel(const std::string& key);
    std::vector<Entry> recover() const;
    void clear();

private:
    int fd_;
    std::string path_;
    struct io_uring ring_;
    mutable std::mutex mu_;

    void writeEntry(Entry::Type type, const std::string& key, const std::string& value);
    void reap();
    static uint32_t crc32(uint8_t type, const std::string& key, const std::string& value);
};
