// P5R v1.0.0 (US BuildId 708BCACA873EA32B) memory offsets for the second-screen companion.
// Source: eden-duo-companions (igawa6/eden-duo-companions), same BuildId.
// All offsets are module-relative (add to the loaded module's base address).
// Marked [verified] when confirmed in-game on the AYN Thor, [unverified] otherwise.
#pragma once
#include <cstdint>

namespace p5r::offsets {

// ---- Party / Unit stats ------------------------------------------------------------------
// Array of unit stat blocks. Stride: kUnitStride bytes per unit. [unverified]
static constexpr uint32_t kUnitStats  = 0x7e3bb0;
static constexpr uint32_t kUnitStride = 0x2a0;

// Active party slot indices (4 x uint8): which unit index is in each party slot. [unverified]
static constexpr uint32_t kPartySlots = 0x880710;

// Max units in the stats array (Joker..Lavenza).
static constexpr int kMaxUnits = 11;

// Unit-relative offsets ------------------------------------------------------------------
static constexpr uint32_t kUnitKind     = 0x04;  // uint8: 1 = active unit, 0 = placeholder
static constexpr uint32_t kUnitMemberId = 0x08;  // uint8: 0=Joker, 1=Ryuji, 2=Morgana ...
static constexpr uint32_t kUnitHP       = 0x0C;  // uint16: current HP (0–999)
static constexpr uint32_t kUnitHPMax    = 0x0E;  // uint16: max HP (0–999) [unverified]
static constexpr uint32_t kUnitSP       = 0x10;  // uint16: current SP (0–999)
static constexpr uint32_t kUnitSPMax    = 0x12;  // uint16: max SP (0–999) [unverified]
static constexpr uint32_t kUnitLevel    = 0x18;  // uint8: level (1–99)

// HP/SP hard cap used for sanity checks.
static constexpr uint16_t kStatMax = 999;

}  // namespace p5r::offsets
