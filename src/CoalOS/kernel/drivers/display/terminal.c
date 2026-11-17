/**
 * @file terminal.c
 * @brief VGA text console: character cells, scrolling and a VT100/ANSI subset
 *        (cursor movement, erase, insert/delete, SGR colours).
 *
 * Input and line editing live in tty.c.
 */
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/vga_hardware.h>
#include <kernel/lib/port_io.h>
#include <kernel/lib/string.h>
#include <kernel/sync/spinlock.h>

#define TAB_WIDTH       8
#define MAX_PARAMS      4
#define DEFAULT_FG      VGA_COLOR_LIGHT_GREY
#define DEFAULT_BG      VGA_COLOR_BLACK

static volatile uint16_t *const vga = (volatile uint16_t *)VGA_ADDRESS;

typedef enum { ST_NORMAL, ST_ESC, ST_CSI } vt_state_t;

static struct {
    int x, y;
    int saved_x, saved_y;
    uint8_t fg, bg;
    bool bold, reverse, cursor_visible;
    uint8_t attr;
    vt_state_t state;
    int params[MAX_PARAMS];
    int nparams;
    bool private_mode;
    int hw_pos; /* last cursor position written to the CRTC, -1 = unknown */
} t = { .fg = DEFAULT_FG, .bg = DEFAULT_BG, .cursor_visible = true, .hw_pos = -1 };

static spinlock_t g_lock;

/* Latin-1 0xA0..0xFF to the VGA code page (CP437). Letters CP437 lacks fall back to the
 * nearest look-alike; 0x80..0x9F are control codes and show as '?'. */
static const uint8_t latin1_to_cp437[96] = {
    0x20,0xAD,0x9B,0x9C,0x0F,0x9D,0x7C,0x15,0x22,'C',0xA6,0xAE,0xAA,'-','R','-',
    0xF8,0xF1,0xFD,'3',0x27,0xE6,0x14,0xFA,',','1',0xA7,0xAF,0xAC,0xAB,'?',0xA8,
    'A','A','A','A',0x8E,0x8F,0x92,0x80,'E',0x90,'E','E','I','I','I','I',
    'D',0xA5,'O','O','O','O',0x99,'x',0xE8,'U','U','U',0x9A,'Y','P',0xE1,
    0x85,0xA0,0x83,'a',0x84,0x86,0x91,0x87,0x8A,0x82,0x88,0x89,0x8D,0xA1,0x8C,0x8B,
    'd',0xA4,'o',0xA2,0x93,'o',0x94,0xF6,0xED,0x97,0xA3,0x96,0x81,'y','p',0x98,
};

static inline uint16_t cell(uint8_t glyph, uint8_t attr) {
    return (uint16_t)(glyph | (attr << 8));
}

static uint8_t glyph_for(uint8_t c) {
    if (c < 0x7F) return c;
    if (c >= 0xA0) return latin1_to_cp437[c - 0xA0];
    return '?';
}

/* ---- Cursor and attributes ------------------------------------------- */

static void update_attr(void) {
    uint8_t fg = t.fg | (t.bold ? 8 : 0);
    t.attr = t.reverse ? VGA_RGB(t.bg, fg) : VGA_RGB(fg, t.bg);
}

static void hw_cursor_update(void) {
    int pos = t.cursor_visible ? t.y * VGA_COLS + t.x : VGA_ROWS * VGA_COLS; /* off-screen hides it */
    if (pos == t.hw_pos) return;
    t.hw_pos = pos;
    outb(VGA_CMD_PORT, VGA_REG_CURSOR_LO); outb(VGA_DATA_PORT, (uint8_t)pos);
    outb(VGA_CMD_PORT, VGA_REG_CURSOR_HI); outb(VGA_DATA_PORT, (uint8_t)(pos >> 8));
}

static void hw_cursor_init(void) {
    outb(VGA_CMD_PORT, VGA_REG_CURSOR_START); outb(VGA_DATA_PORT, 14); /* underline, on */
    outb(VGA_CMD_PORT, VGA_REG_CURSOR_END);   outb(VGA_DATA_PORT, 15);
}

