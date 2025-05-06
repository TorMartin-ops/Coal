/* hello.c – UiAOS user-space smoke-test  (v2.2-mini-fix1)
 * build:  i686-elf-gcc -m32 -Wall -Wextra -nostdlib -fno-builtin \
 *                     -fno-stack-protector -std=gnu99 -c hello.c           */

/* ==== typedefs ==================================================== */
typedef signed   char      int8_t;    typedef unsigned char      uint8_t;
typedef signed   short     int16_t;   typedef unsigned short     uint16_t;
typedef signed   int       int32_t;   typedef unsigned int       uint32_t;
typedef signed   long long int64_t;   typedef unsigned long long uint64_t;
typedef uint32_t           size_t;    typedef int32_t            ssize_t;
typedef int32_t            pid_t;

typedef int32_t  bool;
#define true  1
#define false 0
#define NULL  0

/* ==== kernel ABI constants ======================================= */
#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_PUTS    7
#define SYS_GETPID 20

#define O_RDONLY     0x0000
#define O_WRONLY     0x0001
#define O_CREAT      0x0040
#define O_TRUNC      0x0200
#define DEFAULT_MODE 0666u

/* ==== PIC-safe 3-arg syscall wrapper ============================== */
static inline int32_t syscall3(int32_t num,
                               int32_t arg1,
                               int32_t arg2,
                               int32_t arg3)
{
    int32_t ret;
    __asm__ volatile (
        "pushl %%ebx\n\t"
        "movl  %2,  %%ebx\n\t"   /* EBX = arg1 (from *memory*) */
        "int   $0x80\n\t"
        "popl  %%ebx"
        : "=a"(ret)
        : "0"(num), "m"(arg1), "c"(arg2), "d"(arg3)
        : "cc", "memory"
    );
    return ret;
}

/* ==== thin syscall helpers ======================================= */
#define sys_exit(x)        syscall3(SYS_EXIT , (x), 0, 0)
#define sys_read(fd,b,n)   syscall3(SYS_READ , (fd), (int32_t)(b), (n))
#define sys_write(fd,b,n)  syscall3(SYS_WRITE, (fd), (int32_t)(b), (n))
#define sys_open(p,f,m)    syscall3(SYS_OPEN , (int32_t)(p), (f), (m))
#define sys_close(fd)      syscall3(SYS_CLOSE, (fd), 0, 0)
#define sys_puts(p)        syscall3(SYS_PUTS , (int32_t)(p), 0, 0)
#define sys_getpid()       syscall3(SYS_GETPID,0,0,0)

/* ==== tiny std-lib bits ========================================== */
static size_t strlen_c(const char *s){ size_t i=0; while(s && s[i]) ++i; return i; }
static void   print_str(const char *s){ if(s) sys_puts(s); }

static void utoa10(uint32_t v, char *o){
    char t[11]; int i=0; if(!v) t[i++]='0';
    while(v){ t[i++]='0'+v%10; v/=10; }
    while(i) *o++=t[--i]; *o=0;
}
static void print_int(int v){
    char buf[12];
    if(v<0){ print_str("-"); v=-v; }
    utoa10((uint32_t)v,buf); print_str(buf);
}

/* ==== build-time verbosity switch ================================ */
#ifndef DEBUG
#define DEBUG 0
#endif
#if DEBUG
#define DBG(x) do{x;}while(0)
#else
#define DBG(x) do{}while(0)
#endif

/* ==== MAIN ======================================================== */
#define BUF_SZ 128
int main(void)
{
    const char *file = "/testfile.txt";
    char  wbuf[] = "Hello from mini build!\n";
    char  rbuf[BUF_SZ];

    print_str("== mini-test ==\n");

    /* PID ----------------------------------------------------------- */
    int pid = sys_getpid();
    print_str("pid="); print_int(pid); print_str("\n");

    /* open-write ---------------------------------------------------- */
    int fdw = sys_open(file, O_CREAT|O_WRONLY|O_TRUNC, DEFAULT_MODE);
    print_str("open(w) fd="); print_int(fdw); print_str("\n");
    if(fdw < 0) goto done;

    ssize_t wl = strlen_c(wbuf);
    int wr = sys_write(fdw, wbuf, wl);
    print_str("write="); print_int(wr); print_str("\n");
    sys_close(fdw);

    /* open-read ----------------------------------------------------- */
    int fdr = sys_open(file, O_RDONLY, 0);
    print_str("open(r) fd="); print_int(fdr); print_str("\n");
    if(fdr < 0) goto done;

    int rd = sys_read(fdr, rbuf, BUF_SZ-1);
    print_str("read="); print_int(rd); print_str("\n");
    if(rd >= 0){
        rbuf[rd] = 0;
        print_str("text: "); print_str(rbuf);
    }
    sys_close(fdr);

done:
    print_str("\n== done ==\n");
    sys_exit(0);
    return 0;           /* never reached, but placates GCC */
}
