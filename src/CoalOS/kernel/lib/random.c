#include <kernel/lib/random.h>
#include <kernel/cpu/msr.h>

#define CPUID1_ECX_RDRAND (1u << 30)

static bool rdrand64(uint64_t *v) {
    for (int tries = 0; tries < 10; tries++) {
        uint8_t ok;
        asm volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok) : : "cc");
        if (ok) return true;
    }
    return false;
}

static bool have_rdrand(void) {
    static int cached = -1;

    if (cached < 0) {
        uint32_t a = 1, b, c, d;
        asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        cached = (c & CPUID1_ECX_RDRAND) != 0;
    }
    return cached;
}

/* xorshift64*, reseeded from the TSC on every call: not cryptographic, but different on every boot and call. */
static uint64_t fallback64(void) {
    uint64_t x = rdtsc() | 1;

    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    return x * 0x2545F4914F6CDD1Dull;
}

void random_bytes(void *out, size_t n) {
    uint8_t *p = out;

    while (n) {
        uint64_t v;
        size_t k = n < sizeof(v) ? n : sizeof(v);

        if (!have_rdrand() || !rdrand64(&v)) v = fallback64();
        for (size_t i = 0; i < k; i++) p[i] = (uint8_t)(v >> (8 * i));
        p += k;
        n -= k;
    }
}
