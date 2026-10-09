// Benchmark ONE B+ tree variant. Writes results/<label>.json in the same
// format as the Python version, so make_report.py turns it into the report.
//
//   ./bptree_bench --tree memory --csv data.csv
//   ./bptree_bench --tree disk   --csv data.csv
//   ./bptree_bench --tree hybrid --csv data.csv --pool-mb 128
//
// Phases: profile CSV -> BUILD (only insert() is timed) -> FLUSH -> REOPEN
// from disk (cold) -> SEARCH cold + warm (hits, misses, ranges) -> VERIFY
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <unordered_set>

#include "trees.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

struct Args {
    std::string tree, csv, key_col = "key", payload_col = "payload";
    std::string storage_dir = "storage", results_dir = "results", label;
    long long limit = 0;
    uint32_t page_size = 4096, key_size = 0;
    int leaf_cap = 0, internal_cap = 0;
    double pool_mb = 128;
    bool pin_internal = true, direct_io = false, validate = false, keep_files = false;
    size_t hits = 100000, misses = 10000, ranges = 1000, range_len = 100;
    size_t chunk = 100000, checkpoint = 1000000;
    uint64_t seed = 42;
};

static void usage() {
    std::cerr <<
        "usage: bptree_bench --tree memory|disk|hybrid --csv FILE [options]\n"
        "  --limit N            use first N records (0 = all)\n"
        "  --page-size B        node/page size (default 4096)\n"
        "  --key-size B         fixed key slot (default = longest key)\n"
        "  --leaf-cap N --internal-cap N   override memory-tree capacities\n"
        "  --pool-mb M          hybrid buffer pool size (default 128)\n"
        "  --no-pin-internal    hybrid: plain LRU for all pages\n"
        "  --direct-io          bypass OS page cache (macOS F_NOCACHE)\n"
        "  --hits N --misses N --ranges N --range-len N\n"
        "  --validate           full structural check after build\n"
        "  --label NAME --results-dir DIR --storage-dir DIR --keep-files --seed S\n";
    std::exit(2);
}

static Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto val = [&]() -> std::string { if (i + 1 >= argc) usage(); return argv[++i]; };
        if (k == "--tree") a.tree = val();
        else if (k == "--csv") a.csv = val();
        else if (k == "--key-col") a.key_col = val();
        else if (k == "--payload-col") a.payload_col = val();
        else if (k == "--limit") a.limit = std::stoll(val());
        else if (k == "--page-size") a.page_size = uint32_t(std::stoul(val()));
        else if (k == "--key-size") a.key_size = uint32_t(std::stoul(val()));
        else if (k == "--leaf-cap") a.leaf_cap = std::stoi(val());
        else if (k == "--internal-cap") a.internal_cap = std::stoi(val());
        else if (k == "--pool-mb") a.pool_mb = std::stod(val());
        else if (k == "--no-pin-internal") a.pin_internal = false;
        else if (k == "--direct-io") a.direct_io = true;
        else if (k == "--validate") a.validate = true;
        else if (k == "--keep-files") a.keep_files = true;
        else if (k == "--hits") a.hits = std::stoul(val());
        else if (k == "--misses") a.misses = std::stoul(val());
        else if (k == "--ranges") a.ranges = std::stoul(val());
        else if (k == "--range-len") a.range_len = std::stoul(val());
        else if (k == "--chunk") a.chunk = std::stoul(val());
        else if (k == "--checkpoint") a.checkpoint = std::stoul(val());
        else if (k == "--seed") a.seed = std::stoull(val());
        else if (k == "--label") a.label = val();
        else if (k == "--results-dir") a.results_dir = val();
        else if (k == "--storage-dir") a.storage_dir = val();
        else usage();
    }
    if (a.tree != "memory" && a.tree != "disk" && a.tree != "hybrid") usage();
    if (a.csv.empty()) usage();
    if (a.label.empty()) a.label = a.tree;
    return a;
}

// ------------------------------------------------------------ dataset profile
struct Profile {
    size_t rows = 0, ki = 0, pi = 1, max_key = 0;
    double avg_key = 0, avg_payload = 0;
    uint64_t csv_bytes = 0;
};

static std::string lower_trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t b = 0;
    while (b < s.size() && isspace((unsigned char)s[b])) ++b;
    s = s.substr(b);
    for (auto& c : s) c = char(tolower((unsigned char)c));
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF) s = s.substr(3);  // UTF-8 BOM
    return s;
}

static Profile profile_csv(const Args& a) {
    Profile p;
    p.csv_bytes = fs::file_size(a.csv);
    CsvReader r(a.csv);
    std::vector<std::string> f;
    if (!r.next(f)) throw std::runtime_error("empty CSV");
    bool found_k = false, found_p = false;
    for (size_t j = 0; j < f.size(); ++j) {
        std::string h = lower_trim(f[j]);
        if (h == a.key_col) { p.ki = j; found_k = true; }
        if (h == a.payload_col) { p.pi = j; found_p = true; }
    }
    if (!found_k || !found_p) {
        std::cerr << "[warn] header has no '" << a.key_col << "'/'" << a.payload_col
                  << "' column; using column 0 = key, 1 = payload\n";
        p.ki = 0; p.pi = 1;
    }
    double sk = 0, sp = 0;
    size_t need = std::max(p.ki, p.pi);
    while (r.next(f)) {
        if (f.size() <= need) continue;
        ++p.rows;
        sk += double(f[p.ki].size());
        sp += double(f[p.pi].size());
        p.max_key = std::max(p.max_key, f[p.ki].size());
    }
    p.avg_key = p.rows ? sk / double(p.rows) : 0;
    p.avg_payload = p.rows ? sp / double(p.rows) : 0;
    return p;
}

// ------------------------------------------------------------ searches
using Queries = std::vector<std::pair<std::string, std::string>>;

static uint64_t fnv(uint64_t h, const std::string& s) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

template <class Tree>
static Json point_queries(Tree& t, const Queries& q, bool expect_found) {
    std::vector<long long> lat;
    lat.reserve(q.size());
    std::string out;
    long long correct = 0;
    for (auto& [k, exp] : q) {
        auto t0 = Clock::now();
        bool found = t.search(k, out);
        lat.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        if (expect_found ? (found && out == exp) : !found) ++correct;
    }
    Json j = latency_summary(lat);
    j["correct"] = correct;
    return j;
}

template <class Tree>
static Json range_queries(Tree& t, const std::vector<std::string>& starts, size_t len) {
    std::vector<long long> lat;
    Rows out;
    uint64_t h = 1469598103934665603ULL;
    long long rows = 0;
    bool ordered = true;
    for (auto& lo : starts) {
        auto t0 = Clock::now();
        t.range_scan(lo, len, out);
        lat.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        rows += (long long)out.size();
        if (!out.empty() && out[0].first < lo) ordered = false;
        for (size_t j = 0; j < out.size(); ++j) {
            if (j && !(out[j - 1].first < out[j].first)) ordered = false;
            h = fnv(fnv(h, out[j].first), "\x01" + out[j].second);
        }
    }
    Json j = latency_summary(lat);
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    j["rows_returned"] = rows;
    j["sorted_ok"] = ordered;
    j["digest"] = std::string(hex);
    return j;
}

template <class Tree>
static Json search_phase(Tree& t, const Queries& hits, const Queries& misses,
                         const std::vector<std::string>& starts, size_t len) {
    Json out = Json::object();
    for (int kind = 0; kind < 3; ++kind) {
        IoStats a = t.io_stats();
        Json r = kind == 0 ? point_queries(t, hits, true)
               : kind == 1 ? point_queries(t, misses, false)
                           : range_queries(t, starts, len);
        IoStats d = io_delta(a, t.io_stats());
        double n = kind == 0 ? double(hits.size()) : kind == 1 ? double(misses.size()) : double(starts.size());
        r["io"] = io_json(d);
        r["io_per_op"] = per_op(d, n);
        out[kind == 0 ? "point_hit" : kind == 1 ? "point_miss" : "range"] = r;
    }
    return out;
}

