#ifndef CINC_H
#define CINC_H

#include <cilo/cilo.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cinc_error : int32_t {
  CINC_OK = 0,
  // the innermost CINC_WITHIN expired.
  CINC_ERROR_TIMEOUT = 1,
  // the task or its block was cancelled, it should return soon.
  CINC_ERROR_CANCELLED = 2,
  // the channel was closed and drained, or the peer closed the connection.
  CINC_ERROR_CLOSED = 3,
  // a system call failed, errno says why.
  CINC_ERROR_IO = 4,
} cinc_error;

/* ========================================================================================================== */
/* preprocessor plumbing.                                                                                     */
/* ========================================================================================================== */

#define CINC__CAT_(a, b) a##b
#define CINC__CAT(a, b) CINC__CAT_(a, b)
#define CINC__UNIQUE(x) CINC__CAT(x, __LINE__)
#define CINC__COMMA() ,
#define CINC__NOTHING()

// the number of arguments, up to 8.
#define CINC__COUNT(...) CINC__COUNT_I(__VA_ARGS__ __VA_OPT__(, ) 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define CINC__COUNT_I(_1, _2, _3, _4, _5, _6, _7, _8, n, ...) n

// m(arg, index) for each argument, with sep() in between.
#define CINC__MAP(m, sep, ...) CINC__CAT(CINC__MAP_, CINC__COUNT(__VA_ARGS__))(m, sep __VA_OPT__(, ) __VA_ARGS__)
#define CINC__MAP_0(m, sep)
#define CINC__MAP_1(m, sep, a) m(a, 0)
#define CINC__MAP_2(m, sep, a, b) CINC__MAP_1(m, sep, a) sep() m(b, 1)
#define CINC__MAP_3(m, sep, a, b, c) CINC__MAP_2(m, sep, a, b) sep() m(c, 2)
#define CINC__MAP_4(m, sep, a, b, c, d) CINC__MAP_3(m, sep, a, b, c) sep() m(d, 3)
#define CINC__MAP_5(m, sep, a, b, c, d, e) CINC__MAP_4(m, sep, a, b, c, d) sep() m(e, 4)
#define CINC__MAP_6(m, sep, a, b, c, d, e, f) CINC__MAP_5(m, sep, a, b, c, d, e) sep() m(f, 5)
#define CINC__MAP_7(m, sep, a, b, c, d, e, f, g) CINC__MAP_6(m, sep, a, b, c, d, e, f) sep() m(g, 6)
#define CINC__MAP_8(m, sep, a, b, c, d, e, f, g, h) CINC__MAP_7(m, sep, a, b, c, d, e, f, g) sep() m(h, 7)

// `void` for an empty parameter list.
#define CINC__VOIDIFY(...) CINC__VOIDIFY_I(__VA_OPT__(, ) void)
#define CINC__VOIDIFY_I(a, ...) a

// typeof keeps function pointer and other declarator heavy types in one piece.
#define CINC__TYPE(t, i) typeof(t)
#define CINC__PARAM(t, i) typeof(t) CINC__CAT(a, i)
#define CINC__ZERO(t, i) (typeof(t)){}
#define CINC__MEMBER(t, i) typeof(t) CINC__CAT(a, i);
#define CINC__STORE(t, i) f->CINC__CAT(a, i) = CINC__CAT(a, i);
#define CINC__LOAD(t, i) f->CINC__CAT(a, i)
#define CINC__TYPES(...) CINC__MAP(CINC__TYPE, CINC__COMMA, __VA_ARGS__) CINC__VOIDIFY(__VA_ARGS__)
#define CINC__PARAMS(...) CINC__MAP(CINC__PARAM, CINC__COMMA, __VA_ARGS__) CINC__VOIDIFY(__VA_ARGS__)
#define CINC__ZEROS(...) CINC__MAP(CINC__ZERO, CINC__COMMA, __VA_ARGS__)

// the plumbing behind the block macros: `once` runs the block a single time and `impl` is theirs.
typedef struct cinc__scope {
  void *impl;
  int phase;
  bool once;
} cinc__scope;

/* ========================================================================================================== */
/* the runtime.                                                                                               */
/* ========================================================================================================== */