/* ---- Screen operations ------------------------------------------------ */

static void fill_cells(int from, int count, uint16_t value) {
    for (int i = 0; i < count; i++) vga[from + i] = value;
}

static void scroll_up(void) {
    memmove((void *)vga, (const void *)(vga + VGA_COLS), (VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));
    fill_cells((VGA_ROWS - 1) * VGA_COLS, VGA_COLS, cell(' ', t.attr));
}

static void line_feed(void) {
    if (++t.y >= VGA_ROWS) {
        scroll_up();
        t.y = VGA_ROWS - 1;
    }
}

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void move_to(int x, int y) {
    t.x = clamp(x, 0, VGA_COLS - 1);
    t.y = clamp(y, 0, VGA_ROWS - 1);
}

static void clear_screen_region(int mode) {
    int cur = t.y * VGA_COLS + t.x, all = VGA_ROWS * VGA_COLS;
    uint16_t blank = cell(' ', t.attr);
    if (mode == 0)      fill_cells(cur, all - cur, blank);
    else if (mode == 1) fill_cells(0, cur + 1, blank);
    else                fill_cells(0, all, blank);
}

static void clear_line_region(int mode) {
    int row = t.y * VGA_COLS;
    uint16_t blank = cell(' ', t.attr);
    if (mode == 0)      fill_cells(row + t.x, VGA_COLS - t.x, blank);
    else if (mode == 1) fill_cells(row, t.x + 1, blank);
    else                fill_cells(row, VGA_COLS, blank);
}

/* Inserts (n > 0) or deletes (n < 0) lines at the cursor row. */
static void shift_lines(int n) {
    int rows_below = VGA_ROWS - t.y;
    int cnt = clamp(n < 0 ? -n : n, 0, rows_below);
    int row = t.y * VGA_COLS;
    if (n > 0) {
        memmove((void *)(vga + row + cnt * VGA_COLS), (const void *)(vga + row),
                (rows_below - cnt) * VGA_COLS * sizeof(uint16_t));
        fill_cells(row, cnt * VGA_COLS, cell(' ', t.attr));
    } else {
        memmove((void *)(vga + row), (const void *)(vga + row + cnt * VGA_COLS),
                (rows_below - cnt) * VGA_COLS * sizeof(uint16_t));
        fill_cells((VGA_ROWS - cnt) * VGA_COLS, cnt * VGA_COLS, cell(' ', t.attr));
    }
}

/* Inserts (n > 0) or deletes (n < 0) characters at the cursor, within the line. */
static void shift_chars(int n) {
    int cnt = clamp(n < 0 ? -n : n, 0, VGA_COLS - t.x);
    volatile uint16_t *p = vga + t.y * VGA_COLS + t.x;
    int rest = VGA_COLS - t.x - cnt;
    if (n > 0) {
        memmove((void *)(p + cnt), (const void *)p, rest * sizeof(uint16_t));
        fill_cells(t.y * VGA_COLS + t.x, cnt, cell(' ', t.attr));
    } else {
        memmove((void *)p, (const void *)(p + cnt), rest * sizeof(uint16_t));
        fill_cells(t.y * VGA_COLS + VGA_COLS - cnt, cnt, cell(' ', t.attr));
    }
}

/* ---- VT100 parser ----------------------------------------------------- */

static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void sgr(int p) {
    if (p == 0)                { t.fg = DEFAULT_FG; t.bg = DEFAULT_BG; t.bold = t.reverse = false; }
    else if (p == 1)           t.bold = true;
    else if (p == 22)          t.bold = false;
    else if (p == 7)           t.reverse = true;
    else if (p == 27)          t.reverse = false;
    else if (p >= 30 && p <= 37)   t.fg = ansi_to_vga[p - 30];
    else if (p == 39)          t.fg = DEFAULT_FG;
    else if (p >= 40 && p <= 47)   t.bg = ansi_to_vga[p - 40];
    else if (p == 49)          t.bg = DEFAULT_BG;
    else if (p >= 90 && p <= 97)   t.fg = ansi_to_vga[p - 90] | 8;
    else if (p >= 100 && p <= 107) t.bg = ansi_to_vga[p - 100] | 8;
}

