/* Regression: private file mappings are a copy of the file; the fd is ignored with MAP_ANONYMOUS. */
#include "trace.h"

int main(void) {
    init();
    scratch("reg_mmap_file");
    long fd = R("creat", K(SYS_open, "f", O_CREAT | O_RDWR, 0644));
    R("write", K(SYS_write, fd, "hello", 5));
    long m = K(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    NOTE("private file mapping ok: %s", yn(m > 0));
    if (m > 0) {
        NOTE("content: %.5s, rest zero: %s", (char *)m, yn(((char *)m)[5] == 0));
        ((char *)m)[0] = 'J';
        char b[2] = {0, 0};
        K(SYS_pread64, fd, b, 1, 0);
        NOTE("write stays private: %s", yn(b[0] == 'h'));
    }
    long ro = K(SYS_mmap, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    NOTE("read-only mapping ok: %s", yn(ro > 0));
    R("munmap", K(SYS_munmap, m, 4096));
    R("mmap(unaligned offset)", K(SYS_mmap, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 1));
    R("mmap(bad fd)", K(SYS_mmap, 0, 4096, PROT_READ, MAP_PRIVATE, 77, 0));
    long a = K(SYS_mmap, 0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    NOTE("anonymous mapping ignores fd 0: %s", yn(a > 0));
    return 0;
}
