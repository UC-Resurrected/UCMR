// G60 part A: round screen slot state machine, per-frame entry, callee check, film HUD fade setter.
// Written from spec G60_round_screen.md (sections 3, 4.1, 4.2, 4.5, 4.6, 7).

// ---- functions of other groups / the C part (prototypes as the spec gives them)
extern "C" int32_t u3h_pm_type(uintptr_t base, void *s);
extern "C" int u3h_pm_coop(uintptr_t base, void *s);
extern "C" void u3h_ol01_tick(uintptr_t base);
extern "C" void u3h_rc_auto_tick(uintptr_t base);
extern "C" uint8_t *u3x_rp_h(const uint8_t *handle);
extern "C" int u3x_rp_dead(const uint8_t *record);
extern "C" void u3x_r19_draw(uintptr_t base, void *s, uint8_t *obj, int scr);   // part B

namespace g60a {

// image offsets (game facts, spec 3)
constexpr uintptr_t NGM = 0x17cb060;           // net game manager
constexpr uintptr_t ROUND_OBJ = 0x17dd670;     // round-screen object of screen 0; + 0x74 per screen
constexpr uintptr_t MP_BYTE = 0x17d5c61;
constexpr uintptr_t SECOND_PRESENT = 0x17d7b06;
constexpr uintptr_t SECOND_STATE = 0x17f2fc0;
constexpr uintptr_t SELECT_OFF = 0x17d7ab4;
constexpr uintptr_t FORCE_RESULTS = 0x17d7aa4;
constexpr uintptr_t SELECT_FILM_GATE = 0x17c5b70;
constexpr uintptr_t SELECT_ZERO_WORD = 0x17b4a68;
constexpr uintptr_t FRAME_TIME = 0x17b4a90 + 0x11c;
constexpr uintptr_t PROC_HANDLE = 0x1b3ee38;
constexpr uintptr_t PAD_OBJ = 0x16e2fa0;
constexpr uintptr_t MENU_SCR0 = 0x17bd080;
constexpr uintptr_t MENU_SCR1 = 0x17bfa10;
constexpr uintptr_t HUD0_FADE_PTR = 0x1805fa0 + 0x220;

constexpr uintptr_t FN_SWITCH = 0x796640;
constexpr uintptr_t FN_EVENT = 0x1197380;

// ids (string ids of the names in the comments)
constexpr uint32_t ST_REWIND = 0x1f574fac;     // "Rewind"
constexpr uint32_t ST_LEAVE = 0x9f514949;      // "Leave"
constexpr uint32_t EV_WATCH = 0x8a87d1da;      // "get-watch-player-index"
constexpr uint32_t EV_SHOW_HUD = 0xf498c2b5;   // "get-show-hud"
constexpr uint32_t EV_FREE_CAM = 0xe68ae001;   // "get-free-cam"
constexpr uint32_t SLOT3_PAGE = 0xf650325d;

constexpr uint32_t F_FOUR = 0x40800000;
constexpr uint32_t F_TWO = 0x40000000;
constexpr uint32_t F_ZERO = 0;
constexpr uint32_t SIGN = 0x80000000u;

// callee check (spec 3.2): our own guard values = first u32 of each function this group calls
struct Callee { uint32_t off, first; };
constexpr uint32_t PROLOGUE = 0xe5894855;      // push rbp; mov rbp, rsp
U3X_RO const Callee callees[] = {
    {0x624500, PROLOGUE}, {0x627520, PROLOGUE}, {0x658050, 0x0d8d4853}, {0x10dda70, PROLOGUE},
    {0x796640, PROLOGUE}, {0x1197380, PROLOGUE}, {0x63c430, PROLOGUE},
};

// private variables (zero at load, never reset)
U3X_BSS uint8_t callee_result;      // 0 unchecked, 1 all as expected, 2 one differs
U3X_BSS uint8_t slot3_page_set[2];  // per screen

// slot n field offsets in the round-screen object
constexpr uintptr_t on_at(int n) { return 0x08 + 24 * n; }
constexpr uintptr_t alpha_at(int n) { return 0x0c + 24 * n; }
constexpr uintptr_t speed_at(int n) { return 0x10 + 24 * n; }

[[gnu::always_inline]] inline bool f_nan(uint32_t a) { return (a & 0x7fffffffu) > 0x7f800000u; }
[[gnu::always_inline]] inline bool f_ge(uint32_t a, uint32_t b) { return !f_nan(a) && !f_nan(b) && !f_less(a, b); }

// the net game process (may be null), resolved anew at each use
U3X_FN uint8_t *proc(uintptr_t base) { return u3x_rp_h(reinterpret_cast<const uint8_t *>(base + PROC_HANDLE)); }

// process state id: P -> +0x80 -> +0 -> +0x10, u32 at +0x18; 0 when any pointer is bad
U3X_FN uint32_t state_of(const uint8_t *p) {
    if (!ptr_ok(p)) return 0;
    const uint8_t *a = rd<const uint8_t *>(p, 0x80);
    if (!ptr_ok(a)) return 0;
    const uint8_t *b = rd<const uint8_t *>(a, 0);
    if (!ptr_ok(b)) return 0;
    const uint8_t *c = rd<const uint8_t *>(b, 0x10);
    if (!ptr_ok(c)) return 0;
    return rd<uint32_t>(c, 0x18);
}

U3X_FN bool rewinding(uintptr_t base) { return state_of(proc(base)) == ST_REWIND; }

// send an event to the net game process; true when it answered (value in *out)
U3X_FN bool query(uintptr_t base, uint32_t id, uint64_t *out) {
    alignas(16) uint64_t result[3];
    alignas(16) uint64_t box[4][3];
    for (int i = 0; i < 3; i++) {
        result[i] = 0;
        for (int k = 0; k < 4; k++) box[k][i] = 0;
    }
    game_call<void>(base + FN_EVENT, &result[0], static_cast<void *>(nullptr), id, proc(base), &box[0][0], &box[1][0],
                    &box[2][0], &box[3][0]);
    *out = result[0];
    return result[2] != 0;
}

// the game's slot switch, held back for slots 1..3 "on" when eb[0] bit 0x80 is set
U3X_FN void sw(uintptr_t base, uint8_t *obj, uint32_t slot, bool on, bool sound) {
    uint32_t *eb = ext_eb();
    if (slot != 0 && on && (eb[0] & 0x80)) {
        eb[188] += 1;
        return;
    }
    game_call<void>(base + FN_SWITCH, obj, slot, on ? 1u : 0u, sound ? 1u : 0u);
}

// on flags cleared directly: slots 3, 0, 1, 2
U3X_FN void all_off(uint8_t *obj) {
    obj[on_at(3)] = 0;
    obj[on_at(0)] = 0;
    obj[on_at(1)] = 0;
    obj[on_at(2)] = 0;
}

// on flags cleared directly: slots 0, 1, 3
U3X_FN void clear3(uint8_t *obj) {
    obj[on_at(0)] = 0;
    obj[on_at(1)] = 0;
    obj[on_at(3)] = 0;
}

U3X_FN void end_board(uintptr_t base, void *s, uint8_t *obj) {
    if (u3h_pm_type(base, s) == 10) sw(base, obj, 3, true, false);
    else all_off(obj);
}

// slot 2 off, then slot 3 off or the end board when co-op (the "5" phase 2)
U3X_FN void phase2_five(uintptr_t base, void *s, uint8_t *obj) {
    sw(base, obj, 2, false, false);
    if (u3h_pm_coop(base, s) == 0) sw(base, obj, 3, false, false);
    else end_board(base, s, obj);
}

// row S2, steps B1 to B6: dead flag, slots 3 and 0 off after a state 5, Select toggle
U3X_FN void running_round(uintptr_t base, void *s, uint8_t *obj, int scr, bool mp) {
    uint8_t *ngm = reinterpret_cast<uint8_t *>(base + NGM);
    uint8_t *trk = rd<uint8_t *>(ngm, 0x370 + 8 * static_cast<uintptr_t>(scr));
    if (!ptr_ok(trk)) trk = nullptr;
    const bool film = rd<uint8_t>(ngm, 0x6a24) != 0;

    if (film) {
        uint64_t v;
        obj[0] = 0;
        if (query(base, EV_WATCH, &v)) {
            const uint32_t i = static_cast<uint32_t>(v);
            if (mp && i <= 9 && u3x_rp_dead(ngm + 0x390 + 0x9c0 * static_cast<uintptr_t>(i))) obj[0] = 1;
        }
        if (!query(base, EV_SHOW_HUD, &v) || static_cast<uint8_t>(v) == 0) obj[0] = 0;
        if (rewinding(base)) obj[0] = 0;
    } else {
        obj[0] = (trk == nullptr || u3x_rp_dead(trk)) ? 1 : 0;
    }

    if (trk && mp && u3h_pm_coop(base, s) != 0 && rd<int32_t>(trk, 0x21c) == 1 && rd<int32_t>(trk, 0x600) != 0)
        obj[0] = 0;

    if (rd<int32_t>(obj, 0x68) == 5) {
        sw(base, obj, 3, false, false);
        sw(base, obj, 0, false, false);
    }

    // B6 reads the film byte afresh: the calls above may have changed it
    if (at<uint8_t>(base + SELECT_OFF) == 0 &&
        (rd<uint8_t>(ngm, 0x6a24) == 0 || at<uint8_t>(base + SELECT_FILM_GATE) == 0)) {
        const uint8_t *menu = reinterpret_cast<const uint8_t *>(base + (scr != 0 ? MENU_SCR1 : MENU_SCR0));
        const uint8_t *padobj = at<const uint8_t *>(base + PAD_OBJ);
        const uint8_t *pad = ptr_ok(padobj) ? padobj + 0x7e0 + 0x1d8 * static_cast<uintptr_t>(scr) : nullptr;
        if (pad && !(rd<uint32_t>(menu, 0x08) != 0 && rd<uint32_t>(menu, 0x0c) == 1) &&
            !(rd<uint32_t>(pad, 0x9c) & 0x2) && (rd<uint32_t>(pad, 0x94) & 0x100000) &&
            at<uint64_t>(base + SELECT_ZERO_WORD) == 0) {
            *reinterpret_cast<uint32_t *>(obj + speed_at(0)) = F_TWO;
            sw(base, obj, 0, obj[on_at(0)] == 0, true);
            ext_eb()[185] += 1;
        }
    }
}

// alpha step of the four slots (spec 4.2.3), single precision per operation
U3X_FN void alpha_step(uintptr_t base, uint8_t *obj) {
    const uint32_t step = f_mul(at<uint32_t>(base + FRAME_TIME), F_FOUR);
    for (int n = 0; n < 4; n++) {
        uint32_t d = f_mul(step, rd<uint32_t>(obj, speed_at(n)));
        if (obj[on_at(n)] == 0) d ^= SIGN;
        uint32_t v = f_add(rd<uint32_t>(obj, alpha_at(n)), d);
        if (!f_ge(v, F_ZERO)) v = F_ZERO;
        if (f_ge(f_sub(v, F_ONE), F_ZERO)) v = F_ONE;
        *reinterpret_cast<uint32_t *>(obj + alpha_at(n)) = v;
    }
}

}  // namespace g60a

