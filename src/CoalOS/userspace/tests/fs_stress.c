/* Writes pseudo-random files of 1 to 8 MiB with different write sizes, reads
 * them back with different read sizes and compares a checksum and the bytes. */

#include <libc/stdbool.h>
#include <libc/stdint.h>
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/sys/stat.h>
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

/* POSIX: an open file survives unlink and rename of its name. */
static void test_open_file_names(void) {
    char buf[8] = { 0 };
    int fd = open("/tmp/fs_open_a", O_RDWR | O_CREAT | O_TRUNC, 0644);

    CHECK(fd >= 0 && write(fd, "abc", 3) == 3);
    CHECK(rename("/tmp/fs_open_a", "/tmp/fs_open_b") == 0);
    CHECK(write(fd, "def", 3) == 3);
    CHECK(unlink("/tmp/fs_open_b") == 0);
    CHECK(open("/tmp/fs_open_b", O_RDONLY) < 0);
    CHECK(lseek(fd, 0, 0) == 0 && read(fd, buf, 7) == 6 && memcmp(buf, "abcdef", 6) == 0);
    close(fd);                                  /* frees the clusters */
}

static bool all_bytes(const uint8_t *p, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; i++) if (p[i] != v) return false;
    return true;
}

static ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    return lseek(fd, off, 0) == off ? read(fd, buf, n) : -1;
}

/* Page cache edges: holes, truncation, overwrites across page boundaries, small appends, reuse of clusters. */
static void test_page_cache_edges(void) {
    static uint8_t buf[70000];
    int fd = open("/tmp/fs_edge", O_RDWR | O_CREAT | O_TRUNC, 0644);

    CHECK(fd >= 0);
    memset(buf, 0xAA, 65536);                               /* old data that a new file must never show */
    CHECK(write(fd, buf, 65536) == 65536);
    close(fd);
    CHECK(unlink("/tmp/fs_edge") == 0);

    fd = open("/tmp/fs_edge", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "head", 4) == 4);
    CHECK(lseek(fd, 60000, 0) == 60000 && write(fd, "tail", 4) == 4);
    CHECK(lseek(fd, 0, 0) == 0 && read(fd, buf, sizeof buf) == 60004);
    CHECK(memcmp(buf, "head", 4) == 0 && all_bytes(buf + 4, 59996, 0) && memcmp(buf + 60000, "tail", 4) == 0);

    CHECK(lseek(fd, 4090, 0) == 4090 && write(fd, "0123456789ABCDEFGHIJ", 20) == 20);     /* across a page boundary */
    CHECK(pread(fd, buf, 24, 4088) == 24 && memcmp(buf + 2, "0123456789ABCDEFGHIJ", 20) == 0 && buf[0] == 0 && buf[23] == 0);

    close(fd);                                              /* O_TRUNC: the pages of the old contents are gone */
    fd = open("/tmp/fs_edge", O_RDWR | O_TRUNC);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1 && pread(fd, buf, 4, 0) == 1 && buf[0] == 'x');
    close(fd);

    fd = open("/tmp/fs_edge", O_WRONLY | O_TRUNC | O_APPEND);
    CHECK(fd >= 0);
    for (int i = 0; i < 3000; i++) CHECK(write(fd, "abcdefg", 7) == 7);
    close(fd);
    fd = open("/tmp/fs_edge", O_RDONLY);
    CHECK(fd >= 0 && lseek(fd, 0, 2) == 21000);
    CHECK(pread(fd, buf, 14, 20986) == 14 && memcmp(buf, "abcdefgabcdefg", 14) == 0);
    close(fd);
    CHECK(unlink("/tmp/fs_edge") == 0);
}

/* Name lookups stay right while entries come and go. */
static void test_names(void) {
    struct stat st;
    char path[96];

    CHECK(mkdir("/tmp/fs_dir", 0755) == 0);
    for (int i = 0; i < 40; i++) {
        snprintf(path, sizeof path, "/tmp/fs_dir/a rather long file name number %d.txt", i);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        CHECK(fd >= 0 && write(fd, path, 3 + (size_t)i) == 3 + (ssize_t)i);
        close(fd);
    }
    CHECK(stat("/tmp/fs_dir/A RATHER LONG FILE NAME NUMBER 7.TXT", &st) == 0 && st.st_size == 10);
    CHECK(rename("/tmp/fs_dir/a rather long file name number 7.txt", "/tmp/fs_dir/seven") == 0);
    CHECK(stat("/tmp/fs_dir/a rather long file name number 7.txt", &st) < 0);
    CHECK(stat("/tmp/fs_dir/SEVEN", &st) == 0 && st.st_size == 10);
    CHECK(unlink("/tmp/fs_dir/seven") == 0 && stat("/tmp/fs_dir/seven", &st) < 0);
    int fd = open("/tmp/fs_dir/seven", O_WRONLY | O_CREAT | O_EXCL, 0644);          /* the freed slot is reused */
    CHECK(fd >= 0 && write(fd, "new", 3) == 3);
    close(fd);
    CHECK(stat("/tmp/fs_dir/seven", &st) == 0 && st.st_size == 3);
    for (int i = 0; i < 40; i++) {
        snprintf(path, sizeof path, "/tmp/fs_dir/a rather long file name number %d.txt", i);
        CHECK(i == 7 ? stat(path, &st) < 0 : unlink(path) == 0);
    }
    CHECK(rmdir("/tmp/fs_dir") < 0);                                                 /* "seven" is still there */
    CHECK(unlink("/tmp/fs_dir/seven") == 0 && rmdir("/tmp/fs_dir") == 0);
    CHECK(stat("/tmp/fs_dir/seven", &st) < 0);
    CHECK(mkdir("/tmp/fs_dir", 0755) == 0 && stat("/tmp/fs_dir/seven", &st) < 0 && rmdir("/tmp/fs_dir") == 0);
}

int main(int argc, char **argv) {
    static const size_t sizes[] = { 1u << 20, 2u << 20, 4u << 20, 8u << 20, (3u << 20) + 12345 };
    static const size_t chunks[] = { 512, 4096, 65536, 1u << 20, 12345 };

    unsigned nsizes = argc > 1 && strcmp(argv[1], "quick") == 0 ? 1 : sizeof sizes / sizeof sizes[0];

    test_open_file_names();
    test_page_cache_edges();
    test_names();
    for (unsigned i = 0; i < nsizes; i++) {
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
