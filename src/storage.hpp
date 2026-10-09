// Storage backends.
//
//   MemoryStore      nodes are heap objects in RAM, nothing on disk
//   DiskStore        every get() = one pread of a page, every put() = one pwrite.
//                    No page is cached by the program.
//   BufferPoolStore  DiskStore + bounded buffer pool: internal nodes pinned,
//                    LRU for leaves, dirty write-back on eviction/flush.
//   HeapFile         append-only payload file (disk variants)
//   MemArena         append-only payload arena in RAM (memory-only variant)
//
// Page layout (page_size bytes, little endian; identical to the Python version):
//   [0]    u8  type (1 leaf, 2 internal)   [1] pad
//   [2]    u16 key count n                 [4] i32 next leaf (-1 none)
//   [8..16) reserved
//   [16]   key area: capacity * key_size (keys zero-padded to key_size)
//   leaf:     capacity * u64 record pointers
//   internal: (capacity + 1) * u32 child page ids
// Page 0 is the meta page.
#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bptree.hpp"

inline void disable_os_cache(int fd) {
#ifdef __APPLE__
    fcntl(fd, F_NOCACHE, 1);
#else
    (void)fd;  // Linux O_DIRECT needs aligned buffers; not used
#endif
}

inline void full_pwrite(int fd, const char* buf, size_t n, uint64_t off) {
    while (n) {
        ssize_t w = ::pwrite(fd, buf, n, off_t(off));
        if (w < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("pwrite: ") + strerror(errno));
        }
        buf += w; n -= size_t(w); off += uint64_t(w);
    }
}

inline size_t full_pread(int fd, char* buf, size_t n, uint64_t off) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::pread(fd, buf + got, n - got, off_t(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("pread: ") + strerror(errno));
        }
        if (r == 0) break;
        got += size_t(r);
    }
    return got;
}

inline int open_file(const std::string& path, bool create, bool direct) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | (create ? O_TRUNC : 0), 0644);
    if (fd < 0) throw std::runtime_error("open " + path + ": " + strerror(errno));
    if (direct) disable_os_cache(fd);
    return fd;
}

constexpr uint32_t HEADER_SIZE = 16;

class PageCodec {
public:
    PageCodec(uint32_t page_size, uint32_t key_size) : ps_(page_size), ks_(key_size) {
        if (key_size == 0) throw std::invalid_argument("key_size must be > 0");
        leaf_cap = int((page_size - HEADER_SIZE) / (key_size + 8));
        internal_cap = int((page_size - HEADER_SIZE - 4) / (key_size + 4));
        if (leaf_cap < 3 || internal_cap < 3) throw std::invalid_argument("page too small for key size");
        leaf_val_off = HEADER_SIZE + uint32_t(leaf_cap) * key_size;
        int_child_off = HEADER_SIZE + uint32_t(internal_cap) * key_size;
    }
    uint32_t page_size() const { return ps_; }
    uint32_t key_size() const { return ks_; }

    void encode(const Node& n, char* buf) const {
        std::memset(buf, 0, ps_);
        uint16_t cnt = uint16_t(n.keys.size());
        buf[0] = n.leaf ? 1 : 2;
        std::memcpy(buf + 2, &cnt, 2);
        std::memcpy(buf + 4, &n.next, 4);
        char* p = buf + HEADER_SIZE;
        for (auto& k : n.keys) {
            if (k.size() != ks_) throw std::logic_error("unpadded key");
            std::memcpy(p, k.data(), ks_);
            p += ks_;
        }
        if (n.leaf) {
            std::memcpy(buf + leaf_val_off, n.vals.data(), cnt * 8);
        } else {
            for (size_t j = 0; j <= cnt; ++j) {
                uint32_t c = uint32_t(n.vals[j]);
                std::memcpy(buf + int_child_off + 4 * j, &c, 4);
            }
        }
    }