// ---- entry points

extern "C" U3X_FN int u3x_r19_callees(uintptr_t base) {
    using namespace g60a;
    if (callee_result == 0) {
        callee_result = 1;
        for (const Callee &c : callees) {
            if (at<uint32_t>(base + c.off) != c.first) {
                callee_result = 2;
                ext_eb()[189] += 1;
            }
        }
    }
    return callee_result == 1 ? 1 : 0;
}

extern "C" U3X_FN void u3x_r19_tick(uintptr_t base, void *s, uint8_t *obj, int scr) {
    using namespace g60a;
    const bool mp = at<uint8_t>(base + MP_BYTE) != 0;
    const bool split = mp && (at<uint8_t>(base + SECOND_PRESENT) != 0 || at<int32_t>(base + SECOND_STATE) == 3);
    if (!split && scr != 0) return;

    ext_eb()[181] += 1;
    uint8_t *ngm = reinterpret_cast<uint8_t *>(base + NGM);
    const int32_t r = rd<int32_t>(ngm, 0x6808);
    const int32_t type = u3h_pm_type(base, s);

    // phase 1 picks the row; phase 2 follows from the row and R
    if (type != 12 && r == 2) {
        running_round(base, s, obj, scr, mp);
        sw(base, obj, 2, false, false);
        sw(base, obj, 3, false, false);
    } else if (type != 12 && r == 5 && !rewinding(base)) {
        if (rd<int32_t>(ngm, 0x6810) != 3 && u3h_pm_coop(base, s) == 0) {
            sw(base, obj, 0, true, false);
        } else if (u3h_pm_type(base, s) == 10) {
            sw(base, obj, 3, true, false);
        }
        phase2_five(base, s, obj);
    } else {
        if (type == 12) {
            all_off(obj);
        } else {
            // row SX: slot 0 shows the board while the process is leaving an ended match of mode 3 or 8
            const uint8_t *p = proc(base);
            const int32_t md = rd<int32_t>(ngm, 0x680c);
            sw(base, obj, 0, p != nullptr && state_of(p) == ST_LEAVE && (md == 3 || md == 8), false);
        }
        switch (r) {
        case 3:
            if (obj[0x6c] != 0) {
                clear3(obj);
                sw(base, obj, 2, true, false);
            } else {
                sw(base, obj, 2, false, false);
            }
            sw(base, obj, 3, false, false);
            break;
        case 4:
            sw(base, obj, 2, false, false);
            end_board(base, s, obj);
            break;
        case 5:
            phase2_five(base, s, obj);
            break;
        default:
            sw(base, obj, 2, false, false);
            sw(base, obj, 3, false, false);
            break;
        }
    }

    // common tail
    if (at<uint8_t>(base + FORCE_RESULTS) != 0) {
        clear3(obj);
        sw(base, obj, 2, true, false);
    }
    uint64_t v;
    if (query(base, EV_FREE_CAM, &v) && static_cast<uint8_t>(v) != 0) all_off(obj);
    alpha_step(base, obj);
    *reinterpret_cast<int32_t *>(obj + 0x68) = r;
    ext_eb()[190] = rd<uint32_t>(obj, alpha_at(0));
}

