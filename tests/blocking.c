#include "check.h"

#include <pthread.h>
#include <unistd.h>

static _Atomic(uint64_t) ticks;

// glibc declares pthread_self const, so a call inlined into a blocking block could be merged with one outside.
[[gnu::noinline]] static pthread_t thread_id(void) {
  pthread_t t = pthread_self();
  __asm__ volatile("" : "+r"(t));
  return t;
}

static int ticker(void) {
  while (!cinc_sleep(1)) ticks++;
  return 0;
}
spawnable(ticker);

static void record_thread(void *p) {
  *(pthread_t *)p = thread_id();
  CHECK(!cinc_in_task());
}

static uint64_t nap(uint64_t ms) {
  blocking {
    usleep((useconds_t)ms * 1000);
  }
  return cinc_worker();
}
spawnable(nap, uint64_t);

static int leave_from_inside(void) {
  blocking {
    return 5;
  }
  return 0;
}

static int body(void *) {
  pthread_t worker = thread_id(), inside = worker;
  uint32_t me = cinc_worker();
  cilo_arena *arena = cinc_arena(), *seen = nullptr;
  concurrently {
    auto t = spawn(ticker());
    cinc_sleep(5);
    uint64_t before = ticks;
    blocking {
      inside = thread_id();
      seen = cinc_arena();
      CHECK(!cinc_in_task());
      usleep(100'000);
    }
    // the worker went on with the ticker meanwhile, which a block run in place would have stopped.
    CHECK(ticks - before > 5);
    cancel(t);
  }
  CHECK(!pthread_equal(inside, worker));
  CHECK(pthread_equal(thread_id(), worker));
  CHECK(cinc_worker() == me);
  CHECK(seen == arena);

  // the function form runs on a pool thread too.
  pthread_t ran = worker;
  CHECK(cinc_blocking(record_thread, &ran) == CINC_OK);
  CHECK(!pthread_equal(ran, worker));

  // a return from inside the block comes back first.
  CHECK(leave_from_inside() == 5);
  CHECK(pthread_equal(thread_id(), worker));

  // the deadline goes along, and waits on the pool block the pool thread.
  uint64_t left = 0;
  uint64_t t0 = cinc_now_ms();
  within(300) {
    blocking {
      left = cinc_remaining_ms();
      CHECK(cinc_sleep(1000) == CINC_ERROR_TIMEOUT);
    }
  }
  CHECK(left > 0 && left <= 300);
  CHECK(elapsed_ms(t0) < 900);

  // already expired, the function is skipped.
  within(0) {
    CHECK(cinc_blocking(record_thread, &ran) == CINC_ERROR_TIMEOUT);
  }

  // more blocks than pool threads queue up, and each task comes back to its worker.
  t0 = cinc_now_ms();
  concurrently {
    future(nap) naps[32];
    for (uint32_t i = 0; i < 32; i++) naps[i] = spawn_on(i, nap(20));
    for (uint32_t i = 0; i < 32; i++) CHECK(await(naps[i]) == i % 4);
  }
  uint64_t took = elapsed_ms(t0);
  CHECK(took >= 80);
  CHECK(cinc_runtime_stats().blocking_threads <= 8);
  return 0;
}

int main(void) {
  CHECK(CINC_RUN(body, nullptr, .workers = 4, .blocking_threads = 8) == 0);
  return check_done("blocking");
}
