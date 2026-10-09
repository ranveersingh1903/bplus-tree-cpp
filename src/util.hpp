#pragma once
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#include "json.hpp"
#include "storage.hpp"

// ------------------------------------------------------------------ timing
using Clock = std::chrono::steady_clock;
inline double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// ------------------------------------------------------------------ memory
inline long long rss_bytes() {
#ifdef __APPLE__
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        return (long long)info.resident_size;
    return 0;
#else
    long pages = 0, resident = 0;
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    return (long long)resident * sysconf(_SC_PAGESIZE);
#endif
}

inline long long peak_rss_bytes() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (long long)ru.ru_maxrss;
#else
    return (long long)ru.ru_maxrss * 1024;
#endif
}

inline long long total_ram_bytes() {
#ifdef __APPLE__
    int64_t mem = 0;
    size_t len = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
    return mem;
#else
    return (long long)sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
#endif
}

inline Json io_json(const IoStats& s) {
    Json j = Json::object();
    for (auto& c : s.ints) j[c.name] = c.value;
    for (auto& r : s.reals) j[r.name] = r.value;
    return j;
}

inline IoStats io_delta(const IoStats& a, const IoStats& b) {
    IoStats d;
    for (size_t k = 0; k < b.ints.size() && k < a.ints.size(); ++k)
        d.ints.push_back({b.ints[k].name, b.ints[k].value - a.ints[k].value});
    return d;
}

inline Json per_op(const IoStats& d, double n) {
    Json j = Json::object();
    for (auto& c : d.ints) j[c.name] = n > 0 ? double(c.value) / n : 0.0;
    return j;
}

// ------------------------------------------------------------------ latency
inline Json latency_summary(std::vector<long long>& ns) {
    Json j = Json::object();
    j["count"] = (long long)ns.size();
    if (ns.empty()) return j;
    std::sort(ns.begin(), ns.end());
    double total = 0;
    for (auto v : ns) total += double(v);
    size_t n = ns.size();
    auto pct = [&](double p) { return double(ns[std::min(n - 1, size_t(p * double(n)))]) / 1000.0; };
    j["total_s"] = total / 1e9;
    j["avg_us"] = total / double(n) / 1000.0;
    j["p50_us"] = pct(0.50);
    j["p95_us"] = pct(0.95);
    j["p99_us"] = pct(0.99);
    j["max_us"] = double(ns.back()) / 1000.0;
    j["ops_per_s"] = total > 0 ? double(n) / (total / 1e9) : 0.0;
    return j;
}

// ------------------------------------------------------------------ csv
class CsvReader {
public:
    explicit CsvReader(const std::string& path) : buf_(size_t(1) << 22) {
        f_.rdbuf()->pubsetbuf(buf_.data(), std::streamsize(buf_.size()));
        f_.open(path, std::ios::binary);
        if (!f_) throw std::runtime_error("cannot open CSV " + path);
    }
    // handles quoted fields with "" escapes (single-line records)
    bool next(std::vector<std::string>& fields) {
        if (!std::getline(f_, line_)) return false;
        if (!line_.empty() && line_.back() == '\r') line_.pop_back();
        size_t nf = 0;
        auto field = [&]() -> std::string& {
            if (fields.size() <= nf) fields.emplace_back();
            std::string& s = fields[nf++];
            s.clear();
            return s;
        };
        if (line_.find('"') == std::string::npos) {
            size_t start = 0;
            for (;;) {
                size_t c = line_.find(',', start);
                field().assign(line_, start, c == std::string::npos ? std::string::npos : c - start);
                if (c == std::string::npos) break;
                start = c + 1;
            }
        } else {
            std::string* cur = &field();
            bool q = false;
            for (size_t i = 0; i < line_.size(); ++i) {
                char c = line_[i];
                if (q) {
                    if (c == '"') {
                        if (i + 1 < line_.size() && line_[i + 1] == '"') { *cur += '"'; ++i; }
                        else q = false;
                    } else *cur += c;
                } else if (c == ',') cur = &field();
                else if (c == '"') q = true;
                else *cur += c;
            }
        }
        fields.resize(nf);
        return true;
    }

private:
    std::vector<char> buf_;
    std::ifstream f_;
    std::string line_;
};

// ------------------------------------------------------------------ format
inline std::string human_bytes(double n) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int k = 0;
    while (std::fabs(n) >= 1024 && k < 4) { n /= 1024; ++k; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(k ? 1 : 0) << n << " " << u[k];
    return os.str();
}

inline std::string human_time(double s) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2);
    if (s < 1e-3) os << s * 1e6 << " us";
    else if (s < 1) os << s * 1e3 << " ms";
    else if (s < 120) os << s << " s";
    else os << s / 60 << " min";
    return os.str();
}

inline std::string compiler_name() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("g++ ") + __VERSION__;
#else
    return "unknown";
#endif
}

inline Json system_info() {
    Json j = Json::object();
    struct utsname u;
    uname(&u);
    j["language"] = "C++17";
    j["compiler"] = compiler_name();
    j["platform"] = std::string(u.sysname) + " " + u.release;
    j["machine"] = std::string(u.machine);
    j["cpu_count"] = (long long)std::thread::hardware_concurrency();
    j["total_ram_bytes"] = total_ram_bytes();
    return j;
}
