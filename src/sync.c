#include "internal.h"

#include <string.h>

/* ========================================================================================================== */
/* channels: a ring behind a short lock, and waiters handed their value directly.                             */
/* ========================================================================================================== */

struct cinc_chan {
  _Atomic(uint32_t) lock;
  bool closed;
  size_t elem;
  size_t cap;
  size_t len;
  size_t head;
  void *senders;
  void *receivers;
  unsigned char *ring;
};

cinc_chan *cinc_chan_new_(size_t elem_size, size_t elem_align, size_t capacity) {
  size_t align = elem_align > alignof(cinc_chan) ? elem_align : alignof(cinc_chan);
  size_t offset = (sizeof(cinc_chan) + align - 1) & ~(align - 1), ring, size;
  if (ckd_mul(&ring, elem_size, capacity) || ckd_add(&size, offset, ring)) cilo_oom(SIZE_MAX);
  cinc_chan *ch = align <= 16 ? cilo_malloc(size) : cilo_memalign(align, size);
  *ch = (cinc_chan){.elem = elem_size, .cap = capacity, .ring = (unsigned char *)ch + offset};
  return ch;
}

void cinc_chan_free_(cinc_chan *ch) { cilo_mfree(ch); }

static void claim(cinc__waiter *w, cinc_error result) {
  w->result = result;
  atomic_store_explicit(&w->state, CINC__CLAIMED, memory_order_relaxed);
}

static void release_all(cinc__waiter *w) {
  while (w) {
    cinc__waiter *next = w->next;
    cinc__wq_release(w);
    w = next;
  }
}

static void ring_put(cinc_chan *ch, const void *value) {
  size_t at = ch->head + ch->len++;
  memcpy(ch->ring + (at < ch->cap ? at : at - ch->cap) * ch->elem, value, ch->elem);
}

// under the lock: sends when it needs no wait, and gives a waiter to release once the lock is let go.
static bool try_send(cinc_chan *ch, const void *value, cinc_error *result, cinc__waiter **woken) {
  *result = CINC_OK;
  if (ch->closed) {
    *result = CINC_ERROR_CLOSED;
    return true;
  }
  cinc__waiter *r = cinc__wq_pop(&ch->receivers);
  if (r) {
    memcpy(r->slot, value, ch->elem);
    claim(r, CINC_OK);
    *woken = r;
    return true;
  }
  if (ch->len < ch->cap) {
    ring_put(ch, value);
    return true;
  }
  return false;
}

static bool try_recv(cinc_chan *ch, void *out, cinc_error *result, cinc__waiter **woken) {
  *result = CINC_OK;
  if (ch->len) {
    memcpy(out, ch->ring + ch->head * ch->elem, ch->elem);
    ch->head = ch->head + 1 == ch->cap ? 0 : ch->head + 1;
    ch->len--;
    // a waiting sender takes the free slot.
    cinc__waiter *s = cinc__wq_pop(&ch->senders);
    if (s) {
      ring_put(ch, s->slot);
      claim(s, CINC_OK);
      *woken = s;
    }
    return true;
  }
  cinc__waiter *s = cinc__wq_pop(&ch->senders);
  if (s) {
    memcpy(out, s->slot, ch->elem);
    claim(s, CINC_OK);
    *woken = s;
    return true;
  }
  if (ch->closed) {
    *result = CINC_ERROR_CLOSED;
    return true;
  }
  return false;
}

// a send or a receive that can go on right away does, even in a task that is cancelled or past its deadline:
// handing back a permit or ringing a bell from such a task still works. only a wait is interrupted.
static cinc_error transfer(cinc_chan *ch, void *value, bool send) {
  cinc__thread *th = cinc__self();
  cinc_task *t = cinc__cur(th);
  cinc__waiter *woken = nullptr, w = {.task = t, .slot = value};
  void **queue = send ? &ch->senders : &ch->receivers;
  cinc_error e;
  cinc__lock_bit(&ch->lock, 1);
  bool done = send ? try_send(ch, value, &e, &woken) : try_recv(ch, value, &e, &woken);
  if (!done) cinc__wq_push(queue, &w);
  cinc__unlock_bit(&ch->lock, 1);
  if (done) {
    if (woken) cinc__wq_release(woken);
    return e;
  }
  e = cinc__wq_wait(th, t, &w, &ch->lock, 1, queue, true);
  return e ? e : w.result;
}

