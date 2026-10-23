/* SacredFire/OCTR on Apple Silicon: can an x86-64 build under Rosetta 2 do what the port needs?
 *
 * Build and run (macOS, Xcode command line tools):  clang -arch x86_64 -O1 -o smoke smoke.c && ./smoke
 * run.sh does this and writes report.txt. Each test prints PASS / FAIL / INFO; nothing here is
 * game data, and the program only touches memory it maps itself.
 *
 * What the port relies on (docs/sacredfire/10-other-platforms.md):
 *   1. Guest instructions (PS4 Jaguar: AVX, F16C, BMI1, LZCNT, MOVBE, POPCNT) run under Rosetta.
 *   2. The eboot at 0x800000000 and guest memory at fixed addresses, with one physical page
 *      seen at two addresses (direct memory aliasing), at 4 KiB granularity.
 *   3. Guest TLS: the Windows port rewrites `mov rax, fs:[0]` to `mov rax, gs:[slot]`. On macOS
 *      x86-64, GS points at the pthread TSD array, so gs:[8*key] is pthread_getspecific(key).
 *   4. Write tracking: mprotect a page, catch the fault, record it, unprotect, resume.
 *   5. Code written at run time (the loader patches the eboot) runs, and runs again after a patch.
 */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#ifndef __x86_64__
#error "build with -arch x86_64: the port runs x86-64 guest code, so the host process must be x86-64 (Rosetta 2)"
#endif

static int failures;
/* The condition is evaluated once: some tests pass a call (ftruncate runs once per object). */
#define RESULT(cond, ...) do { int ok_ = (cond); printf("%s  ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); if (!ok_) failures++; } while (0)
#define INFO(...) do { printf("INFO  "); printf(__VA_ARGS__); putchar('\n'); } while (0)

/* ---- 0. host ------------------------------------------------------------------------------ */
static void print_host(void) {
    char brand[128] = "?"; size_t n = sizeof brand;
    sysctlbyname("machdep.cpu.brand_string", brand, &n, NULL, 0);
    int translated = -1; n = sizeof translated;
    if (sysctlbyname("sysctl.proc_translated", &translated, &n, NULL, 0)) translated = 0;
    INFO("cpu: %s", brand);
    INFO("page size seen by this process: %ld", sysconf(_SC_PAGESIZE));
    RESULT(translated == 1, "running under Rosetta 2 (sysctl.proc_translated=%d)", translated);
}

/* ---- 1. instructions ---------------------------------------------------------------------- */
/* Encoded by hand so no assembler or -m flag decides what is emitted. SysV: all used registers
 * are caller-saved; rdi is the first argument (a valid pointer for movbe). */
__asm__(
    ".text\n"
    "_t_avx:    .byte 0xc5,0xf4,0x58,0xc2\n vzeroupper\n ret\n"          /* vaddps ymm0,ymm1,ymm2 */
    "_t_avx2:   .byte 0xc5,0xf5,0xfe,0xc2\n vzeroupper\n ret\n"          /* vpaddd ymm0,ymm1,ymm2 */
    "_t_f16c:   .byte 0xc4,0xe2,0x79,0x13,0xc1\n ret\n"                  /* vcvtph2ps xmm0,xmm1 */
    "_t_bmi1:   .byte 0xc4,0xe2,0x70,0xf2,0xc2\n ret\n"                  /* andn eax,ecx,edx */
    "_t_lzcnt:  mov $0x10, %ecx\n .byte 0xf3,0x0f,0xbd,0xc1\n ret\n"     /* lzcnt eax,ecx -> 27 */
    "_t_movbe:  .byte 0x0f,0x38,0xf0,0x07\n ret\n"                       /* movbe eax,[rdi] */
    "_t_popcnt: mov $0xff, %ecx\n .byte 0xf3,0x0f,0xb8,0xc1\n ret\n"     /* popcnt eax,ecx -> 8 */
);
extern uint32_t t_avx(void), t_avx2(void), t_f16c(void), t_bmi1(void), t_lzcnt(void),
                t_movbe(const void *), t_popcnt(void);

