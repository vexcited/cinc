#ifndef CINC_INTERNAL_H
#define CINC_INTERNAL_H

#include <cinc/cinc.h>
#include <pthread.h>
#include <sched.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#define CINC__KQUEUE 1
#include <sys/event.h>
#else
#define CINC__KQUEUE 0
#endif

#if CILO_ASAN
#include <sanitizer/common_interface_defs.h>
#endif

static_assert(sizeof(void *) == 8, "cinc needs a 64 bit target");

#define CINC__NEVER UINT64_MAX
// idle stacks a worker keeps, past that a finished task unmaps its own.
#define CINC__POOL_MAX 1024u

// a task's park state. a waker moves it to NOTIFIED and, when it was parked, makes it runnable.
enum : uint32_t { CINC__IDLE, CINC__NOTIFIED, CINC__PARKED_WORKER, CINC__PARKED_THREAD, CINC__DEAD };
enum : uint8_t { CINC__PLAIN, CINC__WORKER, CINC__BLOCKING };
enum : uint8_t { CINC__MSG_SPAWN = 1, CINC__MSG_WAKE, CINC__MSG_DONE };
enum : uint32_t { CINC__ACT_NONE, CINC__ACT_EXIT, CINC__ACT_BLOCKING, CINC__ACT_UNBLOCK };
// a waiter or a job: queued, taken by a waker that still touches it, then let go.
enum : uint32_t { CINC__QUEUED, CINC__CLAIMED, CINC__RELEASED, CINC__ABANDONED };
enum : uint8_t { CINC__R = CINC_READABLE, CINC__W = CINC_WRITABLE };

typedef struct cinc__worker cinc__worker;
typedef struct cinc__runtime cinc__runtime;

/* ========================================================================================================== */
/* lock free inbox, Vyukov's intrusive multi producer single consumer queue.                                  */
/* ========================================================================================================== */

typedef struct cinc__node {
  _Atomic(struct cinc__node *) next;
} cinc__node;

typedef struct cinc__mpsc {
  _Atomic(cinc__node *) head;
  cinc__node *tail;
  cinc__node stub;
} cinc__mpsc;

static inline void cinc__mpsc_init(cinc__mpsc *q) {
  atomic_init(&q->stub.next, nullptr);
  atomic_init(&q->head, &q->stub);
  q->tail = &q->stub;
}

static inline void cinc__mpsc_push(cinc__mpsc *q, cinc__node *n) {
  atomic_store_explicit(&n->next, nullptr, memory_order_relaxed);
  cinc__node *prev = atomic_exchange_explicit(&q->head, n, memory_order_seq_cst);
  atomic_store_explicit(&prev->next, n, memory_order_release);
}

// null when empty, or while a producer is halfway through a push.
static inline cinc__node *cinc__mpsc_pop(cinc__mpsc *q) {
  cinc__node *tail = q->tail, *next = atomic_load_explicit(&tail->next, memory_order_acquire);
  if (tail == &q->stub) {
    if (!next) return nullptr;
    q->tail = tail = next;
    next = atomic_load_explicit(&next->next, memory_order_acquire);
  }
  if (next) {
    q->tail = next;
    return tail;
  }
  if (tail != atomic_load_explicit(&q->head, memory_order_acquire)) return nullptr;
  cinc__mpsc_push(q, &q->stub);
  next = atomic_load_explicit(&tail->next, memory_order_acquire);
  if (!next) return nullptr;
  q->tail = next;
  return tail;
}

static inline bool cinc__mpsc_pending(cinc__mpsc *q) {
  return q->tail != &q->stub || atomic_load_explicit(&q->head, memory_order_seq_cst) != &q->stub;
}

/* ========================================================================================================== */
/* spinning.                                                                                                  */
/* ========================================================================================================== */

static inline void cinc__pause(void) {
#if defined(__x86_64__)
  __asm__ volatile("pause");
#elif defined(__aarch64__)
  __asm__ volatile("isb");
#endif
}

