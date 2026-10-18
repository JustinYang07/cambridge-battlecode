#pragma once
// Roster selection: score every champion we have not picked yet and take the best.
// Emits an "@D{json}" trace line per pick so the lab can show draft reasoning.

void play_draft();
