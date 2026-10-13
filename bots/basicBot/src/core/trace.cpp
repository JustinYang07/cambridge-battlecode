#include "trace.h"
#include <algorithm>
#include <cambc.h>
#include <cstdio>
#include "config.h"
#include "json.h"
#include "statics.h"

Trace& trace() {
    static Trace instance;
    return instance;
}

namespace {
Json& buffer() {
    static Json j;  // fixed 8 KiB buffer, reused for every line
    return j;
}
} // namespace

void Trace::configure(int teamBodies, int slot, const char* champion) {
    // ~1 MiB per team, shared by every champion's bot instance, with headroom.
    budget_ = 950000L / std::max(teamBodies, 1);
    slot_ = slot;
    auto& j = buffer();
    j.reset();
    j.raw("@U");
    j.array().value(static_cast<long long>(slot)).value(champion).endArray();
    write(j.view());
}

void Trace::write(std::string_view line) {
#ifndef BB_NO_TRACE
    written_ += static_cast<long>(line.size()) + 1;
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
#else
    (void)line;
#endif
}

int Trace::intern(const char* name, char op) {
    for (int i = 0; i < dictSize_; ++i)
        if (dict_[static_cast<std::size_t>(i)].name == name && dict_[static_cast<std::size_t>(i)].op == op) return i;
    if (dictSize_ >= static_cast<int>(dict_.size())) return 0;
    const int id = dictSize_++;
    dict_[static_cast<std::size_t>(id)] = {name, op};
    auto& j = buffer();
    j.reset();
    j.raw("@K");
    const char opText[2] = {op, 0};
    j.array().value(static_cast<long long>(slot_)).value(static_cast<long long>(id)).value(name).value(opText).endArray();
    write(j.view());
    return id;
}

void Trace::legend() {
    // Written once, on slot 0's first turn (which has a doubled fuel budget).
    const auto& s = Statics::get();
    auto& j = buffer();
    for (const auto& g : s.groups) {
        j.reset();
        j.raw("@G");
        j.array().value(static_cast<long long>(g.id)).value(std::string_view(g.label))
            .value(static_cast<long long>(g.center.x)).value(static_cast<long long>(g.center.y)).endArray();
        write(j.view());
    }
    j.reset();
    j.raw("@A");
    j.array();
    for (auto a : s.altars) j.array().value(static_cast<long long>(a.x)).value(static_cast<long long>(a.y)).endArray();
    j.endArray();
    write(j.view());
    std::fflush(stdout);
}

void Trace::beginTurn(int round, int slot, const char* champion) {
    round_ = round;
    slot_ = slot;
    champion_ = champion;
    count_ = 0;
    actions_.clear();
    notes_.clear();
    alive_ = false;
    full_ = false;
}

void Trace::state(int x, int y, float hp, float maxHp, int gems, int gold, bool alive) {
    x_ = x, y_ = y, hp_ = hp, maxHp_ = maxHp, gems_ = gems, gold_ = gold, alive_ = alive;
}

void Trace::candidates(const Candidate* const* list, int count, bool decisionChanged) {
    count_ = std::min(count, std::min(kMaxCandidates, tune::trace_candidates));
    for (int i = 0; i < count_; ++i) list_[static_cast<std::size_t>(i)] = *list[i];
    full_ = full_ || decisionChanged || ++sinceFull_ >= tune::full_trace_every;
}

void Trace::action(std::string_view text) {
    if (!actions_.empty()) actions_ << ';';
    actions_ << text;
}

void Trace::note(std::string_view text) {
    if (!notes_.empty()) notes_ << "; ";
    notes_.sanitized(text);  // notes can carry engine error messages
}

void Trace::flush(unsigned long long fuelUsed) {
#ifndef BB_NO_TRACE
    if (written_ > budget_) return;
    // Output costs ~33 fuel/byte; running dry mid-write suspends the bot and loses the next turn.
    const auto left = cambc::fuel_remaining();
    if (left < tune::fuel_reserve_trace) return;
    auto& j = buffer();
    const char* idleKind = alive_ ? "channel" : "dead";
    if (full_ && count_ > 0 && written_ < budget_ * 8 / 10 && left > tune::fuel_reserve_full_trace) {
        sinceFull_ = 0;
        // Intern first: dictionary lines must precede the line that uses them.
        std::array<int, kMaxCandidates> kinds{};
        std::array<std::array<int, 14>, kMaxCandidates> factorIds{};
        for (int i = 0; i < count_; ++i) {
            const auto& c = list_[static_cast<std::size_t>(i)];
            kinds[static_cast<std::size_t>(i)] = intern(c.kind, 'k');
            for (int k = 0; k < c.factorCount; ++k)
                factorIds[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] =
                    intern(c.factors[static_cast<std::size_t>(k)].name, c.factors[static_cast<std::size_t>(k)].op);
        }
        j.reset();
        j.raw("@T");
        j.array();
        for (long long v : {static_cast<long long>(round_), static_cast<long long>(slot_), static_cast<long long>(hp_),
                            static_cast<long long>(maxHp_), static_cast<long long>(gems_), static_cast<long long>(gold_),
                            static_cast<long long>(fuelUsed), static_cast<long long>(lost_)})
            j.value(v);
        j.array();
        for (int i = 0; i < count_; ++i) {
            const auto& c = list_[static_cast<std::size_t>(i)];
            j.array().value(static_cast<long long>(kinds[static_cast<std::size_t>(i)])).value(c.target.view())
                .value(static_cast<double>(static_cast<int>(c.score * 10.0f)) / 10.0)
                .value(static_cast<long long>(c.x)).value(static_cast<long long>(c.y)).value(c.note.view());
            j.array();
            for (int k = 0; k < c.factorCount; ++k)
                j.array().value(static_cast<long long>(factorIds[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)]))
                    .value(static_cast<double>(c.factors[static_cast<std::size_t>(k)].value)).endArray();
            j.endArray().endArray();
        }
        j.endArray().value(actions_.view()).value(notes_.view()).endArray();
        const auto t1 = cambc::fuel_remaining();
        write(j.view());
        lastFullBuild_ = static_cast<long>(left - t1);
        lastFullWrite_ = static_cast<long>(t1 - cambc::fuel_remaining());
        lastFullBytes_ = static_cast<long>(j.view().size());
    }
    const Candidate* pick = count_ > 0 ? &list_[0] : nullptr;
    const int kind = intern(pick ? pick->kind : idleKind, 'k');
    j.reset();
    j.raw("@t");
    j.array();
    for (long long v : {static_cast<long long>(round_), static_cast<long long>(slot_), static_cast<long long>(x_),
                        static_cast<long long>(y_), static_cast<long long>(hp_), static_cast<long long>(gems_),
                        static_cast<long long>(gold_), static_cast<long long>(kind),
                        static_cast<long long>(pick ? pick->score : 0.0f), static_cast<long long>(fuelUsed)})
        j.value(v);
    j.value(pick ? pick->target.view() : std::string_view("")).value(actions_.view())
        .value(static_cast<long long>(lost_)).endArray();
    write(j.view());
    std::fflush(stdout);
#else
    (void)fuelUsed;
#endif
}
