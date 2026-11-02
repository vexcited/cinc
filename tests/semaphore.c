#include "check.h"

#include <pthread.h>

enum { PERMITS = 3, TASKS = 64, ROUNDS = 200 };

static cinc_sem seats = {.permits = PERMITS};
static _Atomic(int) inside, most;

static void occupy(void) {
  int now = atomic_fetch_add(&inside, 1) + 1;
  for (int m = atomic_load(&most); now > m && !atomic_compare_exchange_weak(&most, &m, now);) {
  }
}

static int hammer(uint32_t id) {
  for (int i = 0; i < ROUNDS; i++) {
    if (cinc_sem_acquire(&seats)) return 1;
    occupy();
    if ((i + id) % 7 == 0) cinc_yield();
    atomic_fetch_sub(&inside, 1);
    cinc_sem_release(&seats);
  }
  return 0;
}
spawnable(hammer, uint32_t);

static void *thread_hammer(void *) {
  for (int i = 0; i < ROUNDS; i++) {
    acquired(&seats) {
      occupy();
      atomic_fetch_sub(&inside, 1);
    }
  }
  return nullptr;
}

static int take_one(cinc_sem *s) { return cinc_sem_acquire(s); }
spawnable(take_one, cinc_sem *);

static int order[3], next_slot;

static int take_in_order(cinc_sem *s, int id) {
  if (cinc_sem_acquire(s)) return 1;
  order[next_slot++] = id;
  return 0;
}
spawnable(take_in_order, cinc_sem *, int);

static int leave_early(cinc_sem *s) {
  acquired(s) return (int)cinc_sem_available(s) + 1;
  return -1;
}

static int body(void *) {
  // never more holders than permits, across workers and plain threads.
  pthread_t threads[2];
  for (int i = 0; i < 2; i++) pthread_create(&threads[i], nullptr, thread_hammer, nullptr);
  concurrently {
    for (uint32_t i = 0; i < TASKS; i++) spawn_on(i, hammer(i));
  }
  for (int i = 0; i < 2; i++) pthread_join(threads[i], nullptr);
  CHECK(atomic_load(&most) <= PERMITS && atomic_load(&most) >= 1);
  CHECK(cinc_sem_available(&seats) == PERMITS);

  cinc_sem one = {.permits = 1};
  CHECK(cinc_sem_try_acquire(&one));
  CHECK(!cinc_sem_try_acquire(&one));

  // a wait honors the deadline, and gives nothing away.
  within(20) {
    CHECK(cinc_sem_acquire(&one) == CINC_ERROR_TIMEOUT);
  }
  // and the else branch of a block that could not acquire.
  bool held = false, refused = false;
  within(10) {
    acquired(&one) held = true;
    else refused = cinc_check() == CINC_ERROR_TIMEOUT;
  }
  CHECK(!held && refused);

  // a cancelled waiter leaves the queue, the next one gets the permit.
  concurrently {
    auto w = spawn_on(1, take_one(&one));
    cinc_sleep(10);
    cancel(w);
    CHECK(cinc_join(w.task) == CINC_ERROR_CANCELLED);
    auto w2 = spawn_on(2, take_one(&one));
    cinc_sleep(10);
    cinc_sem_release(&one);
    CHECK(await(w2) == CINC_OK);
  }
  CHECK(cinc_sem_available(&one) == 0);

  // a free permit is taken even past the deadline, so code that only gives back never fails.
  cinc_sem_release(&one);
  within(0) {
    CHECK(cinc_sem_acquire(&one) == CINC_OK);
    cinc_sem_release(&one);
  }
  CHECK(cinc_sem_available(&one) == 1);

  // a block releases on return.
  CHECK(leave_early(&one) == 1);
  CHECK(cinc_sem_available(&one) == 1);

  // waiters are served in the order they came.
  cinc_sem none = {};
  concurrently {
    for (int i = 0; i < 3; i++) {
      spawn(take_in_order(&none, i));
      cinc_yield();
    }
    for (int i = 0; i < 3; i++) cinc_sem_release(&none);
  }
  CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2);

  // taking permits back leaves the holders alone, and releases pay the difference before anyone gets in.
  cinc_sem limit = {.permits = 2};
  CHECK(cinc_sem_try_acquire(&limit) && cinc_sem_try_acquire(&limit));
  cinc_sem_adjust(&limit, -1);
  CHECK(cinc_sem_available(&limit) == 0);
  cinc_sem_release(&limit);
  CHECK(cinc_sem_available(&limit) == 0 && !cinc_sem_try_acquire(&limit));
  concurrently {
    auto w = spawn_on(1, take_one(&limit));
    cinc_sleep(10);
    within(0) {
      CHECK(cinc_join(w.task) == CINC_ERROR_TIMEOUT);
    }
    cinc_sem_release(&limit);
    CHECK(await(w) == CINC_OK);
  }
  CHECK(cinc_sem_available(&limit) == 0);
  cinc_sem_release(&limit);
  CHECK(cinc_sem_available(&limit) == 1);

  // adding permits wakes as many waiters, in order.
  cinc_sem grow = {};
  next_slot = 0;
  concurrently {
    for (int i = 0; i < 3; i++) {
      spawn(take_in_order(&grow, i));
      cinc_yield();
    }
    cinc_sem_adjust(&grow, 2);
    cinc_sleep(10);
    CHECK(next_slot == 2 && order[0] == 0 && order[1] == 1);
    cinc_sem_adjust(&grow, 1);
  }
  CHECK(next_slot == 3 && order[2] == 2 && cinc_sem_available(&grow) == 0);
  cinc_sem_adjust(&grow, 0);
  CHECK(cinc_sem_available(&grow) == 0);
  return 0;
}

int main(void) {
  CHECK(CINC_RUN(body, nullptr, .workers = 4) == 0);
  return check_done("semaphore");
}