static sigjmp_buf ill_jump;
static void on_sigill(int sig) { (void)sig; siglongjmp(ill_jump, 1); }

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static void test_instructions(void) {
    uint32_t r1[4], r7[4], r81[4];
    cpuid(1, 0, r1); cpuid(7, 0, r7); cpuid(0x80000001, 0, r81);
    INFO("cpuid: avx=%u f16c=%u movbe=%u popcnt=%u avx2=%u bmi1=%u bmi2=%u lzcnt=%u sse4a=%u",
         (r1[2] >> 28) & 1, (r1[2] >> 29) & 1, (r1[2] >> 22) & 1, (r1[2] >> 23) & 1,
         (r7[1] >> 5) & 1, (r7[1] >> 3) & 1, (r7[1] >> 8) & 1, (r81[2] >> 5) & 1, (r81[2] >> 6) & 1);
    /* Run 1 (M2 Pro, macOS 26.2): cpuid reported none of AVX/F16C/BMI1/LZCNT/MOVBE, yet all of them
     * executed correctly below. Under Rosetta the execution results count, not cpuid. */

    struct sigaction sa = {0}, old;
    sa.sa_handler = on_sigill; sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, &old);
    static const uint32_t word = 0x11223344;
    struct { const char *name; int kind; } tests[] = {
        {"AVX (vaddps ymm)", 0}, {"AVX2 (vpaddd ymm)", 1}, {"F16C (vcvtph2ps)", 2}, {"BMI1 (andn)", 3},
        {"LZCNT", 4}, {"MOVBE", 5}, {"POPCNT", 6},
    };
    for (unsigned i = 0; i < sizeof tests / sizeof *tests; i++) {
        volatile uint32_t got = 0; volatile int ok = 0;
        if (sigsetjmp(ill_jump, 1) == 0) {
            switch (tests[i].kind) {
            case 0: t_avx(); ok = 1; break;
            case 1: t_avx2(); ok = 1; break;
            case 2: t_f16c(); ok = 1; break;
            case 3: t_bmi1(); ok = 1; break;
            case 4: got = t_lzcnt(); ok = got == 27; break;
            case 5: got = t_movbe(&word); ok = got == 0x44332211; break;
            default: got = t_popcnt(); ok = got == 8; break;
            }
            RESULT(ok, "instruction %s%s", tests[i].name, ok ? "" : " gave a wrong result");
        } else {
            /* MOVBE/BMI1/LZCNT missing is survivable: the port rewrites them (15-isa-rewrite.md). */
            RESULT(0, "instruction %s: SIGILL%s", tests[i].name,
                   tests[i].kind >= 3 ? " (the port's ISA rewrite could replace it)" : " (no workaround: the game needs it)");
        }
    }
    sigaction(SIGILL, &old, NULL);
}

/* ---- 2. fixed addresses and aliasing ------------------------------------------------------ */
/* The runtime puts guest memory in USER_MIN..USER_MAX (src/runtime_memory.c: 64 GiB..1008 GiB) and
 * the eboot at 32 GiB. Runs 1-3 (M2 Pro, macOS 26.2): shm views worked at 8-9 GiB but failed or
 * crashed at 64 GiB and up, where Rosetta seems to keep memory of its own. MAP_FIXED replaces
 * whatever is mapped, so every address here is first claimed with mach_vm_allocate(VM_FLAGS_FIXED),
 * which fails on a used range instead of overwriting it; views then go over our own reservation. */
#define GUEST_LO 0x1000000000ull
#define GUEST_HI 0xfc00000000ull
#define VIEW (16ull << 20)

/* Every region at or above 4 GiB, plus the free gaps inside the guest window. */
static uint64_t dump_map(uint64_t *largest_gap_at) {
    mach_vm_address_t addr = 0x100000000ull; mach_vm_size_t size = 0;
    uint64_t guest_end = GUEST_LO, largest = 0, free_total = 0;
    *largest_gap_at = 0;
    INFO("memory map above 4 GiB (start-end size prot user_tag):");
    for (;;) {
        vm_region_submap_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        natural_t depth = 0;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth, (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS) break;
        printf("        %#014llx-%#014llx %10.1f MiB %c%c%c tag %u\n", (unsigned long long)addr, (unsigned long long)(addr + size),
               size / 1048576.0, info.protection & VM_PROT_READ ? 'r' : '-', info.protection & VM_PROT_WRITE ? 'w' : '-',
               info.protection & VM_PROT_EXECUTE ? 'x' : '-', info.user_tag);
        uint64_t lo = addr > GUEST_LO ? addr : GUEST_LO, hi = addr + size < GUEST_HI ? addr + size : GUEST_HI;
        if (lo < hi) {                                   /* region overlaps the guest window */
            if (lo > guest_end && lo - guest_end > largest) { largest = lo - guest_end; *largest_gap_at = guest_end; }
            if (lo > guest_end) free_total += lo - guest_end;
            if (hi > guest_end) guest_end = hi;
        }
        addr += size;
    }
    if (GUEST_HI > guest_end) {
        free_total += GUEST_HI - guest_end;
        if (GUEST_HI - guest_end > largest) { largest = GUEST_HI - guest_end; *largest_gap_at = guest_end; }
    }
    INFO("guest window 64-1008 GiB: %.1f GiB free, largest free run %.1f GiB at %#llx",
         free_total / 1073741824.0, largest / 1073741824.0, (unsigned long long)*largest_gap_at);
    return largest;
}

