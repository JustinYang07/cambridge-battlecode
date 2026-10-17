#pragma once
// #define BB_NO_TRACE  // loopBot: traces on for the lab (define for submissions)
// Decision traces for the lab's decision explorer (lab/labkit/replays.py decodes them).
//
// Writing stdout costs ~33 fuel per byte under the runner, and the engine keeps only
// ~1 MiB per team, so traces use a compact, self-describing encoding:
//   @K[u,id,"name","op"]   dictionary entry, written once per name per bot instance
//   @G[id,"label",x,y]     camp-group legend (first turn, slot 0); targets say "g<id>"
//   @A[[x,y],...]          altar legend (first turn, slot 0); targets say "a<index>"
//   @U[u,"champion"]       which champion plays slot u (each bot's first turn)
//   @P[regions,[region per group],[region per slot]]   team plan (slot 0, once)
//   @t[r,u,x,y,hp,gems,gold,kind,score,fuel,"target","actions",lost]   every turn
//   @T[r,u,hp,maxHp,gems,gold,fuel,lost,[[kind,"target",score,x,y,"note",[[factor,value],...]],...],"actions","note"]
// kind and factor are dictionary ids. The detailed @T line is written when the decision
// changes or every `full_trace_every` turns, and only when the turn has fuel to spare.
//
// Define BB_NO_TRACE to compile tracing out for fuel-sensitive submissions.
#include <array>
#include <string_view>
#include <type_traits>

// Compile-time switch for the work that only feeds traces (choosing and copying the traced
// options, diagnostics strings): with tracing compiled out it cost ~7k fuel a turn for nothing.
#ifdef BB_NO_TRACE
inline constexpr bool kTracing = false;
#else
inline constexpr bool kTracing = true;
#endif
#include <vector>
#include "fixed_str.h"

struct Factor {
    const char* name;  // short human-readable label, e.g. "gems", "travel"
    float value;
    char op;           // '+' adds points, '*' multiplies, 't' is turns (divides the reward)
};

// A note that is dropped: no-trace builds keep options small (see Candidate).
struct NullStr {
    template <class T>
    NullStr& operator<<(const T&) { return *this; }
    NullStr& clear() { return *this; }
    NullStr& sanitized(std::string_view) { return *this; }
    NullStr& point(int, int) { return *this; }
    bool empty() const { return true; }
    std::string_view view() const { return {}; }
    const char* c_str() const { return ""; }
};

// Plain data (no heap) so options can be built and copied cheaply every turn. combatBotv2: the
// factor list and note only feed traces; no-trace builds keep running totals instead (the same
// sums in the same order, so identical scores). An Option was 496 bytes, and the runner charges
// ~1 fuel per byte copied: building, moving and keeping ~20 options a turn cost thousands.
struct Candidate {
    static constexpr int kMaxFactors = 14;
    const char* kind = "";
    FixedStr<24> target;   // short code: g<group>, a<altar>, e(x,y), home, ...
    int x = -1, y = -1;
    float score = 0;
    float reward = 0, time = 0, mult = 1;  // '+', 't' and '*' factors so far
    int added = 0;                         // factors counted (at most kMaxFactors, as the list holds)
    std::array<Factor, kTracing ? kMaxFactors : 0> factors{};
    int factorCount = 0;                   // factors listed (tracing only)
    std::conditional_t<kTracing, FixedStr<48>, NullStr> note;
};

class Trace {
public:
    static constexpr int kMaxCandidates = 8;  // one per option kind (7) plus the chosen one

    void configure(int teamBodies, int slot, const char* champion);
    void legend();  // camp groups and altars, once per team
    // Team plan for the lab: @P[regions,[groupRegion...],[slotRegion...]]
    void plan(int regions, const std::vector<int>& groupRegion, const std::array<int, 8>& slotRegion, int bodies);
    void beginTurn(int round, int slot, const char* champion);
    void state(int x, int y, float hp, float maxHp, int gems, int gold, bool alive);
    // `chosen` must be first; the rest are the best alternatives.
    void candidates(const Candidate* const* list, int count, bool decisionChanged);
    void action(std::string_view text);
    void note(std::string_view text);
    void flush(unsigned long long fuelUsed);
    void lostTurns(int n) { lost_ = n; }
    void forceFull() { full_ = true; }
    bool willWriteFull() const { return full_ && count_ > 0; }
    long lastFullBuild_ = 0, lastFullWrite_ = 0, lastFullBytes_ = 0;  // diagnostics

private:
    int intern(const char* name, char op);
    void write(std::string_view line);
    // One full @T line. A full trace skipped for fuel is kept (with its own round and state) and
    // written on the next turn with room: the turns where new information arrives are the busy
    // ones, and dropping them left the explorer showing breakdowns from 16 rounds earlier.
    struct FullSnapshot {
        int round = 0, slot = 0, lost = 0;
        float hp = 0, maxHp = 0;
        int gems = 0, gold = 0;
        unsigned long long fuelUsed = 0;
        std::array<Candidate, kMaxCandidates> list{};
        int count = 0;
        FixedStr<120> actions;
        FixedStr<240> notes;
    };
    bool writeFull(const FullSnapshot& s);
    FullSnapshot pending_;
    bool hasPending_ = false;

    long budget_ = 180000, written_ = 0;
    int round_ = 0, slot_ = 0, sinceFull_ = 1 << 20, lost_ = 0;
    const char* champion_ = "";
    int x_ = -1, y_ = -1, gems_ = 0, gold_ = 0;
    float hp_ = 0, maxHp_ = 0;
    bool alive_ = false, full_ = false;
    std::array<Candidate, kMaxCandidates> list_{};
    int count_ = 0;
    FixedStr<120> actions_;
    FixedStr<240> notes_;
    struct Interned {
        const char* name;
        char op;
    };
    std::array<Interned, 64> dict_{};
    int dictSize_ = 0;
};

Trace& trace();