template <class Tree>
static Json structure_json(Tree& t, const Args& a) {
    Structure s = t.tree().structure();
    Json j = Json::object();
    j["height"] = s.height;
    j["records"] = s.records;
    j["leaf_nodes"] = s.leaf_nodes;
    j["internal_nodes"] = s.internal_nodes;
    j["total_nodes"] = s.leaf_nodes + s.internal_nodes;
    j["leaf_capacity"] = s.leaf_cap;
    j["internal_capacity"] = s.internal_cap;
    j["max_fanout"] = s.internal_cap + 1;
    j["leaf_fill_factor"] = s.leaf_nodes ? double(s.records) / (double(s.leaf_nodes) * s.leaf_cap) : 0.0;
    if (Tree::kPersistent) {
        j["key_size_bytes"] = t.key_size();
        j["page_size_bytes"] = a.page_size;
    }
    return j;
}

// ------------------------------------------------------------ main runner
template <class Tree, class Make>
static int run(const Args& a, const Profile& prof, uint32_t key_size, int leaf_cap, int internal_cap,
               size_t pool_pages, Make make_tree) {
    size_t n_total = a.limit ? std::min<size_t>(size_t(a.limit), prof.rows) : prof.rows;
    std::string workdir = a.storage_dir + "/" + a.label;

    std::cerr << "\n=== " << a.label << ": " << n_total << " records | page " << a.page_size
              << " B | key slot " << key_size << " B | leaf cap " << leaf_cap << " | internal cap "
              << internal_cap;
    if (a.tree == "hybrid") std::cerr << " | pool " << a.pool_mb << " MB (" << pool_pages << " pages)";
    std::cerr << " ===\n";

    // query sample: same seed -> same queries for every variant
    std::mt19937_64 rng(a.seed);
    size_t n_hits = std::min(a.hits, n_total);
    std::vector<size_t> idx;
    {
        std::unordered_set<size_t> pick;
        pick.reserve(n_hits * 2);
        std::uniform_int_distribution<size_t> U(0, n_total - 1);
        while (pick.size() < n_hits) pick.insert(U(rng));
        idx.assign(pick.begin(), pick.end());
        std::sort(idx.begin(), idx.end());
    }

    long long rss_start = rss_bytes();
    std::unique_ptr<Tree> tree = make_tree(true);

    // -------------------------------------------------------------- BUILD
    CsvReader reader(a.csv);
    std::vector<std::string> f;
    reader.next(f);  // header
    Rows chunk;
    chunk.reserve(a.chunk);
    Queries hits;
    hits.reserve(n_hits);
    Json checkpoints = Json::array();
    double insert_s = 0;
    size_t done = 0, sp = 0, next_cp = a.checkpoint;
    size_t need = std::max(prof.ki, prof.pi);
    auto wall0 = Clock::now();
    while (done < n_total) {
        chunk.clear();
        size_t want = std::min(a.chunk, n_total - done);
        while (chunk.size() < want && reader.next(f))
            if (f.size() > need) chunk.emplace_back(std::move(f[prof.ki]), std::move(f[prof.pi]));
        if (chunk.empty()) break;
        size_t end = done + chunk.size();
        while (sp < idx.size() && idx[sp] < end) hits.push_back(chunk[idx[sp++] - done]);

        auto t0 = Clock::now();
        for (auto& kv : chunk) tree->insert(kv.first, kv.second);
        insert_s += seconds_since(t0);
        done = end;

        if (done >= next_cp || done == n_total) {
            Json cp = Json::object();
            cp["records"] = done;
            cp["insert_s"] = insert_s;
            cp["height"] = tree->tree().height();
            cp["rss_bytes"] = rss_bytes();
            cp["io"] = io_json(tree->io_stats());
            checkpoints.push(cp);
            double rate = insert_s > 0 ? double(done) / insert_s : 0;
            std::cerr << "  " << std::setw(11) << done << " inserted | " << std::setw(10) << human_time(insert_s)
                      << " | " << std::fixed << std::setprecision(0) << rate << " ins/s | height "
                      << tree->tree().height() << " | RSS " << human_bytes(double(rss_bytes()));
            if (done < n_total && rate > 0) std::cerr << " | ETA " << human_time(double(n_total - done) / rate);
            std::cerr << "\n";
            next_cp = (done / a.checkpoint + 1) * a.checkpoint;
        }
    }
    if (done < n_total) {
        std::cerr << "[warn] CSV ended after " << done << " rows\n";
        n_total = done;
    }
    chunk.clear();
    chunk.shrink_to_fit();

    auto t0 = Clock::now();
    tree->flush();
    double flush_s = seconds_since(t0);
    IoStats build_io = tree->io_stats();
    Json structure = structure_json(*tree, a);
    uint64_t idx_bytes = tree->index_bytes(), heap_bytes = tree->heap_bytes();
    long long rss_after_build = rss_bytes();
    std::cerr << "  build done: insert " << human_time(insert_s) << " + flush " << human_time(flush_s)
              << " | height " << tree->tree().height() << " | disk " << human_bytes(double(idx_bytes + heap_bytes))
              << "\n";

    Json validation;
    if (a.validate) {
        t0 = Clock::now();
        ValidationResult v = tree->tree().validate();
        validation = Json::object();
        validation["ok"] = true;
        validation["records"] = v.records;
        validation["leaves"] = v.leaves;
        validation["internal"] = v.internal;
        validation["seconds"] = seconds_since(t0);
        std::cerr << "  validate(): OK\n";
    }

    // -------------------------------------------------------------- REOPEN
    Json reopen = Json::object();
    long long rss_after_reopen = 0;
    if (Tree::kPersistent) {
        tree->close();
        tree.reset();
        t0 = Clock::now();
        tree = make_tree(false);
        reopen["open_s"] = seconds_since(t0);
        if (a.tree == "hybrid") {
            t0 = Clock::now();
            reopen["warmup_pages_read"] = tree->warm_up();
            reopen["warmup_s"] = seconds_since(t0);
        }
        rss_after_reopen = rss_bytes();
        reopen["rss_after_reopen"] = rss_after_reopen;
    }

    // -------------------------------------------------------------- SEARCH
    std::shuffle(hits.begin(), hits.end(), rng);
    Queries misses;
    for (size_t j = 0; j < std::min(a.misses, hits.size()); ++j) {
        std::string k = hits[j].first;
        k.back() = '#';  // '#' never occurs in alphanumeric keys
        misses.emplace_back(k, "");
    }
    std::vector<std::string> starts;
    for (size_t j = 0; j < std::min(a.ranges, hits.size()); ++j) starts.push_back(hits[j].first);

    Json search = Json::object();
    for (const char* pass : {"cold", "warm"}) {
        Json s = search_phase(*tree, hits, misses, starts, a.range_len);
        search[pass] = s;
    }
    IoStats final_io = tree->io_stats();
    long long rss_end = rss_bytes();
    tree->close();

    // -------------------------------------------------------------- RESULT
    Json r = Json::object();
    r["label"] = a.label;
    r["tree"] = a.tree;
    r["records"] = n_total;

    Json cfg = Json::object();
    cfg["page_size"] = a.page_size;
    cfg["key_size"] = key_size;
    cfg["leaf_capacity"] = leaf_cap;
    cfg["internal_capacity"] = internal_cap;
    cfg["pool_mb"] = a.tree == "hybrid" ? Json(a.pool_mb) : Json();
    cfg["pool_pages"] = a.tree == "hybrid" ? Json(pool_pages) : Json();
    cfg["pin_internal"] = a.tree == "hybrid" ? Json(a.pin_internal) : Json();
    cfg["direct_io"] = a.direct_io;
    cfg["seed"] = a.seed;
    cfg["hits"] = hits.size();
    cfg["misses"] = misses.size();
    cfg["ranges"] = starts.size();
    cfg["range_len"] = a.range_len;
    r["config"] = cfg;

    Json ds = Json::object();
    ds["csv"] = fs::absolute(a.csv).string();
    ds["csv_bytes"] = prof.csv_bytes;
    ds["rows"] = prof.rows;
    ds["max_key_bytes"] = prof.max_key;
    ds["avg_key_bytes"] = prof.avg_key;
    ds["avg_payload_bytes"] = prof.avg_payload;
    r["dataset"] = ds;
    r["system"] = system_info();

    Json build = Json::object();
    build["insert_s"] = insert_s;
    build["flush_s"] = flush_s;
    build["total_s"] = insert_s + flush_s;
    build["wall_s_including_csv"] = seconds_since(wall0);
    build["inserts_per_s"] = insert_s > 0 ? double(n_total) / insert_s : 0.0;
    build["avg_insert_us"] = n_total ? insert_s / double(n_total) * 1e6 : 0.0;
    build["io"] = io_json(build_io);
    build["io_per_insert"] = per_op(build_io, double(n_total));
    build["checkpoints"] = checkpoints;
    r["build"] = build;
    r["structure"] = structure;

    Json mem = Json::object();
    mem["rss_start"] = rss_start;
    mem["rss_after_build"] = rss_after_build;
    mem["build_delta"] = rss_after_build - rss_start;
    mem["rss_end"] = rss_end;
    mem["peak_rss"] = peak_rss_bytes();
    if (Tree::kPersistent) mem["rss_after_reopen"] = rss_after_reopen;
    r["memory"] = mem;

    Json disk = Json::object();
    disk["index_bytes"] = idx_bytes;
    disk["heap_bytes"] = heap_bytes;
    disk["total_bytes"] = idx_bytes + heap_bytes;
    r["disk"] = disk;
    r["reopen"] = reopen;
    r["search"] = search;
    r["final_io"] = io_json(final_io);
    r["validation"] = validation;

    fs::create_directories(a.results_dir);
    std::string out = a.results_dir + "/" + a.label + ".json";
    std::ofstream fo(out);
    r.dump(fo);
    fo << "\n";
    std::cerr << "  saved " << out << "\n";
    if (Tree::kPersistent && !a.keep_files) fs::remove_all(workdir);
    return 0;
}

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    Args a = parse(argc, argv);
    try {
        auto t0 = Clock::now();
        std::cerr << "profiling " << a.csv << " ...\n";
        Profile prof = profile_csv(a);
        std::cerr << "  " << prof.rows << " rows, key max " << prof.max_key << " B, payload avg "
                  << std::setprecision(1) << std::fixed << prof.avg_payload << " B (" << human_time(seconds_since(t0))
                  << ")\n";
        if (prof.rows == 0) throw std::runtime_error("no data rows");

        uint32_t ks = a.key_size ? a.key_size : uint32_t(prof.max_key);
           std::pair<int, int> caps = default_caps(a.page_size, ks);
           int lc = caps.first, ic = caps.second;
        if (a.tree == "memory") {
            if (a.leaf_cap) lc = a.leaf_cap;
            if (a.internal_cap) ic = a.internal_cap;
        }
        size_t pool_pages = std::max<size_t>(8, size_t(a.pool_mb * 1024 * 1024 / a.page_size));
        std::string dir = a.storage_dir + "/" + a.label;
        if (a.tree != "memory") fs::remove_all(dir);

        if (a.tree == "memory")
            return run<MemoryTree>(a, prof, ks, lc, ic, 0, [&](bool) {
                return std::make_unique<MemoryTree>(lc, ic);
            });
        if (a.tree == "disk")
            return run<DiskOnlyTree>(a, prof, ks, lc, ic, 0, [&](bool create) {
                return std::make_unique<DiskOnlyTree>(dir, ks, a.page_size, create, a.direct_io, false);
            });
        return run<HybridTree>(a, prof, ks, lc, ic, pool_pages, [&](bool create) {
            return std::make_unique<HybridTree>(dir, ks, a.page_size, create, a.direct_io, true, pool_pages,
                                                a.pin_internal);
        });
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