    void decode(uint32_t pid, const char* buf, Node& n) const {
        uint16_t cnt;
        std::memcpy(&cnt, buf + 2, 2);
        n.pid = pid;
        n.dirty = false;
        n.keys.resize(cnt);
        const char* p = buf + HEADER_SIZE;
        for (uint16_t j = 0; j < cnt; ++j, p += ks_) n.keys[j].assign(p, ks_);
        if (buf[0] == 1) {
            n.leaf = true;
            std::memcpy(&n.next, buf + 4, 4);
            n.vals.resize(cnt);
            std::memcpy(n.vals.data(), buf + leaf_val_off, cnt * 8);
        } else if (buf[0] == 2) {
            n.leaf = false;
            n.next = NO_PAGE;
            n.vals.resize(size_t(cnt) + 1);
            for (size_t j = 0; j <= cnt; ++j) {
                uint32_t c;
                std::memcpy(&c, buf + int_child_off + 4 * j, 4);
                n.vals[j] = c;
            }
        } else {
            throw std::runtime_error("corrupt page " + std::to_string(pid));
        }
    }

    int leaf_cap, internal_cap;
    uint32_t leaf_val_off, int_child_off;

private:
    uint32_t ps_, ks_;
};

struct Counter { std::string name; long long value; };
struct Ratio { std::string name; double value; };
struct IoStats { std::vector<Counter> ints; std::vector<Ratio> reals; };

// ====================================================================== memory
class MemoryStore {
public:
    using Ref = Node*;
    Ref get(uint32_t pid) { ++visits; return nodes_[pid].get(); }
    Ref new_node(bool leaf) {
        nodes_.emplace_back(new Node());
        Node* n = nodes_.back().get();
        n->pid = uint32_t(nodes_.size() - 1);
        n->leaf = leaf;
        return n;
    }
    void put(const Ref&) {}
    std::optional<Meta> load_meta() { return meta_; }
    void save_meta(const Meta& m) { meta_ = m; }
    long long visits = 0;

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    std::optional<Meta> meta_;
};

// =================================================================== disk only
class DiskStore {
public:
    using Ref = std::shared_ptr<Node>;
    static constexpr char MAGIC[9] = "BPTREE01";

    DiskStore(const std::string& path, uint32_t page_size, uint32_t key_size, bool create, bool direct) {
        fd_ = open_file(path, create, direct);
        if (!create) {
            char m[56];
            if (full_pread(fd_, m, 56, 0) != 56 || std::memcmp(m, MAGIC, 8) != 0)
                throw std::runtime_error(path + " is not a B+ tree index");
            ++reads;
            Meta meta;
            int64_t v[4], pages;
            std::memcpy(&page_size, m + 8, 4);
            std::memcpy(&key_size, m + 12, 4);
            std::memcpy(&meta.root, m + 16, 4);
            std::memcpy(&meta.height, m + 20, 4);
            std::memcpy(v, m + 24, 24);
            std::memcpy(&pages, m + 48, 8);
            meta.size = uint64_t(v[0]); meta.n_leaves = uint64_t(v[1]); meta.n_internal = uint64_t(v[2]);
            meta_ = meta;
            num_pages_ = uint32_t(pages);
        }
        codec_ = std::make_unique<PageCodec>(page_size, key_size);
        page_.resize(page_size);
    }
    virtual ~DiskStore() { close(); }

