// Turns results/*.json into results/REPORT.md + results/plots/*.svg
//   ./build/bptree_report results
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

#include "json.hpp"
#include "util.hpp"

namespace fs = std::filesystem;
using Rs = std::vector<const Json*>;

// ------------------------------------------------------------- formatting
static std::string fx(double v, int prec = 2) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}
static std::string commas(long long v) {
    std::string s = std::to_string(v < 0 ? -v : v), out;
    int c = 0;
    for (size_t i = s.size(); i-- > 0;) {
        out.insert(out.begin(), s[i]);
        if (++c % 3 == 0 && i) out.insert(out.begin(), ',');
    }
    return v < 0 ? "-" + out : out;
}
static std::string us(double v) { return fx(v) + " µs"; }
static std::string ht(double s) {
    if (s < 1e-3) return fx(s * 1e6, 1) + " µs";
    if (s < 1) return fx(s * 1e3, 1) + " ms";
    if (s < 120) return fx(s, 2) + " s";
    return fx(s / 60, 1) + " min";
}
static std::string hb(double b) { return human_bytes(b); }

static const char* name_of(const std::string& t) {
    return t == "memory" ? "Memory-only" : t == "disk" ? "Disk-only" : "Memory + Disk";
}
static std::string title(const Json& r) {
    std::string t = name_of(r["tree"].str());
    if (r["tree"].str() == "hybrid") {
        double mb = r["config"]["pool_mb"].num();
        t += " (" + (mb == std::floor(mb) ? std::to_string((long long)mb) : fx(mb, 1)) + " MB pool)";
    }
    return t;
}

// ------------------------------------------------------------- markdown
static std::string table(const std::vector<std::vector<std::string>>& rows, const std::vector<std::string>& hdr) {
    std::ostringstream os;
    os << "|";
    for (auto& h : hdr) os << " " << h << " |";
    os << "\n|";
    for (size_t i = 0; i < hdr.size(); ++i) os << "---|";
    os << "\n";
    for (auto& r : rows) {
        os << "|";
        for (auto& c : r) os << " " << c << " |";
        os << "\n";
    }
    return os.str();
}

using Metric = std::pair<std::string, std::function<std::string(const Json&)>>;
static std::string metric_table(const Rs& rs, const std::vector<Metric>& ms) {
    std::vector<std::string> hdr{"Metric"};
    for (auto* r : rs) hdr.push_back(title(*r));
    std::vector<std::vector<std::string>> rows;
    for (auto& m : ms) {
        std::vector<std::string> row{m.first};
        for (auto* r : rs) row.push_back(m.second(*r));
        rows.push_back(row);
    }
    return table(rows, hdr);
}

// ------------------------------------------------------------- svg charts
static const char* COLORS[] = {"#2b7bba", "#d1495b", "#3a9e5f", "#edae49", "#8e6c8a", "#00798c"};

static double nice_step(double range) {
    double raw = range / 5, mag = std::pow(10, std::floor(std::log10(raw)));
    double f = raw / mag;
    return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * mag;
}
static std::string esc_xml(const std::string& s) {
    std::string o;
    for (char c : s) o += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
    return o;
}
static std::string tick_label(double v) {
    if (v == 0) return "0";
    double a = std::fabs(v);
    if (a >= 1e6) return fx(v / 1e6, a >= 1e7 ? 0 : 1) + "M";
    if (a >= 1e3) return fx(v / 1e3, a >= 1e4 ? 0 : 1) + "k";
    if (a >= 10) return fx(v, 0);
    std::string t = fx(v, a >= 1 ? 1 : 3);
    while (t.back() == '0') t.pop_back();
    if (t.back() == '.') t.pop_back();
    return t;
}

