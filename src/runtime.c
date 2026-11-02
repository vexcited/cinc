#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

void *cinc__switch(void **from, void *to, void *arg);
void cinc__trampoline(void);

static thread_local cinc__thread tls;

// a fresh call every time: a CINC_BLOCKING block moves the task to another thread, and a cached thread local
// address would still point at the first one. the empty asm keeps the optimizer from treating it as pure.
[[gnu::noinline]] cinc__thread *cinc__self(void) {
  cinc__thread *th = &tls;
  __asm__ volatile("" : "+r"(th) : : "memory");
  return th;
}

/* ========================================================================================================== */
/* time.                                                                                                      */
/* ========================================================================================================== */

uint64_t cinc__now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1'000'000'000u + (uint64_t)ts.tv_nsec;
}

uint64_t cinc__after_ms(uint64_t now, uint64_t ms) {
  uint64_t ns, at;
  if (ckd_mul(&ns, ms, 1'000'000u) || ckd_add(&at, now, ns)) return CINC__NEVER;
  return at;
}

uint64_t cinc_now_ms(void) { return cinc__now_ns() / 1'000'000u; }

/* ========================================================================================================== */
/* plain threads: a pseudo task carries their deadlines, arenas and parker.                                   */
/* ========================================================================================================== */

static pthread_key_t plain_key;
static pthread_once_t plain_once = PTHREAD_ONCE_INIT;

static void plain_exit(void *p) {
  cinc__local *l = p;
  cilo_arena_drop(&l->arena);
  cilo_arena_drop(&l->frames);
  if (l->parker_ready) {
    pthread_mutex_destroy(&l->parker.lock);
    pthread_cond_destroy(&l->parker.cond);
  }
  free(l);
}

static void plain_make_key(void) { pthread_key_create(&plain_key, plain_exit); }

static cinc__local *local_of(cinc__thread *th) {
  if (CILO_LIKELY(th->local)) return th->local;
  cinc__local *l = calloc(1, sizeof *l);
  if (!l) cilo_oom(sizeof *l);
  l->plain = (cinc_task){.deadline = CINC__NEVER, .arena = &l->arena, .frames = &l->frames};
  pthread_once(&plain_once, plain_make_key);
  pthread_setspecific(plain_key, l);
  return th->local = l;
}

cinc_task *cinc__plain(cinc__thread *th) { return &local_of(th)->plain; }

static cinc__parker *parker_of(cinc__thread *th) {
  cinc__local *l = local_of(th);
  if (CILO_UNLIKELY(!l->parker_ready)) {
    pthread_mutex_init(&l->parker.lock, nullptr);
#if defined(__APPLE__)
    pthread_cond_init(&l->parker.cond, nullptr);
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&l->parker.cond, &attr);
    pthread_condattr_destroy(&attr);
#endif
    l->parker_ready = true;
  }
  return &l->parker;
}

/* ========================================================================================================== */
/* switching.                                                                                                 */
/* ========================================================================================================== */

// from the scheduler of `th` into a fiber, until it switches back.
static void resume(cinc__thread *th, cinc__fiber *f) {
#if CILO_ASAN
  void *fake = nullptr;
  __sanitizer_start_switch_fiber(&fake, f->stack, f->stack_size);
#endif
  cinc__switch(&th->sched_sp, f->sp, th);
#if CILO_ASAN
  __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#endif
}

// from a fiber back to the scheduler of `th`. returns the thread that switched back in, another one when the
// task moved to or from the blocking pool.
static cinc__thread *suspend(cinc__thread *th, cinc__fiber *f, uint32_t action) {
  th->action = action;
#if CILO_ASAN
  void *fake = nullptr;
  __sanitizer_start_switch_fiber(&fake, th->sched_bottom, th->sched_size);
#endif
  cinc__thread *now = cinc__switch(&f->sp, th->sched_sp, nullptr);
#if CILO_ASAN
  __sanitizer_finish_switch_fiber(fake, &now->sched_bottom, &now->sched_size);
#endif
  return now;
}

// every fiber runs this loop: one task after the other, parked in the scheduler in between.
[[noreturn]] static void fiber_main(cinc__fiber *f, [[maybe_unused]] cinc__thread *th) {
#if CILO_ASAN
  __sanitizer_finish_switch_fiber(nullptr, &th->sched_bottom, &th->sched_size);
#endif
  for (;;) {
    cinc_task *t = f->task;
    t->entry(t->frame);
    suspend(cinc__self(), f, CINC__ACT_EXIT);
  }
}

/* ========================================================================================================== */
/* stacks.                                                                                                    */
/* ========================================================================================================== */

static size_t round_up(size_t n, size_t to) { return (n + to - 1) & ~(to - 1); }

static void fiber_frame(cinc__fiber *f) {
  // the stack ends where the header starts, page aligned. function pointers go in through memcpy, iso c has no
  // cast between them and data pointers.
  void **top = (void **)f;
  void (*entry)(cinc__fiber *, cinc__thread *) = fiber_main;
  void (*start)(void) = cinc__trampoline;
#if defined(__aarch64__)
  void **sp = top - 20;
  memset(sp, 0, 20 * sizeof(void *));
  memcpy(&sp[0], &entry, sizeof entry);
  sp[1] = f;
  memcpy(&sp[11], &start, sizeof start);
#elif defined(__x86_64__)
  void **sp = top - 10;
  memset(sp, 0, 10 * sizeof(void *));
  uint32_t csr[2] = {0x1f80, 0x037f};
  memcpy(sp, csr, sizeof csr);
  sp[4] = f;
  memcpy(&sp[5], &entry, sizeof entry);
  memcpy(&sp[7], &start, sizeof start);
#else
#error "cinc switches stacks on aarch64 and x86_64 only"
#endif
  f->sp = sp;
}

static cinc__fiber *fiber_new(cinc__runtime *rt) {
  size_t page = rt->page, header = round_up(sizeof(cinc__fiber) + (8 << 10), page);
  size_t size = page + rt->stack_size + header;
  int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_STACK)
  flags |= MAP_STACK;
