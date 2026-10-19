#pragma once
// Heap-free string for labels and notes. Under fuel metering every heap allocation
// costs hundreds of instructions, so per-turn data avoids std::string.
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

template <std::size_t N>
struct FixedStr {
    char data[N] = {};
    std::size_t len = 0;

    FixedStr& clear() { len = 0; data[0] = 0; return *this; }
    FixedStr& operator<<(std::string_view s) {
        const std::size_t n = std::min(s.size(), N - 1 - len);
        std::memcpy(data + len, s.data(), n);
        len += n;
        data[len] = 0;
        return *this;
    }
    // For untrusted text (engine error messages) that ends up inside JSON strings.
    FixedStr& sanitized(std::string_view s) {
        const std::size_t start = len;
        *this << s;
        for (std::size_t i = start; i < len; ++i)
            if (data[i] == '"' || data[i] == '\\' || static_cast<unsigned char>(data[i]) < 0x20) data[i] = '\'';
        return *this;
    }
    FixedStr& operator<<(const char* s) { return *this << std::string_view(s); }
    FixedStr& operator<<(char c) {
        if (len + 1 < N) { data[len++] = c; data[len] = 0; }
        return *this;
    }
    FixedStr& operator<<(long long v) {
        char buf[24];
        int n = 0;
        const bool neg = v < 0;
        unsigned long long u = neg ? 0ULL - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
        do { buf[n++] = static_cast<char>('0' + u % 10); u /= 10; } while (u && n < 22);
        if (neg) buf[n++] = '-';
        while (n && len + 1 < N) data[len++] = buf[--n];
        data[len] = 0;
        return *this;
    }
    FixedStr& operator<<(int v) { return *this << static_cast<long long>(v); }
    FixedStr& point(int x, int y) { return *this << '(' << x << ',' << y << ')'; }

    bool empty() const { return len == 0; }
    std::string_view view() const { return {data, len}; }
    const char* c_str() const { return data; }
};
