/**
 * @file syscall_linux.h
 * @brief Linux-compatible system call definitions and numbers
 * @author Architectural improvements for CoalOS
 */

#ifndef SYSCALL_LINUX_H
#define SYSCALL_LINUX_H

#include <libc/stdint.h>

/* Linux x86 32-bit system call numbers */
/* Process Control */
#define __NR_exit                 1
#define __NR_fork                 2
#define __NR_read                 3
#define __NR_write                4
#define __NR_open                 5
#define __NR_close                6
#define __NR_waitpid              7
#define __NR_creat                8
#define __NR_link                 9
#define __NR_unlink              10
#define __NR_execve              11
#define __NR_chdir               12
#define __NR_time                13
#define __NR_mknod               14
#define __NR_chmod               15
#define __NR_chown               16
#define __NR_break               17
#define __NR_oldstat             18
#define __NR_lseek               19
#define __NR_getpid              20
#define __NR_mount               21
#define __NR_umount              22
#define __NR_setuid              23
#define __NR_getuid              24
#define __NR_stime               25
#define __NR_ptrace              26
#define __NR_alarm               27
#define __NR_oldfstat            28
#define __NR_pause               29
#define __NR_utime               30
#define __NR_stty                31
#define __NR_gtty                32
#define __NR_access              33
#define __NR_nice                34
#define __NR_ftime               35
#define __NR_sync                36
#define __NR_kill                37
#define __NR_rename              38
#define __NR_mkdir               39
#define __NR_rmdir               40
#define __NR_dup                 41
#define __NR_pipe                42
#define __NR_times               43
#define __NR_prof                44
#define __NR_brk                 45
#define __NR_setgid              46
#define __NR_getgid              47
#define __NR_signal              48
#define __NR_geteuid             49
#define __NR_getegid             50
#define __NR_acct                51
#define __NR_umount2             52
#define __NR_lock                53
#define __NR_ioctl               54
#define __NR_fcntl               55
#define __NR_mpx                 56
#define __NR_setpgid             57
#define __NR_ulimit              58
#define __NR_oldolduname         59
#define __NR_umask               60
#define __NR_chroot              61
#define __NR_ustat               62
#define __NR_dup2                63
#define __NR_getppid             64
#define __NR_getpgrp             65
#define __NR_setsid              66
#define __NR_sigaction           67
#define __NR_sgetmask            68
#define __NR_ssetmask            69
#define __NR_setreuid            70
#define __NR_setregid            71
#define __NR_sigsuspend          72
#define __NR_sigpending          73
#define __NR_sethostname         74
#define __NR_setrlimit           75
#define __NR_getrlimit           76
#define __NR_getrusage           77
#define __NR_gettimeofday        78
#define __NR_settimeofday        79
#define __NR_getgroups           80
#define __NR_setgroups           81
#define __NR_select              82
#define __NR_symlink             83
#define __NR_oldlstat            84
#define __NR_readlink            85
#define __NR_uselib              86
#define __NR_swapon              87
#define __NR_reboot              88
#define __NR_readdir             89
#define __NR_mmap                90
#define __NR_munmap              91
#define __NR_truncate            92
#define __NR_ftruncate           93
#define __NR_fchmod              94
#define __NR_fchown              95
#define __NR_getpriority         96
#define __NR_setpriority         97
#define __NR_profil              98
#define __NR_statfs              99
#define __NR_fstatfs            100
#define __NR_ioperm             101
#define __NR_socketcall         102
#define __NR_syslog             103
#define __NR_setitimer          104
#define __NR_getitimer          105
#define __NR_stat               106
#define __NR_lstat              107
#define __NR_fstat              108
#define __NR_olduname           109
#define __NR_iopl               110
#define __NR_vhangup            111
#define __NR_idle               112
#define __NR_vm86old            113
#define __NR_wait4              114
#define __NR_swapoff            115
#define __NR_sysinfo            116
#define __NR_ipc                117
#define __NR_fsync              118
#define __NR_sigreturn          119
#define __NR_clone              120
#define __NR_setdomainname      121
#define __NR_uname              122
#define __NR_modify_ldt         123
#define __NR_adjtimex           124
#define __NR_mprotect           125
#define __NR_sigprocmask        126
#define __NR_create_module      127
#define __NR_init_module        128
#define __NR_delete_module      129
#define __NR_get_kernel_syms    130
#define __NR_quotactl           131
#define __NR_getpgid            132
#define __NR_fchdir             133
#define __NR_bdflush            134
#define __NR_sysfs              135
#define __NR_personality        136
#define __NR_afs_syscall        137
#define __NR_setfsuid           138
#define __NR_setfsgid           139
#define __NR__llseek            140
#define __NR_getdents           141
#define __NR__newselect         142
#define __NR_flock              143
#define __NR_msync              144
#define __NR_readv              145
#define __NR_writev             146
#define __NR_getsid             147
#define __NR_fdatasync          148
#define __NR__sysctl            149
#define __NR_mlock              150
#define __NR_munlock            151
#define __NR_mlockall           152
#define __NR_munlockall         153
#define __NR_sched_setparam     154
#define __NR_sched_getparam     155
#define __NR_sched_setscheduler 156
#define __NR_sched_getscheduler 157
#define __NR_sched_yield        158
#define __NR_sched_get_priority_max     159
#define __NR_sched_get_priority_min     160
#define __NR_sched_rr_get_interval      161
#define __NR_nanosleep          162
#define __NR_mremap             163
#define __NR_setresuid          164
#define __NR_getresuid          165
#define __NR_vm86               166
#define __NR_query_module       167
#define __NR_poll               168
#define __NR_nfsservctl         169
#define __NR_setresgid          170
#define __NR_getresgid          171
#define __NR_prctl              172
#define __NR_rt_sigreturn       173
#define __NR_rt_sigaction       174
#define __NR_rt_sigprocmask     175
#define __NR_rt_sigpending      176
#define __NR_rt_sigtimedwait    177
#define __NR_rt_sigqueueinfo    178
#define __NR_rt_sigsuspend      179
#define __NR_pread64            180
#define __NR_pwrite64           181
#define __NR_chown16            182
#define __NR_getcwd             183
#define __NR_capget             184
#define __NR_capset             185
#define __NR_sigaltstack        186
#define __NR_sendfile           187
#define __NR_getpmsg            188
#define __NR_putpmsg            189
#define __NR_vfork              190
#define __NR_ugetrlimit         191
#define __NR_mmap2              192
#define __NR_truncate64         193
#define __NR_ftruncate64        194
#define __NR_stat64             195
#define __NR_lstat64            196
#define __NR_fstat64            197
#define __NR_lchown32           198
#define __NR_getuid32           199
#define __NR_getgid32           200
#define __NR_geteuid32          201
#define __NR_getegid32          202
#define __NR_setreuid32         203
#define __NR_setregid32         204
#define __NR_getgroups32        205
#define __NR_setgroups32        206
#define __NR_fchown32           207
#define __NR_setresuid32        208
#define __NR_getresuid32        209
#define __NR_setresgid32        210
#define __NR_getresgid32        211
#define __NR_chown32            212
#define __NR_setuid32           213
#define __NR_setgid32           214
#define __NR_setfsuid32         215
#define __NR_setfsgid32         216
#define __NR_pivot_root         217
#define __NR_mincore            218
#define __NR_madvise            219
#define __NR_getdents64         220
#define __NR_fcntl64            221
/* 222 is unused */
/* 223 is unused */
#define __NR_gettid             224
#define __NR_readahead          225
#define __NR_setxattr           226
#define __NR_lsetxattr          227
#define __NR_fsetxattr          228
#define __NR_getxattr           229
#define __NR_lgetxattr          230
#define __NR_fgetxattr          231
#define __NR_listxattr          232
#define __NR_llistxattr         233
#define __NR_flistxattr         234
#define __NR_removexattr        235
#define __NR_lremovexattr       236
#define __NR_fremovexattr       237
#define __NR_tkill              238
#define __NR_sendfile64         239
#define __NR_futex              240
#define __NR_sched_setaffinity  241
#define __NR_sched_getaffinity  242
#define __NR_set_thread_area    243
#define __NR_get_thread_area    244
#define __NR_io_setup           245
#define __NR_io_destroy         246
#define __NR_io_getevents       247
#define __NR_io_submit          248
#define __NR_io_cancel          249
#define __NR_fadvise64          250
/* 251 is available for reuse (was reserved for removed syscall) */
#define __NR_exit_group         252
#define __NR_lookup_dcookie     253
#define __NR_epoll_create       254
#define __NR_epoll_ctl          255
#define __NR_epoll_wait         256
#define __NR_remap_file_pages   257
#define __NR_set_tid_address    258
#define __NR_timer_create       259
#define __NR_timer_settime      260
#define __NR_timer_gettime      261
#define __NR_timer_getoverrun   262
#define __NR_timer_delete       263
#define __NR_clock_settime      264
#define __NR_clock_gettime      265
#define __NR_clock_getres       266
#define __NR_clock_nanosleep    267
#define __NR_statfs64           268
#define __NR_fstatfs64          269
#define __NR_tgkill             270
#define __NR_utimes             271
#define __NR_fadvise64_64       272
#define __NR_vserver            273
#define __NR_mbind              274
#define __NR_get_mempolicy      275
#define __NR_set_mempolicy      276
#define __NR_mq_open            277
#define __NR_mq_unlink          278
#define __NR_mq_timedsend       279
#define __NR_mq_timedreceive    280
#define __NR_mq_notify          281
#define __NR_mq_getsetattr      282
#define __NR_kexec_load         283
#define __NR_waitid             284
/* 285 sys_setaltroot */
#define __NR_add_key            286
#define __NR_request_key        287
#define __NR_keyctl             288
#define __NR_ioprio_set         289
#define __NR_ioprio_get         290
#define __NR_inotify_init       291
#define __NR_inotify_add_watch  292
#define __NR_inotify_rm_watch   293
#define __NR_migrate_pages      294
#define __NR_openat             295
#define __NR_mkdirat            296
#define __NR_mknodat            297
#define __NR_fchownat           298
#define __NR_futimesat          299
#define __NR_fstatat64          300
#define __NR_unlinkat           301
#define __NR_renameat           302
#define __NR_linkat             303
#define __NR_symlinkat          304
#define __NR_readlinkat         305
#define __NR_fchmodat           306
#define __NR_faccessat          307
#define __NR_pselect6           308
#define __NR_ppoll              309
#define __NR_unshare            310
#define __NR_set_robust_list    311
#define __NR_get_robust_list    312
#define __NR_splice             313
#define __NR_sync_file_range    314
#define __NR_tee                315
#define __NR_vmsplice           316
#define __NR_move_pages         317
#define __NR_getcpu             318
#define __NR_epoll_pwait        319
#define __NR_utimensat          320
#define __NR_signalfd           321
#define __NR_timerfd_create     322
#define __NR_eventfd            323
#define __NR_fallocate          324
#define __NR_timerfd_settime    325
#define __NR_timerfd_gettime    326
#define __NR_signalfd4          327
#define __NR_eventfd2           328
#define __NR_epoll_create1      329
#define __NR_dup3               330
#define __NR_pipe2              331
#define __NR_inotify_init1      332
#define __NR_preadv             333
#define __NR_pwritev            334
#define __NR_rt_tgsigqueueinfo  335
#define __NR_perf_event_open    336
#define __NR_recvmmsg           337
#define __NR_fanotify_init      338
#define __NR_fanotify_mark      339
#define __NR_prlimit64          340