#endif
  char *m = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (m == MAP_FAILED) cilo_oom(size);
  mprotect(m, page, PROT_NONE);
  cinc__fiber *f = (cinc__fiber *)(m + page + rt->stack_size);
  *f = (cinc__fiber){.map = m, .map_size = size, .stack = m + page, .stack_size = rt->stack_size};
  // the rest of the header page is lent to the arenas, so small tasks map no chunk at all.
  char *lend = (char *)round_up((uintptr_t)(f + 1), 64), *end = m + size;
  size_t half = (size_t)(end - lend) / 2 & ~(size_t)63;
  f->frames = cilo_arena_on(lend, half, false);
  f->arena = cilo_arena_on(lend + half, (size_t)(end - lend) - half, false);
  fiber_frame(f);
  atomic_fetch_add_explicit(&rt->stacks_mapped, 1, memory_order_relaxed);
  return f;
}

static void fiber_free(cinc__fiber *f) {
  cilo_arena_drop(&f->arena);
  cilo_arena_drop(&f->frames);
  for (int i = 0; i < CILO_SCRATCH_COUNT; i++) cilo_arena_drop(&f->scratch[i]);
  munmap(f->map, f->map_size);
}

static cinc__fiber *fiber_get(cinc__worker *w) {
  cinc__fiber *f = w->fibers;
  if (CILO_LIKELY(f)) {
    w->fibers = f->next;
    atomic_store_explicit(&w->nfibers, atomic_load_explicit(&w->nfibers, memory_order_relaxed) - 1,
                          memory_order_relaxed);
    return f;
  }
  return fiber_new(w->rt);
}

// an arena that grew past one spare chunk gives them all back, the next task starts small again. most tasks
// never leave the borrowed chunk, or never touch their scratch arenas, and cost a compare here.
static void arena_recycle(cilo_arena *a) {
  if (!a->chunk && !a->spare) return;
  if (a->chunk && a->chunk->kind == CILO__BORROWED && !a->chunk->prev && !a->spare &&
      a->cur == (char *)(a->chunk + 1)) return;
  cilo_arena_reset(a);
  if (a->spare && a->spare->prev) cilo_arena_trim(a);
}

static void fiber_put(cinc__worker *w, cinc__fiber *f) {
  arena_recycle(&f->arena);
  arena_recycle(&f->frames);
  for (int i = 0; i < CILO_SCRATCH_COUNT; i++) arena_recycle(&f->scratch[i]);
  f->task = nullptr;
  uint32_t n = atomic_load_explicit(&w->nfibers, memory_order_relaxed);
  if (n >= CINC__POOL_MAX) {
    fiber_free(f);
    return;
  }
  f->next = w->fibers;
  w->fibers = f;
  atomic_store_explicit(&w->nfibers, n + 1, memory_order_relaxed);
}

/* ========================================================================================================== */
/* run queue and timers, owned by their worker.                                                               */
/* ========================================================================================================== */

void cinc__runq_push(cinc__worker *w, cinc_task *t) {
  t->next = nullptr;
  if (w->runq_tail) w->runq_tail->next = t;
  else w->runq_head = t;
  w->runq_tail = t;
  w->runq_len++;
}

static cinc_task *runq_pop(cinc__worker *w) {
  cinc_task *t = w->runq_head;
  if (!t) return nullptr;
  w->runq_head = t->next;
  if (!w->runq_head) w->runq_tail = nullptr;
  w->runq_len--;
  return t;
}

// a binary min heap on wake_at, each task knowing its slot so a wake removes it in log n.
static void timer_place(cinc__worker *w, uint32_t i, cinc_task *t) {
  w->timers[i] = t;
  t->timer_slot = i + 1;
}

static void timer_up(cinc__worker *w, uint32_t i) {
  cinc_task *t = w->timers[i];
  while (i) {
    uint32_t parent = (i - 1) / 2;
    if (w->timers[parent]->wake_at <= t->wake_at) break;
    timer_place(w, i, w->timers[parent]);
    i = parent;
  }
  timer_place(w, i, t);
}

static void timer_down(cinc__worker *w, uint32_t i) {
  cinc_task *t = w->timers[i];
  for (;;) {
    uint32_t c = 2 * i + 1;
    if (c >= w->ntimers) break;
    if (c + 1 < w->ntimers && w->timers[c + 1]->wake_at < w->timers[c]->wake_at) c++;
    if (w->timers[c]->wake_at >= t->wake_at) break;
    timer_place(w, i, w->timers[c]);
    i = c;
  }
  timer_place(w, i, t);
}

static void timer_add(cinc__worker *w, cinc_task *t, uint64_t at) {
  if (w->ntimers == w->timers_cap) {
    w->timers_cap = w->timers_cap ? w->timers_cap * 2 : 64;
    w->timers = cilo_mrealloc(w->timers, w->timers_cap * sizeof *w->timers);
  }
  t->wake_at = at;
  w->timers[w->ntimers] = t;
  timer_up(w, w->ntimers++);
}

static void timer_remove(cinc__worker *w, cinc_task *t) {
  uint32_t i = t->timer_slot - 1;
  t->timer_slot = 0;
  cinc_task *last = w->timers[--w->ntimers];
  if (i == w->ntimers) return;
  w->timers[i] = last;
  timer_down(w, i);
  timer_up(w, last->timer_slot - 1);
}

// wakes the tasks whose time came, and gives the next wake up time.
static uint64_t timers_fire(cinc__worker *w) {
  if (!w->ntimers) return CINC__NEVER;
  uint64_t now = cinc__now_ns();
  while (w->ntimers && w->timers[0]->wake_at <= now) {
    cinc_task *t = w->timers[0];
    timer_remove(w, t);
    cinc__unpark(t);
  }
  return w->ntimers ? w->timers[0]->wake_at : CINC__NEVER;
}

/* ========================================================================================================== */
/* parking.                                                                                                   */
/* ========================================================================================================== */

void cinc__notify(cinc__worker *w) {
  // a load first, so a busy worker's line is not pulled over for nothing.
  if (!atomic_load_explicit(&w->sleeping, memory_order_seq_cst)) return;
  if (!atomic_exchange_explicit(&w->sleeping, 0, memory_order_seq_cst)) return;
#if CINC__KQUEUE
  struct kevent ev;
  EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
  kevent(w->pfd, &ev, 1, nullptr, 0, nullptr);
#else
  uint64_t one = 1;
  [[maybe_unused]] ssize_t n = write(w->efd, &one, sizeof one);
#endif
}

