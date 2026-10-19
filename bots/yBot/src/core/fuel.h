#pragma once
// Fuel profiler. A turn that exhausts its budget is suspended and finishes on the
// next turn, so the champion effectively acts every other round. Sections record
// how much fuel each stage used; `split()` reports a turn that overran.
//
// Each fuel_remaining() call costs ~12 fuel, so marks are cheap enough to leave on.
#include <string>

namespace fuel {

void beginTurn();               // call first thing in a turn
void mark(const char* section); // charge fuel used since the previous mark to `section`
bool split();                   // the turn overran its budget (round changed mid-turn)
std::string report();           // "refresh 21k think 9k ..." for traces
// Keep this turn's sections (a cheap copy) so the next turn can format them only if it finds
// this one overran; building the report string every turn cost a few thousand fuel.
void keep();
std::string keptReport();
unsigned long long used();      // total since beginTurn (meaningless when split)
// xBot: lean overrun log for untraced builds (compiled in with BB_FUEL_LOG, a no-op otherwise).
// Call once at the end of a turn: running per-section totals for fight and quiet turns, one @O
// line per overrun (with the section that ran out), and an @A summary every 250 rounds.
void logTurn(int round, int slot, const char* who, const char* option, int enemiesNear);

} // namespace fuel
