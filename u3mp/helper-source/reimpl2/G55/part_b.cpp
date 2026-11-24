// G55 part B: the "+10% <stat>" lines and stat icons of the hat description card, written from spec G55 part B.

extern "C" uint32_t u3h_active_page(uintptr_t base);
extern "C" const char *u3h_text(uintptr_t base, void *s, uint32_t id);
extern "C" uint8_t *u3x_sym_table(void *s, uint32_t id);
extern "C" uint32_t u3x_ppc_ctiwz(uint32_t f);
extern "C" uint8_t *u3x_stat_icon(uintptr_t base, void *s, uint32_t stat, uint32_t m);
extern "C" uint32_t u3h_stat_flag2_id(void);   // the second "flag" stat id (not in the PS4 image)

extern "C" int u3x_hs_text(uintptr_t base, void *s, uint32_t stat, uint32_t m, char *out, uint32_t cap);
extern "C" void u3x_hs_save(uintptr_t base, void *s, uint64_t *regs);
extern "C" void u3x_hs_meas(uintptr_t base, uint32_t scale, const char *text, uint32_t w, uint32_t h, uint32_t flag,
                            uint32_t *out_w, uint32_t *out_h);
extern "C" void u3x_hs_draw(uintptr_t base, void *s, uint64_t *regs);

namespace g55b {

constexpr uintptr_t HAT_ROWS = 0x1b6a7c0;        // u32: rows of the hat list shown
constexpr uintptr_t HAT_TABLE = 0x1b6a7c8;       // pointer to the hat table
constexpr uintptr_t HAT_ROW_INDEX = 0x2208000;   // u32 per list row: index into the hat table
constexpr uintptr_t FN_LAY_OUT = 0x640ac0;
constexpr uintptr_t FN_DRAW_ITEM = 0x63d6f0;
constexpr uint32_t SYM_STAT_TABLE = 0x53ed596au, SYM_HAT_STATS = 0xb8a3581bu, PAGE_HATS = 0x211019f2u;

constexpr uint32_t F_100 = 0x42c80000u, F_005 = 0x3d4ccccdu, F_HALF = 0x3f000000u, F_2 = 0x40000000u,
                   F_4 = 0x40800000u, F_8 = 0x41000000u, F_24 = 0x41c00000u, F_64 = 0x42800000u;

constexpr uint32_t JOINED_SIZE = 384, LINE_SIZE = 96, MAX_LINES = 2;

U3X_RO const char t_plus_space[] = {'+', ' ', 0};
U3X_RO const char t_plus[] = {'+', 0};
U3X_RO const char t_minus[] = {'-', 0};
U3X_RO const char t_space[] = {' ', 0};
U3X_RO const char t_percent[] = {'%', ' ', 0};
U3X_RO const char t_break[] = {'\\', 0};
U3X_RO const char t_indent[] = {' ', ' ', ' ', ' ', ' ', ' ', 0};
U3X_RO const char t_a[] = {'A', 0};
U3X_RO const char t_indent_a[] = {' ', ' ', ' ', ' ', ' ', ' ', 'A', 0};

struct State {
    char joined[JOINED_SIZE];          // description + the stat lines
    uint32_t line_start[MAX_LINES];    // offset of each line in `joined`
    uint32_t stat[MAX_LINES];          // the stat of each line
    uint32_t multiplier[MAX_LINES];    // and its multiplier (float word)
    int32_t lines;                     // lines waiting for the draw hook
    int32_t had_description;           // written, never read
};
U3X_BSS State st;

// a bounded text: `size` includes the NUL, further characters are dropped
struct Text {
    char *p;
    uint32_t size;
    uint32_t n;
};
[[gnu::always_inline]] inline void put(Text &t, char c) {
    if (t.n + 1 < t.size) t.p[t.n++] = c;
}
[[gnu::always_inline]] inline void put(Text &t, const char *s) {
    while (*s) put(t, *s++);
}
U3X_FN void put_number(Text &t, int32_t value) {
    uint32_t u = static_cast<uint32_t>(value);
    if (value < 0) {
        put(t, '-');
        u = 0u - u;
    }
    char digits[10];
    int k = 0;
    do {
        digits[k++] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u);
    while (k) put(t, digits[--k]);
}

// Is `stat` the id the game loads at image offset `at_offset`? (the 32-bit immediate of an instruction whose
// opcode byte comes right before it; another opcode there: no stat matches)
U3X_FN bool stat_at(uintptr_t base, uintptr_t at_offset, uint8_t opcode, uint32_t stat) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(base + at_offset);
    if (p[-1] != opcode) return false;
    const uint32_t id = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                        static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
    return id == stat;
}
[[gnu::always_inline]] inline bool is_flag(uintptr_t base, uint32_t stat) {
    return stat_at(base, 0xc2cb11, 0xbe, stat) || stat == u3h_stat_flag2_id();
}
[[gnu::always_inline]] inline bool is_count(uintptr_t base, uint32_t stat) {
    return stat_at(base, 0xc2b9b6, 0xbe, stat) || stat_at(base, 0x7ad19d, 0xbe, stat) ||
           stat_at(base, 0xd46457, 0xbe, stat);
}
[[gnu::always_inline]] inline bool is_inverse(uintptr_t base, uint32_t stat) {
    return stat_at(base, 0x68a29a, 0xbe, stat) || stat_at(base, 0x68a295, 0xb8, stat) ||
           stat_at(base, 0xd55181, 0xbe, stat);
}

