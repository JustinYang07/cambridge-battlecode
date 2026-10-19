#include "option.h"
#include <algorithm>

const char* to_string(OptionKind kind) {
    switch (kind) {
    case OptionKind::farm: return "farm";
    case OptionKind::capture: return "capture";
    case OptionKind::bank: return "bank";
    case OptionKind::heal: return "heal";
    case OptionKind::fight: return "fight";
    case OptionKind::flee: return "flee";
    case OptionKind::wait: return "wait";
    }
    return "?";
}

float Option::finish() {
    score = c.reward / std::max(c.time, 1.0f) * 100.0f * c.mult;
    c.kind = to_string(kind);
    c.x = at.x;
    c.y = at.y;
    c.score = score;
    if (kind == OptionKind::farm && expectDamage > 0 && c.note.empty())
        c.note << "party " << need << ", lose ~" << static_cast<int>(expectDamage) << " hp";
    return score;
}
