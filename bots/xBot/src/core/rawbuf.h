#pragma once
// combatBotv2: a heap array that is not zeroed on allocation. std::vector's resize/assign zero
// elements one at a time in wasm (~1 fuel per element: 10k for a 100x100 map's byte table, and
// every distance field's uint16 buffer was zeroed just before the export overwrote it). zero() uses
// 128-bit stores: memset/memcpy lower to memory.fill/copy, which the runner charges ~1 fuel per
// byte (10k for 10 KB), while a v128 store loop is ~1 fuel per 4 bytes.
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <wasm_simd128.h>

// Zero n bytes with 16-byte stores (p need not be aligned).
// no_builtin: clang otherwise turns this loop back into memset (memory.fill).
__attribute__((no_builtin("memset"))) inline void fastZero(void* p, std::size_t n) {
    auto* c = static_cast<unsigned char*>(p);
    const v128_t z = wasm_i64x2_splat(0);
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        wasm_v128_store(c + i, z);
        wasm_v128_store(c + i + 16, z);
        wasm_v128_store(c + i + 32, z);
        wasm_v128_store(c + i + 48, z);
    }
    for (; i + 16 <= n; i += 16) wasm_v128_store(c + i, z);
    for (; i < n; ++i) c[i] = 0;
}

template <class T>
struct RawBuf {
    std::unique_ptr<T[]> data;
    std::size_t n = 0;
    void alloc(std::size_t k) {
        if (k != n) {
            data.reset(new T[k]);  // default-initialised: left as is for trivial T
            n = k;
        }
    }
    void zero() { fastZero(data.get(), n * sizeof(T)); }
    bool empty() const { return n == 0; }
    std::size_t size() const { return n; }
    T& operator[](std::size_t i) { return data[i]; }
    const T& operator[](std::size_t i) const { return data[i]; }
    std::span<T> span() { return {data.get(), n}; }
};
