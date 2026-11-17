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
    __asm__ volatile ("syscall" :: "a"(60), "D"(rc));
    for (;;) { }
}

__attribute__((naked)) void _start(void) {
    __asm__ volatile ("mov %rsp, %rdi; and $-16, %rsp; call start_c");
}