// a task already notified is written again: a parker that saw the old notification resets it with an
// exchange, and one of the two read modify writes then publishes the waker's condition to the other.
void cinc__unpark(cinc_task *t) {
  uint32_t s = atomic_load_explicit(&t->wstate, memory_order_relaxed);
  do {
    if (s == CINC__DEAD) return;
  } while (!atomic_compare_exchange_weak_explicit(&t->wstate, &s, CINC__NOTIFIED, memory_order_acq_rel,
                                                  memory_order_relaxed));
  if (s == CINC__NOTIFIED) return;
  if (s == CINC__PARKED_THREAD) {
    // at worst a stale parker of the same task, which wakes for nothing and waits again.
    cinc__parker *p = t->parker;
    pthread_mutex_lock(&p->lock);
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
    return;
  }
  if (s != CINC__PARKED_WORKER) return;
  cinc__worker *w = t->home;
  if (cinc__self()->worker == w) {
    cinc__runq_push(w, t);
    return;
  }
  t->msg = CINC__MSG_WAKE;
  cinc__mpsc_push(&w->inbox, &t->node);
  cinc__notify(w);
}

static void park_worker(cinc__thread *th, cinc_task *t, uint64_t until) {
  uint32_t s = CINC__IDLE;
  if (!atomic_compare_exchange_strong_explicit(&t->wstate, &s, CINC__PARKED_WORKER, memory_order_acq_rel,
                                               memory_order_acquire)) {
    atomic_exchange_explicit(&t->wstate, CINC__IDLE, memory_order_acq_rel);
    return;
  }
  cinc__worker *w = th->worker;
  if (until != CINC__NEVER) timer_add(w, t, until);
  suspend(th, t->fiber, CINC__ACT_NONE);
  if (t->timer_slot) timer_remove(w, t);
}

static void park_thread(cinc__thread *th, cinc_task *t, uint64_t until) {
  cinc__parker *p = parker_of(th);
  t->parker = p;
  uint32_t s = CINC__IDLE;
  if (!atomic_compare_exchange_strong_explicit(&t->wstate, &s, CINC__PARKED_THREAD, memory_order_acq_rel,
                                               memory_order_acquire)) {
    atomic_exchange_explicit(&t->wstate, CINC__IDLE, memory_order_acq_rel);
    return;
  }
  pthread_mutex_lock(&p->lock);
  while (atomic_load_explicit(&t->wstate, memory_order_acquire) == CINC__PARKED_THREAD) {
    if (until == CINC__NEVER) {
      pthread_cond_wait(&p->cond, &p->lock);
      continue;
    }
    uint64_t now = cinc__now_ns();
    if (now >= until) break;
#if defined(__APPLE__)
    uint64_t left = until - now;
    struct timespec ts = {.tv_sec = (time_t)(left / 1'000'000'000u), .tv_nsec = (long)(left % 1'000'000'000u)};
    pthread_cond_timedwait_relative_np(&p->cond, &p->lock, &ts);
#else
    struct timespec ts = {.tv_sec = (time_t)(until / 1'000'000'000u), .tv_nsec = (long)(until % 1'000'000'000u)};
    pthread_cond_timedwait(&p->cond, &p->lock, &ts);
#endif
  }
  pthread_mutex_unlock(&p->lock);
  atomic_exchange_explicit(&t->wstate, CINC__IDLE, memory_order_acq_rel);
}

// parks until an unpark or `until`. it may also return early, every caller waits in a loop on its condition.
void cinc__park(cinc__thread *th, cinc_task *t, uint64_t until) {
  if (cinc__in_worker(th)) park_worker(th, t, until);
  else park_thread(th, t, until);
}

/* ========================================================================================================== */
/* wait queues.                                                                                               */
/* ========================================================================================================== */

void cinc__wq_push(void **head, cinc__waiter *w) {
  cinc__waiter *h = *head;
  if (!h) {
    w->next = w->prev = w;
    *head = w;
    return;
  }
  w->next = h;
  w->prev = h->prev;
  h->prev->next = w;
  h->prev = w;
}

void cinc__wq_push_front(void **head, cinc__waiter *w) {
  cinc__wq_push(head, w);
  *head = w;
}

void cinc__wq_remove(void **head, cinc__waiter *w) {
  if (w->next == w) {
    *head = nullptr;
    return;
  }
  w->prev->next = w->next;
  w->next->prev = w->prev;
  if (*head == w) *head = w->next;
}

cinc__waiter *cinc__wq_pop(void **head) {
  cinc__waiter *w = *head;
  if (w) cinc__wq_remove(head, w);
  return w;
}

// called once the lock is released, after claiming `w` under it. the waiter does not leave before this
// returns, so its task and its stack are still there.
void cinc__wq_release(cinc__waiter *w) {
  cinc__unpark(w->task);
  atomic_store_explicit(&w->state, CINC__RELEASED, memory_order_release);
}

static void wait_released(_Atomic(uint32_t) *state) {
  uint32_t spins = 0;
  while (atomic_load_explicit(state, memory_order_acquire) != CINC__RELEASED) cinc__backoff(&spins);
}

// `w` is queued behind the lock, which is released. returns CINC_OK once a waker claimed it, or the interruption
// after taking it out of the queue.
cinc_error cinc__wq_wait(cinc__thread *th, cinc_task *t, cinc__waiter *w, _Atomic(uint32_t) *word, uint32_t bit,
                         void **head, bool interruptible) {
  for (;;) {
    cinc__park(th, t, interruptible ? t->deadline : CINC__NEVER);
    if (atomic_load_explicit(&w->state, memory_order_acquire) != CINC__QUEUED) break;
    cinc_error e = interruptible ? cinc__interrupted(t) : CINC_OK;
    if (!e) continue;
    cinc__lock_bit(word, bit);
    bool queued = atomic_load_explicit(&w->state, memory_order_relaxed) == CINC__QUEUED;
    if (queued) cinc__wq_remove(head, w);
    cinc__unlock_bit(word, bit);
    if (queued) return e;
    break;
  }
  wait_released(&w->state);
  return CINC_OK;
}

/* ========================================================================================================== */
/* cancellation and deadlines.                                                                                */
/* ========================================================================================================== */

static void cancel_block(cinc__block *b) {
  b->cancelled = true;
  for (cinc_task *c = b->children; c; c = c->sibling)
    if (!atomic_load_explicit(&c->done, memory_order_acquire)) cinc_cancel(c);
}

// a cancelled task passes it on to everything it spawned, from its own thread, where its blocks are safe to walk.
static void propagate(cinc_task *t) {
  t->propagated = true;
  for (cinc__block *b = t->blocks; b; b = b->outer) cancel_block(b);
}