cinc_error cinc_chan_send_(cinc_chan *ch, const void *value) { return transfer(ch, (void *)value, true); }

cinc_error cinc_chan_recv_(cinc_chan *ch, void *out) { return transfer(ch, out, false); }

void cinc_chan_close_(cinc_chan *ch) {
  cinc__waiter *woken = nullptr;
  cinc__lock_bit(&ch->lock, 1);
  if (!ch->closed) {
    ch->closed = true;
    // what is buffered stays for the receivers, only the waiters are let go.
    for (void **q = &ch->senders; q; q = q == &ch->senders ? &ch->receivers : nullptr)
      for (cinc__waiter *w; (w = cinc__wq_pop(q));) {
        claim(w, CINC_ERROR_CLOSED);
        w->next = woken;
        woken = w;
      }
  }
  cinc__unlock_bit(&ch->lock, 1);
  release_all(woken);
}

/* ========================================================================================================== */
/* mutexes. the state word holds the lock, a lock on the waiter list and whether it has any.                  */
/* ========================================================================================================== */

enum : uint32_t { LOCKED = 1, QLOCK = 2, WAITERS = 4 };

static void lock_slow(cinc_mutex *m) {
  cinc__thread *th = cinc__self();
  cinc_task *t = cinc__cur(th);
  bool woken = false;
  for (uint32_t spins = 0;;) {
    uint32_t s = atomic_load_explicit(&m->state, memory_order_relaxed);
    if (!(s & LOCKED)) {
      if (atomic_compare_exchange_weak_explicit(&m->state, &s, s | LOCKED, memory_order_acquire,
                                                memory_order_relaxed))
        return;
      continue;
    }
    // a short spin first: the holder is often about to let go.
    if (spins < 32 && !(s & WAITERS)) {
      spins++;
      cinc__pause();
      continue;
    }
    cinc__lock_bit(&m->state, QLOCK);
    s = atomic_load_explicit(&m->state, memory_order_relaxed);
    bool queued = false;
    while (s & LOCKED)
      if ((queued = atomic_compare_exchange_weak_explicit(&m->state, &s, s | WAITERS, memory_order_acq_rel,
                                                          memory_order_relaxed)))
        break;
    cinc__waiter w = {.task = t};
    if (queued) {
      // one already woken goes back to the front, so it is not passed over forever.
      if (woken) cinc__wq_push_front(&m->waiters, &w);
      else cinc__wq_push(&m->waiters, &w);
    }
    cinc__unlock_bit(&m->state, QLOCK);
    if (!queued) continue;
    cinc__wq_wait(th, t, &w, &m->state, QLOCK, &m->waiters, false);
    woken = true;
    spins = 0;
  }
}

void cinc_lock(cinc_mutex *m) {
  uint32_t s = 0;
  if (CILO_LIKELY(atomic_compare_exchange_strong_explicit(&m->state, &s, LOCKED, memory_order_acquire,
                                                          memory_order_relaxed)))
    return;
  lock_slow(m);
}

void cinc_unlock(cinc_mutex *m) {
  uint32_t s = LOCKED;
  if (CILO_LIKELY(atomic_compare_exchange_strong_explicit(&m->state, &s, 0, memory_order_release,
                                                          memory_order_relaxed)))
    return;
  s = atomic_fetch_and_explicit(&m->state, ~LOCKED, memory_order_release);
  if (!(s & WAITERS)) return;
  // the lock is free already, the first waiter is woken to compete for it.
  cinc__lock_bit(&m->state, QLOCK);
  cinc__waiter *w = cinc__wq_pop(&m->waiters);
  if (!m->waiters) atomic_fetch_and_explicit(&m->state, ~WAITERS, memory_order_relaxed);
  if (w) atomic_store_explicit(&w->state, CINC__CLAIMED, memory_order_relaxed);
  cinc__unlock_bit(&m->state, QLOCK);
  if (w) cinc__wq_release(w);
}

cinc__scope cinc__lock_enter(cinc_mutex *m) {
  cinc_lock(m);
  return (cinc__scope){.impl = m, .once = true};
}

void cinc__lock_leave(cinc__scope *s) { cinc_unlock(s->impl); }