/* Parameter i, or def when absent or zero. */
static int param(int i, int def) {
    return (i < t.nparams && t.params[i] > 0) ? t.params[i] : def;
}

static void csi_dispatch(uint8_t final) {
    switch (final) {
    case 'A': move_to(t.x, t.y - param(0, 1)); break;
    case 'B': move_to(t.x, t.y + param(0, 1)); break;
    case 'C': move_to(t.x + param(0, 1), t.y); break;
    case 'D': move_to(t.x - param(0, 1), t.y); break;
    case 'E': move_to(0, t.y + param(0, 1)); break;
    case 'F': move_to(0, t.y - param(0, 1)); break;
    case 'G': move_to(param(0, 1) - 1, t.y); break;
    case 'd': move_to(t.x, param(0, 1) - 1); break;
    case 'H': case 'f': move_to(param(1, 1) - 1, param(0, 1) - 1); break;
    case 'J': clear_screen_region(t.nparams ? t.params[0] : 0); break;
    case 'K': clear_line_region(t.nparams ? t.params[0] : 0); break;
    case 'L': shift_lines(param(0, 1)); break;
    case 'M': shift_lines(-param(0, 1)); break;
    case '@': shift_chars(param(0, 1)); break;
    case 'P': shift_chars(-param(0, 1)); break;
    case 'X': {
        int cnt = clamp(param(0, 1), 0, VGA_COLS - t.x);
        fill_cells(t.y * VGA_COLS + t.x, cnt, cell(' ', t.attr));
        break;
    }
    case 'm':
        if (t.nparams == 0) sgr(0);
        for (int i = 0; i < t.nparams; i++) sgr(t.params[i]);
        update_attr();
        break;
    case 's': t.saved_x = t.x; t.saved_y = t.y; break;
    case 'u': move_to(t.saved_x, t.saved_y); break;
    case 'h': case 'l':
        if (t.private_mode && t.nparams && t.params[0] == 25) t.cursor_visible = (final == 'h');
        break;
    default: break; /* unsupported sequences are ignored */
    }
}

static void esc_input(uint8_t c) {
    if (t.state == ST_ESC) {
        t.state = ST_NORMAL;
        switch (c) {
        case '[': t.state = ST_CSI; t.nparams = 0; t.private_mode = false;
                  for (int i = 0; i < MAX_PARAMS; i++) t.params[i] = 0;
                  break;
        case '7': t.saved_x = t.x; t.saved_y = t.y; break;
        case '8': move_to(t.saved_x, t.saved_y); break;
        case 'c': t.fg = DEFAULT_FG; t.bg = DEFAULT_BG; t.bold = t.reverse = false;
                  t.cursor_visible = true; update_attr();
                  clear_screen_region(2); move_to(0, 0); break;
        default: break;
        }
        return;
    }
    /* ST_CSI */
    if (c == '?') {
        t.private_mode = true;
    } else if (c >= '0' && c <= '9') {
        if (t.nparams == 0) t.nparams = 1;
        int *p = &t.params[t.nparams - 1];
        if (*p < 10000) *p = *p * 10 + (c - '0');
    } else if (c == ';') {
        if (t.nparams == 0) t.nparams = 1; /* empty first parameter */
        if (t.nparams < MAX_PARAMS) t.nparams++;
    } else if (c >= 0x40 && c <= 0x7E) {
        t.state = ST_NORMAL;
        csi_dispatch(c);
    } else if (c < 0x20 || c > 0x7E) {
        t.state = ST_NORMAL; /* malformed: abort the sequence */
    }
}

