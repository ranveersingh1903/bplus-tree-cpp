// Randomised correctness tests against std::map. Tiny 256 B pages force
// many splits (height >= 4); tiny buffer pools force evictions/write-backs.
//   make test
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>

#include "../src/trees.hpp"

namespace fs = std::filesystem;
static int failures = 0, checks = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) { ++failures; std::cerr << "  FAIL: " << msg << "\n"; } \
    } while (0)

static std::map<std::string, std::string> make_data(size_t n, uint64_t seed) {
    static const char A[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::mt19937_64 r(seed);
    std::map<std::string, std::string> m;
    while (m.size() < n) {
        std::string k(3 + r() % 8, ' '), v(r() % 41, ' ');
        for (auto& c : k) c = A[r() % 62];
        for (auto& c : v) c = A[r() % 62];
        m.emplace(k, "p_" + v);
    }
    return m;
}

static long long counter(const IoStats& s, const std::string& name) {
    for (auto& c : s.ints) if (c.name == name) return c.value;
    return -1;
}

template <class Tree>
static void exercise(const char* name, Tree& t, const std::map<std::string, std::string>& data,
                     const std::vector<std::string>& order) {
    std::cerr << "[" << name << "]\n";
    size_t new_ok = 0;
    for (auto& k : order) new_ok += t.insert(k, data.at(k));
    CHECK(new_ok == data.size(), "every insert reported as new");
    std::string out;
    size_t ok = 0;
    for (auto& [k, v] : data) ok += t.search(k, out) && out == v;
    CHECK(ok == data.size(), "all keys found (" << ok << "/" << data.size() << ")");
    size_t miss_ok = 0;
    for (auto& kv : data) {
        std::string m = kv.first;
        m.back() = '#';
        miss_ok += !t.search(m, out);
    }
    CHECK(miss_ok == data.size(), "missing keys not found");
    try {
        auto v = t.tree().validate();
        CHECK(v.records == data.size(), "validate count");
    } catch (std::exception& e) {
        CHECK(false, e.what());
    }
    CHECK(t.tree().height() >= 4, "tall tree (height " << t.tree().height() << ")");
    std::vector<std::string> sorted;
    for (auto& kv : data) sorted.push_back(kv.first);
    Rows rows;
    std::mt19937 r(3);
    bool range_ok = true;
    for (int q = 0; q < 300 && range_ok; ++q) {
        size_t i = r() % sorted.size();
        t.range_scan(sorted[i], 50, rows);
        size_t expect = std::min<size_t>(50, sorted.size() - i);
        if (rows.size() != expect) range_ok = false;
        for (size_t j = 0; j < rows.size() && range_ok; ++j)
            if (rows[j].first != sorted[i + j] || rows[j].second != data.at(sorted[i + j])) range_ok = false;
    }
    CHECK(range_ok, "range scans match sorted order");
    CHECK(!t.insert(order[0], "NEW"), "duplicate returns false");
    CHECK(t.search(order[0], out) && out == "NEW", "duplicate overwrites");
}

int main() {
    auto data = make_data(20000, 1);
    std::vector<std::string> order;
    for (auto& kv : data) order.push_back(kv.first);
    std::shuffle(order.begin(), order.end(), std::mt19937(7));
    const uint32_t PS = 256, KS = 10;
    auto [lc, ic] = default_caps(PS, KS);
    std::string dir = (fs::temp_directory_path() / "bptree_cpp_test").string();
    std::string out;

    {
        MemoryTree t(lc, ic);
        exercise("memory", t, data, order);
    }

    {  // disk only
        fs::remove_all(dir);
        auto t = std::make_unique<DiskOnlyTree>(dir, KS, PS, true, false, false);
        exercise("disk", *t, data, order);
        long long before = counter(t->io_stats(), "page_reads");
        t->search(order[1], out);
        CHECK(counter(t->io_stats(), "page_reads") - before == t->tree().height(), "disk search reads = height");
        t->close();
        t.reset();
        DiskOnlyTree re(dir, 0, PS, false, false, false);
        size_t ok = 0;
        for (size_t i = 1; i < 3000; ++i) ok += re.search(order[i], out) && out == data.at(order[i]);
        CHECK(ok == 2999, "persistent after reopen");
        try { re.tree().validate(); } catch (std::exception& e) { CHECK(false, e.what()); }
        CHECK(re.insert("zzzzzzzzzz", "after") && re.search("zzzzzzzzzz", out) && out == "after",
              "insert after reopen");
        bool threw = false;
        try { re.insert("xxxxxxxxxxx", "v"); } catch (std::invalid_argument&) { threw = true; }
        CHECK(threw, "key longer than key_size rejected");
    }

    for (bool pinned : {true, false}) {
        fs::remove_all(dir);
        size_t pool = pinned ? 16 : 8;
        auto open = [&](bool create) {
            return std::make_unique<HybridTree>(dir, KS, PS, create, false, true, pool, pinned);
        };
        auto t = open(true);
        exercise(pinned ? "hybrid (pinned internals, 16 pages)" : "hybrid (plain LRU, 8 pages)", *t, data, order);
        CHECK(counter(t->io_stats(), "pool_evictions") > 0, "evictions happened");
        CHECK(counter(t->io_stats(), "dirty_writebacks") > 0, "write-backs happened");
        t->close();
        t.reset();
        auto re = open(false);
        if (pinned) {
            re->warm_up();
            long long before = counter(re->io_stats(), "page_reads");
            re->search(order[5], out);
            CHECK(counter(re->io_stats(), "page_reads") - before <= 1, "pinned search reads <= 1 page");
        }
        size_t ok = 0;
        for (size_t i = 1; i < 3000; ++i) ok += re->search(order[i], out) && out == data.at(order[i]);
        CHECK(ok == 2999, "persistent after reopen");
        try { re->tree().validate(); } catch (std::exception& e) { CHECK(false, e.what()); }
    }

    fs::remove_all(dir);
    std::cerr << "\n" << (checks - failures) << "/" << checks << " checks passed\n";
    return failures ? 1 : 0;
}