[[gnu::always_inline]] inline void clear(void *block, uint32_t bytes) {
    volatile uint8_t *p = static_cast<volatile uint8_t *>(block);
    for (uint32_t k = 0; k < bytes; ++k) p[k] = 0;
}

// game call with six integers, float register 0 and two stack words
U3X_FN void call(uintptr_t fn, const uint64_t *ints, uint32_t float0, uint64_t stack0, uint64_t stack1) {
    GameCall g;
    clear(&g, sizeof(g));
    g.fn = fn;
    for (int k = 0; k < 6; ++k) g.i[k] = ints[k];
    g.x[0] = float0;
    g.s[0] = stack0;
    g.s[1] = stack1;
    g.al = 1;
    u3x_k26_call(&g);
}

// the game's "lay an item out": fills `item` (0x80 bytes) for a text (kind 1) or an image (kind 2) in a box w x h
U3X_FN void lay_out(uintptr_t base, uint8_t *item, uint32_t kind, const uint8_t *image, uint32_t flag,
                    const char *text, uint32_t w, uint32_t h, uint32_t scale) {
    alignas(16) uint32_t description[12];
    alignas(16) uint32_t box[4];
    alignas(16) uint32_t zeros[4];
    clear(description, sizeof(description));
    clear(box, sizeof(box));
    clear(zeros, sizeof(zeros));
    description[0] = kind;
    *reinterpret_cast<uint64_t *>(description + 2) = reinterpret_cast<uint64_t>(image);
    description[4] = flag;
    description[7] = 0xffffffffu;
    description[9] = 0xffffffffu;
    description[10] = F_ONE;
    box[0] = w;
    box[1] = h;
    const uint64_t ints[6] = {reinterpret_cast<uint64_t>(item),  reinterpret_cast<uint64_t>(description),
                              reinterpret_cast<uint64_t>(text),  reinterpret_cast<uint64_t>(box),
                              reinterpret_cast<uint64_t>(zeros), 0xffffffffu};
    call(base + FN_LAY_OUT, ints, scale, 0, 0);
}

}  // namespace g55b