typedef struct cinc_options {
  // event loop threads, zero means one per core. a task stays on the worker it was spawned on.
  uint32_t workers;
  // threads for CINC_BLOCKING, zero means 64.
  uint32_t blocking_threads;
  // per task, zero means 64 KiB with a guard page. finished tasks hand their stack and arena chunks back to
  // their worker, so once warm a spawn maps nothing and takes no lock.
  size_t stack_size;
} cinc_options;

// runs `main` as the first task on worker 0, on the calling thread, and returns its result once every task has
// finished, detached ones included. called from a task, it runs `main` right there instead.
int cinc_run(const cinc_options *options, int (*main)(void *arg), void *arg);
int cinc_run_main(const cinc_options *options, int (*main)(int argc, char **argv), int argc, char **argv);
#define CINC_RUN(main, arg, ...) cinc_run(&(const cinc_options){__VA_ARGS__}, main, arg)

// tokio's #[tokio::main]: CINC_MAIN(.workers = 4) { ... } defines main() and runs the block as the first task,
// with argc and argv in scope.
#define CINC_MAIN(...)                                                                                           \
  static int cinc__main(int argc, char **argv);                                                                  \
  int main(int argc, char **argv) {                                                                              \
    return cinc_run_main(&(const cinc_options){__VA_ARGS__}, cinc__main, argc, argv);                            \
  }                                                                                                              \
  static int cinc__main([[maybe_unused]] int argc, [[maybe_unused]] char **argv)

// false on a plain thread, where every call below blocks the thread instead of parking a task. a CINC_BLOCKING
// block runs on such a thread too, with the task's arena, deadline and cancellation.
bool cinc_in_task(void);
uint32_t cinc_worker(void);
uint32_t cinc_workers(void);

typedef struct cinc_stats {
  // stacks mapped since the runtime started. it stops growing once the workers' pools are warm.
  uint64_t stacks_mapped;
  // stacks waiting in the workers' pools.
  uint64_t stacks_idle;
  uint64_t blocking_threads;
} cinc_stats;
// zero outside a runtime.
cinc_stats cinc_runtime_stats(void);

// the current task's arena, dropped when the task returns. on a plain thread, one per thread. every task also
// gets its own pair of cilo scratch arenas, bound on each switch, so a CILO_SCRATCH block may wait.
cilo_arena *cinc_arena(void);

uint64_t cinc_now_ms(void);
cinc_error cinc_sleep(uint64_t ms);
cinc_error cinc_yield(void);

/* ========================================================================================================== */
/* tasks: any function can be one.                                                                            */
/*                                                                                                            */
/*   double price_of(uint32_t product) { ... }        a plain function, callable as usual                     */
/*   CINC_SPAWNABLE(price_of, uint32_t);              its parameter types, checked against the function       */
/*                                                                                                            */
/*   auto price = CINC_SPAWN(price_of(4));            starts it as a task, the arguments are evaluated here   */
/*   double p = CINC_AWAIT(price);                                                                            */
/*                                                                                                            */
/* CINC_SPAWNABLE leaves the function alone, so it works on functions from other libraries too. it takes up   */
/* to 8 parameters, and a void function gives a future of cinc_unit.                                          */
/*                                                                                                            */
/* a spawn inside a CINC_CONCURRENTLY block belongs to the block. outside of one, or with CINC_SPAWN_IN, it   */
/* is detached: it runs on its own until it returns, cinc_run waits for it, and its future has no task.       */
/* ========================================================================================================== */

typedef struct cinc_task cinc_task;
typedef struct cinc_group cinc_group;
typedef struct cinc_unit {
  char unit;
} cinc_unit;

// zeroed memory from the current task's frame arena, kept apart from cinc_arena() and rewound when the
// innermost CINC_CONCURRENTLY block ends. a spawn costs a pointer bump, no malloc. a detached frame lives as long
// as its task.
void *cinc__frame(size_t size, size_t align);
cinc_task *cinc__start(void (*entry)(void *frame), void *frame);
// the next spawn of the current task lands on `worker` instead of the current one.
void cinc__target(uint32_t worker);
// the next spawn of the current task is detached into `group`, null for none.
void cinc__detach(cinc_group *group);