struct Svg {
    std::ostringstream os;
    double W = 900, H = 470, L = 85, R = 25, T = 55, B = 115;
    Svg(const std::string& title_text) {
        os << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H
           << "' viewBox='0 0 " << W << " " << H << "' font-family='Helvetica,Arial,sans-serif'>\n"
           << "<rect width='100%' height='100%' fill='white'/>\n"
           << "<text x='" << W / 2 << "' y='28' text-anchor='middle' font-size='17' font-weight='bold'>"
           << esc_xml(title_text) << "</text>\n";
    }
    double pw() const { return W - L - R; }
    double ph() const { return H - T - B; }
    void yaxis(double lo, double hi, bool logy, const std::string& label) {
        auto Y = [&](double v) { return ymap(v, lo, hi, logy); };
        std::vector<double> ticks;
        if (logy) for (double d = lo; d <= hi + 1e-9; d += 1) ticks.push_back(d);
        else for (double v = 0, st = nice_step(hi); v <= hi + 1e-9; v += st) ticks.push_back(v);
        for (double t : ticks) {
            double y = Y(logy ? std::pow(10, t) : t);
            os << "<line x1='" << L << "' x2='" << W - R << "' y1='" << y << "' y2='" << y
               << "' stroke='#ddd'/>\n<text x='" << L - 8 << "' y='" << y + 4
               << "' text-anchor='end' font-size='12'>" << tick_label(logy ? std::pow(10, t) : t) << "</text>\n";
        }
        os << "<line x1='" << L << "' x2='" << L << "' y1='" << T << "' y2='" << T + ph() << "' stroke='#333'/>\n"
           << "<line x1='" << L << "' x2='" << W - R << "' y1='" << T + ph() << "' y2='" << T + ph()
           << "' stroke='#333'/>\n"
           << "<text transform='translate(22," << T + ph() / 2 << ") rotate(-90)' text-anchor='middle' font-size='13'>"
           << esc_xml(label) << "</text>\n";
        lo_ = lo; hi_ = hi; log_ = logy;
    }
    double ymap(double v, double lo, double hi, bool logy) const {
        double f = logy ? (std::log10(std::max(v, 1e-12)) - lo) / (hi - lo) : v / hi;
        return T + ph() * (1 - std::clamp(f, 0.0, 1.0));
    }
    double Y(double v) const { return ymap(v, lo_, hi_, log_); }
    void legend(const std::vector<std::string>& names) {
        double x = W - R - 210, y = T + 6;
        for (size_t i = 0; i < names.size(); ++i, y += 20)
            os << "<rect x='" << x << "' y='" << y << "' width='13' height='13' fill='" << COLORS[i % 6]
               << "'/><text x='" << x + 19 << "' y='" << y + 11 << "' font-size='12'>" << esc_xml(names[i])
               << "</text>\n";
    }
    void save(const std::string& path) {
        os << "</svg>\n";
        std::ofstream(path) << os.str();
    }
    double lo_ = 0, hi_ = 1;
    bool log_ = false;
};

struct Series { std::string name; std::vector<double> v; };

static void bar_chart(const std::string& path, const std::string& ttl, const std::string& ylabel,
                      const std::vector<std::string>& cats, const std::vector<Series>& series, bool logy) {
    Svg s(ttl);
    double mx = 0, mn = 1e300;
    for (auto& se : series)
        for (double v : se.v) { mx = std::max(mx, v); if (v > 0) mn = std::min(mn, v); }
    if (mx <= 0) mx = 1;
    if (logy) s.yaxis(std::floor(std::log10(mn * 0.5)), std::ceil(std::log10(mx) + 1e-9), true, ylabel);
    else s.yaxis(0, nice_step(mx * 1.1) * std::ceil(mx * 1.1 / nice_step(mx * 1.1)), false, ylabel);
    double gw = s.pw() / double(cats.size()), bw = gw * 0.78 / double(series.size());
    for (size_t c = 0; c < cats.size(); ++c) {
        double gx = s.L + gw * double(c) + gw * 0.11;
        for (size_t k = 0; k < series.size(); ++k) {
            double v = series[k].v[c];
            double y = s.Y(v), base = s.T + s.ph();
            if (v <= 0) y = base;
            s.os << "<rect x='" << gx + bw * double(k) << "' y='" << y << "' width='" << bw * 0.92 << "' height='"
                 << base - y << "' fill='" << COLORS[k % 6] << "'><title>" << esc_xml(series[k].name) << ": "
                 << tick_label(v) << "</title></rect>\n";
        }
        double cx = s.L + gw * (double(c) + 0.5);
        s.os << "<text transform='translate(" << cx << "," << s.T + s.ph() + 18
             << ") rotate(-18)' text-anchor='end' font-size='12'>" << esc_xml(cats[c]) << "</text>\n";
    }
    if (series.size() > 1) {
        std::vector<std::string> n;
        for (auto& se : series) n.push_back(se.name);
        s.legend(n);
    }
    s.save(path);
}

struct Line { std::string name; std::vector<std::pair<double, double>> pts; };

