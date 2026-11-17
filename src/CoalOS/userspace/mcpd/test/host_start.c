/* Entry point and memory helpers so mcpd runs as a plain static Linux binary. */
extern int main(int argc, char **argv);

void *memcpy(void *d, const void *s, unsigned long n) {
    char *dd = d; const char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memset(void *d, int c, unsigned long n) {
    char *dd = d;
    while (n--) *dd++ = (char)c;
    return d;
}

__attribute__((noreturn, used)) static void start_c(long *sp) {
    int rc = main((int)sp[0], (char **)(sp + 1));
    __asm__ volatile ("int $0x80" :: "a"(1), "b"(rc));
    for (;;) { }
}

__attribute__((naked)) void _start(void) {
    __asm__ volatile ("mov %esp, %eax; and $-16, %esp; push %eax; push %eax; "
                      "call start_c");
}
