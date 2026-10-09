// B+ Tree core algorithm.
//
// Written ONCE, templated over a Store. The three variants (memory-only,
// disk-only, memory+disk) differ only in the Store, so the benchmark compares
// storage strategies, not three different algorithms.
//
// A Store provides:
//   using Ref = ...;                 handle to a node (Node* or shared_ptr<Node>)
//   Ref  get(uint32_t pid);          fetch node
//   Ref  new_node(bool leaf);        allocate node / page id
//   void put(const Ref&);            node was modified
//   std::optional<Meta> load_meta(); void save_meta(const Meta&);
//
// Leaf:     keys[i] -> vals[i] (8-byte value / record pointer), next = right sibling
// Internal: vals = child page ids, vals.size() == keys.size() + 1,
//           child i holds keys k with keys[i-1] <= k < keys[i]
#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

constexpr int32_t NO_PAGE = -1;

struct Node {
    uint32_t pid = 0;
    bool leaf = true;
    bool dirty = false;
    int32_t next = NO_PAGE;
    std::vector<std::string> keys;
    std::vector<uint64_t> vals;
};

struct Meta {
    uint32_t root = 0;
    int32_t height = 1;
    uint64_t size = 0, n_leaves = 0, n_internal = 0;
};

struct Structure {
    int height;
    uint64_t records, leaf_nodes, internal_nodes;
    int leaf_cap, internal_cap;
};

struct ValidationResult {
    uint64_t records, leaves, internal;
};

template <class Store>
class BPlusTree {
public:
    using Ref = typename Store::Ref;

    BPlusTree(Store& store, int leaf_cap, int internal_cap)
        : s_(store), leaf_cap_(leaf_cap), internal_cap_(internal_cap) {
        if (leaf_cap < 3 || internal_cap < 3) throw std::invalid_argument("capacity must be >= 3");
        if (auto m = s_.load_meta()) {
            root_ = m->root; height_ = m->height; size_ = m->size;
            n_leaves_ = m->n_leaves; n_internal_ = m->n_internal;
        } else {
            Ref r = s_.new_node(true);
            s_.put(r);
            root_ = r->pid; height_ = 1; size_ = 0; n_leaves_ = 1; n_internal_ = 0;
            save_meta();
        }
    }

    void save_meta() { s_.save_meta(Meta{root_, height_, size_, n_leaves_, n_internal_}); }

    // Insert key -> val. Returns true if new, false if the key existed (value replaced).
    bool insert(const std::string& key, uint64_t val) {
        path_.clear();
        Ref node = s_.get(root_);
        while (!node->leaf) {
            int i = int(std::upper_bound(node->keys.begin(), node->keys.end(), key) - node->keys.begin());
            path_.emplace_back(node, i);
            node = s_.get(uint32_t(node->vals[i]));
        }
        auto& keys = node->keys;
        auto it = std::lower_bound(keys.begin(), keys.end(), key);
        size_t i = size_t(it - keys.begin());
        if (it != keys.end() && *it == key) {
            node->vals[i] = val;
            s_.put(node);
            return false;
        }
        keys.insert(it, key);
        node->vals.insert(node->vals.begin() + i, val);
        ++size_;
        if (int(keys.size()) <= leaf_cap_) {
            s_.put(node);
            return true;
        }

        // ---- leaf split
        Ref right = s_.new_node(true);
        size_t mid = keys.size() / 2;
        right->keys.assign(std::make_move_iterator(keys.begin() + mid), std::make_move_iterator(keys.end()));
        right->vals.assign(node->vals.begin() + mid, node->vals.end());
        keys.resize(mid);
        node->vals.resize(mid);
        right->next = node->next;
        node->next = int32_t(right->pid);
        s_.put(node);
        s_.put(right);
        ++n_leaves_;
        std::string sep = right->keys[0];
        uint32_t right_pid = right->pid;

        // ---- propagate upward
        while (!path_.empty()) {
            Ref parent = path_.back().first;
            int pi = path_.back().second;
            path_.pop_back();
            auto& pk = parent->keys;
            auto& pv = parent->vals;
            pk.insert(pk.begin() + pi, sep);
            pv.insert(pv.begin() + pi + 1, right_pid);
            if (int(pk.size()) <= internal_cap_) {
                s_.put(parent);
                return true;
            }
            size_t m = pk.size() / 2;
            sep = pk[m];
            Ref nw = s_.new_node(false);
            nw->keys.assign(std::make_move_iterator(pk.begin() + m + 1), std::make_move_iterator(pk.end()));
            nw->vals.assign(pv.begin() + m + 1, pv.end());
            pk.resize(m);
            pv.resize(m + 1);
            s_.put(parent);
            s_.put(nw);
            ++n_internal_;
            right_pid = nw->pid;
        }

        // ---- root split: tree grows one level
        Ref nr = s_.new_node(false);
        nr->keys = {sep};
        nr->vals = {root_, right_pid};
        s_.put(nr);
        root_ = nr->pid;
        ++height_;
        ++n_internal_;
        save_meta();
        return true;
    }