#define CINC_SPAWNABLE(fn, ...)                                                                                  \
  static_assert(_Generic(&fn, typeof(fn(CINC__ZEROS(__VA_ARGS__))) (*)(CINC__TYPES(__VA_ARGS__)): true,          \
                         default: false),                                                                        \
                "CINC_SPAWNABLE(" #fn ", ...) must list the parameter types of " #fn);                           \
  typedef typeof(_Generic(&fn, void (*)(CINC__TYPES(__VA_ARGS__)): (cinc_unit){},                                \
                          default: fn(CINC__ZEROS(__VA_ARGS__)))) fn##__result;                                  \
  typedef struct fn##__frame {                                                                                   \
    CINC__MAP(CINC__MEMBER, CINC__NOTHING, __VA_ARGS__)                                                          \
    fn##__result result;                                                                                         \
  } fn##__frame;                                                                                                 \
  typedef struct fn##__future {                                                                                  \
    cinc_task *task;                                                                                             \
    fn##__result *result;                                                                                        \
  } fn##__future;                                                                                                \
  [[maybe_unused]] static void cinc__entry_##fn(void *frame) {                                                   \
    fn##__frame *f = frame;                                                                                      \
    f->result = _Generic(&fn,                                                                                    \
        void (*)(CINC__TYPES(__VA_ARGS__)): (fn(CINC__MAP(CINC__LOAD, CINC__COMMA, __VA_ARGS__)), (cinc_unit){}), \
        default: fn(CINC__MAP(CINC__LOAD, CINC__COMMA, __VA_ARGS__)));                                           \
  }                                                                                                              \
  [[maybe_unused]] static inline fn##__future cinc__go_##fn(CINC__PARAMS(__VA_ARGS__)) {                         \
    fn##__frame *f = cinc__frame(sizeof *f, alignof(fn##__frame));                                               \
    CINC__MAP(CINC__STORE, CINC__NOTHING, __VA_ARGS__)                                                           \
    return (fn##__future){.task = cinc__start(cinc__entry_##fn, f), .result = &f->result};                       \
  }                                                                                                              \
  typedef fn##__future fn##__future

// `call` is written as a call, `price_of(4)`: pasting onto its first token turns it into the spawner. outside
// a runtime the task runs inline, so code that spawns still works, one task after the other.
#define CINC_SPAWN(call) cinc__go_##call
#define CINC_SPAWN_ON(worker, call) (cinc__target(worker), cinc__go_##call)
// detached into a group: the task outlives the block, and the group waits for it or cancels it. the accept loop
// of a server spawns each connection this way, so nothing piles up in its frame arena.
#define CINC_SPAWN_IN(group, call) (cinc__detach(group), cinc__go_##call)
#define CINC_SPAWN_IN_ON(group, worker, call) (cinc__detach(group), cinc__target(worker), cinc__go_##call)

// for declaring futures, `CINC_FUTURE(price_of) prices[3];`. a single one is simpler with `auto`.
#define CINC_FUTURE(fn) fn##__future

// waits for the task and gives its result, zeroed when it was cancelled or when the wait itself timed out or
// was cancelled. cinc_join(future.task) tells which. awaiting a void function is a void expression, and gcc wants
// a (void) in front of an await whose result is dropped.
cinc_error cinc_join(cinc_task *task);

// every future has this layout. `future` is copied into a one element array so it is evaluated once, which makes
// await(spawn(f(x))) work. a detached future has no task: awaiting one is a bug and aborts, cinc_join gives
// CINC_ERROR_CLOSED.
typedef struct cinc__future {
  cinc_task *task;
  void *result;
} cinc__future;
[[noreturn]] void cinc__await_detached(void);
static inline cinc_error cinc__await_task(cinc_task *task) {
  if (!task) cinc__await_detached();
  return cinc_join(task);
}
static inline void *cinc__await(const void *future, void *zero) {
  cinc__future f;
  memcpy(&f, future, sizeof f);
  return cinc__await_task(f.task) == CINC_OK ? f.result : zero;
}
#define CINC_AWAIT(future)                                                                                       \
  _Generic((future).result,                                                                                      \
      cinc_unit *: (void)cinc__await_task((future).task),                                                        \
      default: *(typeof((future).result))cinc__await((typeof(future)[1]){future}, &(typeof(*(future).result)){}))

