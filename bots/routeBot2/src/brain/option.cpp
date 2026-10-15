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
    float reward = 0, time = 0, mult = 1;
    for (int i = 0; i < c.factorCount; ++i) {
        const auto& f = c.factors[static_cast<std::size_t>(i)];
        if (f.op == '+') reward += f.value;
        else if (f.op == 't') time += f.value;
        else mult *= f.value;
    }
    score = reward / std::max(time, 1.0f) * 100.0f * mult;
    c.kind = to_string(kind);
    c.x = at.x;
    c.y = at.y;
    c.score = score;
    if (kind == OptionKind::farm && expectDamage > 0 && c.note.empty())
        c.note << "party " << need << ", lose ~" << static_cast<int>(expectDamage) << " hp";
    return score;
}
