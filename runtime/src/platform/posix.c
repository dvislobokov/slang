/* posix.c - POSIX implementation of the platform layer (Linux, macOS, BSD):
 * threads, CPU count, clocks, sleeping, time formatting, entropy, aligned
 * memory, console and environment, backtraces and the CPU profiler's
 * SIGPROF sampler and symbolizer. Files, processes, signals and users are
 * in fs_posix.c. The
 * readiness poller is in poll_epoll.c / poll_kqueue.c. */
#include "platform.h"

#if defined(__unix__) || defined(__APPLE__)

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <malloc_np.h>
#else
#include <malloc.h>
#endif
#if defined(__linux__)
#include <sys/random.h>
#endif

_Static_assert(sizeof(pthread_t) <= sizeof(uintptr_t), "pthread_t must fit in uintptr_t");

bool slang__plat_thread_start(slang__plat_thread *t, slang__plat_thread_fn fn, void *arg) {
    pthread_t th;
    if (pthread_create(&th, NULL, fn, arg) != 0) return false;
    t->handle = (uintptr_t)th;
    return true;
}

bool slang__plat_thread_start_detached(slang__plat_thread_fn fn, void *arg) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

void slang__plat_thread_join(slang__plat_thread t) {
    pthread_join((pthread_t)t.handle, NULL);
}

int64_t slang__plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int64_t)n : 1;
}

void slang__plat_yield(void) { sched_yield(); }

int64_t slang__plat_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int64_t slang__plat_wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void slang__plat_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    struct timespec ts = { (time_t)(ns / 1000000000), (long)(ns % 1000000000) };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

size_t slang__plat_strftime(char *buf, size_t cap, const char *fmt, const struct tm *tm) {
    return strftime(buf, cap, fmt, tm);
}

const char *slang__plat_strptime(const char *s, const char *fmt, struct tm *tm, long *gmtoff) {
    const char *end = strptime(s, fmt, tm);
    *gmtoff = end ? (long)tm->tm_gmtoff : 0;  /* set by %z (glibc, BSD), 0 otherwise */
    return end;
}

const char *slang__plat_localtime_path(void) { return "/etc/localtime"; }
const char *slang__plat_zoneinfo_dir(void) { return "/usr/share/zoneinfo"; }

/* ---- memory ------------------------------------------------------------ */

void *slang__plat_aligned_alloc(size_t align, size_t size) { return aligned_alloc(align, size); }
void slang__plat_aligned_free(void *p) { free(p); }

size_t slang__plat_alloc_size(void *p) {
#if defined(__APPLE__)
    return malloc_size(p);
#else
    return malloc_usable_size(p);
#endif
}

/* ---- process, console, environment -------------------------------------- */

void slang__plat_runtime_init(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
}

void slang__plat_stream_write(FILE *f, const char *p, size_t n) {
    if (n) fwrite(p, 1, n, f);
}

int64_t slang__plat_pid(void) { return (int64_t)getpid(); }

char *slang__plat_getenv(const char *name) {
    const char *v = getenv(name);
    return v ? strdup(v) : NULL;
}

int slang__plat_setenv(const char *name, const char *value) {
    int rc = value ? setenv(name, value, 1) : unsetenv(name);
    return rc == 0 ? 0 : errno;
}

extern char **environ;

char **slang__plat_environ(void) {
    size_t n = 0;
    for (char **e = environ; *e; e++) n++;
    char **v = calloc(n + 1, sizeof *v);
    if (!v) return NULL;
    for (size_t i = 0; i < n; i++) v[i] = strdup(environ[i]);
    return v;
}

void slang__plat_free_strv(char **v) {
    if (!v) return;
    for (char **p = v; *p; p++) free(*p);
    free(v);
}

int slang__plat_hostname(char *buf, size_t cap) {
    if (gethostname(buf, cap) != 0) return errno;
    buf[cap - 1] = 0;
    return 0;
}

char *slang__plat_temp_dir(void) {
    const char *t = getenv("TMPDIR");
    return strdup(t && *t ? t : "/tmp");
}

char slang__plat_path_sep(void) { return '/'; }

bool slang__plat_random_bytes(void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
#if defined(__linux__)
    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        got += (size_t)r;
    }
    if (got == n) return true;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    while (got < n) {
        size_t k = n - got > 256 ? 256 : n - got;
        if (getentropy(p + got, k) != 0) break;
        got += k;
    }
    if (got == n) return true;
#endif
    /* Fallback: /dev/urandom. */
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            close(fd);
            return false;
        }
        got += (size_t)r;
    }
    close(fd);
    return true;
}

void slang__plat_backtrace(int fd, int skip) {
    void *frames[64];
    int n = backtrace(frames, 64);
    if (n > skip) backtrace_symbols_fd(frames + skip, n - skip, fd);
}

/* ---- CPU profiling: SIGPROF sampler, frame-pointer walk, symbols ------- */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <dlfcn.h>
#if defined(__linux__)
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#elif defined(__APPLE__)
#include <sys/ucontext.h>
#endif

#define PROF_MAX_FRAMES 64

