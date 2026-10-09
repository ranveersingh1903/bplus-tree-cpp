// The three variants with a common API:
//   insert(key, payload) / search(key, out) / range_scan(lo, limit, out)
//   flush() / close() / io_stats() / structure() / validate() / disk bytes
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bptree.hpp"
#include "storage.hpp"

namespace fs = std::filesystem;
using Rows = std::vector<std::pair<std::string, std::string>>;

inline std::pair<int, int> default_caps(uint32_t page_size, uint32_t key_size) {
    PageCodec c(page_size, key_size);
    return {c.leaf_cap, c.internal_cap};
}

// ------------------------------------------------------------- memory only
class MemoryTree {
public:
    static constexpr bool kPersistent = false;
    MemoryTree(int leaf_cap, int internal_cap) : tree_(store_, leaf_cap, internal_cap) {}

    bool insert(const std::string& k, const std::string& p) { return tree_.insert(k, arena_.append(p)); }
    bool search(const std::string& k, std::string& out) {
        auto v = tree_.search(k);
        if (!v) return false;
        out.assign(arena_.get(*v));
        return true;
    }
    void range_scan(const std::string& lo, size_t limit, Rows& out) {
        tree_.range_scan(lo, limit, tmp_);
        out.clear();
        for (auto& kv : tmp_) out.emplace_back(kv.first, std::string(arena_.get(kv.second)));
    }
    void flush() {}
    void close() {}
    IoStats io_stats() const { return {{{"node_visits", store_.visits}}, {}}; }
    BPlusTree<MemoryStore>& tree() { return tree_; }
    uint64_t index_bytes() const { return 0; }
    uint64_t heap_bytes() const { return 0; }
    long long warm_up() { return 0; }
    uint32_t key_size() const { return 0; }

private:
    MemoryStore store_;
    MemArena arena_;
    BPlusTree<MemoryStore> tree_;
    std::vector<std::pair<std::string, uint64_t>> tmp_;
};

// ---------------------------------------------------- disk only / hybrid
template <class Store>
class DiskTree {
public:
    static constexpr bool kPersistent = true;

    template <class... Extra>
    DiskTree(const std::string& dir, uint32_t key_size, uint32_t page_size, bool create, bool direct,
             bool heap_buffered, Extra&&... extra)
        : index_path_(dir + "/index.db"), heap_path_(dir + "/heap.db") {
        fs::create_directories(dir);
        store_ = std::make_unique<Store>(index_path_, page_size, key_size, create, direct,
                                         std::forward<Extra>(extra)...);
        ks_ = store_->codec().key_size();
        heap_ = std::make_unique<HeapFile>(heap_path_, create, heap_buffered, direct);
        tree_ = std::make_unique<BPlusTree<Store>>(*store_, store_->codec().leaf_cap,
                                                   store_->codec().internal_cap);
    }
    ~DiskTree() { close(); }

    bool insert(const std::string& k, const std::string& p) {
        if (!pad(k)) throw std::invalid_argument("key longer than key_size: " + k);
        return tree_->insert(kbuf_, heap_->append(p.data(), p.size()));
    }
    bool search(const std::string& k, std::string& out) {
        if (!pad(k)) return false;  // longer than any stored key
        auto v = tree_->search(kbuf_);
        if (!v) return false;
        heap_->read(*v, out);
        return true;
    }
    void range_scan(const std::string& lo, size_t limit, Rows& out) {
        out.clear();
        if (!pad(lo)) return;
        tree_->range_scan(kbuf_, limit, tmp_);
        for (auto& kv : tmp_) {
            std::string v;
            heap_->read(kv.second, v);
            size_t z = kv.first.find('\0');
            out.emplace_back(z == std::string::npos ? kv.first : kv.first.substr(0, z), std::move(v));
        }
    }
    void flush() {
        tree_->save_meta();
        store_->flush();
        heap_->flush();
    }
    void close() {
        if (closed_) return;
        tree_->save_meta();
        store_->close();
        heap_->close();
        closed_ = true;
    }
    IoStats io_stats() const {
        IoStats s = store_->stats();
        s.ints.push_back({"heap_reads", heap_->reads});
        s.ints.push_back({"heap_writes", heap_->writes});
        return s;
    }
    // load every internal node (useful for the buffer pool: they get pinned)
    long long warm_up() {
        long long before = store_->reads;
        std::vector<uint32_t> level{tree_->root()}, next;
        for (int h = 0; h < tree_->height() - 1; ++h) {
            next.clear();
            for (uint32_t pid : level) {
                auto n = store_->get(pid);
                for (auto c : n->vals) next.push_back(uint32_t(c));
            }
            level.swap(next);
        }
        return store_->reads - before;
    }
    BPlusTree<Store>& tree() { return *tree_; }
    uint64_t index_bytes() const { return fs::file_size(index_path_); }
    uint64_t heap_bytes() const { return fs::file_size(heap_path_); }
    uint32_t key_size() const { return ks_; }

private:
    bool pad(const std::string& k) {
        if (k.size() > ks_) return false;
        kbuf_.assign(k);
        kbuf_.resize(ks_, '\0');
        return true;
    }
    std::string index_path_, heap_path_;
    uint32_t ks_ = 0;
    std::unique_ptr<Store> store_;
    std::unique_ptr<HeapFile> heap_;
    std::unique_ptr<BPlusTree<Store>> tree_;
    std::string kbuf_;
    std::vector<std::pair<std::string, uint64_t>> tmp_;
    bool closed_ = false;
};

using DiskOnlyTree = DiskTree<DiskStore>;
using HybridTree = DiskTree<BufferPoolStore>;
