// Second-screen companion for the AYN Thor (dual-screen, Display #4 FLAG_PRESENTATION).
// The game thread writes a GameStateSnapshot every ~6 frames via update(); the Android
// SecondScreenService reads it at 10 Hz via consume_snapshot() from JNI.
#pragma once
#include <atomic>
#include <cstdint>

namespace p5r::second_screen {

struct PartyMember {
    uint8_t  id      = 0;     // member index (0=Joker, 1=Ryuji, 2=Morgana, ...)
    bool     active  = false; // slot is in use
    uint16_t hp      = 0;
    uint16_t hp_max  = 999;
    uint16_t sp      = 0;
    uint16_t sp_max  = 999;
    uint8_t  level   = 1;
};

struct GameStateSnapshot {
    uint64_t    frame    = 0;
    bool        valid    = false; // offsets read at least once without fault
    bool        in_battle = false;
    PartyMember party[4];
};

// Called from the game thread (interval_setter_hook) every ~6 frames.
// base: the loaded game module's base address.
// in_battle: set by scan_tasks().
void update(uint64_t base, bool in_battle);

// Called from JNI (SecondScreenService polling thread).
// Returns a copy of the latest snapshot. Thread-safe.
GameStateSnapshot consume_snapshot();

}  // namespace p5r::second_screen