static _Thread_local uintptr_t t_stack_lo, t_stack_hi;
static slang__plat_prof_fn g_prof_fn;
static struct sigaction g_prof_old;
static int g_prof_on;

void slang__plat_thread_stack_init(void) {
    if (t_stack_hi) return;
#if defined(__linux__)
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) != 0) return;
    void *addr = NULL;
    size_t size = 0;
    if (pthread_attr_getstack(&a, &addr, &size) == 0 && addr) {
        t_stack_lo = (uintptr_t)addr;
        t_stack_hi = (uintptr_t)addr + size;
    }
    pthread_attr_destroy(&a);
#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    uintptr_t hi = (uintptr_t)pthread_get_stackaddr_np(self);
    t_stack_hi = hi;
    t_stack_lo = hi - pthread_get_stacksize_np(self);
#endif
}

/* PC, frame pointer and stack pointer of an interrupted context. */
static bool ctx_regs(void *uc_, uintptr_t *pc, uintptr_t *fp, uintptr_t *sp) {
#if defined(__linux__) && defined(__x86_64__)
    ucontext_t *uc = uc_;
    *pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    *fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    *sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    return true;
#elif defined(__linux__) && defined(__aarch64__)
    ucontext_t *uc = uc_;
    *pc = (uintptr_t)uc->uc_mcontext.pc;
    *fp = (uintptr_t)uc->uc_mcontext.regs[29];
    *sp = (uintptr_t)uc->uc_mcontext.sp;
    return true;
#elif defined(__APPLE__) && defined(__x86_64__)
    ucontext_t *uc = uc_;
    *pc = (uintptr_t)uc->uc_mcontext->__ss.__rip;
    *fp = (uintptr_t)uc->uc_mcontext->__ss.__rbp;
    *sp = (uintptr_t)uc->uc_mcontext->__ss.__rsp;
    return true;
#elif defined(__APPLE__) && defined(__aarch64__)
    ucontext_t *uc = uc_;
    *pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    *fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
    *sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
    return true;
#else
    (void)uc_; (void)pc; (void)fp; (void)sp;
    return false;
#endif
}

static void prof_handler(int sig, siginfo_t *info, void *uc) {
    (void)sig;
    (void)info;
    int saved = errno;
    uintptr_t pcs[PROF_MAX_FRAMES];
    int n = 0;
    uintptr_t pc, fp, sp;
    if (ctx_regs(uc, &pc, &fp, &sp)) {
        pcs[n++] = pc;
        uintptr_t lo = t_stack_lo, hi = t_stack_hi;
        /* Walk {saved fp, return address} pairs while they stay inside this
         * thread's stack, above the interrupted SP and strictly increasing. */
        if (hi && sp >= lo && sp < hi) {
            uintptr_t prev = sp;
            while (n < PROF_MAX_FRAMES && fp >= prev && fp + 2 * sizeof(uintptr_t) <= hi &&
                   (fp & (sizeof(uintptr_t) - 1)) == 0) {
                const uintptr_t *f = (const uintptr_t *)fp;
                uintptr_t ret = f[1];
                if (ret < 4096) break;
                pcs[n++] = ret;
                prev = fp + 1;
                fp = f[0];
            }
        }
    }
    slang__plat_prof_fn fn = __atomic_load_n(&g_prof_fn, __ATOMIC_ACQUIRE);
    if (fn && n > 0) fn(pcs, n);
    errno = saved;
}

bool slang__plat_prof_start(int hz, slang__plat_prof_fn fn) {
    if (g_prof_on || hz <= 0) return false;
    slang__plat_thread_stack_init();
    __atomic_store_n(&g_prof_fn, fn, __ATOMIC_RELEASE);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, &g_prof_old) != 0) return false;
    struct itimerval it;
    memset(&it, 0, sizeof it);
    long us = 1000000L / hz;
    if (us < 1) us = 1;
    it.it_interval.tv_sec = us / 1000000L;
    it.it_interval.tv_usec = us % 1000000L;
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_PROF, &it, NULL) != 0) {
        sigaction(SIGPROF, &g_prof_old, NULL);
        return false;
    }
    g_prof_on = 1;
    return true;
}

void slang__plat_prof_stop(void) {
    if (!g_prof_on) return;
    struct itimerval it;
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_PROF, &it, NULL);
    /* A signal already in flight still finds a handler; it sees fn == NULL
     * once the store below is visible. Keep our handler installed (ignoring
     * SIGPROF afterwards would be equivalent). */
    __atomic_store_n(&g_prof_fn, (slang__plat_prof_fn)NULL, __ATOMIC_RELEASE);
    g_prof_on = 0;
}

/* -- symbols of the main executable (ELF .symtab on Linux) -- */

typedef struct { uintptr_t addr, size; const char *name; } sym_ent;
static sym_ent *g_syms;
static size_t g_nsyms;
static char *g_symstr;        /* copy of the string table */
static int g_syms_loaded;
static uintptr_t g_exe_lo, g_exe_hi, g_exe_bias;
static char g_exe_path[512];

