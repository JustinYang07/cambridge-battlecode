#pragma once
#include <cambc.h>
class Bot {
    bool init = false;
    int slot = -1;
    float range = 0;
    int cd = 0;
    int phase = 0;  // 0 walk to the pocket edge, 1 inside, 2 back out
    cambc::bitboard reach, pocket;
public:
    void play_turn();
};