// cancels the task and, through it, every task it spawned in its blocks. it is seen at the task's next wait or
// cinc_check.
void cinc_cancel(cinc_task *task);
#define CINC_CANCEL(future) cinc_cancel((future).task)
// for loops that never wait: CINC_ERROR_CANCELLED or CINC_ERROR_TIMEOUT once the task should stop.
cinc_error cinc_check(void);

cinc__scope cinc__block_enter(void);
void cinc__block_end(cinc__scope *s);
void cinc__block_leave(cinc__scope *s);

// CINC_CONCURRENTLY { ... } waits for every task spawned inside before going on, so no task outlives the
// block that started it. leaving early with break, return or goto cancels the ones still running.
#define CINC_CONCURRENTLY                                                                                        \
  for (cinc__scope CINC__UNIQUE(cinc__c) [[gnu::cleanup(cinc__block_leave)]] = cinc__block_enter();              \
       CINC__UNIQUE(cinc__c).once; cinc__block_end(&CINC__UNIQUE(cinc__c)))

/* groups: Go's WaitGroup for detached tasks, zero initialized is empty. a group outlives its tasks.          */

struct cinc_group {
  // live tasks in the low 48 bits, queued waiters above.
  _Atomic(uint64_t) state;
  _Atomic(uint32_t) lock;
  _Atomic(bool) cancelled;
  void *waiters;
  _Atomic(void *) runtime;
};

// waits until the group is empty. honors the current deadline and cancellation.
cinc_error cinc_group_wait(cinc_group *group);
// cancels the group's tasks, and the ones spawned into it later.
void cinc_group_cancel(cinc_group *group);
uint64_t cinc_group_live(const cinc_group *group);

/* ========================================================================================================== */
/* deadlines and the blocking pool.                                                                           */
/* ========================================================================================================== */

cinc__scope cinc__deadline_push(uint64_t ms);
void cinc__deadline_pop(cinc__scope *s);
// moves to the next phase: 1 runs the block, 2 runs the else branch when the deadline expired.
bool cinc__deadline_step(cinc__scope *s);
// what is left of the innermost deadline, UINT64_MAX without one. for handing a timeout to blocking code.
uint64_t cinc_remaining_ms(void);

// CINC_WITHIN(ms) { ... } else { ... }, the else branch running when the deadline expired. there is no
// unwinding: once expired, every waiting call in the block returns CINC_ERROR_TIMEOUT and the code decides
// how to leave. nested deadlines never extend an outer one.
#define CINC_WITHIN(ms)                                                                                          \
  for (cinc__scope CINC__UNIQUE(cinc__w) [[gnu::cleanup(cinc__deadline_pop)]] = cinc__deadline_push(ms);         \
       cinc__deadline_step(&CINC__UNIQUE(cinc__w));)                                                             \
    if (CINC__UNIQUE(cinc__w).phase == 1)

cinc__scope cinc__blocking_enter(void);
void cinc__blocking_leave(cinc__scope *s);

// CINC_BLOCKING { ... } runs the block on the blocking pool and parks the task meanwhile, for cpu work and
// for libraries that block. the cleanup attribute brings the task back on break and return too.
#define CINC_BLOCKING                                                                                            \
  for (cinc__scope CINC__UNIQUE(cinc__b) [[gnu::cleanup(cinc__blocking_leave)]] = cinc__blocking_enter();        \
       CINC__UNIQUE(cinc__b).once; CINC__UNIQUE(cinc__b).once = false)

// the same as a function, for callbacks and for libraries with deep stacks: `fn` runs on the pool thread's own
// stack. it is skipped when the task is already cancelled or past its deadline, which is then returned.
//
// a CINC_BLOCKING block runs on the task's stack, moved to a pool thread and back. thread locals differ on both
// sides of its edges, and compilers merge calls to functions declared const, like pthread_self and glibc's
// errno, across them. code that reads one inside the block does it in a function the block calls, or uses
// cinc_blocking.
cinc_error cinc_blocking(void (*fn)(void *arg), void *arg);