cinc_error cinc__interrupted(cinc_task *t) {
  if (atomic_load_explicit(&t->cancelled, memory_order_relaxed)) {
    if (!t->propagated) propagate(t);
    return CINC_ERROR_CANCELLED;
  }
  if (t->deadline != CINC__NEVER && cinc__now_ns() >= t->deadline) {
    t->fired = true;
    return CINC_ERROR_TIMEOUT;
  }
  return CINC_OK;
}

void cinc_cancel(cinc_task *t) {
  if (!t || atomic_load_explicit(&t->done, memory_order_acquire)) return;
  if (atomic_exchange_explicit(&t->cancelled, true, memory_order_acq_rel)) return;
  cinc__unpark(t);
}

cinc_error cinc_check(void) { return cinc__interrupted(cinc__cur(cinc__self())); }

uint64_t cinc_remaining_ms(void) {
  cinc_task *t = cinc__cur(cinc__self());
  if (t->deadline == CINC__NEVER) return UINT64_MAX;
  uint64_t now = cinc__now_ns();
  return now >= t->deadline ? 0 : (t->deadline - now + 999'999u) / 1'000'000u;
}

cinc__scope cinc__deadline_push(uint64_t ms) {
  cinc_task *t = cinc__cur(cinc__self());
  uint64_t prev = t->deadline, at = cinc__after_ms(cinc__now_ns(), ms);
  cinc__scope s = {.impl = (void *)(uintptr_t)prev, .once = t->fired};
  t->deadline = at < prev ? at : prev;
  t->fired = false;
  return s;
}

// the outer deadline is back, and it fired too when the inner one was the outer one, clamped.
static void deadline_restore(cinc__scope *s) {
  cinc_task *t = cinc__cur(cinc__self());
  uint64_t prev = (uintptr_t)s->impl;
  bool fired = t->fired;
  t->fired = s->once || (fired && t->deadline >= prev);
  t->deadline = prev;
}

bool cinc__deadline_step(cinc__scope *s) {
  switch (s->phase) {
  case 0:
    s->phase = 1;
    return true;
  case 1: {
    bool fired = cinc__cur(cinc__self())->fired;
    // the else branch runs under the outer deadline, so it can still clean up.
    deadline_restore(s);
    s->phase = fired ? 2 : 3;
    return fired;
  }
  default:
    s->phase = 3;
    return false;
  }
}

void cinc__deadline_pop(cinc__scope *s) {
  if (s->phase <= 1) deadline_restore(s);
}

/* ========================================================================================================== */
/* spawning.                                                                                                  */
/* ========================================================================================================== */

void cinc__target(uint32_t worker) {
  cinc__thread *th = cinc__self();
  th->next_targeted = true;
  th->next_target = worker;
}

void cinc__detach(cinc_group *group) {
  cinc__thread *th = cinc__self();
  th->next_detach = true;
  th->next_group = group;
}

void *cinc__frame(size_t size, size_t align) {
  cinc__thread *th = cinc__self();
  cinc_task *t = cinc__cur(th);
  size_t a = align > alignof(cinc_task) ? align : alignof(cinc_task);
  size_t offset = round_up(sizeof(cinc_task), align);
  bool detached = th->next_detach || !t->blocks;
  cinc_task *h;
  if (detached && cinc__in_worker(th)) {
    // it outlives whatever spawned it, so it lives on the heap until it returns.
    h = a <= 16 ? cilo_calloc(1, offset + size) : memset(cilo_memalign(a, offset + size), 0, offset + size);
  } else {
    th->pending_mark = cilo_mark_of(t->frames);
    h = cilo_arena_alloc_zeroed(t->frames, offset + size, a);
  }
  h->detached = detached;
  th->pending = h;
  return (char *)h + offset;
}

static void detached_add(cinc__worker *w, cinc_task *t) {
  t->dprev = nullptr;
  t->dnext = w->detached;
  if (w->detached) w->detached->dprev = t;
  w->detached = t;
}

static void detached_remove(cinc__worker *w, cinc_task *t) {
  if (t->dprev) t->dprev->dnext = t->dnext;
  else w->detached = t->dnext;
  if (t->dnext) t->dnext->dprev = t->dprev;
}

enum : uint64_t { GROUP_LIVE = (1ull << 48) - 1, GROUP_WAITER = 1ull << 48 };

static void group_enter(cinc_group *g, cinc__runtime *rt) {
  atomic_fetch_add_explicit(&g->state, 1, memory_order_relaxed);
  atomic_store_explicit(&g->runtime, rt, memory_order_relaxed);
}

// with no one waiting the count drops without a lock. with waiters it drops under the lock, so a waiter
// told the group is empty never leaves while this still holds the group.
static void group_leave(cinc_group *g) {
  uint64_t s = atomic_load_explicit(&g->state, memory_order_relaxed);
  while (!(s >> 48))
    if (atomic_compare_exchange_weak_explicit(&g->state, &s, s - 1, memory_order_acq_rel, memory_order_relaxed))
      return;
  cinc__lock_bit(&g->lock, 1);
  s = atomic_fetch_sub_explicit(&g->state, 1, memory_order_acq_rel) - 1;
  cinc__waiter *woken = nullptr;
  if (!(s & GROUP_LIVE)) {
    for (cinc__waiter *w; (w = cinc__wq_pop(&g->waiters));) {
      atomic_fetch_sub_explicit(&g->state, GROUP_WAITER, memory_order_relaxed);
      atomic_store_explicit(&w->state, CINC__CLAIMED, memory_order_relaxed);
      w->next = woken;
      woken = w;
    }
  }
  cinc__unlock_bit(&g->lock, 1);
  while (woken) {
    cinc__waiter *next = woken->next;
    cinc__wq_release(woken);
    woken = next;
  }
}

cinc_task *cinc__start(void (*entry)(void *frame), void *frame) {
  cinc__thread *th = cinc__self();
  cinc_task *h = th->pending, *t = cinc__cur(th);
  cinc_group *group = th->next_group;
  bool targeted = th->next_targeted;
  th->pending = nullptr;
  th->next_detach = th->next_targeted = false;
  th->next_group = nullptr;
  h->entry = entry;
  h->frame = frame;

  if (!cinc__in_worker(th)) {
    // no worker to run it: it runs here and now, and a detached one leaves nothing behind.
    cilo_mark mark = th->pending_mark;
    entry(frame);
    atomic_store_explicit(&h->done, 1, memory_order_release);
    if (!h->detached) return h;
    cilo_rewind(mark);
    return nullptr;
  }

  cinc__worker *w = th->worker;
  cinc__runtime *rt = w->rt;
  cinc__worker *home = targeted ? &rt->workers[th->next_target % rt->nworkers] : w;
  h->home = home;
  if (h->detached) {
    h->deadline = CINC__NEVER;
    h->group = group;
    if (group) {
      group_enter(group, rt);
      if (atomic_load_explicit(&group->cancelled, memory_order_acquire)) h->cancelled = true;
    }
    atomic_fetch_add_explicit(&rt->roots, 1, memory_order_relaxed);
  } else {
    cinc__block *b = t->blocks;
    h->parent = b;
    h->sibling = b->children;
    b->children = h;
    b->live++;
    h->owner_home = w;
    h->deadline = t->deadline;
    if (b->cancelled || atomic_load_explicit(&t->cancelled, memory_order_relaxed)) h->cancelled = true;
  }

  // a detached task on another worker may be done and freed as soon as it is pushed.
  bool detached = h->detached;
  if (home == w) {
    if (detached) detached_add(w, h);
    cinc__runq_push(w, h);
  } else {
    h->msg = CINC__MSG_SPAWN;
    cinc__mpsc_push(&home->inbox, &h->node);
    cinc__notify(home);
  }
  return detached ? nullptr : h;
}

/* ========================================================================================================== */
/* blocks and joins.                                                                                          */
/* ========================================================================================================== */

cinc__scope cinc__block_enter(void) {
  cinc_task *t = cinc__cur(cinc__self());
  cilo_mark mark = cilo_mark_of(t->frames);
  cinc__block *b = cilo_arena_alloc(t->frames, sizeof *b, alignof(cinc__block));
  *b = (cinc__block){.outer = t->blocks, .owner = t, .mark = mark};
  t->blocks = b;
  return (cinc__scope){.impl = b, .once = true};
}

// waits for every child. it cannot give up, but a cancelled owner passes the cancellation on meanwhile.
static void block_wait(cinc__block *b) {
  if (!b->live) return;
  cinc__thread *th = cinc__self();
  cinc_task *t = cinc__cur(th);
  while (b->live) {
    if (atomic_load_explicit(&t->cancelled, memory_order_relaxed) && !t->propagated) propagate(t);
    b->waiting = true;
    cinc__park(th, t, CINC__NEVER);
    b->waiting = false;
  }
}

void cinc__block_end(cinc__scope *s) {
  block_wait(s->impl);
  s->once = false;
}

void cinc__block_leave(cinc__scope *s) {
  cinc__block *b = s->impl;
  if (s->once) {
    // left with break, return or goto.
    cancel_block(b);
    block_wait(b);
  }
  b->owner->blocks = b->outer;
  cilo_rewind(b->mark);
}

// on the owner's worker, once a child returned.
static void child_done(cinc_task *c) {
  cinc__block *b = c->parent;
  atomic_store_explicit(&c->done, 1, memory_order_release);
  cinc__lock_bit(&c->jlock, 1);
  cinc__waiter *woken = nullptr;
  for (cinc__waiter *w; (w = cinc__wq_pop(&c->joiners));) {
    atomic_store_explicit(&w->state, CINC__CLAIMED, memory_order_relaxed);
    w->next = woken;
    woken = w;
  }
  cinc__unlock_bit(&c->jlock, 1);
  while (woken) {
    cinc__waiter *next = woken->next;
    cinc__wq_release(woken);
    woken = next;
  }
  if (!--b->live && b->waiting) cinc__unpark(b->owner);
}

void cinc__await_detached(void) {
  fputs("cinc: await of a detached task. a spawn outside CINC_CONCURRENTLY, or with CINC_SPAWN_IN, gives no "
        "result, spawn it inside a block to await it\n",
        stderr);
  abort();
}

cinc_error cinc_join(cinc_task *c) {
  if (!c) return CINC_ERROR_CLOSED;
  cinc__thread *th = cinc__self();
  cinc_task *t = cinc__cur(th);
  for (;;) {
    if (atomic_load_explicit(&c->done, memory_order_acquire))
      return atomic_load_explicit(&c->cancelled, memory_order_relaxed) ? CINC_ERROR_CANCELLED : CINC_OK;
    cinc_error e = cinc__interrupted(t);
    if (e) return e;
    cinc__waiter w = {.task = t};
    cinc__lock_bit(&c->jlock, 1);
    if (atomic_load_explicit(&c->done, memory_order_acquire)) {
      cinc__unlock_bit(&c->jlock, 1);
      continue;
    }
    cinc__wq_push(&c->joiners, &w);
    cinc__unlock_bit(&c->jlock, 1);
    e = cinc__wq_wait(th, t, &w, &c->jlock, 1, &c->joiners, true);
    if (e) return e;
  }
}

/* ========================================================================================================== */
/* groups.                                                                                                    */
/* ========================================================================================================== */

cinc_error cinc_group_wait(cinc_group *g) {
  cinc__thread *th = cinc__self();
  cinc_task *t = cinc__cur(th);
  for (;;) {
    if (!(atomic_load_explicit(&g->state, memory_order_acquire) & GROUP_LIVE)) return CINC_OK;
    cinc_error e = cinc__interrupted(t);
    if (e) return e;
    cinc__waiter w = {.task = t};
    cinc__lock_bit(&g->lock, 1);
    uint64_t s = atomic_load_explicit(&g->state, memory_order_acquire);
    bool queued = false;
    while (s & GROUP_LIVE)
      if ((queued = atomic_compare_exchange_weak_explicit(&g->state, &s, s + GROUP_WAITER, memory_order_acq_rel,
                                                          memory_order_acquire)))
        break;
    if (queued) cinc__wq_push(&g->waiters, &w);
    cinc__unlock_bit(&g->lock, 1);
    if (!queued) return CINC_OK;
    e = cinc__wq_wait(th, t, &w, &g->lock, 1, &g->waiters, true);
    if (e) {
      atomic_fetch_sub_explicit(&g->state, GROUP_WAITER, memory_order_relaxed);
      return e;
    }
  }
}

void cinc_group_cancel(cinc_group *g) {
  if (atomic_exchange_explicit(&g->cancelled, true, memory_order_acq_rel)) return;
  cinc__runtime *rt = atomic_load_explicit(&g->runtime, memory_order_acquire);
  if (!rt) return;
  atomic_fetch_add_explicit(&rt->epoch, 1, memory_order_seq_cst);
  for (uint32_t i = 0; i < rt->nworkers; i++) cinc__notify(&rt->workers[i]);
}

uint64_t cinc_group_live(const cinc_group *g) {
  return atomic_load_explicit(&g->state, memory_order_acquire) & GROUP_LIVE;
}

/* ========================================================================================================== */
/* the scheduler.                                                                                             */
/* ========================================================================================================== */

static void stop_all(cinc__runtime *rt) {
  atomic_store_explicit(&rt->stop, true, memory_order_seq_cst);
  for (uint32_t i = 0; i < rt->nworkers; i++) cinc__notify(&rt->workers[i]);
}

static void finish(cinc__worker *w, cinc_task *t) {
  fiber_put(w, t->fiber);
  t->fiber = nullptr;
  atomic_store_explicit(&t->wstate, CINC__DEAD, memory_order_release);
  if (!t->detached) {
    if (t->owner_home == w) {
      child_done(t);
    } else {
      // the owner's worker accounts for it. past the push the handle may be gone.
      cinc__worker *owner = t->owner_home;
      t->msg = CINC__MSG_DONE;
      cinc__mpsc_push(&owner->inbox, &t->node);
      cinc__notify(owner);
    }
    return;
  }
  cinc__runtime *rt = w->rt;
  cinc_group *g = t->group;
  detached_remove(w, t);
  cilo_mfree(t);
  if (g) group_leave(g);
  if (atomic_fetch_sub_explicit(&rt->roots, 1, memory_order_acq_rel) == 1) stop_all(rt);
}

static void run(cinc__worker *w, cinc__thread *th, cinc_task *t) {
  cinc__fiber *f = t->fiber;
  if (!f) {
    f = fiber_get(w);
    f->task = t;
    t->fiber = f;
    t->arena = &f->arena;
    t->frames = &f->frames;
    t->scratch = f->scratch;
  }
  // a yield leaves it idle, a wake up notified: only the latter needs the exchange.
  if (atomic_load_explicit(&t->wstate, memory_order_relaxed) != CINC__IDLE)
    atomic_exchange_explicit(&t->wstate, CINC__IDLE, memory_order_acq_rel);
  w->worked = true;
  th->current = t;
  cilo_scratch_bind(t->scratch);
  resume(th, f);
  th->current = nullptr;
  switch (th->action) {
  case CINC__ACT_EXIT:
    finish(w, t);
    break;
  case CINC__ACT_BLOCKING:
    cinc__pool_push(w->rt, &t->job);
    break;
  default:
    break;
  }
  th->action = CINC__ACT_NONE;
}

static void drain(cinc__worker *w) {
  for (cinc__node *n; (n = cinc__mpsc_pop(&w->inbox));) {
    cinc_task *t = (cinc_task *)n;
    switch (t->msg) {
    case CINC__MSG_SPAWN:
      if (t->detached) detached_add(w, t);
      cinc__runq_push(w, t);
      break;
    case CINC__MSG_WAKE:
      cinc__runq_push(w, t);
      break;
    case CINC__MSG_DONE:
      child_done(t);
      break;
    }
  }
}

// after a group cancel, the detached tasks of this worker that belong to a cancelled group are cancelled.
static void cancel_groups(cinc__worker *w) {
  w->epoch = atomic_load_explicit(&w->rt->epoch, memory_order_acquire);
  for (cinc_task *t = w->detached; t; t = t->dnext)
    if (t->group && atomic_load_explicit(&t->group->cancelled, memory_order_acquire)) cinc_cancel(t);
}

// work often comes back within microseconds, the other half of a channel ping pong for instance. a worker that
// just ran something spins a little before paying for a sleep, and its wakers for a system call. io gets a look
// now and then meanwhile.
static bool spin(cinc__worker *w, uint64_t next) {
  enum : uint64_t { SPIN_NS = 20'000, POLL_NS = 5'000 };
  // the first look at io is right away, registrations made by the last tasks may be waiting.
  uint64_t start = cinc__now_ns(), polled = 0;
  uint64_t until = start + SPIN_NS < next ? start + SPIN_NS : next;
  for (uint64_t now = start; now < until; now = cinc__now_ns()) {
    for (int i = 0; i < 32; i++) {
      if (cinc__mpsc_pending(&w->inbox)) return true;
      cinc__pause();
    }
    if (w->io_waiters && now - polled >= POLL_NS) {
      polled = now;
      cinc__poller_wait(w, 0);
      if (w->runq_head) return true;
    }
  }
  return false;
}

static void worker_loop(cinc__worker *w, cinc__thread *th) {
  cinc__runtime *rt = w->rt;
  for (;;) {
    drain(w);
    // what is ready now runs, tasks readied meanwhile wait for the next round so io and timers get a turn.
    for (size_t n = w->runq_len; n && w->runq_head; n--) run(w, th, runq_pop(w));
    uint64_t next = timers_fire(w);
    if (atomic_load_explicit(&rt->epoch, memory_order_relaxed) != w->epoch) cancel_groups(w);
    if (w->runq_head || cinc__mpsc_pending(&w->inbox)) {
      // busy: the poller gets a look now and then, as Go does every 61 rounds, or when registrations wait.
#if CINC__KQUEUE
      bool changes = w->nchanges;
#else
      bool changes = false;
#endif
      if (changes || (w->io_waiters && ++w->ticks % 61 == 0)) cinc__poller_wait(w, 0);
      continue;
    }
    if (atomic_load_explicit(&rt->stop, memory_order_acquire)) break;
    if (w->worked && spin(w, next)) continue;
    w->worked = false;
    atomic_store_explicit(&w->sleeping, 1, memory_order_seq_cst);
    if (cinc__mpsc_pending(&w->inbox) || atomic_load_explicit(&rt->stop, memory_order_seq_cst) ||
        atomic_load_explicit(&rt->epoch, memory_order_seq_cst) != w->epoch) {
      atomic_store_explicit(&w->sleeping, 0, memory_order_relaxed);
      continue;
    }
    uint64_t now = next == CINC__NEVER ? 0 : cinc__now_ns();
    cinc__poller_wait(w, next == CINC__NEVER ? CINC__NEVER : next > now ? next - now : 0);
    atomic_store_explicit(&w->sleeping, 0, memory_order_relaxed);
  }
}

static void *worker_thread(void *p) {
  cinc__worker *w = p;
  cinc__thread *th = cinc__self();
  th->worker = w;
  th->mode = CINC__WORKER;
  w->th = th;
  worker_loop(w, th);
  th->worker = nullptr;
  th->mode = CINC__PLAIN;
  cilo_scratch_bind(nullptr);
  return nullptr;
}

/* ========================================================================================================== */
/* the runtime.                                                                                               */
/* ========================================================================================================== */

typedef struct main_frame {
  int (*fn)(void *arg);
  void *arg;
  int *result;
} main_frame;

static void main_entry(void *p) {
  main_frame *f = p;
  *f->result = f->fn(f->arg);
}

int cinc_run(const cinc_options *options, int (*main)(void *arg), void *arg) {
  cinc__thread *th = cinc__self();
  if (th->mode != CINC__PLAIN) return main(arg);
  cinc_options o = options ? *options : (cinc_options){};

  cinc__runtime *rt = cilo_calloc(1, sizeof *rt);
  long cores = sysconf(_SC_NPROCESSORS_ONLN);
  rt->nworkers = o.workers ? o.workers : cores > 0 ? (uint32_t)cores : 1;
  rt->page = (size_t)sysconf(_SC_PAGESIZE);
  // asan's frames are several times bigger.
  size_t stack = o.stack_size ? o.stack_size : CILO_ASAN ? 256 << 10 : 64 << 10;
  rt->stack_size = round_up(stack < (16 << 10) ? 16 << 10 : stack, rt->page);
  rt->pool.max = o.blocking_threads ? o.blocking_threads : 64;
  pthread_mutex_init(&rt->pool.lock, nullptr);
  pthread_cond_init(&rt->pool.cond, nullptr);
  rt->workers = cilo_calloc(rt->nworkers, sizeof *rt->workers);
  for (uint32_t i = 0; i < rt->nworkers; i++) {
    cinc__worker *w = &rt->workers[i];
    w->rt = rt;
    w->index = i;
    cinc__mpsc_init(&w->inbox);
    if (!cinc__poller_init(w)) {
      fprintf(stderr, "cinc: cannot create the poller of worker %u: %s\n", i, strerror(errno));
      abort();
    }
  }

  int result = 0;
  size_t offset = round_up(sizeof(cinc_task), alignof(main_frame));
  cinc_task *t = cilo_calloc(1, offset + sizeof(main_frame));
  main_frame *f = (main_frame *)((char *)t + offset);
  *f = (main_frame){.fn = main, .arg = arg, .result = &result};
  *t = (cinc_task){.entry = main_entry, .frame = f, .home = &rt->workers[0], .detached = true,
                   .deadline = CINC__NEVER};
  atomic_store_explicit(&rt->roots, 1, memory_order_relaxed);
  detached_add(&rt->workers[0], t);
  cinc__runq_push(&rt->workers[0], t);

  for (uint32_t i = 1; i < rt->nworkers; i++)
    if (pthread_create(&rt->workers[i].thread, nullptr, worker_thread, &rt->workers[i])) {
      fprintf(stderr, "cinc: cannot start worker %u\n", i);
      abort();
    }
  cilo_arena *own_scratch = cilo_scratch_bind(nullptr);
  th->worker = &rt->workers[0];
  th->mode = CINC__WORKER;
  rt->workers[0].th = th;
  worker_loop(&rt->workers[0], th);
  th->worker = nullptr;
  th->mode = CINC__PLAIN;
  cilo_scratch_bind(own_scratch);

  for (uint32_t i = 1; i < rt->nworkers; i++) pthread_join(rt->workers[i].thread, nullptr);
  cinc__pool_stop(rt);
  for (uint32_t i = 0; i < rt->nworkers; i++) {
    cinc__worker *w = &rt->workers[i];
    while (w->fibers) {
      cinc__fiber *next = w->fibers->next;
      fiber_free(w->fibers);
      w->fibers = next;
    }
    cinc__poller_close(w);
    cilo_mfree(w->timers);
    cilo_mfree(w->fds);
  }
  pthread_mutex_destroy(&rt->pool.lock);
  pthread_cond_destroy(&rt->pool.cond);
  cilo_mfree(rt->workers);
  cilo_mfree(rt);
  return result;
}

typedef struct argv_main {
  int (*fn)(int argc, char **argv);
  int argc;
  char **argv;
} argv_main;

static int run_argv(void *p) {
  argv_main *m = p;
  return m->fn(m->argc, m->argv);
}

int cinc_run_main(const cinc_options *options, int (*main)(int argc, char **argv), int argc, char **argv) {
  argv_main m = {.fn = main, .argc = argc, .argv = argv};
  return cinc_run(options, run_argv, &m);
}

bool cinc_in_task(void) { return cinc__in_worker(cinc__self()); }

uint32_t cinc_worker(void) {
  cinc__thread *th = cinc__self();
  if (th->worker) return th->worker->index;
  return th->current && th->current->home ? th->current->home->index : 0;
}

uint32_t cinc_workers(void) {
  cinc__thread *th = cinc__self();
  if (th->worker) return th->worker->rt->nworkers;
  return th->current && th->current->home ? th->current->home->rt->nworkers : 1;
}

cinc_stats cinc_runtime_stats(void) {
  cinc__thread *th = cinc__self();
  cinc__worker *w = th->worker ? th->worker : th->current ? th->current->home : nullptr;
  if (!w) return (cinc_stats){};
  cinc__runtime *rt = w->rt;
  cinc_stats s = {.stacks_mapped = atomic_load_explicit(&rt->stacks_mapped, memory_order_relaxed)};
  for (uint32_t i = 0; i < rt->nworkers; i++)
    s.stacks_idle += atomic_load_explicit(&rt->workers[i].nfibers, memory_order_relaxed);
  pthread_mutex_lock(&rt->pool.lock);
  s.blocking_threads = rt->pool.threads;
  pthread_mutex_unlock(&rt->pool.lock);
  return s;
}

cilo_arena *cinc_arena(void) { return cinc__cur(cinc__self())->arena; }

cinc_error cinc_sleep(uint64_t ms) {
  cinc__thread *th = cinc__self();
  cinc_task *t = cinc__cur(th);
  cinc_error e = cinc__interrupted(t);
  if (e) return e;
  uint64_t until = cinc__after_ms(cinc__now_ns(), ms);
  for (;;) {
    // a sleep that would end past the deadline times out, even when a late wake up saw both pass.
    if (cinc__now_ns() >= until && until <= t->deadline) return CINC_OK;
    if ((e = cinc__interrupted(t))) return e;
    cinc__park(th, t, until < t->deadline ? until : t->deadline);
  }
}

cinc_error cinc_yield(void) {
  cinc__thread *th = cinc__self();
  if (cinc__in_worker(th)) {
    cinc_task *t = th->current;
    cinc__runq_push(th->worker, t);
    suspend(th, t->fiber, CINC__ACT_NONE);
    return cinc__interrupted(t);
  }
  sched_yield();
  return cinc__interrupted(cinc__cur(th));
}

/* ========================================================================================================== */
/* the blocking pool.                                                                                         */
/* ========================================================================================================== */

static void job_finish(cinc__job *j) {
  uint32_t s = CINC__QUEUED;
  if (!atomic_compare_exchange_strong_explicit(&j->state, &s, CINC__CLAIMED, memory_order_acq_rel,
                                               memory_order_acquire)) {
    // its waiter gave up and left it to us.
    j->abandon(j);
    return;
  }
  cinc__unpark(j->task);
  atomic_store_explicit(&j->state, CINC__RELEASED, memory_order_release);
}

static void run_job(cinc__thread *th, cinc__job *j) {
  cinc_task *t = j->task;
  if (j->resume) {
    th->mode = CINC__BLOCKING;
    th->current = t;
    cilo_scratch_bind(t->scratch);
    resume(th, t->fiber);
    th->current = nullptr;
    th->mode = CINC__PLAIN;
    th->action = CINC__ACT_NONE;
    cilo_scratch_bind(nullptr);
    // the block ended, the task goes home.
    cinc__worker *home = t->home;
    t->msg = CINC__MSG_WAKE;
    cinc__mpsc_push(&home->inbox, &t->node);
    cinc__notify(home);
    return;
  }
  if (t) {
    th->mode = CINC__BLOCKING;
    th->current = t;
    cilo_scratch_bind(t->scratch);
  }
  j->fn(j->arg);
  if (t) {
    th->current = nullptr;
    th->mode = CINC__PLAIN;
    cilo_scratch_bind(nullptr);
  }
  job_finish(j);
}

static void *pool_thread(void *p) {
  cinc__runtime *rt = p;
  cinc__pool *pool = &rt->pool;
  cinc__thread *th = cinc__self();
  pthread_mutex_lock(&pool->lock);
  for (;;) {
    while (!pool->head && !pool->stop) {
      pool->idle++;
      pthread_cond_wait(&pool->cond, &pool->lock);
      pool->idle--;
    }
    if (!pool->head) break;
    cinc__job *j = pool->head;
    pool->head = j->next;
    if (!pool->head) pool->tail = nullptr;
    pthread_mutex_unlock(&pool->lock);
    run_job(th, j);
    pthread_mutex_lock(&pool->lock);
  }
  pthread_mutex_unlock(&pool->lock);
  return nullptr;
}

void cinc__pool_push(cinc__runtime *rt, cinc__job *j) {
  cinc__pool *pool = &rt->pool;
  j->next = nullptr;
  pthread_mutex_lock(&pool->lock);
  if (pool->tail) pool->tail->next = j;
  else pool->head = j;
  pool->tail = j;
  if (pool->idle) {
    pthread_cond_signal(&pool->cond);
  } else if (pool->threads < pool->max) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 << 20);
    pthread_t tid;
    if (!pthread_create(&tid, &attr, pool_thread, rt)) {
      pool->threads++;
      if (pool->ntids % 16 == 0) pool->tids = cilo_mrealloc(pool->tids, (pool->ntids + 16) * sizeof(pthread_t));
      pool->tids[pool->ntids++] = tid;
    }
    pthread_attr_destroy(&attr);
  }
  pthread_mutex_unlock(&pool->lock);
}