static void line_chart(const std::string& path, const std::string& ttl, const std::string& xlabel,
                       const std::string& ylabel, const std::vector<Line>& lines) {
    Svg s(ttl);
    s.B = 70;
    double mx = 0, my = 0;
    for (auto& l : lines)
        for (auto& p : l.pts) { mx = std::max(mx, p.first); my = std::max(my, p.second); }
    if (mx <= 0) mx = 1;
    if (my <= 0) my = 1;
    double ystep = nice_step(my * 1.1);
    s.yaxis(0, ystep * std::ceil(my * 1.1 / ystep), false, ylabel);
    double xstep = nice_step(mx), xmax = xstep * std::ceil(mx / xstep);
    auto X = [&](double v) { return s.L + s.pw() * v / xmax; };
    for (double v = 0; v <= xmax + 1e-9; v += xstep)
        s.os << "<text x='" << X(v) << "' y='" << s.T + s.ph() + 18 << "' text-anchor='middle' font-size='12'>"
             << tick_label(v) << "</text>\n";
    s.os << "<text x='" << s.L + s.pw() / 2 << "' y='" << s.H - 18 << "' text-anchor='middle' font-size='13'>"
         << esc_xml(xlabel) << "</text>\n";
    std::vector<std::string> names;
    for (size_t i = 0; i < lines.size(); ++i) {
        names.push_back(lines[i].name);
        s.os << "<polyline fill='none' stroke-width='2.2' stroke='" << COLORS[i % 6] << "' points='";
        for (auto& p : lines[i].pts) s.os << X(p.first) << "," << s.Y(p.second) << " ";
        s.os << "'/>\n";
        for (auto& p : lines[i].pts)
            s.os << "<circle cx='" << X(p.first) << "' cy='" << s.Y(p.second) << "' r='3' fill='" << COLORS[i % 6]
                 << "'/>\n";
    }
    s.legend(names);
    s.save(path);
}

// ------------------------------------------------------------- report
static int expected_height(double n, int lc, int ic) {
    if (n <= lc) return 1;
    double fill = std::log(2.0), leaves = n / (lc * fill), fan = (ic + 1) * fill;
    return 1 + std::max(1, int(std::ceil(std::log(leaves) / std::log(fan))));
}

