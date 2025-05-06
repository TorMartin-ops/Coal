/* =========================================================================
 *  hello.c  –  UiAOS user-space test (v2.1, ultra-verbose debug build)
 * =========================================================================
 *  – single translation unit, no #include
 *  – i686-ELF, static, -nostdlib friendly
 *  – build:  i686-elf-gcc -m32 -Wall -Wextra -nostdlib -fno-builtin \
 *                               -fno-stack-protector -std=gnu99 -c hello.c
 * -------------------------------------------------------------------------*/

 typedef  signed   char      int8_t;
 typedef  unsigned char      uint8_t;
 typedef  signed   short     int16_t;
 typedef  unsigned short     uint16_t;
 typedef  signed   int       int32_t;
 typedef  unsigned int       uint32_t;
 typedef  signed   long long int64_t;
 typedef  unsigned long long uint64_t;
 typedef  uint32_t           size_t;
 typedef  int32_t            ssize_t;
 typedef  uint32_t           uintptr_t;
 typedef  int32_t            pid_t;
 typedef  int32_t            bool;
 #define true  1
 #define false 0
 #define NULL ((void*)0)
 
 /* -------------------------------------------------------------------------
  *  Kernel ABI
  * -------------------------------------------------------------------------*/
 #define SYS_EXIT     1
 #define SYS_READ     3
 #define SYS_WRITE    4
 #define SYS_OPEN     5
 #define SYS_CLOSE    6
 #define SYS_PUTS     7
 #define SYS_LSEEK   19
 #define SYS_GETPID  20
 
 #define O_RDONLY    0x0000
 #define O_WRONLY    0x0001
 #define O_RDWR      0x0002
 #define O_CREAT     0x0040
 #define O_TRUNC     0x0200
 #define DEFAULT_MODE 0666u
 
 /* ------------------------------------------------------------------------
  *  syscall3 – EBX-safe wrapper (EBX is an *input*, so PIC is unharmed)
  * ----------------------------------------------------------------------*/
 static inline int syscall3(int num, int arg1, int arg2, int arg3)
 {
     int ret;
     asm volatile(
         "int $0x80"
         : "=a"(ret)
         : "0"(num), "b"(arg1), "c"(arg2), "d"(arg3)
         : "memory"
     );
     return ret;
 }
 
 /* thin convenience wrappers ------------------------------------------------*/
 #define sys_exit(x)          syscall3(SYS_EXIT , (x), 0, 0)
 #define sys_read(fd,buf,n)   syscall3(SYS_READ , (fd), (int)(buf), (n))
 #define sys_write(fd,buf,n)  syscall3(SYS_WRITE, (fd), (int)(buf), (n))
 #define sys_open(p,f,m)      syscall3(SYS_OPEN , (int)(p), (f), (m))
 #define sys_close(fd)        syscall3(SYS_CLOSE, (fd), 0, 0)
 #define sys_puts(p)          syscall3(SYS_PUTS , (int)(p), 0, 0)
 #define sys_getpid()         syscall3(SYS_GETPID,0,0,0)
 
 /* --------------------------------------------------------------------- */
 /*  tiny helpers – strlen / integer printing / hex-dump                  */
 /* ---------------------------------------------------------------------*/
 static size_t strlen_c(const char *s){ size_t i=0; while(s&&s[i]) ++i; return i; }
 static void   print_str(const char *s){ if(s) sys_puts(s); }
 
 static void utoa_base(uint32_t v,unsigned b,char *o){
     char tmp[32]; int i=0;
     if(!v) tmp[i++]='0';
     while(v){ uint32_t d=v%b; tmp[i++]=(d<10)?('0'+d):('a'+d-10); v/=b; }
     int j=0; while(i) o[j++]=tmp[--i]; o[j]=0;
 }
 static void print_int(int v){
     char buf[16]; bool neg=(v<0); uint32_t u=neg?-(uint32_t)v:(uint32_t)v;
     if(neg){ buf[0]='-'; utoa_base(u,10,buf+1); } else utoa_base(u,10,buf);
     print_str(buf);
 }
 static void print_hex(uint32_t v){ char b[16]; b[0]='0'; b[1]='x'; utoa_base(v,16,b+2); print_str(b); }
 
 static void log_fd(const char *tag,int fd){
     print_str(tag); print_int(fd); print_str(" ("); print_hex((uint32_t)fd); print_str(")\n");
 }
 
 /* optional: print raw bytes as hex ----------------------------------------*/
 static void hexdump(const char *prefix,const void *buf,size_t n){
     const uint8_t *p=(const uint8_t*)buf;
     print_str(prefix); print_str("len="); print_int((int)n); print_str(": ");
     for(size_t i=0;i<n;i++){
         char h[4]; utoa_base(p[i],16,h); if(p[i]<16){ print_str("0"); }
         print_str(h); if(i+1<n) print_str(" ");
     }
     print_str("\n");
 }
 
 /* --------------------------------------------------------------------- */
 #define WBUF_SZ  128
 #define RBUF_SZ  128
 
 int main(void)
 {
     const char *fname="/testfile.txt";
     char wbuf[WBUF_SZ];
     char rbuf[RBUF_SZ];
 
     print_str("=== hello.c ultra-verbose v2.1 ===\n");
 
     /* ------------------------------------------------------------------ */
     int pid=sys_getpid();
     print_str("[DBG] sys_getpid() -> "); print_int(pid); print_str("\n");
 
     /* -------- open (write/create) ------------------------------------- */
     print_str("[DBG] open(O_CREAT|O_WRONLY|O_TRUNC) path="); print_str(fname); print_str("\n");
     int fdw=sys_open(fname,O_CREAT|O_WRONLY|O_TRUNC,DEFAULT_MODE);
     log_fd("[DBG] open() returned ",fdw);
     if(fdw<0){ print_str("[ERR] open failed – aborting\n"); goto done; }
 
     /* prepare buffer ---------------------------------------------------- */
     const char *msg="Hello from ultra-verbose build!\n";
     size_t wl=0; while(msg[wl]&&wl<WBUF_SZ-1){ wbuf[wl]=msg[wl]; ++wl; }
     wbuf[wl]=0;
 
     hexdump("[DBG] write-buffer ",wbuf,wl);
 
     /* write ------------------------------------------------------------- */
     log_fd("[DBG] write() using ",fdw);
     int wr=sys_write(fdw,wbuf,(int)wl);
     print_str("[DBG] sys_write ret="); print_int(wr); print_str("\n");
     if(wr!= (int)wl){ print_str("[WARN] partial/failed write\n"); }
 
     /* close write fd ---------------------------------------------------- */
     log_fd("[DBG] close() fd ",fdw);
     sys_close(fdw);
 
     /* open read-only ---------------------------------------------------- */
     print_str("[DBG] reopen read-only\n");
     int fdr=sys_open(fname,O_RDONLY,0);
     log_fd("[DBG] open(RD) -> ",fdr);
     if(fdr<0){ print_str("[ERR] open(RD) failed\n"); goto done; }
 
     /* read -------------------------------------------------------------- */
     print_str("[DBG] read() up to "); print_int(RBUF_SZ-1); print_str(" bytes\n");
     int rd=sys_read(fdr,rbuf,RBUF_SZ-1);
     print_str("[DBG] sys_read ret="); print_int(rd); print_str("\n");
     if(rd<0){ print_str("[ERR] read failed\n"); goto close_rd; }
 
     rbuf[rd]=0;
     hexdump("[DBG] read-buffer ",rbuf,(size_t)rd);
     print_str("[DBG] read text: "); print_str(rbuf);
 
 close_rd:
     log_fd("\n[DBG] close() fd ",fdr);
     sys_close(fdr);
 
 done:
     print_str("\n=== done ===\n");
     sys_exit(0);
     for(;;);                              /* not reached, placate linker   */
     return 0;
 }
 