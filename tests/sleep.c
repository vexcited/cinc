#include "check.h"

#include <stdlib.h>
#include <time.h>

// loaded machines and docker's virtual machine wake late now and then, never early.
enum { SLACK_MS = 25 };

static _Atomic(uint64_t) worst_late, early;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1'000'000'000u + (uint64_t)ts.tv_nsec;
}

static int sleeper(uint64_t ms) {
  uint64_t t0 = now_ns();
  cinc_sleep(ms);
  uint64_t took = now_ns() - t0, want = ms * 1'000'000u;
  if (took < want) early++;
  uint64_t late = took > want ? took - want : 0, seen = worst_late;
  while (late > seen && !atomic_compare_exchange_weak(&worst_late, &seen, late)) {
  }
  return 0;
}
spawnable(sleeper, uint64_t);

static int body(void *) {
  static const uint64_t lengths[] = {1, 2, 5, 10, 25, 50, 100};
  for (size_t i = 0; i < sizeof lengths / sizeof *lengths; i++) sleeper(lengths[i]);
  CHECK(early == 0);
  CHECK(worst_late < SLACK_MS * 1'000'000u);

  // a thousand sleepers on every worker at once.
  worst_late = 0;
  concurrently {
    for (uint32_t i = 0; i < 1000; i++) spawn_on(i, sleeper(1 + (uint64_t)rand() % 100));
  }
  CHECK(early == 0);
  CHECK(worst_late < SLACK_MS * 1'000'000u);
  printf("worst wake up %.3f ms late\n", (double)worst_late / 1e6);

  // zero returns right away.
  uint64_t t0 = cinc_now_ms();
  CHECK(cinc_sleep(0) == CINC_OK);
  CHECK(elapsed_ms(t0) < 5);
  return 0;
}

int main(void) {
  CHECK(CINC_RUN(body, nullptr, .workers = 4) == 0);
  // and on a plain thread.
  worst_late = 0;
  sleeper(20);
  CHECK(early == 0);
  CHECK(worst_late < SLACK_MS * 1'000'000u);
  return check_done("sleep");
}
