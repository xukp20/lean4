/* One private Lean 4.35 Linux x86-64 process. Fixed mmap slots, with no
   allocation, file writes, or observer locks on the sampled paths. */
#define _GNU_SOURCE
#include <lean/lean.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if !defined(__linux__) || !defined(__x86_64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "CoreTraceSink435 requires little-endian Linux x86-64"
#endif

extern uint64_t runtime_heartbeats(void) __asm__("_ZN4lean18get_num_heartbeatsEv");
extern uint64_t lbt_core_current_task_token(void) __attribute__((weak));

enum { HEADER_BYTES = 4096, CAPACITY = 1048576, NAME_BYTES = 32 * 1024 * 1024 };
enum { COMMAND = 1, PROOF, READ_IMPORTS, FINALIZE_IMPORT, LOAD_EXTS, TASK_WAIT, COMMANDS_END,
       KERNEL_ASYNC, KERNEL_CHECK, LINTER, STATEFUL_LINTER, REALIZE, SCOPE_BINDERS, SCOPE_KEEP,
       WRITE_MODULE, REFS_COLLECT, ILEAN, REPORT, MARK_PERSISTENT, CODEGEN, ASYNC_COMPILE,
       KERNEL_CHECKED_TASK };
enum { BEGIN, END, MARKER };
enum { WAIT_THRESHOLD_NS = 50000 };

struct header {
    char magic[8];
    uint32_t version, header_bytes, slot_bytes, capacity;
    uint64_t pid;
    _Atomic uint64_t reserved, published, clock_errors, finished;
    uint64_t process_cpu_start, process_cpu_end;
    _Atomic uint64_t short_waits, names_reserved;
    uint64_t process_wall_start, process_wall_end, adjusted_hb;
};

struct event {
    _Atomic uint64_t committed;
    uint64_t op, wall, cpu, tid, command, scope, start_byte, end_byte, heartbeats;
    uint32_t kind, boundary, status, padding;
    uint64_t origin, value;
};

_Static_assert(sizeof(struct header) == 120 && sizeof(struct event) == 112, "core wire layout");
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2, "core sink requires lock-free counters");

static struct header *header;
static struct event *slots;
static char *names;
static _Atomic unsigned enabled;
static _Thread_local uint64_t current_command, current_scope;
static _Thread_local uint64_t heartbeat_adjustment, wait_wall, wait_cpu, thread_id;
static _Thread_local uint64_t wait_target;

struct sample { uint64_t wall, cpu, hb; };

static uint64_t now(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock, &t) != 0 || t.tv_sec < 0) {
        atomic_fetch_add_explicit(&header->clock_errors, 1, memory_order_relaxed);
        return UINT64_MAX;
    }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static int recording(void) {
    return atomic_load_explicit(&enabled, memory_order_acquire);
}

static void finish(void) {
    atomic_store_explicit(&enabled, 0, memory_order_release);
    header->process_wall_end = now(CLOCK_MONOTONIC);
    header->process_cpu_end = now(CLOCK_PROCESS_CPUTIME_ID);
    atomic_store_explicit(&header->finished, 1, memory_order_release);
}

static uint32_t configure(const char *path, uint64_t adjusted_hb) {
    if (header) return EALREADY;
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    size_t size = HEADER_BYTES + CAPACITY * sizeof(struct event) + NAME_BYTES;
    if (ftruncate(fd, (off_t)size) != 0) {
        int error = errno;
        close(fd);
        return error;
    }
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int error = errno;
    close(fd);
    if (mapping == MAP_FAILED) return error;
    header = mapping;
    slots = (struct event *)((char *)mapping + HEADER_BYTES);
    names = (char *)(slots + CAPACITY);
    memcpy(header->magic, "LBTCORE2", 8);
    header->version = 2;
    header->header_bytes = HEADER_BYTES;
    header->slot_bytes = sizeof(struct event);
    header->capacity = CAPACITY;
    header->pid = (uint64_t)getpid();
    header->adjusted_hb = adjusted_hb;
    header->process_wall_start = now(CLOCK_MONOTONIC);
    header->process_cpu_start = now(CLOCK_PROCESS_CPUTIME_ID);
    if (atexit(finish) != 0) return EIO;
    atomic_store_explicit(&enabled, 1, memory_order_release);
    return 0;
}