    Ref get(uint32_t pid) { return read_page(pid); }
    void put(const Ref& n) { write_page(*n); }
    Ref new_node(bool leaf) {
        auto n = std::make_shared<Node>();
        n->pid = num_pages_++;
        n->leaf = leaf;
        return n;
    }
    std::optional<Meta> load_meta() { return meta_; }
    void save_meta(const Meta& m) {
        meta_ = m;
        std::memset(page_.data(), 0, page_.size());
        char* b = page_.data();
        uint32_t ps = codec_->page_size(), ks = codec_->key_size();
        int64_t v[4] = {int64_t(m.size), int64_t(m.n_leaves), int64_t(m.n_internal), int64_t(num_pages_)};
        std::memcpy(b, MAGIC, 8);
        std::memcpy(b + 8, &ps, 4);
        std::memcpy(b + 12, &ks, 4);
        std::memcpy(b + 16, &m.root, 4);
        std::memcpy(b + 20, &m.height, 4);
        std::memcpy(b + 24, v, 32);
        full_pwrite(fd_, b, ps, 0);
        ++writes;
    }
    virtual void flush() {
        if (meta_) save_meta(*meta_);
        ::fsync(fd_);
    }
    void close() {
        if (fd_ >= 0) {
            flush();
            ::close(fd_);
            fd_ = -1;
        }
    }
    const PageCodec& codec() const { return *codec_; }
    virtual IoStats stats() const {
        return {{{"page_reads", reads}, {"page_writes", writes}, {"pages_allocated", num_pages_}}, {}};
    }

    long long reads = 0, writes = 0;

protected:
    Ref read_page(uint32_t pid) {
        ++reads;
        uint32_t ps = codec_->page_size();
        if (full_pread(fd_, page_.data(), ps, uint64_t(pid) * ps) != ps)
            throw std::runtime_error("short read of page " + std::to_string(pid));
        auto n = std::make_shared<Node>();
        codec_->decode(pid, page_.data(), *n);
        return n;
    }
    void write_page(const Node& n) {
        ++writes;
        uint32_t ps = codec_->page_size();
        codec_->encode(n, page_.data());
        full_pwrite(fd_, page_.data(), ps, uint64_t(n.pid) * ps);
    }

    int fd_ = -1;
    uint32_t num_pages_ = 1;  // page 0 = meta
    std::unique_ptr<PageCodec> codec_;
    std::vector<char> page_;
    std::optional<Meta> meta_;
};

// =============================================================== memory + disk
class BufferPoolStore : public DiskStore {
public:
    BufferPoolStore(const std::string& path, uint32_t page_size, uint32_t key_size, bool create, bool direct,
                    size_t pool_pages, bool pin_internal)
        : DiskStore(path, page_size, key_size, create, direct), cap_(pool_pages), pin_(pin_internal) {
        if (pool_pages < 8) throw std::invalid_argument("buffer pool needs >= 8 pages");
        cache_.reserve(pool_pages * 2);
    }
    ~BufferPoolStore() override { close(); }

    Ref get(uint32_t pid) {
        auto p = pinned_.find(pid);
        if (p != pinned_.end()) { ++hits; return p->second; }
        auto c = cache_.find(pid);
        if (c != cache_.end()) {
            ++hits;
            lru_.splice(lru_.end(), lru_, c->second.second);
            return c->second.first;
        }
        ++misses;
        Ref n = read_page(pid);
        admit(n);
        return n;
    }
    void put(const Ref& n) {
        n->dirty = true;
        if (!pinned_.count(n->pid) && !cache_.count(n->pid)) admit(n);
    }
    void flush() override {
        for (auto& kv : pinned_) write_back(*kv.second);
        for (auto& kv : cache_) write_back(*kv.second.first);
        DiskStore::flush();
    }
    IoStats stats() const override {
        IoStats s = DiskStore::stats();
        long long total = hits + misses;
        s.ints.push_back({"pool_capacity_pages", (long long)cap_});
        s.ints.push_back({"pool_hits", hits});
        s.ints.push_back({"pool_misses", misses});
        s.ints.push_back({"pool_evictions", evictions});
        s.ints.push_back({"dirty_writebacks", writebacks});
        s.ints.push_back({"pinned_internal_pages", (long long)pinned_.size()});
        s.ints.push_back({"cached_leaf_pages", (long long)cache_.size()});
        s.reals.push_back({"pool_hit_rate", total ? double(hits) / double(total) : 0.0});
        return s;
    }

