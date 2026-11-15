/* Native loopback queue interleaving regression; no running game required.
 * cc -std=c11 -O2 -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections \
 *   tests/test_loopback_queue.c -Wl,--gc-sections -lm -o /tmp/test-loopback
 * /tmp/test-loopback out/eboot.elf
 */
#include "../src/runtime_online_mode.c"
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <assert.h>
#include <elf.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

struct message { void *vtable; int refs, pad; void *buffer; };
struct queue {
    unsigned char prefix[0x38];
    void *head, *tail;
    unsigned count;
};
static struct queue queue;
static unsigned char (*enqueue)(void *, void *);
static unsigned char (*dequeue)(void *, void *, void *);
static void *allocations[8], *removed;
static unsigned allocated, releases;
static int interleave, quarantine;
static pthread_t receiver;
static atomic_int receiver_started;

static void pop(void) {
    unsigned char reliable = 0;
    assert(dequeue(&queue, &removed, &reliable));
    assert(removed && reliable == 1);
}

static void *receive_during_allocation(void *unused) {
    (void)unused;
    assert(pthread_mutex_trylock(&loopback_mutex) == EBUSY);
    atomic_store(&receiver_started, 1);
    pop();
    return NULL;
}

static void *allocate(size_t size) {
    assert(size == 24);
    if (interleave) {
        int threaded = interleave == 2;
        interleave = 0;
        /* Native enqueue already captured tail before this allocation. */
        if (threaded) {
            assert(!pthread_create(&receiver, NULL, receive_during_allocation, NULL));
            while (!atomic_load(&receiver_started)) sched_yield();
        } else pop();
    }
    void *node = calloc(1, size);
    assert(node);
    if (quarantine) {
        assert(allocated < 8);
        allocations[allocated] = node;
    }
    ++allocated;
    return node;
}

static void release(void *node) {
    if (!node) return;
    ++releases;
    /* Quarantine the unpatched case so stale-tail writes are deterministic. */
    if (!quarantine) free(node);
}

static void jump(unsigned char *at, void *target) {
    memcpy(at, "\xff\x25\0\0\0\0", 6);
    memcpy(at + 6, &target, sizeof target);
}

static unsigned char *load_native(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    Elf64_Ehdr header;
    assert(fread(&header, sizeof header, 1, file) == 1);
    assert(!memcmp(header.e_ident, ELFMAG, SELFMAG));
    size_t size = 0x2b00000;
    unsigned char *image = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr segment;
        assert(!fseek(file, header.e_phoff + i * header.e_phentsize, SEEK_SET));
        assert(fread(&segment, sizeof segment, 1, file) == 1);
        if (segment.p_type != PT_LOAD && segment.p_type != 0x61000010) continue;
        assert(segment.p_vaddr + segment.p_filesz <= size);
        assert(!fseek(file, segment.p_offset, SEEK_SET));
        assert(fread(image + segment.p_vaddr, 1, segment.p_filesz, file) == segment.p_filesz);
    }
    fclose(file);
    assert(!memcmp(image + 0x19f690, "\x49\x8b\x5e\x40\xbf\x18\0\0\0", 9));
    assert(!memcmp(image + 0x19f7b4, "\x41\x83\x7f\x48\0", 5));
    assert(!memcmp(image + 0x19f7e4, "\x49\x8b\x04\x24", 4));
    const uintptr_t expected[] = {0x19f920, 0x19fa20, 0x19f570, 0x19f580, 0x19f7a0};
    for (unsigned i = 0; i < 5; ++i) {
        uintptr_t entry;
        memcpy(&entry, image + 0x2423cc8 + i * 8, 8);
        assert(entry == expected[i]);
        entry += (uintptr_t)image;
        memcpy(image + 0x2423cc8 + i * 8, &entry, 8);
    }
    jump(image + 0x17bdc0, allocate);
    jump(image + 0x17bdf0, release);
    enqueue = (void *)(image + 0x19f580);
    dequeue = (void *)(image + 0x19f7a0);
    return image;
}

static void run_case(int serialized) {
    unsigned char buffers[2][0x30] = {{0}};
    struct message messages[2] = {{0}};
    memset(&queue, 0, sizeof queue);
    allocated = releases = 0;
    atomic_store(&receiver_started, 0);
    removed = NULL;
    quarantine = !serialized;
    for (unsigned i = 0; i < 2; ++i) {
        *(int *)(buffers[i] + 8) = 100;
        messages[i].refs = 100;
        messages[i].buffer = buffers[i];
    }
    struct message *first = &messages[0], *second = &messages[1];
    assert(enqueue(&queue, &first));
    assert(queue.count == 1 && queue.head == queue.tail);
    interleave = serialized ? 2 : 1;
    assert(enqueue(&queue, &second));
    if (!serialized) {
        assert(queue.count == 1 && !queue.head && queue.tail);
        assert(releases == 1 && removed == first);
        /* The next real native pop reproduces the NULL dereference. */
        pid_t child = fork();
        assert(child >= 0);
        if (!child) { pop(); _exit(0); }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    } else {
        assert(!pthread_join(receiver, NULL));
        assert(removed == first && queue.count == 1 && queue.head == queue.tail);
        pop();
        assert(removed == second && !queue.count && !queue.head && !queue.tail);
        assert(releases == 2);
    }
    if (quarantine) for (unsigned i = 0; i < allocated; ++i) free(allocations[i]);
}