void cinc__pool_stop(cinc__runtime *rt) {
  cinc__pool *pool = &rt->pool;
  pthread_mutex_lock(&pool->lock);
  pool->stop = true;
  pthread_cond_broadcast(&pool->cond);
  pthread_mutex_unlock(&pool->lock);
  for (uint32_t i = 0; i < pool->ntids; i++) pthread_join(pool->tids[i], nullptr);
  cilo_mfree(pool->tids);
}

cinc_error cinc__offload(cinc__thread *th, cinc_task *t, cinc__job *j, bool interruptible) {
  j->task = t;
  atomic_store_explicit(&j->state, CINC__QUEUED, memory_order_relaxed);
  cinc__pool_push(th->worker->rt, j);
  for (;;) {
    uint32_t s = atomic_load_explicit(&j->state, memory_order_acquire);
    if (s != CINC__QUEUED) break;
    cinc_error e = interruptible ? cinc__interrupted(t) : CINC_OK;
    if (e && atomic_compare_exchange_strong_explicit(&j->state, &s, CINC__ABANDONED, memory_order_acq_rel,
                                                     memory_order_acquire))
      return e;
    if (!e) cinc__park(th, t, interruptible ? t->deadline : CINC__NEVER);
  }
  wait_released(&j->state);
  return CINC_OK;
}

cinc_error cinc_blocking(void (*fn)(void *arg), void *arg) {
  cinc__thread *th = cinc__self();
  if (!cinc__in_worker(th)) {
    cinc_error e = cinc__interrupted(cinc__cur(th));
    if (!e) fn(arg);
    return e;
  }
  cinc_task *t = th->current;
  cinc_error e = cinc__interrupted(t);
  if (e) return e;
  cinc__job j = {.fn = fn, .arg = arg, .task = t};
  return cinc__offload(th, t, &j, false);
}

cinc__scope cinc__blocking_enter(void) {
  cinc__thread *th = cinc__self();
  if (!cinc__in_worker(th)) return (cinc__scope){.once = true};
  cinc_task *t = th->current;
  t->job = (cinc__job){.task = t, .resume = true};
  // the scheduler hands the task to the pool once it is off its stack, and this returns on a pool thread.
  suspend(th, t->fiber, CINC__ACT_BLOCKING);
  return (cinc__scope){.impl = t, .once = true};
}

void cinc__blocking_leave(cinc__scope *s) {
  cinc_task *t = s->impl;
  if (!t) return;
  s->impl = nullptr;
  suspend(cinc__self(), t->fiber, CINC__ACT_UNBLOCK);
}