    std::optional<uint64_t> search(const std::string& key) {
        Ref node = s_.get(root_);
        while (!node->leaf) {
            auto& k = node->keys;
            node = s_.get(uint32_t(node->vals[std::upper_bound(k.begin(), k.end(), key) - k.begin()]));
        }
        auto& k = node->keys;
        auto it = std::lower_bound(k.begin(), k.end(), key);
        if (it != k.end() && *it == key) return node->vals[it - k.begin()];
        return std::nullopt;
    }

    // keys >= lo (and <= hi if given), at most `limit`, following the leaf chain
    void range_scan(const std::string& lo, size_t limit, std::vector<std::pair<std::string, uint64_t>>& out,
                    const std::string* hi = nullptr) {
        out.clear();
        Ref node = s_.get(root_);
        while (!node->leaf) {
            auto& k = node->keys;
            node = s_.get(uint32_t(node->vals[std::upper_bound(k.begin(), k.end(), lo) - k.begin()]));
        }
        size_t i = size_t(std::lower_bound(node->keys.begin(), node->keys.end(), lo) - node->keys.begin());
        for (;;) {
            for (; i < node->keys.size(); ++i) {
                if (hi && node->keys[i] > *hi) return;
                out.emplace_back(node->keys[i], node->vals[i]);
                if (limit && out.size() >= limit) return;
            }
            if (node->next == NO_PAGE) return;
            node = s_.get(uint32_t(node->next));
            i = 0;
        }
    }

    Structure structure() const {
        return Structure{height_, size_, n_leaves_, n_internal_, leaf_cap_, internal_cap_};
    }

    // Checks every B+ tree invariant; throws std::runtime_error on violation.
    ValidationResult validate() {
        struct Item { uint32_t pid; const std::string* lo; const std::string* hi; int depth; Ref hold_lo, hold_hi; };
        auto fail = [](const std::string& m) { throw std::runtime_error("B+ tree invalid: " + m); };
        std::vector<Item> stack{{root_, nullptr, nullptr, 1, Ref(), Ref()}};
        std::vector<std::pair<uint32_t, int32_t>> leaves;
        uint64_t count = 0, internal = 0;
        int min_leaf = (leaf_cap_ + 1) / 2, min_int = internal_cap_ / 2;
        while (!stack.empty()) {
            Item it = stack.back();
            stack.pop_back();
            Ref n = s_.get(it.pid);
            auto& k = n->keys;
            for (size_t j = 1; j < k.size(); ++j)
                if (!(k[j - 1] < k[j])) fail("keys not sorted in node " + std::to_string(it.pid));
            if (!k.empty()) {
                if (it.lo && k.front() < *it.lo) fail("key below bound");
                if (it.hi && !(k.back() < *it.hi)) fail("key above bound");
            }
            if (n->leaf) {
                if (it.depth != height_) fail("leaf at wrong depth");
                if (k.size() != n->vals.size()) fail("leaf keys/vals mismatch");
                if (int(k.size()) > leaf_cap_) fail("leaf overflow");
                if (it.pid != root_ && int(k.size()) < min_leaf) fail("leaf underflow");
                leaves.emplace_back(it.pid, n->next);
                count += k.size();
            } else {
                ++internal;
                if (n->vals.size() != k.size() + 1) fail("bad child count");
                if (int(k.size()) > internal_cap_) fail("internal overflow");
                if (it.pid != root_ && int(k.size()) < min_int) fail("internal underflow");
                for (size_t j = n->vals.size(); j-- > 0;) {
                    const std::string* lo = j == 0 ? it.lo : &k[j - 1];
                    const std::string* hi = j == k.size() ? it.hi : &k[j];
                    // keep the node alive while its keys are referenced as bounds
                    stack.push_back({uint32_t(n->vals[j]), lo, hi, it.depth + 1, j == 0 ? it.hold_lo : n,
                                     j == k.size() ? it.hold_hi : n});
                }
            }
        }
        for (size_t j = 1; j < leaves.size(); ++j)
            if (leaves[j - 1].second != int32_t(leaves[j].first)) fail("leaf chain broken");
        if (leaves.back().second != NO_PAGE) fail("last leaf has next");
        if (count != size_) fail("record count mismatch");
        if (leaves.size() != n_leaves_) fail("leaf counter mismatch");
        if (internal != n_internal_) fail("internal counter mismatch");
        return {count, leaves.size(), internal};
    }

    uint32_t root() const { return root_; }
    int height() const { return height_; }
    uint64_t size() const { return size_; }

private:
    Store& s_;
    int leaf_cap_, internal_cap_;
    uint32_t root_ = 0;
    int32_t height_ = 1;
    uint64_t size_ = 0, n_leaves_ = 0, n_internal_ = 0;
    std::vector<std::pair<Ref, int>> path_;
};