static inline void cinc__backoff(uint32_t *spins) {
  if (++*spins < 64) cinc__pause();
  else sched_yield();
}

// a lock on one bit of a word, held for a few instructions only.
static inline void cinc__lock_bit(_Atomic(uint32_t) *word, uint32_t bit) {
  uint32_t spins = 0;
  while (atomic_fetch_or_explicit(word, bit, memory_order_acquire) & bit)
    while (atomic_load_explicit(word, memory_order_relaxed) & bit) cinc__backoff(&spins);
}

static inline void cinc__unlock_bit(_Atomic(uint32_t) *word, uint32_t bit) {
  atomic_fetch_and_explicit(word, ~bit, memory_order_release);
}

/* ========================================================================================================== */
/* tasks.                                                                                                     */
/* ========================================================================================================== */

typedef struct cinc__parker {
  pthread_mutex_t lock;
  pthread_cond_t cond;
} cinc__parker;

// a stack mapping: the guard page, the stack, then this header with the arenas lent the rest of its page.
typedef struct cinc__fiber {
  void *sp;
  cinc_task *task;
  struct cinc__fiber *next;
  char *map;
  size_t map_size;
  char *stack;
  size_t stack_size;
  cilo_arena arena;
  cilo_arena frames;
  cilo_arena scratch[CILO_SCRATCH_COUNT];
} cinc__fiber;

// lives on the waiting side's stack, in a circular list behind a lock.
typedef struct cinc__waiter {
  struct cinc__waiter *next;
  struct cinc__waiter *prev;
  cinc_task *task;
  void *slot;
  _Atomic(uint32_t) state;
  cinc_error result;
} cinc__waiter;

typedef struct cinc__block {
  struct cinc__block *outer;
  cinc_task *owner;
  cinc_task *children;
  cilo_mark mark;
  // children still running, only touched on the owner's worker.
  uint32_t live;
  bool waiting;
  bool cancelled;
} cinc__block;

typedef struct cinc__job {
  struct cinc__job *next;
  void (*fn)(void *arg);
  void *arg;
  // the task to move to the pool and back, or the one waiting for `fn`.
  cinc_task *task;
  // frees a job its waiter gave up on.
  void (*abandon)(struct cinc__job *job);
  _Atomic(uint32_t) state;
  bool resume;
} cinc__job;

struct cinc_task {
  // first, so an inbox node is its task.
  cinc__node node;
  cinc_task *next;
  cinc__fiber *fiber;
  cinc__worker *home;
  // the worker of the block owner, where the end of the task is accounted.
  cinc__worker *owner_home;
  void (*entry)(void *);
  void *frame;
  cinc__parker *parker;
  _Atomic(uint32_t) wstate;
  _Atomic(uint32_t) done;
  _Atomic(bool) cancelled;
  uint8_t msg;
  bool detached;
  bool propagated;
  // a wait returned CINC_ERROR_TIMEOUT for the innermost deadline.
  bool fired;
  uint32_t timer_slot;
  uint64_t wake_at;
  uint64_t deadline;
  cinc__block *blocks;
  cinc__block *parent;
  cinc_task *sibling;
  cinc_group *group;
  cinc_task *dprev;
  cinc_task *dnext;
  _Atomic(uint32_t) jlock;
  void *joiners;
  cinc__job job;
  cilo_arena *arena;
  cilo_arena *frames;
  cilo_arena *scratch;
};

/* ========================================================================================================== */
/* workers.                                                                                                   */
/* ========================================================================================================== */

typedef struct cinc__fdwait {
  struct cinc__fdwait *next;
  struct cinc__fdwait *prev;
  cinc_task *task;
  uint8_t want;
  uint8_t got;
} cinc__fdwait;

typedef struct cinc__fdent {
  cinc__fdwait *waiters;
  bool added;
} cinc__fdent;

typedef struct cinc__thread cinc__thread;

