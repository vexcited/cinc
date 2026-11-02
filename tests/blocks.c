#include "check.h"

static _Atomic(int) cancelled, finished;

static int sleeper(uint64_t ms) {
  cinc_error e = cinc_sleep(ms);
  atomic_fetch_add(e == CINC_ERROR_CANCELLED ? &cancelled : &finished, 1);
  return e;
}
spawnable(sleeper, uint64_t);

// spawns its own children, on every worker, in a block of its own.
static int parent(uint32_t n) {
  concurrently {
    for (uint32_t i = 0; i < n; i++) spawn_on(i, sleeper(10'000));
  }
  return cinc_check();
}
spawnable(parent, uint32_t);

// a cpu loop that only looks at cinc_check.
static int spinner(void) {
  while (!cinc_check()) {
  }
  atomic_fetch_add(&cancelled, 1);
  return 0;
}
spawnable(spinner);

static int leave_by_return(void) {
  concurrently {
    spawn(sleeper(10'000));
    spawn_on(1, parent(3));
    cinc_sleep(5);
    return 7;
  }
  return 0;
}

static void reset(void) {
  cancelled = 0;
  finished = 0;
}

static int body(void *) {
  uint64_t t0 = cinc_now_ms();
  // break out of a block with grandchildren on every worker.
  concurrently {
    for (uint32_t i = 0; i < 4; i++) spawn_on(i, parent(4));
    cinc_sleep(20);
    break;
  }
  CHECK(cancelled == 16);
  CHECK(elapsed_ms(t0) < 2000);

  reset();
  CHECK(leave_by_return() == 7);
  CHECK(cancelled == 4);

  reset();
  concurrently {
    spawn(sleeper(10'000));
    spawn_on(2, spinner());
    cinc_sleep(5);
    goto out;
  }
out:
  CHECK(cancelled == 2);

  // a normal end waits, cancels nothing.
  reset();
  t0 = cinc_now_ms();
  concurrently {
    for (uint32_t i = 0; i < 8; i++) spawn_on(i, sleeper(30));
  }
  CHECK(finished == 8);
  CHECK(elapsed_ms(t0) >= 30);

  // cancelling a future reaches into the blocks of the task.
  reset();
  concurrently {
    auto a = spawn_on(3, parent(5));
    cinc_sleep(10);
    cancel(a);
    CHECK(cinc_join(a.task) == CINC_ERROR_CANCELLED);
    CHECK(cancelled == 5);
    auto b = spawn(sleeper(1));
    CHECK(await(b) == CINC_OK);
    cancel(b);
    CHECK(cinc_join(b.task) == CINC_OK);
  }

  // nested blocks, the inner one left early, the outer one still waiting for its own.
  reset();
  concurrently {
    spawn(sleeper(20));
    for (int round = 0; round < 3; round++) {
      concurrently {
        spawn_on((uint32_t)round, sleeper(10'000));
        concurrently {
          spawn(sleeper(10'000));
          break;
        }
        break;
      }
    }
  }
  CHECK(cancelled == 6);
  CHECK(finished == 1);

  // a cancelled owner waiting at the end of its block passes it on.
  reset();
  concurrently {
    auto p = spawn_on(1, parent(8));
    cinc_sleep(5);
    cancel(p);
  }
  CHECK(cancelled == 8);
  return 0;
}

int main(void) {
  CHECK(CINC_RUN(body, nullptr, .workers = 4) == 0);
  return check_done("blocks");
}
