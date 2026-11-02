#include "check.h"

#include <pthread.h>

enum { PRODUCERS = 4, CONSUMERS = 4, EACH = 5'000, SLOTS = 8 };

// a bounded queue on a mutex and two conditions.
typedef struct Queue {
  cinc_mutex lock;
  cinc_cond not_empty, not_full;
  uint64_t items[SLOTS];
  size_t head, len;
  bool closed;
} Queue;

static cinc_error put(Queue *q, uint64_t v) {
  cinc_error e = CINC_OK;
  locked(&q->lock) {
    while (q->len == SLOTS)
      if ((e = cinc_cond_wait(&q->not_full, &q->lock))) break;
    if (e) break;
    q->items[(q->head + q->len++) % SLOTS] = v;
    cinc_cond_signal(&q->not_empty);
  }
  return e;
}

// false once the queue is closed and empty.
static bool get(Queue *q, uint64_t *v) {
  bool got = false;
  locked(&q->lock) {
    while (!q->len && !q->closed)
      if (cinc_cond_wait(&q->not_empty, &q->lock)) break;
    if (!q->len) break;
    *v = q->items[q->head];
    q->head = (q->head + 1) % SLOTS;
    q->len--;
    got = true;
    cinc_cond_signal(&q->not_full);
  }
  return got;
}

static void close_queue(Queue *q) {
  locked(&q->lock) {
    q->closed = true;
    cinc_cond_broadcast(&q->not_empty);
  }
}

static int produce(Queue *q, uint64_t base) {
  for (uint64_t i = 0; i < EACH; i++)
    if (put(q, base + i)) return 1;
  return 0;
}
spawnable(produce, Queue *, uint64_t);

static uint64_t consume(Queue *q) {
  uint64_t sum = 0, v;
  while (get(q, &v)) sum += v;
  return sum;
}
spawnable(consume, Queue *);

static void *plain_consume(void *p) { return (void *)(uintptr_t)consume(p); }

typedef struct Gate {
  cinc_mutex lock;
  cinc_cond open;
  bool opened;
  _Atomic(int) through;
} Gate;

static int pass(Gate *g) {
  cinc_error e = CINC_OK;
  locked(&g->lock) {
    while (!g->opened)
      if ((e = cinc_cond_wait(&g->open, &g->lock))) break;
  }
  if (!e) atomic_fetch_add(&g->through, 1);
  return e;
}
spawnable(pass, Gate *);

static int body(void *) {
  // producers and consumers on every worker, and a plain thread consuming too.
  Queue q = {};
  pthread_t th;
  pthread_create(&th, nullptr, plain_consume, &q);
  uint64_t sum = 0;
  concurrently {
    future(consume) consumers[CONSUMERS];
    for (uint32_t i = 0; i < CONSUMERS; i++) consumers[i] = spawn_on(i, consume(&q));
    concurrently {
      for (uint32_t i = 0; i < PRODUCERS; i++) spawn_on(i + 1, produce(&q, 0));
    }
    close_queue(&q);
    for (int i = 0; i < CONSUMERS; i++) sum += await(consumers[i]);
  }
  void *plain;
  pthread_join(th, &plain);
  sum += (uintptr_t)plain;
  CHECK(sum == (uint64_t)PRODUCERS * EACH * (EACH - 1) / 2);

  // a broadcast lets every waiter through.
  Gate g = {};
  concurrently {
    for (uint32_t i = 0; i < 16; i++) spawn_on(i, pass(&g));
    cinc_sleep(10);
    CHECK(atomic_load(&g.through) == 0);
    locked(&g.lock) {
      g.opened = true;
      cinc_cond_broadcast(&g.open);
    }
  }
  CHECK(atomic_load(&g.through) == 16);

  // a wait times out or is cancelled with the mutex held again.
  Gate shut = {};
  within(20) {
    cinc_lock(&shut.lock);
    CHECK(cinc_cond_wait(&shut.open, &shut.lock) == CINC_ERROR_TIMEOUT);
    cinc_unlock(&shut.lock);
  }
  concurrently {
    auto p = spawn_on(1, pass(&shut));
    cinc_sleep(10);
    cancel(p);
    CHECK(cinc_join(p.task) == CINC_ERROR_CANCELLED);
  }
  // the cancelled waiter left the queue: a signal reaches the next one.
  concurrently {
    auto p = spawn_on(2, pass(&shut));
    cinc_sleep(10);
    locked(&shut.lock) {
      shut.opened = true;
      cinc_cond_signal(&shut.open);
    }
    CHECK(await(p) == CINC_OK);
  }
  return 0;
}

int main(void) {
  CHECK(CINC_RUN(body, nullptr, .workers = 4) == 0);
  return check_done("cond");
}