U3X_FN int u3x_hs_text(uintptr_t base, void *s, uint32_t stat, uint32_t m, char *out, uint32_t cap) {
    using namespace g55b;
    const uint8_t *table = u3x_sym_table(s, SYM_STAT_TABLE);
    if (!table) return 0;
    const uint8_t *entries = rd<const uint8_t *>(table, 8);
    const int32_t count = rd<int32_t>(table, 0);
    if (!entries || count <= 0 || count > 512) return 0;
    const uint8_t *found = nullptr;
    for (int32_t k = 0; k < count; ++k)   // the last matching entry wins
        if (rd<uint32_t>(entries, k * 0x28) == stat) found = entries + k * 0x28;
    if (!found) return 0;
    const char *name = u3h_text(base, s, rd<uint32_t>(found, 0x24));
    if (!name) return 0;

    Text t{out, cap, 0};
    if (is_flag(base, stat)) {
        put(t, t_plus_space);
    } else if (is_count(base, stat)) {
        put(t, t_plus);
        put_number(t, static_cast<int32_t>(u3x_ppc_ctiwz(m)));
        put(t, t_space);
    } else {   // a percentage above or below 1.0
        if (is_inverse(base, stat)) m = f_div(F_ONE, m);
        const bool lower = f_less(m, F_ONE);
        const uint32_t change = lower ? f_sub(F_ONE, m) : f_sub(m, F_ONE);
        put(t, lower ? t_minus : t_plus);
        put_number(t, static_cast<int32_t>(u3x_ppc_ctiwz(f_mul(change, F_100))));
        put(t, t_percent);
    }
    put(t, name);
    out[t.n] = 0;
    return 1;
}

U3X_FN void u3x_hs_save(uintptr_t base, void *s, uint64_t *regs) {
    using namespace g55b;
    if (u3h_active_page(base) != PAGE_HATS) return;

    // the selected list row -> the hat's item id
    const int32_t row = static_cast<int32_t>(regs[3]);
    const uint32_t rows = at<uint32_t>(base + HAT_ROWS);
    if (row < 0 || static_cast<uint32_t>(row) >= rows || rows > 96) return;
    const uint32_t index = at<uint32_t>(base + HAT_ROW_INDEX + 4 * static_cast<uintptr_t>(row));
    const uint8_t *hats = at<const uint8_t *>(base + HAT_TABLE);
    if (!hats || index >= rd<uint32_t>(hats, 0)) return;
    const uint8_t *hat_entries = rd<const uint8_t *>(hats, 8);
    if (!hat_entries) return;
    const uint32_t item_id = rd<uint32_t>(hat_entries, static_cast<uintptr_t>(index) * 0x38);

    const uint8_t *table = u3x_sym_table(s, SYM_HAT_STATS);
    if (!table) return;
    const uint8_t *stat_rows = rd<const uint8_t *>(table, 8);
    const int32_t count = rd<int32_t>(table, 0);
    if (!stat_rows || count <= 0 || count > 4096) return;

    // the joined text: the description, then one indented line per stat
    const char *description = reinterpret_cast<const char *>(regs[13]);
    Text joined{st.joined, JOINED_SIZE, 0};
    if (description && *description) put(joined, description);

    int32_t lines = 0;
    for (int32_t k = 0; k < count && lines < static_cast<int32_t>(MAX_LINES); ++k) {
        const uint8_t *stat_row = stat_rows + k * 16;
        if (rd<uint32_t>(stat_row, 0) != item_id) continue;
        const uint8_t *pairs = rd<const uint8_t *>(stat_row, 8);
        if (!pairs) continue;
        const int32_t pair_count = rd<int32_t>(stat_row, 4);
        for (int32_t j = 0; j < pair_count && lines < static_cast<int32_t>(MAX_LINES); ++j) {
            const uint32_t stat = rd<uint32_t>(pairs, j * 8);
            const uint32_t multiplier = rd<uint32_t>(pairs, j * 8 + 4);
            char line[LINE_SIZE];
            if (!u3x_hs_text(base, s, stat, multiplier, line, LINE_SIZE)) continue;
            if (joined.n) put(joined, t_break);
            st.line_start[lines] = joined.n;
            put(joined, t_indent);
            put(joined, line);
            st.stat[lines] = stat;
            st.multiplier[lines] = multiplier;
            ++lines;
        }
    }
    st.joined[joined.n] = 0;

    if (lines > 0) {
        regs[13] = reinterpret_cast<uint64_t>(st.joined);
        st.lines = lines;
        st.had_description = (description && *description) ? 1 : 0;   // first byte as it is now, after the loop
    }
}