/* Claim [at, at+size) only if nothing is there. */
static int claim(uint64_t at, uint64_t size) {
    mach_vm_address_t a = at;
    return mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) == KERN_SUCCESS && a == at;
}
static void release(uint64_t at, uint64_t size) { mach_vm_deallocate(mach_task_self(), at, size); }

static int shm_fd(size_t size) {
    char name[64]; snprintf(name, sizeof name, "/octr-smoke-%d-%zu", (int)getpid(), size);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { INFO("shm_open: %s", strerror(errno)); return -1; }
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size)) { INFO("ftruncate(shm, %zu): %s", size, strerror(errno)); close(fd); return -1; }
    return fd;
}

static int aliased(volatile uint8_t *a, volatile uint8_t *b, size_t size) {
    a[12345 % size] = 0x5a; b[size - 1] = 0xa5;
    return b[12345 % size] == 0x5a && a[size - 1] == 0xa5;
}

/* shm: two views at base and base+VIEW, then a 3-page view at base+2*VIEW+4 KiB from offset 0x5000. */
static void try_shm(const char *label, uint64_t base) {
    if (!claim(base, 3 * VIEW)) { RESULT(0, "%s at %#llx: range already in use, skipped", label, (unsigned long long)base); return; }
    int fd = shm_fd(VIEW), e1 = 0, e2 = 0;
    uint8_t *a = MAP_FAILED, *b = MAP_FAILED;
    if (fd >= 0) { a = mmap((void *)(uintptr_t)base, VIEW, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0); e1 = errno; }
    if (a != MAP_FAILED) { b = mmap((void *)(uintptr_t)(base + VIEW), VIEW, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0); e2 = errno; }
    if (a == MAP_FAILED || b == MAP_FAILED) {
        RESULT(0, "%s at %#llx: shm views: %s", label, (unsigned long long)base, strerror(a == MAP_FAILED ? e1 : e2));
    } else {
        RESULT(aliased(a, b, VIEW), "%s at %#llx: shm page visible at both addresses", label, (unsigned long long)base);
        a[0x5000] = 0x3c;
        uint8_t *want = (uint8_t *)(uintptr_t)(base + 2 * VIEW + 0x1000);
        uint8_t *c = mmap(want, 0x3000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0x5000);
        int e3 = errno;
        RESULT(c == want && c[0] == 0x3c, "%s: shm view at a 4 KiB-aligned address and offset%s%s", label,
               c == MAP_FAILED ? ": " : "", c == MAP_FAILED ? strerror(e3) : "");
    }
    if (fd >= 0) close(fd);
    release(base, 3 * VIEW);
}

/* macOS's own aliasing: remap pages we allocated to a second address (copy=FALSE). */
static void try_remap(const char *label, uint64_t base) {
    if (!claim(base, 3 * VIEW)) { RESULT(0, "%s at %#llx: range already in use, skipped", label, (unsigned long long)base); return; }
    mach_vm_address_t dst = base + VIEW, small = base + 2 * VIEW + 0x1000; vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &dst, VIEW, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                     mach_task_self(), base, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) RESULT(0, "%s at %#llx: remap: %s", label, (unsigned long long)base, mach_error_string(kr));
    else RESULT(aliased((uint8_t *)(uintptr_t)base, (uint8_t *)(uintptr_t)dst, VIEW), "%s at %#llx: page visible at both addresses",
                label, (unsigned long long)base);
    ((volatile uint8_t *)(uintptr_t)base)[0x5000] = 0x3c;
    kr = mach_vm_remap(mach_task_self(), &small, 0x3000, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                       mach_task_self(), base + 0x5000, FALSE, &cur, &max, VM_INHERIT_NONE);
    RESULT(kr == KERN_SUCCESS && *(volatile uint8_t *)(uintptr_t)small == 0x3c,
           "%s: 4 KiB-aligned remap of 3 pages from offset 0x5000%s%s",
           label, kr != KERN_SUCCESS ? ": " : "", kr != KERN_SUCCESS ? mach_error_string(kr) : "");
    release(base, 3 * VIEW);
}