static double io_po(const Json& r, const char* pass, const char* q, const char* k) {
    return r["search"][pass][q]["io_per_op"][k].num();
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "results";
    std::vector<Json> all;
    try {
        for (auto& e : fs::directory_iterator(dir)) {
            if (e.path().extension() != ".json") continue;
            std::ifstream f(e.path());
            std::stringstream ss;
            ss << f.rdbuf();
            all.push_back(Json::parse(ss.str()));
        }
    } catch (std::exception& e) {
        std::cerr << "error reading " << dir << ": " << e.what() << "\n";
        return 1;
    }
    if (all.empty()) { std::cerr << "no results in " << dir << "/\n"; return 1; }
    std::map<std::string, int> order{{"memory", 0}, {"disk", 1}, {"hybrid", 2}};
    std::sort(all.begin(), all.end(), [&](const Json& a, const Json& b) {
        auto ka = std::make_tuple(order[a["tree"].str()], a["config"]["pool_mb"].num(), a["label"].str());
        auto kb = std::make_tuple(order[b["tree"].str()], b["config"]["pool_mb"].num(), b["label"].str());
        return ka < kb;
    });
    Rs rs, disk_rs, hy;
    for (auto& r : all) {
        rs.push_back(&r);
        if (r["tree"].str() != "memory") disk_rs.push_back(&r);
        if (r["tree"].str() == "hybrid") hy.push_back(&r);
    }
    const Json& r0 = *rs[0];
    const Json& cfg = r0["config"];
    const Json& ds = r0["dataset"];
    const Json& sy = r0["system"];
    long long n = r0["records"].integer();
    int ps = int(cfg["page_size"].integer()), ks = int(cfg["key_size"].integer());
    int lc = int(cfg["leaf_capacity"].integer()), ic = int(cfg["internal_capacity"].integer());

    // ---------------- charts
    std::string pd = dir + "/plots";
    fs::create_directories(pd);
    std::vector<std::string> cats;
    for (auto* r : rs) cats.push_back(title(*r));
    {
        std::vector<Line> lines;
        for (auto* r : rs) {
            Line l{title(*r), {{0.0, 0.0}}};
            const Json& cps = (*r)["build"]["checkpoints"];
            for (size_t i = 0; i < cps.size(); ++i)
                l.pts.emplace_back(cps[i]["records"].num() / 1e6, cps[i]["insert_s"].num());
            lines.push_back(l);
        }
        line_chart(pd + "/insertion_progress.svg", "Insertion progress", "records inserted (millions)",
                   "cumulative insertion time (s)", lines);
    }
    {
        Series t{"build time (s)", {}}, th{"inserts / s", {}};
        for (auto* r : rs) { t.v.push_back((*r)["build"]["total_s"].num()); th.v.push_back((*r)["build"]["inserts_per_s"].num()); }
        bar_chart(pd + "/build_time.svg", "Total build time (insert + flush)", "seconds", cats, {t}, false);
        bar_chart(pd + "/insert_throughput.svg", "Insert throughput", "inserts per second", cats, {th}, false);
    }
    {
        std::vector<Series> se{{"cold avg", {}}, {"cold p99", {}}, {"warm avg", {}}, {"warm p99", {}}};
        for (auto* r : rs) {
            se[0].v.push_back((*r)["search"]["cold"]["point_hit"]["avg_us"].num());
            se[1].v.push_back((*r)["search"]["cold"]["point_hit"]["p99_us"].num());
            se[2].v.push_back((*r)["search"]["warm"]["point_hit"]["avg_us"].num());
            se[3].v.push_back((*r)["search"]["warm"]["point_hit"]["p99_us"].num());
        }
        bar_chart(pd + "/search_latency.svg", "Point search latency (existing keys)", "µs (log scale)", cats, se, true);
    }
    if (!disk_rs.empty()) {
        std::vector<std::string> dc;
        std::vector<Series> se{{"page reads / insert", {}}, {"page writes / insert", {}},
                               {"page reads / search (cold)", {}}, {"page reads / search (warm)", {}}};
        for (auto* r : disk_rs) {
            dc.push_back(title(*r));
            se[0].v.push_back((*r)["build"]["io_per_insert"]["page_reads"].num());
            se[1].v.push_back((*r)["build"]["io_per_insert"]["page_writes"].num());
            se[2].v.push_back(io_po(*r, "cold", "point_hit", "page_reads"));
            se[3].v.push_back(io_po(*r, "warm", "point_hit", "page_reads"));
        }
        bar_chart(pd + "/disk_io.svg", "Index page I/O per operation", "pages", dc, se, false);
    }
    {
        std::vector<Series> se{{"RAM used by build (MB)", {}}, {"disk used (MB)", {}}};
        for (auto* r : rs) {
            se[0].v.push_back(std::max(0.0, (*r)["memory"]["build_delta"].num()) / 1048576.0);
            se[1].v.push_back((*r)["disk"]["total_bytes"].num() / 1048576.0);
        }
        bar_chart(pd + "/memory_disk_usage.svg", "Memory vs disk footprint", "MB", cats, se, false);
    }
    if (hy.size() >= 2) {
        Line hit{"build hit rate (%)", {}}, sh{"search hit rate, cold (%)", {}};
        for (auto* r : hy) {
            double mb = (*r)["config"]["pool_mb"].num();
            hit.pts.emplace_back(mb, 100 * (*r)["build"]["io"]["pool_hit_rate"].num());
            const Json& io = (*r)["search"]["cold"]["point_hit"]["io"];
            double h = io["pool_hits"].num(), m = io["pool_misses"].num();
            sh.pts.emplace_back(mb, h + m > 0 ? 100 * h / (h + m) : 0);
        }
        line_chart(pd + "/pool_sweep.svg", "Memory + Disk: buffer pool size vs hit rate", "buffer pool (MB)",
                   "hit rate (%)", {hit, sh});
    }

    // ---------------- markdown
    std::ostringstream w;
    std::time_t now = std::time(nullptr);
    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%d %H:%M", std::localtime(&now));
    w << "# B+ Tree Implementation – Benchmark Report\n\n_Generated " << date
      << " by `bptree_report`. All numbers are measured, not estimated._\n\n";

    w << "## 1. Setup\n\n"
      << table({{"Dataset", "`" + fs::path(ds["csv"].str()).filename().string() + "` (" + hb(ds["csv_bytes"].num()) +
                                ", " + commas(ds["rows"].integer()) + " rows)"},
                {"Records indexed", commas(n)},
                {"Key", "unique alphanumeric, avg " + fx(ds["avg_key_bytes"].num(), 1) + " B, max " +
                            std::to_string(ds["max_key_bytes"].integer()) + " B"},
                {"Payload", "avg " + fx(ds["avg_payload_bytes"].num(), 1) + " B"},
                {"Machine", sy["platform"].str() + " " + sy["machine"].str() + " · " +
                                std::to_string(sy["cpu_count"].integer()) + " CPUs · " +
                                hb(sy["total_ram_bytes"].num()) + " RAM"},
                {"Implementation", sy["language"].str() + " (" + sy["compiler"].str() + ", -O3)"},
                {"Queries", commas(cfg["hits"].integer()) + " existing-key lookups, " +
                                commas(cfg["misses"].integer()) + " missing-key lookups, " +
                                commas(cfg["ranges"].integer()) + " range scans × " +
                                std::to_string(cfg["range_len"].integer()) + " records (seed " +
                                std::to_string(cfg["seed"].integer()) + ")"}},
               {"Item", "Value"})
      << "\n";

    w << "## 2. Assumptions\n\n"
      << "* **Node / page size:** " << ps << " bytes (typical OS / SSD page). One B+ tree node = one page.\n"
      << "* **Key storage:** fixed-width slot of " << ks << " bytes (= longest key in the dataset), zero-padded. "
      << "Zero padding keeps byte-wise order identical to string order.\n"
      << "* **Page layout:** 16 B header (node type, key count, next-leaf page id, reserved) + key area + pointer area.\n"
      << "* **Leaf order:** ⌊(" << ps << " − 16) / (" << ks << " + 8)⌋ = **" << lc
      << " keys** per leaf (8 B record pointer = 40-bit offset + 24-bit length).\n"
      << "* **Internal order:** ⌊(" << ps << " − 16 − 4) / (" << ks << " + 4)⌋ = **" << ic << " keys / " << ic + 1
      << " children** (4 B child page id).\n"
      << "* The memory-only tree uses the **same capacities**, so all three trees have the same shape and only "
         "the storage medium differs.\n"
      << "* **Storage structure (disk variants):** `index.db` = array of fixed-size pages, page 0 = meta page "
         "(root id, height, counters); `heap.db` = append-only payload file. Leaves store only (key, pointer), so "
         "fan-out does not depend on payload size.\n"
      << "* **Memory-only payloads** live in an in-RAM append-only arena (64 MB blocks); leaves hold the same "
         "8 B pointer format as on disk.\n"
      << "* **Disk-only:** zero caching in the program – every node visit is a `pread` of one page, every "
         "modification an immediate `pwrite`, every payload append its own write.\n";
    if (!hy.empty()) {
        std::string pools, pages;
        for (size_t i = 0; i < hy.size(); ++i) {
            pools += (i ? ", " : "") + fx((*hy[i])["config"]["pool_mb"].num(), 0) + " MB";
            pages += (i ? ", " : "") + commas((*hy[i])["config"]["pool_pages"].integer());
        }
        w << "* **Memory + Disk:** buffer pool of " << pools << " (" << pages << " pages). "
          << ((*hy[0])["config"]["pin_internal"].truthy() ? "Internal nodes are pinned in RAM; leaves use LRU replacement. "
                                                           : "Plain LRU for all pages. ")
          << "Dirty pages are written back only on eviction or flush; payload appends go through a 1 MB write "
             "buffer. On open, the internal levels are loaded into the pool.\n";
    }
    w << "* **Insertion order:** CSV order (already randomly shuffled). Only `insert()` is timed; CSV parsing is "
         "excluded. Disk builds include the final flush + fsync.\n"
      << "* **Duplicate keys:** keys are unique; a repeated key would overwrite the value. Deletion is not part "
         "of the assignment.\n"
      << "* **I/O counts are logical page I/Os** issued by the program. "
      << (cfg["direct_io"].truthy() ? "The OS page cache was bypassed (`F_NOCACHE`).\n"
                                    : "The OS page cache was left on, so some reads may be served from kernel RAM; "
                                      "`--direct-io` bypasses it on macOS.\n")
      << "* **RAM used by build** = process RSS after the build minus RSS before it; it includes a small fixed "
         "benchmark overhead (one CSV chunk + the sampled query keys) common to all variants.\n"
      << "* Each variant runs in a separate process, so RAM measurements are independent.\n\n";

    auto correct_all = [](const Json& r) {
        for (const char* p : {"cold", "warm"})
            for (const char* q : {"point_hit", "point_miss"})
                if (r["search"][p][q]["correct"].integer() != r["search"][p][q]["count"].integer()) return false;
        return true;
    };

    w << "## 3. Summary\n\n"
      << metric_table(rs, {
             {"Build time (insert + flush)", [](const Json& r) { return ht(r["build"]["total_s"].num()); }},
             {"Insert throughput", [](const Json& r) { return commas((long long)r["build"]["inserts_per_s"].num()) + " /s"; }},
             {"Avg insert latency", [](const Json& r) { return us(r["build"]["avg_insert_us"].num()); }},
             {"Point search avg (cold)", [](const Json& r) { return us(r["search"]["cold"]["point_hit"]["avg_us"].num()); }},
             {"Point search avg (warm)", [](const Json& r) { return us(r["search"]["warm"]["point_hit"]["avg_us"].num()); }},
             {"Tree height", [](const Json& r) { return std::to_string(r["structure"]["height"].integer()); }},
             {"RAM used by build", [](const Json& r) { return hb(r["memory"]["build_delta"].num()); }},
             {"Disk used", [](const Json& r) { return hb(r["disk"]["total_bytes"].num()); }},
             {"Page I/O during build (R / W)", [](const Json& r) {
                  return r["tree"].str() == "memory" ? std::string("-")
                       : commas(r["build"]["io"]["page_reads"].integer()) + " / " + commas(r["build"]["io"]["page_writes"].integer()); }},
             {"Page reads per search (cold)", [](const Json& r) {
                  return r["tree"].str() == "memory" ? std::string("-") : fx(io_po(r, "cold", "point_hit", "page_reads")); }},
             {"All answers correct", [&](const Json& r) { return std::string(correct_all(r) ? "✅" : "❌"); }},
         })
      << "\n![Build time](plots/build_time.svg)\n\n";

    w << "## 4. Insertion\n\n"
      << metric_table(rs, {
             {"Records", [](const Json& r) { return commas(r["records"].integer()); }},
             {"Insert time", [](const Json& r) { return ht(r["build"]["insert_s"].num()); }},
             {"Flush time", [](const Json& r) { return ht(r["build"]["flush_s"].num()); }},
             {"Total build time", [](const Json& r) { return ht(r["build"]["total_s"].num()); }},
             {"Throughput", [](const Json& r) { return commas((long long)r["build"]["inserts_per_s"].num()) + " inserts/s"; }},
             {"Avg time per insert", [](const Json& r) { return us(r["build"]["avg_insert_us"].num()); }},
             {"Wall time incl. CSV parsing", [](const Json& r) { return ht(r["build"]["wall_s_including_csv"].num()); }},
         })
      << "\n![Insertion progress](plots/insertion_progress.svg)\n\n![Throughput](plots/insert_throughput.svg)\n\n";

    w << "## 5. Tree structure\n\n"
      << metric_table(rs, {
             {"Height (levels, root..leaf)", [](const Json& r) { return std::to_string(r["structure"]["height"].integer()); }},
             {"Leaf nodes", [](const Json& r) { return commas(r["structure"]["leaf_nodes"].integer()); }},
             {"Internal nodes", [](const Json& r) { return commas(r["structure"]["internal_nodes"].integer()); }},
             {"Total nodes / pages", [](const Json& r) { return commas(r["structure"]["total_nodes"].integer()); }},
             {"Leaf capacity (order)", [](const Json& r) { return std::to_string(r["structure"]["leaf_capacity"].integer()); }},
             {"Internal capacity / max fan-out", [](const Json& r) {
                  return std::to_string(r["structure"]["internal_capacity"].integer()) + " / " +
                         std::to_string(r["structure"]["max_fanout"].integer()); }},
             {"Avg leaf fill factor", [](const Json& r) { return fx(100 * r["structure"]["leaf_fill_factor"].num(), 1) + " %"; }},
         })
      << "\nTheory check: with random inserts nodes are ~ln 2 ≈ 69 % full, so the expected height is 1 + ⌈log<sub>"
      << fx((ic + 1) * std::log(2.0), 0) << "</sub>(" << commas(n) << " / " << fx(lc * std::log(2.0), 0)
      << ")⌉ = **" << expected_height(double(n), lc, ic) << "**. Measured: **" << r0["structure"]["height"].integer()
      << "**.\n\n";

    w << "## 6. Memory and disk usage\n\n"
      << metric_table(rs, {
             {"RSS before build", [](const Json& r) { return hb(r["memory"]["rss_start"].num()); }},
             {"RSS after build", [](const Json& r) { return hb(r["memory"]["rss_after_build"].num()); }},
             {"RAM used by build (delta)", [](const Json& r) { return hb(r["memory"]["build_delta"].num()); }},
             {"Peak RSS", [](const Json& r) { return hb(r["memory"]["peak_rss"].num()); }},
             {"RAM per record", [](const Json& r) {
                  return fx(std::max(0.0, r["memory"]["build_delta"].num()) / r["records"].num(), 1) + " B"; }},
             {"Index file", [](const Json& r) { return hb(r["disk"]["index_bytes"].num()); }},
             {"Heap (payload) file", [](const Json& r) { return hb(r["disk"]["heap_bytes"].num()); }},
             {"Total disk", [](const Json& r) { return hb(r["disk"]["total_bytes"].num()); }},
             {"Disk per record", [](const Json& r) { return fx(r["disk"]["total_bytes"].num() / r["records"].num(), 1) + " B"; }},
         })
      << "\n![Memory vs disk](plots/memory_disk_usage.svg)\n\n";

    if (!disk_rs.empty()) {
        auto is_h = [](const Json& r) { return r["tree"].str() == "hybrid"; };
        w << "## 7. Disk I/O\n\n### During the build\n\n"
          << metric_table(disk_rs, {
                 {"Index page reads", [](const Json& r) { return commas(r["build"]["io"]["page_reads"].integer()); }},
                 {"Index page writes", [](const Json& r) { return commas(r["build"]["io"]["page_writes"].integer()); }},
                 {"Page reads / insert", [](const Json& r) { return fx(r["build"]["io_per_insert"]["page_reads"].num(), 3); }},
                 {"Page writes / insert", [](const Json& r) { return fx(r["build"]["io_per_insert"]["page_writes"].num(), 3); }},
                 {"Heap write calls", [](const Json& r) { return commas(r["build"]["io"]["heap_writes"].integer()); }},
                 {"Data written to index", [](const Json& r) {
                      return hb(r["build"]["io"]["page_writes"].num() * r["config"]["page_size"].num()); }},
                 {"Buffer pool hit rate", [&](const Json& r) {
                      return is_h(r) ? fx(100 * r["build"]["io"]["pool_hit_rate"].num(), 1) + " %" : std::string("-"); }},
                 {"Evictions / dirty write-backs", [&](const Json& r) {
                      return is_h(r) ? commas(r["build"]["io"]["pool_evictions"].integer()) + " / " +
                                           commas(r["build"]["io"]["dirty_writebacks"].integer()) : std::string("-"); }},
             })
          << "\n### During search (per query)\n\n"
          << metric_table(disk_rs, {
                 {"Point hit – page reads (cold)", [](const Json& r) { return fx(io_po(r, "cold", "point_hit", "page_reads")); }},
                 {"Point hit – page reads (warm)", [](const Json& r) { return fx(io_po(r, "warm", "point_hit", "page_reads")); }},
                 {"Point hit – heap reads", [](const Json& r) { return fx(io_po(r, "cold", "point_hit", "heap_reads")); }},
                 {"Point miss – page reads (cold)", [](const Json& r) { return fx(io_po(r, "cold", "point_miss", "page_reads")); }},
                 {"Range scan – page reads (cold)", [](const Json& r) { return fx(io_po(r, "cold", "range", "page_reads")); }},
                 {"Range scan – heap reads", [](const Json& r) { return fx(io_po(r, "cold", "range", "heap_reads")); }},
                 {"Pool hit rate (cold searches)", [&](const Json& r) {
                      if (!is_h(r)) return std::string("-");
                      const Json& io = r["search"]["cold"]["point_hit"]["io"];
                      double h = io["pool_hits"].num(), m = io["pool_misses"].num();
                      return fx(h + m > 0 ? 100 * h / (h + m) : 0, 1) + " %"; }},
                 {"Reopen time", [](const Json& r) { return ht(r["reopen"]["open_s"].num()); }},
                 {"Warm-up (internal nodes loaded)", [&](const Json& r) {
                      return is_h(r) ? commas(r["reopen"]["warmup_pages_read"].integer()) + " pages in " +
                                           ht(r["reopen"]["warmup_s"].num()) : std::string("-"); }},
             })
          << "\n![Disk I/O](plots/disk_io.svg)\n\n";
    }

    w << "## 8. Search performance\n\n";
    for (const char* p : {"cold", "warm"}) {
        std::string pass = p;
        w << "### " << (pass == "cold" ? "Cold pass (right after reopening from disk)" : "Warm pass (same queries again)")
          << "\n\n"
          << metric_table(rs, {
                 {"Point hit avg / p50", [pass](const Json& r) {
                      const Json& x = r["search"][pass]["point_hit"];
                      return fx(x["avg_us"].num()) + " / " + us(x["p50_us"].num()); }},
                 {"Point hit p95 / p99", [pass](const Json& r) {
                      const Json& x = r["search"][pass]["point_hit"];
                      return fx(x["p95_us"].num()) + " / " + us(x["p99_us"].num()); }},
                 {"Point hit throughput", [pass](const Json& r) {
                      return commas((long long)r["search"][pass]["point_hit"]["ops_per_s"].num()) + " /s"; }},
                 {"Point miss avg", [pass](const Json& r) { return us(r["search"][pass]["point_miss"]["avg_us"].num()); }},
                 {"Range scan avg", [pass](const Json& r) { return us(r["search"][pass]["range"]["avg_us"].num()); }},
                 {"Range scan p99", [pass](const Json& r) { return us(r["search"][pass]["range"]["p99_us"].num()); }},
             })
          << "\n";
    }
    w << "![Search latency](plots/search_latency.svg)\n\n";

    std::set<std::string> digests;
    for (auto* r : rs) digests.insert((*r)["search"]["cold"]["range"]["digest"].str());
    w << "## 9. Correctness\n\n"
      << metric_table(rs, {
             {"Existing keys found with correct payload", [](const Json& r) {
                  const Json& x = r["search"]["cold"]["point_hit"];
                  return commas(x["correct"].integer()) + " / " + commas(x["count"].integer()); }},
             {"Missing keys correctly not found", [](const Json& r) {
                  const Json& x = r["search"]["cold"]["point_miss"];
                  return commas(x["correct"].integer()) + " / " + commas(x["count"].integer()); }},
             {"Range results sorted", [](const Json& r) {
                  return std::string(r["search"]["cold"]["range"]["sorted_ok"].truthy() ? "yes" : "NO"); }},
             {"Range result digest", [](const Json& r) {
                  return "`" + r["search"]["cold"]["range"]["digest"].str().substr(0, 12) + "`"; }},
             {"Full structural validate()", [](const Json& r) {
                  return std::string(r["validation"].truthy() ? "passed" : "not run"); }},
         })
      << "\nAll variants return "
      << (digests.size() == 1 ? "**identical** range-scan results (same digest).\n\n"
                              : "**different** range results – investigate!\n\n");

    if (hy.size() >= 2) {
        std::vector<std::vector<std::string>> rows;
        for (auto* r : hy)
            rows.push_back({fx((*r)["config"]["pool_mb"].num(), 0) + " MB", commas((*r)["config"]["pool_pages"].integer()),
                            ht((*r)["build"]["total_s"].num()), fx(100 * (*r)["build"]["io"]["pool_hit_rate"].num(), 1) + " %",
                            commas((*r)["build"]["io"]["page_reads"].integer()),
                            commas((*r)["build"]["io"]["page_writes"].integer()),
                            us((*r)["search"]["cold"]["point_hit"]["avg_us"].num())});
        w << "## 10. Buffer pool sweep (Memory + Disk)\n\n"
          << table(rows, {"Pool", "Pages", "Build", "Hit rate", "Page reads", "Page writes", "Search avg (cold)"})
          << "\n![Pool sweep](plots/pool_sweep.svg)\n\n";
    }

    // ---------------- observations
    const Json *m = nullptr, *d = nullptr, *h = nullptr;
    for (auto* r : rs) {
        if ((*r)["label"].str() == "memory") m = r;
        if ((*r)["label"].str() == "disk") d = r;
        if ((*r)["label"].str() == "hybrid") h = r;
    }
    if (!h && !hy.empty()) h = hy.back();
    w << "## Observations\n\n";
    if (m && d) {
        w << "* **Disk-only vs memory-only:** building is "
          << fx((*d)["build"]["total_s"].num() / (*m)["build"]["total_s"].num(), 1) << "× slower and a cold point search is "
          << fx((*d)["search"]["cold"]["point_hit"]["avg_us"].num() / (*m)["search"]["cold"]["point_hit"]["avg_us"].num(), 1)
          << "× slower. Every insert costs ≈ " << fx((*d)["build"]["io_per_insert"]["page_reads"].num(), 1)
          << " page reads (one per level) plus " << fx((*d)["build"]["io_per_insert"]["page_writes"].num())
          << " page writes, and every page is (de)serialised.\n"
          << "* **Memory-only** is fastest but needs " << hb((*m)["memory"]["build_delta"].num()) << " of RAM (≈ "
          << fx((*m)["memory"]["build_delta"].num() / (*m)["records"].num(), 0)
          << " B per record), while the disk variants keep the same data in " << hb((*d)["disk"]["total_bytes"].num())
          << " on disk with almost no RAM.\n";
    }
    if (h && d) {
        w << "* **Memory + Disk** cuts index page reads during the build from " << commas((*d)["build"]["io"]["page_reads"].integer())
          << " to " << commas((*h)["build"]["io"]["page_reads"].integer()) << " and writes from "
          << commas((*d)["build"]["io"]["page_writes"].integer()) << " to " << commas((*h)["build"]["io"]["page_writes"].integer())
          << ", giving a " << fx((*d)["build"]["total_s"].num() / (*h)["build"]["total_s"].num(), 1)
          << "× faster build than disk-only with a " << fx((*h)["config"]["pool_mb"].num(), 0) << " MB pool.\n"
          << "* With the internal nodes pinned, a cold search in the hybrid tree needs at most one page read (the leaf) – "
             "measured " << fx(io_po(*h, "cold", "point_hit", "page_reads")) << " – versus "
          << fx(io_po(*d, "cold", "point_hit", "page_reads")) << " (= tree height) for disk-only.\n";
    }
    if (m && h)
        w << "* Hybrid build takes " << fx((*h)["build"]["total_s"].num() / (*m)["build"]["total_s"].num(), 1)
          << "× the memory-only time while using " << hb((*h)["memory"]["build_delta"].num()) << " instead of "
          << hb((*m)["memory"]["build_delta"].num()) << " of RAM – the classic database trade-off of a bounded buffer pool.\n";
    w << "* Height stays small (" << r0["structure"]["height"].integer() << " levels for " << commas(n)
      << " records) because a " << ps << " B page gives a fan-out of " << ic + 1
      << ", so search cost grows only logarithmically.\n"
      << "* Range scans use the leaf sibling chain: after one root-to-leaf descent only consecutive leaves are read.\n"
      << "* Implemented in C++17 (-O3), so timings and RAM reflect the data structure and storage design rather than "
         "language overhead.\n";

    std::string out = dir + "/REPORT.md";
    std::ofstream(out) << w.str();
    std::cerr << "report written to " << out << " (charts in " << pd << "/)\n";
    return 0;
}
