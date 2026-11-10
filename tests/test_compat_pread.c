/* Windows only (src/compat_win.c): two threads on one CRT descriptor, one doing pread at random
 * offsets, the other reading the file sequentially under the descriptor lock, as runtime_file.c
 * does, and checking every byte (byte i is (i * 7 + i / 251) & 0xff). Without the lock in
 * positioned I/O almost every byte came from the other thread's position (issue #217).
 *   clang -O2 -DWITH_LOCK -Isrc tests/test_compat_pread.c src/compat_win.c -lbcrypt -o out/compat-pread-test.exe */
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

ssize_t compat_pread(int fd, void *buffer, size_t size, int64_t offset);
#ifdef WITH_LOCK
void compat_fd_lock(int fd);
void compat_fd_unlock(int fd);
#define LOCK(fd) compat_fd_lock(fd)
#define UNLOCK(fd) compat_fd_unlock(fd)
#else
#define LOCK(fd) ((void)0)
#define UNLOCK(fd) ((void)0)
#endif

#define SIZE (8u << 20)
static int fd;
static volatile LONG stop;

static unsigned char byte_at(uint64_t i) { return (unsigned char)((i * 7 + i / 251) & 0xff); }

static DWORD WINAPI positioned_reader(LPVOID unused) {
    (void)unused;
    unsigned char buf[4096];
    uint64_t seed = 12345;
    while (!stop) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const int64_t at = (int64_t)((seed >> 20) % (SIZE - sizeof buf));
        compat_pread(fd, buf, sizeof buf, at);
    }
    return 0;
}

int main(void) {
    const char *path = "race_test.bin";
    FILE *f = fopen(path, "wb");
    for (uint64_t i = 0; i < SIZE; i++) fputc(byte_at(i), f);
    fclose(f);
    fd = _open(path, _O_RDONLY | _O_BINARY);
    HANDLE t = CreateThread(NULL, 0, positioned_reader, NULL, 0, NULL);
    long long wrong = 0, reads = 0;
    unsigned char buf[1000];
    for (int pass = 0; pass < 20; pass++) {
        LOCK(fd);
        _lseeki64(fd, 0, SEEK_SET);
        UNLOCK(fd);
        uint64_t pos = 0;
        for (;;) {
            LOCK(fd);
            const int n = _read(fd, buf, sizeof buf);
            UNLOCK(fd);
            if (n <= 0) break;
            reads++;
            for (int k = 0; k < n; k++) wrong += buf[k] != byte_at(pos + k);
            pos += (uint64_t)n;
        }
    }
    stop = 1;
    WaitForSingleObject(t, INFINITE);
    _close(fd);
    remove(path);
    printf("%s: %lld sequential reads, %lld wrong bytes\n",
#ifdef WITH_LOCK
           "with the lock",
#else
           "without the lock",
#endif
           reads, wrong);
    return wrong != 0;
}