/* System call table size */
#define __NR_syscalls           341

/* Error codes returned in EAX (negative) */
#define LINUX_EPERM              1  /* Operation not permitted */
#define LINUX_ENOENT             2  /* No such file or directory */
#define LINUX_ESRCH              3  /* No such process */
#define LINUX_EINTR              4  /* Interrupted system call */
#define LINUX_EIO                5  /* I/O error */
#define LINUX_ENXIO              6  /* No such device or address */
#define LINUX_E2BIG              7  /* Argument list too long */
#define LINUX_ENOEXEC            8  /* Exec format error */
#define LINUX_EBADF              9  /* Bad file number */
#define LINUX_ECHILD            10  /* No child processes */
#define LINUX_EAGAIN            11  /* Try again */
#define LINUX_ENOMEM            12  /* Out of memory */
#define LINUX_EACCES            13  /* Permission denied */
#define LINUX_EFAULT            14  /* Bad address */
#define LINUX_ENOTBLK           15  /* Block device required */
#define LINUX_EBUSY             16  /* Device or resource busy */
#define LINUX_EEXIST            17  /* File exists */
#define LINUX_EXDEV             18  /* Cross-device link */
#define LINUX_ENODEV            19  /* No such device */
#define LINUX_ENOTDIR           20  /* Not a directory */
#define LINUX_EISDIR            21  /* Is a directory */
#define LINUX_EINVAL            22  /* Invalid argument */
#define LINUX_ENFILE            23  /* File table overflow */
#define LINUX_EMFILE            24  /* Too many open files */
#define LINUX_ENOTTY            25  /* Not a typewriter */
#define LINUX_ETXTBSY           26  /* Text file busy */
#define LINUX_EFBIG             27  /* File too large */
#define LINUX_ENOSPC            28  /* No space left on device */
#define LINUX_ESPIPE            29  /* Illegal seek */
#define LINUX_EROFS             30  /* Read-only file system */
#define LINUX_EMLINK            31  /* Too many links */
#define LINUX_EPIPE             32  /* Broken pipe */
#define LINUX_EDOM              33  /* Math argument out of domain of func */
#define LINUX_ERANGE            34  /* Math result not representable */
#define LINUX_ENOSYS            38  /* Function not implemented */

