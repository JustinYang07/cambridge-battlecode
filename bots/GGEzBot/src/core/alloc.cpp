// combatBotv2: a small-object allocator replacing the global operator new/delete.
//
// Measured in the runner: wasi-libc's malloc + free costs ~390 fuel per pair (an aligned 400-byte
// block, i.e. one bitboard, ~540), while the engine's bitboard set is ~39 and draw_chebyshev ~27.
// Every std::vector, std::string and bitboard goes through operator new, so allocation was a large
// share of each turn (the first turn on lanes built ~250 of them). Here blocks up to 4 KB come from
// per-size-class free lists carved out of 256 KB chunks: a pop or a push, a few instructions.
// Larger blocks fall back to malloc. Memory is never handed back to libc, which is fine for a
// bot whose working set is bounded by the map.
//
// Every block carries a 16-byte header holding its size class, so unsized delete works and user
// pointers stay 16-byte aligned (bitboards ask for align 16).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {

constexpr std::size_t kHeader = 16;
constexpr std::size_t kChunk = 256 * 1024;
// Size classes (payload bytes): 16..256 in steps of 16, then 320..1024 in steps of 64, then
// 1280..4096 in steps of 256.
constexpr int kClasses = 16 + 12 + 12;
constexpr std::uint32_t kLarge = 0xFFFFFFFFu;    // header tag: plain malloc block
constexpr std::uint32_t kAligned = 0xFFFFFFFEu;  // header tag: over-aligned malloc block

struct FreeNode {
    FreeNode* next;
};

FreeNode* freeList[kClasses] = {};
char* bumpAt = nullptr;
char* bumpEnd = nullptr;

inline int classOf(std::size_t n) {
    if (n <= 256) return n == 0 ? 0 : static_cast<int>((n - 1) >> 4);
    if (n <= 1024) return 16 + static_cast<int>((n - 257) >> 6);
    if (n <= 4096) return 28 + static_cast<int>((n - 1025) >> 8);
    return -1;
}

inline std::size_t classSize(int c) {
    if (c < 16) return static_cast<std::size_t>(c + 1) << 4;
    if (c < 28) return 256 + (static_cast<std::size_t>(c - 15) << 6);
    return 1024 + (static_cast<std::size_t>(c - 27) << 8);
}

inline std::uint32_t& tag(void* user) { return *reinterpret_cast<std::uint32_t*>(static_cast<char*>(user) - kHeader); }

void* allocate(std::size_t n) {
    const int c = classOf(n);
    if (c < 0) {
        char* raw = static_cast<char*>(std::malloc(n + kHeader));
        if (!raw) throw std::bad_alloc();
        void* user = raw + kHeader;
        tag(user) = kLarge;
        return user;
    }
    if (FreeNode* node = freeList[c]) {
        freeList[c] = node->next;
        return node;
    }
    const std::size_t block = kHeader + classSize(c);
    if (static_cast<std::size_t>(bumpEnd - bumpAt) < block) {
        // The tail of the old chunk is abandoned (at most 4 KB).
        char* chunk = static_cast<char*>(std::malloc(kChunk));
        if (!chunk) throw std::bad_alloc();
        // malloc returns 16-byte aligned memory on wasm32; headers keep user pointers aligned.
        bumpAt = chunk;
        bumpEnd = chunk + kChunk;
    }
    void* user = bumpAt + kHeader;
    bumpAt += block;
    tag(user) = static_cast<std::uint32_t>(c);
    return user;
}

void release(void* user) noexcept {
    if (!user) return;
    const std::uint32_t t = tag(user);
    if (t == kLarge) {
        std::free(static_cast<char*>(user) - kHeader);
        return;
    }
    if (t == kAligned) {
        std::free(*reinterpret_cast<void**>(static_cast<char*>(user) - kHeader + 8));
        return;
    }
    auto* node = static_cast<FreeNode*>(user);
    node->next = freeList[t];
    freeList[t] = node;
}

void* allocateAligned(std::size_t n, std::size_t align) {
    if (align <= kHeader) return allocate(n);
    char* raw = static_cast<char*>(std::malloc(n + align + kHeader));
    if (!raw) throw std::bad_alloc();
    std::uintptr_t p = reinterpret_cast<std::uintptr_t>(raw) + kHeader;
    p = (p + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
    void* user = reinterpret_cast<void*>(p);
    tag(user) = kAligned;
    *reinterpret_cast<void**>(static_cast<char*>(user) - kHeader + 8) = raw;
    return user;
}

} // namespace

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t n, std::align_val_t a) { return allocateAligned(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocateAligned(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try {
        return allocateAligned(n, static_cast<std::size_t>(a));
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try {
        return allocateAligned(n, static_cast<std::size_t>(a));
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }
