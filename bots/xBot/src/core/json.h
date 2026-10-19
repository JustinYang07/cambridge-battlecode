#pragma once
// Minimal append-only JSON writer for trace lines. Fuel is ~1 per WASM instruction,
// so it writes into one fixed buffer with memcpy instead of growing a std::string
// character by character, and formats numbers without snprintf (~3k fuel per call).
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

class Json {
    static constexpr std::size_t kCap = 8192;
    char buf_[kCap];
    std::size_t len_ = 0;
    bool needComma_ = false;

    void put(char c) {
        if (len_ + 1 < kCap) buf_[len_++] = c;
    }
    void put(std::string_view s) {
        const std::size_t n = s.size() < kCap - 1 - len_ ? s.size() : kCap - 1 - len_;
        std::memcpy(buf_ + len_, s.data(), n);
        len_ += n;
    }
    void sep() {
        if (needComma_) put(',');
        needComma_ = true;
    }
    void key(std::string_view k) {
        sep();
        put('"');
        put(k);
        put("\":");
    }
    // Fuel-aware formatting: bulk memcpy is ~1 fuel, while per-character loops and 64-bit
    // division are expensive, so digits come from a 2-digit table using 32-bit maths.
    template <class Sink>
    static void integerTo(Sink&& sink, long long v) {
        static constexpr char kPairs[] =
            "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
            "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
            "8081828384858687888990919293949596979899";
        char out[24];
        char* end = out + sizeof out;
        char* p = end;
        const bool neg = v < 0;
        unsigned long long u = neg ? 0ULL - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
        while (u >= 4294967296ULL) { *--p = static_cast<char>('0' + u % 10); u /= 10; }  // rare
        auto x = static_cast<std::uint32_t>(u);
        while (x >= 100) {
            const std::uint32_t q = x / 100, r = x - q * 100;
            p -= 2;
            std::memcpy(p, kPairs + r * 2, 2);
            x = q;
        }
        if (x >= 10) { p -= 2; std::memcpy(p, kPairs + x * 2, 2); }
        else *--p = static_cast<char>('0' + x);
        if (neg) *--p = '-';
        sink(std::string_view(p, static_cast<std::size_t>(end - p)));
    }
    template <class Sink>
    static void numberTo(Sink&& sink, double v) {
        if (!(v > -9e12 && v < 9e12)) { sink("null"); return; }  // also catches NaN
        const double scaled = v * 100.0;
        const auto hundredths = static_cast<long long>(scaled + (scaled >= 0 ? 0.5 : -0.5));
        if (hundredths % 100 == 0) { integerTo(sink, hundredths / 100); return; }
        const long long a = hundredths < 0 ? -hundredths : hundredths;
        if (hundredths < 0) sink("-");
        integerTo(sink, a / 100);
        const auto f = static_cast<int>(a % 100);
        char frac[3] = {'.', static_cast<char>('0' + f / 10), static_cast<char>('0' + f % 10)};
        sink(std::string_view(frac, f % 10 ? 3 : 2));
    }
    // Callers pass trusted text (labels, codes, sanitised notes), so no per-character escaping.
    template <class Sink>
    static void quoteTo(Sink&& sink, std::string_view s) {
        sink("\"");
        sink(s);
        sink("\"");
    }

public:
    // std::string helpers for the few non-hot callers.
    static void integer(std::string& out, long long v) { integerTo([&](std::string_view s) { out += s; }, v); }
    static void number(std::string& out, double v) { numberTo([&](std::string_view s) { out += s; }, v); }
    static void quote(std::string& out, std::string_view s) { quoteTo([&](std::string_view p) { out += p; }, s); }

    void raw(std::string_view s) { put(s); }
    Json& begin() { sep(); put('{'); needComma_ = false; return *this; }
    Json& begin(std::string_view k) { key(k); put('{'); needComma_ = false; return *this; }
    Json& end() { put('}'); needComma_ = true; return *this; }
    Json& array(std::string_view k) { key(k); put('['); needComma_ = false; return *this; }
    Json& array() { sep(); put('['); needComma_ = false; return *this; }
    Json& endArray() { put(']'); needComma_ = true; return *this; }
    Json& field(std::string_view k, double v) { key(k); numberTo([&](std::string_view s) { put(s); }, v); return *this; }
    Json& field(std::string_view k, int v) { key(k); integerTo([&](std::string_view s) { put(s); }, v); return *this; }
    Json& field(std::string_view k, bool v) { key(k); put(v ? "true" : "false"); return *this; }
    Json& field(std::string_view k, std::string_view v) { key(k); quoteTo([&](std::string_view s) { put(s); }, v); return *this; }
    Json& field(std::string_view k, const char* v) { return field(k, std::string_view(v)); }
    Json& point(std::string_view k, int x, int y) {
        key(k);
        put('[');
        integerTo([&](std::string_view s) { put(s); }, x);
        put(',');
        integerTo([&](std::string_view s) { put(s); }, y);
        put(']');
        return *this;
    }
    Json& value(double v) { sep(); numberTo([&](std::string_view s) { put(s); }, v); return *this; }
    Json& value(long long v) { sep(); integerTo([&](std::string_view s) { put(s); }, v); return *this; }
    Json& value(std::string_view s) { sep(); quoteTo([&](std::string_view p) { put(p); }, s); return *this; }
    Json& value(const char* s) { return value(std::string_view(s)); }
    std::string_view view() const { return {buf_, len_}; }
    void reset() { len_ = 0; needComma_ = false; }
};
