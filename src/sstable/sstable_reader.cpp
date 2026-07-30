#include "lru_block_cache.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace {

static std::vector<SSTable::Entry> parseBlockEntries(const char* data, size_t size) {
    std::vector<SSTable::Entry> entries;
    size_t offset = 0;
    while (offset + 9 <= size) {
        uint8_t is_tombstone = static_cast<uint8_t>(data[offset]);
        offset += 1;

        uint32_t key_len = 0;
        std::memcpy(&key_len, data + offset, 4);
        offset += 4;
        if (offset + key_len + 4 > size) break;

        std::string key(data + offset, key_len);
        offset += key_len;

        uint32_t val_len = 0;
        std::memcpy(&val_len, data + offset, 4);
        offset += 4;
        if (offset + val_len > size) break;

        std::string val(data + offset, val_len);
        offset += val_len;

        SSTable::Entry e;
        e.key = std::move(key);
        e.value = is_tombstone ? std::nullopt : std::optional<std::string>(std::move(val));
        entries.push_back(std::move(e));
    }
    return entries;
}

} // namespace

SSTable::SSTable(const std::string& path) : path_(path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        throw std::runtime_error("SSTable: cannot open: " + path);

    uint64_t file_size = in.tellg();
    if (file_size < 16)
        throw std::runtime_error("SSTable: file too small: " + path);

    in.seekg(-16, std::ios::end);
    uint64_t index_offset = readUint64(in);
    uint64_t bloom_offset = readUint64(in);
    index_offset_ = index_offset;

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

SSTable::~SSTable() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::optional<std::optional<std::string>> SSTable::get(const std::string& key) const {
    if (!bloom_->mayContain(key))
        return std::nullopt;

    if (index_.empty()) return std::nullopt;

    int lo = 0, hi = static_cast<int>(index_.size()) - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (index_[mid].first <= key) lo = mid;
        else hi = mid - 1;
    }

    uint64_t block_start_offset = index_[lo].second;
    uint64_t block_end_offset   = (static_cast<size_t>(lo + 1) < index_.size())
                                  ? index_[lo + 1].second
                                  : index_offset_;

    // 1. Check LRU Block Cache first!
    auto cached_block = BlockCache::globalInstance().get(path_, block_start_offset);
    if (cached_block.has_value()) {
        for (const auto& e : *cached_block) {
            if (e.key == key) return e.value;
            if (e.key > key)  return std::nullopt;
        }
        return std::nullopt;
    }

    // 2. Cache MISS -> Read block via persistent fd_ pread (no open/close syscalls!)
    if (fd_ < 0 || block_end_offset <= block_start_offset) return std::nullopt;
    size_t block_len = block_end_offset - block_start_offset;
    std::string buf(block_len, '\0');
    ssize_t bytes_read = ::pread(fd_, buf.data(), block_len, block_start_offset);
    if (bytes_read <= 0) return std::nullopt;

    std::vector<Entry> entries = parseBlockEntries(buf.data(), static_cast<size_t>(bytes_read));
    std::optional<std::optional<std::string>> result = std::nullopt;
    for (const auto& e : entries) {
        if (e.key == key) {
            result = e.value;
            break;
        }
        if (e.key > key) break;
    }

    // 3. Populate LRU Block Cache
    BlockCache::globalInstance().put(path_, block_start_offset, std::move(entries));

    return result;
}

std::vector<SSTable::Entry> SSTable::readAll() const {
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

SSTableIterator::SSTableIterator(const std::string& path, uint64_t data_end_offset)
    : in_(path, std::ios::binary), data_end_offset_(data_end_offset) {
    advance();
}

void SSTableIterator::advance() {
    if (in_.is_open() && static_cast<uint64_t>(in_.tellg()) < data_end_offset_ && in_.peek() != EOF) {
        uint8_t is_tombstone = 0;
        in_.read(reinterpret_cast<char*>(&is_tombstone), 1);
        uint32_t key_len = 0;
        in_.read(reinterpret_cast<char*>(&key_len), 4);
        std::string key(key_len, '\0');
        in_.read(key.data(), key_len);

        uint32_t val_len = 0;
        in_.read(reinterpret_cast<char*>(&val_len), 4);
        std::string val(val_len, '\0');
        in_.read(val.data(), val_len);

        SSTable::Entry e;
        e.key = std::move(key);
        e.value = is_tombstone ? std::nullopt : std::optional<std::string>(std::move(val));
        current_ = std::move(e);
    } else {
        current_ = std::nullopt;
    }
}

SSTable::Entry SSTableIterator::next() {
    SSTable::Entry res = std::move(*current_);
    advance();
    return res;
}

std::unique_ptr<SSTableIterator> SSTable::createIterator() const {
    return std::make_unique<SSTableIterator>(path_, index_offset_);
}

uint64_t SSTable::findBlock(const std::string& key) const {
    if (index_.empty()) return 0;
    int lo = 0, hi = static_cast<int>(index_.size()) - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (index_[mid].first <= key) lo = mid;
        else hi = mid - 1;
    }
    return index_[lo].second;
}

SSTable::Entry SSTable::readEntry(std::ifstream& in) {
    uint8_t is_tombstone;
    in.read(reinterpret_cast<char*>(&is_tombstone), 1);
    Entry e;
    e.key = readString(in);
    std::string val = readString(in);
    e.value = is_tombstone ? std::nullopt : std::optional<std::string>(val);
    return e;
}

std::string SSTable::readString(std::ifstream& in) {
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), 4);
    std::string s(len, '\0');
    in.read(s.data(), len);
    return s;
}

uint32_t SSTable::readUint32(std::ifstream& in) { uint32_t v = 0; in.read(reinterpret_cast<char*>(&v), 4); return v; }
uint64_t SSTable::readUint64(std::ifstream& in) { uint64_t v = 0; in.read(reinterpret_cast<char*>(&v), 8); return v; }

std::unique_ptr<BloomFilter> SSTable::deserializeBloom(std::ifstream& in) {
    uint64_t num_hashes, num_blocks;
    in.read(reinterpret_cast<char*>(&num_hashes), 8);
    in.read(reinterpret_cast<char*>(&num_blocks), 8);

    // num_blocks comes from the file, and `num_blocks * 64` sized an allocation
    // directly from it. A corrupted or truncated footer could therefore ask for
    // an arbitrary amount of memory before a single byte was validated, and the
    // multiplication itself overflows once num_blocks exceeds 2^58.
    //
    // Bound it by what the file can actually contain: the bloom section runs
    // from here to the start of the 16-byte footer. The comparison is done by
    // division so no product is formed before the check.
    const std::streampos here = in.tellg();
    in.seekg(0, std::ios::end);
    const uint64_t file_size = static_cast<uint64_t>(in.tellg());
    in.seekg(here);

    constexpr uint64_t kFooterBytes = 16;
    constexpr uint64_t kBlockBytes = 64;
    const uint64_t pos = static_cast<uint64_t>(here);
    const uint64_t available = (file_size >= pos + kFooterBytes) ? (file_size - pos - kFooterBytes) : 0;

    if (num_blocks > available / kBlockBytes) {
        throw std::runtime_error("SSTable: bloom filter block count exceeds file contents");
    }

    std::vector<uint8_t> data(num_blocks * kBlockBytes);
    in.read(reinterpret_cast<char*>(data.data()), data.size());
    return std::make_unique<BloomFilter>(BloomFilter::deserialize(num_blocks, num_hashes, data));
}