extern "C" U3X_FN int u3x_r19_frame(uintptr_t base, void *s) {
    using namespace g60a;
    ext_eb()[180] += 1;
    u3h_ol01_tick(base);
    u3h_rc_auto_tick(base);
    if (u3x_r19_callees(base) == 0) return 0;
    if (at<uint8_t>(base + MP_BYTE) == 0) return 0;
    for (int k = 1; k >= 0; k--) {
        uint8_t *obj = reinterpret_cast<uint8_t *>(base + ROUND_OBJ + 0x74 * static_cast<uintptr_t>(k));
        if (slot3_page_set[k] == 0) {
            slot3_page_set[k] = 1;
            if (rd<uint32_t>(obj, 0x60) == 0) {
                *reinterpret_cast<uint32_t *>(obj + 0x60) = SLOT3_PAGE;
                ext_eb()[191] += 1;
            }
        }
        u3x_r19_tick(base, s, obj, k);
        u3x_r19_draw(base, s, obj, k);
    }
    return 0;
}

// film HUD fade: five stores into screen 0's HUD fade object (pointer not checked)
extern "C" U3X_FN void u3x_rp_hud_fade(uintptr_t base, uint32_t a, uint32_t b, uint32_t c, uint32_t i, uint8_t by) {
    uint8_t *f = at<uint8_t *>(base + g60a::HUD0_FADE_PTR);
    *reinterpret_cast<uint32_t *>(f + 0x14) = c;
    *reinterpret_cast<uint32_t *>(f + 0x04) = i;
    f[0x08] = by;
    *reinterpret_cast<uint32_t *>(f + 0x0c) = a;
    *reinterpret_cast<uint32_t *>(f + 0x10) = b;
}