LEAN_EXPORT uint32_t lbt_core_configure(b_lean_obj_arg path) {
    return configure(lean_string_cstr(path), 0);
}

LEAN_EXPORT uint32_t lbt_core_configure_env(void) {
    const char *path = getenv("LEAN_BUILD_TRACE_CORE");
    return path && *path ? configure(path, 1) : 0;
}

LEAN_EXPORT void lbt_core_adjust_heartbeats(uint64_t before, uint64_t after) {
    if (recording()) heartbeat_adjustment += after - before;
}

static struct sample sample(uint32_t kind) {
    struct sample stamp;
    stamp.wall = now(CLOCK_MONOTONIC);
    stamp.cpu = now(CLOCK_THREAD_CPUTIME_ID);
    stamp.hb = (kind == PROOF || kind == COMMAND || kind == SCOPE_BINDERS)
        ? runtime_heartbeats() - heartbeat_adjustment : 0;
    return stamp;
}

static uint64_t record_at(uint32_t kind, uint32_t boundary, uint32_t status, uint64_t op,
                          uint64_t command, uint64_t scope, uint64_t lo, uint64_t hi,
                          uint64_t origin, uint64_t value, struct sample stamp) {
    uint64_t i = atomic_fetch_add_explicit(&header->reserved, 1, memory_order_relaxed);
    if (i >= CAPACITY) return 0;
    if (!op) op = i + 1;
    struct event *row = &slots[i];
    row->wall = stamp.wall;
    row->cpu = stamp.cpu;
    if (!thread_id) thread_id = (uint64_t)gettid();
    row->tid = thread_id;
    row->heartbeats = stamp.hb;
    row->op = op;
    row->command = command;
    row->scope = scope;
    row->start_byte = lo;
    row->end_byte = hi;
    row->kind = kind;
    row->boundary = boundary;
    row->status = status;
    row->origin = origin;
    row->value = value;
    atomic_store_explicit(&row->committed, i + 1, memory_order_release);
    atomic_fetch_add_explicit(&header->published, 1, memory_order_relaxed);
    return op;
}

static uint64_t record(uint32_t kind, uint32_t boundary, uint32_t status, uint64_t op,
                       uint64_t command, uint64_t scope, uint64_t lo, uint64_t hi,
                       uint64_t origin, uint64_t value) {
    return record_at(kind, boundary, status, op, command, scope, lo, hi, origin, value, sample(kind));
}

static uint64_t begin(uint32_t kind) {
    if (!recording()) return 0;
    return record(kind, BEGIN, 0, 0, current_command, current_scope, UINT64_MAX, UINT64_MAX, 0, 0);
}

static void end(uint32_t kind, uint64_t op, uint32_t status, uint64_t value) {
    if (!op || !recording()) return;
    const struct event *entry = &slots[op - 1];
    record(kind, END, status, op, entry->command, entry->scope, entry->start_byte, entry->end_byte,
           entry->origin, value);
}

LEAN_EXPORT uint8_t lbt_core_enabled(void) { return (uint8_t)recording(); }
LEAN_EXPORT uint64_t lbt_core_current_command(void) { return current_command; }
LEAN_EXPORT uint64_t lbt_core_current_scope(void) { return current_scope; }

static uint64_t command_of(uint64_t scope) {
    if (!scope) return 0;
    return slots[scope - 1].kind == COMMAND ? scope : slots[scope - 1].command;
}

LEAN_EXPORT uint64_t lbt_core_command_begin(uint64_t lo, uint64_t hi) {
    if (!recording()) return 0;
    uint64_t op = record(COMMAND, BEGIN, 0, 0, current_command, current_scope, lo, hi, 0, 0);
    if (op) current_command = current_scope = op;
    return op;
}

LEAN_EXPORT uint64_t lbt_core_proof_begin(uint64_t command, uint64_t lo, uint64_t hi) {
    if (!recording()) return 0;
    uint64_t op = record(PROOF, BEGIN, 0, 0, command, current_scope, lo, hi, command, 0);
    if (op) {
        current_command = command;
        current_scope = op;
    }
    return op;
}

static void close_scope(uint32_t kind, uint64_t op, uint32_t status, uint64_t value) {
    if (op && op == current_scope) {
        const struct event *entry = &slots[op - 1];
        end(kind, op, status, value);
        current_scope = entry->scope;
        current_command = command_of(current_scope);
    }
}