/* ========================================================================================================== */
/* io, on posix file descriptors. every call honors the current deadline and cancellation.                    */
/* ========================================================================================================== */

typedef enum cinc_io : uint8_t {
  CINC_READABLE = 1,
  CINC_WRITABLE = 2,
} cinc_io;

// the building block for libraries with their own protocol code: wait, then call read or write as usual.
cinc_error cinc_wait_fd(int fd, cinc_io events);
// reads what is available, at most `len`. zero bytes and CINC_ERROR_CLOSED mean the peer closed.
cinc_error cinc_read(int fd, void *buf, size_t len, size_t *got);
// reads exactly `len` bytes, CINC_ERROR_CLOSED when the peer closes first. `got` may be null.
cinc_error cinc_read_exact(int fd, void *buf, size_t len, size_t *got);
cinc_error cinc_write_all(int fd, const void *buf, size_t len);
// writes every buffer in order with as few system calls as possible, for headers and a body.
struct iovec;
cinc_error cinc_writev_all(int fd, const struct iovec *iov, int count);
// resolves `host` on the blocking pool, then connects without blocking the worker. the socket is non blocking,
// with TCP_NODELAY and without SIGPIPE.
cinc_error cinc_connect(const char *host, uint16_t port, int *fd);
// `bind` is "host:port", "[::]:port" or ":port". an empty host or [::] listens on IPv6 and IPv4 at once, a
// name tries its IPv4 addresses first. with `reuse_port` several sockets can listen on the same address.
cinc_error cinc_listen(const char *bind, bool reuse_port, int *fd);
// the port a socket is bound to, for listening on port 0.
uint16_t cinc_local_port(int fd);
// the accepted socket is non blocking, with TCP_NODELAY and without SIGPIPE.
cinc_error cinc_accept(int listener, int *fd);
// wakes the tasks of this worker waiting on `fd`, then closes it.
void cinc_close(int fd);

// whether the kernel spreads connections over sockets listening with `reuse_port`. Linux does: a server listens
// once per worker and accepts on each. macOS hands every connection to one socket: a server listens once,
// accepts on one task and spawns each connection with CINC_SPAWN_IN_ON, round robin over the workers.
#if defined(__linux__)
#define CINC_REUSEPORT_BALANCES 1
#else
#define CINC_REUSEPORT_BALANCES 0
#endif

/* ========================================================================================================== */
/* shutdown.                                                                                                  */
/* ========================================================================================================== */

// waits for SIGINT or SIGTERM and gives which one in `signo`, which may be null. the first call installs the
// handlers, and once a signal came every call returns right away, in every task. a second signal ends the
// process the default way, so a stuck shutdown can still be interrupted.
cinc_error cinc_wait_shutdown(int *signo);

/* ========================================================================================================== */
/* channels.                                                                                                  */
/*                                                                                                            */
/*   typedef CINC_CHAN(Job) Jobs;                 Jobs jobs = CINC_MAKE_CHAN(Jobs, 64);                       */
/*   CINC_CHAN_SEND(jobs, job);                   CINC_RANGE(job, jobs) { ... }                               */
/* ========================================================================================================== */

typedef struct cinc_chan cinc_chan;

// zero capacity is a rendezvous, the sender waits for a receiver. a channel works across workers. a send or a
// receive that can go on without waiting does, even in a task that is cancelled or past its deadline, so a
// permit or a bell is still handed back from one. only a wait gives CINC_ERROR_CANCELLED or CINC_ERROR_TIMEOUT.
cinc_chan *cinc_chan_new_(size_t elem_size, size_t elem_align, size_t capacity);
cinc_error cinc_chan_send_(cinc_chan *ch, const void *value);
cinc_error cinc_chan_recv_(cinc_chan *ch, void *out);
// senders get CINC_ERROR_CLOSED right away, receivers once the buffer is drained.
void cinc_chan_close_(cinc_chan *ch);
void cinc_chan_free_(cinc_chan *ch);

// `of` is never set, it only carries T for the macros.
#define CINC_CHAN(T)                                                                                             \
  struct {                                                                                                       \
    cinc_chan *raw;                                                                                              \
    T *of;                                                                                                       \
  }
