#include "check.h"

#include <pthread.h>

enum { TASKS = 64, ROUNDS = 2'000, THREADS = 4 };

typedef struct Shared {
  cinc_mutex lock;
  uint64_t counter;
  uint64_t inside;
  uint64_t overlaps;
} Shared;

static void enter(Shared *s) {
  if (s->inside++) s->overlaps++;
}

static void leave(Shared *s) {
  s->counter++;
  s->inside--;
}

static int hammer(Shared *s, uint32_t id) {
  for (int i = 0; i < ROUNDS; i++) {
    locked(&s->lock) {
      enter(s);
      // sometimes the lock is held across a wait.
      if ((i + id) % 97 == 0) cinc_yield();
      leave(s);
    }
  }
  return 0;
}
spawnable(hammer, Shared *, uint32_t);

static void *thread_hammer(void *p) {
  Shared *s = p;
  for (int i = 0; i < ROUNDS; i++) {
    cinc_lock(&s->lock);
    enter(s);
    leave(s);
    cinc_unlock(&s->lock);
  }
  return nullptr;
}

static int first_out(Shared *s) {
  for (;;) locked(&s->lock) {
      return 1;
    }
}

static int body(void *p) {
  Shared *s = p;
  pthread_t threads[THREADS];
  for (int i = 0; i < THREADS; i++) pthread_create(&threads[i], nullptr, thread_hammer, s);
  concurrently {
    for (uint32_t i = 0; i < TASKS; i++) spawn_on(i, hammer(s, i));
  }
  for (int i = 0; i < THREADS; i++) pthread_join(threads[i], nullptr);
  CHECK(s->counter == (uint64_t)(TASKS + THREADS) * ROUNDS);
  CHECK(s->overlaps == 0);
  // a return out of locked unlocks.
  CHECK(first_out(s) == 1);
  CHECK(atomic_load(&s->lock.state) == 0);
  return 0;
}

int main(void) {
  Shared s = {};
  CHECK(CINC_RUN(body, &s, .workers = 4) == 0);
  return check_done("mutex");
}
