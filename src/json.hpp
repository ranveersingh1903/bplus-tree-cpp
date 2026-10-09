// Minimal JSON value: build, dump, parse. Used for benchmark results.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

class Json {
public:
    enum class T { Null, Bool, Int, Dbl, Str, Arr, Obj };
    Json() = default;
    Json(bool v) : t_(T::Bool), b_(v) {}
    template <class I, std::enable_if_t<std::is_integral_v<I> && !std::is_same_v<I, bool>, int> = 0>
    Json(I v) : t_(T::Int), i_((long long)v) {}
    Json(double v) : t_(T::Dbl), d_(v) {}
    Json(const char* v) : t_(T::Str), s_(v) {}
    Json(std::string v) : t_(T::Str), s_(std::move(v)) {}
    static Json object() { Json j; j.t_ = T::Obj; return j; }
    static Json array() { Json j; j.t_ = T::Arr; return j; }

    // ---- building
    Json& operator[](const std::string& k) {
        if (t_ == T::Null) t_ = T::Obj;
        for (auto& p : o_) if (p.first == k) return p.second;
        o_.emplace_back(k, Json());
        return o_.back().second;
    }
    void push(Json v) { if (t_ == T::Null) t_ = T::Arr; a_.push_back(std::move(v)); }

    // ---- reading
    const Json& operator[](const std::string& k) const {
        for (auto& p : o_) if (p.first == k) return p.second;
        return null_ref();
    }
    const Json& operator[](size_t i) const { return i < a_.size() ? a_[i] : null_ref(); }
    bool has(const std::string& k) const {
        for (auto& p : o_) if (p.first == k) return true;
        return false;
    }
    bool is_null() const { return t_ == T::Null; }
    bool truthy() const {
        switch (t_) {
            case T::Null: return false;
            case T::Bool: return b_;
            case T::Int: return i_ != 0;
            case T::Dbl: return d_ != 0;
            case T::Str: return !s_.empty();
            case T::Arr: return !a_.empty();
            case T::Obj: return !o_.empty();
        }
        return false;
    }
    double num() const { return t_ == T::Int ? double(i_) : t_ == T::Dbl ? d_ : 0.0; }
    long long integer() const { return t_ == T::Int ? i_ : t_ == T::Dbl ? (long long)d_ : 0; }
    const std::string& str() const { return s_; }
    size_t size() const { return t_ == T::Arr ? a_.size() : o_.size(); }
    const std::vector<std::pair<std::string, Json>>& items() const { return o_; }

    // ---- output
    void dump(std::ostream& os, int ind = 0) const {
        std::string pad(size_t(ind + 2), ' '), endpad(size_t(ind), ' ');
        switch (t_) {
            case T::Null: os << "null"; break;
            case T::Bool: os << (b_ ? "true" : "false"); break;
            case T::Int: os << i_; break;
            case T::Dbl:
                if (std::isfinite(d_)) os << std::setprecision(12) << d_; else os << "null";
                break;
            case T::Str: esc(os, s_); break;
            case T::Arr:
                if (a_.empty()) { os << "[]"; break; }
                os << "[\n";
                for (size_t j = 0; j < a_.size(); ++j) {
                    os << pad; a_[j].dump(os, ind + 2);
                    os << (j + 1 < a_.size() ? ",\n" : "\n");
                }
                os << endpad << "]";
                break;
            case T::Obj:
                if (o_.empty()) { os << "{}"; break; }
                os << "{\n";
                for (size_t j = 0; j < o_.size(); ++j) {
                    os << pad; esc(os, o_[j].first); os << ": ";
                    o_[j].second.dump(os, ind + 2);
                    os << (j + 1 < o_.size() ? ",\n" : "\n");
                }
                os << endpad << "}";
                break;
        }
    }

    // ---- parsing
    static Json parse(const std::string& text) {
        size_t p = 0;
        Json j = parse_value(text, p);
        skip(text, p);
        if (p != text.size()) throw std::runtime_error("JSON: trailing data");
        return j;
    }

private:
    static const Json& null_ref() { static const Json n; return n; }
    static void esc(std::ostream& os, const std::string& s) {
        os << '"';
        for (unsigned char c : s) {
            if (c == '"') os << "\\\"";
            else if (c == '\\') os << "\\\\";
            else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); os << b; }
            else os << c;
        }
        os << '"';
    }
    static void skip(const std::string& s, size_t& p) {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p;
    }
    static std::string parse_string(const std::string& s, size_t& p) {
        if (s[p] != '"') throw std::runtime_error("JSON: expected string");
        ++p;
        std::string out;
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c == '\\') {
                char e = s[p++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        unsigned cp = unsigned(std::strtoul(s.substr(p, 4).c_str(), nullptr, 16));
                        p += 4;
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                        else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out += e;
                }
            } else {
                out += c;
            }
        }
        ++p;
        return out;
    }
    static Json parse_value(const std::string& s, size_t& p) {
        skip(s, p);
        if (p >= s.size()) throw std::runtime_error("JSON: unexpected end");
        char c = s[p];
        if (c == '{') {
            Json j = object();
            ++p; skip(s, p);
            if (s[p] == '}') { ++p; return j; }
            for (;;) {
                skip(s, p);
                std::string k = parse_string(s, p);
                skip(s, p);
                if (s[p++] != ':') throw std::runtime_error("JSON: expected ':'");
                j.o_.emplace_back(k, parse_value(s, p));
                skip(s, p);
                if (s[p] == ',') { ++p; continue; }
                if (s[p] == '}') { ++p; return j; }
                throw std::runtime_error("JSON: bad object");
            }
        }
        if (c == '[') {
            Json j = array();
            ++p; skip(s, p);
            if (s[p] == ']') { ++p; return j; }
            for (;;) {
                j.a_.push_back(parse_value(s, p));
                skip(s, p);
                if (s[p] == ',') { ++p; continue; }
                if (s[p] == ']') { ++p; return j; }
                throw std::runtime_error("JSON: bad array");
            }
        }
        if (c == '"') return Json(parse_string(s, p));
        if (s.compare(p, 4, "true") == 0) { p += 4; return Json(true); }
        if (s.compare(p, 5, "false") == 0) { p += 5; return Json(false); }
        if (s.compare(p, 4, "null") == 0) { p += 4; return Json(); }
        size_t start = p;
        bool is_dbl = false;
        while (p < s.size() && (isdigit((unsigned char)s[p]) || s[p] == '-' || s[p] == '+' || s[p] == '.' ||
                                s[p] == 'e' || s[p] == 'E')) {
            if (s[p] == '.' || s[p] == 'e' || s[p] == 'E') is_dbl = true;
            ++p;
        }
        if (start == p) throw std::runtime_error("JSON: bad value");
        std::string num = s.substr(start, p - start);
        if (is_dbl) return Json(std::strtod(num.c_str(), nullptr));
        return Json((long long)std::strtoll(num.c_str(), nullptr, 10));
    }

    T t_ = T::Null;
    bool b_ = false;
    long long i_ = 0;
    double d_ = 0;
    std::string s_;
    std::vector<Json> a_;
    std::vector<std::pair<std::string, Json>> o_;
};
