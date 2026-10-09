// Synthetic dataset with the same shape as the assignment data
// (unique alphanumeric key, random payload, shuffled) for a quick demo.
//   ./build/gen_sample 200000 data/sample.csv
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

int main(int argc, char** argv) {
    size_t rows = argc > 1 ? std::stoul(argv[1]) : 200000;
    std::string out = argc > 2 ? argv[2] : "data/sample.csv";
    static const char A[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::mt19937_64 r(7);
    std::unordered_set<std::string> seen;
    std::vector<std::string> keys;
    keys.reserve(rows);
    while (keys.size() < rows) {
        std::string k(12, ' ');
        for (auto& c : k) c = A[r() % 62];
        if (seen.insert(k).second) keys.push_back(k);
    }
    std::shuffle(keys.begin(), keys.end(), r);
    if (auto p = std::filesystem::path(out).parent_path(); !p.empty()) std::filesystem::create_directories(p);
    std::ofstream f(out);
    f << "key,payload\n";
    for (auto& k : keys) {
        std::string p(32 + r() % 33, ' ');
        for (auto& c : p) c = A[r() % 62];
        f << k << ',' << p << '\n';
    }
    std::cerr << "wrote " << rows << " rows to " << out << "\n";
}
