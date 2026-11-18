/*
 * QEMU TCG plugin: AFL-style edge coverage of the guest kernel.
 *
 * Every translation block inside [lo, hi) (the kernel text) gets an execution callback that bumps
 *     map[cur ^ prev]   with   prev = cur >> 1
 * in a bitmap shared with the fuzzer (a file, mapped MAP_SHARED), exactly as AFL does for its instrumented
 * basic blocks. The guest is not modified. One vCPU is assumed (Coal OS has one).
 *
 * Arguments: shm=PATH (file of at least 65536 bytes), lo=ADDR, hi=ADDR (hex or decimal).
 */
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "qemu-plugin.h"

#define MAP_SIZE 65536

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint8_t *map;
static uint64_t lo = 0xffffffff80000000ull, hi = 0xffffffffc0000000ull;
static uint32_t prev;

static void tb_exec(unsigned int vcpu, void *udata)
{
    uint32_t cur = (uint32_t)(uintptr_t)udata;
    uint8_t *p = &map[(cur ^ prev) & (MAP_SIZE - 1)];

    if (*p != 255)
        (*p)++;
    prev = cur >> 1;
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    uint64_t pc = qemu_plugin_tb_vaddr(tb);
    uint32_t h;

    if (pc < lo || pc >= hi)
        return;
    h = (uint32_t)(pc * 0x9E3779B97F4A7C15ull >> 40);          /* spread the addresses over the map */
    qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_exec, QEMU_PLUGIN_CB_NO_REGS, (void *)(uintptr_t)h);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
    const char *shm = NULL;

    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "shm=", 4))
            shm = argv[i] + 4;
        else if (!strncmp(argv[i], "lo=", 3))
            lo = strtoull(argv[i] + 3, NULL, 0);
        else if (!strncmp(argv[i], "hi=", 3))
            hi = strtoull(argv[i] + 3, NULL, 0);
    }
    if (!shm) {
        fprintf(stderr, "coverage plugin: shm=PATH is required\n");
        return -1;
    }
    int fd = open(shm, O_RDWR);
    if (fd < 0) {
        perror("coverage plugin: open");
        return -1;
    }
    map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("coverage plugin: mmap");
        return -1;
    }
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    return 0;
}
