/**
 * @file keymap.c
 * @brief US and Norwegian layouts. Characters are Latin-1, so the Norwegian
 *        letters are 0xE6 (ae), 0xF8 (oe) and 0xE5 (aa); they are written as
 *        octal escapes below.
 */
#include <kernel/drivers/input/keymap.h>
#include <kernel/drivers/input/keyboard.h>
#include <kernel/sync/spinlock.h>

#define SC_MAX   0x58
#define PLANES   3 /* normal, shift, altgr */
#define N 0
#define S 1
#define G 2

static uint8_t g_tbl[PLANES][SC_MAX + 1];

static void fill(uint8_t plane, uint8_t first, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        g_tbl[plane][first + i] = (uint8_t)s[i];
    }
}
#define FILL(plane, first, str) fill(plane, first, str, sizeof(str) - 1)

/* Rows: 0x02-0x0D digits, 0x10-0x1B top letters, 0x1E-0x29 home row, 0x2C-0x35 bottom row. */
static void build(KeymapLayout layout) {
    for (int p = 0; p < PLANES; p++) {
        for (int i = 0; i <= SC_MAX; i++) g_tbl[p][i] = 0;
    }
    for (int p = N; p <= S; p++) {
        g_tbl[p][0x01] = 0x1B;  /* ESC */
        g_tbl[p][0x0E] = 0x7F;  /* Backspace sends DEL, like the Linux console */
        g_tbl[p][0x0F] = '\t';
        g_tbl[p][0x1C] = '\r';  /* Enter; the tty maps CR to NL (ICRNL) */
        g_tbl[p][0x37] = '*';   /* keypad */
        g_tbl[p][0x39] = ' ';
        g_tbl[p][0x4A] = '-';
        g_tbl[p][0x4E] = '+';
    }

    if (layout == KEYMAP_NORWEGIAN) {
        FILL(N, 0x02, "1234567890+\264");
        FILL(S, 0x02, "!\"#\244%&/()=?`");
        FILL(G, 0x02, "\0@\243$\0\0{[]}\\|");
        FILL(N, 0x10, "qwertyuiop\345\250");
        FILL(S, 0x10, "QWERTYUIOP\305^");
        FILL(G, 0x10, "\0\0\0\0\0\0\0\0\0\0\0~");
        FILL(N, 0x1E, "asdfghjkl\370\346\247");
        FILL(S, 0x1E, "ASDFGHJKL\330\306\275");
        FILL(N, 0x2C, "zxcvbnm,.-");
        FILL(S, 0x2C, "ZXCVBNM;:_");
        FILL(G, 0x2C, "\0\0\0\0\0\0\265\0\0\0");
        g_tbl[N][0x2B] = '\'';  g_tbl[S][0x2B] = '*';
        g_tbl[N][0x56] = '<';   g_tbl[S][0x56] = '>';  g_tbl[G][0x56] = '|';
    } else {
        FILL(N, 0x02, "1234567890-=");
        FILL(S, 0x02, "!@#$%^&*()_+");
        FILL(N, 0x10, "qwertyuiop[]");
        FILL(S, 0x10, "QWERTYUIOP{}");
        FILL(N, 0x1E, "asdfghjkl;'`");
        FILL(S, 0x1E, "ASDFGHJKL:\"~");
        FILL(N, 0x2C, "zxcvbnm,./");
        FILL(S, 0x2C, "ZXCVBNM<>?");
        g_tbl[N][0x2B] = '\\';  g_tbl[S][0x2B] = '|';
        g_tbl[N][0x56] = '\\';  g_tbl[S][0x56] = '|';
    }
}

void keymap_load(KeymapLayout layout) {
    uintptr_t flags = local_irq_save();
    build(layout);
    local_irq_restore(flags);
}

static bool is_letter(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7);
}

uint8_t keymap_translate(uint8_t scancode, uint8_t mods) {
    if (scancode > SC_MAX) return 0;

    uint8_t c = g_tbl[N][scancode];
    if ((mods & MOD_ALTGR) && g_tbl[G][scancode]) {
        c = g_tbl[G][scancode];
    } else {
        bool shifted = (mods & MOD_SHIFT) != 0;
        if ((mods & MOD_CAPS) && is_letter(c)) shifted = !shifted;
        if (shifted && g_tbl[S][scancode]) c = g_tbl[S][scancode];
    }

    if (mods & MOD_CTRL) {
        if (c >= 'a' && c <= 'z')       c -= 'a' - 1;
        else if (c >= '@' && c <= '_')  c &= 0x1F;
        else if (c == '?')              c = 0x7F;
        else if (c != '\r' && c != '\t' && c != 0x1B && c != 0x7F) return 0;
    }
    return c;
}