#define CINC_MAKE_CHAN(type, capacity)                                                                           \
  ((type){.raw = cinc_chan_new_(sizeof(*(type){}.of), alignof(typeof(*(type){}.of)), capacity)})
// the one member struct copies scalars and structs alike, and type checks the value. variadic so a compound
// literal's commas pass through.
#define CINC_CHAN_SEND(ch, ...) cinc_chan_send_((ch).raw, &(struct { typeof(*(ch).of) v; }){__VA_ARGS__}.v)
#define CINC_CHAN_RECV(ch, out) cinc_chan_recv_((ch).raw, _Generic((out), typeof((ch).of): (out)))
#define CINC_CHAN_CLOSE(ch) cinc_chan_close_((ch).raw)
#define CINC_CHAN_FREE(ch) cinc_chan_free_((ch).raw)
// go's `for v := range ch`: receives until the channel is closed and drained, or the task is cancelled while
// it waits for the next value.
#define CINC_RANGE(var, ch) for (typeof(*(ch).of) var; cinc_chan_recv_((ch).raw, &var) == CINC_OK;)

/* go's select, over up to 16 sends and receives. it gives the index of the case that ran:                    */
/*                                                                                                            */
/*   cinc_error e;                                                                                            */
/*   switch (CINC_SELECT(&e, CINC_ON_RECV(jobs, &job), CINC_ON_SEND(results, r))) {                          */
/*   case 0: ... e is CINC_ERROR_CLOSED once jobs is closed and drained                                       */
/*   case 1: ...                                                                                              */
/*   default: return e;                           cancelled, or past the deadline                             */
/*   }                                                                                                        */

typedef struct cinc_case {
  // a case on a null channel never runs, which turns a case off like a nil channel in go.
  cinc_chan *ch;
  // the value to send, or where the received one goes.
  void *value;
  bool send;
} cinc_case;

// runs one case that can go on, picked at random among the ready ones, and gives its index with CINC_OK or, on
// a closed channel, CINC_ERROR_CLOSED in `error`. otherwise it waits for one, unless `wait` is false. -1 when
// it gave up: the interruption in `error`, or CINC_ERROR_TIMEOUT for a select that does not wait. `error` may be
// null.
int cinc_select_(const cinc_case *cases, size_t count, bool wait, cinc_error *error);

// the channel parameter is not named `ch`, which would rename the member.
#define CINC_ON_SEND(channel, ...)                                                                               \
  ((cinc_case){.ch = (channel).raw, .value = &(struct { typeof(*(channel).of) v; }){__VA_ARGS__}.v, .send = true})
#define CINC_ON_RECV(channel, out)                                                                               \
  ((cinc_case){.ch = (channel).raw, .value = _Generic((out), typeof((channel).of): (out))})
#define CINC__SELECT(wait, error, ...)                                                                           \
  cinc_select_((const cinc_case[]){__VA_ARGS__}, sizeof((const cinc_case[]){__VA_ARGS__}) / sizeof(cinc_case),   \
               wait, error)
#define CINC_SELECT(error, ...) CINC__SELECT(true, error, __VA_ARGS__)
// go's select with a default case: -1 and CINC_ERROR_TIMEOUT when no case is ready.
#define CINC_TRY_SELECT(error, ...) CINC__SELECT(false, error, __VA_ARGS__)

/* ========================================================================================================== */
/* mutexes, parking the task rather than the thread. zero initialized is unlocked.                            */
/* ========================================================================================================== */

typedef struct cinc_mutex {
  _Atomic(uint32_t) state;
  void *waiters;
} cinc_mutex;

void cinc_lock(cinc_mutex *m);
void cinc_unlock(cinc_mutex *m);

cinc__scope cinc__lock_enter(cinc_mutex *m);
void cinc__lock_leave(cinc__scope *s);

#define CINC_LOCKED(m)                                                                                           \
  for (cinc__scope CINC__UNIQUE(cinc__l) [[gnu::cleanup(cinc__lock_leave)]] = cinc__lock_enter(m);               \
       CINC__UNIQUE(cinc__l).once; CINC__UNIQUE(cinc__l).once = false)

#ifdef __cplusplus
}
#endif

#endif
