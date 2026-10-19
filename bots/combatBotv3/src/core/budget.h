#pragma once
// combatBotv3 fuel governor. A turn that runs out of fuel is suspended and loses the next turn, and
// the stage that runs out is nearly always the last one: execute (pathing, spells), after scoring
// has spent what it liked. So each champion keeps a running estimate of what its execute costs
// (a slowly decaying maximum, kept apart for turns with an enemy in sight, which cost more), and
// scoring stops while that much plus the turn's tail is still left.
#include <cambc.h>

namespace budget {

// Record what this turn's execute cost (contact: an enemy champion was in sight).
void noteExecute(unsigned long long cost, bool contact);
// Fuel to keep for execute and everything after it this turn.
unsigned long long reserve(bool contact);
// Fuel still free for optional work (scoring) above that reserve; 0 when there is none.
unsigned long long spare(bool contact);
// Farm scoring: record its bound pass, and what a farm scoring turn needs (bound pass plus one camp).
void noteFarmPre(unsigned long long cost);
unsigned long long farmNeed();
// What everything after farm scoring costs (bank, survive, choose, execute, wards, bookkeeping),
// measured: markFarmDone() where farm scoring ends (or would have), noteTurnEnd() at the end of
// the turn. farmTail() is the running estimate (a slowly decaying maximum).
void markFarmDone();
void noteTurnEnd(bool contact);
unsigned long long farmTail(bool contact);

} // namespace budget