LEAN_EXPORT lean_obj_res lbt_core_scope_end(uint32_t kind, uint64_t op, uint32_t status) {
    close_scope(kind, op, status, 0);
    return lean_box(0);
}

LEAN_EXPORT lean_obj_res lbt_core_commands_end(void) {
    if (recording())
        record(COMMANDS_END, MARKER, 1, 0, 0, 0, UINT64_MAX, UINT64_MAX, 0, 0);
    return lean_box(0);
}

LEAN_EXPORT uint64_t lbt_core_span_begin(uint32_t kind) { return begin(kind); }
LEAN_EXPORT lean_obj_res lbt_core_span_end(uint32_t kind, uint64_t op, uint32_t status) {
    end(kind, op, status, 0);
    return lean_box(0);
}

LEAN_EXPORT uint64_t lbt_core_work_begin(uint32_t kind, uint64_t origin, b_lean_obj_arg label) {
    if (!recording()) return 0;
    uint64_t length = lean_string_size(label) - 1;
    uint64_t offset = atomic_fetch_add_explicit(&header->names_reserved, length, memory_order_relaxed);
    uint64_t command = command_of(origin);
    if (offset <= NAME_BYTES && length <= NAME_BYTES - offset)
        memcpy(names + offset, lean_string_cstr(label), length);
    uint64_t task = kind == KERNEL_ASYNC && lbt_core_current_task_token
        ? lbt_core_current_task_token() : 0;
    uint64_t op = record(kind, BEGIN, 0, 0, command, current_scope, offset, length, origin, task);
    if (op) {
        current_scope = op;
        current_command = command;
    }
    return op;
}

LEAN_EXPORT uint64_t lbt_core_binders_begin(uint64_t active) {
    if (!active || !recording()) return 0;
    uint64_t op = record(SCOPE_BINDERS, BEGIN, 0, 0, current_command, current_scope,
                         UINT64_MAX, UINT64_MAX, 0, active);
    if (op) current_scope = op;
    return op;
}

LEAN_EXPORT lean_obj_res lbt_core_kernel_checked_task(uint64_t op, b_lean_obj_arg task) {
    if (op && recording())
        record(KERNEL_CHECKED_TASK, MARKER, 1, 0, current_command, op, UINT64_MAX, UINT64_MAX, 0,
               (uint64_t)(uintptr_t)task);
    return lean_box(0);
}

LEAN_EXPORT lean_obj_res lbt_core_binders_end(uint64_t op, uint32_t status, uint64_t rebuilt) {
    close_scope(SCOPE_BINDERS, op, status, rebuilt);
    return lean_box(0);
}

LEAN_EXPORT lean_obj_res lbt_core_kept_vars(uint64_t active, uint64_t kept) {
    if (active && recording())
        record(SCOPE_KEEP, MARKER, 1, 0, current_command, current_scope, active, kept, 0, 0);
    return lean_box(0);
}

/* C++ calls these only around actual condition-variable wait calls. */
LEAN_EXPORT uint64_t lbt_core_wait_begin_task(const void *target) {
    if (!current_scope || !recording()) return 0;
    wait_target = (uint64_t)(uintptr_t)target;
    wait_wall = now(CLOCK_MONOTONIC);
    wait_cpu = now(CLOCK_THREAD_CPUTIME_ID);
    return current_scope;
}

LEAN_EXPORT uint64_t lbt_core_wait_begin(void) { return lbt_core_wait_begin_task(NULL); }

LEAN_EXPORT void lbt_core_wait_end(uint64_t scope) {
    if (!recording()) return;
    uint64_t wall = now(CLOCK_MONOTONIC);
    if (wall >= wait_wall && wall - wait_wall < WAIT_THRESHOLD_NS) {
        atomic_fetch_add_explicit(&header->short_waits, 1, memory_order_relaxed);
        return;
    }
    struct sample start = {wait_wall, wait_cpu, 0};
    struct sample stop = {wall, now(CLOCK_THREAD_CPUTIME_ID), 0};
    uint64_t op = record_at(TASK_WAIT, BEGIN, 0, 0, current_command, scope,
                            UINT64_MAX, UINT64_MAX, 0, wait_target, start);
    if (op) record_at(TASK_WAIT, END, 1, op, current_command, scope,
                      UINT64_MAX, UINT64_MAX, 0, wait_target, stop);
}
