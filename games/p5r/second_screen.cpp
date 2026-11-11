// Second-screen companion — game-side data reader.
// Reads party HP/SP/level from P5R v1.0.0 memory every ~6 frames and publishes
// a GameStateSnapshot for the SecondScreenService JNI polling thread.
#include "second_screen.h"
#include "p5r_offsets.h"

#include <algorithm>
#include <mutex>

#include <nx/runtime.h>  // ld<T>()

namespace p5r::second_screen {

namespace {

std::mutex       g_mutex;
GameStateSnapshot g_snapshot;

// Read one unit's stats from the stats array at base+kUnitStats+(idx*kUnitStride).
// Returns false and leaves `out` unchanged when the data looks invalid.
bool read_unit(uint64_t base, int idx, PartyMember& out) {
    using namespace offsets;
    const uint64_t unit = base + kUnitStats + uint64_t(idx) * kUnitStride;

    const uint8_t  kind  = nx::ld<uint8_t>(unit + kUnitKind);
    if (kind != 1) return false;  // placeholder slot

    const uint8_t  id    = nx::ld<uint8_t>(unit + kUnitMemberId);
    const uint16_t hp    = nx::ld<uint16_t>(unit + kUnitHP);
    const uint16_t hpmax = nx::ld<uint16_t>(unit + kUnitHPMax);
    const uint16_t sp    = nx::ld<uint16_t>(unit + kUnitSP);
    const uint16_t spmax = nx::ld<uint16_t>(unit + kUnitSPMax);
    const uint8_t  level = nx::ld<uint8_t>(unit + kUnitLevel);

    // Sanity: reject obviously uninitialized memory.
    if (hp > kStatMax || hpmax > kStatMax || hpmax == 0) return false;
    if (sp > kStatMax || spmax > kStatMax)                return false;
    if (level == 0 || level > 99)                         return false;

    out.id     = id;
    out.active = true;
    out.hp     = hp;
    out.hp_max = hpmax > 0 ? hpmax : kStatMax;
    out.sp     = sp;
    out.sp_max = spmax > 0 ? spmax : kStatMax;
    out.level  = level;
    return true;
}

}  // namespace

void update(uint64_t base, bool in_battle) {
    if (!base) return;
    using namespace offsets;

    GameStateSnapshot snap;
    snap.frame     = 0;  // filled by caller if needed
    snap.in_battle = in_battle;
    snap.valid     = false;

    // Read the 4 active party slots.
    const uint64_t slots_ptr = base + kPartySlots;
    int filled = 0;
    for (int s = 0; s < 4; s++) {
        const uint8_t idx = nx::ld<uint8_t>(slots_ptr + uint64_t(s));
        if (idx >= kMaxUnits) continue;
        PartyMember m{};
        if (read_unit(base, int(idx), m)) {
            snap.party[s] = m;
            filled++;
        }
    }
    if (filled > 0) snap.valid = true;

    std::lock_guard<std::mutex> lk(g_mutex);
    g_snapshot = snap;
}

GameStateSnapshot consume_snapshot() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_snapshot;
}

}  // namespace p5r::second_screen