static void vt_write(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (t.state != ST_NORMAL) { esc_input(c); continue; }

        /* UTF-8 encoded U+0080..U+00FF (FAT long names, text files) maps onto Latin-1 */
        if ((c == 0xC2 || c == 0xC3) && i + 1 < n && (s[i + 1] & 0xC0) == 0x80) {
            c = (uint8_t)(((c & 0x03) << 6) | (s[++i] & 0x3F));
        }

        if (c >= 0x20 && c != 0x7F) {
            vga[t.y * VGA_COLS + t.x] = cell(glyph_for(c), t.attr);
            if (++t.x >= VGA_COLS) { t.x = 0; line_feed(); }
            continue;
        }
        switch (c) {
        case '\n': case '\v': case '\f': t.x = 0; line_feed(); break;
        case '\r': t.x = 0; break;
        case '\b': if (t.x > 0) t.x--; break;
        case '\t': t.x = clamp((t.x / TAB_WIDTH + 1) * TAB_WIDTH, 0, VGA_COLS - 1); break;
        case 0x1B: t.state = ST_ESC; break;
        default: break;
        }
    }
}

/* ---- Public API ------------------------------------------------------- */

static void write_locked(const char *data, size_t n) {
    uintptr_t flags = spinlock_acquire_irqsave(&g_lock);
    vt_write((const uint8_t *)data, n);
    hw_cursor_update();
    spinlock_release_irqrestore(&g_lock, flags);
}

void terminal_write_bytes(const char *data, size_t size) {
    if (!data || !size) return;
    write_locked(data, size);
}

static void clear_all(void) {
    uintptr_t flags = spinlock_acquire_irqsave(&g_lock);
    clear_screen_region(2);
    move_to(0, 0);
    hw_cursor_update();
    spinlock_release_irqrestore(&g_lock, flags);
}

/* CP437 has no o-slash. Derive it from 'o'/'O' plus a diagonal stroke and store it in the
 * slots of the look-alike glyphs that latin1_to_cp437[] points to. */
static uint8_t vga_reg_swap(uint16_t index_port, uint8_t reg, int new_val) {
    uint8_t old;

    outb(index_port, reg);
    old = inb(index_port + 1);
    if (new_val >= 0) outb(index_port + 1, (uint8_t)new_val);
    return old;
}

static void make_slashed(volatile uint8_t *font, uint8_t base, uint8_t slot) {
    uint8_t g[VGA_FONT_HEIGHT];
    int first = -1, last = 0;

    for (int r = 0; r < VGA_FONT_HEIGHT; r++) {
        g[r] = font[base * VGA_FONT_STRIDE + r];
        if (g[r]) { if (first < 0) first = r; last = r; }
    }
    if (first < 0) return;
    for (int r = first; r <= last; r++)             /* bottom-left to top-right */
        g[r] |= (uint8_t)(0x80 >> (1 + (last - r) * 5 / (last - first ? last - first : 1)));
    for (int r = 0; r < VGA_FONT_HEIGHT; r++)
        font[slot * VGA_FONT_STRIDE + r] = g[r];
}

static void font_upload_slashed_o(void) {
    volatile uint8_t *font = (volatile uint8_t *)VGA_FONT_MEM;
    uint8_t seq2 = vga_reg_swap(VGA_SEQ_INDEX, 2, 0x04);   /* write plane 2 only */
    uint8_t seq4 = vga_reg_swap(VGA_SEQ_INDEX, 4, 0x07);   /* sequential, no odd/even */
    uint8_t gc4  = vga_reg_swap(VGA_GC_INDEX, 4, 0x02);    /* read plane 2 */
    uint8_t gc5  = vga_reg_swap(VGA_GC_INDEX, 5, 0x00);
    uint8_t gc6  = vga_reg_swap(VGA_GC_INDEX, 6, 0x00);    /* map A0000, 128 KiB */

    make_slashed(font, 'o', 0xED);
    make_slashed(font, 'O', 0xE8);

    vga_reg_swap(VGA_GC_INDEX, 6, gc6);
    vga_reg_swap(VGA_GC_INDEX, 5, gc5);
    vga_reg_swap(VGA_GC_INDEX, 4, gc4);
    vga_reg_swap(VGA_SEQ_INDEX, 4, seq4);
    vga_reg_swap(VGA_SEQ_INDEX, 2, seq2);
}

void terminal_init(void) {
    spinlock_init(&g_lock);
    font_upload_slashed_o();
    update_attr();
    hw_cursor_init();
    clear_all();
}
