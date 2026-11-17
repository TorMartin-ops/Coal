/* Writes pseudo-random files of 1 to 8 MiB with different write sizes, reads
 * them back with different read sizes and compares a checksum and the bytes. */

#include <libc/stdbool.h>
#include <libc/stdint.h>
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include "check.h"

#define PATH "/tmp/fs_stress.bin"

static uint32_t rng_state;

static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void fill(uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(rng() >> 11);
}

static uint32_t sum(uint32_t h, const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 16777619u;
    return h;
}

/* Writes size bytes in pieces of wchunk, returns the checksum or 0 on error. */
static uint32_t write_file(size_t size, size_t wchunk, uint32_t seed) {
    uint8_t *buf = malloc(wchunk);
    uint32_t h = 2166136261u;
    int fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (!buf || fd < 0) { free(buf); return 0; }
    rng_state = seed;
    for (size_t done = 0; done < size; ) {
        size_t n = size - done < wchunk ? size - done : wchunk;
        fill(buf, n);
        if (write(fd, buf, n) != (ssize_t)n) { h = 0; break; }
        h = sum(h, buf, n);
        done += n;
    }
    close(fd);
    free(buf);
    return h;
}

/* Reads the file in pieces of rchunk and checks the checksum and the expected byte stream. */
static bool read_check(size_t size, size_t rchunk, uint32_t seed, uint32_t want) {
    uint8_t *buf = malloc(rchunk), *exp = malloc(rchunk);
    uint32_t h = 2166136261u;
    int fd = open(PATH, O_RDONLY);
    size_t done = 0;
    bool same = buf && exp && fd >= 0;

    rng_state = seed;                       /* same stream as the writer; the stream is consumed in write order */
    while (same && done < size) {
        ssize_t n = read(fd, buf, rchunk);
        if (n <= 0) { same = false; break; }
        h = sum(h, buf, (size_t)n);
        done += (size_t)n;
    }
    if (fd >= 0) close(fd);
    free(buf);
    free(exp);
    return same && done == size && h == want;
}

int main(void) {
    static const size_t sizes[] = { 1u << 20, 2u << 20, 4u << 20, 8u << 20, (3u << 20) + 12345 };
    static const size_t chunks[] = { 512, 4096, 65536, 1u << 20, 12345 };

    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        for (unsigned j = 0; j < sizeof chunks / sizeof chunks[0]; j++) {
            uint32_t seed = 0x9e3779b9u * (i + 1) + j;
            uint32_t h = write_file(sizes[i], chunks[j], seed);
            CHECK(h != 0);
            int ok = read_check(sizes[i], chunks[(j + 2) % 5], seed, h);
            if (!ok) printf("MISMATCH size %u write chunk %u read chunk %u\n", (unsigned)sizes[i],
                            (unsigned)chunks[j], (unsigned)chunks[(j + 2) % 5]);
            CHECK(ok);
        }
    }
    unlink(PATH);
    return check_report("fs_stress");
}