/* Convert internal error codes to Linux error codes */
static inline int coalos_to_linux_error(int error) {
    /* Map CoalOS error codes to Linux equivalents */
    switch (error) {
        case -1: return -LINUX_EPERM;
        case -2: return -LINUX_ENOENT;
        case -3: return -LINUX_ESRCH;
        case -5: return -LINUX_EIO;
        case -9: return -LINUX_EBADF;
        case -11: return -LINUX_EAGAIN;
        case -12: return -LINUX_ENOMEM;
        case -13: return -LINUX_EACCES;
        case -14: return -LINUX_EFAULT;
        case -17: return -LINUX_EEXIST;
        case -20: return -LINUX_ENOTDIR;
        case -21: return -LINUX_EISDIR;
        case -22: return -LINUX_EINVAL;
        case -24: return -LINUX_EMFILE;
        case -28: return -LINUX_ENOSPC;
        case -30: return -LINUX_EROFS;
        default: return -LINUX_EIO;
    }
}

/* Syscall handler function type */
typedef int (*syscall_handler_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

/* Linux-compatible syscall table */
extern syscall_handler_t linux_syscall_table[__NR_syscalls];

/* Initialize Linux-compatible syscall table */
void init_linux_syscall_table(void);

/* Syscall dispatcher for Linux compatibility mode */
int linux_syscall_dispatcher(uint32_t syscall_num, uint32_t ebx, uint32_t ecx, 
                            uint32_t edx, uint32_t esi, uint32_t edi);

#endif /* SYSCALL_LINUX_H */