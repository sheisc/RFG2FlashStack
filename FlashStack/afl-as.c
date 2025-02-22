﻿/*
   american fuzzy lop - wrapper for GNU as
   ---------------------------------------

   Written and maintained by Michal Zalewski <lcamtuf@google.com>

   Copyright 2013, 2014, 2015 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   The sole purpose of this wrapper is to preprocess assembly files generated
   by GCC / clang and inject the instrumentation bits included from afl-as.h. It
   is automatically invoked by the toolchain when compiling programs using
   afl-gcc / afl-clang.

   Note that it's an explicit non-goal to instrument hand-written assembly,
   be it in separate .s files or in __asm__ blocks. The only aspiration this
   utility has right now is to be able to skip them gracefully and allow the
   compilation process to continue.

   That said, see experimental/clang_asm_normalize/ for a solution that may
   allow clang users to make things work even with hand-crafted assembly. Just
   note that there is no equivalent for GCC.

 */

#define AFL_MAIN

#include "config.h"
#include "types.h"
#include "debug.h"
#include "alloc-inl.h"

#include "afl-as.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
// PAGE_SIZE
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/time.h>

#include "spa.h"



static u8** as_params;          /* Parameters passed to the real 'as'   */

static u8*  input_file;         /* Originally specified input file      */
static u8*  modified_file;      /* Instrumented file for the real 'as'  */

static u8   be_quiet,           /* Quiet mode (no stderr output)        */
            clang_mode,         /* Running in clang mode?               */
            pass_thru,          /* Just pass data through?              */
            just_version,       /* Just show version?                   */
            sanitizer;          /* Using ASAN / MSAN                    */

static u32  inst_ratio = 100,   /* Instrumentation probability (%)      */
            as_par_cnt = 1;     /* Number of params to 'as'             */

static int with_64_bit_cmd_option = 0;

/* If we don't find --32 or --64 in the command line, default to
   instrumentation for whichever mode we were compiled with. This is not
   perfect, but should do the trick for almost all use cases. */

#ifdef __x86_64__

static u8   use_64bit = 1;

#else

static u8   use_64bit = 0;

#ifdef __APPLE__
#  error "Sorry, 32-bit Apple platforms are not supported."
#endif /* __APPLE__ */

#endif /* ^__x86_64__ */


// further check whether it is a pass_thru, for fixing up rustc-generated assembly code
static int spa_is_rustc_pass_thru(u8 * fpath){
    static u8 tmp_line[MAX_LINE];
    int n_s = 0, n_e = 0; // start, end
    if(fpath){
        FILE *f = fopen(fpath, "r");

        if (!f) PFATAL("Unable to read '%s'", fpath);
        while (fgets(tmp_line, MAX_LINE, f)) {
            if(!strncmp(tmp_line, SPA_CFI_STARTPROC, strlen(SPA_CFI_STARTPROC))){
                n_s++;
            }
            else if(!strncmp(tmp_line, SPA_CFI_ENDPROC, strlen(SPA_CFI_ENDPROC))){
                n_e++;
            }
        }
        fclose(f);
    }
    return n_s > 0 && n_s == n_e;

}

#define  MAX_NUM_OF_SPA_PROTECTED_FUNCS  0x400000

static char * spa_protected_funcs[MAX_NUM_OF_SPA_PROTECTED_FUNCS];
static int num_of_protected_funcs;
static char * protected_funcs_buf;

// iron@CSE:tocttou$ make CC=spa-clang 2>&1 | grep "###SPA_FUNCNAME###" | awk '{print $2}' | uniq | sort | tee ./spa_protected_funcs.txt
// We assume the protected funcs have been sorted
static int spa_open_protected_funcs_list(u8 * fpath){
    static u8 tmp_line[MAX_LINE];

    if(fpath){
        FILE *f = fopen(fpath, "r");
        if (!f) PFATAL("Unable to read '%s'", fpath);

        num_of_protected_funcs = 0;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        // FIXME: check
        protected_funcs_buf = malloc(len + 8);
        char *cur_ptr = protected_funcs_buf;
        fseek(f, 0, SEEK_SET);
        while (fgets(tmp_line, MAX_LINE, f)) {
            long cur_len = strlen(tmp_line);
            if(cur_len > 1){
                strncpy(cur_ptr, tmp_line, strlen(tmp_line));
                spa_protected_funcs[num_of_protected_funcs] = cur_ptr;
                num_of_protected_funcs++;
                // "\n" --> 0
                cur_ptr += strlen(tmp_line);
                *(cur_ptr - 1) = 0;
                //fprintf(stderr, "%d: %s\n", num_of_protected_funcs-1, spa_protected_funcs[num_of_protected_funcs-1]);
            }

        }
        fclose(f);
    }
    return 0;

}

// objdump -T /lib/x86_64-linux-gnu/libc-2.27.so | grep "DF .text" | awk '{print "\""$7"\"\,"}' | tee libc_names.txt
// objdump -T /lib/x86_64-linux-gnu/libpthread.so.0 | grep "DF .text" | awk '{print "\""$7"\"\,"}'
static char * libc_names[] = {
    "__strspn_c1",
    "putwchar",
    "__gethostname_chk",
    "__strspn_c2",
    "setrpcent",
    "__wcstod_l",
    "__strspn_c3",
    "epoll_create",
    "sched_get_priority_min",
    "__getdomainname_chk",
    "klogctl",
    "__tolower_l",
    "dprintf",
    "setuid",
    "__wcscoll_l",
    "iswalpha",
    "__getrlimit",
    "__internal_endnetgrent",
    "chroot",
    "_IO_file_setbuf",
    "getdate",
    "__vswprintf_chk",
    "_IO_file_fopen",
    "pthread_cond_signal",
    "pthread_cond_signal",
    "strtoull_l",
    "xdr_short",
    "lfind",
    "_IO_padn",
    "strcasestr",
    "__libc_fork",
    "xdr_int64_t",
    "wcstod_l",
    "socket",
    "key_encryptsession_pk",
    "argz_create",
    "putchar_unlocked",
    "xdr_pmaplist",
    "__stpcpy_chk",
    "__xpg_basename",
    "__res_init",
    "__ppoll_chk",
    "fgetsgent_r",
    "getc",
    "pwritev2",
    "wcpncpy",
    "_IO_wdefault_xsputn",
    "mkdtemp",
    "srand48_r",
    "sighold",
    "__sched_getparam",
    "__default_morecore",
    "iruserok",
    "cuserid",
    "isnan",
    "setstate_r",
    "_IO_file_stat",
    "argz_replace",
    "globfree64",
    "argp_usage",
    "timerfd_gettime",
    "__libc_alloc_buffer_copy_string",
    "clock_adjtime",
    "argz_next",
    "__fork",
    "getspnam_r",
    "__sched_yield",
    "__gmtime_r",
    "l64a",
    "_IO_file_attach",
    "wcsftime_l",
    "gets",
    "fflush",
    "_authenticate",
    "getrpcbyname",
    "putc_unlocked",
    "hcreate",
    "a64l",
    "xdr_long",
    "sigsuspend",
    "__libc_init_first",
    "_dl_signal_exception",
    "shmget",
    "_IO_wdo_write",
    "getw",
    "gethostid",
    "__cxa_at_quick_exit",
    "flockfile",
    "wcstof32x",
    "wcsncasecmp_l",
    "argz_add",
    "inotify_init1",
    "__backtrace_symbols",
    "_IO_un_link",
    "vasprintf",
    "__wcstod_internal",
    "authunix_create",
    "_mcount",
    "__wcstombs_chk",
    "__netlink_assert_response",
    "gmtime_r",
    "fchmod",
    "__printf_chk",
    "obstack_vprintf",
    "sigwait",
    "setgrent",
    "__fgetws_chk",
    "__register_atfork",
    "iswctype_l",
    "wctrans",
    "acct",
    "exit",
    "_IO_vfprintf",
    "execl",
    "re_set_syntax",
    "htonl",
    "wordexp",
    "endprotoent",
    "getprotobynumber_r",
    "__wcstof128_internal",
    "isinf",
    "__assert",
    "clearerr_unlocked",
    "fnmatch",
    "xdr_keybuf",
    "gnu_dev_major",
    "__islower_l",
    "readdir",
    "xdr_uint32_t",
    "htons",
    "pathconf",
    "sigrelse",
    "seed48_r",
    "psiginfo",
    "__nss_hostname_digits_dots",
    "execv",
    "sprintf",
    "_IO_putc",
    "nfsservctl",
    "envz_merge",
    "strftime_l",
    "setlocale",
    "memfrob",
    "mbrtowc",
    "srand",
    "iswcntrl_l",
    "getutid_r",
    "execvpe",
    "iswblank",
    "tr_break",
    "__libc_pthread_init",
    "__vfwprintf_chk",
    "fgetws_unlocked",
    "__write",
    "__select",
    "towlower",
    "ttyname_r",
    "fopen",
    "gai_strerror",
    "fgetspent",
    "strsignal",
    "wcsncpy",
    "getnetbyname_r",
    "getprotoent_r",
    "svcfd_create",
    "ftruncate",
    "xdr_unixcred",
    "dcngettext",
    "xdr_rmtcallres",
    "_IO_puts",
    "_dl_catch_error",
    "inet_nsap_addr",
    "inet_aton",
    "ttyslot",
    "wordfree",
    "posix_spawn_file_actions_addclose",
    "getdirentries",
    "_IO_unsave_markers",
    "_IO_default_uflow",
    "__strtold_internal",
    "__wcpcpy_chk",
    "__strcpy_small",
    "erand48",
    "__merge_grp",
    "wcstoul_l",
    "modify_ldt",
    "__libc_memalign",
    "isfdtype",
    "__strcspn_c1",
    "getfsfile",
    "__strcspn_c2",
    "lcong48",
    "__strcspn_c3",
    "getpwent",
    "re_match_2",
    "__nss_next2",
    "putgrent",
    "getservent_r",
    "argz_stringify",
    "open_wmemstream",
    "inet6_opt_append",
    "clock_getcpuclockid",
    "setservent",
    "timerfd_create",
    "posix_openpt",
    "svcerr_systemerr",
    "fflush_unlocked",
    "__isgraph_l",
    "__swprintf_chk",
    "vwprintf",
    "wait",
    "__read_nocancel",
    "setbuffer",
    "posix_memalign",
    "posix_spawnattr_setschedpolicy",
    "getipv4sourcefilter",
    "__vwprintf_chk",
    "__longjmp_chk",
    "tempnam",
    "isalpha",
    "__libc_alloc_buffer_alloc_array",
    "strtof_l",
    "regexec",
    "regexec",
    "llseek",
    "revoke",
    "re_match",
    "tdelete",
    "pipe",
    "readlinkat",
    "wcstof32_l",
    "__wctomb_chk",
    "get_avphys_pages",
    "authunix_create_default",
    "_IO_ferror",
    "getrpcbynumber",
    "__sysconf",
    "argz_count",
    "__strdup",
    "__readlink_chk",
    "register_printf_modifier",
    "__res_ninit",
    "setregid",
    "tcdrain",
    "setipv4sourcefilter",
    "wcstold",
    "cfmakeraw",
    "_IO_proc_open",
    "perror",
    "shmat",
    "__sbrk",
    "_IO_str_pbackfail",
    "rpmatch",
    "__getlogin_r_chk",
    "__isoc99_sscanf",
    "statvfs64",
    "pvalloc",
    "__libc_rpc_getport",
    "dcgettext",
    "_IO_fprintf",
    "_IO_wfile_overflow",
    "registerrpc",
    "__libc_dynarray_emplace_enlarge",
    "wcstoll",
    "posix_spawnattr_setpgroup",
    "qecvt_r",
    "__arch_prctl",
    "ecvt_r",
    "_IO_do_write",
    "getutxid",
    "wcscat",
    "_IO_switch_to_get_mode",
    "__fdelt_warn",
    "wcrtomb",
    "sync_file_range",
    "__signbitf",
    "getnetbyaddr",
    "connect",
    "wcspbrk",
    "__isnan",
    "__open64_2",
    "_longjmp",
    "envz_remove",
    "ngettext",
    "ldexpf",
    "fileno_unlocked",
    "strtof32_l",
    "__signbitl",
    "lutimes",
    "munlock",
    "ftruncate64",
    "getpwuid",
    "dl_iterate_phdr",
    "key_get_conv",
    "__nss_disable_nscd",
    "__nss_hash",
    "getpwent_r",
    "fts64_set",
    "mmap64",
    "sendfile",
    "__mmap",
    "inet6_rth_init",
    "strfromf64x",
    "strtof32x",
    "ldexpl",
    "inet6_opt_next",
    "__libc_allocate_rtsig_private",
    "ungetwc",
    "ecb_crypt",
    "__wcstof_l",
    "versionsort",
    "xdr_longlong_t",
    "tfind",
    "_IO_printf",
    "__argz_next",
    "wmemcpy",
    "pkey_free",
    "recvmmsg",
    "__fxstatat64",
    "posix_spawnattr_init",
    "fts64_children",
    "__sigismember",
    "get_current_dir_name",
    "semctl",
    "fputc_unlocked",
    "verr",
    "mbsrtowcs",
    "getprotobynumber",
    "fgetsgent",
    "getsecretkey",
    "__nss_services_lookup2",
    "unlinkat",
    "isalnum_l",
    "xdr_authdes_verf",
    "__fdelt_chk",
    "__strtof_internal",
    "closedir",
    "initgroups",
    "inet_ntoa",
    "wcstof_l",
    "__freelocale",
    "glob64",
    "glob64",
    "__fwprintf_chk",
    "pmap_rmtcall",
    "putc",
    "nanosleep",
    "setspent",
    "fchdir",
    "xdr_char",
    "__isinf",
    "fopencookie",
    "wcstoll_l",
    "ftrylockfile",
    "endaliasent",
    "isalpha_l",
    "_IO_wdefault_pbackfail",
    "feof_unlocked",
    "__nss_passwd_lookup2",
    "isblank",
    "getusershell",
    "svc_sendreply",
    "uselocale",
    "re_search_2",
    "getgrgid",
    "siginterrupt",
    "epoll_wait",
    "fputwc",
    "error",
    "mkfifoat",
    "get_kernel_syms",
    "getrpcent_r",
    "ftell",
    "__isoc99_scanf",
    "__read_chk",
    "inet_ntop",
    "signal",
    "__res_nclose",
    "__fgetws_unlocked_chk",
    "getdomainname",
    "personality",
    "puts",
    "__iswupper_l",
    "mbstowcs",
    "__vsprintf_chk",
    "__newlocale",
    "getpriority",
    "getsubopt",
    "fork",
    "tcgetsid",
    "putw",
    "ioperm",
    "warnx",
    "_IO_setvbuf",
    "pmap_unset",
    "iswspace",
    "_dl_mcount_wrapper_check",
    "__cxa_thread_atexit_impl",
    "isastream",
    "vwscanf",
    "fputws",
    "sigprocmask",
    "_IO_sputbackc",
    "strtoul_l",
    "listxattr",
    "regfree",
    "lcong48_r",
    "sched_getparam",
    "inet_netof",
    "gettext",
    "callrpc",
    "waitid",
    "futimes",
    "_IO_init_wmarker",
    "sigfillset",
    "__resolv_context_get_override",
    "gtty",
    "ntp_adjtime",
    "getgrent",
    "__libc_dynarray_finalize",
    "__libc_malloc",
    "__wcsncpy_chk",
    "readdir_r",
    "sigorset",
    "_IO_flush_all",
    "setreuid",
    "vfscanf",
    "memalign",
    "drand48_r",
    "endnetent",
    "fsetpos64",
    "hsearch_r",
    "__stack_chk_fail",
    "wcscasecmp",
    "_IO_feof",
    "key_setsecret",
    "daemon",
    "__lxstat",
    "svc_run",
    "_IO_wdefault_finish",
    "memfd_create",
    "__wcstoul_l",
    "shmctl",
    "inotify_rm_watch",
    "_IO_fflush",
    "xdr_quad_t",
    "unlink",
    "__mbrtowc",
    "putchar",
    "xdrmem_create",
    "pthread_mutex_lock",
    "listen",
    "fgets_unlocked",
    "putspent",
    "xdr_int32_t",
    "msgrcv",
    "__ivaliduser",
    "__send",
    "select",
    "getrpcent",
    "iswprint",
    "getsgent_r",
    "__iswalnum_l",
    "mkdir",
    "ispunct_l",
    "__libc_fatal",
    "__sched_cpualloc",
    "shmdt",
    "process_vm_writev",
    "realloc",
    "__pwrite64",
    "fstatfs",
    "setstate",
    "if_nameindex",
    "btowc",
    "__argz_stringify",
    "_IO_ungetc",
    "rewinddir",
    "strtold",
    "_IO_adjust_wcolumn",
    "fsync",
    "__iswalpha_l",
    "getaliasent_r",
    "xdr_key_netstres",
    "prlimit",
    "clock",
    "__obstack_vprintf_chk",
    "towupper",
    "sockatmark",
    "xdr_replymsg",
    "putmsg",
    "abort",
    "_IO_flush_all_linebuffered",
    "xdr_u_short",
    "__strtof128_nan",
    "strtoll",
    "_exit",
    "svc_getreq_common",
    "name_to_handle_at",
    "wcstoumax",
    "vsprintf",
    "sigwaitinfo",
    "moncontrol",
    "__res_iclose",
    "socketpair",
    "div",
    "__strtod_l",
    "scandirat",
    "ether_aton",
    "hdestroy",
    "__read",
    "tolower",
    "popen",
    "cfree",
    "ruserok_af",
    "_tolower",
    "step",
    "towctrans",
    "__dcgettext",
    "lsetxattr",
    "setttyent",
    "__isoc99_swscanf",
    "malloc_info",
    "__open64",
    "__bsd_getpgrp",
    "setsgent",
    "__tdelete",
    "getpid",
    "fts64_open",
    "kill",
    "getcontext",
    "__isoc99_vfwscanf",
    "pthread_condattr_init",
    "imaxdiv",
    "posix_fallocate64",
    "svcraw_create",
    "__snprintf",
    "fanotify_init",
    "__sched_get_priority_max",
    "__tfind",
    "argz_extract",
    "bind_textdomain_codeset",
    "fgetpos",
    "strdup",
    "_IO_fgetpos64",
    "svc_exit",
    "creat64",
    "getc_unlocked",
    "inet_pton",
    "strftime",
    "__flbf",
    "lockf64",
    "_IO_switch_to_main_wget_area",
    "xencrypt",
    "putpmsg",
    "__libc_system",
    "xdr_uint16_t",
    "__libc_mallopt",
    "sysv_signal",
    "pthread_attr_getschedparam",
    "strtoll_l",
    "__sched_cpufree",
    "__dup2",
    "pthread_mutex_destroy",
    "_dl_catch_exception",
    "fgetwc",
    "chmod",
    "vlimit",
    "sbrk",
    "__assert_fail",
    "clntunix_create",
    "iswalnum",
    "__toascii_l",
    "__isalnum_l",
    "printf",
    "__getmntent_r",
    "ether_ntoa_r",
    "finite",
    "quick_exit",
    "__connect",
    "quick_exit",
    "getnetbyname",
    "getentropy",
    "mkstemp",
    "flock",
    "statvfs",
    "error_at_line",
    "rewind",
    "strcoll_l",
    "llabs",
    "localtime_r",
    "wcscspn",
    "vtimes",
    "__libc_secure_getenv",
    "copysign",
    "inet6_opt_finish",
    "__nanosleep",
    "setjmp",
    "modff",
    "iswlower",
    "__poll",
    "isspace",
    "strtod",
    "tmpnam_r",
    "__confstr_chk",
    "fallocate",
    "__wctype_l",
    "setutxent",
    "fgetws",
    "__wcstoll_l",
    "__isalpha_l",
    "strtof",
    "iswdigit_l",
    "__wcsncat_chk",
    "__libc_msgsnd",
    "gmtime",
    "__uselocale",
    "__ctype_get_mb_cur_max",
    "ffs",
    "__iswlower_l",
    "xdr_opaque_auth",
    "modfl",
    "envz_add",
    "putsgent",
    "strtok",
    "getpt",
    "endpwent",
    "_IO_fopen",
    "strtol",
    "sigqueue",
    "fts_close",
    "isatty",
    "setmntent",
    "endnetgrent",
    "lchown",
    "mmap",
    "_IO_file_read",
    "getpw",
    "setsourcefilter",
    "fgetspent_r",
    "sched_yield",
    "glob_pattern_p",
    "__strsep_1c",
    "strtoq",
    "__clock_getcpuclockid",
    "wcsncasecmp",
    "ctime_r",
    "getgrnam_r",
    "clearenv",
    "xdr_u_quad_t",
    "wctype_l",
    "fstatvfs",
    "sigblock",
    "__libc_sa_len",
    "__libc_reallocarray",
    "__libc_alloc_buffer_allocate",
    "pthread_attr_setscope",
    "iswxdigit_l",
    "feof",
    "svcudp_create",
    "swapoff",
    "syslog",
    "copy_file_range",
    "posix_spawnattr_destroy",
    "__strtoul_l",
    "eaccess",
    "__fread_unlocked_chk",
    "fsetpos",
    "pread64",
    "inet6_option_alloc",
    "dysize",
    "symlink",
    "getspent",
    "_IO_wdefault_uflow",
    "pthread_attr_setdetachstate",
    "fgetxattr",
    "srandom_r",
    "truncate",
    "isprint",
    "__libc_calloc",
    "posix_fadvise",
    "memccpy",
    "getloadavg",
    "execle",
    "wcsftime",
    "__fentry__",
    "xdr_void",
    "ldiv",
    "__nss_configure_lookup",
    "cfsetispeed",
    "__recv",
    "ether_ntoa",
    "xdr_key_netstarg",
    "tee",
    "fgetc",
    "parse_printf_format",
    "strfry",
    "_IO_vsprintf",
    "reboot",
    "getaliasbyname_r",
    "jrand48",
    "execlp",
    "gethostbyname_r",
    "c16rtomb",
    "swab",
    "_IO_funlockfile",
    "wcstof64x",
    "_IO_flockfile",
    "__strsep_2c",
    "seekdir",
    "__mktemp",
    "__isascii_l",
    "isblank_l",
    "alphasort64",
    "pmap_getport",
    "makecontext",
    "fdatasync",
    "register_printf_specifier",
    "authdes_getucred",
    "truncate64",
    "__ispunct_l",
    "__iswgraph_l",
    "strtoumax",
    "argp_failure",
    "fgets",
    "__vfscanf",
    "__openat64_2",
    "__iswctype",
    "posix_spawnattr_setflags",
    "getnetent_r",
    "clock_nanosleep",
    "sched_setaffinity",
    "sched_setaffinity",
    "vscanf",
    "getpwnam",
    "inet6_option_append",
    "getppid",
    "calloc",
    "_IO_unsave_wmarkers",
    "getmsg",
    "_dl_addr",
    "msync",
    "renameat",
    "_IO_init",
    "__signbit",
    "futimens",
    "asctime_r",
    "freelocale",
    "initstate",
    "isxdigit",
    "mbrtoc16",
    "ungetc",
    "_IO_file_init",
    "__wuflow",
    "lockf",
    "ether_line",
    "xdr_authdes_cred",
    "__clock_gettime",
    "qecvt",
    "iswctype",
    "__mbrlen",
    "tmpfile",
    "__internal_setnetgrent",
    "xdr_int8_t",
    "envz_entry",
    "pivot_root",
    "sprofil",
    "__towupper_l",
    "rexec_af",
    "xprt_unregister",
    "newlocale",
    "xdr_authunix_parms",
    "tsearch",
    "getaliasbyname",
    "svcerr_progvers",
    "isspace_l",
    "inet6_opt_get_val",
    "argz_insert",
    "gsignal",
    "gethostbyname2_r",
    "__cxa_atexit",
    "posix_spawn_file_actions_init",
    "__fwriting",
    "prctl",
    "setlogmask",
    "malloc_stats",
    "__towctrans_l",
    "xdr_enum",
    "__strsep_3c",
    "unshare",
    "fread_unlocked",
    "brk",
    "send",
    "isprint_l",
    "setitimer",
    "__towctrans",
    "__isoc99_vsscanf",
    "setcontext",
    "iswupper_l",
    "signalfd",
    "sigemptyset",
    "inet6_option_next",
    "_dl_sym",
    "openlog",
    "getaddrinfo",
    "_IO_init_marker",
    "getchar_unlocked",
    "dirname",
    "__gconv_get_alias_db",
    "localeconv",
    "cfgetospeed",
    "writev",
    "pwritev64v2",
    "_IO_default_xsgetn",
    "isalnum",
    "setutent",
    "_seterr_reply",
    "_IO_switch_to_wget_mode",
    "inet6_rth_add",
    "fgetc_unlocked",
    "swprintf",
    "getchar",
    "warn",
    "getutid",
    "pkey_mprotect",
    "__gconv_get_cache",
    "glob",
    "glob",
    "semtimedop",
    "__secure_getenv",
    "__wcstof_internal",
    "islower",
    "tcsendbreak",
    "telldir",
    "__strtof_l",
    "utimensat",
    "fcvt",
    "_IO_setbuffer",
    "_IO_iter_file",
    "rmdir",
    "__errno_location",
    "tcsetattr",
    "__strtoll_l",
    "bind",
    "fseek",
    "xdr_float",
    "chdir",
    "open64",
    "confstr",
    "__libc_vfork",
    "muntrace",
    "read",
    "preadv2",
    "inet6_rth_segments",
    "getsgent",
    "getwchar",
    "getpagesize",
    "getnameinfo",
    "xdr_sizeof",
    "dgettext",
    "_IO_ftell",
    "putwc",
    "__pread_chk",
    "_IO_sprintf",
    "_IO_list_lock",
    "getrpcport",
    "__syslog_chk",
    "endgrent",
    "asctime",
    "strndup",
    "init_module",
    "mlock",
    "clnt_sperrno",
    "xdrrec_skiprecord",
    "__strcoll_l",
    "mbsnrtowcs",
    "__gai_sigqueue",
    "toupper",
    "sgetsgent_r",
    "mbtowc",
    "setprotoent",
    "__getpid",
    "eventfd",
    "netname2user",
    "_toupper",
    "getsockopt",
    "svctcp_create",
    "pkey_alloc",
    "getdelim",
    "_IO_wsetb",
    "setgroups",
    "setxattr",
    "clnt_perrno",
    "_IO_doallocbuf",
    "erand48_r",
    "lrand48",
    "grantpt",
    "__resolv_context_get",
    "ttyname",
    "mbrtoc32",
    "pthread_attr_init",
    "herror",
    "getopt",
    "wcstoul",
    "utmpname",
    "__fgets_unlocked_chk",
    "getlogin_r",
    "isdigit_l",
    "vfwprintf",
    "_IO_seekoff",
    "__setmntent",
    "hcreate_r",
    "tcflow",
    "wcstouq",
    "_IO_wdoallocbuf",
    "rexec",
    "msgget",
    "wcstof32x_l",
    "fwscanf",
    "xdr_int16_t",
    "__getcwd_chk",
    "fchmodat",
    "envz_strip",
    "dup2",
    "clearerr",
    "_IO_enable_locks",
    "__strtof128_internal",
    "dup3",
    "rcmd_af",
    "pause",
    "__rpc_thread_svc_max_pollfd",
    "__libc_scratch_buffer_grow",
    "unsetenv",
    "__posix_getopt",
    "rand_r",
    "__finite",
    "_IO_str_init_static",
    "timelocal",
    "__libc_dlvsym",
    "strtof64x",
    "xdr_pointer",
    "argz_add_sep",
    "wctob",
    "longjmp",
    "__fxstat64",
    "_IO_file_xsputn",
    "strptime",
    "clnt_sperror",
    "__adjtimex",
    "__vprintf_chk",
    "shutdown",
    "fattach",
    "setns",
    "vsnprintf",
    "_setjmp",
    "malloc_get_state",
    "poll",
    "getpmsg",
    "_IO_getline",
    "ptsname",
    "fexecve",
    "re_comp",
    "clnt_perror",
    "qgcvt",
    "svcerr_noproc",
    "__fprintf_chk",
    "open_by_handle_at",
    "_IO_marker_difference",
    "__wcstol_internal",
    "_IO_sscanf",
    "wcstof128_l",
    "sigaddset",
    "ctime",
    "iswupper",
    "svcerr_noprog",
    "fallocate64",
    "_IO_iter_end",
    "getgrnam",
    "__wmemcpy_chk",
    "adjtimex",
    "pthread_mutex_unlock",
    "sethostname",
    "_IO_setb",
    "__pread64",
    "mcheck",
    "__isblank_l",
    "xdr_reference",
    "getpwuid_r",
    "endrpcent",
    "__munmap",
    "netname2host",
    "inet_network",
    "isctype",
    "putenv",
    "wcswidth",
    "pmap_set",
    "fchown",
    "pthread_cond_broadcast",
    "pthread_cond_broadcast",
    "_IO_link_in",
    "ftok",
    "xdr_netobj",
    "catopen",
    "__wcstoull_l",
    "register_printf_function",
    "__sigsetjmp",
    "__isoc99_wscanf",
    "preadv64",
    "__ffs",
    "inet_makeaddr",
    "getttyent",
    "__libc_alloc_buffer_create_failure",
    "gethostbyaddr",
    "get_phys_pages",
    "_IO_popen",
    "argp_help",
    "fputc",
    "frexp",
    "__towlower_l",
    "gethostent_r",
    "_IO_seekmark",
    "psignal",
    "verrx",
    "setlogin",
    "versionsort64",
    "__internal_getnetgrent_r",
    "fseeko64",
    "fremovexattr",
    "__wcscpy_chk",
    "__libc_valloc",
    "recv",
    "__isoc99_fscanf",
    "_rpc_dtablesize",
    "_IO_sungetc",
    "getsid",
    "create_module",
    "mktemp",
    "inet_addr",
    "__mbstowcs_chk",
    "getrusage",
    "_IO_peekc_locked",
    "_IO_remove_marker",
    "__sendmmsg",
    "__isspace_l",
    "iswlower_l",
    "fts_read",
    "getfsspec",
    "__strtoll_internal",
    "iswgraph",
    "ualarm",
    "__dprintf_chk",
    "fputs",
    "query_module",
    "mlock2",
    "posix_spawn_file_actions_destroy",
    "strtok_r",
    "endhostent",
    "pthread_cond_wait",
    "pthread_cond_wait",
    "argz_delete",
    "__isprint_l",
    "xdr_u_long",
    "__woverflow",
    "__wmempcpy_chk",
    "fpathconf",
    "iscntrl_l",
    "regerror",
    "nrand48",
    "sendmmsg",
    "getspent_r",
    "wmempcpy",
    "lseek",
    "setresgid",
    "xdr_string",
    "ftime",
    "sigaltstack",
    "getwc",
    "memcpy",
    "endusershell",
    "__sched_get_priority_min",
    "__tsearch",
    "getwd",
    "mbrlen",
    "freopen64",
    "posix_spawnattr_setschedparam",
    "getdate_r",
    "fclose",
    "__libc_pread",
    "_IO_adjust_column",
    "_IO_seekwmark",
    "__nss_lookup",
    "__sigpause",
    "euidaccess",
    "symlinkat",
    "rand",
    "pselect",
    "pthread_setcanceltype",
    "tcsetpgrp",
    "nftw64",
    "wcscmp",
    "nftw64",
    "mprotect",
    "__getwd_chk",
    "ffsl",
    "__nss_lookup_function",
    "getmntent",
    "__wcscasecmp_l",
    "__strtol_internal",
    "__vsnprintf_chk",
    "mkostemp64",
    "__wcsftime_l",
    "_IO_file_doallocate",
    "pthread_setschedparam",
    "strtoul",
    "hdestroy_r",
    "fmemopen",
    "fmemopen",
    "endspent",
    "munlockall",
    "sigpause",
    "getutmp",
    "getutmpx",
    "vprintf",
    "xdr_u_int",
    "setsockopt",
    "_IO_default_xsputn",
    "malloc",
    "eventfd_read",
    "strtouq",
    "getpass",
    "remap_file_pages",
    "siglongjmp",
    "xdr_keystatus",
    "uselib",
    "sigisemptyset",
    "strfmon",
    "duplocale",
    "killpg",
    "xdr_int",
    "accept4",
    "umask",
    "__isoc99_vswscanf",
    "ftello64",
    "fdopendir",
    "realpath",
    "realpath",
    "pthread_attr_getschedpolicy",
    "modf",
    "ftello",
    "timegm",
    "__libc_dlclose",
    "__libc_mallinfo",
    "raise",
    "setegid",
    "__clock_getres",
    "setfsgid",
    "malloc_usable_size",
    "_IO_wdefault_doallocate",
    "__isdigit_l",
    "_IO_vfscanf",
    "remove",
    "sched_setscheduler",
    "timespec_get",
    "wcstold_l",
    "setpgid",
    "aligned_alloc",
    "__openat_2",
    "getpeername",
    "wcscasecmp_l",
    "__strverscmp",
    "__fgets_chk",
    "__libc_dynarray_resize_clear",
    "__res_state",
    "pmap_getmaps",
    "__strndup",
    "frexpf",
    "_flushlbf",
    "mbsinit",
    "towupper_l",
    "__strncpy_chk",
    "getgid",
    "asprintf",
    "tzset",
    "__libc_pwrite",
    "__copy_grp",
    "re_compile_pattern",
    "frexpl",
    "__lxstat64",
    "svcudp_bufcreate",
    "xdrrec_eof",
    "isupper",
    "vsyslog",
    "fstatfs64",
    "__strerror_r",
    "finitef",
    "getutline",
    "__uflow",
    "prlimit64",
    "strtol_l",
    "__isnanf",
    "finitel",
    "__nl_langinfo_l",
    "svc_getreq_poll",
    "pthread_attr_setinheritsched",
    "nl_langinfo",
    "__vsnprintf",
    "setfsent",
    "__isnanl",
    "wcstof64x_l",
    "hasmntopt",
    "clock_getres",
    "opendir",
    "__libc_current_sigrtmax",
    "wcsncat",
    "getnetbyaddr_r",
    "strfromf32",
    "__mbsrtowcs_chk",
    "_IO_fgets",
    "gethostent",
    "bzero",
    "clnt_broadcast",
    "mcheck_check_all",
    "__isinff",
    "__sigaddset",
    "pthread_condattr_destroy",
    "__statfs",
    "getspnam",
    "__wcscat_chk",
    "inet6_option_space",
    "__xstat64",
    "fgetgrent_r",
    "clone",
    "__ctype_b_loc",
    "sched_getaffinity",
    "__isinfl",
    "__iswpunct_l",
    "__xpg_sigpause",
    "getenv",
    "sched_getaffinity",
    "sscanf",
    "profil",
    "preadv",
    "jrand48_r",
    "setresuid",
    "__open_2",
    "recvfrom",
    "__profile_frequency",
    "wcsnrtombs",
    "ruserok",
    "_obstack_allocated_p",
    "fts_set",
    "xdr_u_longlong_t",
    "nice",
    "xdecrypt",
    "regcomp",
    "__fortify_fail",
    "getitimer",
    "__open",
    "isgraph",
    "catclose",
    "clntudp_bufcreate",
    "getservbyname",
    "__freading",
    "wcwidth",
    "msgctl",
    "inet_lnaof",
    "sigdelset",
    "ioctl",
    "syncfs",
    "gnu_get_libc_release",
    "fchownat",
    "alarm",
    "_IO_sputbackwc",
    "__libc_pvalloc",
    "system",
    "xdr_getcredres",
    "__wcstol_l",
    "__close_nocancel",
    "err",
    "vfwscanf",
    "chflags",
    "inotify_init",
    "timerfd_settime",
    "getservbyname_r",
    "strtof32",
    "ffsll",
    "xdr_bool",
    "__isctype",
    "setrlimit64",
    "sched_getcpu",
    "group_member",
    "_IO_free_backup_area",
    "munmap",
    "_IO_fgetpos",
    "posix_spawnattr_setsigdefault",
    "_obstack_begin_1",
    "endsgent",
    "_nss_files_parse_pwent",
    "ntp_gettimex",
    "wait3",
    "__getgroups_chk",
    "wait4",
    "_obstack_newchunk",
    "advance",
    "inet6_opt_init",
    "gethostbyname",
    "__snprintf_chk",
    "__lseek",
    "wcstol_l",
    "posix_spawn_file_actions_adddup2",
    "__iscntrl_l",
    "seteuid",
    "mkdirat",
    "dup",
    "setfsuid",
    "mrand48_r",
    "__strtod_nan",
    "strfromf128",
    "pthread_exit",
    "xdr_u_char",
    "getwchar_unlocked",
    "pututxline",
    "fchflags",
    "clock_settime",
    "getlogin",
    "msgsnd",
    "arch_prctl",
    "scalbnf",
    "sigandset",
    "_IO_file_finish",
    "sched_rr_get_interval",
    "__resolv_context_put",
    "__sysctl",
    "strfromd",
    "getgroups",
    "xdr_double",
    "strfromf",
    "scalbnl",
    "readv",
    "rcmd",
    "getuid",
    "iruserok_af",
    "readlink",
    "lsearch",
    "fscanf",
    "mkostemps64",
    "strfroml",
    "ether_aton_r",
    "__printf_fp",
    "readahead",
    "host2netname",
    "mremap",
    "removexattr",
    "_IO_switch_to_wbackup_area",
    "xdr_pmap",
    "execve",
    "getprotoent",
    "_IO_wfile_sync",
    "getegid",
    "xdr_opaque",
    "__libc_dynarray_resize",
    "setrlimit",
    "getopt_long",
    "_IO_file_open",
    "settimeofday",
    "open_memstream",
    "sstk",
    "getpgid",
    "utmpxname",
    "__fpurge",
    "_dl_vsym",
    "__strncat_chk",
    "__libc_current_sigrtmax_private",
    "strtold_l",
    "vwarnx",
    "posix_madvise",
    "explicit_bzero",
    "__mempcpy_small",
    "posix_spawnattr_getpgroup",
    "fgetpos64",
    "execvp",
    "pthread_attr_getdetachstate",
    "_IO_wfile_xsputn",
    "mincore",
    "mallinfo",
    "getauxval",
    "freeifaddrs",
    "__duplocale",
    "malloc_trim",
    "_IO_str_underflow",
    "svcudp_enablecache",
    "__wcsncasecmp_l",
    "linkat",
    "_IO_default_pbackfail",
    "inet6_rth_space",
    "_IO_free_wbackup_area",
    "pthread_cond_timedwait",
    "pthread_cond_timedwait",
    "_IO_fsetpos",
    "getpwnam_r",
    "__strtof_nan",
    "freopen",
    "__clock_nanosleep",
    "__libc_alloca_cutoff",
    "getsgnam",
    "backtrace_symbols_fd",
    "wcstof32",
    "__xmknod",
    "remque",
    "__recv_chk",
    "inet6_rth_reverse",
    "_IO_wfile_seekoff",
    "ptrace",
    "towlower_l",
    "getifaddrs",
    "scalbn",
    "putwc_unlocked",
    "printf_size_info",
    "if_nametoindex",
    "__wcstold_l",
    "strfromf64",
    "__wcstoll_internal",
    "creat",
    "__libc_alloc_buffer_copy_bytes",
    "__fxstat",
    "_IO_file_close_it",
    "_IO_file_close",
    "key_decryptsession_pk",
    "sendfile64",
    "wcstoimax",
    "sendmsg",
    "__backtrace_symbols_fd",
    "pwritev",
    "__strsep_g",
    "strtoull",
    "__wunderflow",
    "__fwritable",
    "_IO_fclose",
    "ulimit",
    "__sysv_signal",
    "__realpath_chk",
    "obstack_printf",
    "_IO_wfile_underflow",
    "posix_spawnattr_getsigmask",
    "fputwc_unlocked",
    "drand48",
    "qsort_r",
    "__nss_passwd_lookup",
    "xdr_free",
    "__obstack_printf_chk",
    "fileno",
    "pclose",
    "__isxdigit_l",
    "__bzero",
    "sethostent",
    "re_search",
    "inet6_rth_getaddr",
    "__setpgid",
    "__dgettext",
    "gethostname",
    "pthread_equal",
    "fstatvfs64",
    "sgetspent_r",
    "__libc_ifunc_impl_list",
    "__clone",
    "utimes",
    "pthread_mutex_init",
    "usleep",
    "sigset",
    "ustat",
    "chown",
    "__cmsg_nxthdr",
    "_obstack_memory_used",
    "__libc_realloc",
    "splice",
    "posix_spawn",
    "posix_spawn",
    "__iswblank_l",
    "_IO_sungetwc",
    "getcwd",
    "__getdelim",
    "xdr_vector",
    "eventfd_write",
    "swapcontext",
    "lgetxattr",
    "__rpc_thread_svc_fdset",
    "__finitef",
    "xdr_uint8_t",
    "strtof64",
    "wcsxfrm_l",
    "if_indextoname",
    "authdes_pk_create",
    "svcerr_decode",
    "swscanf",
    "vmsplice",
    "gnu_get_libc_version",
    "fwrite",
    "updwtmpx",
    "__finitel",
    "des_setparity",
    "getsourcefilter",
    "copysignf",
    "fread",
    "__cyg_profile_func_enter",
    "isnanf",
    "lrand48_r",
    "qfcvt_r",
    "fcvt_r",
    "iconv_close",
    "iswalnum_l",
    "adjtime",
    "getnetgrent_r",
    "_IO_wmarker_delta",
    "endttyent",
    "seed48",
    "rename",
    "copysignl",
    "sigaction",
    "rtime",
    "isnanl",
    "__explicit_bzero_chk",
    "_IO_default_finish",
    "getfsent",
    "epoll_ctl",
    "__isoc99_vwscanf",
    "__iswxdigit_l",
    "__ctype_init",
    "_IO_fputs",
    "fanotify_mark",
    "madvise",
    "_nss_files_parse_grent",
    "_dl_mcount_wrapper",
    "passwd2des",
    "getnetname",
    "setnetent",
    "__stpcpy_small",
    "__sigdelset",
    "mkstemp64",
    "scandir",
    "isinff",
    "gnu_dev_minor",
    "__libc_current_sigrtmin_private",
    "geteuid",
    "__libc_siglongjmp",
    "getresgid",
    "statfs",
    "ether_hostton",
    "mkstemps64",
    "sched_setparam",
    "iswalpha_l",
    "srandom",
    "quotactl",
    "__iswspace_l",
    "getrpcbynumber_r",
    "isinfl",
    "__open_catalog",
    "sigismember",
    "__isoc99_vfscanf",
    "getttynam",
    "atof",
    "re_set_registers",
    "__call_tls_dtors",
    "clock_gettime",
    "pthread_attr_setschedparam",
    "bcopy",
    "setlinebuf",
    "__stpncpy_chk",
    "getsgnam_r",
    "wcswcs",
    "atoi",
    "__strtok_r_1c",
    "xdr_hyper",
    "__iswprint_l",
    "stime",
    "getdirentries64",
    "textdomain",
    "posix_spawnattr_getschedparam",
    "sched_get_priority_max",
    "tcflush",
    "atol",
    "inet6_opt_find",
    "wcstoull",
    "mlockall",
    "ether_ntohost",
    "waitpid",
    "ftw64",
    "iswxdigit",
    "stty",
    "__fpending",
    "unlockpt",
    "close",
    "__mbsnrtowcs_chk",
    "strverscmp",
    "xdr_union",
    "backtrace",
    "catgets",
    "posix_spawnattr_getschedpolicy",
    "lldiv",
    "pthread_setcancelstate",
    "endutent",
    "tmpnam",
    "inet_nsap_ntoa",
    "strerror_l",
    "open",
    "twalk",
    "srand48",
    "toupper_l",
    "svcunixfd_create",
    "ftw",
    "iopl",
    "__wcstoull_internal",
    "strerror_r",
    "sgetspent",
    "_IO_iter_begin",
    "pthread_getschedparam",
    "__fread_chk",
    "c32rtomb",
    "dngettext",
    "vhangup",
    "__rpc_thread_createerr",
    "key_secretkey_is_set",
    "localtime",
    "endutxent",
    "swapon",
    "umount",
    "lseek64",
    "__wcsnrtombs_chk",
    "ferror_unlocked",
    "difftime",
    "wctrans_l",
    "capset",
    "_Exit",
    "flistxattr",
    "clnt_spcreateerror",
    "obstack_free",
    "pthread_attr_getscope",
    "wcstof64",
    "getaliasent",
    "sigreturn",
    "rresvport_af",
    "secure_getenv",
    "sigignore",
    "iswdigit",
    "svcerr_weakauth",
    "__monstartup",
    "iswcntrl",
    "fcloseall",
    "__wprintf_chk",
    "funlockfile",
    "endmntent",
    "fprintf",
    "getsockname",
    "scandir64",
    "utime",
    "hsearch",
    "__open_nocancel",
    "__strtold_nan",
    "argp_error",
    "__strpbrk_c2",
    "__strpbrk_c3",
    "abs",
    "sendto",
    "iswpunct_l",
    "addmntent",
    "__libc_scratch_buffer_grow_preserve",
    "updwtmp",
    "__strtold_l",
    "__nss_database_lookup",
    "_IO_least_wmarker",
    "vfork",
    "addseverity",
    "__poll_chk",
    "epoll_create1",
    "xprt_register",
    "getgrent_r",
    "key_gendes",
    "__vfprintf_chk",
    "_dl_signal_error",
    "mktime",
    "mblen",
    "tdestroy",
    "sysctl",
    "__getauxval",
    "clnt_create",
    "alphasort",
    "xdr_rmtcall_args",
    "__strtok_r",
    "xdrstdio_create",
    "mallopt",
    "strtof32x_l",
    "strtoimax",
    "getline",
    "__iswdigit_l",
    "getrpcbyname_r",
    "iconv",
    "get_myaddress",
    "bdflush",
    "imaxabs",
    "mkstemps",
    "lremovexattr",
    "re_compile_fastmap",
    "setusershell",
    "fdopen",
    "_IO_str_seekoff",
    "readdir64",
    "svcerr_auth",
    "xdr_callmsg",
    "qsort",
    "canonicalize_file_name",
    "__getpgid",
    "_IO_sgetn",
    "iconv_open",
    "process_vm_readv",
    "_IO_fsetpos64",
    "__strtod_internal",
    "strfmon_l",
    "mrand48",
    "wcstombs",
    "posix_spawnattr_getflags",
    "accept",
    "__libc_free",
    "gethostbyname2",
    "__strtoull_l",
    "__nss_hosts_lookup",
    "cbc_crypt",
    "_IO_str_overflow",
    "argp_parse",
    "envz_get",
    "xdr_netnamestr",
    "_IO_seekpos",
    "getresuid",
    "__vsyslog_chk",
    "posix_spawnattr_setsigmask",
    "hstrerror",
    "__strcasestr",
    "inotify_add_watch",
    "_IO_proc_close",
    "statfs64",
    "tcgetattr",
    "toascii",
    "authnone_create",
    "isupper_l",
    "__mprotect",
    "getutxline",
    "sethostid",
    "tmpfile64",
    "sleep",
    "wcsxfrm",
    "times",
    "_IO_file_sync",
    "strtof128_l",
    "strxfrm_l",
    "__gconv_transliterate",
    "__libc_allocate_rtsig",
    "__wcrtomb_chk",
    "__ctype_toupper_loc",
    "clntraw_create",
    "wcstof128",
    "pwritev64",
    "insque",
    "__getpagesize",
    "epoll_pwait",
    "valloc",
    "__strcpy_chk",
    "__ctype_tolower_loc",
    "getutxent",
    "_IO_list_unlock",
    "__vdprintf_chk",
    "fputws_unlocked",
    "xdr_array",
    "llistxattr",
    "__nss_group_lookup2",
    "__cxa_finalize",
    "__libc_current_sigrtmin",
    "umount2",
    "syscall",
    "sigpending",
    "bsearch",
    "__assert_perror_fail",
    "freeaddrinfo",
    "__vasprintf_chk",
    "get_nprocs",
    "setvbuf",
    "getprotobyname_r",
    "__xpg_strerror_r",
    "__wcsxfrm_l",
    "__resolv_context_get_preinit",
    "vsscanf",
    "__libc_scratch_buffer_set_array_size",
    "fgetpwent",
    "gethostbyaddr_r",
    "setaliasent",
    "xdr_rejected_reply",
    "capget",
    "__sigsuspend",
    "readdir64_r",
    "getpublickey",
    "__sched_setscheduler",
    "__rpc_thread_svc_pollfd",
    "svc_unregister",
    "fts_open",
    "setsid",
    "pututline",
    "sgetsgent",
    "getutent",
    "posix_spawnattr_getsigdefault",
    "iswgraph_l",
    "wcscoll",
    "register_printf_type",
    "printf_size",
    "pthread_attr_destroy",
    "__wcstoul_internal",
    "nrand48_r",
    "strfromf32x",
    "xdr_uint64_t",
    "svcunix_create",
    "__sigaction",
    "_nss_files_parse_spent",
    "cfsetspeed",
    "__wcpncpy_chk",
    "fcntl",
    "wcsspn",
    "getrlimit64",
    "wctype",
    "inet6_option_init",
    "__iswctype_l",
    "__libc_clntudp_bufcreate",
    "ecvt",
    "__wmemmove_chk",
    "__sprintf_chk",
    "bindresvport",
    "rresvport",
    "__asprintf",
    "cfsetospeed",
    "fwide",
    "getgrgid_r",
    "pthread_cond_init",
    "pthread_cond_init",
    "setpgrp",
    "cfgetispeed",
    "wcsdup",
    "__socket",
    "atoll",
    "bsd_signal",
    "__strtol_l",
    "ptsname_r",
    "xdrrec_create",
    "__h_errno_location",
    "fsetxattr",
    "__inet6_scopeid_pton",
    "_IO_file_seekoff",
    "_IO_ftrylockfile",
    "__sigtimedwait",
    "__close",
    "_IO_iter_next",
    "getmntent_r",
    "labs",
    "link",
    "__strftime_l",
    "xdr_cryptkeyres",
    "innetgr",
    "openat",
    "futimesat",
    "_IO_wdefault_xsgetn",
    "__iswcntrl_l",
    "__pread64_chk",
    "vdprintf",
    "vswprintf",
    "_IO_getline_info",
    "clntudp_create",
    "scandirat64",
    "getprotobyname",
    "__twalk",
    "strptime_l",
    "argz_create_sep",
    "tolower_l",
    "__fsetlocking",
    "__backtrace",
    "__xstat",
    "wcscoll_l",
    "__madvise",
    "getrlimit",
    "sigsetmask",
    "scanf",
    "isdigit",
    "getxattr",
    "lchmod",
    "key_encryptsession",
    "iscntrl",
    "__libc_msgrcv",
    "mount",
    "getdtablesize",
    "random_r",
    "__toupper_l",
    "iswpunct",
    "errx",
    "key_setnet",
    "_IO_file_write",
    "uname",
    "svc_getreqset",
    "wcstod",
    "__chk_fail",
    "mcount",
    "posix_spawnp",
    "__isoc99_vscanf",
    "mprobe",
    "posix_spawnp",
    "_IO_file_overflow",
    "wcstof",
    "backtrace_symbols",
    "__wcsrtombs_chk",
    "_IO_list_resetlock",
    "_mcleanup",
    "__wctrans_l",
    "isxdigit_l",
    "_IO_fwrite",
    "sigtimedwait",
    "pthread_self",
    "wcstok",
    "ruserpass",
    "svc_register",
    "__waitpid",
    "wcstol",
    "pkey_set",
    "endservent",
    "fopen64",
    "pthread_attr_setschedpolicy",
    "vswscanf",
    "__nss_group_lookup",
    "ctermid",
    "pread",
    "wcschrnul",
    "__libc_dlsym",
    "__endmntent",
    "wcstoq",
    "pwrite",
    "sigstack",
    "mkostemp",
    "__vfork",
    "__freadable",
    "strsep",
    "iswblank_l",
    "mkostemps",
    "_IO_file_underflow",
    "_obstack_begin",
    "getnetgrent",
    "user2netname",
    "bindtextdomain",
    "wcsrtombs",
    "getrandom",
    "__nss_next",
    "wcstof64_l",
    "access",
    "fts64_read",
    "fmtmsg",
    "__sched_getscheduler",
    "qfcvt",
    "mcheck_pedantic",
    "mtrace",
    "ntp_gettime",
    "_IO_getc",
    "pipe2",
    "memmem",
    "__fxstatat",
    "__fbufsize",
    "_IO_marker_delta",
    "sync",
    "getgrouplist",
    "sysinfo",
    "sigvec",
    "getwc_unlocked",
    "svc_getreq",
    "argz_append",
    "setgid",
    "malloc_set_state",
    "__inet_pton_length",
    "preadv64v2",
    "__strcat_chk",
    "wprintf",
    "__argz_count",
    "ulckpwdf",
    "fts_children",
    "strxfrm",
    "getservbyport_r",
    "mkfifo",
    "openat64",
    "sched_getscheduler",
    "faccessat",
    "on_exit",
    "__res_randomid",
    "setbuf",
    "fwrite_unlocked",
    "strtof128",
    "_IO_gets",
    "__libc_longjmp",
    "recvmsg",
    "__strtoull_internal",
    "iswspace_l",
    "islower_l",
    "__underflow",
    "pwrite64",
    "strerror",
    "xdr_wrapstring",
    "__asprintf_chk",
    "__strfmon_l",
    "tcgetpgrp",
    "strtof64_l",
    "__libc_start_main",
    "fgetwc_unlocked",
    "dirfd",
    "_nss_files_parse_sgent",
    "nftw",
    "xdr_des_block",
    "nftw",
    "xdr_cryptkeyarg2",
    "xdr_callhdr",
    "setpwent",
    "iswprint_l",
    "strtof64x_l",
    "semop",
    "endfsent",
    "__isupper_l",
    "wscanf",
    "ferror",
    "getutent_r",
    "authdes_create",
    "ppoll",
    "__strxfrm_l",
    "fdetach",
    "pthread_cond_destroy",
    "ldexp",
    "fgetpwent_r",
    "pthread_cond_destroy",
    "__wait",
    "gcvt",
    "fwprintf",
    "xdr_bytes",
    "setenv",
    "setpriority",
    "__libc_dlopen_mode",
    "posix_spawn_file_actions_addopen",
    "nl_langinfo_l",
    "_IO_default_doallocate",
    "__gconv_get_modules_db",
    "__recvfrom_chk",
    "_IO_fread",
    "fgetgrent",
    "setdomainname",
    "write",
    "__clock_settime",
    "getservbyport",
    "if_freenameindex",
    "strtod_l",
    "getnetent",
    "getutline_r",
    "posix_fallocate",
    "__pipe",
    "fseeko",
    "xdrrec_endofrecord",
    "lckpwdf",
    "towctrans_l",
    "inet6_opt_set_val",
    "vfprintf",
    "strcoll",
    "ssignal",
    "random",
    "globfree",
    "delete_module",
    "basename",
    "argp_state_help",
    "__wcstold_internal",
    "ntohl",
    "closelog",
    "getopt_long_only",
    "getpgrp",
    "isascii",
    "get_nprocs_conf",
    "wcsncmp",
    "re_exec",
    "clnt_pcreateerror",
    "monstartup",
    "__ptsname_r_chk",
    "__fcntl",
    "ntohs",
    "pkey_get",
    "snprintf",
    "__overflow",
    "__isoc99_fwscanf",
    "posix_fadvise64",
    "xdr_cryptkeyarg",
    "__strtoul_internal",
    "wmemmove",
    "sysconf",
    "__gets_chk",
    "_obstack_free",
    "setnetgrent",
    "gnu_dev_makedev",
    "xdr_u_hyper",
    "__xmknodat",
    "wcstoull_l",
    "_IO_fdopen",
    "inet6_option_find",
    "isgraph_l",
    "getservent",
    "clnttcp_create",
    "__ttyname_r_chk",
    "wctomb",
    "reallocarray",
    "fputs_unlocked",
    "siggetmask",
    "putwchar_unlocked",
    "semget",
    "putpwent",
    "_IO_str_init_readonly",
    "xdr_accepted_reply",
    "initstate_r",
    "__vsscanf",
    "wcsstr",
    "free",
    "_IO_file_seek",
    "__libc_dynarray_at_failure",
    "ispunct",
    "__cyg_profile_func_exit",
    "pthread_attr_getinheritsched",
    "__readlinkat_chk",
    "__nss_hosts_lookup2",
    "key_decryptsession",
    "vwarn",
    "fts64_close",
    "wcpcpy",

    /////////////////  libpthread.so  ////////////////////////////////////
    "pthread_getaffinity_np",
    "write",
    "pthread_getaffinity_np",
    "pthread_setname_np",
    "pthread_mutexattr_setrobust",
    "__wait",
    "__pthread_clock_gettime",
    "__pthread_rwlock_init",
    "__pthread_cleanup_routine",
    "sem_close",
    "pthread_cond_init",
    "pthread_cond_init",
    "__pthread_mutexattr_settype",
    "pthread_mutexattr_setkind_np",
    "pthread_rwlock_trywrlock",
    "pthread_mutex_destroy",
    "__pthread_rwlock_destroy",
    "send",
    "open64",
    "pthread_yield",
    "pthread_equal",
    "read",
    "pthread_getconcurrency",
    "pthread_self",
    "__pthread_rwlock_rdlock",
    "pthread_spin_trylock",
    "__pthread_getspecific",
    "pthread_rwlock_wrlock",
    "pthread_attr_setscope",
    "pthread_condattr_setpshared",
    "recvfrom",
    "sem_getvalue",
    "__libc_current_sigrtmin",
    "__shm_directory",
    "system",
    "pthread_setschedparam",
    "__pthread_key_create",
    "pthread_condattr_setclock",
    "pthread_mutex_getprioceiling",
    "wait",
    "_pthread_cleanup_push_defer",
    "pthread_getname_np",
    "pthread_attr_getdetachstate",
    "pthread_mutex_consistent",
    "pthread_tryjoin_np",
    "pthread_sigmask",
    "__pthread_rwlock_trywrlock",
    "__pthread_register_cancel_defer",
    "tcdrain",
    "pthread_atfork",
    "pthread_mutexattr_getkind_np",
    "pthread_mutex_init",
    "pthread_cond_wait",
    "pthread_cond_wait",
    "__h_errno_location",
    "pthread_exit",
    "pthread_setcancelstate",
    "pthread_cond_destroy",
    "pthread_cond_destroy",
    "pthread_mutexattr_settype",
    "sem_timedwait",
    "pthread_setattr_default_np",
    "pthread_rwlock_unlock",
    "recv",
    "fsync",
    "pthread_timedjoin_np",
    "pthread_mutexattr_gettype",
    "__errno_location",
    "__pthread_get_minstack",
    "pthread_key_delete",
    "pthread_once",
    "pthread_sigqueue",
    "pthread_kill_other_threads_np",
    "pthread_attr_setstackaddr",
    "raise",
    "sem_post",
    "pthread_kill",
    "pthread_mutex_lock",
    "pthread_mutexattr_init",
    "pthread_setspecific",
    "pread64",
    "pthread_barrierattr_setpshared",
    "__fcntl",
    "pwrite",
    "__pthread_mutex_init",
    "pthread_mutexattr_setpshared",
    "pthread_cond_signal",
    "pthread_cond_signal",
    "pread",
    "pthread_rwlockattr_init",
    "pthread_attr_getschedpolicy",
    "__connect",
    "__pthread_unregister_cancel_restore",
    "pthread_mutexattr_setprioceiling",
    "__nanosleep",
    "pthread_cond_timedwait",
    "pthread_cond_timedwait",
    "pthread_rwlock_timedwrlock",
    "accept",
    "__fork",
    "pthread_spin_init",
    "pthread_getattr_np",
    "pthread_mutexattr_getprotocol",
    "lseek64",
    "sem_destroy",
    "pthread_attr_getstackaddr",
    "fcntl",
    "pthread_attr_setstack",
    "__pthread_mutex_lock",
    "pthread_attr_getscope",
    "__pthread_initialize_minimal",
    "pthread_cancel",
    "pthread_rwlock_init",
    "pthread_attr_setaffinity_np",
    "pthread_attr_setaffinity_np",
    "pthread_create",
    "pthread_barrierattr_destroy",
    "__pthread_mutexattr_destroy",
    "pwrite64",
    "__pthread_mutex_destroy",
    "pthread_join",
    "pthread_barrierattr_getpshared",
    "pthread_condattr_getclock",
    "sem_open",
    "pthread_attr_getinheritsched",
    "sigaction",
    "pthread_mutexattr_getpshared",
    "pthread_setconcurrency",
    "__pthread_once",
    "pthread_spin_lock",
    "pthread_attr_getschedparam",
    "__pthread_rwlock_wrlock",
    "sem_unlink",
    "pthread_getspecific",
    "__pthread_unwind",
    "pthread_mutex_timedlock",
    "pthread_barrier_destroy",
    "pthread_mutexattr_setprotocol",
    "fork",
    "siglongjmp",
    "__pthread_mutexattr_init",
    "__pthread_unwind_next",
    "pthread_setschedprio",
    "__res_state",
    "ftrylockfile",
    "funlockfile",
    "pthread_barrier_init",
    "pthread_mutex_consistent_np",
    "pthread_getschedparam",
    "__pthread_mutex_unlock",
    "__libc_current_sigrtmax",
    "pthread_mutexattr_getrobust",
    "pthread_getcpuclockid",
    "__pthread_unregister_cancel",
    "__libc_allocate_rtsig",
    "pthread_rwlock_tryrdlock",
    "pthread_attr_setinheritsched",
    "__pthread_barrier_init",
    "pthread_rwlock_timedrdlock",
    "__open",
    "pthread_barrierattr_init",
    "msync",
    "pthread_attr_init",
    "_pthread_cleanup_pop_restore",
    "__pthread_clock_settime",
    "pthread_mutexattr_destroy",
    "pthread_key_create",
    "pthread_rwlock_rdlock",
    "__pthread_rwlock_unlock",
    "pthread_rwlockattr_getkind_np",
    "connect",
    "sem_init",
    "__pthread_register_cancel",
    "pthread_attr_getaffinity_np",
    "pthread_attr_getaffinity_np",
    "pthread_cond_broadcast",
    "pthread_cond_broadcast",
    "pthread_detach",
    "pthread_attr_setschedparam",
    "sem_trywait",
    "__pthread_mutex_trylock",
    "pthread_attr_setstacksize",
    "__pthread_rwlock_tryrdlock",
    "pthread_setaffinity_np",
    "pthread_setaffinity_np",
    "pthread_barrier_wait",
    "pthread_mutexattr_getrobust_np",
    "_IO_ftrylockfile",
    "pthread_attr_setschedpolicy",
    "__close",
    "pthread_attr_destroy",
    "sendto",
    "flockfile",
    "pthread_spin_unlock",
    "pthread_rwlockattr_destroy",
    "open",
    "pthread_attr_getstack",
    "__pthread_barrier_wait",
    "pthread_rwlockattr_setpshared",
    "pthread_condattr_init",
    "_pthread_cleanup_pop",
    "pthread_attr_setguardsize",
    "pthread_rwlock_destroy",
    "pthread_spin_destroy",
    "__lseek",
    "pthread_rwlockattr_setkind_np",
    "pause",
    "__open64",
    "pthread_attr_getstacksize",
    "sem_wait",
    "close",
    "pthread_mutex_unlock",
    "__pwrite64",
    "longjmp",
    "nanosleep",
    "pthread_mutex_setprioceiling",
    "__pthread_setspecific",
    "__write",
    "_IO_funlockfile",
    "_IO_flockfile",
    "pthread_attr_setdetachstate",
    "__send",
    "sigwait",
    "pthread_condattr_destroy",
    "pthread_condattr_getpshared",
    "sendmsg",
    "__read",
    "lseek",
    "pthread_getattr_default_np",
    "pthread_attr_getguardsize",
    "recvmsg",
    "pthread_mutexattr_getprioceiling",
    "pthread_setcanceltype",
    "waitpid",
    "pthread_rwlockattr_getpshared",
    "pthread_mutexattr_setrobust_np",
    "_pthread_cleanup_push",
    "pthread_mutex_trylock",
    "__pread64",
    "pthread_testcancel",
    "__sigaction",


};

// objdump -T /usr/lib/x86_64-linux-gnu/libstdc++.so.6 | grep "DF .text" | awk '{print "\""$7"\"\,"}' | tee names.txt
static char * libcxx_names[] = {
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE4swapERS3_",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1EPKwmRKS1_",
    "_ZNSt5ctypeIwED2Ev",
    "_ZNKSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_RSt8ios_basecPK2tmPKcSB_",
    "_ZNSt18condition_variable10notify_oneEv",
    "_ZNKSt3_V214error_category10_M_messageB5cxx11Ei",
    "_ZSt9has_facetISt11__timepunctIwEEbRKSt6locale",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEEC2ESt13_Ios_Openmode",
    "_ZNSt18__moneypunct_cacheIcLb1EEC2Em",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEEC1EP8_IO_FILE",
    "_ZNSt7__cxx1110moneypunctIwLb1EEC1EPSt18__moneypunct_cacheIwLb1EEm",
    "_ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE11do_get_yearES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt7__cxx1115time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEaSEPKc",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE5uflowEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep7_M_grabERKS1_S5_",
    "_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKNSt7__cxx1112basic_stringIS4_S5_T1_EE",
    "_ZNKSt8valarrayImE4sizeEv",
    "_ZNKSbIwSt11char_traitsIwESaIwEE5emptyEv",
    "_ZNSsC2IPcEET_S1_RKSaIcE",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE11frac_digitsEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNKSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEE3strEv",
    "_ZNSt9type_infoD0Ev",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE9pubsetbufEPcl",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRPv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEEC2Ev",
    "_ZNSiC2EPSt15basic_streambufIcSt11char_traitsIcEE",
    "_ZNSt12strstreambufC1EPKal",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13_S_copy_charsEPcN9__gnu_cxx17__normal_iteratorIPKcS4_EESA_",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEE4swapERS3_",
    "_ZNSt7__cxx1110moneypunctIwLb1EED2Ev",
    "_ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZN10__cxxabiv120__si_class_type_infoD2Ev",
    "_ZNSo9_M_insertIdEERSoT_",
    "_ZTv0_n24_NSt14basic_iostreamIwSt11char_traitsIwEED1Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE3getERw",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEEC2ESt13_Ios_Openmode",
    "_ZNSt14codecvt_bynameIwc11__mbstate_tED2Ev",
    "_ZGTtNSt12domain_errorC1EPKc",
    "_ZNKSt10moneypunctIcLb1EE13positive_signEv",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14_M_extract_intIyEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNKSt8ios_base7failure4whatEv",
    "_ZNKSt10moneypunctIwLb1EE13positive_signEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5eraseEN9__gnu_cxx17__normal_iteratorIPcS4_EE",
    "_ZNSbIwSt11char_traitsIwESaIwEE7_M_dataEPw",
    "_ZTv0_n24_NSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE6rbeginEv",
    "_ZTv0_n24_NSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_PKwS8_",
    "_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_RS3_",
    "_ZNKSs4findEPKcm",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE5sputnEPKwl",
    "_ZNSi10_M_extractIPvEERSiRT_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPKwS4_EES9_S8_m",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb0EED2Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5beginEv",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNSt19istreambuf_iteratorIcSt11char_traitsIcEEppEv",
    "_ZNSt12length_errorD2Ev",
    "_ZNSt19istreambuf_iteratorIcSt11char_traitsIcEEppEv",
    "_ZTv0_n24_NSt18basic_stringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNSt14collate_bynameIwEC1EPKcm",
    "_ZNKSs2atEm",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsgetnEPwl",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE3badEv",
    "_ZNSt13runtime_errorC1EPKc",
    "_ZNKSt15__exception_ptr13exception_ptrcvMS0_FvvEEv",
    "_ZNSt7__cxx1114collate_bynameIwEC2EPKcm",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEixEm",
    "_ZNSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZNSt8valarrayImED2Ev",
    "_ZNSt7codecvtIwc11__mbstate_tED2Ev",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEE4swapERS3_",
    "_ZNKSbIwSt11char_traitsIwESaIwEE8capacityEv",
    "_ZNSs5eraseEN9__gnu_cxx17__normal_iteratorIPcSsEES2_",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7compareEmmRKS4_mm",
    "_ZNSt10moneypunctIcLb0EED1Ev",
    "_ZNSt16invalid_argumentD2Ev",
    "_ZNSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC2Em",
    "_ZNKSt19basic_istringstreamIcSt11char_traitsIcESaIcEE5rdbufEv",
    "_ZNKSs13get_allocatorEv",
    "_ZSt9terminatev",
    "_ZGTtNSt13runtime_errorD2Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE5crendEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2EOS4_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendERKS4_mm",
    "_ZNKSs7compareEmmPKc",
    "_ZNSt8numpunctIcED1Ev",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1Ev",
    "_ZGTtNSt11logic_errorC2EPKc",
    "_ZNKSt5ctypeIwE10do_toupperEPwPKw",
    "_ZSt9use_facetINSt7__cxx1110moneypunctIwLb0EEEERKT_RKSt6locale",
    "_ZNKSt20__codecvt_utf16_baseIDiE13do_max_lengthEv",
    "_ZNSt8numpunctIwED0Ev",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE6setbufEPcl",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13shrink_to_fitEv",
    "_ZNK10__cxxabiv120__si_class_type_info20__do_find_public_srcElPKvPKNS_17__class_type_infoES2_",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE10pos_formatEv",
    "_ZNSt17bad_function_callD0Ev",
    "_ZNK10__cxxabiv117__class_type_info10__do_catchEPKSt9type_infoPPvj",
    "_ZGTtNKSt9exceptionD1Ev",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE13_M_set_bufferEl",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2EOS4_RKS3_",
    "_ZNKSt10moneypunctIwLb0EE11curr_symbolEv",
    "_ZNSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC1Em",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEEC2ERKNS_12basic_stringIcS2_S3_EESt13_Ios_Openmode",
    "_ZNSt6locale5_ImplC1ERKS0_m",
    "_ZNSt10moneypunctIwLb1EE24_M_initialize_moneypunctEP15__locale_structPKc",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE27_M_allocate_internal_bufferEv",
    "_ZNSt17moneypunct_bynameIwLb1EED2Ev",
    "_ZNSt19__codecvt_utf8_baseIDiED0Ev",
    "_ZNSt7__cxx117collateIcED2Ev",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEEC1EOS4_ONS4_14__xfer_bufptrsE",
    "_ZGTtNSt15underflow_errorC2EPKc",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE3getERSt15basic_streambufIwS1_E",
    "_ZNSt12domain_errorD2Ev",
    "_ZNSt15__exception_ptr13exception_ptrC2ERKS0_",
    "_ZNKSt8messagesIwE6do_getEiiiRKSbIwSt11char_traitsIwESaIwEE",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEEC2ERSt14basic_iostreamIwS1_E",
    "_ZTv0_n24_NSt13basic_fstreamIwSt11char_traitsIwEED1Ev",
    "_ZNKSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEE3strEv",
    "_ZNSt15messages_bynameIwEC1ERKSsm",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEED2Ev",
    "_ZNSt6locale5_ImplC2EPKcm",
    "_ZNKSs4findERKSsm",
    "_ZSt9has_facetINSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEEEbRKSt6locale",
    "_ZNSt17moneypunct_bynameIcLb1EEC1EPKcm",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE4fillEc",
    "_ZNSt12ctype_bynameIcEC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSs5frontEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE6sbumpcEv",
    "_ZTv0_n24_NSt13basic_istreamIwSt11char_traitsIwEED1Ev",
    "_ZNSt11__timepunctIwEC2Em",
    "_ZStlsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_St8_Setbase",
    "_ZNSt7__cxx118numpunctIwE22_M_initialize_numpunctEP15__locale_struct",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendERKS4_",
    "_ZNKSt20__codecvt_utf16_baseIDiE11do_encodingEv",
    "_ZNSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEEaSERKS2_",
    "_ZSt9has_facetISt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEEbRKSt6locale",
    "_ZSt9has_facetISt11__timepunctIcEEbRKSt6locale",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ERKNS_12basic_stringIcS2_S3_EESt13_Ios_Openmode",
    "_ZNSt25__codecvt_utf8_utf16_baseIDiED1Ev",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEEC2EOS4_ONS4_14__xfer_bufptrsE",
    "_ZNSt11__timepunctIcED0Ev",
    "_ZNSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZNSt12strstreambufC1EPKcl",
    "_ZNSt8messagesIcEC2EP15__locale_structPKcm",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7compareERKS4_",
    "__cxa_call_unexpected",
    "_ZNSt7__cxx1110moneypunctIcLb1EEC2EPSt18__moneypunct_cacheIcLb1EEm",
    "_ZNSaIcED1Ev",
    "_ZStlsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_St5_Setw",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEEaSEOS2_",
    "_ZNKSt7codecvtIcc11__mbstate_tE10do_unshiftERS0_PcS3_RS3_",
    "_ZNSt7__cxx118messagesIcEC2EP15__locale_structPKcm",
    "_ZNSt13random_device14_M_init_pretr1ERKSs",
    "_ZNKSt10moneypunctIwLb0EE11frac_digitsEv",
    "_ZNSt14codecvt_bynameIcc11__mbstate_tEC2ERKSsm",
    "_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_",
    "_ZNSbIwSt11char_traitsIwESaIwEE6rbeginEv",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEE3strERKSs",
    "_ZTv0_n24_NSt13basic_ostreamIwSt11char_traitsIwEED1Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE6setbufEPwl",
    "_ZNKSt14basic_ofstreamIwSt11char_traitsIwEE7is_openEv",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE26_M_destroy_internal_bufferEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE9_M_mutateEmmm",
    "_ZNKSt14basic_ofstreamIwSt11char_traitsIwEE7is_openEv",
    "_ZNSt9__cxx199815_List_node_base7reverseEv",
    "_ZNSt10moneypunctIwLb1EEC1EPSt18__moneypunct_cacheIwLb1EEm",
    "_ZNSt9__cxx199815_List_node_base8transferEPS0_S1_",
    "_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_PKc",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE21_M_extract_via_formatES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tmPKc",
    "_ZNKSs5rfindEPKcm",
    "_ZNSt7codecvtIwc11__mbstate_tEC1EP15__locale_structm",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED2Ev",
    "_ZNKSt20__codecvt_utf16_baseIDsE11do_encodingEv",
    "_ZNSt10istrstreamC1EPc",
    "_ZNKSs9_M_ibeginEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsEPFRSt9basic_iosIwS1_ES5_E",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE24_M_extract_wday_or_monthES3_S3_RiPPKcmRSt8ios_baseRSt12_Ios_Iostate",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPKwS4_EES9_S8_",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb0EEC1ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt7__cxx118messagesIcEC2Em",
    "_ZN11__gnu_debug30_Safe_unordered_container_base7_M_swapERS0_",
    "_ZNSt6localeC1ERKS_",
    "_ZNSt7__cxx118messagesIwEC1Em",
    "_ZNSbIwSt11char_traitsIwESaIwEE6insertEmmw",
    "_ZNSt10istrstreamD2Ev",
    "_ZNSt6localeC2EPNS_5_ImplE",
    "_ZNKSt20__codecvt_utf16_baseIDiE5do_inER11__mbstate_tPKcS4_RS4_PDiS6_RS6_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEaSERKS4_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEpLEPKc",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE11do_groupingEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsEPFRSt8ios_baseS4_E",
    "_ZNSt7__cxx1110moneypunctIcLb0EEC1Em",
    "_ZNSt20bad_array_new_lengthD2Ev",
    "_ZNSt11char_traitsIwE2eqERKwS2_",
    "_ZNSt11char_traitsIwE2eqERKwS2_",
    "_ZNSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNKSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEE5rdbufEv",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE26_M_destroy_internal_bufferEv",
    "_ZNSt18__moneypunct_cacheIcLb0EED0Ev",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC2ERKSsSt13_Ios_Openmode",
    "_ZNSt8ios_base4InitD1Ev",
    "_ZNKSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_RSt8ios_basewPK2tmcc",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC2ERKSsSt13_Ios_Openmode",
    "_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St13_Setprecision",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6appendERKS4_",
    "_ZNKSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE9_M_insertILb0EEES3_S3_RSt8ios_basewRKSbIwS2_SaIwEE",
    "_ZNKSt7__cxx118numpunctIwE9falsenameEv",
    "_ZNSt6thread6_StateD1Ev",
    "_ZGTtNSt12out_of_rangeC2EPKc",
    "_ZNSt8__detail15_List_node_base4swapERS0_S1_",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE10pos_formatEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE5flushEv",
    "_ZNKSt7collateIwE4hashEPKwS2_",
    "_ZNSt8ios_base17_M_call_callbacksENS_5eventE",
    "_ZNSt12ctype_bynameIcEC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZStrsIwSt11char_traitsIwEERSt13basic_istreamIT_T0_ES6_St13_Setprecision",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE22_M_convert_to_externalEPcl",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE14_M_group_floatEPKcmcS6_PcS7_Ri",
    "_ZNSt11logic_errorC2ERKS_",
    "_ZNKSt7collateIcE7compareEPKcS2_S2_S2_",
    "_ZNSolsEPFRSoS_E",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEaSEOS2_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertEN9__gnu_cxx17__normal_iteratorIPKcS4_EEmc",
    "_ZNSt12ctype_bynameIcEC2EPKcm",
    "_ZNSt11__timepunctIcEC2EP15__locale_structPKcm",
    "_ZSt15get_new_handlerv",
    "_ZNSbIwSt11char_traitsIwESaIwEE5clearEv",
    "_ZNKSt9type_info15__is_function_pEv",
    "_ZNSt12strstreambufC1EPhlS0_",
    "_ZNKSt7codecvtIcc11__mbstate_tE16do_always_noconvEv",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE9showmanycEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6assignEmc",
    "_ZNSs13_S_copy_charsEPcPKcS1_",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEE6xsputnEPKwl",
    "_ZNSt20__codecvt_utf16_baseIDiED0Ev",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE11do_get_dateES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNKSt15basic_streambufIcSt11char_traitsIcEE5egptrEv",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE13do_pos_formatEv",
    "_ZNSt17moneypunct_bynameIwLb0EED1Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6assignEOS4_",
    "_ZN10__cxxabiv129__pointer_to_member_type_infoD0Ev",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE13do_pos_formatEv",
    "_ZNSt14overflow_errorC2ERKSs",
    "_ZNKSt10moneypunctIcLb1EE16do_decimal_pointEv",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEE4swapERS2_",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE9underflowEv",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE6narrowEwc",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE4setgEPwS3_S3_",
    "_ZNSt7__cxx1110moneypunctIcLb1EEC2Em",
    "_ZNSirsEPSt15basic_streambufIcSt11char_traitsIcEE",
    "_ZNSt7__cxx1115messages_bynameIcEC1ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE14_M_extract_intIyEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEEC1Ev",
    "_ZNSt7__cxx1115time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE6sentryC1ERS2_b",
    "_ZNKSs4copyEPcmm",
    "_ZNSs13shrink_to_fitEv",
    "_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_St13_Setprecision",
    "_ZTv0_n24_NSt19basic_ostringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEED1Ev",
    "_ZNSi4swapERSi",
    "_ZNSt9type_infoD2Ev",
    "_ZNSt10ostrstreamC1EPciSt13_Ios_Openmode",
    "_ZStplIcSt11char_traitsIcESaIcEENSt7__cxx1112basic_stringIT_T0_T1_EES5_RKS8_",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13find_first_ofEPKcm",
    "_ZNKSt7__cxx118messagesIwE4openERKNS_12basic_stringIcSt11char_traitsIcESaIcEEERKSt6locale",
    "_ZNSt11logic_errorC2ERKSs",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1EOS3_",
    "_ZNKSt8messagesIwE4openERKSsRKSt6localePKc",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEEaSEOS4_",
    "_ZNSt18__moneypunct_cacheIcLb1EED1Ev",
    "_ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEE8overflowEi",
    "_ZNSt8ios_base7_M_initEv",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE13do_date_orderEv",
    "_ZNSt15time_put_bynameIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC1ERKSsm",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE14_M_extract_numES4_S4_RiiimRSt8ios_baseRSt12_Ios_Iostate",
    "_ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE9underflowEv",
    "_ZNKSt20__codecvt_utf16_baseIDsE10do_unshiftER11__mbstate_tPcS3_RS3_",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE4openEPKcSt13_Ios_Openmode",
    "_ZTv0_n24_NSt10ostrstreamD1Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE9_M_mutateEmmPKcm",
    "_ZSt9has_facetISt7codecvtIcc11__mbstate_tEEbRKSt6locale",
    "_ZNSs12_Alloc_hiderC2EPcRKSaIcE",
    "_ZNSt7codecvtIcc11__mbstate_tEC1EP15__locale_structm",
    "_ZNKSt10moneypunctIcLb1EE16do_thousands_sepEv",
    "_ZNSt12length_errorC2ERKSs",
    "_ZSt9use_facetINSt7__cxx1110moneypunctIcLb0EEEERKT_RKSt6locale",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEC2ERKSsSt13_Ios_Openmode",
    "_ZNSbIwSt11char_traitsIwESaIwEEaSEw",
    "_ZNSbIwSt11char_traitsIwESaIwEE6appendERKS2_mm",
    "_ZNSbIwSt11char_traitsIwESaIwEE5eraseEmm",
    "_ZNKSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE3strEv",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDsE6do_outER11__mbstate_tPKDsS4_RS4_PcS6_RS6_",
    "_ZNKSt13basic_fstreamIcSt11char_traitsIcEE5rdbufEv",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE7rdstateEv",
    "_ZNSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC1Em",
    "_ZNKSt3_V214error_category23default_error_conditionEi",
    "_ZNSt10ostrstream6freezeEb",
    "_ZNKSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_bRSt8ios_basecRKSs",
    "_ZNKSt9strstream6pcountEv",
    "_ZNKSt15basic_streambufIcSt11char_traitsIcEE5pbaseEv",
    "_ZNSt12strstreambuf8_M_allocEm",
    "_ZSt16generic_categoryv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7_S_copyEPcPKcm",
    "_ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base",
    "_ZGTtNSt16invalid_argumentC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt10moneypunctIcLb1EEC2EPSt18__moneypunct_cacheIcLb1EEm",
    "_ZGTtNSt11range_errorC1EPKc",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertEN9__gnu_cxx17__normal_iteratorIPwS4_EESt16initializer_listIwE",
    "_ZGTtNSt12domain_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt12ctype_bynameIwEC1ERKSsm",
    "_ZN11__gnu_debug19_Safe_iterator_base16_M_detach_singleEv",
    "_ZNSt10moneypunctIcLb1EE24_M_initialize_moneypunctEP15__locale_structPKc",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb1EEC1EPKcm",
    "_ZNSt8ios_base17register_callbackEPFvNS_5eventERS_iEi",
    "_ZNSoaSEOSo",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE11get_weekdayES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSo5tellpEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE4readEPwl",
    "_ZNSbIwSt11char_traitsIwESaIwEEaSESt16initializer_listIwE",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE15_M_insert_floatIdEES3_S3_RSt8ios_baseccT_",
    "_ZNSt15numpunct_bynameIcEC1EPKcm",
    "_ZNSolsEPFRSt9basic_iosIcSt11char_traitsIcEES3_E",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEEC2ESt13_Ios_Openmode",
    "_ZNKSt10moneypunctIcLb0EE16do_thousands_sepEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEaSEc",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEC1ERKSsSt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE18_M_construct_aux_2Emw",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv",
    "_ZNKSt25__codecvt_utf8_utf16_baseIwE13do_max_lengthEv",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE17find_first_not_ofEPKcm",
    "_ZNKSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_bRSt8ios_baseRSt12_Ios_IostateRSbIwS2_SaIwEE",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE11do_get_timeES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "__atomic_flag_for_address",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2EOS2_",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4dataEv",
    "_ZN11__gnu_debug19_Safe_sequence_base13_M_detach_allEv",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNKSt20__codecvt_utf16_baseIDiE10do_unshiftER11__mbstate_tPcS3_RS3_",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEED0Ev",
    "_ZNKSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basecPK2tmcc",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEEaSEOS3_",
    "_ZStlsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_St12_Setiosflags",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strERKSs",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractIdEERS2_RT_",
    "_ZNSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNKSt14error_category10equivalentEiRKSt15error_condition",
    "_ZNSt8numpunctIwED2Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_mRKS1_",
    "_ZNKSt9type_info10__do_catchEPKS_PPvj",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEE4openERKNSt7__cxx1112basic_stringIcS0_IcESaIcEEESt13_Ios_Openmode",
    "_ZNSt17bad_function_callD2Ev",
    "_ZNSs7replaceEmmRKSsmm",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE16do_negative_signEv",
    "_ZNSt7__cxx118numpunctIcE22_M_initialize_numpunctEP15__locale_struct",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_disposeERKS1_",
    "_ZNSt19__codecvt_utf8_baseIDiED2Ev",
    "_ZNSt7__cxx1110moneypunctIcLb1EEC1EPSt18__moneypunct_cacheIcLb1EEm",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEE4fileEv",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5frontEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1Ev",
    "_ZNSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZNSo6sentryD2Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsEPSt15basic_streambufIwS1_E",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_mmRKS1_",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEED0Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12_M_constructIN9__gnu_cxx17__normal_iteratorIPKwS4_EEEEvT_SB_St20forward_iterator_tag",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE9_M_appendEPKwm",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb1EEC2ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE17_M_stringbuf_initESt13_Ios_Openmode",
    "_ZNKSt11__timepunctIwE21_M_months_abbreviatedEPPKw",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE10date_orderEv",
    "_ZNKSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE10_M_extractILb1EEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRSs",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC1EPKcSt13_Ios_Openmode",
    "_ZNSsC1ESt16initializer_listIcERKSaIcE",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSt15messages_bynameIcEC2EPKcm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1ERKS4_mRKS3_",
    "_ZNSt14collate_bynameIwED0Ev",
    "_ZNSs6insertEN9__gnu_cxx17__normal_iteratorIPcSsEEc",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE15_M_check_lengthEmmPKc",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE3tieEv",
    "_ZNSt12strstreambufC2EPalS0_",
    "_ZNSt6thread15_M_start_threadESt10shared_ptrINS_10_Impl_baseEEPFvvE",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE8get_yearES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSi10_M_extractIlEERSiRT_",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEC1ERKNSt7__cxx1112basic_stringIcS0_IcESaIcEEESt13_Ios_Openmode",
    "_ZNSt5ctypeIwE19_M_initialize_ctypeEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE7getlineEPwlw",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNSt11__timepunctIcED2Ev",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC2EOS3_",
    "_ZNKSs15_M_check_lengthEmmPKc",
    "_ZNKSs15_M_check_lengthEmmPKc",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE10pubseekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNKSt10moneypunctIwLb1EE11curr_symbolEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5eraseEN9__gnu_cxx17__normal_iteratorIPwS4_EES8_",
    "_ZNSt11__timepunctIwED1Ev",
    "_ZNKSt15basic_streambufIcSt11char_traitsIcEE5epptrEv",
    "_ZNSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC2Em",
    "__gxx_personality_v0",
    "_ZNSbIwSt11char_traitsIwESaIwEE5eraseEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_",
    "_ZNSaIwED2Ev",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE24_M_extract_wday_or_monthES4_S4_RiPPKwmRSt8ios_baseRSt12_Ios_Iostate",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode",
    "_ZStrsIdwSt11char_traitsIwEERSt13basic_istreamIT0_T1_ES6_RSt7complexIT_E",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEEC1EOS4_",
    "_ZNSoC2EOSo",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEC1Ev",
    "_ZNSi10_M_extractImEERSiRT_",
    "_ZNSbIwSt11char_traitsIwESaIwEED2Ev",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE13get_monthnameES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt7__cxx1115time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC1ERKNS_12basic_stringIcS2_IcESaIcEEEm",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEEC1ESt13_Ios_Openmode",
    "_ZNSt10istrstreamC2EPKc",
    "_ZSt9has_facetINSt7__cxx1110moneypunctIcLb0EEEEbRKSt6locale",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE8overflowEi",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC1ERKNSt7__cxx1112basic_stringIcS0_IcESaIcEEESt13_Ios_Openmode",
    "_ZNSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC2Em",
    "_ZTv0_n24_NSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZGTtNSt15underflow_errorD0Ev",
    "_ZNSt7codecvtIDic11__mbstate_tED0Ev",
    "_ZTv0_n24_NSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE8get_dateES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt11__timepunctIcEC1EPSt17__timepunct_cacheIcEm",
    "_ZNKSt5ctypeIwE8do_widenEPKcS2_Pw",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEEC2Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEEC1Ev",
    "_ZNKSs17find_first_not_ofEcm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4rendEv",
    "_ZNSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC1Em",
    "_ZNKSs5c_strEv",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDsE16do_always_noconvEv",
    "_ZNSt15underflow_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt6locale5_Impl16_M_install_cacheEPKNS_5facetEm",
    "_ZNSbIwSt11char_traitsIwESaIwEE7_M_leakEv",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE10exceptionsESt12_Ios_Iostate",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7compareERKS4_",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EOS2_",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEEC1Ev",
    "_ZNKSt7__cxx118numpunctIcE8groupingEv",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEEaSEOS3_",
    "_ZGTtNSt12out_of_rangeD1Ev",
    "_ZTv0_n24_NSt14basic_ofstreamIwSt11char_traitsIwEED0Ev",
    "_ZNKSt8messagesIcE7do_openERKSsRKSt6locale",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEEC2EOS4_",
    "_ZNSsC2ESt16initializer_listIcERKSaIcE",
    "_ZNSt12strstreambufC1EPKhl",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4sizeEv",
    "_ZNSt7__cxx118messagesIcED1Ev",
    "_ZNKSt20__codecvt_utf16_baseIDsE6do_outER11__mbstate_tPKDsS4_RS4_PcS6_RS6_",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ERKSsSt13_Ios_Openmode",
    "_ZNKSbIwSt11char_traitsIwESaIwEE6cbeginEv",
    "_ZNSt7__cxx118messagesIwED0Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13_M_local_dataEv",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1ERKSsSt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPKwS4_EES9_mw",
    "_ZNSt8numpunctIwEC2EPSt16__numpunct_cacheIwEm",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4rendEv",
    "_ZN9__gnu_cxx17__pool_alloc_base9_M_refillEm",
    "_ZNSt18__moneypunct_cacheIcLb0EED2Ev",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE9_M_insertIxEERS2_T_",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE16do_get_monthnameES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt7__cxx1110moneypunctIcLb0EED0Ev",
    "_ZNSt6locale5facet15_S_get_c_localeEv",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC2ERKSsSt13_Ios_Openmode",
    "_ZNSt6locale5_Impl19_M_replace_categoryEPKS0_PKPKNS_2idE",
    "_ZNSo3putEc",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14do_get_weekdayES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZN10__cxxabiv116__enum_type_infoD0Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEEpLERKS2_",
    "_ZSt9has_facetISt7collateIwEEbRKSt6locale",
    "_ZGTtNSt11range_errorD0Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4sizeEv",
    "_ZNKSt7codecvtIwc11__mbstate_tE5do_inERS0_PKcS4_RS4_PwS6_RS6_",
    "_ZGTtNSt11logic_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKcl",
    "_ZNSt7__cxx1115messages_bynameIwEC1EPKcm",
    "_ZNKSt12strstreambuf6pcountEv",
    "_ZNSs9_M_mutateEmmm",
    "_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_RKSs",
    "_ZNKSt8messagesIwE3getEiiiRKSbIwSt11char_traitsIwESaIwEE",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE4swapERS2_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEED2Ev",
    "_ZNSt6locale5facet13_S_get_c_nameEv",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEEC2EOS4_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE9_M_insertIyEERS2_T_",
    "_ZNKSt10moneypunctIcLb0EE16do_decimal_pointEv",
    "_ZSt17__copy_streambufsIcSt11char_traitsIcEElPSt15basic_streambufIT_T0_ES6_",
    "_ZNKSbIwSt11char_traitsIwESaIwEEixEm",
    "_ZN11__gnu_debug19_Safe_sequence_base7_M_swapERS0_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7_S_moveEPwPKwm",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE15_M_create_pbackEv",
    "_ZSt14__convert_to_vIdEvPKcRT_RSt12_Ios_IostateRKP15__locale_struct",
    "_ZNKSbIwSt11char_traitsIwESaIwEE7compareERKS2_",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE11do_groupingEv",
    "_ZN9__gnu_cxx6__poolILb0EE16_M_reserve_blockEmm",
    "_ZNSt15time_put_bynameIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNSt20__codecvt_utf16_baseIDiED2Ev",
    "_ZSt11_Hash_bytesPKvmm",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEEC2EOS3_",
    "_ZNSt5ctypeIcEC1EPKtbm",
    "_ZGTtNSt14overflow_errorC1EPKc",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSt7__cxx1115time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC2EPKcm",
    "_ZN10__cxxabiv129__pointer_to_member_type_infoD2Ev",
    "_ZSt9has_facetISt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEEbRKSt6locale",
    "_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_mc",
    "_ZNKSbIwSt11char_traitsIwESaIwEE13find_first_ofEPKwm",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDiE5do_inER11__mbstate_tPKcS4_RS4_PDiS6_RS6_",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb0EEC1ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE17_M_stringbuf_initESt13_Ios_Openmode",
    "_ZNSt6localeC2ERKS_S1_i",
    "_ZNSt15_List_node_base8transferEPS_S0_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEED0Ev",
    "_ZNSt8__detail15_List_node_base11_M_transferEPS0_S1_",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEEC1EOS3_",
    "_ZStlsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_c",
    "_ZNSt15time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC1ERKSsm",
    "_ZNSt7__cxx1110moneypunctIcLb0EEC1EP15__locale_structPKcm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE5rfindEwm",
    "_ZThn16_NSt14basic_iostreamIwSt11char_traitsIwEED0Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPKcS4_EES9_S8_S8_",
    "_ZNK11__gnu_debug19_Safe_iterator_base11_M_singularEv",
    "_ZNSt6__norm15_List_node_base8transferEPS0_S1_",
    "_ZNSt6locale5facetD0Ev",
    "_ZNSt13__future_base12_Result_baseC1Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE6insertEmPKwm",
    "_ZNKSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEE3strEv",
    "_ZNSt7__cxx1110moneypunctIcLb1EED1Ev",
    "_ZNSt7__cxx1115numpunct_bynameIwEC1ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt15numpunct_bynameIcEC2ERKSsm",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE5rdbufEv",
    "_ZNSt10moneypunctIcLb1EEC1EPSt18__moneypunct_cacheIcLb1EEm",
    "_ZNSt7__cxx1114collate_bynameIcEC1EPKcm",
    "_ZNSs12_S_empty_repEv",
    "_ZSt9has_facetISt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEEbRKSt6locale",
    "_ZNSt11__timepunctIwEC1EPSt17__timepunct_cacheIwEm",
    "_ZNSt14overflow_errorD1Ev",
    "_ZNKSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES4_bRSt8ios_basewe",
    "_ZNSi10_M_extractIjEERSiRT_",
    "_ZTv0_n24_NSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNKSt10moneypunctIwLb0EE13positive_signEv",
    "_ZNSt15__exception_ptr13exception_ptrC2EMS0_FvvE",
    "_ZThn16_NSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNSt12strstreambufC2El",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4backEv",
    "_ZNSirsEPFRSt9basic_iosIcSt11char_traitsIcEES3_E",
    "_ZNKSt6locale4nameB5cxx11Ev",
    "_ZNSt8ios_base7_M_swapERS_",
    "__once_proxy",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEEC1ERKNS_12basic_stringIwS2_S3_EESt13_Ios_Openmode",
    "_ZNSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNSt8ios_base7failureD0Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4findEwm",
    "_ZStrsIfcSt11char_traitsIcEERSt13basic_istreamIT0_T1_ES6_RSt7complexIT_E",
    "_ZNSt11range_errorC2EPKc",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE4swapERS2_",
    "_ZNKSt7__cxx118messagesIcE18_M_convert_to_charERKNS_12basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSs5beginEv",
    "_ZSt9use_facetINSt7__cxx117collateIwEEERKT_RKSt6locale",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEEaSEOS4_",
    "_ZNSt14collate_bynameIcED1Ev",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC2Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE6ignoreEl",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE6ignoreEl",
    "_ZNSt11range_errorD0Ev",
    "_ZNSt7__cxx118numpunctIwEC1Em",
    "_ZNKSt20__codecvt_utf16_baseIwE11do_encodingEv",
    "_ZNSsaSERKSs",
    "_ZNKSt5ctypeIcE13_M_widen_initEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2IPcvEET_S7_RKS3_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertEN9__gnu_cxx17__normal_iteratorIPcS4_EEc",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertEN9__gnu_cxx17__normal_iteratorIPwS4_EEw",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEED2Ev",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEED2Ev",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE16do_negative_signEv",
    "_ZGTtNSt16invalid_argumentD1Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE17find_first_not_ofEPKwmm",
    "_ZNKSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE10_M_extractILb1EEES4_S4_S4_RSt8ios_baseRSt12_Ios_IostateRNS_12basic_stringIcS2_IcESaIcEEE",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE6ignoreEv",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13_M_local_dataEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE6ignoreEv",
    "_ZNSs7_M_leakEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6appendERKS4_mm",
    "_ZThn16_NSt13basic_fstreamIcSt11char_traitsIcEED1Ev",
    "_ZSt9has_facetINSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEEEbRKSt6locale",
    "__cxa_vec_dtor",
    "_ZNSi6sentryC2ERSib",
    "_ZNKSt7codecvtIcc11__mbstate_tE13do_max_lengthEv",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEEC2ERKNS_12basic_stringIcS2_S3_EESt13_Ios_Openmode",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE8max_sizeEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2EPKcmRKS3_",
    "_ZNKSt15basic_streambufIwSt11char_traitsIwEE5egptrEv",
    "_ZNSt12future_errorD1Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6lengthEv",
    "_ZNKSs4_Rep12_M_is_sharedEv",
    "_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEEC2ERKNS_12basic_stringIcS2_S3_EESt13_Ios_Openmode",
    "_ZNSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC2Em",
    "_ZSt9has_facetISt10moneypunctIcLb0EEEbRKSt6locale",
    "_ZNSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZNSt7__cxx1115time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC2ERKNS_12basic_stringIcS2_IcESaIcEEEm",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEEC2EOS2_",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEE4openERKSsSt13_Ios_Openmode",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE13do_date_orderEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6assignEPKcm",
    "_ZNSt17moneypunct_bynameIwLb1EEC2ERKSsm",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE9pbackfailEi",
    "_ZNKSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE9_M_insertILb0EEES3_S3_RSt8ios_basecRKSs",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12_Alloc_hiderC1EPcRKS3_",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEEaSEOS2_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1ERKS3_",
    "_ZNSt14collate_bynameIwED2Ev",
    "_ZNKSt10moneypunctIcLb0EE11do_groupingEv",
    "_ZNSirsERPv",
    "_ZNKSt19__codecvt_utf8_baseIwE16do_always_noconvEv",
    "_ZStplIwSt11char_traitsIwESaIwEESbIT_T0_T1_ERKS6_S8_",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEE4swapERS3_",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE14do_get_weekdayES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEE4fileEv",
    "_ZNKSt10moneypunctIcLb1EE10neg_formatEv",
    "_ZNSsC1EOSs",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE8_M_checkEmPKc",
    "__cxa_throw_bad_array_new_length",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE8pubimbueERKSt6locale",
    "_ZNKSbIwSt11char_traitsIwESaIwEE13find_first_ofERKS2_m",
    "_ZNSt5ctypeIwEC2EP15__locale_structm",
    "_ZNSbIwSt11char_traitsIwESaIwEEpLESt16initializer_listIwE",
    "_ZNSbIwSt11char_traitsIwESaIwEE6insertEmRKS2_",
    "_ZNKSt9type_info11__do_upcastEPKN10__cxxabiv117__class_type_infoEPPv",
    "_ZNKSt10moneypunctIwLb1EE11frac_digitsEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12_Alloc_hiderC2EPcRKS3_",
    "_ZNKSt13basic_fstreamIwSt11char_traitsIwEE5rdbufEv",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE11get_weekdayES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNKSt7__cxx117collateIcE7compareEPKcS3_S3_S3_",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE3strERKSbIwS1_S2_E",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEE4swapERS4_",
    "_ZNSiC2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1IPcvEET_S7_RKS3_",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEED0Ev",
    "_ZNSo9_M_insertIlEERSoT_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendEPKc",
    "_ZNKSt15basic_streambufIwSt11char_traitsIwEE5pbaseEv",
    "_ZNSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEE5uflowEv",
    "_ZNSt12length_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt12domain_errorC1ERKSs",
    "_ZNKSt10moneypunctIcLb0EE8groupingEv",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE13negative_signEv",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE2atEm",
    "_ZNKSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES4_S4_bRSt8ios_baseRSt12_Ios_IostateRNS_12basic_stringIwS3_SaIwEEE",
    "_ZNSt7__cxx1115time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC2EPKcm",
    "_ZSt9use_facetISt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEERKT_RKSt6locale",
    "_ZNKSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES4_bRSt8ios_basecRKNS_12basic_stringIcS3_SaIcEEE",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEmmRKS4_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1ERKS4_mRKS3_",
    "_ZGTtNSt15underflow_errorD2Ev",
    "_ZNSt7codecvtIDic11__mbstate_tED2Ev",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEED1Ev",
    "_ZNSo6sentryC2ERSo",
    "_ZNSt15messages_bynameIcEC2ERKSsm",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEED0Ev",
    "_ZNSt7__cxx1110moneypunctIcLb1EEC2EP15__locale_structPKcm",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERPv",
    "_ZNSbIwSt11char_traitsIwESaIwEE5beginEv",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEED0Ev",
    "_ZNSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNSt10moneypunctIwLb1EEC1Em",
    "_ZNKSt7__cxx118numpunctIcE8truenameEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE9push_backEc",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE15_M_insert_floatIeEES3_S3_RSt8ios_basewcT_",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE5closeEv",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE8get_yearES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1ERKS4_",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE11do_get_timeES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSbIwSt11char_traitsIwESaIwEE7_M_moveEPwPKwm",
    "_ZNSbIwSt11char_traitsIwESaIwEE7_M_moveEPwPKwm",
    "_ZNSo5writeEPKcl",
    "_ZNSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNKSt19basic_istringstreamIcSt11char_traitsIcESaIcEE3strEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPcS4_EES8_S8_S8_",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEE7is_openEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE5sgetnEPcl",
    "_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_PKcm",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ERKSsSt13_Ios_Openmode",
    "_ZNSt17moneypunct_bynameIcLb0EEC1ERKSsm",
    "_ZNSt7__cxx118messagesIwED2Ev",
    "__cxa_guard_abort",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE17find_first_not_ofEwm",
    "_ZNSt10_Sp_lockerD1Ev",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEaSEOS2_",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE4swapERS4_",
    "_ZGTtNKSt11logic_error4whatEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2ERKS4_mmRKS3_",
    "_ZNSt18condition_variableC2Ev",
    "_ZNKSt10moneypunctIcLb0EE10neg_formatEv",
    "_ZNSt7__cxx1110moneypunctIcLb0EED2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6assignEPKc",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE5clearESt12_Ios_Iostate",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE8groupingEv",
    "_ZNKSs4rendEv",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE7_M_syncEPwmm",
    "_ZNSdC1EOSd",
    "_ZNSt13runtime_errorC2ERKS_",
    "_ZStrsISt11char_traitsIcEERSt13basic_istreamIcT_ES5_Pa",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6substrEmm",
    "_ZGTtNSt13runtime_errorC1EPKc",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13shrink_to_fitEv",
    "_ZN10__cxxabiv116__enum_type_infoD2Ev",
    "_ZSt17__copy_streambufsIwSt11char_traitsIwEElPSt15basic_streambufIT_T0_ES6_",
    "_ZGTtNSt11range_errorD2Ev",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE9showmanycEv",
    "_ZGTtNSt13runtime_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES4_S4_bRSt8ios_baseRSt12_Ios_IostateRNS_12basic_stringIcS3_SaIcEEE",
    "_ZN11__gnu_debug19_Safe_iterator_base12_M_get_mutexEv",
    "__cxa_get_globals",
    "_ZNSt9__cxx199815_List_node_base9_M_unhookEv",
    "_ZStrsISt11char_traitsIcEERSt13basic_istreamIcT_ES5_Ph",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEEC1ESt13_Ios_Openmode",
    "_ZNKSt10moneypunctIcLb0EE13positive_signEv",
    "_ZNKSs4sizeEv",
    "_ZNKSt4hashISt10error_codeEclES0_",
    "_ZNSs6assignESt16initializer_listIcE",
    "_ZNSt16invalid_argumentC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt19__codecvt_utf8_baseIwE9do_lengthER11__mbstate_tPKcS4_m",
    "_ZNSt6locale7classicEv",
    "_ZN10__cxxabiv119__pointer_type_infoD1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEaSEw",
    "_ZNKSt10moneypunctIwLb0EE8groupingEv",
    "_ZNSt17moneypunct_bynameIcLb0EED0Ev",
    "_ZNSt17moneypunct_bynameIwLb0EEC1EPKcm",
    "_ZNSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC2Em",
    "_ZNKSt8messagesIcE20_M_convert_from_charEPc",
    "_ZNKSt7__cxx117collateIwE12_M_transformEPwPKwm",
    "_ZNSt9basic_iosIcSt11char_traitsIcEEC1Ev",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEED2Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE5sgetcEv",
    "_ZNKSs3endEv",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE5imbueERKSt6locale",
    "_ZNKSt15basic_streambufIwSt11char_traitsIwEE5epptrEv",
    "_ZNSt9strstreamC1Ev",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEE4swapERS4_",
    "_ZSt14get_unexpectedv",
    "_ZNSs15_M_replace_safeEmmPKcm",
    "_ZNSt13runtime_errorC2ERKSs",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2ESt16initializer_listIwERKS1_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEED2Ev",
    "_ZNKSt8messagesIcE3getEiiiRKSs",
    "_ZSt14set_unexpectedPFvvE",
    "_ZNSt12__basic_fileIcED2Ev",
    "_ZNSt13__future_base12_Result_baseD0Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE9underflowEv",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEEC2ESt13_Ios_Openmode",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEEC2ESt13_Ios_Openmode",
    "_ZNKSt7collateIcE10_M_compareEPKcS2_",
    "_ZNKSs7compareEPKc",
    "_ZNSt6locale5facetD2Ev",
    "_ZNSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC1Em",
    "_ZNSt7__cxx118numpunctIcEC2Em",
    "_ZNKSt19__codecvt_utf8_baseIDsE10do_unshiftER11__mbstate_tPcS3_RS3_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6resizeEm",
    "_ZNSbIwSt11char_traitsIwESaIwEE6assignESt16initializer_listIwE",
    "_ZNKSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_bRSt8ios_basewe",
    "_ZNSs6assignERKSs",
    "_ZNSt8numpunctIcEC1EPSt16__numpunct_cacheIcEm",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE9pubsetbufEPwl",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDsE13do_max_lengthEv",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEE3strERKNS_12basic_stringIcS2_S3_EE",
    "_ZNKSt20__codecvt_utf16_baseIDsE9do_lengthER11__mbstate_tPKcS4_m",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE5tellpEv",
    "_ZNSt13__future_base19_Async_state_commonD1Ev",
    "_ZN10__cxxabiv117__class_type_infoD0Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE8_M_eraseEmm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEmmRKS4_mm",
    "_ZNKSt7__cxx118numpunctIcE9falsenameEv",
    "_ZNKSs11_M_disjunctEPKc",
    "_ZNKSs11_M_disjunctEPKc",
    "_ZNSi3getERSt15basic_streambufIcSt11char_traitsIcEEc",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12_Alloc_hiderC2EPwOS3_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS4_EES8_S8_S8_",
    "_ZN9__gnu_cxx6__poolILb1EE13_M_initializeEPFvPvE",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6rbeginEv",
    "_ZSt19__throw_regex_errorNSt15regex_constants10error_typeE",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_RSt8ios_basewPKv",
    "_ZNSt12out_of_rangeC1EPKc",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE3badEv",
    "_ZNSt17moneypunct_bynameIcLb1EED1Ev",
    "_ZNSt17__timepunct_cacheIcEC2Em",
    "_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE2atEm",
    "_ZNKSt5ctypeIcE10do_tolowerEc",
    "_ZNKSs5rfindEPKcmm",
    "_ZSt9has_facetISt7collateIcEEbRKSt6locale",
    "_ZNSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "__cxa_pure_virtual",
    "_ZNSt17__timepunct_cacheIwEC1Em",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6rbeginEv",
    "_ZNSt8ios_base7failureD2Ev",
    "__cxa_vec_delete",
    "_ZNSt12strstreambufD1Ev",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC2EOS2_",
    "_ZNSt6locale5_Impl16_M_install_facetEPKNS_2idEPKNS_5facetE",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13_S_copy_charsEPwS5_S5_",
    "_ZNKSt6locale4nameEv",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode",
    "_ZSt9has_facetINSt7__cxx118messagesIwEEEbRKSt6locale",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE4swapERS3_",
    "_ZNSt10moneypunctIcLb0EEC2EP15__locale_structPKcm",
    "_ZSt9use_facetINSt7__cxx118numpunctIwEEERKT_RKSt6locale",
    "_ZNSi10_M_extractIeEERSiRT_",
    "_ZNSt12__basic_fileIcE2fdEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7reserveEm",
    "_ZSt17__verify_groupingPKcmRKSs",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEED1Ev",
    "_ZNKSt10moneypunctIwLb1EE16do_decimal_pointEv",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC2ESt13_Ios_Openmode",
    "_ZNKSt19__codecvt_utf8_baseIDiE10do_unshiftER11__mbstate_tPcS3_RS3_",
    "_ZNSt11range_errorD2Ev",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE6setbufEPwl",
    "_ZNSt6__norm15_List_node_base9_M_unhookEv",
    "_ZSt9has_facetISt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEEbRKSt6locale",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEC1Ev",
    "_ZNSi10_M_extractIfEERSiRT_",
    "_ZNSt7__cxx118numpunctIwED0Ev",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE4moveERS2_",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE5imbueERKSt6locale",
    "_ZNSt6__norm15_List_node_base6unhookEv",
    "_ZNSt12ctype_bynameIwEC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1ESt16initializer_listIwERKS3_",
    "_ZStrsISt11char_traitsIcEERSt13basic_istreamIcT_ES5_Ra",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4findERKS4_m",
    "_ZNKSt8numpunctIwE9falsenameEv",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE12__safe_gbumpEl",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE13_M_insert_intIxEES3_S3_RSt8ios_basecT_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6resizeEmw",
    "_ZStrsISt11char_traitsIcEERSt13basic_istreamIcT_ES5_Rh",
    "_ZNSoC1Ev",
    "_ZNKSt10moneypunctIwLb1EE16do_thousands_sepEv",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE13negative_signEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEPFRS2_S3_E",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEmmRKS2_mm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEED2Ev",
    "_ZTv0_n24_NSt10istrstreamD0Ev",
    "_ZNSt14overflow_errorC2EPKc",
    "_ZNSo9_M_insertIbEERSoT_",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb0EED0Ev",
    "_ZNSsC2IPKcEET_S2_RKSaIcE",
    "_ZNSt12ctype_bynameIcEC2ERKSsm",
    "_ZNSsC1EmcRKSaIcE",
    "_ZNKSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES4_bRSt8ios_basewe",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1ERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
    "_ZNSt6locale5_Impl16_M_replace_facetEPKS0_PKNS_2idE",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7_M_dataEv",
    "_ZNSt16__numpunct_cacheIcEC2Em",
    "_ZNSt13random_device16_M_getval_pretr1Ev",
    "_ZNKSs5frontEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_S6_S6_",
    "_ZNSt12strstreambufC2EPKal",
    "_ZNSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE21_M_extract_via_formatES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tmPKw",
    "_ZNSt7__cxx1115time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNSt10moneypunctIwLb0EEC2Em",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1ERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
    "_ZNSt16__numpunct_cacheIwEC1Em",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE8overflowEj",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEE4openEPKcSt13_Ios_Openmode",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE8capacityEv",
    "_ZTv0_n24_NSiD0Ev",
    "_ZNSs5eraseEN9__gnu_cxx17__normal_iteratorIPcSsEE",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE15_M_extract_nameES3_S3_RiPPKcmRSt8ios_baseRSt12_Ios_Iostate",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE5rdbufEv",
    "_ZN10__gnu_norm15_List_node_base7reverseEv",
    "_ZNSt18__moneypunct_cacheIcLb1EE8_M_cacheERKSt6locale",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEE4swapERS3_",
    "_ZSt17rethrow_exceptionNSt15__exception_ptr13exception_ptrE",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb1EEC2ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSbIwSt11char_traitsIwESaIwEE6insertEmRKS2_mm",
    "_ZNSt8ios_baseC1Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1EPKcRKS3_",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEEaSEOS2_",
    "_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC2Em",
    "_ZdlPv",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13find_first_ofEwm",
    "_ZNSirsEPFRSiS_E",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14_M_extract_intItEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNSiD1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE9_S_assignEPwmw",
    "_ZNSt12strstreambufC1EPclS0_",
    "_ZNKSs5rfindEcm",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEED2Ev",
    "_ZNKSt7__cxx118messagesIcE8do_closeEi",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE9pbackfailEj",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEEC1EOS4_",
    "_ZThn16_NSdD0Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEaSEOS4_",
    "_ZNSt14collate_bynameIwEC2EPKcm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1EPKwRKS3_",
    "_ZNSs4swapERSs",
    "_ZNKSt3_V214error_category10equivalentERKSt10error_codei",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE12_M_group_intEPKcmwRSt8ios_basePwS9_Ri",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE16do_negative_signEv",
    "_ZNSt7__cxx1114collate_bynameIcEC1ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZThn16_NSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE8get_dateES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEED2Ev",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb1EED1Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE6stosscEv",
    "_ZNSt12__basic_fileIcE8xsputn_2EPKclS2_l",
    "__cxa_vec_new2",
    "_ZNSt6localeC2Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6assignEPKwm",
    "_ZNSt11range_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "__cxa_vec_new3",
    "_ZNKSt10moneypunctIwLb0EE14do_curr_symbolEv",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE8_M_checkEmPKc",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE8overflowEi",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEED2Ev",
    "_ZNK10__cxxabiv119__pointer_type_info14__is_pointer_pEv",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb0EEC1EPKcm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5eraseEmm",
    "_ZNKSt19__codecvt_utf8_baseIwE10do_unshiftER11__mbstate_tPcS3_RS3_",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEaSEOS2_",
    "_ZNSt6locale21_S_normalize_categoryEi",
    "_ZNSt12ctype_bynameIwEC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZN10__cxxabiv121__vmi_class_type_infoD0Ev",
    "__cxa_begin_catch",
    "_ZNK10__cxxabiv120__function_type_info15__is_function_pEv",
    "_ZNSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_St5_Setw",
    "_ZSt17current_exceptionv",
    "_ZNSt16bad_array_lengthD1Ev",
    "_ZStlsIwSt11char_traitsIwESaIwEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E",
    "_ZNSt10moneypunctIwLb1EED0Ev",
    "_ZNSt14codecvt_bynameIwc11__mbstate_tEC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertEmPKw",
    "_ZNSt8ios_base20_M_dispose_callbacksEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE16_M_get_allocatorEv",
    "_ZGTtNSt14overflow_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSsaSESt16initializer_listIcE",
    "_ZNSt7__cxx1114collate_bynameIcED0Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13_S_copy_charsEPwN9__gnu_cxx17__normal_iteratorIPKwS4_EESA_",
    "_ZNSt18condition_variableD1Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13get_allocatorEv",
    "_ZNKSs8_M_limitEmm",
    "_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC1Em",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE5seekgElSt12_Ios_Seekdir",
    "_ZNSt7__cxx1110moneypunctIwLb1EEC1EP15__locale_structPKcm",
    "_ZStlsIdwSt11char_traitsIwEERSt13basic_ostreamIT0_T1_ES6_RKSt7complexIT_E",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE16find_last_not_ofEPKcm",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE9underflowEv",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEEC2Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractIbEERS2_RT_",
    "_ZNSt13runtime_errorD0Ev",
    "_ZSt20__throw_length_errorPKc",
    "_ZNSt15__exception_ptr13exception_ptraSERKS0_",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7compareEPKw",
    "_ZNKSs16find_last_not_ofEPKcm",
    "_ZNKSt10moneypunctIcLb1EE10pos_formatEv",
    "_ZNKSt19__codecvt_utf8_baseIDsE16do_always_noconvEv",
    "_ZGTtNSt11logic_errorC1EPKc",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7_M_dataEPw",
    "_ZNSt17moneypunct_bynameIcLb1EEC2EPKcm",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tmcc",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE13_M_insert_intIyEES3_S3_RSt8ios_basewT_",
    "_ZSt9has_facetISt7codecvtIwc11__mbstate_tEEbRKSt6locale",
    "_ZNSt13random_device9_M_getvalEv",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEE4swapERS3_",
    "_ZNSt6locale11_M_coalesceERKS_S1_i",
    "_ZTv0_n24_NSt14basic_ifstreamIcSt11char_traitsIcEED0Ev",
    "_ZNSs6assignEPKcm",
    "_ZNSi10_M_extractIdEERSiRT_",
    "_ZNSiC2EOSi",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE16do_get_monthnameES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt17moneypunct_bynameIcLb0EED2Ev",
    "_ZNSt9basic_iosIcSt11char_traitsIcEED0Ev",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE8groupingEv",
    "_ZNSt12strstreambufC2EPKcl",
    "_ZNSsC2ERKSsmRKSaIcE",
    "_ZNSt8messagesIcEC1Em",
    "_ZSt20__throw_domain_errorPKc",
    "_ZGTtNSt15underflow_errorC1EPKc",
    "_ZStlsIdcSt11char_traitsIcEERSt13basic_ostreamIT0_T1_ES6_RKSt7complexIT_E",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEE3strERKSbIwS1_S2_E",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEE7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNSt9strstreamD0Ev",
    "_ZNKSt10istrstream5rdbufEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE5seekpElSt12_Ios_Seekdir",
    "_ZNSt8bad_castD0Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractIPvEERS2_RT_",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7crbeginEv",
    "_ZNKSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES4_S4_bRSt8ios_baseRSt12_Ios_IostateRe",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4swapERS4_",
    "_ZNSt19istreambuf_iteratorIwSt11char_traitsIwEEppEv",
    "_ZNKSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEE5rdbufEv",
    "_ZNSt19istreambuf_iteratorIwSt11char_traitsIwEEppEv",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE11get_weekdayES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt6localeC2ERKS_PKci",
    "_ZNSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZGTtNKSt13bad_exception4whatEv",
    "_ZNSt15time_put_bynameIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC2ERKSsm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2EOS4_RKS3_",
    "_ZNKSt8messagesIwE7do_openERKSsRKSt6locale",
    "_ZNSo5seekpElSt12_Ios_Seekdir",
    "_ZNSt13__future_base12_Result_baseD2Ev",
    "_ZSt9has_facetISt8numpunctIwEEbRKSt6locale",
    "_ZSt7getlineIwSt11char_traitsIwESaIwEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EES4_",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEE5uflowEv",
    "_ZNSsC1EPKcmRKSaIcE",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE11do_get_timeES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt12__basic_fileIcE4syncEv",
    "_ZGTtNSt12length_errorD0Ev",
    "_ZNKSt5ctypeIcE10do_toupperEc",
    "_ZNSbIwSt11char_traitsIwESaIwEE12_S_constructIN9__gnu_cxx17__normal_iteratorIPwS2_EEEES6_T_S8_RKS1_St20forward_iterator_tag",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEmmRKS2_",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZSt9use_facetISt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEERKT_RKSt6locale",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEEaSEOS2_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE3putEw",
    "__cxa_free_dependent_exception",
    "_ZNKSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_bRSt8ios_baseRSt12_Ios_IostateRe",
    "_ZNSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNSt7__cxx118numpunctIcED1Ev",
    "_ZNK10__cxxabiv121__vmi_class_type_info20__do_find_public_srcElPKvPKNS_17__class_type_infoES2_",
    "_ZNKSt7codecvtIDic11__mbstate_tE13do_max_lengthEv",
    "_ZN10__cxxabiv117__class_type_infoD2Ev",
    "_ZSt9use_facetINSt7__cxx117collateIcEEERKT_RKSt6locale",
    "_ZStplIwSt11char_traitsIwESaIwEENSt7__cxx1112basic_stringIT_T0_T1_EEPKS5_RKS8_",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE7rdstateEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertEN9__gnu_cxx17__normal_iteratorIPKwS4_EEmw",
    "_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St8_SetfillIS3_E",
    "_ZNSbIwSt11char_traitsIwESaIwEE6assignEOS2_",
    "_ZNSt6localeC2ERKS_",
    "_ZGTtNSt14overflow_errorD1Ev",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEEC1EOS3_",
    "_ZNSt14codecvt_bynameIwc11__mbstate_tEC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEEaSEOS4_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertEN9__gnu_cxx17__normal_iteratorIPcS4_EESt16initializer_listIcE",
    "_ZNKSt7codecvtIDic11__mbstate_tE11do_encodingEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE4swapERS2_",
    "_ZNKSt20__codecvt_utf16_baseIDiE9do_lengthER11__mbstate_tPKcS4_m",
    "_ZNKSt13basic_ostreamIwSt11char_traitsIwEE6sentrycvbEv",
    "_ZNSt7__cxx1115messages_bynameIwEC1ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE16find_last_not_ofERKS2_m",
    "_ZNKSt10moneypunctIcLb0EE10pos_formatEv",
    "_ZNSs9push_backEc",
    "_ZSt9use_facetISt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEERKT_RKSt6locale",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEEC1ESt13_Ios_Openmode",
    "_ZSt18uncaught_exceptionv",
    "_ZNSt10__num_base15_S_format_floatERKSt8ios_basePcc",
    "_ZNKSt7__cxx118messagesIwE4openERKNS_12basic_stringIcSt11char_traitsIcESaIcEEERKSt6localePKc",
    "_ZNKSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_bRSt8ios_baseRSt12_Ios_IostateRe",
    "_ZNSt5ctypeIcE13classic_tableEv",
    "_ZNSt9strstreamC1EPciSt13_Ios_Openmode",
    "_ZNKSt10moneypunctIwLb0EE16do_decimal_pointEv",
    "_ZNSt20__codecvt_utf16_baseIwED1Ev",
    "_ZNKSs12find_last_ofEPKcm",
    "_ZN14__gnu_parallel9_Settings3setERS0_",
    "_ZN9__gnu_cxx6__poolILb1EE13_M_initializeEv",
    "_ZNSt7__cxx118numpunctIcEC2EPSt16__numpunct_cacheIcEm",
    "_ZNSt17__timepunct_cacheIcED1Ev",
    "_ZNSt16nested_exceptionD0Ev",
    "_ZNSs10_S_compareEmm",
    "_ZNSt18__moneypunct_cacheIwLb1EEC2Em",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEE8overflowEj",
    "_ZNSt10ostrstreamC1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEmmPKwm",
    "_ZNKSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_bRSt8ios_baseRSt12_Ios_IostateRSbIwS2_SaIwEE",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEEC1ERKS2_",
    "_ZNKSt7collateIcE4hashEPKcS2_",
    "_ZNSt17__timepunct_cacheIwED0Ev",
    "_ZNKSt10moneypunctIwLb1EE13decimal_pointEv",
    "_ZNSirsERb",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE15_M_check_lengthEmmPKc",
    "_ZNSaIwEC1ERKS_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEaSESt16initializer_listIwE",
    "_ZStrsIecSt11char_traitsIcEERSt13basic_istreamIT0_T1_ES6_RSt7complexIT_E",
    "_ZNSsC1Ev",
    "_ZNSirsERd",
    "_ZGTtNSt12domain_errorD0Ev",
    "_ZNSirsERe",
    "_ZNSirsERf",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE17find_first_not_ofERKS4_m",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEED0Ev",
    "_ZNKSt10ostrstream6pcountEv",
    "_ZNSt15time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZNSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC2Em",
    "_ZNSirsERi",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE17find_first_not_ofEcm",
    "_ZNSirsERj",
    "_ZNSo9_M_insertIyEERSoT_",
    "_ZNSt12ctype_bynameIcED1Ev",
    "_ZNSt7__cxx118numpunctIwED2Ev",
    "_ZNKSi6sentrycvbEv",
    "_ZNSirsERl",
    "_ZNSsC2EPKcRKSaIcE",
    "__atomic_flag_wait_explicit",
    "_ZNKSt14error_category23default_error_conditionEi",
    "_ZdaPvmSt11align_val_t",
    "_ZNSirsERm",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE15_M_update_egptrEv",
    "_ZNSt8messagesIwEC1EP15__locale_structPKcm",
    "_ZNSt12ctype_bynameIwED0Ev",
    "_ZNKSt10moneypunctIwLb0EE16do_thousands_sepEv",
    "_ZNSt7codecvtIwc11__mbstate_tEC2EP15__locale_structm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1IN9__gnu_cxx17__normal_iteratorIPcS4_EEvEET_SA_RKS3_",
    "_ZThn16_NSt18basic_stringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNSt10moneypunctIwLb0EEC1EP15__locale_structPKcm",
    "_ZNSiaSEOSi",
    "_ZNSt7__cxx118messagesIwEC1EP15__locale_structPKcm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEmmmc",
    "_ZNSirsERs",
    "_ZNSoD0Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE5ungetEv",
    "_ZNKSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_bRSt8ios_baseRSt12_Ios_IostateRSs",
    "_ZGTtNSt12out_of_rangeC1EPKc",
    "_ZNSbIwSt11char_traitsIwESaIwEE9_M_assignEPwmw",
    "_ZNSbIwSt11char_traitsIwESaIwEE9_M_assignEPwmw",
    "_ZNSirsERt",
    "_ZNKSt10moneypunctIwLb1EE13thousands_sepEv",
    "_ZNKSbIwSt11char_traitsIwESaIwEE8_M_checkEmPKc",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDiE13do_max_lengthEv",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE5sgetcEv",
    "_ZGTtNSt15underflow_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZTv0_n24_NSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSirsERx",
    "_ZNSt11__timepunctIwE23_M_initialize_timepunctEP15__locale_struct",
    "_ZNSt19__codecvt_utf8_baseIDsED0Ev",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb0EED2Ev",
    "_ZNSirsERy",
    "_ZNKSt20__codecvt_utf16_baseIwE10do_unshiftER11__mbstate_tPcS3_RS3_",
    "_ZNSs6rbeginEv",
    "_ZNSs7_M_dataEPc",
    "_ZGTtNSt12length_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5eraseEN9__gnu_cxx17__normal_iteratorIPwS4_EE",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE3tieEv",
    "_Znam",
    "_ZNKSt10moneypunctIcLb1EE11do_groupingEv",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC2EOS3_",
    "_ZNSt7collateIwEC1EP15__locale_structm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6assignERKS4_",
    "_ZTv0_n24_NSt13basic_fstreamIcSt11char_traitsIcEED0Ev",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE14_M_extract_intItEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZStlsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_PKc",
    "_ZNKSt19__codecvt_utf8_baseIDsE6do_outER11__mbstate_tPKDsS4_RS4_PcS6_RS6_",
    "_ZNSt7__cxx1115time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZSt9use_facetISt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEERKT_RKSt6locale",
    "_ZNSt25__codecvt_utf8_utf16_baseIwED1Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE8_M_limitEmm",
    "_ZNKSs13find_first_ofEPKcm",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep26_M_set_length_and_sharableEm",
    "_ZNK10__cxxabiv129__pointer_to_member_type_info15__pointer_catchEPKNS_17__pbase_type_infoEPPvj",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep26_M_set_length_and_sharableEm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2EPKwmRKS3_",
    "_ZNKSt10moneypunctIwLb1EE13do_neg_formatEv",
    "_ZNSt16__numpunct_cacheIcED1Ev",
    "_ZNSt8ios_baseD0Ev",
    "_ZNSt7__cxx1115numpunct_bynameIcEC2ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt10moneypunctIwLb0EED1Ev",
    "_ZNSt16__numpunct_cacheIwED0Ev",
    "__cxa_allocate_exception",
    "_ZNSi10_M_extractIbEERSiRT_",
    "_ZNKSt12future_error4whatEv",
    "_ZNSt13runtime_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt25__codecvt_utf8_utf16_baseIDsED1Ev",
    "_ZSt9has_facetISt8numpunctIcEEbRKSt6locale",
    "_ZNSt6__norm15_List_node_base4hookEPS0_",
    "_ZNSs7reserveEm",
    "_ZNSs6resizeEmc",
    "_ZNKSt7__cxx118numpunctIwE12do_falsenameEv",
    "_ZNKSt11__timepunctIwE15_M_time_formatsEPPKw",
    "_ZNKSs5rfindERKSsm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertEmRKS4_mm",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE16do_negative_signEv",
    "_ZNSt7collateIwEC1Em",
    "_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNSo8_M_writeEPKcl",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEED2Ev",
    "_ZNSt15time_put_bynameIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZNSt6locale5_ImplC2Em",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE6xsgetnEPwl",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE3getEPwl",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6_M_padEwlRSt8ios_basePwPKwRi",
    "_ZNKSt7collateIwE12_M_transformEPwPKwm",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_l",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb1EEC2EPKcm",
    "_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertEN9__gnu_cxx17__normal_iteratorIPwS4_EEmw",
    "_ZNSt15__exception_ptr13exception_ptrC2Ev",
    "_ZNSt15_List_node_base4hookEPS_",
    "_ZNSs6assignEOSs",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEEC1EOS3_",
    "_ZNSs6insertEN9__gnu_cxx17__normal_iteratorIPcSsEESt16initializer_listIcE",
    "_ZNSt6localeD1Ev",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRPv",
    "_ZNSt15numpunct_bynameIcEC2EPKcm",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2ERKS1_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5clearEv",
    "_ZNKSt10moneypunctIwLb0EE14do_frac_digitsEv",
    "_ZNSt12out_of_rangeD0Ev",
    "_ZNKSt11__timepunctIwE8_M_am_pmEPPKw",
    "_ZNSbIwSt11char_traitsIwESaIwEE5frontEv",
    "_ZNSt7__cxx1115messages_bynameIcED1Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13find_first_ofEcm",
    "_ZNSt7__cxx1115messages_bynameIwED0Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE7reserveEm",
    "_ZN10__cxxabiv121__vmi_class_type_infoD2Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNSs6assignERKSsmm",
    "_ZNKSt7__cxx117collateIwE12do_transformEPKwS3_",
    "_ZNSt8__detail15_List_node_base10_M_reverseEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE9_M_insertImEERS2_T_",
    "_ZSt9has_facetISt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEEbRKSt6locale",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6cbeginEv",
    "_ZNSt10moneypunctIwLb1EED2Ev",
    "_ZNSs6appendESt16initializer_listIcE",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE24_M_extract_wday_or_monthES3_S3_RiPPKwmRSt8ios_baseRSt12_Ios_Iostate",
    "_ZTv0_n24_NSt19basic_istringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSt7__cxx1115time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC1ERKNS_12basic_stringIcS3_SaIcEEEm",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tmcc",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6assignERKS4_",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEE9pbackfailEj",
    "_ZNSt18__moneypunct_cacheIwLb0EEC1Em",
    "_ZNSt7__cxx1114collate_bynameIcED2Ev",
    "_ZNSt7codecvtIcc11__mbstate_tEC2EP15__locale_structm",
    "_ZNSt7__cxx1114collate_bynameIwED1Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEED1Ev",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE11curr_symbolEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2ERKS4_mmRKS3_",
    "_ZNSt11range_errorC1ERKSs",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEEaSEOS3_",
    "_ZNSt7__cxx1115time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC2ERKNS_12basic_stringIcS3_SaIcEEEm",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEEC2ERKNS_12basic_stringIwS2_S3_EESt13_Ios_Openmode",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13_M_local_dataEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEmmmw",
    "_ZNSt13runtime_errorD2Ev",
    "_ZNK11__gnu_debug16_Error_formatter10_M_messageENS_13_Debug_msg_idE",
    "_ZNSt12bad_weak_ptrD0Ev",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE9set_rdbufEPSt15basic_streambufIcS1_E",
    "_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2EPKwRKS1_",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE15_M_extract_nameES3_S3_RiPPKwmRSt8ios_baseRSt12_Ios_Iostate",
    "_ZNSt11logic_errorC2EPKc",
    "_ZNSbIwSt11char_traitsIwESaIwEE6resizeEmw",
    "_ZNKSt8bad_cast4whatEv",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE4swapERS2_",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4rendEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1EOS4_RKS3_",
    "_ZNSbIwSt11char_traitsIwESaIwEEpLEw",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4findEcm",
    "_ZNSbIwSt11char_traitsIwESaIwEE6appendESt16initializer_listIwE",
    "_ZNSt8valarrayImEC2ERKS0_",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2ERKS2_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE3endEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEmmPKw",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE4openERKSsSt13_Ios_Openmode",
    "_ZTv0_n24_NSt18basic_stringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZSt9use_facetISt8messagesIcEERKT_RKSt6locale",
    "_ZNSs5eraseEmm",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE5imbueERKSt6locale",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEE7is_openEv",
    "_ZNSt9basic_iosIcSt11char_traitsIcEED2Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4sizeEv",
    "_ZNSt14overflow_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE6sentryD2Ev",
    "_ZNSt20__codecvt_utf16_baseIDsED0Ev",
    "_ZNSt8ios_base7failureC1ERKSs",
    "_ZNSt8messagesIwEC2Em",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEEC1EOS4_ONS4_14__xfer_bufptrsE",
    "_ZNSt14error_categoryC1Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE7compareEmmRKS2_mm",
    "_ZSt9use_facetISt7collateIcEERKT_RKSt6locale",
    "_ZNSt9strstreamD2Ev",
    "_ZNKSt25__codecvt_utf8_utf16_baseIwE6do_outER11__mbstate_tPKwS4_RS4_PcS6_RS6_",
    "_ZNSt8bad_castD2Ev",
    "_ZN11__gnu_debug19_Safe_iterator_base16_M_attach_singleEPNS_19_Safe_sequence_baseEb",
    "__cxa_vec_cleanup",
    "_ZNSt15messages_bynameIcED1Ev",
    "_ZNSt11regex_errorC2ENSt15regex_constants10error_typeE",
    "_ZNSt8messagesIcED0Ev",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEEC2EOS4_",
    "_ZNSbIwSt11char_traitsIwESaIwEE4swapERS2_",
    "_ZNKSt7__cxx118numpunctIwE16do_decimal_pointEv",
    "__cxa_end_catch",
    "_ZNSt15messages_bynameIwED0Ev",
    "_ZNSt12strstreambuf9pbackfailEi",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEEC1EOS2_",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE11_M_is_localEv",
    "_ZNSt6__norm15_List_node_base10_M_reverseEv",
    "_ZNSi4readEPcl",
    "atomic_flag_test_and_set_explicit",
    "_ZNSt10moneypunctIwLb1EEC2EP15__locale_structPKcm",
    "_ZNKSt10moneypunctIcLb1EE13decimal_pointEv",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE11frac_digitsEv",
    "_ZNSs4_Rep10_M_disposeERKSaIcE",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRb",
    "_ZTv0_n24_NSt9strstreamD0Ev",
    "_ZGTtNSt11range_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE6setbufEPwl",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tmcc",
    "_ZN9__gnu_cxx17__pool_alloc_base12_M_get_mutexEv",
    "_ZNSt9bad_allocD0Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEpLEc",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRd",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRe",
    "_ZGTtNSt12length_errorD2Ev",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRf",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13find_first_ofEPKcmm",
    "_ZNKSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEE5rdbufEv",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED2Ev",
    "_ZNSspLESt16initializer_listIcE",
    "_ZSt9use_facetINSt7__cxx118numpunctIcEEERKT_RKSt6locale",
    "_ZNSt7__cxx1115numpunct_bynameIwEC1EPKcm",
    "_ZNKSt7__cxx118messagesIwE6do_getEiiiRKNS_12basic_stringIwSt11char_traitsIwESaIwEEE",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRj",
    "_ZNSt6chrono3_V212steady_clock3nowEv",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tmcc",
    "_ZNSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRl",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC2EOS2_",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRm",
    "_ZNKSt7codecvtIDsc11__mbstate_tE5do_inERS0_PKcS4_RS4_PDsS6_RS6_",
    "_ZSt4endlIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEE7is_openEv",
    "_ZNKSs7compareERKSs",
    "_ZNSt7__cxx1110moneypunctIwLb0EEC2EPSt18__moneypunct_cacheIwLb0EEm",
    "_ZNSoC1EOSo",
    "_ZNKSt7collateIwE7compareEPKwS2_S2_S2_",
    "_ZNKSt7__cxx118numpunctIwE16do_thousands_sepEv",
    "_ZNKSt20__codecvt_utf16_baseIDsE16do_always_noconvEv",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5emptyEv",
    "_ZSt9use_facetISt5ctypeIcEERKT_RKSt6locale",
    "_ZNSt10istrstreamC1EPKc",
    "_ZNSt19__codecvt_utf8_baseIwED0Ev",
    "_ZNSt12strstreambufC2EPKhl",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRt",
    "_ZNSbIwSt11char_traitsIwESaIwEE14_M_replace_auxEmmmw",
    "_ZNKSt10moneypunctIcLb1EE13thousands_sepEv",
    "_ZNK11__gnu_debug16_Error_formatter13_M_print_wordEPKc",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE3strERKNS_12basic_stringIcS2_S3_EE",
    "_ZN10__gnu_norm15_List_node_base8transferEPS0_S1_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12_Alloc_hiderC1EPcOS3_",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRx",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE8in_availEv",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRy",
    "_ZNSt7__cxx117collateIwEC1EP15__locale_structm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE8pop_backEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE8pubimbueERKSt6locale",
    "_ZSt9has_facetISt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEEbRKSt6locale",
    "_ZNSt10ostrstreamD0Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4copyEPwmm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12_M_constructEmc",
    "_ZNSt16nested_exceptionD2Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE12_Alloc_hiderC1EPwRKS1_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4backEv",
    "_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E",
    "_ZNSt17__timepunct_cacheIwED2Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertEmmw",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE14__xfer_bufptrsD1Ev",
    "_ZNKSt13runtime_error4whatEv",
    "_ZNSdC2Ev",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEE4syncEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE13_S_copy_charsEPwN9__gnu_cxx17__normal_iteratorIPKwS2_EES8_",
    "_ZGTtNSt12domain_errorD2Ev",
    "_ZNKSt10moneypunctIcLb1EE13do_neg_formatEv",
    "_ZdlPvm",
    "_ZSt24__throw_out_of_range_fmtPKcz",
    "_ZNSt18__moneypunct_cacheIwLb1EED1Ev",
    "_ZNKSt7__cxx118messagesIcE20_M_convert_from_charEPc",
    "_ZNSbIwSt11char_traitsIwESaIwEE6appendEPKw",
    "_ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_E",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEED2Ev",
    "_ZNSt7__cxx1115messages_bynameIwEC2EPKcm",
    "_ZTv0_n24_NSoD1Ev",
    "_ZNSt15time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_E",
    "_ZNSt12__basic_fileIcE5closeEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS4_EES8_NS6_IPKwS4_EESB_",
    "_ZNSt6localeC1EPNS_5_ImplE",
    "_ZNSbIwSt11char_traitsIwESaIwEE12_Alloc_hiderC2EPwRKS1_",
    "_ZN10__cxxabiv117__pbase_type_infoD1Ev",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE7copyfmtERKS2_",
    "_ZSt9has_facetINSt7__cxx117collateIwEEEbRKSt6locale",
    "_ZSt9use_facetINSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEEERKT_RKSt6locale",
    "_ZNSt8ios_base7failureB5cxx11D1Ev",
    "_ZNKSt3tr14hashINSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEEEclES6_",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEEC2ESt13_Ios_Openmode",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEC1EOS2_",
    "_ZNSt12out_of_rangeC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt12ctype_bynameIwED2Ev",
    "_ZNSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEE6xsgetnEPcl",
    "_ZNKSt8numpunctIwE8groupingEv",
    "_ZNSsaSEPKc",
    "_ZNSt7collateIcEC2Em",
    "_ZNSt15numpunct_bynameIcED0Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE12_S_constructIPKwEEPwT_S7_RKS1_St20forward_iterator_tag",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4findERKS2_m",
    "_ZNSoD2Ev",
    "_ZNSs7replaceEmmRKSs",
    "_ZNKSt5ctypeIwE8do_widenEc",
    "_ZNSt12system_errorD0Ev",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE13negative_signEv",
    "_ZNSt7codecvtIDsc11__mbstate_tED0Ev",
    "_ZStrsIwSt11char_traitsIwEERSt13basic_istreamIT_T0_ES6_St5_Setw",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE13negative_signEv",
    "_ZNKSs6substrEmm",
    "_ZNSt6__norm15_List_node_base11_M_transferEPS0_S1_",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEEC2EOS3_",
    "_ZNSt19__codecvt_utf8_baseIDsED2Ev",
    "_ZN10__cxxabiv117__array_type_infoD0Ev",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE16do_positive_signEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE9_M_insertIlEERS2_T_",
    "_ZNSt14collate_bynameIcEC1EPKcm",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE16find_last_not_ofEPKwmm",
    "_ZNSt12__basic_fileIcE4openEPKcSt13_Ios_Openmodei",
    "_ZNSt10istrstream3strEv",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5crendEv",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE4fillEv",
    "_ZNKSsixEm",
    "_ZNSbIwSt11char_traitsIwESaIwEE13_S_copy_charsEPwS3_S3_",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE7is_openEv",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE11do_get_timeES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt6__norm15_List_node_base7_M_hookEPS0_",
    "_ZNSt7__cxx1114collate_bynameIcEC2EPKcm",
    "_ZNKSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_RSt8ios_basewPK2tmPKwSB_",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEEC1EOS4_",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1Ev",
    "_ZNSt11__timepunctIwEC1EP15__locale_structPKcm",
    "_ZNSt15time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC1ERKSsm",
    "_ZNSt17moneypunct_bynameIcLb1EEC2ERKSsm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE5rfindEPKwmm",
    "_ZNSt8ios_baseD2Ev",
    "_ZNK10__cxxabiv117__class_type_info12__do_dyncastElNS0_10__sub_kindEPKS0_PKvS3_S5_RNS0_16__dyncast_resultE",
    "_ZNKSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE3strEv",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE8get_yearES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSbIwSt11char_traitsIwESaIwEE3endEv",
    "_ZNSt16__numpunct_cacheIwED2Ev",
    "__cxa_get_globals_fast",
    "_ZNKSt8numpunctIwE13decimal_pointEv",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE9pbackfailEi",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEpLESt16initializer_listIwE",
    "_ZNKSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE10_M_extractILb1EEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRSs",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE14__xfer_bufptrsD2Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractIlEERS2_RT_",
    "_ZdaPv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractIxEERS2_RT_",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE6appendERKS2_",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE16find_last_not_ofERKS4_m",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsputnEPKwl",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12_M_constructIPKcEEvT_S8_St20forward_iterator_tag",
    "_ZNSt22condition_variable_anyC2Ev",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEaSEOS2_",
    "_ZNSt7__cxx117collateIwEC1Em",
    "_ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPKcS4_EES9_S8_m",
    "_ZNSs12_Alloc_hiderC1EPcRKSaIcE",
    "_ZNKSt11__timepunctIcE15_M_time_formatsEPPKc",
    "_ZNSt15_List_node_base7_M_hookEPS_",
    "_ZNSs6insertEN9__gnu_cxx17__normal_iteratorIPcSsEEmc",
    "_ZNKSt8numpunctIcE9falsenameEv",
    "_ZNSsaSEc",
    "_ZNSt7collateIcEC1EP15__locale_structm",
    "_ZNSt15__exception_ptr13exception_ptrD1Ev",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb1EED0Ev",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC2EPKcSt13_Ios_Openmode",
    "_ZSt10unexpectedv",
    "_ZNKSt10moneypunctIcLb0EE14do_curr_symbolEv",
    "_ZSt25__throw_bad_function_callv",
    "_ZNSt7collateIwED0Ev",
    "_ZNSt6locale5_ImplD1Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE6insertEN9__gnu_cxx17__normal_iteratorIPwS2_EEw",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1ERKS3_",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEED1Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEEC1ERKS2_",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basewb",
    "_ZNSt11regex_errorD1Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE2atEm",
    "_ZNKSt8numpunctIwE13thousands_sepEv",
    "_ZNSt10moneypunctIcLb1EEC2Em",
    "_ZNSt15__exception_ptreqERKNS_13exception_ptrES2_",
    "_ZSt15system_categoryv",
    "_ZNSt12out_of_rangeD2Ev",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basewd",
    "_ZStlsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_St14_Resetiosflags",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basewe",
    "_ZNKSt7codecvtIDic11__mbstate_tE6do_outERS0_PKDiS4_RS4_PcS6_RS6_",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE17find_first_not_ofEPKwm",
    "_ZNSt6thread6detachEv",
    "_ZNSt7__cxx1115messages_bynameIwED2Ev",
    "_ZNSt7codecvtIcc11__mbstate_tEC1Em",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7_S_moveEPcPKcm",
    "_ZNKSt3tr14hashISsEclESs",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertIN9__gnu_cxx17__normal_iteratorIPwS4_EEEEvS9_T_SA_",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basewl",
    "_ZNSt14codecvt_bynameIcc11__mbstate_tEC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt11range_errorC1EPKc",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basewm",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEE3strERKNS_12basic_stringIwS2_S3_EE",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEEC1EOS3_",
    "_ZNSt10moneypunctIwLb0EEC2EPSt18__moneypunct_cacheIwLb0EEm",
    "_ZNSt7__cxx1110moneypunctIwLb0EEC1Em",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE16_M_get_allocatorEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE10pubseekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNSs6insertEmmc",
    "_ZN10__cxxabiv120__function_type_infoD1Ev",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEEaSEOS4_",
    "_ZNKSt7__cxx117collateIcE12do_transformEPKcS3_",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC2Ev",
    "_ZGTtNSt11logic_errorD1Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE16find_last_not_ofEPKwm",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basewx",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basewy",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE13_M_insert_intIlEES3_S3_RSt8ios_basewT_",
    "_ZNSt18__moneypunct_cacheIwLb0EED0Ev",
    "_ZNKSt10moneypunctIwLb1EE13do_pos_formatEv",
    "_ZN9__gnu_cxx6__poolILb1EE16_M_get_thread_idEv",
    "_ZNSt9__atomic011atomic_flag5clearESt12memory_order",
    "_ZNSt12bad_weak_ptrD2Ev",
    "_ZNKSt10moneypunctIcLb0EE14do_frac_digitsEv",
    "_ZThn16_NSt18basic_stringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZNVSt9__atomic011atomic_flag12test_and_setESt12memory_order",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE9showmanycEv",
    "_ZNSt15underflow_errorC2EPKc",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basewPKv",
    "_ZNSt7__cxx1115numpunct_bynameIcED0Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7_M_dataEv",
    "_ZN14__gnu_parallel9_Settings3getEv",
    "_ZNKSt13basic_fstreamIcSt11char_traitsIcEE7is_openEv",
    "_ZNKSt13basic_fstreamIcSt11char_traitsIcEE7is_openEv",
    "_ZNKSt19__codecvt_utf8_baseIDsE13do_max_lengthEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendEPKcm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1ERKS4_",
    "_ZNSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC1Em",
    "_ZN10__cxxabiv123__fundamental_type_infoD0Ev",
    "_ZNKSt14basic_ifstreamIcSt11char_traitsIcEE5rdbufEv",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC2ERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEEC2Ev",
    "_ZNSo9_M_insertIeEERSoT_",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEE9pbackfailEi",
    "_ZNSt5ctypeIwEC2Em",
    "_ZNSs12_S_constructIPcEES0_T_S1_RKSaIcESt20forward_iterator_tag",
    "_ZNSt9basic_iosIwSt11char_traitsIwEEC1Ev",
    "_ZNKSt3_V214error_category10_M_messageEi",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE13get_monthnameES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt14error_categoryD0Ev",
    "_ZNSsC1ERKSsmm",
    "_ZSt15set_new_handlerPFvvE",
    "_ZNKSt7codecvtIDic11__mbstate_tE10do_unshiftERS0_PcS3_RS3_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEEC1ERSt14basic_iostreamIwS1_E",
    "_ZNSt20__codecvt_utf16_baseIDsED2Ev",
    "_ZNSt5ctypeIcED0Ev",
    "_ZTv0_n24_NSt14basic_ofstreamIcSt11char_traitsIcEED1Ev",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE11curr_symbolEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEmmPKwm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertEmRKS4_",
    "_ZNSt7__cxx1110moneypunctIwLb0EEC1EPSt18__moneypunct_cacheIwLb0EEm",
    "_ZNSt8messagesIcED2Ev",
    "_ZNSt15messages_bynameIwED2Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE5gbumpEi",
    "_ZSt21__copy_streambufs_eofIwSt11char_traitsIwEElPSt15basic_streambufIT_T0_ES6_Rb",
    "_ZNSt7__cxx1114collate_bynameIwEC1ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNKSt8numpunctIwE16do_thousands_sepEv",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE16_M_extract_floatES3_S3_RSt8ios_baseRSt12_Ios_IostateRSs",
    "_ZNSt12domain_errorC2ERKSs",
    "_ZNSt8messagesIwED1Ev",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEPFRSt9basic_iosIwS1_ES5_E",
    "_ZNSsC1EPKcRKSaIcE",
    "_ZNSs7replaceEmmPKc",
    "_ZNSt7__cxx1110moneypunctIwLb1EEC2Em",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12find_last_ofEPKwm",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEEaSEOS3_",
    "_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St12_Setiosflags",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPcS4_EES8_S7_S7_",
    "_ZNSt9bad_allocD2Ev",
    "_ZNSi5tellgEv",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE8overflowEj",
    "_ZNSt10bad_typeidD1Ev",
    "_ZNKSo6sentrycvbEv",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14_M_extract_intIlEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_St16initializer_listIcE",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC2EPKcSt13_Ios_Openmode",
    "_ZNSspLEPKc",
    "_ZNSt7__cxx1115numpunct_bynameIwED1Ev",
    "_ZNSt13bad_exceptionD0Ev",
    "_ZNSt14codecvt_bynameIcc11__mbstate_tEC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZThn16_NSt13basic_fstreamIwSt11char_traitsIwEED1Ev",
    "_ZNSt8ios_base7_M_moveERS_",
    "_ZNKSt5ctypeIcE9do_narrowEcc",
    "__cxa_deleted_virtual",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode",
    "_ZNKSt7collateIwE12do_transformEPKwS2_",
    "_ZNSt19__codecvt_utf8_baseIwED2Ev",
    "_ZTv0_n24_NSdD0Ev",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEEC1EOS3_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPKwS4_EES9_NS6_IPwS4_EESB_",
    "_ZNKSt7__cxx117collateIwE4hashEPKwS3_",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE8get_timeES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSo6sentryC1ERSo",
    "_ZNKSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE10_M_extractILb0EEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRSs",
    "_ZNSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC2Em",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE22_M_convert_to_externalEPwl",
    "_ZNSt8valarrayImEC2Em",
    "_ZNSt10ostrstreamD2Ev",
    "_ZNSt7codecvtIwc11__mbstate_tEC2Em",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEE4swapERS4_",
    "_ZN9__gnu_cxx6__poolILb1EE16_M_reserve_blockEmm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE2atEm",
    "_ZNSt6locale5_ImplC2ERKS0_m",
    "_ZNSt10moneypunctIcLb0EEC1Em",
    "_ZNKSt15basic_streambufIwSt11char_traitsIwEE6getlocEv",
    "_ZNSt11logic_errorD1Ev",
    "_ZNSt15underflow_errorD1Ev",
    "_ZNKSt7codecvtIcc11__mbstate_tE11do_encodingEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPKcS4_EES9_mc",
    "_ZNSs7replaceEmmPKcm",
    "_ZNSdD1Ev",
    "_ZNSsD2Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPKwS4_EES9_RKS4_",
    "_ZTv0_n24_NSt19basic_istringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEC2EPKcSt13_Ios_Openmode",
    "_ZNKSt7__cxx118messagesIcE4openERKNS_12basic_stringIcSt11char_traitsIcESaIcEEERKSt6localePKc",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7compareEmmRKS4_mm",
    "_ZNSt8numpunctIcEC1Em",
    "_ZNSs12_S_constructEmcRKSaIcE",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE4moveERS2_",
    "_ZNKSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_bRSt8ios_basewRKSbIwS2_SaIwEE",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13get_allocatorEv",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7compareEmmPKwm",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEEC1ERKNS_12basic_stringIwS2_S3_EESt13_Ios_Openmode",
    "_ZNSt15numpunct_bynameIwEC2ERKSsm",
    "_ZStrsIdcSt11char_traitsIcEERSt13basic_istreamIT0_T1_ES6_RSt7complexIT_E",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb0EEC2ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE14_M_extract_numES3_S3_RiiimRSt8ios_baseRSt12_Ios_Iostate",
    "_ZNSbIwSt11char_traitsIwESaIwEE6assignEPKw",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEE4openERKSsSt13_Ios_Openmode",
    "_ZNKSt18basic_stringstreamIwSt11char_traitsIwESaIwEE3strEv",
    "_ZNSt17moneypunct_bynameIwLb0EEC2EPKcm",
    "_ZNKSt8numpunctIwE8truenameEv",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE4moveEOS2_",
    "_ZGTtNSt12out_of_rangeC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt7__cxx117collateIcEC2Em",
    "_ZNSsC2IN9__gnu_cxx17__normal_iteratorIPcSsEEEET_S4_RKSaIcE",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNSt15numpunct_bynameIcED2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPKcS4_EES9_St16initializer_listIcE",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE6setbufEPcl",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE16do_positive_signEv",
    "_ZNSt12system_errorD2Ev",
    "_ZNSt15numpunct_bynameIwED1Ev",
    "_ZSt9use_facetISt11__timepunctIwEERKT_RKSt6locale",
    "_ZNSt7codecvtIDsc11__mbstate_tED2Ev",
    "_ZNSt9exceptionD0Ev",
    "_ZStrsIwSt11char_traitsIwESaIwEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_E",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5rfindEPKwm",
    "_ZSt9has_facetINSt7__cxx118numpunctIwEEEbRKSt6locale",
    "_ZSt7getlineIwSt11char_traitsIwESaIwEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_E",
    "_ZNSt7collateIcED1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS4_EES8_S7_S7_",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7crbeginEv",
    "__cxa_vec_cctor",
    "_ZN10__cxxabiv117__array_type_infoD2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE9_S_assignEPcmc",
    "_ZNKSs8_M_checkEmPKc",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertEmPKc",
    "_ZNKSt7collateIcE12do_transformEPKcS2_",
    "_ZNSt9basic_iosIwSt11char_traitsIwEEC2EPSt15basic_streambufIwS1_E",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7reserveEm",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE8get_dateES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6resizeEmc",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE10_M_replaceEmmPKwm",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEED0Ev",
    "_ZSt21__throw_runtime_errorPKc",
    "_ZNSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC1Em",
    "_ZNSbIwSt11char_traitsIwESaIwEE9push_backEw",
    "_ZNSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC2Em",
    "_ZNSs6appendERKSs",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE10neg_formatEv",
    "_ZSt16__throw_bad_castv",
    "_ZNKSt10moneypunctIcLb0EE11curr_symbolEv",
    "_ZNSt7__cxx1110moneypunctIcLb0EEC2EPSt18__moneypunct_cacheIcLb0EEm",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7compareEPKc",
    "_ZNSbIwSt11char_traitsIwESaIwEE18_S_construct_aux_2EmwRKS1_",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE10date_orderEv",
    "_ZNKSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_bRSt8ios_basewe",
    "_ZNSbIwSt11char_traitsIwESaIwEE13_S_copy_charsEPwPKwS5_",
    "_ZSt25notify_all_at_thread_exitRSt18condition_variableSt11unique_lockISt5mutexE",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDsE10do_unshiftER11__mbstate_tPcS3_RS3_",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEED2Ev",
    "_ZNSt22condition_variable_anyD1Ev",
    "_ZNSaIcEC1Ev",
    "atomic_flag_clear_explicit",
    "_ZNKSt20__codecvt_utf16_baseIwE13do_max_lengthEv",
    "_ZnwmRKSt9nothrow_t",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEC2EPKcSt13_Ios_Openmode",
    "_ZNSt8numpunctIwE22_M_initialize_numpunctEP15__locale_struct",
    "_ZNKSt7__cxx117collateIwE10do_compareEPKwS3_S3_S3_",
    "_ZNSt3_V215system_categoryEv",
    "_ZNSt7__cxx1115messages_bynameIcEC2ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE11_M_disjunctEPKc",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEEC2ESt13_Ios_Openmode",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEE3strERKNS_12basic_stringIcS2_S3_EE",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE15_M_insert_floatIdEES3_S3_RSt8ios_basewcT_",
    "_ZNKSt10moneypunctIcLb1EE13do_pos_formatEv",
    "_ZNSt14codecvt_bynameIcc11__mbstate_tED0Ev",
    "_ZNSt8numpunctIwEC1EP15__locale_structm",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb1EED2Ev",
    "_ZNKSt8messagesIcE5closeEi",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPcS4_EES8_mc",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertEmPKcm",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE4syncEv",
    "_ZNSt7collateIwED2Ev",
    "_ZNSt7__cxx117collateIwED0Ev",
    "_ZNKSt8numpunctIwE16do_decimal_pointEv",
    "_ZNSt10moneypunctIwLb0EEC1EPSt18__moneypunct_cacheIwLb0EEm",
    "_ZNSs13_S_copy_charsEPcN9__gnu_cxx17__normal_iteratorIS_SsEES2_",
    "_ZNSt6thread15_M_start_threadESt10shared_ptrINS_10_Impl_baseEE",
    "_ZNSt8numpunctIcE22_M_initialize_numpunctEP15__locale_struct",
    "_ZNKSt5ctypeIwE19_M_convert_to_wmaskEt",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEpLEw",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5rfindEPKcmm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEaSEOS4_",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE13positive_signEv",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2ERKS2_mmRKS1_",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEEC1EOS4_",
    "_ZNSolsEPKv",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE13do_date_orderEv",
    "_ZSt9use_facetINSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEEERKT_RKSt6locale",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE8get_yearES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSsC1ERKSs",
    "_ZN11__gnu_debug30_Safe_unordered_container_base13_M_detach_allEv",
    "_ZNSt15time_put_bynameIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC1EPKcm",
    "_ZNSt10moneypunctIcLb1EED1Ev",
    "_ZStlsIfwSt11char_traitsIwEERSt13basic_ostreamIT0_T1_ES6_RKSt7complexIT_E",
    "_ZNSt8ios_base6xallocEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE8_M_eraseEmm",
    "_ZSt5flushIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRb",
    "_ZTv0_n24_NSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC1Em",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRd",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_S5_S5_",
    "_ZNKSt7__cxx117collateIwE10_M_compareEPKwS3_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPKwS4_EES9_PwSA_",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE9showmanycEv",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRe",
    "_ZNSt12strstreambufC2EPhlS0_",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRf",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE4failEv",
    "_ZNKSt7__cxx118messagesIwE18_M_convert_to_charERKNS_12basic_stringIwSt11char_traitsIwESaIwEEE",
    "_ZNSt7codecvtIcc11__mbstate_tED0Ev",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDiE10do_unshiftER11__mbstate_tPcS3_RS3_",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6_M_padEclRSt8ios_basePcPKcRi",
    "_ZNKSt4hashISbIwSt11char_traitsIwESaIwEEEclES3_",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRj",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRl",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE14do_curr_symbolEv",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEED1Ev",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRm",
    "_ZNKSt7__cxx117collateIwE7compareEPKwS3_S3_S3_",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEEC2ESt13_Ios_Openmode",
    "_ZSt17__throw_bad_allocv",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC2ERKSsSt13_Ios_Openmode",
    "_ZNSt18__moneypunct_cacheIwLb0EED2Ev",
    "_ZNSs18_S_construct_aux_2EmcRKSaIcE",
    "_ZNKSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES4_S4_bRSt8ios_baseRSt12_Ios_IostateRe",
    "_ZNSt7__cxx1110moneypunctIwLb0EED0Ev",
    "_ZNSt8ios_base4InitC1Ev",
    "_ZNSt15time_put_bynameIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC1EPKcm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE2atEm",
    "_ZNKSt7__cxx118numpunctIcE16do_decimal_pointEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertIN9__gnu_cxx17__normal_iteratorIPcS4_EEEEvS9_T_SA_",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRt",
    "_ZNKSt7codecvtIDsc11__mbstate_tE11do_encodingEv",
    "_ZNKSbIwSt11char_traitsIwESaIwEE5beginEv",
    "_ZStlsIfcSt11char_traitsIcEERSt13basic_ostreamIT0_T1_ES6_RKSt7complexIT_E",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1EPKcmRKS3_",
    "_ZNSt7__cxx1115numpunct_bynameIcED2Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE12__safe_pbumpEl",
    "_ZNSt15messages_bynameIwEC2ERKSsm",
    "_ZNKSt5ctypeIwE10do_scan_isEtPKwS2_",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEEC1EOS2_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEpLERKS4_",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRx",
    "_ZTv0_n24_NSt14basic_ifstreamIwSt11char_traitsIwEED0Ev",
    "_ZNSt12length_errorC2EPKc",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRy",
    "_ZNKSt7__cxx118messagesIcE5closeEi",
    "_ZN10__cxxabiv123__fundamental_type_infoD2Ev",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEED1Ev",
    "_ZNSt3_V214error_categoryD0Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE5sputcEc",
    "_ZNSt7__cxx118numpunctIwEC1EP15__locale_structm",
    "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_a",
    "_ZnamSt11align_val_t",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE15_M_update_egptrEv",
    "_ZNSt9basic_iosIwSt11char_traitsIwEED0Ev",
    "_ZNKSs4findEcm",
    "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c",
    "_ZNSt14overflow_errorC1EPKc",
    "_ZNSt8ios_base7failureB5cxx11C1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt13__future_base11_State_baseD0Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEEC2EPSt15basic_streambufIwS1_E",
    "_ZNSt6gslice8_IndexerC2EmRKSt8valarrayImES4_",
    "_ZNSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZNSt14error_categoryD2Ev",
    "_ZNSt17moneypunct_bynameIwLb0EEC2ERKSsm",
    "_ZStplIwSt11char_traitsIwESaIwEESbIT_T0_T1_ES3_RKS6_",
    "_ZNSt5ctypeIcED2Ev",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE7_M_syncEPwmm",
    "_ZNSt15time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNSi3getEPclc",
    "_ZNKSt7codecvtIDic11__mbstate_tE5do_inERS0_PKcS4_RS4_PDiS6_RS6_",
    "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_h",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE3endEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2IN9__gnu_cxx17__normal_iteratorIPwS4_EEvEET_SA_RKS3_",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC1ERKSsSt13_Ios_Openmode",
    "_ZNSt5ctypeIwED1Ev",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE11frac_digitsEv",
    "_ZNSt15__exception_ptr13exception_ptrC1EPv",
    "_ZN9__gnu_cxx6__poolILb0EE13_M_initializeEv",
    "_ZNSt18__moneypunct_cacheIcLb1EEC1Em",
    "_ZNKSt10moneypunctIcLb1EE16do_negative_signEv",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEEC2EOS4_",
    "_ZNKSbIwSt11char_traitsIwESaIwEE6_M_repEv",
    "_ZNKSt7__cxx118numpunctIcE16do_thousands_sepEv",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE10neg_formatEv",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE14do_frac_digitsEv",
    "_ZNK10__cxxabiv119__pointer_type_info15__pointer_catchEPKNS_17__pbase_type_infoEPPvj",
    "_ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC2Em",
    "_ZNKSt10moneypunctIwLb0EE13decimal_pointEv",
    "_ZNSi5seekgESt4fposI11__mbstate_tE",
    "_ZSt5flushIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tmcc",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE9_M_insertIdEERS2_T_",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEEC1Ev",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE3strERKNS_12basic_stringIwS2_S3_EE",
    "_ZNKSt11__timepunctIcE21_M_months_abbreviatedEPPKc",
    "_ZNSs7replaceEmmmc",
    "_ZNKSbIwSt11char_traitsIwESaIwEE12find_last_ofERKS2_m",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE9underflowEv",
    "_ZNSt7__cxx1110moneypunctIwLb1EED1Ev",
    "_ZN10__cxxabiv120__si_class_type_infoD1Ev",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb0EEC2EPKcm",
    "_ZNSt7__cxx1110moneypunctIcLb0EEC2EP15__locale_structPKcm",
    "_ZTv0_n24_NSt14basic_iostreamIwSt11char_traitsIwEED0Ev",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEEC1ESt13_Ios_Openmode",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE16_M_destroy_pbackEv",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13find_first_ofERKS4_m",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE6stosscEv",
    "_ZNSt14codecvt_bynameIwc11__mbstate_tED1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6appendEPKwm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE5c_strEv",
    "_ZNSt13bad_exceptionD2Ev",
    "_ZNSt7__cxx1115messages_bynameIcEC1EPKcm",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE4swapERS3_",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE9set_rdbufEPSt15basic_streambufIwS1_E",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE9_M_insertIeEERS2_T_",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE7copyfmtERKS2_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertEmRKS4_",
    "_ZTv0_n24_NSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEE4syncEv",
    "_ZSt9has_facetINSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEEEbRKSt6locale",
    "_ZNSt7__cxx117collateIcEC1EP15__locale_structm",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNKSt10moneypunctIwLb0EE13thousands_sepEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13_S_copy_charsEPcPKcS7_",
    "_ZNSt14codecvt_bynameIwc11__mbstate_tEC1ERKSsm",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb0EED1Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12find_last_ofEwm",
    "_ZNSt10moneypunctIcLb0EEC2EPSt18__moneypunct_cacheIcLb0EEm",
    "_ZTv0_n24_NSt18basic_stringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNKSt19__codecvt_utf8_baseIDiE13do_max_lengthEv",
    "_ZNSt12length_errorD1Ev",
    "_ZNKSt3tr14hashIeEclEe",
    "_ZNKSt7__cxx118numpunctIwE13decimal_pointEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4swapERS4_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5beginEv",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC1EOS2_",
    "__cxa_bad_typeid",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE8_M_pbumpEPwS4_l",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_RSt8ios_basecPKv",
    "_ZNKSt8messagesIwE8do_closeEi",
    "_ZNSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZStplIcSt11char_traitsIcESaIcEENSt7__cxx1112basic_stringIT_T0_T1_EEPKS5_RKS8_",
    "_ZNSt8valarrayImED1Ev",
    "_ZNSt7codecvtIwc11__mbstate_tED1Ev",
    "_ZNSt12ctype_bynameIwEC1EPKcm",
    "_ZNKSt10moneypunctIwLb0EE13do_neg_formatEv",
    "_ZNSt16invalid_argumentD1Ev",
    "_ZNSt10moneypunctIcLb0EED0Ev",
    "_ZNSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC1Em",
    "_ZNSt8numpunctIwEC2Em",
    "_ZNSt18__moneypunct_cacheIwLb0EE8_M_cacheERKSt6locale",
    "_ZNSt8numpunctIwEC1EPSt16__numpunct_cacheIwEm",
    "_ZGTtNSt13runtime_errorD1Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2IPwEET_S5_RKS1_",
    "_ZSt15_Fnv_hash_bytesPKvmm",
    "_ZNSt8numpunctIcED0Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4findEwm",
    "_ZN11__gnu_debug19_Safe_iterator_base9_M_attachEPNS_19_Safe_sequence_baseEb",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE4fillEv",
    "_ZN10__gnu_norm15_List_node_base4swapERS0_S1_",
    "_ZNSt11char_traitsIcE2eqERKcS2_",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNSt11char_traitsIcE2eqERKcS2_",
    "_ZNKSt10moneypunctIwLb1EE14do_curr_symbolEv",
    "_ZNKSs6rbeginEv",
    "_ZNKSt7__cxx118numpunctIwE13thousands_sepEv",
    "_ZNSs6appendEPKcm",
    "_ZNSt11logic_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE8get_timeES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZThn16_NSt9strstreamD1Ev",
    "_ZNSt10istrstreamC1EPKcl",
    "_ZNK10__cxxabiv120__si_class_type_info11__do_upcastEPKNS_17__class_type_infoEPKvRNS1_15__upcast_resultE",
    "_ZNKSt8__detail20_Prime_rehash_policy14_M_need_rehashEmmm",
    "_ZNSt9exceptionD2Ev",
    "_ZNSt17moneypunct_bynameIwLb1EED1Ev",
    "_ZStplIwSt11char_traitsIwESaIwEESbIT_T0_T1_EPKS3_RKS6_",
    "_ZNSt7__cxx1110moneypunctIcLb0EEC1EPSt18__moneypunct_cacheIcLb0EEm",
    "_ZNKSt5ctypeIcE10do_tolowerEPcPKc",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE6ignoreElj",
    "_ZNSt7__cxx117collateIcED1Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1IPKcvEET_S8_RKS3_",
    "_ZNSt14codecvt_bynameIwc11__mbstate_tEC1EPKcm",
    "_ZNSt12domain_errorD1Ev",
    "_ZNSo9_M_insertIPKvEERSoT_",
    "_ZTv0_n24_NSt13basic_fstreamIwSt11char_traitsIwEED0Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZSt4endsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNKSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEE5rdbufEv",
    "_ZNSt7__cxx1110moneypunctIwLb0EEC1EP15__locale_structPKcm",
    "_ZN11__gnu_debug25_Safe_local_iterator_base9_M_detachEv",
    "_ZNKSbIwSt11char_traitsIwESaIwEE6lengthEv",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE13positive_signEv",
    "_ZNSiC1EOSi",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEED2Ev",
    "_ZNSi4peekEv",
    "_ZN9__gnu_cxx27__verbose_terminate_handlerEv",
    "_ZNSt11__timepunctIcEC2Em",
    "_ZNVSt9__atomic011atomic_flag5clearESt12memory_order",
    "_ZNSbIwSt11char_traitsIwESaIwEE4backEv",
    "_ZNKSt3tr14hashIRKSbIwSt11char_traitsIwESaIwEEEclES6_",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEE4openERKSsSt13_Ios_Openmode",
    "_ZNSt11__timepunctIwEC1Em",
    "_ZNSt10istrstreamC2EPcl",
    "_ZTv0_n24_NSt13basic_istreamIwSt11char_traitsIwEED0Ev",
    "_ZNKSt11__timepunctIcE6_M_putEPcmPKcPK2tm",
    "_ZNSbIwSt11char_traitsIwESaIwEE12_S_constructIPwEES4_T_S5_RKS1_St20forward_iterator_tag",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEE5closeEv",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE7_M_syncEPcmm",
    "_ZNSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNSs14_M_replace_auxEmmmc",
    "_ZStplIcSt11char_traitsIcESaIcEENSt7__cxx1112basic_stringIT_T0_T1_EERKS8_SA_",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEEC2EOS3_",
    "_ZNSs4rendEv",
    "_ZNSt25__codecvt_utf8_utf16_baseIDiED0Ev",
    "_ZNSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEEaSERKS2_",
    "_ZNKSt10moneypunctIcLb0EE11frac_digitsEv",
    "_ZNSt12domain_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt7codecvtIwc11__mbstate_tE6do_outERS0_PKwS4_RS4_PcS6_RS6_",
    "_ZStrsIwSt11char_traitsIwEERSt13basic_istreamIT_T0_ES6_St8_SetfillIS3_E",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE16find_last_not_ofEcm",
    "_ZNSt8valarrayImEixEm",
    "_ZNKSt13basic_istreamIwSt11char_traitsIwEE6sentrycvbEv",
    "_ZNKSt7codecvtIcc11__mbstate_tE5do_inERS0_PKcS4_RS4_PcS6_RS6_",
    "_ZSt18__throw_bad_typeidv",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE8in_availEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractIeEERS2_RT_",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb0EEC2ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSaIwEC2Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4_Rep12_M_is_sharedEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPKcS4_EES9_RKS4_",
    "_ZStlsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_S3_",
    "_ZNSt14codecvt_bynameIcc11__mbstate_tED2Ev",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE15_M_extract_nameES4_S4_RiPPKcmRSt8ios_baseRSt12_Ios_Iostate",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE6setbufEPcl",
    "_ZTv0_n24_NSt13basic_ostreamIwSt11char_traitsIwEED0Ev",
    "_ZNKSt14basic_ofstreamIcSt11char_traitsIcEE5rdbufEv",
    "_ZNKSt7__cxx118numpunctIcE11do_groupingEv",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE15_M_cache_localeERKSt6locale",
    "_ZNSt7__cxx117collateIwED2Ev",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE9_M_insertIbEERS2_T_",
    "_ZNSt12out_of_rangeC1ERKSs",
    "_ZNKSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE9_M_insertILb1EEES4_S4_RSt8ios_basecRKNS_12basic_stringIcS3_SaIcEEE",
    "_ZNKSt6localeeqERKS_",
    "_ZNSaIwEC2ERKS_",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE8get_dateES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNKSs16find_last_not_ofEPKcmm",
    "_ZNSt7__cxx1115numpunct_bynameIwEC2ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE4swapERS2_",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC2ERKSsSt13_Ios_Openmode",
    "_ZSt9use_facetISt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEERKT_RKSt6locale",
    "_ZN9__gnu_cxx6__poolILb1EE16_M_reclaim_blockEPcm",
    "__cxa_init_primary_exception",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEE4swapERS2_",
    "_ZNSt12__basic_fileIcE8sys_openEiSt13_Ios_Openmode",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE16do_get_monthnameES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEEC2ESt13_Ios_Openmode",
    "_ZNSt7__cxx118messagesIcEC1Em",
    "_ZNKSt10moneypunctIcLb1EE11curr_symbolEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS4_EES8_PKwm",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE16do_positive_signEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1EOS4_RKS3_",
    "_ZNKSt11__timepunctIwE15_M_am_pm_formatEPKw",
    "_ZNSs4_Rep9_S_createEmmRKSaIcE",
    "_ZNKSt8numpunctIcE16do_decimal_pointEv",
    "_ZSt9has_facetINSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEEEbRKSt6locale",
    "_ZNSt10istrstreamD1Ev",
    "_ZNK10__cxxabiv117__pbase_type_info15__pointer_catchEPKS0_PPvj",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE17find_first_not_ofEPKcmm",
    "_ZNSbIwSt11char_traitsIwESaIwEE6resizeEm",
    "_ZSt4endsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_",
    "_ZNKSbIwSt11char_traitsIwESaIwEE12find_last_ofEPKwm",
    "_ZNSbIwSt11char_traitsIwESaIwEE8pop_backEv",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE14_M_extract_intIlEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNSt18__moneypunct_cacheIcLb0EEC2Em",
    "_ZNKSt8numpunctIwE12do_falsenameEv",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE10pubseekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNSt7codecvtIcc11__mbstate_tED2Ev",
    "_ZNSt20bad_array_new_lengthD1Ev",
    "_ZStlsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_PKS3_",
    "_ZNSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13_S_copy_charsEPwN9__gnu_cxx17__normal_iteratorIS5_S4_EES8_",
    "_ZN9__gnu_cxx17__pool_alloc_base16_M_get_free_listEm",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14do_get_weekdayES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNKSt10moneypunctIcLb0EE13decimal_pointEv",
    "_ZNSt8numpunctIcEC1EP15__locale_structm",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE11_M_is_localEv",
    "_ZNSt6__norm15_List_node_base4swapERS0_S1_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE11_M_capacityEm",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC1ERKSsSt13_Ios_Openmode",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE4openERKNSt7__cxx1112basic_stringIcS0_IcESaIcEEESt13_Ios_Openmode",
    "_ZNSt7__cxx1110moneypunctIwLb0EED2Ev",
    "_ZNSt15messages_bynameIwEC1EPKcm",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5crendEv",
    "_ZNSt6thread6_StateD0Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5clearEv",
    "_ZNKSt10moneypunctIcLb1EE8groupingEv",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDiE6do_outER11__mbstate_tPKDiS4_RS4_PcS6_RS6_",
    "_ZNSbIwSt11char_traitsIwESaIwEE15_M_replace_safeEmmPKwm",
    "_ZNKSt8messagesIcE4openERKSsRKSt6locale",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE10pos_formatEv",
    "_ZNKSt8numpunctIcE16do_thousands_sepEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEmmPKw",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4findEPKcmm",
    "_ZNKSt8messagesIwE4openERKSsRKSt6locale",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEEC1ESt13_Ios_Openmode",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv",
    "_ZNSt3_V214error_categoryD2Ev",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE10exceptionsEv",
    "_ZNSs6insertEmPKc",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2Ev",
    "_ZNSt9basic_iosIwSt11char_traitsIwEED2Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE5sputnEPKcl",
    "_ZSt19__throw_range_errorPKc",
    "_ZNSt13__future_base11_State_baseD2Ev",
    "_ZNKSt11__timepunctIwE19_M_days_abbreviatedEPPKw",
    "_ZNKSt10moneypunctIcLb0EE16do_negative_signEv",
    "_ZNKSt10moneypunctIcLb0EE13thousands_sepEv",
    "_ZNSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12find_last_ofEPKcm",
    "_ZNSt17moneypunct_bynameIwLb0EED0Ev",
    "_ZNSt15time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZNKSt7__cxx118numpunctIwE11do_groupingEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEPSt15basic_streambufIwS1_E",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEmmRKS4_",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE14do_curr_symbolEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPcl",
    "_ZNKSt5ctypeIcE9do_narrowEPKcS2_cPc",
    "_ZnwmSt11align_val_tRKSt9nothrow_t",
    "__cxa_guard_release",
    "_ZdlPvSt11align_val_t",
    "_ZNSt7__cxx1110moneypunctIcLb1EEC1Em",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE17find_first_not_ofERKS4_m",
    "_ZNSt7__cxx1115time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNSt12strstreambuf8_M_setupEPcS0_l",
    "_ZNKSt5ctypeIcE10do_toupperEPcPKc",
    "_ZNSt6locale5facet17_S_clone_c_localeERP15__locale_struct",
    "_ZNKSbIwSt11char_traitsIwESaIwEE9_M_ibeginEv",
    "_ZNSt15_List_node_base7reverseEv",
    "_ZNSt9type_infoD1Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEED0Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractIjEERS2_RT_",
    "_ZNSt18__moneypunct_cacheIcLb1EED0Ev",
    "_ZNSi8readsomeEPcl",
    "_ZNKSt10moneypunctIcLb0EE13do_neg_formatEv",
    "_ZGTtNSt16invalid_argumentC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEEC2EOS3_",
    "_ZNSt11logic_erroraSERKS_",
    "_ZNSi3getEPcl",
    "_ZNSt10moneypunctIcLb0EEC1EPSt18__moneypunct_cacheIcLb0EEm",
    "_ZN11__gnu_debug19_Safe_sequence_base12_M_get_mutexEv",
    "_ZNSi4syncEv",
    "_ZGTtNSt12domain_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE8overflowEi",
    "_ZNKSt7__cxx117collateIwE9transformEPKwS3_",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4findEPKwm",
    "_ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEE7is_openEv",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_RSt8ios_basecb",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE13do_date_orderEv",
    "_ZNSt7__cxx118numpunctIcEC1EP15__locale_structm",
    "_ZSt9use_facetISt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEERKT_RKSt6locale",
    "_ZTv0_n24_NSt10ostrstreamD0Ev",
    "_ZNSt7__cxx1110moneypunctIwLb1EEC2EP15__locale_structPKcm",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE8_M_limitEmm",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_RSt8ios_basecd",
    "_ZSt9use_facetISt10moneypunctIwLb1EEERKT_RKSt6locale",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_RSt8ios_basece",
    "_ZNKSbIwSt11char_traitsIwESaIwEE7compareEmmPKwm",
    "_ZSt14__convert_to_vIeEvPKcRT_RSt12_Ios_IostateRKP15__locale_struct",
    "_ZNKSt19__codecvt_utf8_baseIwE13do_max_lengthEv",
    "_ZNSt7collateIwEC2EP15__locale_structm",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode",
    "_ZNK10__cxxabiv120__si_class_type_info12__do_dyncastElNS_17__class_type_info10__sub_kindEPKS1_PKvS4_S6_RNS1_16__dyncast_resultE",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEC1EOS2_",
    "_ZNKSt14basic_ifstreamIwSt11char_traitsIwEE5rdbufEv",
    "_ZTv0_n24_NSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEEC2ERKNS_12basic_stringIwS2_S3_EESt13_Ios_Openmode",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_RSt8ios_basecl",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_RSt8ios_basecm",
    "_ZNSi5seekgElSt12_Ios_Seekdir",
    "_ZNSt12__basic_fileIcE8sys_openEP8_IO_FILESt13_Ios_Openmode",
    "_ZNKSt8numpunctIcE13decimal_pointEv",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE6narrowEcc",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE10exceptionsESt12_Ios_Iostate",
    "_ZSt17iostream_categoryv",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE14do_frac_digitsEv",
    "_ZNSt9__cxx199815_List_node_base10_M_reverseEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1EPKwmRKS3_",
    "_ZNSt14codecvt_bynameIcc11__mbstate_tEC1EPKcm",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE9_M_insertIPKvEERS2_T_",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE13get_monthnameES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE5gbumpEi",
    "_ZNSt6thread4joinEv",
    "_ZNSt11range_errorC2ERKSs",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE8_M_pbumpEPcS4_l",
    "_ZnwmSt11align_val_t",
    "_ZNKSt15basic_streambufIcSt11char_traitsIcEE5ebackEv",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEE5closeEv",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEEC1ERKSbIwS1_S2_ESt13_Ios_Openmode",
    "_ZN9__gnu_cxx18__exchange_and_addEPVii",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2IPwvEET_S7_RKS3_",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12find_last_ofEPKcmm",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_RSt8ios_basecx",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_RSt8ios_basecy",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE6sentryC2ERS2_b",
    "_ZNKSbIwSt11char_traitsIwESaIwEE13get_allocatorEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE7pubsyncEv",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEEaSEOS4_",
    "_ZNSt10moneypunctIcLb0EED2Ev",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEEC1ERKNS_12basic_stringIwS2_S3_EESt13_Ios_Openmode",
    "_ZNSsC1IPKcEET_S2_RKSaIcE",
    "_ZNSt15time_put_bynameIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC2ERKSsm",
    "_ZNSt8ios_base7failureB5cxx11C1EPKcRKSt10error_code",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEEC1ERKNS_12basic_stringIwS2_S3_EESt13_Ios_Openmode",
    "_ZNKSt7codecvtIwc11__mbstate_tE10do_unshiftERS0_PcS3_RS3_",
    "_ZSt9has_facetISt8messagesIwEEbRKSt6locale",
    "_ZNKSt8numpunctIcE13thousands_sepEv",
    "_ZNSt8numpunctIcED2Ev",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC2Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE9sputbackcEw",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5emptyEv",
    "_ZnamSt11align_val_tRKSt9nothrow_t",
    "_ZNSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZNSt15__exception_ptrneERKNS_13exception_ptrES2_",
    "_ZNSt8numpunctIwED1Ev",
    "_ZNKSt11__timepunctIcE15_M_am_pm_formatEPKc",
    "_ZNKSt10moneypunctIwLb1EE14do_frac_digitsEv",
    "_ZNSt14codecvt_bynameIcc11__mbstate_tEC1ERKSsm",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEED2Ev",
    "_ZNSt17bad_function_callD1Ev",
    "_ZNKSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE3strEv",
    "_ZNSt8ios_base7failureC2ERKSs",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE10pos_formatEv",
    "_ZNKSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE9_M_insertILb0EEES4_S4_RSt8ios_basecRKNS_12basic_stringIcS3_SaIcEEE",
    "__cxa_vec_ctor",
    "_ZNSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC2Em",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEEC2EPSt15basic_streambufIwS1_E",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEEC2EOS2_",
    "_ZNSi7getlineEPcl",
    "_ZNSt11logic_errorC1EPKc",
    "_ZNSt19__codecvt_utf8_baseIDiED1Ev",
    "_ZNSdC1EPSt15basic_streambufIcSt11char_traitsIcEE",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE13_M_insert_intIyEES3_S3_RSt8ios_basecT_",
    "_ZNSo6sentryD1Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPKcS4_EES9_PcSA_",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2ERKS2_mm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE9_M_mutateEmmPKwm",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEE7is_openEv",
    "_ZNKSs7_M_dataEv",
    "_ZNSo9_M_insertImEERSoT_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5frontEv",
    "_ZNSt7__cxx1114collate_bynameIcEC2ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt15time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC2ERKSsm",
    "_ZNKSt7__cxx118messagesIwE7do_openERKNS_12basic_stringIcSt11char_traitsIcESaIcEEERKSt6locale",
    "_ZNSt7__cxx118numpunctIwEC2EPSt16__numpunct_cacheIwEm",
    "_ZNSbIwSt11char_traitsIwESaIwEEaSEOS2_",
    "_ZNSt12ctype_bynameIwEC2ERKSsm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE6substrEmm",
    "_ZNSs6insertEmRKSs",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6assignESt16initializer_listIcE",
    "_ZNSt7__cxx1115numpunct_bynameIwEC2EPKcm",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE5sputcEw",
    "_ZNKSt20__codecvt_utf16_baseIDiE6do_outER11__mbstate_tPKDiS4_RS4_PcS6_RS6_",
    "_ZNKSt7__cxx117collateIcE10do_compareEPKcS3_S3_S3_",
    "_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_St14_Resetiosflags",
    "_ZNSt11__timepunctIcE23_M_initialize_timepunctEP15__locale_struct",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEEC1EOS2_",
    "_ZNKSt3tr14hashINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEEclES6_",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEEC2ERKSbIwS1_S2_ESt13_Ios_Openmode",
    "_ZNSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPcS4_EES8_PKcSA_",
    "_ZNSt11__timepunctIcED1Ev",
    "_ZNSt25__codecvt_utf8_utf16_baseIDiED2Ev",
    "_ZNKSt10moneypunctIcLb1EE14do_curr_symbolEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1IPwvEET_S7_RKS3_",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE4syncEv",
    "_ZSt9use_facetINSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEEERKT_RKSt6locale",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE9pbackfailEj",
    "_ZNSaIcED2Ev",
    "_ZNKSs8max_sizeEv",
    "_ZNSt11__timepunctIwED0Ev",
    "_ZNSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC1Em",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEEC1EOS4_",
    "_ZNSaIwED1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS4_EES8_PKwSA_",
    "_ZNSo4swapERSo",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2IPKcvEET_S8_RKS3_",
    "_ZNSt15__exception_ptr13exception_ptr4swapERS0_",
    "_ZNSbIwSt11char_traitsIwESaIwEED1Ev",
    "_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base",
    "_ZNKSt7__cxx118numpunctIcE11do_truenameEv",
    "_ZNKSt10moneypunctIwLb0EE13do_pos_formatEv",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE10pubseekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC1Em",
    "_ZNKSt19basic_ostringstreamIwSt11char_traitsIwESaIwEE5rdbufEv",
    "_ZNKSt25__codecvt_utf8_utf16_baseIwE16do_always_noconvEv",
    "_ZNSt7__cxx118messagesIwEC2EP15__locale_structPKcm",
    "_ZTv0_n24_NSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSt10moneypunctIwLb0EEC2EP15__locale_structPKcm",
    "_ZSt9has_facetISt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEEbRKSt6locale",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12_M_constructEmw",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE16do_positive_signEv",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEE7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE4failEv",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEEC1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE9_M_assignERKS4_",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEEC2EPSt15basic_streambufIwS1_E",
    "_ZNSi7getlineEPclc",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4backEv",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEaSEOS3_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1EmcRKS3_",
    "_ZNKSt10moneypunctIcLb1EE11frac_digitsEv",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14_M_extract_numES4_S4_RiiimRSt8ios_baseRSt12_Ios_Iostate",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE9showmanycEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE14_M_replace_auxEmmmw",
    "_ZNSt7__cxx118messagesIwEC2Em",
    "_ZGTtNSt11logic_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZGTtNSt12out_of_rangeD0Ev",
    "_ZNSt9__cxx199815_List_node_base7_M_hookEPS0_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5eraseEN9__gnu_cxx17__normal_iteratorIPKcS4_EE",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEC2EOS2_",
    "_ZNKSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE10_M_extractILb1EEES4_S4_S4_RSt8ios_baseRSt12_Ios_IostateRNS_12basic_stringIcS3_SaIcEEE",
    "_ZNSt7__cxx118messagesIcED0Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEEC1EOS2_",
    "_ZNSt12__basic_fileIcE6xsputnEPKcl",
    "_ZNSt7__cxx1110moneypunctIcLb0EEC2Em",
    "__cxa_tm_cleanup",
    "_ZNKSt3tr14hashISbIwSt11char_traitsIwESaIwEEEclES4_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEPKv",
    "_ZN11__gnu_debug19_Safe_sequence_base18_M_detach_singularEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2EmcRKS3_",
    "_ZNSt12strstreambufC2EPFPvmEPFvS0_E",
    "_ZNSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2ESt16initializer_listIcERKS3_",
    "_ZNSt18__moneypunct_cacheIcLb0EED1Ev",
    "_ZNSt7__cxx1110moneypunctIwLb0EE24_M_initialize_moneypunctEP15__locale_structPKc",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDiE11do_encodingEv",
    "_ZNSt6locale5facet18_S_create_c_localeERP15__locale_structPKcS2_",
    "_ZNSt8ios_base4InitD2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1EOS4_",
    "_ZNSspLERKSs",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE7putbackEw",
    "_ZNSt14collate_bynameIcEC2EPKcm",
    "_ZNSt6thread6_StateD2Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1ESt16initializer_listIwERKS1_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE9_M_lengthEm",
    "_ZNKSt13basic_fstreamIwSt11char_traitsIwEE7is_openEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPKwS4_EES9_St16initializer_listIwE",
    "_ZNKSt13basic_fstreamIwSt11char_traitsIwEE7is_openEv",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSt7__cxx117collateIwEC2EP15__locale_structm",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEEC2EOS4_",
    "_ZNKSbIwSt11char_traitsIwESaIwEE5frontEv",
    "_ZNKSt15basic_streambufIcSt11char_traitsIcEE6getlocEv",
    "_ZNSs12_S_constructIN9__gnu_cxx17__normal_iteratorIPcSsEEEES2_T_S4_RKSaIcESt20forward_iterator_tag",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEmmPKc",
    "_ZNSt8ios_base13_M_grow_wordsEib",
    "_ZNSbIwSt11char_traitsIwESaIwEEixEm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEEaSEOS4_",
    "_ZGTtNSt12length_errorC2EPKc",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_refcopyEv",
    "_ZNSt15time_put_bynameIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZNKSt7__cxx118numpunctIwE11do_truenameEv",
    "_ZNSt20__codecvt_utf16_baseIDiED1Ev",
    "_ZNSt17moneypunct_bynameIwLb0EED2Ev",
    "_ZNKSs6cbeginEv",
    "_ZN10__cxxabiv129__pointer_to_member_type_infoD1Ev",
    "_ZNKSs7crbeginEv",
    "_ZNKSt10moneypunctIwLb1EE8groupingEv",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE15_M_extract_nameES4_S4_RiPPKwmRSt8ios_baseRSt12_Ios_Iostate",
    "_ZSt16__ostream_insertIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_PKS3_l",
    "_ZNSt15__exception_ptr13exception_ptrC1ERKS0_",
    "_ZNKSt9exception4whatEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE6sentryC1ERS2_",
    "_ZSt9use_facetISt7codecvtIwc11__mbstate_tEERKT_RKSt6locale",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE4peekEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEEC2Ev",
    "_ZNSt10_Sp_lockerC1EPKvS1_",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE12__safe_gbumpEl",
    "_ZTv0_n24_NSt19basic_ostringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNKSt8__detail20_Prime_rehash_policy11_M_next_bktEm",
    "_ZNKSt5ctypeIcE8do_widenEPKcS2_Pc",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEED2Ev",
    "_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_PKcS4_",
    "_ZNSt18__moneypunct_cacheIcLb1EED2Ev",
    "_ZNSt7__cxx1110moneypunctIcLb1EED0Ev",
    "_ZNKSs16find_last_not_ofERKSsm",
    "_ZNKSt14basic_ofstreamIwSt11char_traitsIwEE5rdbufEv",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEEcvPvEv",
    "_ZSt20_Rb_tree_black_countPKSt18_Rb_tree_node_baseS1_",
    "_ZNSt14overflow_errorD0Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6assignERKS4_mm",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE7_M_seekElSt12_Ios_Seekdir11__mbstate_t",
    "_ZNK10__cxxabiv117__class_type_info11__do_upcastEPKS0_PPv",
    "_ZSt9has_facetINSt7__cxx1110moneypunctIwLb0EEEEbRKSt6locale",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEEaSEOS3_",
    "_ZStplIwSt11char_traitsIwESaIwEENSt7__cxx1112basic_stringIT_T0_T1_EES5_RKS8_",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEEC2ERKSbIwS1_S2_ESt13_Ios_Openmode",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_mm",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE6snextcEv",
    "_ZNSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC2Em",
    "_ZNSt10money_base20_S_construct_patternEccc",
    "_ZNSt12strstreambufC1El",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE21_M_extract_via_formatES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tmPKc",
    "_ZNSt8ios_base7failureB5cxx11C2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKSt10error_code",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEEC2EOS3_",
    "_ZNKSt14basic_ifstreamIcSt11char_traitsIcEE7is_openEv",
    "_ZNKSt14basic_ifstreamIcSt11char_traitsIcEE7is_openEv",
    "_ZNKSbIwSt11char_traitsIwESaIwEE12find_last_ofEwm",
    "_ZNKSt18basic_stringstreamIcSt11char_traitsIcESaIcEE3strEv",
    "_ZNKSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basewPK2tmcc",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE10_M_replaceEmmPKcm",
    "_ZNSt17moneypunct_bynameIwLb1EEC1EPKcm",
    "_ZNSt5ctypeIcEC2EPKtbm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEixEm",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE7getlineEPwl",
    "_ZNSt17moneypunct_bynameIcLb0EEC1EPKcm",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE5closeEv",
    "_ZNKSt7codecvtIDsc11__mbstate_tE16do_always_noconvEv",
    "_ZNSs6appendERKSsmm",
    "_ZStrsIwSt11char_traitsIwEERSt13basic_istreamIT_T0_ES6_PS3_",
    "_ZNSt14collate_bynameIcED0Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE11_M_disjunctEPKw",
    "_ZNKSbIwSt11char_traitsIwESaIwEE3endEv",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEEC1EOS3_",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC1Ev",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRPv",
    "_ZNSt16invalid_argumentC1ERKSs",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_RKS2_",
    "_ZNKSt10moneypunctIwLb1EE16do_negative_signEv",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEE3strERKNS_12basic_stringIwS2_S3_EE",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE3getEPwlw",
    "_ZNKSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE10_M_extractILb0EEES4_S4_S4_RSt8ios_baseRSt12_Ios_IostateRNS_12basic_stringIcS2_IcESaIcEEE",
    "_ZNKSt7codecvtIcc11__mbstate_tE6do_outERS0_PKcS4_RS4_PcS6_RS6_",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSt7collateIcEC2EP15__locale_structm",
    "_ZGTtNSt16invalid_argumentD0Ev",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEED1Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE9sputbackcEc",
    "_ZN11__gnu_debug25_Safe_local_iterator_base9_M_attachEPNS_19_Safe_sequence_baseEb",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12find_last_ofERKS4_m",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE4moveEOS2_",
    "_ZNSs6insertEmPKcm",
    "_ZSt9use_facetINSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEEERKT_RKSt6locale",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E",
    "_ZNSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZThn16_NSt13basic_fstreamIcSt11char_traitsIcEED0Ev",
    "_ZdaPvSt11align_val_t",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4backEv",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEEC2EP8_IO_FILE",
    "_ZNKSt10bad_typeid4whatEv",
    "_ZNSspLEc",
    "_ZNSt12future_errorD0Ev",
    "_ZNKSt10moneypunctIcLb0EE13do_pos_formatEv",
    "_ZNSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC1Em",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7compareEmmRKS4_",
    "_ZNSt15underflow_errorC1EPKc",
    "_ZNSt8messagesIwEC2EP15__locale_structPKcm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE9pbackfailEj",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEEC1ERKNS_12basic_stringIcS2_S3_EESt13_Ios_Openmode",
    "_ZNSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNSt12strstreambufC2EPclS0_",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE8max_sizeEv",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEED1Ev",
    "_ZNSt6__norm15_List_node_base7reverseEv",
    "_ZStrsIwSt11char_traitsIwEERSt13basic_istreamIT_T0_ES6_St12_Setiosflags",
    "_ZNKSbIwSt11char_traitsIwESaIwEE16find_last_not_ofEwm",
    "_ZSt9use_facetISt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEERKT_RKSt6locale",
    "_ZNSt12strstreambuf8overflowEi",
    "_ZN9__gnu_cxx9free_list8_M_clearEv",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE13positive_signEv",
    "_ZNKSt8ios_base7failureB5cxx114whatEv",
    "_ZNSt14collate_bynameIwED1Ev",
    "_ZNSaIcEC1ERKS_",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5rfindEPKcm",
    "_ZNSt10moneypunctIcLb1EEC1EP15__locale_structPKcm",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE8overflowEj",
    "_ZNSbIwSt11char_traitsIwESaIwEE6assignERKS2_",
    "_ZNKSi6gcountEv",
    "_ZNKSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE9_M_insertILb1EEES3_S3_RSt8ios_basewRKSbIwS2_SaIwEE",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE17_M_stringbuf_initESt13_Ios_Openmode",
    "_ZSt9use_facetISt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEERKT_RKSt6locale",
    "_ZNKSt10moneypunctIcLb1EE14do_frac_digitsEv",
    "_ZNKSs4_Rep12_M_is_leakedEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendESt16initializer_listIcE",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEEC2ESt13_Ios_Openmode",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE5pbumpEi",
    "_ZNKSt7__cxx117collateIcE12_M_transformEPcPKcm",
    "_ZNSt10ostrstreamC2EPciSt13_Ios_Openmode",
    "_ZNKSt7__cxx117collateIcE4hashEPKcS3_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE10_S_compareEmm",
    "_ZNSt11__timepunctIwED2Ev",
    "_ZNKSt3_V214error_category10equivalentEiRKSt15error_condition",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEEC2EOS3_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE9_M_appendEPKcm",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2IN9__gnu_cxx17__normal_iteratorIPwS2_EEEET_S8_RKS1_",
    "_ZNSiC1Ev",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE14__xfer_bufptrsC2ERKS4_PS4_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEEaSEOS2_",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE11do_groupingEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep12_S_empty_repEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE13shrink_to_fitEv",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEC2Ev",
    "_ZNSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZSt9use_facetISt7codecvtIcc11__mbstate_tEERKT_RKSt6locale",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5beginEv",
    "_ZNSt11regex_errorC1ENSt15regex_constants10error_typeE",
    "_ZN9__gnu_cxx6__poolILb1EE21_M_destroy_thread_keyEPv",
    "_ZSt9has_facetISt8messagesIcEEbRKSt6locale",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEEC1ERKSbIwS1_S2_ESt13_Ios_Openmode",
    "_ZTv0_n24_NSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNSt7codecvtIDic11__mbstate_tED1Ev",
    "_ZNSt7__cxx1110moneypunctIcLb0EE24_M_initialize_moneypunctEP15__locale_structPKc",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEE5closeEv",
    "_ZGTtNSt15underflow_errorD1Ev",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEED0Ev",
    "_ZNKSt5ctypeIwE9do_narrowEwc",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6lengthEv",
    "_ZNSt12__basic_fileIcE7seekoffElSt12_Ios_Seekdir",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEEC2Ev",
    "_ZNKSs8capacityEv",
    "_ZSt9use_facetISt8numpunctIwEERKT_RKSt6locale",
    "_ZSt15future_categoryv",
    "_ZNSt10istrstreamC2EPc",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE16do_thousands_sepEv",
    "_ZNSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC2Em",
    "_ZNSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE14do_curr_symbolEv",
    "_ZNKSt7codecvtIDsc11__mbstate_tE13do_max_lengthEv",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEEC2Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2ERKS3_",
    "_ZNSbIwSt11char_traitsIwESaIwEE12_S_constructEmwRKS1_",
    "_ZSt21__throw_bad_exceptionv",
    "_ZGTtNSt13runtime_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSs4backEv",
    "_ZNKSt11__timepunctIwE7_M_daysEPPKw",
    "_ZGTtNSt12out_of_rangeD2Ev",
    "_ZNSt7__cxx118numpunctIcEC1EPSt16__numpunct_cacheIcEm",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE10exceptionsEv",
    "_ZTv0_n24_NSt14basic_ofstreamIwSt11char_traitsIwEED1Ev",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEE5closeEv",
    "_ZNKSt7__cxx118messagesIcE7do_openERKNS_12basic_stringIcSt11char_traitsIcESaIcEEERKSt6locale",
    "_ZSt9has_facetISt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEEbRKSt6locale",
    "_ZNSt7__cxx118messagesIcED2Ev",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEEntEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE12_M_leak_hardEv",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEEC2ESt13_Ios_Openmode",
    "_ZNSt7__cxx1115messages_bynameIwEC2ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_Znwm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2EPKcRKS3_",
    "_ZNSt7__cxx118messagesIwED1Ev",
    "_ZNSt11__timepunctIwEC2EP15__locale_structPKcm",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE17_M_stringbuf_initESt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12_M_constructIN9__gnu_cxx17__normal_iteratorIPKcS4_EEEEvT_SB_St20forward_iterator_tag",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDsE11do_encodingEv",
    "__cxa_thread_atexit",
    "_ZStrsIwSt11char_traitsIwESaIwEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE",
    "_ZStplIcSt11char_traitsIcESaIcEESbIT_T0_T1_ERKS6_S8_",
    "_ZSt21__copy_streambufs_eofIcSt11char_traitsIcEElPSt15basic_streambufIT_T0_ES6_Rb",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6appendEPKw",
    "_ZNSt18condition_variableC1Ev",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC2EPKcSt13_Ios_Openmode",
    "_ZNSt15underflow_errorC1ERKSs",
    "_ZNSt7__cxx1110moneypunctIcLb0EED1Ev",
    "_ZNSt10_Sp_lockerC2EPKv",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14_M_extract_intIxEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNKSt6locale2id5_M_idEv",
    "_ZNSt15numpunct_bynameIcEC1ERKSsm",
    "_ZSt20__throw_system_errori",
    "_ZNSt15underflow_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZN10__cxxabiv116__enum_type_infoD1Ev",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEED2Ev",
    "_ZGTtNSt11range_errorD1Ev",
    "_ZNKSt7__cxx118numpunctIcE13decimal_pointEv",
    "_ZN9__gnu_cxx12__atomic_addEPVii",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2EPKwRKS3_",
    "_ZNSt12strstreambuf7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNKSs5crendEv",
    "_ZNK11__gnu_debug19_Safe_iterator_base14_M_can_compareERKS0_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEmmRKS4_mm",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE9underflowEv",
    "_ZNSs7_M_copyEPcPKcm",
    "_ZNSs7_M_copyEPcPKcm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertEmRKS4_mm",
    "_ZNSs4_Rep10_M_refcopyEv",
    "_ZN10__cxxabiv119__pointer_type_infoD0Ev",
    "_ZSt9use_facetINSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEEERKT_RKSt6locale",
    "_ZNSt15time_put_bynameIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4findEPKwmm",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb1EEC1EPKcm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2ERKS4_",
    "_ZNKSt11__timepunctIcE20_M_date_time_formatsEPPKc",
    "_ZNSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC1Em",
    "_ZNSoC1EPSt15basic_streambufIcSt11char_traitsIcEE",
    "_ZSt9use_facetISt10moneypunctIcLb1EEERKT_RKSt6locale",
    "_ZNSt9__cxx199815_List_node_base4hookEPS0_",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb0EEC1EPKcm",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC2EPKcSt13_Ios_Openmode",
    "__cxa_rethrow",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEEC2ERKSsSt13_Ios_Openmode",
    "_ZNSt10istrstreamC2EPKcl",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEED1Ev",
    "_ZNSt13runtime_erroraSERKS_",
    "_ZNSt12__basic_fileIcED1Ev",
    "_ZNKSt7__cxx118numpunctIcE13thousands_sepEv",
    "_ZNKSt10moneypunctIwLb1EE10neg_formatEv",
    "_ZNSt17moneypunct_bynameIcLb0EEC2ERKSsm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE17find_first_not_ofERKS2_m",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5eraseEN9__gnu_cxx17__normal_iteratorIPcS4_EES8_",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1IN9__gnu_cxx17__normal_iteratorIPwS2_EEEET_S8_RKS1_",
    "_ZThn16_NSt14basic_iostreamIwSt11char_traitsIwEED1Ev",
    "_ZNSt13__future_base12_Result_baseC2Ev",
    "_ZNSt6locale5facetD1Ev",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1EOS2_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12_Alloc_hiderC1EPwRKS3_",
    "_ZNSt7__cxx118numpunctIcEC1Em",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE5seekpESt4fposI11__mbstate_tE",
    "_ZNSt7__cxx117collateIcEC2EP15__locale_structm",
    "_ZNSt7__cxx1110moneypunctIcLb1EED2Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEmmmw",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6rbeginEv",
    "_ZNSs4_Rep10_M_destroyERKSaIcE",
    "_ZNSi6ignoreEl",
    "_ZNSi6ignoreEl",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEpLERKS4_",
    "_ZNSt13__future_base19_Async_state_commonD0Ev",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEEC2EOS4_",
    "_ZNSt8__detail15_List_node_base9_M_unhookEv",
    "_ZSt9has_facetISt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEEbRKSt6locale",
    "_ZNSt9basic_iosIwSt11char_traitsIwEEC1EPSt15basic_streambufIwS1_E",
    "_ZNSt14overflow_errorD2Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE16find_last_not_ofEPKcmm",
    "_ZTv0_n24_NSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSsC2ERKSs",
    "_ZNSt15time_put_bynameIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC2EPKcm",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEEC2ERKNS_12basic_stringIwS2_S3_EESt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12_Alloc_hiderC2EPwRKS3_",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEC2ERKSsSt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertEN9__gnu_cxx17__normal_iteratorIPcS4_EEmc",
    "_ZNSt16__numpunct_cacheIwE8_M_cacheERKSt6locale",
    "_ZThn16_NSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNSi6ignoreEv",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRb",
    "_ZN9__gnu_cxx9__freeresEv",
    "_ZNSi6ignoreEv",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEEC2ERKNS_12basic_stringIwS2_S3_EESt13_Ios_Openmode",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRd",
    "_ZNKSt7__cxx118messagesIwE3getEiiiRKNS_12basic_stringIwSt11char_traitsIwESaIwEEE",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEE3strERKNS_12basic_stringIcS2_S3_EE",
    "_ZNSt17__timepunct_cacheIcEC1Em",
    "_ZNSt17moneypunct_bynameIcLb1EED0Ev",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRe",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRf",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE7sungetcEv",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE8get_timeES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt8ios_base7failureD1Ev",
    "_ZNSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode",
    "_ZNKSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE9_M_insertILb1EEES4_S4_RSt8ios_basewRKNS_12basic_stringIwS3_SaIwEEE",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRj",
    "_ZNSt12strstreambufD0Ev",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE4openERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRl",
    "_ZNKSt10moneypunctIwLb0EE16do_negative_signEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE6insertEmPKw",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRm",
    "_ZNSt15time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC1EPKcm",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEED0Ev",
    "_ZNSt14collate_bynameIcED2Ev",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE13positive_signEv",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEEC1EOS4_",
    "_ZSt7getlineIwSt11char_traitsIwESaIwEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_ES4_",
    "_ZNSt15time_put_bynameIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC2EPKcm",
    "_ZNKSt13bad_exception4whatEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4rendEv",
    "_ZNSt7__cxx118numpunctIwEC2Em",
    "_ZNSt11range_errorD1Ev",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEEC1EP8_IO_FILE",
    "_ZNKSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE10_M_extractILb0EEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRSs",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRt",
    "_ZNKSt17bad_function_call4whatEv",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEEC2EOS2_",
    "_ZNSt7__cxx1115numpunct_bynameIcEC1EPKcm",
    "_ZNKSt10moneypunctIwLb0EE10neg_formatEv",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRx",
    "_ZNSt8numpunctIwEC2EP15__locale_structm",
    "_ZGTtNSt16invalid_argumentD2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6resizeEm",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRy",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEED2Ev",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEEC2ESt13_Ios_Openmode",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE16do_decimal_pointEv",
    "_ZNSt8ios_base15sync_with_stdioEb",
    "_ZNSs4_Rep26_M_set_length_and_sharableEm",
    "_ZNSs4_Rep26_M_set_length_and_sharableEm",
    "_ZNKSt8messagesIcE6do_getEiiiRKSs",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE11_M_setstateESt12_Ios_Iostate",
    "_ZNSt12domain_errorC2EPKc",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEaSEOS2_",
    "_ZNSs6appendEPKc",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE13_M_insert_intIlEES3_S3_RSt8ios_basecT_",
    "_ZNKSs5emptyEv",
    "_ZNSt12future_errorD2Ev",
    "_ZNSs13_S_copy_charsEPcN9__gnu_cxx17__normal_iteratorIPKcSsEES4_",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE4syncEv",
    "_ZSt19__throw_ios_failurePKc",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE4swapERS2_",
    "_ZNKSt7__cxx118numpunctIcE12do_falsenameEv",
    "_ZNSt15messages_bynameIcEC1ERKSsm",
    "_ZNSt15_List_node_base9_M_unhookEv",
    "_ZNKSt7__cxx118numpunctIwE8groupingEv",
    "_ZNKSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES4_bRSt8ios_basece",
    "_ZNSt12length_errorC1EPKc",
    "_ZNKSt7__cxx118messagesIcE4openERKNS_12basic_stringIcSt11char_traitsIcESaIcEEERKSt6locale",
    "_ZNKSt15basic_streambufIwSt11char_traitsIwEE5ebackEv",
    "_ZNSt16__numpunct_cacheIcEC1Em",
    "_ZNK11__gnu_debug16_Error_formatter17_M_get_max_lengthEv",
    "_ZNSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE8overflowEj",
    "_ZNSt10moneypunctIwLb0EEC1Em",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14_M_extract_intIjEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEEC1EOS3_",
    "_ZN11__gnu_debug19_Safe_sequence_base22_M_revalidate_singularEv",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE8setstateESt12_Ios_Iostate",
    "_ZNSt6chrono3_V212system_clock3nowEv",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEE3strERKSbIwS1_S2_E",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE16do_decimal_pointEv",
    "_ZSt20_Rb_tree_rotate_leftPSt18_Rb_tree_node_baseRS0_",
    "_ZSt9has_facetINSt7__cxx118messagesIcEEEbRKSt6locale",
    "_ZNSt6chrono12system_clock3nowEv",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE11_M_setstateESt12_Ios_Iostate",
    "_ZNSoC2ERSd",
    "_ZNKSbIwSt11char_traitsIwESaIwEE7compareEPKw",
    "_ZNSsC2ERKSsmmRKSaIcE",
    "_ZNSt7__cxx1115messages_bynameIcEC2EPKcm",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDiE16do_always_noconvEv",
    "_ZNSt11range_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tmcc",
    "_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC1Em",
    "__cxa_throw",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE8capacityEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractItEERS2_RT_",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE9pbackfailEi",
    "_ZNSiD0Ev",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE10date_orderEv",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE5sgetnEPwl",
    "__cxa_current_exception_type",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEEC1EPSt15basic_streambufIwS1_E",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEED1Ev",
    "_ZNSs2atEm",
    "_ZNSt7__cxx118numpunctIwEC2EP15__locale_structm",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE5imbueERKSt6locale",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE4swapERS2_",
    "_ZNSs6assignEPKc",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE4goodEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1ERKS4_mm",
    "_ZNSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE8readsomeEPwl",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5eraseEmm",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC2EOS2_",
    "_ZNKSt7codecvtIDsc11__mbstate_tE10do_unshiftERS0_PcS3_RS3_",
    "_ZNSt12__basic_fileIcEC2EP15pthread_mutex_t",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE11_M_capacityEm",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE16do_thousands_sepEv",
    "_ZNSt12strstreambufC1EPFPvmEPFvS0_E",
    "_ZNSt9basic_iosIcSt11char_traitsIcEEC2EPSt15basic_streambufIcS1_E",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE4swapERS2_",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEED2Ev",
    "_ZNK11__gnu_debug16_Error_formatter10_Parameter14_M_print_fieldEPKS0_PKc",
    "_ZNSt12strstreambufC1EPalS0_",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEED1Ev",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb1EED0Ev",
    "_ZSt9has_facetINSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEEEbRKSt6locale",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEE4swapERS2_",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEE5closeEv",
    "_ZNSt9__cxx199815_List_node_base6unhookEv",
    "_ZNSt6localeC1Ev",
    "_ZNSt12ctype_bynameIwEC2EPKcm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13_M_set_lengthEm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEaSESt16initializer_listIcE",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEED1Ev",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE14do_frac_digitsEv",
    "_ZNSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZNSt10moneypunctIwLb1EEC2Em",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE14_M_extract_intIxEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNSbIwSt11char_traitsIwESaIwEE6assignERKS2_mm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEixEm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2ERKS4_mm",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basecb",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE16find_last_not_ofEwm",
    "_ZSt9use_facetINSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEEERKT_RKSt6locale",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_mw",
    "_ZNKSbIwSt11char_traitsIwESaIwEE16find_last_not_ofEPKwmm",
    "_ZNSt16bad_array_lengthD0Ev",
    "_ZNSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basecd",
    "_ZNSt6localeC1ERKS_S1_i",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basece",
    "_ZNKSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_bRSt8ios_baseRSt12_Ios_IostateRe",
    "_ZNK11__gnu_debug16_Error_formatter15_M_print_stringEPKc",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_refdataEv",
    "_ZNSt12length_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE11do_groupingEv",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4cendEv",
    "_ZNKSt8messagesIcE18_M_convert_to_charERKSs",
    "_ZNK10__cxxabiv117__class_type_info11__do_upcastEPKS0_PKvRNS0_15__upcast_resultE",
    "_ZNSt10_Sp_lockerD2Ev",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basecl",
    "_ZNK10__cxxabiv117__class_type_info20__do_find_public_srcElPKvPKS0_S2_",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basecm",
    "_ZGTtNSt16invalid_argumentC2EPKc",
    "_ZNSt13random_device7_M_initERKSs",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEaSEOS3_",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEEC1Ev",
    "_ZNSt14codecvt_bynameIwc11__mbstate_tEC2EPKcm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6assignEPKw",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE11do_get_yearES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb1EEC1ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt5ctypeIcEC2EP15__locale_structPKtbm",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEE4swapERS2_",
    "_ZNKSt8messagesIwE5closeEi",
    "_ZNSt6thread20hardware_concurrencyEv",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE14do_curr_symbolEv",
    "_ZNKSt13random_device13_M_getentropyEv",
    "_ZNSt15__exception_ptr13exception_ptrC1EMS0_FvvE",
    "_ZSt9use_facetINSt7__cxx118messagesIwEEERKT_RKSt6locale",
    "_ZNKSt7codecvtIDsc11__mbstate_tE6do_outERS0_PKDsS4_RS4_PcS6_RS6_",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basecx",
    "_ZNKSs12find_last_ofEPKcmm",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basecy",
    "_ZNSbIwSt11char_traitsIwESaIwEE5eraseEN9__gnu_cxx17__normal_iteratorIPwS2_EE",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRb",
    "_ZNSt6localeC2EPKc",
    "_ZNKSt11__timepunctIcE7_M_daysEPPKc",
    "_ZNKSt18basic_stringstreamIwSt11char_traitsIwESaIwEE5rdbufEv",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE3endEv",
    "_ZNSt15time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC1EPKcm",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRd",
    "_ZNKSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE9_M_insertILb0EEES4_S4_RSt8ios_basewRKNS_12basic_stringIwS3_SaIwEEE",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE13_M_insert_intImEES3_S3_RSt8ios_basewT_",
    "_ZNSt16invalid_argumentC2EPKc",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6rbeginEv",
    "_ZNSt16__numpunct_cacheIcE8_M_cacheERKSt6locale",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRe",
    "_ZN10__cxxabiv119__pointer_type_infoD2Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12find_last_ofEPKwmm",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRf",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE5imbueERKSt6locale",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE3tieEPSt13basic_ostreamIwS1_E",
    "_ZNSt17moneypunct_bynameIcLb0EED1Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE6assignEPKwm",
    "_ZNKSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES4_bRSt8ios_basece",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEEC1ERKSsSt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPKwS4_EES9_S9_S9_",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE5pbumpEi",
    "_ZNSt9basic_iosIcSt11char_traitsIcEEC2Ev",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRj",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRl",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE8setstateESt12_Ios_Iostate",
    "_ZSt9has_facetISt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEEbRKSt6locale",
    "_ZNKSbIwSt11char_traitsIwESaIwEE17find_first_not_ofEPKwm",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRm",
    "_ZNSt9strstreamC2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPcS4_EES8_NS6_IPKcS4_EESB_",
    "_ZNSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12find_last_ofERKS4_m",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS4_EES8_mw",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_RSt8ios_basewb",
    "_ZNSt17moneypunct_bynameIwLb1EEC1ERKSsm",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE4setpEPcS3_",
    "_ZNKSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_bRSt8ios_basewRKSbIwS2_SaIwEE",
    "_ZNSt13__future_base12_Result_baseD1Ev",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_RSt8ios_basewd",
    "_ZNSt15numpunct_bynameIwEC1EPKcm",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRt",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPKcS4_EES9_S8_",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_RSt8ios_basewe",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12_Alloc_hiderC2EPcOS3_",
    "_ZNKSt8messagesIwE20_M_convert_from_charEPc",
    "_ZNKSt7collateIwE9transformEPKwS2_",
    "_ZNKSt5ctypeIwE10do_tolowerEw",
    "_ZNSt10istrstreamC1EPcl",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE9underflowEv",
    "_ZNSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC2Em",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE14_M_replace_auxEmmmc",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC2ERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
    "_ZNKSt7__cxx117collateIcE9transformEPKcS3_",
    "_ZNKSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEE5rdbufEv",
    "_ZNKSt10moneypunctIcLb1EE13negative_signEv",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRx",
    "_ZNSt12out_of_rangeC2ERKSs",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRSt12_Ios_IostateRy",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE10_M_destroyEm",
    "_ZNKSt10moneypunctIwLb1EE13negative_signEv",
    "_ZNSt16invalid_argumentC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE4openERKSsSt13_Ios_Openmode",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC2ERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
    "_ZNKSt7__cxx119money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES4_bRSt8ios_basecRKNS_12basic_stringIcS3_SaIcEEE",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE16_M_get_allocatorEv",
    "_ZNSt9__cxx199815_List_node_base4swapERS0_S1_",
    "_ZNKSt10moneypunctIcLb1EE16do_positive_signEv",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_RSt8ios_basewl",
    "_ZNKSt5ctypeIwE9do_narrowEPKwS2_cPc",
    "_ZNSt13__future_base19_Async_state_commonD2Ev",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEC1ERKSsSt13_Ios_Openmode",
    "_ZNSt6locale6globalERKS_",
    "_ZNSt7__cxx118numpunctIcED0Ev",
    "_ZNKSt15__exception_ptr13exception_ptrntEv",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_RSt8ios_basewm",
    "_ZN10__cxxabiv117__class_type_infoD1Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE6insertEN9__gnu_cxx17__normal_iteratorIPwS2_EESt16initializer_listIwE",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE5writeEPKwl",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_destroyERKS1_",
    "_ZGTtNSt14overflow_errorD0Ev",
    "_ZNSdC2EPSt15basic_streambufIcSt11char_traitsIcEE",
    "_ZNKSs13find_first_ofEPKcmm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE5rfindEPKwm",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE17find_first_not_ofEPKwmm",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2IPKwEET_S6_RKS1_",
    "_ZNKSbIwSt11char_traitsIwESaIwEE8_M_limitEmm",
    "_ZNKSt7codecvtIDic11__mbstate_tE16do_always_noconvEv",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE21_M_extract_via_formatES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tmPKw",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5rfindEwm",
    "_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_S2_S2_",
    "_ZNKSt7__cxx117collateIcE7do_hashEPKcS3_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEpLESt16initializer_listIcE",
    "_ZNKSt7__cxx117collateIwE7do_hashEPKwS3_",
    "_ZNSolsEPFRSt8ios_baseS0_E",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEE4swapERS4_",
    "_ZNSt17moneypunct_bynameIcLb1EED2Ev",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_RSt8ios_basewx",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES3_RSt8ios_basewy",
    "_ZNKSt19__codecvt_utf8_baseIDiE6do_outER11__mbstate_tPKDiS4_RS4_PcS6_RS6_",
    "_ZNSt17__timepunct_cacheIwEC2Em",
    "_ZNSt12ctype_bynameIcEC1ERKSsm",
    "_ZNSt20__codecvt_utf16_baseIwED0Ev",
    "_ZNSt12strstreambufD2Ev",
    "_ZNKSt7collateIcE12_M_transformEPcPKcm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE13find_first_ofEwm",
    "_ZNSsC1IN9__gnu_cxx17__normal_iteratorIPcSsEEEET_S4_RKSaIcE",
    "_ZNSt17__timepunct_cacheIcED0Ev",
    "_ZNKSt15basic_streambufIwSt11char_traitsIwEE4gptrEv",
    "_ZNKSbIwSt11char_traitsIwESaIwEE11_M_disjunctEPKw",
    "_ZNKSbIwSt11char_traitsIwESaIwEE11_M_disjunctEPKw",
    "_ZNSt18__moneypunct_cacheIwLb1EEC1Em",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6appendEmw",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEED2Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE7pubsyncEv",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE14__xfer_bufptrsC2ERKS4_PS4_",
    "_ZNSt15messages_bynameIwEC2EPKcm",
    "_ZNSt8ios_base7failureB5cxx11C1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKSt10error_code",
    "_ZGTtNSt15underflow_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt8messagesIwE18_M_convert_to_charERKSbIwSt11char_traitsIwESaIwEE",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEEC1ERKSbIwS1_S2_ESt13_Ios_Openmode",
    "_ZNSt8messagesIcEC1EP15__locale_structPKcm",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEEC2ERKNS_12basic_stringIcS2_S3_EESt13_Ios_Openmode",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2EPKwmRKS1_",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE19_M_terminate_outputEv",
    "_ZNSt6localeaSERKS_",
    "_ZNSt7__cxx118messagesIcEC1EP15__locale_structPKcm",
    "_ZGTtNSt12length_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base",
    "_ZNSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC1Em",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEC2Ev",
    "_ZNSt7__cxx118numpunctIwED1Ev",
    "_ZNSt12ctype_bynameIcED0Ev",
    "_ZNKSt12__basic_fileIcE7is_openEv",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEEC1ERKSbIwS1_S2_ESt13_Ios_Openmode",
    "_ZNKSt10moneypunctIwLb0EE11do_groupingEv",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNKSbIwSt11char_traitsIwESaIwEE13find_first_ofEPKwmm",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12_M_constructIN9__gnu_cxx17__normal_iteratorIPwS4_EEEEvT_SA_St20forward_iterator_tag",
    "_ZNKSs12find_last_ofEcm",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEEaSEOS4_",
    "_ZNKSt10moneypunctIwLb1EE10pos_formatEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13_S_copy_charsEPwPKwS7_",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE9pbackfailEi",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE3strERKSs",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE3eofEv",
    "_ZNSi3getERSt15basic_streambufIcSt11char_traitsIcEE",
    "_ZNSoC2Ev",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14_M_extract_intImEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNKSs7_M_iendEv",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE8overflowEi",
    "_ZSt20__throw_out_of_rangePKc",
    "_ZTv0_n24_NSt10istrstreamD1Ev",
    "_ZNKSt7__cxx118numpunctIwE8truenameEv",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb0EED1Ev",
    "_ZNKSt20__codecvt_utf16_baseIwE9do_lengthER11__mbstate_tPKcS4_m",
    "_ZNKSt20__codecvt_utf16_baseIwE5do_inER11__mbstate_tPKcS4_RS4_PwS6_RS6_",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1IPKwEET_S6_RKS1_",
    "_ZNKSs7compareEmmRKSsmm",
    "_ZNSt9__cxx199815_List_node_base11_M_transferEPS0_S1_",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE16find_last_not_ofEPKwm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertEN9__gnu_cxx17__normal_iteratorIPKcS4_EEc",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE19_M_terminate_outputEv",
    "_ZSt9use_facetISt11__timepunctIcEERKT_RKSt6locale",
    "_ZdaPvm",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4cendEv",
    "_ZNSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZNSt15_List_node_base6unhookEv",
    "_ZNSt7__cxx1115time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEEC2ERKS2_",
    "_ZNSt16__numpunct_cacheIwEC2Em",
    "_ZNSt25__codecvt_utf8_utf16_baseIwED0Ev",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode",
    "_ZNSt8numpunctIcEC2EP15__locale_structm",
    "_ZTv0_n24_NSiD1Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE7_M_dataEv",
    "_ZNSt16__numpunct_cacheIcED0Ev",
    "_ZNSs4_Rep7_M_grabERKSaIcES2_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS4_EES8_RKS4_",
    "_ZnamRKSt9nothrow_t",
    "_ZNSt10moneypunctIwLb0EED0Ev",
    "_ZNSt8ios_baseC2Ev",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE4initEPSt15basic_streambufIwS1_E",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEC2EOS2_",
    "_ZNSt15_List_node_base4swapERS_S0_",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEEC2ERKSbIwS1_S2_ESt13_Ios_Openmode",
    "_ZNKSt20__codecvt_utf16_baseIwE16do_always_noconvEv",
    "_ZNKSs7compareEmmRKSs",
    "_ZNSt25__codecvt_utf8_utf16_baseIDsED0Ev",
    "_ZNSs6resizeEm",
    "_ZNKSt7collateIwE10_M_compareEPKwS2_",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE14do_get_weekdayES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZN9__gnu_cxx6__poolILb1EE10_M_destroyEv",
    "_ZNSiC1EPSt15basic_streambufIcSt11char_traitsIcEE",
    "_ZNKSt10error_code23default_error_conditionEv",
    "_ZNSsC1ERKSsmmRKSaIcE",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE13decimal_pointEv",
    "_ZNSiD2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEmmPKcm",
    "_ZNKSt19__codecvt_utf8_baseIwE5do_inER11__mbstate_tPKcS4_RS4_PwS6_RS6_",
    "_ZGTtNSt12domain_errorC2EPKc",
    "_ZNSt18__moneypunct_cacheIwLb1EE8_M_cacheERKSt6locale",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE13decimal_pointEv",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNSt15_List_node_base11_M_transferEPS_S0_",
    "_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZN9__gnu_cxx6__poolILb0EE16_M_reclaim_blockEPcm",
    "_ZNSt15time_put_bynameIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tmPKwSD_",
    "_ZNSt6locale5_ImplC1Em",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEEC2ERKSbIwS1_S2_ESt13_Ios_Openmode",
    "_ZNSt14codecvt_bynameIcc11__mbstate_tEC2EPKcm",
    "_ZThn16_NSdD1Ev",
    "_ZNKSt11__timepunctIcE9_M_monthsEPPKc",
    "_ZNSt7__cxx1114collate_bynameIwEC2ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNKSt9bad_alloc4whatEv",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tmPKcSD_",
    "_ZNSt12__basic_fileIcE6xsgetnEPcl",
    "_ZNSt11__timepunctIcEC1EP15__locale_structPKcm",
    "_ZSt19__throw_logic_errorPKc",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5frontEv",
    "_ZNSt15__exception_ptr13exception_ptrC1Ev",
    "_ZThn16_NSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNKSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE10_M_extractILb0EEES4_S4_S4_RSt8ios_baseRSt12_Ios_IostateRNS_12basic_stringIcS3_SaIcEEE",
    "_ZNKSt8numpunctIcE8groupingEv",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE7_M_syncEPcmm",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE8_M_pbumpEPwS5_l",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7_M_dataEPc",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb1EED2Ev",
    "_ZNKSt10moneypunctIwLb0EE10pos_formatEv",
    "_ZNSt9strstream3strEv",
    "_ZNKSt7codecvtIwc11__mbstate_tE11do_encodingEv",
    "_ZNKSt5ctypeIcE8do_widenEc",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6cbeginEv",
    "_ZNSt13random_device7_M_initERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEEC1ESt13_Ios_Openmode",
    "_ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE",
    "_ZNSt13runtime_errorC2EPKc",
    "_ZNSt7__cxx1115messages_bynameIcED0Ev",
    "_ZNKSs13find_first_ofEcm",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE14_M_get_ext_posER11__mbstate_t",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE5widenEc",
    "_ZNKSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE24_M_extract_wday_or_monthES4_S4_RiPPKcmRSt8ios_baseRSt12_Ios_Iostate",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE13thousands_sepEv",
    "_ZN10__cxxabiv121__vmi_class_type_infoD1Ev",
    "_ZNSs5clearEv",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE13thousands_sepEv",
    "_ZNSt16bad_array_lengthD2Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE5rfindERKS2_m",
    "_ZNSs4_Rep10_M_refdataEv",
    "_ZNSt10moneypunctIwLb1EED1Ev",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEEC2ESt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1ERKS4_mmRKS3_",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE12__safe_pbumpEl",
    "_ZNSt7__cxx118numpunctIcEC2EP15__locale_structm",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4backEv",
    "_ZTv0_n24_NSt19basic_istringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNKSt10ostrstream5rdbufEv",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE15_M_update_egptrEv",
    "_ZNSt18basic_stringstreamIwSt11char_traitsIwESaIwEEaSEOS3_",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE11do_get_dateES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2ERKS2_mRKS1_",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE8groupingEv",
    "_ZNSt7__cxx1114collate_bynameIcED1Ev",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEEC2EOS4_ONS4_14__xfer_bufptrsE",
    "_ZNSt18condition_variableD2Ev",
    "_ZNSsC2ERKSsmm",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE4openERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractImEERS2_RT_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1IPKwvEET_S8_RKS3_",
    "_ZNKSt5ctypeIcE14_M_narrow_initEv",
    "_ZNSt17moneypunct_bynameIwLb1EEC2EPKcm",
    "_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC2Em",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEED0Ev",
    "_ZNSt7__cxx1114collate_bynameIwED0Ev",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE7_M_seekElSt12_Ios_Seekdir11__mbstate_t",
    "_ZNK10__cxxabiv117__pbase_type_info10__do_catchEPKSt9type_infoPPvj",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertEmPKwm",
    "_ZSt9has_facetINSt7__cxx117collateIcEEEbRKSt6locale",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractIyEERS2_RT_",
    "_ZNKSs4cendEv",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE13do_neg_formatEv",
    "_ZNKSt11__timepunctIcE8_M_am_pmEPPKc",
    "_ZNKSt15basic_streambufIwSt11char_traitsIwEE4pptrEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsEPFRS2_S3_E",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1IN9__gnu_cxx17__normal_iteratorIPwS4_EEvEET_SA_RKS3_",
    "_ZNKSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE5rdbufEv",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE6xsputnEPKcl",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE13do_neg_formatEv",
    "_ZNSt7__cxx1114collate_bynameIwEC1EPKcm",
    "_ZNSt13runtime_errorD1Ev",
    "_ZN9__gnu_cxx6__poolILb0EE10_M_destroyEv",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE8_M_pbumpEPcS5_l",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE14do_frac_digitsEv",
    "_ZNKSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_bRSt8ios_basece",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tmPKwSC_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2ERKS4_mRKS3_",
    "_ZNKSt5ctypeIwE10do_toupperEw",
    "_ZTv0_n24_NSt14basic_ifstreamIcSt11char_traitsIcEED1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6assignERKS4_mm",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEEC2EOS2_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1EOS4_",
    "_ZSt2wsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_",
    "_ZGTtNSt14overflow_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt13basic_filebufIcSt11char_traitsIcEE7is_openEv",
    "_ZNKSt7__cxx118messagesIwE5closeEi",
    "_ZNKSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE9_M_insertILb1EEES3_S3_RSt8ios_basecRKSs",
    "_ZNKSt20__codecvt_utf16_baseIwE6do_outER11__mbstate_tPKwS4_RS4_PcS6_RS6_",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEE3strERKSbIwS1_S2_E",
    "_ZNSt9basic_iosIcSt11char_traitsIcEED1Ev",
    "_ZNSt8messagesIcEC2Em",
    "_ZN11__gnu_debug19_Safe_iterator_base9_M_detachEv",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE9showmanycEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE6sentryD1Ev",
    "_ZNSt8messagesIwEC1Em",
    "_ZNKSt7codecvtIwc11__mbstate_tE9do_lengthERS0_PKcS4_m",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEEC2EOS4_",
    "_ZNSt9strstreamD1Ev",
    "_ZNSt8bad_castD1Ev",
    "_ZNSt15messages_bynameIcED0Ev",
    "_ZStlsIwSt11char_traitsIwESaIwEERSt13basic_ostreamIT_T0_ES7_RKNSt7__cxx1112basic_stringIS4_S5_T1_EE",
    "_ZNSt8ios_base7failureB5cxx11C2EPKcRKSt10error_code",
    "__cxa_guard_acquire",
    "_ZNSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZNSt12strstreambuf7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5beginEv",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE6setbufEPcl",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEE4openERKNSt7__cxx1112basic_stringIcS0_IcESaIcEEESt13_Ios_Openmode",
    "_ZNKSt10moneypunctIcLb0EE16do_positive_signEv",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEEcvPvEv",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEEC1EPSt15basic_streambufIwS1_E",
    "_ZGTtNSt12length_errorD1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6assignESt16initializer_listIwE",
    "__cxa_allocate_dependent_exception",
    "_ZNSt6locale5_ImplC1EPKcm",
    "_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5rfindERKS4_m",
    "_ZNKSt19__codecvt_utf8_baseIDiE11do_encodingEv",
    "_ZNSt10moneypunctIwLb0EE24_M_initialize_moneypunctEP15__locale_structPKc",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE4setgEPcS3_S3_",
    "_ZNKSt11__timepunctIwE9_M_monthsEPPKw",
    "_ZNSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZNSt7__cxx118numpunctIcED2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2ERKS3_",
    "_ZNKSt19basic_istringstreamIwSt11char_traitsIwESaIwEE5rdbufEv",
    "_ZNSs4_Rep12_S_empty_repEv",
    "_ZGTtNSt14overflow_errorD2Ev",
    "_ZNSt7__cxx1110moneypunctIwLb1EE24_M_initialize_moneypunctEP15__locale_structPKc",
    "_ZNKSt14basic_ifstreamIwSt11char_traitsIwEE7is_openEv",
    "_ZNKSt14basic_ifstreamIwSt11char_traitsIwEE7is_openEv",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE14_M_extract_intIjEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNSt15time_put_bynameIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC1ERKNSt7__cxx1112basic_stringIcS2_SaIcEEEm",
    "_ZNKSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES4_S4_bRSt8ios_baseRSt12_Ios_IostateRNS_12basic_stringIcS3_SaIcEEE",
    "_ZNKSt15basic_stringbufIwSt11char_traitsIwESaIwEE3strEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEEC2EOS2_",
    "_ZNKSt7__cxx118messagesIcE3getEiiiRKNS_12basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSirsEPFRSt8ios_baseS0_E",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEb",
    "_ZNKSt16bad_array_length4whatEv",
    "_ZNSt7__cxx1110moneypunctIcLb1EE24_M_initialize_moneypunctEP15__locale_structPKc",
    "_ZNKSt4hashIRKSbIwSt11char_traitsIwESaIwEEEclES5_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEd",
    "_ZNKSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEE3strEv",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE8get_timeES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_PS3_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEe",
    "_ZNSt20__codecvt_utf16_baseIwED2Ev",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEf",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEi",
    "_ZNSt17__timepunct_cacheIcED2Ev",
    "_ZNSt16nested_exceptionD1Ev",
    "_ZStrsIwSt11char_traitsIwEERSt13basic_istreamIT_T0_ES6_RS3_",
    "_ZSt13set_terminatePFvvE",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEj",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEE9underflowEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE10_M_destroyEm",
    "_ZNSt10ostrstreamC2Ev",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEE6xsputnEPKcl",
    "_ZNKSt19__codecvt_utf8_baseIDsE11do_encodingEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2EOS4_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEl",
    "_ZNSt17__timepunct_cacheIwED1Ev",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEm",
    "_ZNSsC2Ev",
    "_ZNSdC1Ev",
    "_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_St8_SetfillIS3_E",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5c_strEv",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE11do_get_yearES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEaSEOS2_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE8pop_backEv",
    "_ZGTtNSt12domain_errorD1Ev",
    "_ZNKSs12find_last_ofERKSsm",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDsE9do_lengthER11__mbstate_tPKcS4_m",
    "_ZNSt18__moneypunct_cacheIwLb1EED0Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12_Alloc_hiderC1EPwOS3_",
    "_ZNKSt19__codecvt_utf8_baseIDsE9do_lengthER11__mbstate_tPKcS4_m",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEED1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS4_EES8_PKw",
    "_ZTv0_n24_NSoD0Ev",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEs",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEt",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNSs13_S_copy_charsEPcS_S_",
    "_ZNSt15time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEaSEPKw",
    "_ZNSt6locale5_Impl21_M_replace_categoriesEPKS0_i",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE8_M_writeEPKwl",
    "_ZSt24__throw_invalid_argumentPKc",
    "_ZNKSt7__cxx118messagesIwE8do_closeEi",
    "_ZNK10__cxxabiv121__vmi_class_type_info11__do_upcastEPKNS_17__class_type_infoEPKvRNS1_15__upcast_resultE",
    "_ZNSt12ctype_bynameIcED2Ev",
    "_ZN10__cxxabiv117__pbase_type_infoD0Ev",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEx",
    "_ZNSt8ios_base7failureB5cxx11D0Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6assignEOS4_",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE15_M_insert_floatIeEES3_S3_RSt8ios_baseccT_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEy",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2ERKS4_",
    "_ZNSt12ctype_bynameIwED1Ev",
    "_ZNSt9__atomic011atomic_flag12test_and_setESt12memory_order",
    "_ZNKSt19__codecvt_utf8_baseIwE6do_outER11__mbstate_tPKwS4_RS4_PcS6_RS6_",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7compareEmmPKw",
    "_ZNSs4_Rep13_M_set_leakedEv",
    "_ZNSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE10_S_compareEmm",
    "_ZThn16_NSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZNKSt25__codecvt_utf8_utf16_baseIwE9do_lengthER11__mbstate_tPKcS4_m",
    "_ZNSt28__atomic_futex_unsigned_base19_M_futex_notify_allEPj",
    "_ZNSt7collateIcEC1Em",
    "_ZNSoD1Ev",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE6xsgetnEPcl",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5rfindERKS4_m",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE3getERSt15basic_streambufIwS1_Ew",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEE6sentryC2ERS2_",
    "_ZTv0_n24_NSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_St8_Setbase",
    "_ZSt2wsIwSt11char_traitsIwEERSt13basic_istreamIT_T0_ES6_",
    "_ZNKSs7compareEmmPKcm",
    "_ZSt9has_facetISt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEEbRKSt6locale",
    "_ZNSt19__codecvt_utf8_baseIDsED1Ev",
    "__dynamic_cast",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE10_M_disposeEv",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1ERKS1_",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE10date_orderEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2ERKS4_RKS3_",
    "_ZNSt12strstreambuf9underflowEv",
    "_ZGTtNSt12length_errorC1EPKc",
    "_ZTv0_n24_NSt13basic_fstreamIcSt11char_traitsIcEED1Ev",
    "_ZNSt25__codecvt_utf8_utf16_baseIwED2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE9_M_lengthEm",
    "_ZNSt18condition_variable10notify_allEv",
    "_ZStrsIfwSt11char_traitsIwEERSt13basic_istreamIT0_T1_ES6_RSt7complexIT_E",
    "_ZNSt11logic_errorC1ERKS_",
    "_ZNKSt10moneypunctIwLb1EE11do_groupingEv",
    "_ZNSt12ctype_bynameIcEC1EPKcm",
    "_ZNSt10ostrstream3strEv",
    "_ZNKSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES4_S4_bRSt8ios_baseRSt12_Ios_IostateRe",
    "_ZNSt16__numpunct_cacheIcED2Ev",
    "_ZNKSt7collateIwE10do_compareEPKwS2_S2_S2_",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE5rdbufEPSt15basic_streambufIcS1_E",
    "_ZNSt8ios_baseD1Ev",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE4goodEv",
    "_ZNKSt25__codecvt_utf8_utf16_baseIwE10do_unshiftER11__mbstate_tPcS3_RS3_",
    "_ZNKSs17find_first_not_ofEPKcmm",
    "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKa",
    "_ZNSt10moneypunctIwLb0EED2Ev",
    "_ZNSsC1ERKSaIcE",
    "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc",
    "_ZNSt16__numpunct_cacheIwED1Ev",
    "_ZNKSs13find_first_ofERKSsm",
    "_ZNSt7__cxx1110moneypunctIwLb0EEC2EP15__locale_structPKcm",
    "_ZSt9has_facetISt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEEbRKSt6locale",
    "_ZNSolsEPSt15basic_streambufIcSt11char_traitsIcEE",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE5imbueERKSt6locale",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE14__xfer_bufptrsD1Ev",
    "_ZNSt8numpunctIcEC2EPSt16__numpunct_cacheIcEm",
    "_ZNSt25__codecvt_utf8_utf16_baseIDsED2Ev",
    "_ZN10__gnu_norm15_List_node_base6unhookEv",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE13get_monthnameES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKh",
    "_ZSt9use_facetINSt7__cxx1110moneypunctIwLb1EEEERKT_RKSt6locale",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEEC1ESt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEaSERKS4_",
    "_ZNSt15time_put_bynameIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC1ERKSsm",
    "_ZNSt14overflow_errorC1ERKSs",
    "_ZNSt22condition_variable_anyC1Ev",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode",
    "_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED2Ev",
    "_ZNKSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEE3strEv",
    "_ZNSt7collateIwEC2Em",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE6snextcEv",
    "_ZNSt15time_put_bynameIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4findEPKcm",
    "_ZNSsC2ERKSaIcE",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE16do_decimal_pointEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE3getEv",
    "__cxa_demangle",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEED2Ev",
    "_ZNSt17moneypunct_bynameIcLb0EEC2EPKcm",
    "_ZNKSt8numpunctIcE8truenameEv",
    "_ZNSt11logic_errorC1ERKSs",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE14_M_get_ext_posER11__mbstate_t",
    "_ZNKSt4hashIRKSsEclES1_",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEEC2EOS3_",
    "_ZNSt6localeD2Ev",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEED0Ev",
    "_ZNSt11regex_errorD0Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_",
    "_ZNKSt14basic_ofstreamIcSt11char_traitsIcEE7is_openEv",
    "_ZNKSt14basic_ofstreamIcSt11char_traitsIcEE7is_openEv",
    "_ZNSt10moneypunctIcLb1EEC1Em",
    "_ZNSi5ungetEv",
    "_ZNSt16invalid_argumentC2ERKSs",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_St16initializer_listIwE",
    "_ZNSt12out_of_rangeD1Ev",
    "_ZGTtNSt11range_errorC2EPKc",
    "_ZNSt13runtime_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt3_V216generic_categoryEv",
    "_ZNKSt19__codecvt_utf8_baseIDiE16do_always_noconvEv",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tmPKcSC_",
    "_ZNKSbIwSt11char_traitsIwESaIwEE7compareEmmRKS2_",
    "_ZNSt7__cxx1115messages_bynameIcED2Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_PKw",
    "_ZNSt12strstreambuf3strEv",
    "_ZNSt9strstreamC2EPciSt13_Ios_Openmode",
    "_ZNSt12length_errorC1ERKSs",
    "_ZNSt7__cxx1115messages_bynameIwED1Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE6appendEmw",
    "_ZNSt6localeC1ERKS_PKci",
    "_ZNSt15time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC2ERKSsm",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE16do_thousands_sepEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE5tellgEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2ERKS4_RKS3_",
    "_ZStlsIewSt11char_traitsIwEERSt13basic_ostreamIT0_T1_ES6_RKSt7complexIT_E",
    "_ZNSt18__moneypunct_cacheIwLb0EEC2Em",
    "_ZNKSt4hashIeEclEe",
    "_ZNSbIwSt11char_traitsIwESaIwEE6insertEN9__gnu_cxx17__normal_iteratorIPwS2_EEmw",
    "_ZNSt7__cxx1114collate_bynameIwED2Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEED2Ev",
    "_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St8_Setbase",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5eraseEN9__gnu_cxx17__normal_iteratorIPKwS4_EE",
    "_ZN10__gnu_norm15_List_node_base4hookEPS0_",
    "_ZN10__cxxabiv120__function_type_infoD0Ev",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEE4openEPKcSt13_Ios_Openmode",
    "_ZNKSt9strstream5rdbufEv",
    "_ZNKSbIwSt11char_traitsIwESaIwEE7crbeginEv",
    "_ZGTtNSt11logic_errorD0Ev",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC1Ev",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE6sbumpcEv",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEE3strERKNS_12basic_stringIwS2_S3_EE",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12_M_constructIPcEEvT_S7_St20forward_iterator_tag",
    "_ZNKSbIwSt11char_traitsIwESaIwEE15_M_check_lengthEmmPKc",
    "_ZNSt12bad_weak_ptrD1Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE15_M_check_lengthEmmPKc",
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE16_M_extract_floatES3_S3_RSt8ios_baseRSt12_Ios_IostateRSs",
    "_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEED2Ev",
    "_ZNKSt3tr14hashIRKSsEclES2_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5eraseEN9__gnu_cxx17__normal_iteratorIPKcS4_EES9_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12_M_constructIPKwEEvT_S8_St20forward_iterator_tag",
    "_ZNSaIcEC2ERKS_",
    "_ZNK11__gnu_debug16_Error_formatter8_M_errorEv",
    "_ZdlPvSt11align_val_tRKSt9nothrow_t",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1EOS2_",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEEC2ERKS2_",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE8groupingEv",
    "_ZNSsC1ERKSsmRKSaIcE",
    "_ZNSbIwSt11char_traitsIwESaIwEEaSEPKw",
    "_ZNKSt13basic_istreamIwSt11char_traitsIwEE6gcountEv",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode",
    "_ZNSt10moneypunctIcLb0EE24_M_initialize_moneypunctEP15__locale_structPKc",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE16do_thousands_sepEv",
    "_ZdaPvSt11align_val_tRKSt9nothrow_t",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb1EEC1ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNKSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_bRSt8ios_baseRSt12_Ios_IostateRSs",
    "_ZTv0_n24_NSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev",
    "_ZStlsIecSt11char_traitsIcEERSt13basic_ostreamIT0_T1_ES6_RKSt7complexIT_E",
    "__cxa_vec_new",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev",
    "_ZNSt5ctypeIwEC1Em",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEEC2ERKSsSt13_Ios_Openmode",
    "_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNSt20__codecvt_utf16_baseIDsED1Ev",
    "_ZTv0_n24_NSt14basic_ofstreamIcSt11char_traitsIcEED0Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2IPKwvEET_S8_RKS3_",
    "_ZSt9use_facetISt10moneypunctIwLb0EEERKT_RKSt6locale",
    "_ZNSt14error_categoryC2Ev",
    "_ZNKSt8messagesIcE4openERKSsRKSt6localePKc",
    "_ZNSs6insertEmRKSsmm",
    "_ZNSt15messages_bynameIcED2Ev",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE9underflowEv",
    "_ZNSs8pop_backEv",
    "_ZNSi10_M_extractIyEERSiRT_",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE4syncEv",
    "_ZNSt8messagesIcED1Ev",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEC2ERKNSt7__cxx1112basic_stringIcS0_IcESaIcEEESt13_Ios_Openmode",
    "_ZNSt15messages_bynameIwED1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE9_M_createERmm",
    "_ZNSt8messagesIwED0Ev",
    "_ZNSt11__timepunctIcEC2EPSt17__timepunct_cacheIcEm",
    "_ZTv0_n24_NSt9strstreamD1Ev",
    "_ZNSt7__cxx1110moneypunctIwLb1EEC1Em",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6insertEmmc",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEE4swapERS2_",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE5rdbufEPSt15basic_streambufIwS1_E",
    "_ZNSt9bad_allocD1Ev",
    "__cxa_free_exception",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE13_M_set_bufferEl",
    "_ZNKSt10moneypunctIwLb0EE13negative_signEv",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE16do_get_monthnameES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE6setbufEPwl",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEpLEPKw",
    "_ZNKSs17find_first_not_ofEPKcm",
    "_ZNKSt5ctypeIwE5do_isEPKwS2_Pt",
    "_ZNSt10bad_typeidD0Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2ERKS4_mRKS3_",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE4setpEPwS3_",
    "_ZNSt15messages_bynameIcEC1EPKcm",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE14_M_extract_intImEES3_S3_S3_RSt8ios_baseRSt12_Ios_IostateRT_",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE14_M_group_floatEPKcmwPKwPwS9_Ri",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEC1EPKcSt13_Ios_Openmode",
    "_ZSt21_Rb_tree_rotate_rightPSt18_Rb_tree_node_baseRS0_",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE7seekoffElSt12_Ios_SeekdirSt13_Ios_Openmode",
    "_ZNSt9strstream6freezeEb",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEEC1ERKSsSt13_Ios_Openmode",
    "_ZNSt14overflow_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt14basic_iostreamIwSt11char_traitsIwEEC1EPSt15basic_streambufIwS1_E",
    "_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St5_Setw",
    "_ZNSt7__cxx1115numpunct_bynameIwED0Ev",
    "_ZNSs3endEv",
    "_ZNKSt15basic_streambufIcSt11char_traitsIcEE4gptrEv",
    "_ZThn16_NSt13basic_fstreamIwSt11char_traitsIwEED0Ev",
    "_ZSt9use_facetISt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEERKT_RKSt6locale",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5eraseEN9__gnu_cxx17__normal_iteratorIPKwS4_EES9_",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEE5widenEc",
    "_ZdlPvmSt11align_val_t",
    "_ZNSt8ios_base5imbueERKSt6locale",
    "_ZNKSt7__cxx119money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE3getES4_S4_bRSt8ios_baseRSt12_Ios_IostateRNS_12basic_stringIwS3_SaIwEEE",
    "_ZNSt15basic_streambufIwSt11char_traitsIwEE7sungetcEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2ERKS4_mm",
    "_ZNSt19__codecvt_utf8_baseIwED1Ev",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14_M_extract_numES3_S3_RiiimRSt8ios_baseRSt12_Ios_Iostate",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13_M_set_lengthEm",
    "_ZNKSt20__codecvt_utf16_baseIDiE16do_always_noconvEv",
    "_ZGTtNSt11range_errorC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt7__cxx1110moneypunctIcLb1EE13do_pos_formatEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE2atEm",
    "_ZNSt12strstreambuf7_M_freeEPc",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7_S_copyEPwPKwm",
    "_ZNKSt9type_info14__is_pointer_pEv",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE13do_pos_formatEv",
    "_ZNKSt7codecvtIcc11__mbstate_tE9do_lengthERS0_PKcS4_m",
    "_ZNSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC1Em",
    "_ZNKSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE11do_get_dateES4_S4_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSi3getEv",
    "_ZNSt10ostrstreamD1Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4dataEv",
    "_ZNSt8valarrayImEC1Em",
    "_ZNSt7codecvtIwc11__mbstate_tEC1Em",
    "_ZNSs6appendEmc",
    "_ZGTtNSt12out_of_rangeC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt15underflow_errorC2ERKSs",
    "_ZNSs12_M_leak_hardEv",
    "_ZNSt15underflow_errorD0Ev",
    "_ZNSt11logic_errorD0Ev",
    "_ZNKSt7__cxx117collateIcE10_M_compareEPKcS3_",
    "_ZNSsD1Ev",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE14__xfer_bufptrsD2Ev",
    "_ZNSdD0Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_PKwm",
    "_ZSt9has_facetISt5ctypeIwEEbRKSt6locale",
    "_ZTv0_n24_NSt19basic_istringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "__cxa_vec_delete2",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2ESt16initializer_listIwERKS3_",
    "_ZNSt17moneypunct_bynameIcLb1EEC1ERKSsm",
    "_ZNSt18__moneypunct_cacheIwLb1EED2Ev",
    "_ZNK10__cxxabiv121__vmi_class_type_info12__do_dyncastElNS_17__class_type_info10__sub_kindEPKS1_PKvS4_S6_RNS1_16__dyncast_resultE",
    "__cxa_vec_delete3",
    "_ZNKSt19basic_ostringstreamIwSt11char_traitsIwESaIwEE3strEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6insertEN9__gnu_cxx17__normal_iteratorIPKwS4_EEw",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEE14__xfer_bufptrsC1ERKS4_PS4_",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basecPKv",
    "_ZN10__cxxabiv117__pbase_type_infoD2Ev",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode",
    "_ZNKSt19__codecvt_utf8_baseIDsE5do_inER11__mbstate_tPKcS4_RS4_PDsS6_RS6_",
    "_ZNSt8ios_base7failureB5cxx11D2Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12_M_constructIN9__gnu_cxx17__normal_iteratorIPcS4_EEEEvT_SA_St20forward_iterator_tag",
    "_ZSt9use_facetINSt7__cxx1110moneypunctIcLb1EEEERKT_RKSt6locale",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4rendEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1ERKS4_RKS3_",
    "_ZNSt7__cxx1119basic_ostringstreamIwSt11char_traitsIwESaIwEEC1EOS4_",
    "_ZNSt7__cxx1117moneypunct_bynameIwLb1EEC2EPKcm",
    "_ZNSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEED2Ev",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDiE9do_lengthER11__mbstate_tPKcS4_m",
    "_ZNSt10_Sp_lockerC1EPKv",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7compareEmmPKc",
    "_ZNSt7__cxx117collateIcEC1Em",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSt15numpunct_bynameIcED1Ev",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEE9underflowEv",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13find_first_ofEPKwmm",
    "_ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEE4swapERS2_",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC2ERKSsSt13_Ios_Openmode",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE9_M_createERmm",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEE6xsgetnEPwl",
    "_ZNSt7__cxx1110moneypunctIcLb1EEC1EP15__locale_structPKcm",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb0EEC2EPKcm",
    "_ZNSt15numpunct_bynameIwED0Ev",
    "_ZNSt12system_errorD1Ev",
    "_ZNKSt14error_category10equivalentERKSt10error_codei",
    "_ZNKSt7__cxx119money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES4_S4_bRSt8ios_baseRSt12_Ios_IostateRe",
    "_ZNSt7codecvtIDsc11__mbstate_tED1Ev",
    "_ZNKSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_bRSt8ios_baseRSt12_Ios_IostateRe",
    "_ZN9__gnu_cxx18stdio_sync_filebufIcSt11char_traitsIcEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNSt12strstreambuf6setbufEPcl",
    "_ZNSi3getERc",
    "_ZNSt7collateIcED0Ev",
    "_ZNSt12__basic_fileIcEC1EP15pthread_mutex_t",
    "_ZSt20__throw_future_errori",
    "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC2EOS2_",
    "_ZN10__cxxabiv117__array_type_infoD1Ev",
    "_ZGTtNSt14overflow_errorC2EPKc",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEE4openERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
    "_ZNSoC2EPSt15basic_streambufIcSt11char_traitsIcEE",
    "_ZNKSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEE5rdbufEv",
    "_ZNSt11__timepunctIwEC2EPSt17__timepunct_cacheIwEm",
    "_ZNKSt11__timepunctIcE19_M_days_abbreviatedEPPKc",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4copyEPcmm",
    "_ZNSt8valarrayImEC1ERKS0_",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE5seekgESt4fposI11__mbstate_tE",
    "_ZNSo9_M_insertIxEERSoT_",
    "_ZNSt12out_of_rangeC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC2Ev",
    "_ZNKSt12bad_weak_ptr4whatEv",
    "_ZNSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC1Em",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13find_first_ofERKS4_m",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE16_M_get_allocatorEv",
    "_ZNKSt8numpunctIcE11do_groupingEv",
    "_ZNSi6ignoreEli",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEEC1EOS4_",
    "_ZNSt8__detail15_List_node_base7_M_hookEPS0_",
    "_ZNKSt7codecvtIDsc11__mbstate_tE9do_lengthERS0_PKcS4_m",
    "_ZNKSbIwSt11char_traitsIwESaIwEE12find_last_ofEPKwmm",
    "_ZSt14__convert_to_vIfEvPKcRT_RSt12_Ios_IostateRKP15__locale_struct",
    "_ZSt9use_facetINSt7__cxx118messagesIcEEERKT_RKSt6locale",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEEntEv",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1ERKSsSt13_Ios_Openmode",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEC1EPKcSt13_Ios_Openmode",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEEC1ERKNS_12basic_stringIcS2_S3_EESt13_Ios_Openmode",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE4openERKSsSt13_Ios_Openmode",
    "_ZNSsixEm",
    "_ZNKSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_bRSt8ios_basece",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEEC1EOS3_",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE16do_decimal_pointEv",
    "_ZNSt7__cxx1115time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC1EPKcm",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEE4openERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
    "_ZNKSt25__codecvt_utf8_utf16_baseIwE11do_encodingEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC2EmwRKS3_",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE11do_get_yearES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE13find_first_ofEPKwm",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE5uflowEv",
    "_ZNSt7__cxx117collateIwEC2Em",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE10_M_disposeEv",
    "_ZNKSt11__timepunctIwE6_M_putEPwmPKwPK2tm",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1EPKwRKS1_",
    "_ZNKSt9basic_iosIwSt11char_traitsIwEEcvbEv",
    "_ZNKSt8numpunctIcE12do_falsenameEv",
    "_ZNKSs16find_last_not_ofEcm",
    "_ZNSt15__exception_ptr13exception_ptrD2Ev",
    "_ZNSo5seekpESt4fposI11__mbstate_tE",
    "_ZSt9has_facetINSt7__cxx118time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEEEbRKSt6locale",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb1EED1Ev",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6assignEmw",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEE4openERKNSt7__cxx1112basic_stringIcS0_IcESaIcEEESt13_Ios_Openmode",
    "_ZNKSt15__exception_ptr13exception_ptr20__cxa_exception_typeEv",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1IPwEET_S5_RKS1_",
    "_ZNSt7collateIwED1Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7compareEmmPKcm",
    "_ZNSt15time_get_bynameIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC2EPKcm",
    "_ZNSs7_M_moveEPcPKcm",
    "_ZNSbIwSt11char_traitsIwESaIwEEpLEPKw",
    "_ZNSt6locale5_ImplD2Ev",
    "_ZNSs7_M_moveEPcPKcm",
    "_ZNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEEC2EOS4_",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE11curr_symbolEv",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1ERKS4_RKS3_",
    "_ZNSt11regex_errorD2Ev",
    "_ZNKSt11__timepunctIwE20_M_date_time_formatsEPPKw",
    "_ZNKSt7codecvtIwc11__mbstate_tE16do_always_noconvEv",
    "_ZNSi10_M_extractIxEERSiRT_",
    "_ZSt9has_facetINSt7__cxx118numpunctIcEEEbRKSt6locale",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13_S_copy_charsEPcS5_S5_",
    "_ZNSt7__cxx1115numpunct_bynameIcEC2EPKcm",
    "_ZNSt7codecvtIcc11__mbstate_tEC2Em",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE13thousands_sepEv",
    "_ZNSt10moneypunctIcLb1EED0Ev",
    "_ZNKSt15basic_streambufIcSt11char_traitsIcEE4pptrEv",
    "__cxa_get_exception_ptr",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPKcS4_EES9_S9_S9_",
    "_ZTv0_n24_NSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep9_S_createEmmRKS1_",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE4fillEw",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE3endEv",
    "_ZNKSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE3putES3_RSt8ios_basecPK2tmcc",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEixEm",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE3tieEPSo",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1ERKS4_mmRKS3_",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEEcvbEv",
    "_ZStplIcSt11char_traitsIcESaIcEESbIT_T0_T1_EPKS3_RKS6_",
    "_ZNSt7__cxx1110moneypunctIwLb0EEC2Em",
    "_ZSt13get_terminatev",
    "_ZNSt8ios_base7failureB5cxx11C2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE12_M_group_intEPKcmcRSt8ios_basePcS9_Ri",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEED0Ev",
    "_ZN10__cxxabiv120__function_type_infoD2Ev",
    "_ZNKSt8numpunctIwE11do_groupingEv",
    "_ZNSt19basic_istringstreamIwSt11char_traitsIwESaIwEEC2ESt13_Ios_Openmode",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE13do_neg_formatEv",
    "_ZGTtNSt11logic_errorD2Ev",
    "_ZNK11__gnu_debug16_Error_formatter10_Parameter20_M_print_descriptionEPKS0_",
    "_ZGTtNKSt9exception4whatEv",
    "_ZNKSt10moneypunctIcLb0EE13negative_signEv",
    "_ZNSt18__moneypunct_cacheIwLb0EED1Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEE10_M_extractIfEERS2_RT_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13_S_copy_charsEPcN9__gnu_cxx17__normal_iteratorIS5_S4_EES8_",
    "_ZNSt5ctypeIwEC1EP15__locale_structm",
    "_ZNSt6locale5facet19_S_destroy_c_localeERP15__locale_struct",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5rfindEPKwmm",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1ERKS4_mm",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEEC2ERKNSt7__cxx1112basic_stringIcS0_IcESaIcEEESt13_Ios_Openmode",
    "_ZThn16_NSt18basic_stringstreamIwSt11char_traitsIwESaIwEED1Ev",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4cendEv",
    "_ZNSsC1IPcEET_S1_RKSaIcE",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE11frac_digitsEv",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep8_M_cloneERKS1_m",
    "_ZNKSt7collateIcE7do_hashEPKcS2_",
    "_ZNKSt7collateIwE7do_hashEPKwS2_",
    "_ZNKSs6lengthEv",
    "_ZNSt13basic_fstreamIwSt11char_traitsIwEE4openERKSsSt13_Ios_Openmode",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEEC2EOS3_",
    "_ZNSt7__cxx1115numpunct_bynameIcED1Ev",
    "_ZNSt12domain_errorC1EPKc",
    "_ZNKSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_bRSt8ios_basecRKSs",
    "_ZStrsIwSt11char_traitsIwEERSt13basic_istreamIT_T0_ES6_St14_Resetiosflags",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6appendESt16initializer_listIwE",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4dataEv",
    "_ZNSt19basic_ostringstreamIwSt11char_traitsIwESaIwEEaSEOS3_",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEElsEPFRSt8ios_baseS4_E",
    "_ZNSt7__cxx1115numpunct_bynameIcEC1ERKNS_12basic_stringIcSt11char_traitsIcESaIcEEEm",
    "_ZNSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC2Em",
    "_ZNKSs17find_first_not_ofERKSsm",
    "_ZNKSt10lock_error4whatEv",
    "_ZN10__cxxabiv123__fundamental_type_infoD1Ev",
    "_ZNKSt11__timepunctIwE15_M_date_formatsEPPKw",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEE4openEPKcSt13_Ios_Openmode",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEED0Ev",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1EOS3_",
    "_ZNSt10moneypunctIcLb0EEC1EP15__locale_structPKcm",
    "_ZNSsC2EmcRKSaIcE",
    "_ZdaPvRKSt9nothrow_t",
    "_ZNSt9basic_iosIwSt11char_traitsIwEEC2Ev",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEED2Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7compareEmmRKS4_",
    "_ZNSt14error_categoryD1Ev",
    "_ZNSt5ctypeIcED1Ev",
    "_ZNSi6sentryC1ERSib",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE16find_last_not_ofERKS4_m",
    "_ZSt19uncaught_exceptionsv",
    "_ZNSt7__cxx118numpunctIwEC1EPSt16__numpunct_cacheIwEm",
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRSt12_Ios_IostateRPv",
    "_ZNSsC2EOSs",
    "_ZNSbIwSt11char_traitsIwESaIwEE7_M_copyEPwPKwm",
    "_ZNSbIwSt11char_traitsIwESaIwEE7_M_copyEPwPKwm",
    "_ZNSt5ctypeIwED0Ev",
    "_ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_ES4_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE9_M_assignERKS4_",
    "_ZNKSt7collateIcE9transformEPKcS2_",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE10neg_formatEv",
    "_ZNSt8messagesIwED2Ev",
    "_ZNSs12_S_constructIPKcEEPcT_S3_RKSaIcESt20forward_iterator_tag",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPcS4_EES8_PKcm",
    "_ZSt9use_facetISt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEERKT_RKSt6locale",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE5frontEv",
    "_ZNSo5flushEv",
    "_ZNKSt20__codecvt_utf16_baseIDsE13do_max_lengthEv",
    "_ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEC1Em",
    "_ZNSt13basic_ostreamIwSt11char_traitsIwEEC1EOS2_",
    "_ZNSoC1ERSd",
    "_ZNSt10bad_typeidD2Ev",
    "_ZNKSs4findEPKcmm",
    "_ZSt9use_facetISt8messagesIwEERKT_RKSt6locale",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4copyEPwmm",
    "_ZNSt7__cxx1110moneypunctIwLb1EED0Ev",
    "_ZN10__cxxabiv120__si_class_type_infoD0Ev",
    "_ZNKSt18basic_stringstreamIcSt11char_traitsIcESaIcEE5rdbufEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEEaSERKS2_",
    "_ZNSt28__atomic_futex_unsigned_base19_M_futex_wait_untilEPjjbNSt6chrono8durationIlSt5ratioILl1ELl1EEEENS2_IlS3_ILl1ELl1000000000EEEE",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEEC2EP8_IO_FILE",
    "_ZSt23__throw_underflow_errorPKc",
    "_ZNSt7__cxx1115numpunct_bynameIwED2Ev",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC1ERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
    "_ZNSbIwSt11char_traitsIwESaIwEE10_S_compareEmm",
    "_ZNSt14codecvt_bynameIwc11__mbstate_tED0Ev",
    "_ZSt9use_facetISt7collateIwEERKT_RKSt6locale",
    "_ZSt9use_facetISt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEERKT_RKSt6locale",
    "_ZNSt13bad_exceptionD1Ev",
    "_ZNSt9basic_iosIwSt11char_traitsIwEE15_M_cache_localeERKSt6locale",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC2ESt13_Ios_Openmode",
    "_ZSt7getlineIwSt11char_traitsIwESaIwEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE",
    "_ZNSt7__cxx1110moneypunctIwLb1EEC2EPSt18__moneypunct_cacheIwLb1EEm",
    "_ZNKSt10moneypunctIwLb1EE16do_positive_signEv",
    "_ZNKSt19__codecvt_utf8_baseIDiE9do_lengthER11__mbstate_tPKcS4_m",
    "_ZNKSt7__cxx118messagesIwE20_M_convert_from_charEPc",
    "_ZTv0_n24_NSdD1Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE8max_sizeEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE18_M_construct_aux_2Emc",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEEaSEOS4_",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNKSbIwSt11char_traitsIwESaIwEE17find_first_not_ofEwm",
    "_ZNSt7__cxx1117moneypunct_bynameIcLb0EED0Ev",
    "_ZNKSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEE3strEv",
    "_ZNSt12length_errorD0Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep13_M_set_leakedEv",
    "_ZNKSt7__cxx118messagesIcE6do_getEiiiRKNS_12basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSs6_M_repEv",
    "_ZNSt18__moneypunct_cacheIcLb0EE8_M_cacheERKSt6locale",
    "_ZNKSbIwSt11char_traitsIwESaIwEE7compareEmmPKw",
    "_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_NS0_IPKcSsEES5_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE9push_backEw",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4findEPKwm",
    "_ZNSt11logic_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt7__cxx1115time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC1EPKcm",
    "_ZNSt15underflow_errorD2Ev",
    "_ZNSt10moneypunctIcLb0EEC2Em",
    "__cxa_bad_cast",
    "_ZNSt6gslice8_IndexerC1EmRKSt8valarrayImES4_",
    "_ZNSt11logic_errorD2Ev",
    "_ZNSdD2Ev",
    "_ZNSt9basic_iosIcSt11char_traitsIcEE4swapERS2_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPKwS4_EES9_S8_S8_",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE16_M_destroy_pbackEv",
    "_ZNKSs4dataEv",
    "_ZStplIcSt11char_traitsIcESaIcEESbIT_T0_T1_ES3_RKS6_",
    "_ZNSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED0Ev",
    "_ZNSs4_Rep8_M_cloneERKSaIcEm",
    "_ZN9__gnu_cxx9free_list6_M_getEm",
    "_ZNSbIwSt11char_traitsIwESaIwEE13_S_copy_charsEPwN9__gnu_cxx17__normal_iteratorIS3_S2_EES6_",
    "_ZNSt7codecvtIwc11__mbstate_tED0Ev",
    "_ZNSs5beginEv",
    "_ZNSt8numpunctIcEC2Em",
    "_ZNKSt20__codecvt_utf16_baseIDsE5do_inER11__mbstate_tPKcS4_RS4_PDsS6_RS6_",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1EmwRKS3_",
    "_ZNSt9basic_iosIcSt11char_traitsIcEEC1EPSt15basic_streambufIcS1_E",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPcS4_EES8_PKc",
    "_ZNSt16invalid_argumentD0Ev",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE13_M_local_dataEv",
    "_ZNSt8numpunctIwEC1Em",
    "_ZNKSt10moneypunctIwLb0EE16do_positive_signEv",
    "_ZSt9use_facetISt5ctypeIwEERKT_RKSt6locale",
    "_ZNSdC2EOSd",
    "_ZNSt15time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEC2EPKcm",
    "_ZGTtNSt13runtime_errorD0Ev",
    "_ZNSt12strstreambuf6freezeEb",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendEmc",
    "_ZGTtNSt13runtime_errorC2EPKc",
    "_ZStrsIwSt11char_traitsIwEERSt13basic_istreamIT_T0_ES6_St8_Setbase",
    "_ZNKSt8messagesIcE8do_closeEi",
    "_ZNSbIwSt11char_traitsIwESaIwEE6assignEmw",
    "_ZGTtNSt16invalid_argumentC1EPKc",
    "_ZNSt13random_device14_M_init_pretr1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt9basic_iosIcSt11char_traitsIcEE3eofEv",
    "_ZNSs9_M_assignEPcmc",
    "_ZNSs9_M_assignEPcmc",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE13decimal_pointEv",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEE4swapERS4_",
    "_ZNKSt19__codecvt_utf8_baseIwE11do_encodingEv",
    "_ZNSt7__cxx1118basic_stringstreamIwSt11char_traitsIwESaIwEED2Ev",
    "_ZNKSt7__cxx1110moneypunctIwLb0EE13decimal_pointEv",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5rfindEcm",
    "_ZNSbIwSt11char_traitsIwESaIwEE6appendEPKwm",
    "_ZNKSt11logic_error4whatEv",
    "_ZNSt15numpunct_bynameIwED2Ev",
    "_ZThn16_NSt9strstreamD0Ev",
    "_ZNKSt7codecvtIwc11__mbstate_tE13do_max_lengthEv",
    "_ZNSt12__basic_fileIcE4fileEv",
    "_ZNSt12domain_errorC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNSt9exceptionD1Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE12_S_empty_repEv",
    "_ZNKSt7collateIcE10do_compareEPKcS2_S2_S2_",
    "_ZNSt17moneypunct_bynameIwLb1EED0Ev",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEE4swapERS4_",
    "_ZNKSt4hashISsEclESs",
    "_ZNSt7__cxx1115basic_stringbufIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode",
    "_ZStlsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_St8_SetfillIS3_E",
    "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode",
    "_ZNSt14codecvt_bynameIwc11__mbstate_tEC2ERKSsm",
    "_ZNSsaSEOSs",
    "_ZNSs4backEv",
    "_ZNSt7__cxx117collateIcED0Ev",
    "_ZNSt7collateIcED2Ev",
    "_ZNSs4_Rep15_M_set_sharableEv",
    "_ZN9__gnu_cxx18stdio_sync_filebufIwSt11char_traitsIwEEaSEOS3_",
    "_ZNSt12domain_errorD0Ev",
    "_ZNSt6localeC1EPKc",
    "_ZNSt15time_put_bynameIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC2ERKNSt7__cxx1112basic_stringIcS2_SaIcEEEm",
    "_ZNSt15numpunct_bynameIwEC2EPKcm",
    "_ZNSt16invalid_argumentC1EPKc",
    "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep15_M_set_sharableEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERb",
    "_ZNSt13runtime_errorC1ERKS_",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE27_M_allocate_internal_bufferEv",
    "_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_S1_S1_",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEED0Ev",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERd",
    "_ZNSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEEC2Em",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev",
    "_ZNKSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE3putES4_bRSt8ios_basewRKNS_12basic_stringIwS3_SaIwEEE",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERe",
    "_ZNKSbIwSt11char_traitsIwESaIwEE7_M_iendEv",
    "_ZNSsC2EPKcmRKSaIcE",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERf",
    "_ZNKSt8numpunctIcE11do_truenameEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPKcS4_EES9_NS6_IPcS4_EESB_",
    "_ZNSt11__timepunctIcEC1Em",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1EmwRKS1_",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE15_M_update_egptrEv",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE7replaceEN9__gnu_cxx17__normal_iteratorIPcS4_EES8_RKS4_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC2IN9__gnu_cxx17__normal_iteratorIPcS4_EEvEET_SA_RKS3_",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERi",
    "_ZNSt11this_thread11__sleep_forENSt6chrono8durationIlSt5ratioILl1ELl1EEEENS1_IlS2_ILl1ELl1000000000EEEE",
    "_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St14_Resetiosflags",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE10neg_formatEv",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE13thousands_sepEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERj",
    "_ZSt9use_facetISt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEERKT_RKSt6locale",
    "_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEE3strERKSs",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERl",
    "__cxa_throw_bad_array_length",
    "_ZNKSt7codecvtIDic11__mbstate_tE9do_lengthERS0_PKcS4_m",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERm",
    "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE11do_get_dateES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED0Ev",
    "_ZStrsIewSt11char_traitsIwEERSt13basic_istreamIT0_T1_ES6_RSt7complexIT_E",
    "_ZNSolsEb",
    "_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE12_M_constructIPwEEvT_S7_St20forward_iterator_tag",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE4swapERS4_",
    "_ZNSolsEd",
    "_ZNSt13random_device7_M_finiEv",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERs",
    "_ZNSt22condition_variable_anyD2Ev",
    "_ZNSaIcEC2Ev",
    "_ZNSolsEe",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERt",
    "_ZNSolsEf",
    "_ZNSt10moneypunctIcLb1EEC2EP15__locale_structPKcm",
    "_ZSt9has_facetISt5ctypeIcEEbRKSt6locale",
    "_ZNSbIwSt11char_traitsIwESaIwEEC2EmwRKS1_",
    "_ZdlPvRKSt9nothrow_t",
    "_ZNSt15_List_node_base10_M_reverseEv",
    "_ZNSt13runtime_errorC1ERKSs",
    "_ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EES4_",
    "_ZNSaIwEC1Ev",
    "_ZNSolsEi",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERx",
    "_ZNSolsEj",
    "_ZNKSt7__cxx1110moneypunctIcLb0EE13do_neg_formatEv",
    "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE13_M_insert_intIxEES3_S3_RSt8ios_basewT_",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE6substrEmm",
    "_ZNSt13basic_istreamIwSt11char_traitsIwEErsERy",
    "_ZNKSt5ctypeIwE11do_scan_notEtPKwS2_",
    "_ZNSt12out_of_rangeC2EPKc",
    "_ZNSolsEl",
    "_ZNSbIwSt11char_traitsIwESaIwEEC1Ev",
    "_ZNSt14codecvt_bynameIcc11__mbstate_tED1Ev",
    "_ZNSt15basic_stringbufIwSt11char_traitsIwESaIwEE9pbackfailEj",
    "_ZNSolsEm",
    "_ZNSt7__cxx1119basic_istringstreamIwSt11char_traitsIwESaIwEEC1ESt13_Ios_Openmode",
    "_ZNSt7__cxx117collateIwED1Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5c_strEv",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE15_M_create_pbackEv",
    "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE13_M_insert_intImEES3_S3_RSt8ios_basecT_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1ESt16initializer_listIcERKS3_",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4findEPKwmm",
    "_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED0Ev",
    "_ZNSolsEs",
    "_ZNSolsEt",
    "_ZNKSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEE4findERKS4_m",
    "_ZNKSt25__codecvt_utf8_utf16_baseIDsE5do_inER11__mbstate_tPKcS4_RS4_PDsS6_RS6_",
    "_ZNSolsEx",
    "_ZNSolsEy",
    "_ZNSt17moneypunct_bynameIwLb0EEC1ERKSsm",
    "_ZNSt10moneypunctIcLb1EED2Ev",
    "_ZNKSt5ctypeIwE10do_tolowerEPwPKw",
    "_ZSt9use_facetISt8numpunctIcEERKT_RKSt6locale",
    "_ZNSt10istrstreamD0Ev",
    "_ZNSd4swapERSd",
    "_ZNKSbIwSt11char_traitsIwESaIwEE4_Rep12_M_is_leakedEv",
    "_ZNSdaSEOSd",
    "_ZNKSt5ctypeIwE5do_isEtw",
    "_ZNSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEEC2Em",
    "_ZNSt13basic_filebufIwSt11char_traitsIwEE6xsputnEPKwl",
    "_ZNSt18__moneypunct_cacheIcLb0EEC1Em",
    "_ZNSt7__cxx1119basic_istringstreamIcSt11char_traitsIcESaIcEEC1ERKNS_12basic_stringIcS2_S3_EESt13_Ios_Openmode",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC2ERKNSt7__cxx1112basic_stringIcS0_IcESaIcEEESt13_Ios_Openmode",
    "_ZNSt5ctypeIcEC1EP15__locale_structPKtbm",
    "_ZSt9has_facetISt10moneypunctIwLb0EEEbRKSt6locale",
    "_ZNSt20bad_array_new_lengthD0Ev",
    "_ZNSt7codecvtIcc11__mbstate_tED1Ev",
    "_ZGTtNKSt13runtime_error4whatEv",
    "_ZNSt12__basic_fileIcE9showmanycEv",
    "_ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE9showmanycEv",
    "_ZSt22__throw_overflow_errorPKc",
    "_ZNSi7putbackEc",
    "_ZGTtNKSt13bad_exceptionD1Ev",
    "_ZNKSt19__codecvt_utf8_baseIDiE5do_inER11__mbstate_tPKcS4_RS4_PDiS6_RS6_",
    "_ZNSt10moneypunctIwLb1EEC2EPSt18__moneypunct_cacheIwLb1EEm",
    "_ZNKSt8numpunctIwE11do_truenameEv",
    "_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC1EOS2_",
    "_ZNSbIwSt11char_traitsIwESaIwEE4rendEv",
    "_ZNSt13basic_fstreamIcSt11char_traitsIcEED2Ev",
    "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE11get_weekdayES3_S3_RSt8ios_baseRSt12_Ios_IostateP2tm",
    "_ZNSs6assignEmc",
    "_ZStplIwSt11char_traitsIwESaIwEENSt7__cxx1112basic_stringIT_T0_T1_EERKS8_SA_",
    "_ZNKSt20bad_array_new_length4whatEv",
    "_ZNKSt7__cxx1110moneypunctIwLb1EE11curr_symbolEv",
    "_ZNKSt25__codecvt_utf8_utf16_baseIwE5do_inER11__mbstate_tPKcS4_RS4_PwS6_RS6_",
    "_ZSt9use_facetISt10moneypunctIcLb0EEERKT_RKSt6locale",
    "_ZNSt7__cxx1110moneypunctIwLb0EED1Ev",
    "_ZNSt8ios_base4InitC2Ev",
    "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE12find_last_ofEcm",
    "_ZNSt10_Sp_lockerC2EPKvS1_",
    "_ZNSt15numpunct_bynameIwEC1ERKSsm",
    "_ZNSt14basic_ofstreamIwSt11char_traitsIwEEC1ERKNSt7__cxx1112basic_stringIcS0_IcESaIcEEESt13_Ios_Openmode",
    "_ZNKSt7__cxx119money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES4_bRSt8ios_basewRKNS_12basic_stringIwS3_SaIwEEE",
    "_ZSt17__verify_groupingPKcmRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
    "_ZNKSt19basic_istringstreamIwSt11char_traitsIwESaIwEE3strEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE4syncEv",
    "_ZNSi10_M_extractItEERSiRT_",
    "_ZTv0_n24_NSt14basic_ifstreamIwSt11char_traitsIwEED1Ev",
    "_ZStlsIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_St13_Setprecision",
    "_ZNKSt13basic_filebufIwSt11char_traitsIwEE7is_openEv",
    "_ZNSt13basic_filebufIcSt11char_traitsIcEED2Ev",
    "_ZNSt3_V214error_categoryD1Ev",
    "_ZNSbIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_NS4_IPKwS2_EES9_",
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1Ev",
    "_ZNSt9basic_iosIwSt11char_traitsIwEED1Ev",
    "_ZNKSt11__timepunctIcE15_M_date_formatsEPPKc",
    "_ZNSt10moneypunctIwLb1EEC1EP15__locale_structPKcm",
    "_ZNSt13__future_base11_State_baseD1Ev",
    "_ZNSt14basic_ifstreamIcSt11char_traitsIcEE4openERKSsSt13_Ios_Openmode",
    "_ZNSt7__cxx1115basic_stringbufIwSt11char_traitsIwESaIwEE14__xfer_bufptrsC1ERKS4_PS4_",
    "_ZNSt7__cxx118time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEED1Ev",
    "_ZNSt15time_get_bynameIwSt19istreambuf_iteratorIwSt11char_traitsIwEEED1Ev",
    "_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_St12_Setiosflags",
};

/*

iron@CSE:spa$ cat Unified_cpp_memory_build.s | grep callq | sort | uniq | awk '{printf "\""$2":\",\n"}'
...
"_ZN9AllocatorI15MozJemallocBaseE17jemalloc_ptr_infoEPKvP19jemalloc_ptr_info_s:",
 */

static char *one_arg_funcs[] = {
    "malloc:",
    "free:",
    "malloc_usable_size:",
    "pvalloc:",
    "valloc:",
};

static char *two_args_funcs[] = {
    "calloc:",
    "realloc:",
    "aligned_alloc:",
    "memalign:",
};

static char *three_args_funcs[] = {
    "posix_memalign:",
};

// FIXME:  NO MORE than 3 arguments here.
static char * customized_libc_funcs[] = {
  // one argument
  "malloc:",
  "free:",
  "malloc_usable_size:",
  "pvalloc:",
  "valloc:",

  // two arguments
  "calloc:",
  "realloc:",
  "aligned_alloc:",
  "memalign:",

  // three arguments
  "posix_memalign:",

};

static char * no_instr_funcs[] = {
    "flash.stack.no_instr_funcs.place.holder:",
#if 0
    //
    "_ZL15WasmTrapHandleriP9siginfo_tPv:",
    "_ZNK2js4wasm11CodeSegment4codeEv:",
    "_ZNK2js4wasm4Code10lookupTrapEPvPNS0_4TrapEPNS0_14BytecodeOffsetE:",
    "_ZN2js3jit13JitActivation13startWasmTrapENS_4wasm4TrapEjRKN2JS22ProfilingFrameIterator13RegisterStateE:",
    "_ZN2js4wasm14StartUnwindingERKN2JS22ProfilingFrameIterator13RegisterStateEPNS0_11UnwindStateEPb:",
    "_ZN2js4wasm10LookupCodeEPKvPPKNS0_9CodeRangeE:",
    "_ZNK2js4wasm13ModuleSegment11lookupRangeEPKv:",
    "_ZNK2js4wasm15LazyStubSegment11lookupRangeEPKv:",
    "_ZN2js4wasm14LookupInSortedERKN7mozilla6VectorINS0_9CodeRangeELm0ENS_17SystemAllocPolicyEEENS3_12OffsetInCodeE:",
    "_ZNK2js4wasm4Code14lookupCallSiteEPv:",
    "abort:",
    "__stack_chk_fail:",
#endif
};

static char * on_stack_handlers[] = {

    //_Z15WasmTrapHandleriP9siginfo_tPv
    // void WasmTrapHandler(int signum, siginfo_t* info, void* context)
    "_ZL15WasmTrapHandleriP9siginfo_tPv:",
    "_Z15WasmTrapHandleriP9siginfo_tPv:",
#if 0
    // FIXME: one handler might be called in another handler in Firefox?
    //        We only handle the outter-most handler now.

    // void SEGVHandler::handler(int signum, siginfo_t* info, void* context)
    "_ZN11SEGVHandler7handlerEiP9siginfo_tPv:",

    // void MmapSIGBUSHandler(int signum, siginfo_t* info, void* context)
    "_ZL17MmapSIGBUSHandleriP9siginfo_tPv:",
    "_Z17MmapSIGBUSHandleriP9siginfo_tPv:",

    // static void CatchFatalSignals(int num, siginfo_t* info, void* context)
    "_ZL17CatchFatalSignalsiP9siginfo_tPv:",
    "_Z17CatchFatalSignalsiP9siginfo_tPv:",

    // static void UnixExceptionHandler(int signum, siginfo_t* info, void* context)
    "_ZL20UnixExceptionHandleriP9siginfo_tPv:",
    "_Z20UnixExceptionHandleriP9siginfo_tPv:",

    // void nsProfileLock::FatalSignalHandler(int signo)
    "_ZN13nsProfileLock18FatalSignalHandlerEi:",

    // void nsProfileLock::FatalSignalHandler(int signo, siginfo_t* info, void* context)
    "_ZN13nsProfileLock18FatalSignalHandlerEiP9siginfo_tPv:",
    // void fpehandler(int signum, siginfo_t* si, void* context)
    "_ZL10fpehandleriP9siginfo_tPv:",
    "_Z10fpehandleriP9siginfo_tPv:",

    // void ExceptionHandler::SignalHandler(int sig, siginfo_t* info, void* uc)
    "_ZN16ExceptionHandler13SignalHandlerEiP9siginfo_tPv:",

    // void SEGVHandler::test_handler(int signum, siginfo_t* info, void* context)
    "_ZN11SEGVHandler12test_handlerEiP9siginfo_tPv:",
#endif
    // for test
    "spa_demo_blackbox_handler:",
};

static int is_target_func(char *line, char ** funcs, int n){
    for(int i = 0; i < n; i++){
        if(!strncmp(line, funcs[i], strlen(funcs[i]))){
            return 1;
        }
    }
    return 0;
}

static int is_one_arg_func(char *line){
    return is_target_func(line, one_arg_funcs, sizeof(one_arg_funcs)/sizeof(char *));
}

static int is_two_args_func(char *line){
    return is_target_func(line, two_args_funcs, sizeof(two_args_funcs)/sizeof(char *));
}

static int is_three_args_func(char *line){
    return is_target_func(line, three_args_funcs, sizeof(three_args_funcs)/sizeof(char *));
}

//static int is_three_args_func()

static int is_customized_libc_func(char *line){
//    // FIXME
//#if defined(USE_SPA_GS_RSP)
//    if(!strncmp(line, SPA_CXX_GLOBAL_SUB_I_PREFIX, strlen(SPA_CXX_GLOBAL_SUB_I_PREFIX))){
//        return 1;
//    }
//#endif
    return is_target_func(line, customized_libc_funcs, sizeof(customized_libc_funcs)/sizeof(char *));
}

static int is_no_instr_func(char *line){
    return is_target_func(line, no_instr_funcs, sizeof(no_instr_funcs)/sizeof(char *));
}

static int is_on_stack_handler(char *line){
    return is_target_func(line, on_stack_handlers, sizeof(on_stack_handlers)/ sizeof(char *));
}

static int is_libc_func_called(char * line){
    return is_target_func(line, libcxx_names, sizeof(libcxx_names)/sizeof(char *));
}

static int is_libcxx_func_called(char *line){
    return is_target_func(line, libc_names, sizeof(libc_names)/sizeof(char *));
}


// export LC_ALL=C;
// sort
// https://blog.csdn.net/zhangna20151015/article/details/51163624
static int is_protected_function(char *line){
    char name[MAX_LINE];
    // FIXME
    strcpy(name, line);
    char *p = name;

    while(*p != '\n' && *p != ' ' && *p != '\t'){
        p++;
    }
    // ignore the comments
    *p = 0;

    // FIXME:
#if 0
    for(int i = 0; i < num_of_protected_funcs; i++){
        int n1 = strlen(spa_protected_funcs[i]);
        int n2 = strlen(name);
        if(n1 == n2 && !strncmp(name, spa_protected_funcs[i], n1)){
            return 1;
        }
    }
#endif
#if 1
    int low = 0, high = num_of_protected_funcs-1;
    while(low <= high){
        int mid = (low + high) / 2;
        //printf("mid = %d, low = %d, high = %d\n", mid,  low, high);
        int r = strcmp(name, spa_protected_funcs[mid]);
        //printf("mid = %d, %s %s\n", mid,  name, spa_protected_funcs[mid]);
        if(r == 0){
            return 1;
        }else if(r > 0){
            low = mid + 1;
        }else{
            high = mid - 1;
        }
    }
#endif

    return 0;
}


/* Examine and modify parameters to pass to 'as'. Note that the file name
   is always the last parameter passed by GCC, so we exploit this property
   to keep the code simple. */

static void edit_params(int argc, char** argv) {

  u8 *tmp_dir = getenv("TMPDIR"), *afl_as = getenv("AFL_AS");
  u32 i;

#ifdef __APPLE__

  u8 use_clang_as = 0;

  /* On MacOS X, the Xcode cctool 'as' driver is a bit stale and does not work
     with the code generated by newer versions of clang that are hand-built
     by the user. See the thread here: http://goo.gl/HBWDtn.

     To work around this, when using clang and running without AFL_AS
     specified, we will actually call 'clang -c' instead of 'as -q' to
     compile the assembly file.

     The tools aren't cmdline-compatible, but at least for now, we can
     seemingly get away with this by making only very minor tweaks. Thanks
     to Nico Weber for the idea. */

  if (clang_mode && !afl_as) {

    use_clang_as = 1;

    afl_as = getenv("AFL_CC");
    if (!afl_as) afl_as = getenv("AFL_CXX");
    if (!afl_as) afl_as = "clang";

  }

#endif /* __APPLE__ */

  /* Although this is not documented, GCC also uses TEMP and TMP when TMPDIR
     is not set. We need to check these non-standard variables to properly
     handle the pass_thru logic later on. */

  if (!tmp_dir) tmp_dir = getenv("TEMP");
  if (!tmp_dir) tmp_dir = getenv("TMP");
  if (!tmp_dir) tmp_dir = "/tmp";

  as_params = ck_alloc((argc + 32) * sizeof(u8*));

  //as_params[0] = afl_as ? afl_as : (u8*)"as";
  as_params[0] = afl_as ? afl_as : (u8*)"/usr/bin/as";

  as_params[argc] = 0;

  for (i = 1; i < argc - 1; i++) {
    //printf("argv[%d] = %s\n", i, argv[i]);
    if (!strcmp(argv[i], "--64")) use_64bit = 1;
    else if (!strcmp(argv[i], "--32")) use_64bit = 0;

#ifdef __APPLE__

    /* The Apple case is a bit different... */

    if (!strcmp(argv[i], "-arch") && i + 1 < argc) {

      if (!strcmp(argv[i + 1], "x86_64")) use_64bit = 1;
      else if (!strcmp(argv[i + 1], "i386"))
        FATAL("Sorry, 32-bit Apple platforms are not supported.");

    }

    /* Strip options that set the preference for a particular upstream
       assembler in Xcode. */

    if (clang_mode && (!strcmp(argv[i], "-q") || !strcmp(argv[i], "-Q")))
      continue;

#endif /* __APPLE__ */

    as_params[as_par_cnt++] = argv[i];

  }

#ifdef __APPLE__

  /* When calling clang as the upstream assembler, append -c -x assembler
     and hope for the best. */

  if (use_clang_as) {

    as_params[as_par_cnt++] = "-c";
    as_params[as_par_cnt++] = "-x";
    as_params[as_par_cnt++] = "assembler";

  }

#endif /* __APPLE__ */

  input_file = argv[argc - 1];
  //printf("argv[%d] = %s\n", argc - 1, input_file);
  if (input_file[0] == '-') {

    if (!strcmp(input_file + 1, "-version")) {
      just_version = 1;
      modified_file = input_file;
      goto wrap_things_up;
    }

    if (input_file[1]) FATAL("Incorrect use (not called through afl-gcc?)");
      else input_file = NULL;

  } else {

    /* Check if this looks like a standard invocation as a part of an attempt
       to compile a program, rather than using gcc on an ad-hoc .s file in
       a format we may not understand. This works around an issue compiling
       NSS. */

    if (strncmp(input_file, tmp_dir, strlen(tmp_dir)) &&
        strncmp(input_file, "/var/tmp/", 9) &&
        strncmp(input_file, "/tmp/", 5)) pass_thru = 1;

  }
#if 1 // added by iron
  if(spa_is_rustc_pass_thru(input_file)){
      pass_thru = 0;
  }
  //
  {
      s32 _len = strlen(input_file);
      u8 * _dotname = ck_alloc(_len + 1);
      strncpy(_dotname, input_file, _len);
      for(i = 0; i < _len; i++){
          if(_dotname[i] == '\\' || _dotname[i] == '/'){
              _dotname[i] = '.';
          }
      }
      modified_file = alloc_printf("%s/SPA-%u-%u-###%s###.s", tmp_dir, getpid(),
                                   (u32)time(NULL), _dotname);
  }
#endif
//  modified_file = alloc_printf("%s/.afl-%u-%u.s", tmp_dir, getpid(),
//                               (u32)time(NULL));

wrap_things_up:

  as_params[as_par_cnt++] = modified_file;
  as_params[as_par_cnt]   = NULL;

}


/* Process input file, generate modified_file. Insert instrumentation in all
   the appropriate places. */

static void add_instrumentation(void) {

  static u8 line[MAX_LINE];

  FILE* inf;
  FILE* outf;
  s32 outfd;
  u32 ins_lines = 0, n_start = 0, n_end = 0;

  u8  instr_ok = 0, skip_csect = 0, skip_next_label = 0, in_main = 0, is_main_exe = 0,
      skip_intel = 0, skip_app = 0, instrument_next = 0, start2end = 0,
       //in_malloc = 0, in_valloc = 0, in_realloc = 0, in_calloc = 0, in_free = 0, in_abort = 0;
      on_stack_handler = 0,
      in_customized_func = 0, in_no_instr_func = 0;

#ifdef __APPLE__

  u8* colon_pos;

#endif /* __APPLE__ */

  if (input_file) {

    inf = fopen(input_file, "r");
    if (!inf) PFATAL("Unable to read '%s'", input_file);

  } else inf = stdin;

  outfd = open(modified_file, O_WRONLY | O_EXCL | O_CREAT, 0600);

  if (outfd < 0) PFATAL("Unable to write to '%s'", modified_file);

  outf = fdopen(outfd, "w");

  if (!outf) PFATAL("fdopen() failed");

  while (fgets(line, MAX_LINE, inf)) {
#if 1   // added by iron.
    /*
       https://www.felixcloutier.com/x86/divps
       When building Firefox-79 by SPA,  LLVM 10
       "/tmp/skcms-7d2933.s", Error: unknown vector operation: ` {z}'
       It is OK for clang, but not for /usr/bin/as:
          vdivps	%zmm10, %zmm0, %zmm0 {%k1} {z}
       So we change it into
           vdivps	%zmm10, %zmm0, %zmm0 {%k1}{z}
       to make both of them happy.

       SPA + LLVM 7
       /tmp/.SPA-1793-1599755391-###.tmp.skcms-1ba473.s###.s
       vmovdqu16	%zmm5, %zmm5 {%k1} {z}
     */
//    if(!strncmp(line, "\tvdivps", 7)){
//        // FIXME: \r\n
//        u8 *ptr = strstr(line, " {z}\n");
//        if(!ptr){
//            ptr = strstr(line, " {z}\r\n");
//        }
//        if(ptr){
//            strcpy(ptr, "{z}\n");
//        }
//    }
      u8 *write_mask_str;
      if((write_mask_str = strstr(line, SPA_XMM_YMM_ZMM_WRITE_MASK_CLANG)) != NULL){
          // FIXME: \r\n
          // We are sure there is enough space here.
          strcpy(write_mask_str, SPA_XMM_YMM_ZMM_WRITE_MAST_GCC);
      }

#endif

    /* In some cases, we want to defer writing the instrumentation trampoline
       until after all the labels, macros, comments, etc. If we're in this
       mode, and if the line starts with a tab followed by a character, dump
       the trampoline now. */

    if (!pass_thru && !skip_intel && !skip_app && !skip_csect && instr_ok &&
        instrument_next && line[0] == '\t' && isalpha(line[1])) {

//      fprintf(outf, use_64bit ? trampoline_fmt_64 : trampoline_fmt_32,
//              R(MAP_SIZE));
      fprintf(outf, use_64bit ? "###SPA## trampoline_fmt_64\n" : "###SPA## trampoline_fmt_32\n");
      if(in_main && !pass_thru && use_64bit){
          fprintf(outf, "###SPA### the entry of main().\n");

#if 0

//#if defined(USE_SPA_BUDDY_STACK_TLS_WITH_STK_SIZE)
//          // Set a flag to notify others that main() has been called.
//          char *reg_x = "%r11";
//          fprintf(outf, "\tmovq\t" ".BUDDY.CALL_STACK_SIZE_MASK@GOTPCREL(%%rip), %s\n", reg_x);
//          fprintf(outf, "\tmovq\t" "$%d, %d(%s)\n", SPA_MAIN_FUNC_CALLED_FLAG,
//                  SPA_OFFSET_OF_MAIN_FUNC_CALLED_FLAG, reg_x);
//#if 0
//          // Get a callee-saved register, such that it is not changed after mprotect().
//          // (1) save the registers
//          fprintf(outf, "\tpushq\t" "%%rdi\n");
//          fprintf(outf, "\tpushq\t" "%%rsi\n");
//          fprintf(outf, "\tpushq\t" "%%rdx\n");


//          // (2) mprotect(addr, 4096, PROT_WRITE | PROT_READ);
//          fprintf(outf, "\tmovq\t" "$3, %%rdx\n");
//          // 4096, one page
//          fprintf(outf, "\tmovq\t" "$4096, %%rsi\n");
//          // the page-aligned address
//          fprintf(outf, "\tmovq\t" ".BUDDY.CALL_STACK_SIZE_MASK@GOTPCREL(%%rip), %%rdi\n");
//          fprintf(outf, "\tcallq\t" "mprotect@PLT\n");

//          // (3) update
//          fprintf(outf, "\tmovq\t" ".BUDDY.CALL_STACK_SIZE_MASK@GOTPCREL(%%rip), %%rdi\n");
//          fprintf(outf, "\tmovq\t" "$%d, %d(%%rdi)\n", SPA_MAIN_FUNC_CALLED_FLAG,
//                  SPA_OFFSET_OF_MAIN_FUNC_CALLED_FLAG);

//          // (4) mprotect(addr, 4096, PROT_READ);
//          fprintf(outf, "\tmovq\t" "$1, %%rdx\n");
//          // 4096, one page
//          fprintf(outf, "\tmovq\t" "$4096, %%rsi\n");
//          fprintf(outf, "\tcallq\t" "mprotect@PLT\n");

//          // (5) restore the registers
//          fprintf(outf, "\tpopq\t" "%%rdx\n");
//          fprintf(outf, "\tpopq\t" "%%rsi\n");
//          fprintf(outf, "\tpopq\t" "%%rdi\n");
//#endif

//#endif

#endif
      }
      instrument_next = 0;
      ins_lines++;

    }

    // The begin of a function
    if(!strncmp(line, SPA_CFI_STARTPROC, strlen(SPA_CFI_STARTPROC))){
        start2end = 1;
        n_start++;
        if(!pass_thru && use_64bit){
#if defined(USE_SPA_SHADOW_STACK)
            fprintf(outf, "%s", line);
            fprintf(outf, "\tpopq\t-%ld(%%rsp)\n", (DEF_SPA_SS_OFFSET));
            fprintf(outf, "\tsubq\t$8, %%rsp\n");
            continue;


#elif defined(USE_SPA_SHADOW_STACK_VIA_REG)
            char * reg_name = "%rax";
            fprintf(outf, "%s", line);
            fprintf(outf, "\tmovq\t" "(%%rsp), %s\n", reg_name);
            fprintf(outf, "\tmovq\t" "%s, -%ld(%%rsp)\n", reg_name, (long)(DEF_SPA_SS_OFFSET));
            continue;


#elif defined(USE_SPA_BUDDY_STACK_TLS)
            fprintf(outf, "%s", line);
            char * reg_x = "%r11";
            fprintf(outf, "###SPA### FUNCTION_ENTRY\n");
            // Get the random value from TLS
            fprintf(outf, "\tmovq\t" "%%rsp, %s\n", reg_x);
            fprintf(outf, "\tandq\t" "$%ld, %s\n", (long)DEF_BUDDY_CALL_STACK_SIZE_MASK, reg_x);
            fprintf(outf, "\tmovq\t" "-%ld(%s), %s\n", (long)(DEF_BUDDY_FUNCTION_LOCAL_STORAGE_SIZE), reg_x, reg_x);
            // randomize return address
            fprintf(outf, "\taddq\t" "(%%rsp), %s\n", reg_x);
            // Save it on the shadow stack
            fprintf(outf, "\tmovq\t" "%s, -%ld(%%rsp)\n", reg_x, (long)(DEF_SPA_SS_OFFSET));
            continue;

#elif defined(USE_SPA_GS_RSP)
            //
            fprintf(outf, "%s", line);
            //
            if(in_no_instr_func){
                continue;
            }

            if(on_stack_handler){
                // FIXME: no more than 3 arguments
                // rdi, rsi, rdx, rcx, r8, r9
                fprintf(outf, "\tpushq\t" "%%rdi\n");
                fprintf(outf, "\tpushq\t" "%%rsi\n");
                fprintf(outf, "\tpushq\t" "%%rdx\n");
                fprintf(outf, "\tcallq\t" "unsw_inc_asm_js_crash_cnt@plt\n");
                fprintf(outf, "\tpopq\t" "%%rdx\n");
                fprintf(outf, "\tpopq\t" "%%rsi\n");
                fprintf(outf, "\tpopq\t" "%%rdi\n");

                continue;
            }
            /*
                workaroud,
                a protected malloc() might be called before initialization of gs register.
             */
            if(in_customized_func){ // including malloc
                /*
                    If we use "callq init_main_shadow_stack"
                    then we get this error when building Firefox79.0.

                     0:30.44 /usr/bin/ld: ../../memory/build/Unified_cpp_memory_build0.o:
                             warning: relocation against `init_main_shadow_stack'
                             in read-only section `.text.malloc'
                     0:30.46 /usr/bin/ld: read-only segment has dynamic relocations.
                     0:30.46 clang-7: error: linker command failed with exit code 1
                             (use -v to see invocation)
                 */
                // FIXME: no more than 3 arguments
                // rdi, rsi, rdx, rcx, r8, r9
                fprintf(outf, "\tpushq\t" "%%rdi\n");
                fprintf(outf, "\tpushq\t" "%%rsi\n");
                fprintf(outf, "\tpushq\t" "%%rdx\n");
                fprintf(outf, "\tcallq\t" "init_main_shadow_stack@plt\n");
                fprintf(outf, "\tpopq\t" "%%rdx\n");
                fprintf(outf, "\tpopq\t" "%%rsi\n");
                fprintf(outf, "\tpopq\t" "%%rdi\n");

                continue;
            }
            //
            fprintf(outf, "\tmovq\t(%%rsp), %%r11\n");
            fprintf(outf, "\tmovq\t$-0x%lx, %%r10\n", SPA_USER_SPACE_SIZE);
            fprintf(outf, "\tmovq\t%%r11, %%gs:(%%rsp, %%r10, 1)\n");
            continue;

// USE_SHADESMAR_GS
#elif defined(USE_SHADESMAR_GS)
            //
            fprintf(outf, "%s", line);
            //
            if(in_no_instr_func){
                continue;
            }

            /*
                workaroud,
                a protected malloc() might be called before initialization of gs register.
             */
            if(in_customized_func){ // including malloc
                /*
                    If we use "callq init_main_shadow_stack"
                    then we get this error when building Firefox79.0.

                     0:30.44 /usr/bin/ld: ../../memory/build/Unified_cpp_memory_build0.o:
                             warning: relocation against `init_main_shadow_stack'
                             in read-only section `.text.malloc'
                     0:30.46 /usr/bin/ld: read-only segment has dynamic relocations.
                     0:30.46 clang-7: error: linker command failed with exit code 1
                             (use -v to see invocation)
                 */
                // FIXME: no more than 3 arguments
                // rdi, rsi, rdx, rcx, r8, r9
                fprintf(outf, "\tpushq\t" "%%rdi\n");
                fprintf(outf, "\tpushq\t" "%%rsi\n");
                fprintf(outf, "\tpushq\t" "%%rdx\n");
                fprintf(outf, "\tcallq\t" "init_main_shadow_stack@plt\n");
                fprintf(outf, "\tpopq\t" "%%rdx\n");
                fprintf(outf, "\tpopq\t" "%%rsi\n");
                fprintf(outf, "\tpopq\t" "%%rdi\n");

                continue;
            }

            continue;

#elif defined(USE_SPA_FS_GS_TLS)
            fprintf(outf, "%s", line);
#if 1
            if(in_no_instr_func){
                continue;
            }

            if(on_stack_handler){
                //
//                fprintf(outf, "\tmovq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(diff));
//                fprintf(outf, "\tmovq\t%%r11, %%gs:(%ld)\n", GET_GS_METADATA_FIELD_OFFSET(saved_diff));

//                fprintf(outf, "\tmovq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(shadow_stack));
//                fprintf(outf, "\taddq\t$0x%lx, %%r11\n", DEF_BUDDY_SHADOW_STACK_SIZE);
//                fprintf(outf, "\tsubq\t%%rsp, %%r11\n");
//                fprintf(outf, "\tmovq\t%%r11, %%gs:(%ld)\n", GET_GS_METADATA_FIELD_OFFSET(diff));

                // FIXME: no more than 3 arguments
                // rdi, rsi, rdx, rcx, r8, r9
                fprintf(outf, "\tpushq\t" "%%rdi\n");
                fprintf(outf, "\tpushq\t" "%%rsi\n");
                fprintf(outf, "\tpushq\t" "%%rdx\n");
                fprintf(outf, "\tcallq\t" "unsw_inc_asm_js_crash_cnt@plt\n");
                fprintf(outf, "\tpopq\t" "%%rdx\n");
                fprintf(outf, "\tpopq\t" "%%rsi\n");
                fprintf(outf, "\tpopq\t" "%%rdi\n");

                continue;
            }
            /*
                workaroud,
                a protected malloc() might be called before initialization of gs register.
             */
            // in_malloc = 0, in_valloc = 0, in_realloc = 0, in_calloc = 0, in_free = 0, in_abort = 0;

            if(in_customized_func){ // including malloc
                /*
                    If we use "callq init_main_shadow_stack"
                    then we get this error when building Firefox79.0.

                     0:30.44 /usr/bin/ld: ../../memory/build/Unified_cpp_memory_build0.o:
                             warning: relocation against `init_main_shadow_stack'
                             in read-only section `.text.malloc'
                     0:30.46 /usr/bin/ld: read-only segment has dynamic relocations.
                     0:30.46 clang-7: error: linker command failed with exit code 1
                             (use -v to see invocation)
                 */
                // FIXME: no more than 3 arguments
                // rdi, rsi, rdx, rcx, r8, r9
                fprintf(outf, "\tpushq\t" "%%rdi\n");
                fprintf(outf, "\tpushq\t" "%%rsi\n");
                fprintf(outf, "\tpushq\t" "%%rdx\n");
                fprintf(outf, "\tcallq\t" "init_main_shadow_stack@plt\n");
                fprintf(outf, "\tpopq\t" "%%rdx\n");
                fprintf(outf, "\tpopq\t" "%%rsi\n");
                fprintf(outf, "\tpopq\t" "%%rdi\n");

                continue;
            }
#endif
            // if (r11 == gs:(shadow stack))
            //      no need to add
            // else
            //

//            fprintf(outf, "\taddq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(R));
//            fprintf(outf, "\tmovq\t%%gs:(%ld), %%r10\n", GET_GS_METADATA_FIELD_OFFSET(diff));
//            fprintf(outf, "\tcmpq\t%%r11, (%%rsp, %%r10, 1)\n");
//            fprintf(outf, "\tje\t1f\n");
//            fprintf(outf, "\tmovq\t(%%rsp), %%r11\n");
//            fprintf(outf, "\taddq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(R));
//            fprintf(outf, "\tmovq\t%%r11, (%%rsp, %%r10, 1)\n");
//            fprintf(outf, "1:\n");


            fprintf(outf, "\tmovq\t(%%rsp), %%r11\n");

            // Disable MSR
#if defined(SPA_ENABLE_GS_MSR)
            fprintf(outf, "\taddq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(R));
#endif

            fprintf(outf, "\tmovq\t%%gs:(%ld), %%r10\n", GET_GS_METADATA_FIELD_OFFSET(diff));
            fprintf(outf, "\tmovq\t%%r11, (%%rsp, %%r10, 1)\n");
            continue;

#elif defined(USE_SPA_BUDDY_STACK_TLS_WITH_STK_SIZE)
            fprintf(outf, "%s", line);
            char *reg_x = "%r11";
            char *reg_y = "%r10";
            fprintf(outf, "###SPA### FUNCTION_ENTRY\n");
            // reg_x = rsp
            fprintf(outf, "\tmovq\t" "%%rsp, %s\n", reg_x);
#if defined(NON_PIE_GLOBAL_VAR_FOR_STK_SIZE)
            // reg_y = BUDDY_CALL_STACK_SIZE_MASK
            fprintf(outf, "\tmovq\t" ".BUDDY.CALL_STACK_SIZE_MASK, %s\n", reg_y);
#else
            // reg_y = BUDDY_CALL_STACK_SIZE_MASK
            fprintf(outf, "\tmovq\t" ".BUDDY.CALL_STACK_SIZE_MASK@GOTPCREL(%%rip), %s\n", reg_y);
            fprintf(outf, "\tmovq\t" "(%s), %s\n", reg_y, reg_y);
#endif
            // reg_x &= BUDDY_CALL_STACK_SIZE_MASK
            fprintf(outf, "\tandq\t" "%s, %s\n", reg_y, reg_x);
            // rand_val =
            fprintf(outf, "\tmovq\t" "(%s, %s, 2), %s\n", reg_x, reg_y, reg_x);
            // randomize return address
            fprintf(outf, "\taddq\t" "(%%rsp), %s\n", reg_x);
            // Save it on the shadow stack
            fprintf(outf, "\tmovq\t" "%s, (%%rsp, %s, 1)\n", reg_x, reg_y);
            continue;


#else       // SE_SPA_SHADOW_STACK_PLUS_GLOBAL_RANDVAR
            fprintf(outf, "%s", line);
            char * reg_name = "%rax";
            // USE movq here.
            fprintf(outf, "\tmovq\t" "%s, %s\n", SPA_RANDOM_VAL, reg_name);
            fprintf(outf, "\taddq\t" "(%%rsp), %s\n", reg_name);
            fprintf(outf, "\tmovq\t" "%s, -%ld(%%rsp)\n", reg_name, (long)(DEF_SPA_SS_OFFSET));
            continue;
#endif

        }
    }

    // The end of a function
    if(!strncmp(line, SPA_CFI_ENDPROC, strlen(SPA_CFI_ENDPROC))){
        start2end = 0;
        n_end++;
        if(!pass_thru){
            fprintf(outf, "%s", line);
            continue;
        }
    }

#if defined(ENABLE_GS_RSP_CALL_INSTRUMENTED)

    if(!pass_thru && !skip_intel && !skip_app && !skip_csect && instr_ok && use_64bit && start2end
            && with_64_bit_cmd_option){ // ignore 32 bit now       
    // FIXME:  merge the following tow cases to avoid redundancy
#if defined(USE_SPA_GS_RSP)
        if(!strncmp(line, SPA_CALLQ_STAR, strlen(SPA_CALLQ_STAR))){ // indirect call
            // "\n" --> "\0"
            // callq *32(%rsp)               # 8-byte Folded Reload
            line[strlen(line) - 1] = 0;
            // FIXME: a little ugly
            char *comment = strstr(line, " # ");
            if(comment){
                *comment = 0;
            }
            comment = strstr(line, "\t#\t");
            if(comment){
                *comment = 0;
            }
            comment = strstr(line, " #\t");
            if(comment){
                *comment = 0;
            }
            comment = strstr(line, "\t# ");
            if(comment){
                *comment = 0;
            }

            fprintf(outf, "\tmovq\t%s, %%rax\n", line + strlen(SPA_CALLQ_STAR));
            fprintf(outf, "\tmovq\t$0x%lx, %%r11\n", SPA_PROTECTED_FUNC_MAGIC_NUM);
            fprintf(outf, "\tcmpq\t(%%rax), %%r11\n");
            fprintf(outf, "\tjne\t1f\n");

            // write randomized return address to the shadow stack
            fprintf(outf, "\tleaq\t2f(%%rip), %%r11\n");


            fprintf(outf, "\tmovq\t$-0x%lx, %%r10\n", SPA_USER_SPACE_SIZE);
            fprintf(outf, "\tmovq\t%%r11, %%gs:-8(%%rsp, %%r10, 1)\n");

            // skip the prologue of the protected function
            fprintf(outf, "\taddq\t$0x%x, %%rax\n", SPA_LENGTH_OF_PROTECTED_PROLOGUE);
            fprintf(outf, "1:\n");
            fprintf(outf, "\tcallq\t*%%rax\n");
            fprintf(outf, "2:\n");
            continue;
        }
        else if(!strncmp(line, SPA_CALLQ_NO_STAR, strlen(SPA_CALLQ_NO_STAR))
                    && line[strlen(SPA_CALLQ_NO_STAR)] != '*'){ // direct call
            // FIXME:  No need for unprotected library function
            // __tls_get_addr also needs to be specially handled? due to 16 bytes aligned?
            // callq	__tls_get_addr@PLT

            if(strstr(line, "@PLT") || strstr(line, "@plt")
                    || strstr(line, "__tls_get_addr")){  // including library functions ?
                fprintf(outf, "%s", line);
                continue;
            }
            // +1 to skip the white space, either " " or "\t"
            char * func_name_line = (line + strlen(SPA_CALLQ_NO_STAR) + 1);
            if(is_libcxx_func_called(func_name_line) || is_libc_func_called(func_name_line)){
                fprintf(outf, "%s", line);
                continue;
            }

            if(is_protected_function(func_name_line)){
                line[strlen(line) - 1] = 0;
                // delete the comments
                char *comment = strstr(line, " # ");
                if(comment){
                    *comment = 0;
                }
                comment = strstr(line, "\t#\t");
                if(comment){
                    *comment = 0;
                }
                comment = strstr(line, " #\t");
                if(comment){
                    *comment = 0;
                }
                comment = strstr(line, "\t# ");
                if(comment){
                    *comment = 0;
                }
                // write randomized return address to the shadow stack
                fprintf(outf, "\tleaq\t1f(%%rip), %%r11\n");


                fprintf(outf, "\tmovq\t$-0x%lx, %%r10\n", SPA_USER_SPACE_SIZE);
                fprintf(outf, "\tmovq\t%%r11, %%gs:-8(%%rsp, %%r10, 1)\n");


                // direct call
                fprintf(outf, "%s+%d\n", line, SPA_LENGTH_OF_PROTECTED_PROLOGUE);
                //fprintf(stderr, "%s", line);
                fprintf(outf, "1:\n");
                continue;
            }
            fprintf(outf, "%s", line);
            continue;
        }


#elif defined(USE_SPA_FS_GS_TLS)
        if(!strncmp(line, SPA_CALLQ_STAR, strlen(SPA_CALLQ_STAR))){ // indirect call
            // "\n" --> "\0"
            // callq *32(%rsp)               # 8-byte Folded Reload
            line[strlen(line) - 1] = 0;
            // FIXME: a little ugly
            char *comment = strstr(line, " # ");
            if(comment){
                *comment = 0;
            }
            comment = strstr(line, "\t#\t");
            if(comment){
                *comment = 0;
            }
            comment = strstr(line, " #\t");
            if(comment){
                *comment = 0;
            }
            comment = strstr(line, "\t# ");
            if(comment){
                *comment = 0;
            }

            fprintf(outf, "\tmovq\t%s, %%rax\n", line + strlen(SPA_CALLQ_STAR));
            fprintf(outf, "\tmovq\t$0x%lx, %%r11\n", SPA_PROTECTED_FUNC_MAGIC_NUM);
            fprintf(outf, "\tcmpq\t(%%rax), %%r11\n");
            fprintf(outf, "\tjne\t1f\n");

            // write randomized return address to the shadow stack
            fprintf(outf, "\tleaq\t2f(%%rip), %%r11\n");

            // Disable MSR
#if defined(SPA_ENABLE_GS_MSR)
            fprintf(outf, "\taddq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(R));
#endif

            fprintf(outf, "\tmovq\t%%gs:(%ld), %%r10\n", GET_GS_METADATA_FIELD_OFFSET(diff));
            fprintf(outf, "\tmovq\t%%r11, -8(%%rsp, %%r10, 1)\n");
            // skip the prologue of the protected function
            fprintf(outf, "\taddq\t$0x%x, %%rax\n", SPA_LENGTH_OF_PROTECTED_PROLOGUE);
            fprintf(outf, "1:\n");
            fprintf(outf, "\tcallq\t*%%rax\n");
            fprintf(outf, "2:\n");
            continue;
        }
        else if(!strncmp(line, SPA_CALLQ_NO_STAR, strlen(SPA_CALLQ_NO_STAR))
                    && line[strlen(SPA_CALLQ_NO_STAR)] != '*'){ // direct call
            // FIXME:  No need for unprotected library function
            // __tls_get_addr also needs to be specially handled? due to 16 bytes aligned?
            // callq	__tls_get_addr@PLT

            if(strstr(line, "@PLT") || strstr(line, "@plt")
                    || strstr(line, "__tls_get_addr")){  // including library functions ?
                fprintf(outf, "%s", line);
                continue;
            }
            // +1 to skip the white space, either " " or "\t"
            char * func_name_line = (line + strlen(SPA_CALLQ_NO_STAR) + 1);
            if(is_libcxx_func_called(func_name_line) || is_libc_func_called(func_name_line)){
                fprintf(outf, "%s", line);
                continue;
            }

            if(is_protected_function(func_name_line)){
                line[strlen(line) - 1] = 0;
                // delete the comments
                char *comment = strstr(line, " # ");
                if(comment){
                    *comment = 0;
                }
                comment = strstr(line, "\t#\t");
                if(comment){
                    *comment = 0;
                }
                comment = strstr(line, " #\t");
                if(comment){
                    *comment = 0;
                }
                comment = strstr(line, "\t# ");
                if(comment){
                    *comment = 0;
                }
                // write randomized return address to the shadow stack
                fprintf(outf, "\tleaq\t1f(%%rip), %%r11\n");

                // Disable MSR
#if defined(SPA_ENABLE_GS_MSR)
                fprintf(outf, "\taddq\t%%gs:%ld, %%r11\n", GET_GS_METADATA_FIELD_OFFSET(R));
#endif

                fprintf(outf, "\tmovq\t%%gs:%ld, %%r10\n", GET_GS_METADATA_FIELD_OFFSET(diff));
                fprintf(outf, "\tmovq\t%%r11, -8(%%rsp, %%r10, 1)\n");


                // direct call
                fprintf(outf, "%s+%d\n", line, SPA_LENGTH_OF_PROTECTED_PROLOGUE);
                //fprintf(stderr, "%s", line);
                fprintf(outf, "1:\n");
                continue;
            }
            fprintf(outf, "%s", line);
            continue;
        }
#endif
    }
#endif

    if(!pass_thru && !skip_intel && !skip_app && use_64bit && start2end){ // ignore 32 bit now

        if(!strncmp(line, SPA_GCC_RET, strlen(SPA_GCC_RET))
                || !strncmp(line, SPA_CLANG_RETQ, strlen(SPA_CLANG_RETQ))){

          //if(!strncmp(line, SPA_CLANG_RETQ, strlen(SPA_CLANG_RETQ))){
#if defined(USE_SPA_SHADOW_STACK)
            fprintf(outf, "\taddq\t$8, %%rsp\n");
            fprintf(outf, "\tmovq\t-%ld(%%rsp), %%r11\n", (DEF_SPA_SS_OFFSET));
            fprintf(outf, "\tjmpq\t*%%r11\n");
            continue;


#elif defined(USE_SPA_SHADOW_STACK_VIA_REG)
            fprintf(outf, "\tmovq\t-%ld(%%rsp), %%r11\n", (DEF_SPA_SS_OFFSET));
            fprintf(outf, "\taddq\t$8, %%rsp\n");
            fprintf(outf, "\tjmpq\t*%%r11\n");
            continue;


#elif defined(USE_SPA_BUDDY_STACK_TLS)
            char * reg_x = "%r11";
            char * reg_y = "%r10";
            fprintf(outf, "###SPA### FUNCTION_EXIT\n");
            // Load the random value and do de-randomization
            fprintf(outf, "\tmovq\t" "%%rsp, %s\n", reg_y);
            fprintf(outf, "\tandq\t" "$%ld, %s\n", DEF_BUDDY_CALL_STACK_SIZE_MASK, reg_y);
            // Load randomized ret addr
            fprintf(outf, "\tmovq\t" "-%ld(%%rsp), %s\n", (long)(DEF_SPA_SS_OFFSET), reg_x);
            // de-randomization
            fprintf(outf, "\tsubq\t" "-%ld(%s), %s\n", (long)(DEF_BUDDY_FUNCTION_LOCAL_STORAGE_SIZE), reg_y, reg_x);
            // adjust call stack pointer
            fprintf(outf, "\taddq\t" "$%ld, %%rsp\n", (long)(SPA_CPU_WORD_LENGTH));
            // jump to call-site
            fprintf(outf, "\tjmp\t"  "*%s\n", reg_x);
            continue;

#elif defined(USE_SPA_GS_RSP)

            if(in_customized_func || in_no_instr_func){
                fprintf(outf, "%s", line);
                continue;
            }

            if(on_stack_handler){
                fprintf(outf, "%s", line);
                continue;
            }

            // 22 bytes
            /*
                 598:	49 ba 00 00 00 00 00 	movabs $0xffff800000000000,%r10
                 59f:	80 ff ff
                 5a2:	65 4e 8b 1c 14       	mov    %gs:(%rsp,%r10,1),%r11
                 5a7:	48 83 c4 08          	add    $0x8,%rsp
                 5ab:	41 ff e3             	jmpq   *%r11
             */
            // 20 bytes
            /*
                 598:	49 ba 00 00 00 00 00 	movabs $0xffff800000000000,%r10
                 59f:	80 ff ff
                 5a2:	48 83 c4 08          	add    $0x8,%rsp
                 5a6:	65 42 ff 64 14 f8    	jmpq   *%gs:-0x8(%rsp,%r10,1)

             */
            // epilogue
            fprintf(outf, "\tmovq\t$-0x%lx, %%r10\n", SPA_USER_SPACE_SIZE);
            fprintf(outf, "\taddq\t" "$%ld, %%rsp\n", (long)(SPA_CPU_WORD_LENGTH));
            fprintf(outf, "\tjmpq\t*%%gs:-8(%%rsp, %%r10, 1)\n");
            continue;

// USE_SHADESMAR_GS
#elif defined(USE_SHADESMAR_GS)
            fprintf(outf, "%s", line);
            continue;

#elif defined(USE_SPA_FS_GS_TLS)

            if(in_customized_func || in_no_instr_func){
                fprintf(outf, "%s", line);
                continue;
            }

            if(on_stack_handler){

//                // FIXME: save return values ?
//                fprintf(outf, "\tpushq\t" "%%rax\n");
//                fprintf(outf, "\tpushq\t" "%%rdx\n");
//                fprintf(outf, "\tcallq\t" "unsw_inc_asm_js_crash_cnt@plt\n");
//                fprintf(outf, "\tpopq\t" "%%rdx\n");
//                fprintf(outf, "\tpopq\t" "%%rax\n");



//                fprintf(outf, "\tmovq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(saved_diff));
//                fprintf(outf, "\tmovq\t%%r11, %%gs:(%ld)\n", GET_GS_METADATA_FIELD_OFFSET(diff));



                fprintf(outf, "%s", line);
                continue;
            }

            fprintf(outf, "\tmovq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(diff));
            fprintf(outf, "\tmovq\t(%%rsp, %%r11, 1), %%r11\n");

            // Disable MSR
#if defined(SPA_ENABLE_GS_MSR)
            fprintf(outf, "\tsubq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(R));
#endif

            fprintf(outf, "\taddq\t" "$%ld, %%rsp\n", (long)(SPA_CPU_WORD_LENGTH));
            fprintf(outf, "\tjmpq\t*%%r11\n");


//            fprintf(outf, "\tmovq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(diff));
//            fprintf(outf, "\tmovq\t(%%rsp, %%r11, 1), %%r10\n");
//            fprintf(outf, "\tsubq\t%%gs:(%ld), %%r10\n", GET_GS_METADATA_FIELD_OFFSET(R));
//            fprintf(outf, "\taddq\t" "$%ld, %%rsp\n", (long)(SPA_CPU_WORD_LENGTH));
//            fprintf(outf, "\tjmpq\t*%%r10\n");


//            fprintf(outf, "\tmovq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(diff));
//            fprintf(outf, "\tmovq\t(%%rsp, %%r11, 1), %%r11\n");
//            fprintf(outf, "\tsubq\t%%gs:(%ld), %%r11\n", GET_GS_METADATA_FIELD_OFFSET(R));
//            fprintf(outf, "\tpopq\t%%r10\n");
//            fprintf(outf, "\tcmpq\t%%r10, %%r11\n");
//            fprintf(outf, "\tjne\t 1f\n");
//            fprintf(outf, "\tjmpq\t*%%r11\n");
//            fprintf(outf, "1:\n");
//            fprintf(outf, "\tud2\n");

            continue;

#elif defined(USE_SPA_BUDDY_STACK_TLS_WITH_STK_SIZE)
            char *reg_x = "%r11";
            char *reg_y = "%r10";
            fprintf(outf, "###SPA### FUNCTION_EXIT\n");
            // reg_x = rsp
            fprintf(outf, "\tmovq\t" "%%rsp, %s\n", reg_x);
#if defined(NON_PIE_GLOBAL_VAR_FOR_STK_SIZE)
            // reg_y = BUDDY_CALL_STACK_SIZE_MASK
            fprintf(outf, "\tmovq\t" ".BUDDY.CALL_STACK_SIZE_MASK, %s\n", reg_y);
#else
            // reg_y = BUDDY_CALL_STACK_SIZE_MASK
            fprintf(outf, "\tmovq\t" ".BUDDY.CALL_STACK_SIZE_MASK@GOTPCREL(%%rip), %s\n", reg_y);
            fprintf(outf, "\tmovq\t" "(%s), %s\n", reg_y, reg_y);
#endif
            // reg_x &= CALL_STACK_SIZE_MASK
            fprintf(outf, "\tandq\t" "%s, %s\n", reg_y, reg_x);
            // reg_x = rand_val
            fprintf(outf, "\tmovq\t" "(%s, %s, 2), %s\n", reg_x, reg_y, reg_x);
            // reg_y = saved return address
            fprintf(outf, "\tmovq\t" "(%%rsp, %s, 1), %s\n", reg_y, reg_y);
            fprintf(outf, "\tsubq\t" "%s, %s\n", reg_x, reg_y);
            // adjust call stack pointer
            fprintf(outf, "\taddq\t" "$%ld, %%rsp\n", (long)(SPA_CPU_WORD_LENGTH));
            // jump to call-site
            fprintf(outf, "\tjmp\t"  "*%s\n", reg_y);
            continue;


#else       // USE_SPA_SHADOW_STACK_PLUS_GLOBAL_RANDVAR
            char * reg_name = "%r11";
            //
            fprintf(outf, "\tmovq\t" "-%ld(%%rsp), %s\n", (long)(DEF_SPA_SS_OFFSET), reg_name);
            fprintf(outf, "\tsubq\t" "%s, %s\n", SPA_RANDOM_VAL, reg_name);
            fprintf(outf, "\taddq\t" "$%ld, %%rsp\n", (long)(SPA_CPU_WORD_LENGTH));
            fprintf(outf, "\tjmp\t"  "*%s\n", reg_name);
            continue;
#endif
        }
    }

    /* Output the actual line, call it a day in pass-thru mode. */

    fputs(line, outf);

    if (pass_thru) continue;

    /* All right, this is where the actual fun begins. For one, we only want to
       instrument the .text section. So, let's keep track of that in processed
       files - and let's set instr_ok accordingly. */

    if (line[0] == '\t' && line[1] == '.') {

      /* OpenBSD puts jump tables directly inline with the code, which is
         a bit annoying. They use a specific format of p2align directives
         around them, so we use that as a signal. */

      if (!clang_mode && instr_ok && !strncmp(line + 2, "p2align ", 8) &&
          isdigit(line[10]) && line[11] == '\n') skip_next_label = 1;

      if (!strncmp(line + 2, "text\n", 5) ||
          !strncmp(line + 2, "section\t.text", 13) ||
          !strncmp(line + 2, "section\t__TEXT,__text", 21) ||
          !strncmp(line + 2, "section __TEXT,__text", 21)) {
        instr_ok = 1;
        continue;
      }

      if (!strncmp(line + 2, "section\t", 8) ||
          !strncmp(line + 2, "section ", 8) ||
          !strncmp(line + 2, "bss\n", 4) ||
          !strncmp(line + 2, "data\n", 5)) {
        instr_ok = 0;
        continue;
      }

    }

    /* Detect off-flavor assembly (rare, happens in gdb). When this is
       encountered, we set skip_csect until the opposite directive is
       seen, and we do not instrument. */

    if (strstr(line, ".code")) {

      if (strstr(line, ".code32")) skip_csect = use_64bit;
      if (strstr(line, ".code64")) skip_csect = !use_64bit;

    }

    /* Detect syntax changes, as could happen with hand-written assembly.
       Skip Intel blocks, resume instrumentation when back to AT&T. */

    if (strstr(line, ".intel_syntax")) skip_intel = 1;
    if (strstr(line, ".att_syntax")) skip_intel = 0;

    /* Detect and skip ad-hoc __asm__ blocks, likewise skipping them. */

    if (line[0] == '#' || line[1] == '#') {

      if (strstr(line, "#APP")) skip_app = 1;
      if (strstr(line, "#NO_APP")) skip_app = 0;

    }

    /* If we're in the right mood for instrumenting, check for function
       names or conditional labels. This is a bit messy, but in essence,
       we want to catch:

         ^main:      - function entry point (always instrumented)
         ^.L0:       - GCC branch label
         ^.LBB0_0:   - clang branch label (but only in clang mode)
         ^\tjnz foo  - conditional branches

       ...but not:

         ^# BB#0:    - clang comments
         ^ # BB#0:   - ditto
         ^.Ltmp0:    - clang non-branch labels
         ^.LC0       - GCC non-branch labels
         ^.LBB0_0:   - ditto (when in GCC mode)
         ^\tjmp foo  - non-conditional jumps

       Additionally, clang and GCC on MacOS X follow a different convention
       with no leading dots on labels, hence the weird maze of #ifdefs
       later on.

     */

    if (skip_intel || skip_app || skip_csect || !instr_ok ||
        line[0] == '#' || line[0] == ' ') continue;

    /* Conditional branch instruction (jnz, etc). We append the instrumentation
       right after the branch (to instrument the not-taken path) and at the
       branch destination label (handled later on). */

    if (line[0] == '\t') {

      if (line[1] == 'j' && line[2] != 'm' && R(100) < inst_ratio) {

//        fprintf(outf, use_64bit ? trampoline_fmt_64 : trampoline_fmt_32,
//                R(MAP_SIZE));
        fprintf(outf, use_64bit ? "###SPA### trampoline_fmt_64\n" : "###SPA### trampoline_fmt_32\n");
        ins_lines++;

      }

      continue;

    }

    /* Label of some sort. This may be a branch destination, but we need to
       tread carefully and account for several different formatting
       conventions. */

#ifdef __APPLE__

    /* Apple: L<whatever><digit>: */

    if ((colon_pos = strstr(line, ":"))) {

      if (line[0] == 'L' && isdigit(*(colon_pos - 1))) {

#else

    /* Everybody else: .L<whatever>: */

    if (strstr(line, ":")) {

      if (line[0] == '.') {

#endif /* __APPLE__ */

        /* .L0: or LBB0_0: style jump destination */

#ifdef __APPLE__

        /* Apple: L<num> / LBB<num> */

        if ((isdigit(line[1]) || (clang_mode && !strncmp(line, "LBB", 3)))
            && R(100) < inst_ratio) {

#else

        /* Apple: .L<num> / .LBB<num> */

        if ((isdigit(line[2]) || (clang_mode && !strncmp(line + 1, "LBB", 3)))
            && R(100) < inst_ratio) {

#endif /* __APPLE__ */

          /* An optimization is possible here by adding the code only if the
             label is mentioned in the code in contexts other than call / jmp.
             That said, this complicates the code by requiring two-pass
             processing (messy with stdin), and results in a speed gain
             typically under 10%, because compilers are generally pretty good
             about not generating spurious intra-function jumps.

             We use deferred output chiefly to avoid disrupting
             .Lfunc_begin0-style exception handling calculations (a problem on
             MacOS X). */

          if (!skip_next_label) instrument_next = 1; else skip_next_label = 0;

        }

      } else {

        /* Function label (always instrumented, deferred mode). */

        instrument_next = 1;

        // added by iron, 2020.09.01
        if(!strncmp(line, "main:", 5) && !start2end){
            in_main = 1;
            is_main_exe = 1;
        }else{
            in_main = 0;
        }
        // output function name
        if(!start2end){
            char * colon = strstr(line, ":");
            colon[0] = 0;
            fprintf(stderr, "\n###SPA_FUNCNAME### %s\n", line);
            colon[0] = ':';
        }
        /*
            Workaround for firefox:

            valloc, malloc, realloc, calloc, free, abort

         */
#if 0
        if(!strncmp(line, "malloc:", 7) && !start2end){
            in_malloc = 1;
        }else{
            in_malloc = 0;
        }
#endif
        if(is_customized_libc_func(line) && !start2end){
            in_customized_func = 1;
        }else{
            in_customized_func = 0;
        }

        if(is_no_instr_func(line) && !start2end){
            in_no_instr_func = 1;
        }else{
            in_no_instr_func = 0;
        }

        if(is_on_stack_handler(line) && !start2end){
            on_stack_handler = 1;
        }else{
            on_stack_handler = 0;
        }

      }

    }

  }

  if(is_main_exe){
      fprintf(outf, "###SPA### this module contains main().\n");
      SAYF("###SPA###  %s contains main().\n", input_file);
#if defined(USE_SPA_SHADOW_STACK_PLUS_GLOBAL_RANDVAR)
        if(!pass_thru){
            /*
          .type .unsw.randomval,@object         # @bssData
              .globl  .unsw.randomval
              .bss
              .p2align  12
          .unsw.randomval:
              .zero PAGE_SIZE
              .size .unsw.randomval, PAGE_SIZE
           */

            fprintf(outf, "\n\t.type\t.unsw.randomval, @object\n");
            fprintf(outf, "\t.globl\t.unsw.randomval\n");
            fprintf(outf, "\t.bss\n");
            fprintf(outf, "\t.p2align\t12\n");
            fprintf(outf, ".unsw.randomval:\n");
            fprintf(outf, "\t.zero\t%u\n", (u32) PAGE_SIZE);
            fprintf(outf, "\t.size\t.unsw.randomval, %u\n", (u32) PAGE_SIZE);
        }
#endif
  }

#if 0

//#if defined(USE_SPA_BUDDY_STACK_TLS_WITH_STK_SIZE)
//  if(!pass_thru){ // FIXME: including pass_thru ?

//#if defined(NON_PIE_GLOBAL_VAR_FOR_STK_SIZE)
//    if(is_main_exe){
//#endif
//      /*

//            -------------------------Weak Symbol to make the linker happy ------------------
//            .type	.BUDDY.CALL_STACK_SIZE_MASK,@object # @CALL_STACK_SIZE_MASK
//                .data
//                .weak	.BUDDY.CALL_STACK_SIZE_MASK
//                .p2align	12
//            .BUDDY.CALL_STACK_SIZE_MASK:
//                .quad	-8388608                # 0xffffffffff800000
//                .quad   0x57534E5540455343      # CSE@UNSW
//                .zero   4080
//                .size	.BUDDY.CALL_STACK_SIZE_MASK, 4096
//         */

//      fprintf(outf, "\n\t.type\t.BUDDY.CALL_STACK_SIZE_MASK, @object\n");
//      fprintf(outf, "\t.data\n");
////      if(is_main_exe){ // FIXME
////        fprintf(outf, "\t.globl\t.BUDDY.CALL_STACK_SIZE_MASK\n");
////      }else{
////        fprintf(outf, "\t.weak\t.BUDDY.CALL_STACK_SIZE_MASK\n");
////      }
//      fprintf(outf, "\t.weak\t.BUDDY.CALL_STACK_SIZE_MASK\n");
//      fprintf(outf, "\t.p2align\t12\n");
//      fprintf(outf, ".BUDDY.CALL_STACK_SIZE_MASK:\n");
//      fprintf(outf, "\t.quad\t%ld\n", (long) DEF_BUDDY_CALL_STACK_SIZE_MASK);
//      fprintf(outf, "\t.quad\t%ld\n", (long) SPA_GLOBAL_STACK_SIZE_MAGIC_NUM);
//      fprintf(outf, "\t.quad\t%ld\n", (long) SPA_GLOBAL_STACK_SIZE_MAGIC_NUM);
//      fprintf(outf, "\t.quad\t%ld\n", (long) SPA_GLOBAL_STACK_SIZE_MAGIC_NUM);
//      // FIXME: 64-bit word-length hardcoded here
//      fprintf(outf, "\t.zero\t%u\n", (u32) (PAGE_SIZE - 32));
//      fprintf(outf, "\t.size\t.BUDDY.CALL_STACK_SIZE_MASK, %u\n", (u32) PAGE_SIZE);

//#if defined(NON_PIE_GLOBAL_VAR_FOR_STK_SIZE)
//    }
//#endif
//  }
//#endif

#endif
//  if (ins_lines)
//    fputs(use_64bit ? main_payload_64 : main_payload_32, outf);

  if (ins_lines)
    fputs(use_64bit ? "###SPA### main_payload_64\n" : "###SPA### main_payload_32\n", outf);

  fprintf(outf,"###SPA### %s:  cfi_startproc = %d, cfi_endproc = %d, pass_thru = %d \n",
                     input_file, n_start, n_end, pass_thru);

  if (input_file) fclose(inf);
  fclose(outf);


  if (!be_quiet) {
    if(!pass_thru && n_start != n_end){
      FATAL("%s:  the numbers of .cfi_startproc(%d) and .cfi_endproc(%d) are different.\n",
                         input_file, n_start, n_end);
    }
    if (!ins_lines) WARNF("No instrumentation targets found%s.",
                          pass_thru ? " (pass-thru mode)" : "");
    else OKF("Instrumented %u locations (%u-startproc, %u-endproc, %s-bit, %s mode, ratio %u%%).",
             ins_lines, n_start, n_end,  use_64bit ? "64" : "32",
             getenv("AFL_HARDEN") ? "hardened" :
             (sanitizer ? "ASAN/MSAN" : "non-hardened"),
             inst_ratio);

  }

}



/* Main entry point */

int main(int argc, char** argv) {

  s32 pid;
  u32 rand_seed;
  int status;
  u8* inst_ratio_str = getenv("AFL_INST_RATIO");

  struct timeval tv;
  struct timezone tz;

  clang_mode = !!getenv(CLANG_ENV_VAR);

  if (isatty(2) && !getenv("AFL_QUIET")) {

    //SAYF(cCYA "afl-as " cBRI VERSION cRST " by <lcamtuf@google.com>\n");
    SAYF(cCYA "spa-as " cBRI SPA_VERSION cRST "\n");
  } else be_quiet = 1;

  if (argc < 2) {

    SAYF("\n"
         "It is a wrapper around GNU 'as',\n"
         "executed by the toolchain whenever using afl-clang or afl-clang++. \n"
         "You probably don't want to run this program directly.\n\n"
        );

    exit(1);

  }

  gettimeofday(&tv, &tz);

  rand_seed = tv.tv_sec ^ tv.tv_usec ^ getpid();

  srandom(rand_seed);

  edit_params(argc, argv);

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 0 and 100)");

  }

  if (getenv(AS_LOOP_ENV_VAR))
    FATAL("Endless loop when calling 'as' (remove '.' from your PATH)");

  setenv(AS_LOOP_ENV_VAR, "1", 1);

  /* When compiling with ASAN, we don't have a particularly elegant way to skip
     ASAN-specific branches. But we can probabilistically compensate for
     that... */

  if (getenv("AFL_USE_ASAN") || getenv("AFL_USE_MSAN")) {
    sanitizer = 1;
    inst_ratio /= 3;
  }
#if 0
  for(int i = 0; i < argc; i++){
      fprintf(stderr, "argv[%d] = %s\n", i, argv[i]);
  }
#endif
  if(strstr(argv[1], "--64")){
    with_64_bit_cmd_option = 1;
  }
  // FIXME
  //spa_open_protected_funcs_list("/home/iron/test/spa/tocttou/spa_protected_funcs.txt");
  spa_open_protected_funcs_list(getenv(SPA_PROTECTED_FUNCS_PATH_ENV));

  if (!just_version) add_instrumentation();

  if (!(pid = fork())) {

    execvp(as_params[0], (char**)as_params);
    FATAL("Oops, failed to execute '%s' - check your PATH", as_params[0]);

  }

  if (pid < 0) PFATAL("fork() failed");

  if (waitpid(pid, &status, 0) <= 0) PFATAL("waitpid() failed");

  if (!getenv("AFL_KEEP_ASSEMBLY")) unlink(modified_file);

  exit(WEXITSTATUS(status));

}
