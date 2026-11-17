/**
 * @file random.c
 * @brief ChaCha20 based generator with fast key erasure (see random.h).
 *
 * Each output block's first 32 bytes become the next key and are never handed out, so a later
 * compromise of the state does not reveal earlier output.
 */
#include <kernel/lib/random.h>
#include <kernel/lib/string.h>
#include <kernel/sync/spinlock.h>

static uint32_t g_key[8];
static uint32_t g_counter;
static bool     g_seeded;

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL(d, 16); c += d; b ^= c; b = ROTL(b, 12); \
    a += b; d ^= a; d = ROTL(d, 8);  c += d; b ^= c; b = ROTL(b, 7)

/* One ChaCha20 block for the current key and counter. */
static void block(uint32_t out[16]) {
    uint32_t s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, 0, 0, 0, 0, 0, 0, 0, 0, g_counter++, 0, 0, 0 };
    uint32_t x[16];

    memcpy(s + 4, g_key, sizeof(g_key));
    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]); QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]); QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + s[i];
}

/* XORs input into the key, then runs the generator once so every input bit reaches every key bit. */
static void mix(const uint8_t *data, size_t len) {
    uint32_t out[16];

    for (size_t i = 0; i < len; i++) ((uint8_t *)g_key)[i % sizeof(g_key)] ^= data[i];
    block(out);
    memcpy(g_key, out, sizeof(g_key));
}

static void seed_from_cycle_counter(void) {
    for (int i = 0; i < 32; i++) {
        uint32_t t[2];
        asm volatile("rdtsc" : "=a"(t[0]), "=d"(t[1]));
        mix((const uint8_t *)t, sizeof(t));
        for (volatile int spin = 0; spin < 50 + i * 7; spin++) { }      /* let the counter move by a varying amount */
    }
    g_seeded = true;
}

void random_add(const void *data, size_t len) {
    uintptr_t fl = local_irq_save();
    mix(data, len);
    local_irq_restore(fl);
}

void random_bytes(void *out, size_t len) {
    uint8_t *p = out;
    uintptr_t fl = local_irq_save();

    if (!g_seeded) seed_from_cycle_counter();
    while (len) {
        uint32_t b[16];
        size_t n = MIN(len, sizeof(b) / 2);
        block(b);
        memcpy(g_key, b, sizeof(g_key));                    /* the first half is the next key */
        memcpy(p, b + 8, n);
        p += n;
        len -= n;
    }
    local_irq_restore(fl);
}