struct cinc__worker {
  cinc__runtime *rt;
  uint32_t index;
  pthread_t thread;
  cinc__thread *th;
  cinc__mpsc inbox;
  // set while blocked in the poller, a producer that clears it wakes the worker.
  alignas(64) _Atomic(uint32_t) sleeping;
  alignas(64) cinc_task *runq_head;
  cinc_task *runq_tail;
  size_t runq_len;
  cinc_task **timers;
  uint32_t ntimers;
  uint32_t timers_cap;
  int pfd;
  int efd;
#if CINC__KQUEUE
  struct kevent changes[128];
  int nchanges;
#endif
  cinc__fdent *fds;
  uint32_t nfds;
  uint32_t io_waiters;
  uint32_t ticks;
  cinc__fiber *fibers;
  _Atomic(uint32_t) nfibers;
  cinc_task *detached;
  uint64_t epoch;
};

typedef struct cinc__pool {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  cinc__job *head;
  cinc__job *tail;
  uint32_t threads;
  uint32_t idle;
  uint32_t max;
  bool stop;
  pthread_t *tids;
  uint32_t ntids;
} cinc__pool;

struct cinc__runtime {
  cinc__worker *workers;
  uint32_t nworkers;
  size_t stack_size;
  size_t page;
  // the main task and every detached one still running. the runtime stops when it reaches zero.
  alignas(64) _Atomic(uint64_t) roots;
  _Atomic(bool) stop;
  // bumped by a group cancel so workers look at their detached tasks.
  _Atomic(uint64_t) epoch;
  _Atomic(uint64_t) stacks_mapped;
  cinc__pool pool;
};

struct cinc__thread {
  cinc__worker *worker;
  cinc_task *current;
  uint8_t mode;
  uint32_t action;
  void *sched_sp;
  const void *sched_bottom;
  size_t sched_size;
  // what the next spawn was told, and the handle between cinc__frame and cinc__start.
  cinc_task *pending;
  cilo_mark pending_mark;
  cinc_group *next_group;
  bool next_detach;
  bool next_targeted;
  uint32_t next_target;
  bool parker_ready;
  bool plain_ready;
  cinc__parker parker;
  cinc_task plain;
  cilo_arena plain_arena;
  cilo_arena plain_frames;
};

/* ========================================================================================================== */
/* shared between the sources.                                                                                */
/* ========================================================================================================== */

cinc__thread *cinc__self(void);
cinc_task *cinc__plain(cinc__thread *th);
static inline cinc_task *cinc__cur(cinc__thread *th) { return th->current ? th->current : cinc__plain(th); }
static inline bool cinc__in_worker(const cinc__thread *th) { return th->mode == CINC__WORKER && th->current; }

uint64_t cinc__now_ns(void);
uint64_t cinc__after_ms(uint64_t now, uint64_t ms);
cinc_error cinc__interrupted(cinc_task *t);
void cinc__park(cinc__thread *th, cinc_task *t, uint64_t until);
void cinc__unpark(cinc_task *t);
void cinc__notify(cinc__worker *w);
void cinc__runq_push(cinc__worker *w, cinc_task *t);

// heads are void pointers, which is what the public structs can hold.
void cinc__wq_push(void **head, cinc__waiter *w);
void cinc__wq_push_front(void **head, cinc__waiter *w);
void cinc__wq_remove(void **head, cinc__waiter *w);
cinc__waiter *cinc__wq_pop(void **head);
void cinc__wq_release(cinc__waiter *w);
cinc_error cinc__wq_wait(cinc__thread *th, cinc_task *t, cinc__waiter *w, _Atomic(uint32_t) *word, uint32_t bit,
                         void **head, bool interruptible);

void cinc__pool_push(cinc__runtime *rt, cinc__job *job);
void cinc__pool_stop(cinc__runtime *rt);
// runs the job on the pool and waits for it. an abandonable job may be left behind on interruption.
cinc_error cinc__offload(cinc__thread *th, cinc_task *t, cinc__job *job, bool interruptible);

bool cinc__poller_init(cinc__worker *w);
void cinc__poller_close(cinc__worker *w);
void cinc__poller_wait(cinc__worker *w, uint64_t timeout_ns);

#endif