U3X_FN void u3x_hs_meas(uintptr_t base, uint32_t scale, const char *text, uint32_t w, uint32_t h, uint32_t flag,
                        uint32_t *out_w, uint32_t *out_h) {
    using namespace g55b;
    alignas(16) uint8_t item[0x80];
    clear(item, sizeof(item));
    lay_out(base, item, 1, nullptr, flag, text, w, h, scale);
    *out_w = rd<uint32_t>(item, 0x3c);
    *out_h = rd<uint32_t>(item, 0x40);
}

U3X_FN void u3x_hs_draw(uintptr_t base, void *s, uint64_t *regs) {
    using namespace g55b;
    const int32_t lines = st.lines;
    st.lines = 0;
    if (lines <= 0) return;

    // the frame of the game's card function
    uint8_t *frame = reinterpret_cast<uint8_t *>(regs[14]);
    const uint64_t *ui_object = reinterpret_cast<const uint64_t *>(regs[2]);
    const uint32_t *text_rect = reinterpret_cast<const uint32_t *>(frame - 0x180);
    const uint32_t *text_style = reinterpret_cast<const uint32_t *>(frame - 0xa8);
    uint8_t *item = frame - 0x118;
    // R is read where it is used: width and height before every measurement, x after the first three
    // measurements, y per line after that line's icon query and measurement

    // the caller's style, white and without shadow
    alignas(16) uint32_t style[10];
    for (int k = 0; k < 10; ++k) style[k] = rd<uint32_t>(frame - 0x58, 4 * k);
    style[1] = style[2] = style[3] = F_ONE;
    style[5] = 0;

    const uint32_t scale =
        f_mul(f_add(f_mul(f_mul(f_mul(text_style[6], text_style[7]), text_style[4]), F_005), F_ONE), text_style[0]);

    // line height and the width of the indent, measured with the game's own layout
    uint32_t line_height, width_a, width_indent_a, unused;
    u3x_hs_meas(base, scale, t_a, text_rect[2], text_rect[3], 1, &unused, &line_height);
    u3x_hs_meas(base, scale, t_a, text_rect[2], text_rect[3], 0, &width_a, &unused);
    u3x_hs_meas(base, scale, t_indent_a, text_rect[2], text_rect[3], 0, &width_indent_a, &unused);
    uint32_t indent = f_sub(width_indent_a, width_a);
    if (f_less(line_height, F_8) || f_less(F_64, line_height)) line_height = F_24;
    if (f_less(indent, F_4) || f_less(F_64, indent)) indent = F_24;

    const uint32_t room = f_sub(indent, F_2);
    const uint32_t icon_size = f_less(room, line_height) ? room : line_height;
    const uint32_t icon_x = f_add(text_rect[0],f_mul(f_sub(indent, icon_size), F_HALF));

    for (int32_t i = 0; i < lines; ++i) {
        const uint8_t *icon = u3x_stat_icon(base, s, st.stat[i], st.multiplier[i]);
        if (!icon) continue;

        // the text up to this line plus one letter: its height gives the line's bottom
        char prefix[JOINED_SIZE + 2];
        const uint32_t start = st.line_start[i];
        for (uint32_t k = 0; k < start; ++k) prefix[k] = st.joined[k];
        prefix[start] = t_a[0];
        prefix[start + 1] = 0;
        uint32_t prefix_height;
        u3x_hs_meas(base, scale, prefix, text_rect[2], text_rect[3], 1, &unused, &prefix_height);
        const uint32_t y =
            f_add(f_add(text_rect[1],f_sub(prefix_height, line_height)), f_mul(f_sub(line_height, icon_size), F_HALF));

        lay_out(base, item, 2, icon, 0, nullptr, icon_size, icon_size, style[0]);

        alignas(16) uint32_t icon_rect[4] = {icon_x, y, icon_size, icon_size};
        const uint64_t ints[6] = {ui_object[0],
                                  rd<uint64_t>(frame - 0x198, 0),
                                  reinterpret_cast<uint64_t>(style),
                                  reinterpret_cast<uint64_t>(item),
                                  reinterpret_cast<uint64_t>(icon_rect),
                                  0};
        call(base + FN_DRAW_ITEM, ints, 0, 0, 0xffffffffu);
    }
}
