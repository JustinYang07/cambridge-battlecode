#pragma once
// Fighting: damage estimates used by the brain, plus target selection, basic
// attacks and ability casting. Abilities are handled generically from the SDK's
// ability metadata (reset / damage / aoe / stun / heal / shield / dash), so new
// champions need no special-case code.
#include <cambc.h>
#include <optional>
#include "../core/statics.h"
#include "../core/world.h"

namespace combat {

// ---- estimates
float campDamagePerTurn(const World& w, const GroupInfo& g);   // our damage per turn against this group

struct FightEstimate {
    float turns = 0;       // until the whole group is dead
    float damage = 0;      // health we expect to lose
    float hitting = 0;     // monster strength currently able to hit us
};
// Fight `g` with `party` champions of roughly our strength. Uses live health of visible
// members, otherwise the remembered fraction. Pessimistic: monsters mostly focus us.
// `share` is the fraction of monster hits we expect to absorb in a party (<0: role default).
FightEstimate campFight(const World& w, const GroupInfo& g, float healthLeft, int party, float share = -1.0f);
float championDamagePerTurn(const World& w);                     // our damage per turn against a champion
float threatPerTurn(const World& w, cambc::vec2 at);             // enemy champion damage that can reach `at`

// What the brain wants the executor to hit this turn.
struct Focus {
    int groupId = -1;                         // farm this group's camps
    std::optional<cambc::champion> enemy;     // or fight this champion
    bool defend = true;                       // hit enemy champions that are attacking us
    bool anyEnemy = false;                    // any enemy champion in range will do (rangeBot: hold range)
};

// Spend this turn's attack and cast on targets already in range. `beforeMove`
// allows movement-exclusive spells, which forfeit walking this turn.
void act(World& w, const Focus& focus, bool beforeMove);
// Cast support abilities (heals, shields) that do not depend on a target.
void support(World& w);
// A damage spell reaches 4 tiles, beyond basic-attack range (and monster range 3): cast it
// instead of stepping closer. Returns true if a spell was cast.
bool castFromRange(World& w, const Focus& focus);
// Stun or hit an enemy that is tributing or recalling: a stun cancels the channel.
bool interruptChannels(World& w);
// Place an altar lock: on an enemy's tribute (cancels it) or on an enemy-side altar we pass.
bool useAltarLock(World& w);

} // namespace combat