static void test_memory(void) {
    const uint64_t text_at = 0x800000000ull, text = 0x6000000;               /* eboot image range */
    int ok = claim(text_at, text);
    if (ok) { memset((void *)(uintptr_t)text_at, 0xc3, 4096); release(text_at, text); }
    RESULT(ok, "eboot range 0x800000000 + %#llx is free", (unsigned long long)text);

    uint64_t gap_at = 0, gap = dump_map(&gap_at);
    RESULT(gap >= (8ull << 30), "a free run of at least 8 GiB inside the guest window (%.1f GiB)", gap / 1073741824.0);
    try_shm("shm at 8 GiB (outside the window, worked in run 3)", 0x200000000ull);
    if (gap >= 4 * VIEW) {
        uint64_t base = (gap_at + 0xffffff) & ~0xffffffull;                 /* 16 MiB aligned */
        try_shm("shm inside the guest window", base);
        try_remap("mach_vm_remap inside the guest window", base);
    }

    /* mprotect of one 4 KiB page must leave its 16 KiB neighbours writable. */
    uint8_t *a = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    int rc = mprotect(a + 0x1000, 0x1000, PROT_READ);
    a[0x0000] = 1; a[0x2000] = 2; a[0x3fff] = 3;
    RESULT(rc == 0, "mprotect of a single 4 KiB page (neighbours stay writable)");
    munmap(a, 0x10000);
}

/* ---- 3. guest TLS through GS ------------------------------------------------------------- */
static pthread_key_t tls_key;
static uint64_t gs_read(uint64_t offset) {
    uint64_t v; __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"(offset)); return v;
}
static void *tls_thread(void *arg) {
    pthread_setspecific(tls_key, arg);
    return (void *)(uintptr_t)(gs_read(8ull * tls_key) == (uint64_t)(uintptr_t)arg);
}
static void test_tls(void) {
    if (pthread_key_create(&tls_key, NULL)) { RESULT(0, "pthread_key_create"); return; }
    INFO("pthread key %lu -> gs:[%#lx]", (unsigned long)tls_key, 8ul * tls_key);
    pthread_setspecific(tls_key, (void *)0x1234abcd);
    int ok = gs_read(8ull * tls_key) == 0x1234abcd;
    pthread_t t[8]; void *res; int all = 1;
    for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, tls_thread, (void *)(uintptr_t)(0x1000 + i));
    for (int i = 0; i < 8; i++) { pthread_join(t[i], &res); all &= res != NULL; }
    RESULT(ok && all, "gs:[8*key] reads pthread_getspecific(key) on the main thread and 8 others");
}

/* ---- 4. write tracking via faults ------------------------------------------------------- */
static volatile uintptr_t fault_addr, fault_rip; static volatile int faults;
static uint8_t *tracked;
static void on_fault(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    ucontext_t *uc = ctx;
    fault_addr = (uintptr_t)si->si_addr; fault_rip = uc->uc_mcontext->__ss.__rip; faults++;
    mprotect((void *)((uintptr_t)si->si_addr & ~(uintptr_t)0xfff), 0x1000, PROT_READ | PROT_WRITE);
}
static void test_faults(void) {
    tracked = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    struct sigaction sa = {0}, s1, s2;
    sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s1); sigaction(SIGBUS, &sa, &s2);
    mprotect(tracked + 0x4000, 0x1000, PROT_READ);
    volatile uint8_t *w = tracked + 0x4123;
    *w = 0x77;                                   /* faults once, handler unprotects, write retries */
    sigaction(SIGSEGV, &s1, NULL); sigaction(SIGBUS, &s2, NULL);
    RESULT(faults == 1 && fault_addr == (uintptr_t)w && *w == 0x77 && fault_rip != 0,
           "write fault caught with its address and rip, page unprotected, write resumed (faults=%d)", faults);
    munmap(tracked, 0x10000);
}

/* ---- 5. code patched at run time --------------------------------------------------------- */
static void test_code_patch(void) {
    uint8_t *code = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED) { RESULT(0, "RWX anonymous mapping: %s", strerror(errno)); return; }
    static const uint8_t ret1[] = {0xb8, 1, 0, 0, 0, 0xc3};                /* mov eax,1; ret */
    memcpy(code, ret1, sizeof ret1);
    int (*fn)(void) = (int (*)(void))code;
    int first = fn();
    code[1] = 2;                                                          /* patch to mov eax,2 */
    int second = fn();
    RESULT(first == 1 && second == 2, "code written, run, patched and run again (%d then %d)", first, second);
    munmap(code, 0x4000);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("OCTR macOS smoke test\n=====================\n");
    print_host();
    test_instructions();
    test_memory();
    test_tls();
    test_faults();
    test_code_patch();
    printf("=====================\n%s: %d failed\n", failures ? "SOME TESTS FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