    long long hits = 0, misses = 0, evictions = 0, writebacks = 0;

private:
    void write_back(Node& n) {
        if (n.dirty) {
            write_page(n);
            ++writebacks;
            n.dirty = false;
        }
    }
    void admit(const Ref& n) {
        if (pin_ && !n->leaf) {
            pinned_[n->pid] = n;
        } else {
            lru_.push_back(n->pid);
            cache_[n->pid] = {n, std::prev(lru_.end())};
        }
        while (!lru_.empty() && lru_.size() + pinned_.size() > cap_) {
            uint32_t victim = lru_.front();
            lru_.pop_front();
            auto it = cache_.find(victim);
            Ref v = std::move(it->second.first);
            cache_.erase(it);
            ++evictions;
            write_back(*v);  // the object stays alive if the tree still holds it
        }
    }

    size_t cap_;
    bool pin_;
    std::unordered_map<uint32_t, Ref> pinned_;
    std::list<uint32_t> lru_;  // front = least recently used
    std::unordered_map<uint32_t, std::pair<Ref, std::list<uint32_t>::iterator>> cache_;
};

// ================================================================== heap file
// record pointer = (offset << 24) | length  -> one pread per payload
class HeapFile {
public:
    HeapFile(const std::string& path, bool create, bool buffered, bool direct) : buffered_(buffered) {
        fd_ = open_file(path, create, direct);
        struct stat st;
        fstat(fd_, &st);
        size_ = uint64_t(st.st_size);
        buf_start_ = size_;
    }
    ~HeapFile() { close(); }

    uint64_t append(const char* data, size_t n) {
        if (n >= (1u << 24)) throw std::invalid_argument("payload larger than 16 MB");
        uint64_t off = size_;
        if (buffered_) {
            buf_.insert(buf_.end(), data, data + n);
            if (buf_.size() >= (1u << 20)) flush_buf();
        } else {
            full_pwrite(fd_, data, n, off);
            ++writes;
        }
        size_ += n;
        return (off << 24) | n;
    }
    void read(uint64_t ptr, std::string& out) {
        if (!buf_.empty()) flush_buf();
        ++reads;
        size_t n = size_t(ptr & 0xFFFFFF);
        out.resize(n);
        full_pread(fd_, out.data(), n, ptr >> 24);
    }
    void flush() {
        flush_buf();
        ::fsync(fd_);
    }
    void close() {
        if (fd_ >= 0) {
            flush();
            ::close(fd_);
            fd_ = -1;
        }
    }
    long long reads = 0, writes = 0;

private:
    void flush_buf() {
        if (buf_.empty()) return;
        full_pwrite(fd_, buf_.data(), buf_.size(), buf_start_);
        ++writes;
        buf_start_ += buf_.size();
        buf_.clear();
    }
    int fd_ = -1;
    bool buffered_;
    uint64_t size_ = 0, buf_start_ = 0;
    std::vector<char> buf_;
};

// ================================================================== mem arena
class MemArena {
public:
    static constexpr size_t BLOCK = size_t(64) << 20;
    uint64_t append(const std::string& s) {
        size_t n = s.size();
        if (n >= (1u << 24)) throw std::invalid_argument("payload larger than 16 MB");
        if (blocks_.empty() || pos_ + n > BLOCK) {
            blocks_.emplace_back(new char[BLOCK]);
            pos_ = 0;
        }
        uint64_t off = uint64_t(blocks_.size() - 1) * BLOCK + pos_;
        std::memcpy(blocks_.back().get() + pos_, s.data(), n);
        pos_ += n;
        used_ += n;
        return (off << 24) | n;
    }
    std::string_view get(uint64_t ptr) const {
        uint64_t off = ptr >> 24;
        return {blocks_[off / BLOCK].get() + off % BLOCK, size_t(ptr & 0xFFFFFF)};
    }
    size_t used() const { return used_; }

private:
    std::vector<std::unique_ptr<char[]>> blocks_;
    size_t pos_ = 0, used_ = 0;
};