#if defined(__linux__)
static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    (void)data;
    /* The first object is the main program. */
    g_exe_bias = (uintptr_t)info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
            uintptr_t lo = g_exe_bias + ph->p_vaddr, hi = lo + ph->p_memsz;
            if (!g_exe_lo || lo < g_exe_lo) g_exe_lo = lo;
            if (hi > g_exe_hi) g_exe_hi = hi;
        }
    }
    return 1;
}

static int sym_cmp(const void *a, const void *b) {
    const sym_ent *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static void load_syms(void) {
    g_syms_loaded = 1;
    dl_iterate_phdr(phdr_cb, NULL);
    ssize_t pl = readlink("/proc/self/exe", g_exe_path, sizeof g_exe_path - 1);
    if (pl <= 0) return;
    g_exe_path[pl] = 0;
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(ElfW(Ehdr))) {
        close(fd);
        return;
    }
    size_t len = (size_t)st.st_size;
    unsigned char *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    const ElfW(Ehdr) *eh = (const ElfW(Ehdr) *)m;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_shoff == 0 ||
        eh->e_shoff + (size_t)eh->e_shnum * sizeof(ElfW(Shdr)) > len) {
        munmap(m, len);
        return;
    }
    const ElfW(Shdr) *sh = (const ElfW(Shdr) *)(m + eh->e_shoff);
    int pick = -1;
    for (int pass = 0; pass < 2 && pick < 0; pass++)
        for (int i = 0; i < eh->e_shnum; i++)
            if (sh[i].sh_type == (pass == 0 ? SHT_SYMTAB : SHT_DYNSYM)) { pick = i; break; }
    if (pick < 0 || sh[pick].sh_link >= eh->e_shnum) {
        munmap(m, len);
        return;
    }
    const ElfW(Shdr) *ss = &sh[pick], *sstr = &sh[ss->sh_link];
    if (ss->sh_offset + ss->sh_size > len || sstr->sh_offset + sstr->sh_size > len || ss->sh_entsize == 0) {
        munmap(m, len);
        return;
    }
    size_t n = ss->sh_size / ss->sh_entsize;
    g_symstr = malloc(sstr->sh_size + 1);
    g_syms = malloc((n ? n : 1) * sizeof *g_syms);
    if (!g_symstr || !g_syms) {
        free(g_symstr);
        free(g_syms);
        g_symstr = NULL;
        g_syms = NULL;
        munmap(m, len);
        return;
    }
    memcpy(g_symstr, m + sstr->sh_offset, sstr->sh_size);
    g_symstr[sstr->sh_size] = 0;
    for (size_t i = 0; i < n; i++) {
        const ElfW(Sym) *s = (const ElfW(Sym) *)(m + ss->sh_offset + i * ss->sh_entsize);
        if (ELF64_ST_TYPE(s->st_info) != STT_FUNC || s->st_value == 0 || s->st_name >= sstr->sh_size) continue;
        g_syms[g_nsyms].addr = g_exe_bias + s->st_value;
        g_syms[g_nsyms].size = s->st_size;
        g_syms[g_nsyms].name = g_symstr + s->st_name;
        g_nsyms++;
    }
    munmap(m, len);
    qsort(g_syms, g_nsyms, sizeof *g_syms, sym_cmp);
}
#else
static void load_syms(void) { g_syms_loaded = 1; }
#endif

bool slang__plat_exe_range(uintptr_t *lo, uintptr_t *hi, char *path, size_t cap) {
    if (!g_syms_loaded) load_syms();
    if (!g_exe_hi) return false;
    *lo = g_exe_lo;
    *hi = g_exe_hi;
    if (cap) snprintf(path, cap, "%s", g_exe_path);
    return true;
}

bool slang__plat_symbolize(uintptr_t pc, char *buf, size_t cap, uintptr_t *start) {
    if (!g_syms_loaded) load_syms();
    if (g_nsyms && pc >= g_syms[0].addr) {
        size_t lo = 0, hi = g_nsyms;
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (g_syms[mid].addr <= pc) lo = mid;
            else hi = mid;
        }
        const sym_ent *e = &g_syms[lo];
        if (pc < e->addr + (e->size ? e->size : 1 << 20)) {
            snprintf(buf, cap, "%s", e->name);
            *start = e->addr;
            return true;
        }
    }
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_sname) {
        snprintf(buf, cap, "%s", di.dli_sname);
        *start = (uintptr_t)di.dli_saddr;
        return true;
    }
    return false;
}

void slang__plat_symbolize_done(void) {
    free(g_syms);
    free(g_symstr);
    g_syms = NULL;
    g_symstr = NULL;
    g_nsyms = 0;
    g_syms_loaded = 0;
    g_exe_lo = g_exe_hi = 0;
}

/* ---- dynamic loading (std.plugin) ---- */

void *slang__plat_dl_open(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }

void *slang__plat_dl_sym(void *h, const char *name) { return h ? dlsym(h, name) : NULL; }

void slang__plat_dl_close(void *h) {
    if (h) dlclose(h);
}

const char *slang__plat_dl_error(void) {
    const char *e = dlerror();
    return e ? e : "unknown error";
}

#endif /* POSIX; Windows: win32.c */
