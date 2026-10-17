#pragma once
// An Option is one thing a champion could commit to this turn. Scorers build
// options with an explicit list of factors (the "points" a human would weigh),
// and the score is always computed from those factors so traces explain it.
//
// Score unit: expected gem-equivalents earned per 100 turns.
// Options are plain data (no heap): building ~40 of them per turn must stay cheap.
#include <cambc.h>
#include <optional>
#include "../core/trace.h"

enum class OptionKind : std::uint8_t { farm, capture, bank, heal, fight, flee, wait };
const char* to_string(OptionKind kind);

struct Option {
    OptionKind kind = OptionKind::wait;
    int target = -1;                       // camp group id or altar index
    cambc::vec2 at{-1, -1};                // where it happens, for traces and movement
    std::optional<cambc::champion> enemy;  // fight / flee subject
    int need = 1;                          // champions needed to take this camp group safely
    float expectDamage = 0;                // farm: health we expect to lose (shown in traces)
    float score = 0;
    Candidate c;                           // label, note and factors, in trace form

    Option& add(const char* name, float value) { return factor(name, value, '+'); }
    Option& turns(const char* name, float value) { return factor(name, value, 't'); }
    Option& mul(const char* name, float value) { return factor(name, value, '*'); }

    // score = (sum of '+' factors) / (sum of 't' factors) * 100 * (product of '*' factors)
    float finish();

private:
    Option& factor(const char* name, float value, char op) {
        if (c.added < Candidate::kMaxFactors) {
            ++c.added;
            if (op == '+') c.reward += value;
            else if (op == 't') c.time += value;
            else c.mult *= value;
            if constexpr (kTracing) c.factors[static_cast<std::size_t>(c.factorCount++)] = {name, value, op};
        }
        return *this;
    }
};
