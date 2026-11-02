#include <cinc/dsl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1'000'000'000u + (uint64_t)ts.tv_nsec;
}

static void report(const char *name, uint64_t ns, uint64_t ops) {
  printf("%-40s %8.1f ns\n", name, (double)ns / (double)ops);
}

static int noop(int x) { return x + 1; }
spawnable(noop, int);

static void yielder(uint64_t n) {
  for (uint64_t i = 0; i < n; i++) cinc_yield();
}
spawnable(yielder, uint64_t);

typedef chan(uint64_t) Nums;

static uint64_t ping(Nums to, Nums from, uint64_t n) {
  uint64_t v = 0;
  for (uint64_t i = 0; i < n; i++) {
    chan_send(to, v);
    chan_recv(from, &v);
  }
  return v;
}
spawnable(ping, Nums, Nums, uint64_t);

static void pong(Nums from, Nums to) {
  uint64_t v;
  while (!chan_recv(from, &v)) chan_send(to, v + 1);
}
spawnable(pong, Nums, Nums);

static void producer(Nums ch, uint64_t n) {
  for (uint64_t i = 0; i < n; i++) chan_send(ch, i);
}
spawnable(producer, Nums, uint64_t);

static uint64_t ping_pong(uint32_t ping_on, uint32_t pong_on, uint64_t n) {
  Nums a = make_chan(Nums, 0), b = make_chan(Nums, 0);
  uint64_t t0 = now_ns();
  concurrently {
    spawn_on(pong_on, pong(a, b));
    await(spawn_on(ping_on, ping(a, b, n)));
    chan_close(a);
  }
  uint64_t took = now_ns() - t0;
  chan_free(a);
  chan_free(b);
  return took;
}

static int bench(void *) {
  enum : uint64_t { N = 1'000'000 };

  // warm the stacks and the frame arenas.
  for (int i = 0; i < 1000; i++) concurrently {
      (void)await(spawn(noop(i)));
    }

  uint64_t t0 = now_ns();
  for (uint64_t i = 0; i < N; i++) concurrently {
      (void)await(spawn(noop((int)i)));
    }
  report("spawn + await, one at a time", now_ns() - t0, N);

  t0 = now_ns();
  for (uint64_t round = 0; round < N / 1000; round++) concurrently {
      for (int i = 0; i < 1000; i++) spawn(noop(i));
    }
  report("spawn + join, 1000 per block", now_ns() - t0, N);

  t0 = now_ns();
  for (uint64_t round = 0; round < N / 1000; round++) concurrently {
      for (uint32_t i = 0; i < 1000; i++) spawn_on(i % cinc_workers(), noop((int)i));
    }
  report("spawn + join, 1000 over every worker", now_ns() - t0, N);

  // two tasks yielding to each other: every yield is two switches, into the scheduler and out.
  t0 = now_ns();
  concurrently {
    spawn(yielder(N));
    spawn(yielder(N));
  }
  report("yield, two tasks on one worker", now_ns() - t0, 2 * N);
  report("context switch", now_ns() - t0, 4 * N);

  report("channel ping pong, one worker", ping_pong(0, 0, N), N);
  report("channel ping pong, two workers", ping_pong(1, 2, N / 10), N / 10);

  Nums ch = make_chan(Nums, 1024);
  t0 = now_ns();
  concurrently {
    spawn_on(1, producer(ch, N));
    uint64_t v;
    for (uint64_t i = 0; i < N; i++) chan_recv(ch, &v);
  }
  report("buffered channel, two workers", now_ns() - t0, N);
  chan_free(ch);

  cinc_mutex m = {};
  t0 = now_ns();
  for (uint64_t i = 0; i < N; i++) locked(&m) {
    }
  report("mutex lock + unlock, uncontended", now_ns() - t0, N);

  cinc_stats s = cinc_runtime_stats();
  printf("%llu stacks mapped\n", (unsigned long long)s.stacks_mapped);
  return 0;
}

int main(void) { return CINC_RUN(bench, nullptr, .workers = 4); }
