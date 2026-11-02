#include "check.h"

#include <pthread.h>

typedef chan(uint64_t) Nums;
typedef struct Point {
  int32_t x, y;
} Point;
typedef chan(Point) Points;

enum { PRODUCERS = 4, EACH = 20'000 };

static _Atomic(uint64_t) got_sum, got_count;

static int produce(Nums ch, uint64_t from) {
  for (uint64_t i = 0; i < EACH; i++)
    if (chan_send(ch, from + i)) return 1;
  return 0;
}
spawnable(produce, Nums, uint64_t);

// takes from both channels until both are closed, turning off a case once its channel is done.
static int drain_both(Nums a, Nums b) {
  for (;;) {
    uint64_t v;
    cinc_error e;
    int i = chan_select(&e, on_recv(a, &v), on_recv(b, &v));
    if (i < 0) return 1;
    if (e == CINC_ERROR_CLOSED) {
      if (i == 0) a = (Nums){};
      else b = (Nums){};
      if (!a.raw && !b.raw) return 0;
      continue;
    }
    atomic_fetch_add(&got_sum, v);
    atomic_fetch_add(&got_count, 1);
  }
}
spawnable(drain_both, Nums, Nums);

static int select_recv(Nums a, Nums b) {
  uint64_t v = 0;
  cinc_error e;
  int i = chan_select(&e, on_recv(a, &v), on_recv(b, &v));
  return i < 0 ? -(int)e : i * 100 + (int)v;
}
spawnable(select_recv, Nums, Nums);

// sends to whichever side takes it first, through selects on both ends.
static int select_send(Nums a, Nums b, uint64_t value, int rounds) {
  for (int r = 0; r < rounds; r++) {
    cinc_error e;
    if (chan_select(&e, on_send(a, value), on_send(b, value)) < 0 || e) return 1;
  }
  return 0;
}
spawnable(select_send, Nums, Nums, uint64_t, int);

static uint64_t select_take(Nums a, Nums b, int rounds) {
  uint64_t sum = 0;
  for (int r = 0; r < rounds; r++) {
    uint64_t v = 0;
    cinc_error e;
    if (chan_select(&e, on_recv(a, &v), on_recv(b, &v)) < 0 || e) return 0;
    sum += v;
  }
  return sum;
}
spawnable(select_take, Nums, Nums, int);

static void *plain_select(void *p) {
  Nums *ch = p;
  uint64_t v = 0;
  cinc_error e;
  int i = chan_select(&e, on_recv(ch[0], &v), on_recv(ch[1], &v));
  return (void *)(uintptr_t)(i == 1 && !e ? v : 0);
}

static int body(void *) {
  Nums a = make_chan(Nums, 1), b = make_chan(Nums, 1), none = {};
  uint64_t v = 0;
  cinc_error e = CINC_OK;

  // nothing ready.
  CHECK(chan_try_select(&e, on_recv(a, &v), on_recv(b, &v)) == -1 && e == CINC_ERROR_TIMEOUT);

  // a ready case runs right away, with the value typed and a compound literal's commas passing through.
  chan_send(b, 7);
  CHECK(chan_try_select(&e, on_recv(a, &v), on_recv(b, &v)) == 1 && e == CINC_OK && v == 7);
  CHECK(chan_select(&e, on_send(a, 3), on_recv(b, &v)) == 0 && e == CINC_OK);
  CHECK(chan_recv(a, &v) == CINC_OK && v == 3);
  Points pts = make_chan(Points, 1);
  Point pt = {};
  CHECK(chan_select(&e, on_send(pts, (Point){1, 2})) == 0 && e == CINC_OK);
  CHECK(chan_select(&e, on_recv(pts, &pt)) == 0 && pt.x == 1 && pt.y == 2);
  chan_free(pts);

  // a null channel never runs, the case is off.
  CHECK(chan_try_select(&e, on_recv(none, &v)) == -1);
  chan_send(a, 4);
  CHECK(chan_select(&e, on_recv(none, &v), on_recv(a, &v)) == 1 && v == 4);

  // when both are ready either may run.
  int hits[2] = {};
  for (int r = 0; r < 200; r++) {
    chan_send(a, 1);
    chan_send(b, 2);
    int i = chan_select(&e, on_recv(a, &v), on_recv(b, &v));
    CHECK(i == 0 || i == 1);
    if (i == 0 || i == 1) hits[i]++;
    chan_recv(i == 0 ? b : a, &v);
  }
  CHECK(hits[0] > 20 && hits[1] > 20);

  // a waiting select is woken by a send on another worker, through either case.
  concurrently {
    auto s = spawn_on(1, select_recv(a, b));
    cinc_sleep(10);
    chan_send(b, 9);
    CHECK(await(s) == 109);
    s = spawn_on(2, select_recv(a, b));
    cinc_sleep(10);
    chan_send(a, 5);
    CHECK(await(s) == 5);
  }
  // the other case left its channel's queue: the value stays buffered.
  chan_send(b, 8);
  CHECK(chan_recv(b, &v) == CINC_OK && v == 8);

  // a deadline or a cancellation ends the wait, and the channels work on.
  within(20) {
    CHECK(chan_select(&e, on_recv(a, &v), on_recv(b, &v)) == -1 && e == CINC_ERROR_TIMEOUT);
  }
  concurrently {
    auto s = spawn_on(3, select_recv(a, b));
    cinc_sleep(10);
    cancel(s);
    CHECK(cinc_join(s.task) == CINC_ERROR_CANCELLED);
  }
  chan_send(a, 6);
  CHECK(chan_recv(a, &v) == CINC_OK && v == 6);

  // a closed channel's case runs with CINC_ERROR_CLOSED, sends included.
  Nums c = make_chan(Nums, 0);
  concurrently {
    auto s = spawn_on(1, select_recv(a, c));
    cinc_sleep(10);
    chan_close(c);
    CHECK(await(s) == 100);
  }
  CHECK(chan_select(&e, on_send(c, 1)) == 0 && e == CINC_ERROR_CLOSED);
  chan_free(c);

  // selects on both ends of rendezvous channels, across workers.
  Nums r1 = make_chan(Nums, 0), r2 = make_chan(Nums, 0);
  concurrently {
    auto taker = spawn_on(1, select_take(r1, r2, 10'000));
    spawn_on(2, select_send(r1, r2, 1, 5'000));
    spawn_on(3, select_send(r2, r1, 2, 5'000));
    CHECK(await(taker) == 15'000);
  }
  chan_free(r1);
  chan_free(r2);

  // many producers into two channels, many selecting consumers.
  Nums x = make_chan(Nums, 16), y = make_chan(Nums, 0);
  concurrently {
    for (uint32_t i = 0; i < 4; i++) spawn_on(i, drain_both(x, y));
    concurrently {
      for (uint32_t i = 0; i < PRODUCERS; i++) spawn_on(i + 1, produce(i % 2 ? x : y, 0));
    }
    chan_close(x);
    chan_close(y);
  }
  CHECK(got_count == (uint64_t)PRODUCERS * EACH);
  CHECK(got_sum == (uint64_t)PRODUCERS * EACH * (EACH - 1) / 2);
  chan_free(x);
  chan_free(y);

  // a plain thread selects too.
  Nums pair[2] = {make_chan(Nums, 0), make_chan(Nums, 0)};
  pthread_t th;
  pthread_create(&th, nullptr, plain_select, pair);
  cinc_sleep(10);
  chan_send(pair[1], 42);
  void *res;
  pthread_join(th, &res);
  CHECK((uintptr_t)res == 42);
  chan_free(pair[0]);
  chan_free(pair[1]);

  chan_free(a);
  chan_free(b);
  return 0;
}

int main(void) {
  CHECK(CINC_RUN(body, nullptr, .workers = 4) == 0);
  return check_done("select");
}
