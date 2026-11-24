// S09 part B: small game rules, C++ exports re-implemented in place (spec S09_small_rules.md, 2.1 part B, 4.10..4.21).

// ---- functions of other groups and of the C part (S09 2.2)
extern "C" int32_t u3h_kr_mode_type(uintptr_t base);
extern "C" int u3h_kr_30feec(uintptr_t base);
extern "C" int32_t u3x_kr_dc_int(uintptr_t base, uint32_t off);
extern "C" uint8_t *u3x_rp_h(const uint8_t *h);

// The helper's replica of the game's bit stream (G11, S09 3.6). The merged file already defines it; this copy is only
// for building this part on its own (the integrator drops it).
#ifndef U3X_HAVE_GTSTREAM
struct GtStream {
    uint32_t *words;     // +0
    uint32_t pos;        // +8  bit position
    uint32_t cap;        // +0xc capacity in bits
    uint32_t w10;        // +0x10
    uint32_t w14;        // +0x14
};
#endif

namespace s09b {

// image offsets (S09 3.3, 3.4)
constexpr uintptr_t STATS_MODE_MASK = 0x67c2b0;   // u32 immediate of the game's own excluded-mode test
constexpr uintptr_t SPLIT_BYTE = 0x17d7b06;
constexpr uintptr_t SPLIT_WORD = 0x17f2fc0;

// single values (S09 3.5)
constexpr int SOUND_PICKED = 0;
constexpr int SOUND_REFUSED = 8;
constexpr int32_t MODE_TYPE_DC = 0xf;
constexpr uint32_t DC_OFF_A = 4;
constexpr uint32_t DC_OFF_B = 0xc;
constexpr int32_t DEFAULT_A = 3;
constexpr int32_t DEFAULT_B = 0;
constexpr int32_t SPLIT_WORD_SECOND = 3;

}  // namespace s09b

// 4.10: loadout list click sound
extern "C" U3X_FN int u3x_click_sound(int unlock_status) {
    return (unlock_status == 0 || unlock_status == 1) ? s09b::SOUND_PICKED : s09b::SOUND_REFUSED;
}

// 4.11: the booster shown (slot word, else the player's word) is already this one
extern "C" U3X_FN int u3x_booster_already(uint32_t slot_word, uint32_t player_word, uint32_t booster) {
    const uint32_t shown = slot_word != 0 ? slot_word : player_word;
    return shown == booster ? 1 : 0;
}

// 4.12: friend list group: in this game 0, online elsewhere 1, offline 2
extern "C" U3X_FN int u3x_friend_group(const uint8_t *entry) {
    const int32_t presence = rd<int32_t>(entry, 0x1b0);
    if (presence == 1) return 0;
    if (presence == 2) return 1;
    return 2;
}

// 4.13: progress in percent between two thresholds (32-bit wrapping arithmetic, signed truncating division)
extern "C" U3X_FN int32_t u3x_xp_percent(int32_t low, int32_t high, int32_t xp) {
    if (xp < low || high == low) return 0;
    const int32_t done = static_cast<int32_t>(static_cast<uint32_t>(xp) - static_cast<uint32_t>(low));
    const int32_t span = static_cast<int32_t>(static_cast<uint32_t>(high) - static_cast<uint32_t>(low));
    const int32_t scaled = static_cast<int32_t>(static_cast<uint32_t>(done) * 100u);
    return scaled / span;
}

// 4.14: statistics page shown for this ended match (excluded modes read from the game's own mask)
extern "C" U3X_FN int u3x_match_stats_shown(uintptr_t base, uint32_t mode, int flag) {
    const uint32_t excluded = at<uint32_t>(base + s09b::STATS_MODE_MASK);
    if (mode < 32 && ((excluded >> mode) & 1u)) return 0;
    if (flag != 0) return 0;
    return 1;
}

// 4.15: a late joiner that has not spawned this round yet
extern "C" U3X_FN int u3x_waiting_joiner(const uint8_t *net_player) {
    return (rd<uint8_t>(net_player, 0xe1) != 0 && rd<uint8_t>(net_player, 0x5e4) == 0 &&
            rd<uint8_t>(net_player, 0x5f9) != 0 && rd<uint8_t>(net_player, 0x5fa) == 0) ? 1 : 0;
}

// 4.16: joined and not leaving
extern "C" U3X_FN int u3x_net_player_in_game(const uint8_t *np) {
    return (rd<uint8_t>(np, 0xe1) != 0 && rd<uint8_t>(np, 0x5e4) == 0) ? 1 : 0;
}

// 4.17: datagram length in bytes: used bits (at most the capacity) rounded up to whole 32-bit words
extern "C" U3X_FN uint32_t u3x_gt_bytes(const GtStream *s) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(s);
    uint32_t used = rd<uint32_t>(p, 8);
    const uint32_t cap = rd<uint32_t>(p, 0xc);
    if (used > cap) used = cap;
    return 4u * ((used + 31u) / 32u);
}

// 4.18: this machine hosts the session (no room yet counts as host)
extern "C" U3X_FN int u3x_session_host(const uint8_t *s) {
    if (rd<uint64_t>(s, 0x18) == 0) return 1;
    return rd<uint32_t>(s, 0x12e4) == rd<uint32_t>(s, 0x12e8) ? 1 : 0;
}

// 4.19: kill score values
extern "C" U3X_FN int u3x_kr_34fc04(uintptr_t base) {
    return u3h_kr_mode_type(base) == s09b::MODE_TYPE_DC ? 1 : 0;
}

extern "C" U3X_FN int32_t u3x_kr_37c984(uintptr_t base) {
    if (u3x_kr_34fc04(base) != 0) return u3x_kr_dc_int(base, s09b::DC_OFF_A);
    return s09b::DEFAULT_A;
}

extern "C" U3X_FN int32_t u3x_kr_37c914(uintptr_t base) {
    if (u3x_kr_34fc04(base) != 0) return u3x_kr_dc_int(base, s09b::DC_OFF_B);
    return s09b::DEFAULT_B;
}

extern "C" U3X_FN int u3x_kr_30ffa8(uintptr_t base, const uint8_t *np, int b) {
    if (u3h_kr_30feec(base) == 0) return 1;
    return np != nullptr ? b : 0;
}

// 4.20: a second local player is present
extern "C" U3X_FN int u3x_split_screen(uintptr_t base) {
    if (at<uint8_t>(base + s09b::SPLIT_BYTE) != 0) return 1;
    return at<int32_t>(base + s09b::SPLIT_WORD) == s09b::SPLIT_WORD_SECOND ? 1 : 0;
}

// 4.21: film trackers
extern "C" U3X_FN int u3x_rp_trk_live(uint8_t *t) {
    if (u3x_rp_h(t + 0x10) != nullptr) return 1;
    return u3x_rp_h(t + 0x20) != nullptr ? 1 : 0;
}

extern "C" U3X_FN int u3x_rp_dead(const uint8_t *t) {
    return rd<int32_t>(t, 0x3a0) < rd<int32_t>(t, 0x39c) ? 1 : 0;
}