enum { MESSAGES = 20000, SENDERS = 2 };
struct packet { struct message message; unsigned char buffer[0x30]; unsigned sender, sequence; };
static struct packet packets[SENDERS][MESSAGES];

static void *send_many(void *arg) {
    unsigned sender = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < MESSAGES; ++i) {
        struct packet *packet = &packets[sender][i];
        packet->sender = sender;
        packet->sequence = i;
        packet->message.refs = 100;
        packet->message.buffer = packet->buffer;
        *(int *)(packet->buffer + 8) = 100;
        void *message = &packet->message;
        assert(enqueue(&queue, &message));
    }
    return NULL;
}

static void stress(void) {
    memset(&queue, 0, sizeof queue);
    allocated = releases = 0;
    pthread_t senders[SENDERS];
    for (unsigned i = 0; i < SENDERS; ++i)
        assert(!pthread_create(&senders[i], NULL, send_many, (void *)(uintptr_t)i));
    unsigned next[SENDERS] = {0};
    struct packet *packet = NULL;
    for (unsigned i = 0; i < SENDERS * MESSAGES;) {
        unsigned char reliable = 0;
        if (!dequeue(&queue, &packet, &reliable)) { sched_yield(); continue; }
        assert(reliable == 1 && packet->sender < SENDERS);
        assert(packet->sequence == next[packet->sender]++);
        ++i;
    }
    for (unsigned i = 0; i < SENDERS; ++i) {
        assert(!pthread_join(senders[i], NULL));
        assert(next[i] == MESSAGES);
    }
    --packet->message.refs;
    assert(!queue.count && !queue.head && !queue.tail);
    assert(allocated == SENDERS * MESSAGES && releases == allocated);
    for (unsigned s = 0; s < SENDERS; ++s)
        for (unsigned i = 0; i < MESSAGES; ++i) {
            assert(packets[s][i].message.refs == 100);
            assert(*(int *)(packets[s][i].buffer + 8) == 100);
        }
}

static unsigned destroyed;
static void message_destroy(void *message) {
    assert(((struct message *)message)->refs == 0);
    ++destroyed;
}

static void destroy_cases(unsigned char *image) {
    void *vtable[] = {NULL, message_destroy};
    for (unsigned deleting = 0; deleting < 2; ++deleting) {
        struct queue *self = calloc(1, sizeof *self);
        unsigned char buffer[0x30] = {0};
        *(int *)(buffer + 8) = 100;
        struct message message = {vtable, 1, 0, buffer};
        void *reference = &message;
        assert(enqueue(self, &reference));
        assert(message.refs == 2);
        --message.refs;
        NativeEvent destructor;
        memcpy(&destructor, image + 0x2423cc8 + deleting * 8, 8);
        unsigned before = releases;
        destructor((void *)self);
        assert(message.refs == 0 && destroyed == deleting + 1);
        assert(releases == before + 1 + deleting);
        if (!deleting) {
            assert(!self->count && !self->head && !self->tail);
            free(self);
        }
    }
    /* Replacing the output reference releases its last owner exactly once. */
    memset(&queue, 0, sizeof queue);
    unsigned char buffers[2][0x30] = {{0}};
    struct message messages[2];
    for (unsigned i = 0; i < 2; ++i) {
        *(int *)(buffers[i] + 8) = 100;
        messages[i] = (struct message){vtable, 1, 0, buffers[i]};
        void *reference = &messages[i];
        assert(enqueue(&queue, &reference));
        --messages[i].refs;
    }
    removed = NULL;
    pop();
    assert(removed == &messages[0] && messages[0].refs == 1);
    pop();
    assert(removed == &messages[1] && messages[0].refs == 0 && destroyed == 3);
    --messages[1].refs;
    message_destroy(&messages[1]);
    assert(destroyed == 4);
    unsigned char reliable = 0;
    assert(!dequeue(&queue, &removed, &reliable));
    assert(!queue.count && !queue.head && !queue.tail);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    struct rlimit no_core = {0, 0};
    assert(!setrlimit(RLIMIT_CORE, &no_core));
    unsigned char *image = load_native(argv[1]);
    alarm(30);
    run_case(0);
    /* A mismatched vtable must leave every entry untouched. */
    unsigned char saved[40];
    memcpy(saved, image + 0x2423cc8, sizeof saved);
    image[0x2423ce0] ^= 1;
    assert(!loopback_queue_start(image));
    image[0x2423ce0] ^= 1;
    assert(!memcmp(saved, image + 0x2423cc8, sizeof saved));
    assert(loopback_queue_start(image));
    memcpy(&enqueue, image + 0x2423ce0, 8);
    memcpy(&dequeue, image + 0x2423ce8, 8);
    run_case(1);
    stress();
    destroy_cases(image);
    assert(!munmap(image, 0x2b00000));
    puts("Native crash reproduced; patched concurrent FIFO, reference ownership and destruction pass.");
}
