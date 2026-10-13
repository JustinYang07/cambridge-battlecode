#include "brain.h"
#include "scorers.h"
#include "../core/fuel.h"

namespace brain {

std::vector<Option> think(World& w) {
    std::vector<Option> options;
    options.reserve(Statics::get().groups.size() + 8);
    scorers::farm(w, options);
    fuel::mark("think.farm");
    scorers::bank(w, options);
    scorers::survive(w, options);
    scorers::fight(w, options);
    scorers::idle(w, options);
    fuel::mark("think.other");
    return options;
}

int choose(const std::vector<Option>& options) {
    int best = -1;
    for (int i = 0; i < static_cast<int>(options.size()); ++i)
        if (best < 0 || options[static_cast<std::size_t>(i)].score > options[static_cast<std::size_t>(best)].score)
            best = i;
    return best;
}

} // namespace brain
