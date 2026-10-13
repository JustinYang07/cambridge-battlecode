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
unsigned long long used();      // total since beginTurn (meaningless when split)

} // namespace fuel
