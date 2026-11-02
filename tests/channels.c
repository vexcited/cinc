#include "check.h"

#include <pthread.h>

typedef chan(uint64_t) Nums;
typedef struct Pair {
  uint32_t from;
  uint64_t value;
} Pair;
typedef chan(Pair) Pairs;

enum { PRODUCERS = 8, CONSUMERS = 8, EACH = 20'000 };

static _Atomic(uint64_t) received_sum, received_count;

static int produce(Pairs ch, uint32_t id) {
  for (uint64_t i = 0; i < EACH; i++)
    if (chan_send(ch, (Pair){.from = id, .value = i})) return 1;
  return 0;
}
spawnable(produce, Pairs, uint32_t);

static int consume(Pairs ch) {
  uint64_t last[PRODUCERS];
  for (int i = 0; i < PRODUCERS; i++) last[i] = UINT64_MAX;
  int out_of_order = 0;
  range(p, ch) {
    // values from one producer arrive in the order it sent them.
    if (last[p.from] != UINT64_MAX && p.value <= last[p.from]) out_of_order++;
    last[p.from] = p.value;
    atomic_fetch_add(&received_sum, p.value);
    atomic_fetch_add(&received_count, 1);
  }
  return out_of_order;
}
spawnable(consume, Pairs);

static int recv_one(Nums ch) {
  uint64_t v;
  return chan_recv(ch, &v);
}
spawnable(recv_one, Nums);

static int send_one(Nums ch, uint64_t v) { return chan_send(ch, v); }
spawnable(send_one, Nums, uint64_t);

static uint64_t ping(Nums to, Nums from, int rounds) {
  uint64_t v = 0;
  for (int i = 0; i < rounds; i++) {
    chan_send(to, v + 1);
    chan_recv(from, &v);
  }
  return v;
}
spawnable(ping, Nums, Nums, int);

static int pong(Nums from, Nums to) {
  uint64_t v;
  while (!chan_recv(from, &v)) chan_send(to, v + 1);
  return 0;
}
spawnable(pong, Nums, Nums);

static void *plain_producer(void *p) {
  Nums *ch = p;
  for (uint64_t i = 1; i <= 1000; i++) chan_send(*ch, i);
  chan_close(*ch);
  return nullptr;
}

static void *plain_consumer(void *p) {
  Nums *ch = p;
  uint64_t sum = 0;
  range(v, *ch) sum += v;
  return (void *)(uintptr_t)sum;
}

static int body(void *) {
  // buffered, in order, and drained after close.
  Nums ch = make_chan(Nums, 4);
  for (uint64_t i = 0; i < 4; i++) CHECK(chan_send(ch, i) == CINC_OK);
  chan_close(ch);
  CHECK(chan_send(ch, 9) == CINC_ERROR_CLOSED);
  uint64_t v;
  for (uint64_t i = 0; i < 4; i++) CHECK(chan_recv(ch, &v) == CINC_OK && v == i);
  CHECK(chan_recv(ch, &v) == CINC_ERROR_CLOSED);
  chan_free(ch);

  // many producers and consumers on every worker.
  Pairs pairs = make_chan(Pairs, 64);
  concurrently {
    future(consume) consumers[CONSUMERS];
    for (uint32_t i = 0; i < CONSUMERS; i++) consumers[i] = spawn_on(i, consume(pairs));
    concurrently {
      for (uint32_t i = 0; i < PRODUCERS; i++) spawn_on(i + 1, produce(pairs, i));
    }
    chan_close(pairs);
    int disorder = 0;
    for (int i = 0; i < CONSUMERS; i++) disorder += await(consumers[i]);
    CHECK(disorder == 0);
  }
  chan_free(pairs);
  CHECK(received_count == (uint64_t)PRODUCERS * EACH);
  CHECK(received_sum == (uint64_t)PRODUCERS * EACH * (EACH - 1) / 2);

  // the same through a rendezvous channel.
  received_count = received_sum = 0;
  pairs = make_chan(Pairs, 0);
  concurrently {
    for (uint32_t i = 0; i < CONSUMERS; i++) spawn_on(i, consume(pairs));
    concurrently {
      for (uint32_t i = 0; i < PRODUCERS; i++) spawn_on(i + 3, produce(pairs, i));
    }
    chan_close(pairs);
  }
  chan_free(pairs);
  CHECK(received_count == (uint64_t)PRODUCERS * EACH);

  // ping pong across workers on rendezvous channels.
  Nums a = make_chan(Nums, 0), b = make_chan(Nums, 0);
  concurrently {
    spawn_on(1, pong(a, b));
    CHECK(await(spawn_on(2, ping(a, b, 10'000))) == 20'000);
    chan_close(a);
  }

  // close lets blocked senders and receivers go.
  concurrently {
    auto r1 = spawn_on(1, recv_one(a));
    auto r2 = spawn(recv_one(a));
    cinc_sleep(10);
    chan_close(b);
    auto s = spawn_on(2, send_one(b, 1));
    CHECK(await(s) == CINC_ERROR_CLOSED);
    Nums c = make_chan(Nums, 0);
    auto s1 = spawn_on(3, send_one(c, 1));
    cinc_sleep(10);
    chan_close(c);
    CHECK(await(s1) == CINC_ERROR_CLOSED);
    chan_free(c);
    CHECK(await(r1) == CINC_ERROR_CLOSED);
    CHECK(await(r2) == CINC_ERROR_CLOSED);
  }
  chan_free(a);
  chan_free(b);

  // a blocked receive is cancellable.
  a = make_chan(Nums, 1);
  concurrently {
    auto r = spawn_on(1, recv_one(a));
    cinc_sleep(10);
    cancel(r);
    CHECK(cinc_join(r.task) == CINC_ERROR_CANCELLED);
    // and the channel still works for the next one.
    auto r2 = spawn_on(2, recv_one(a));
    CHECK(chan_send(a, 5) == CINC_OK);
    CHECK(await(r2) == CINC_OK);
  }
  chan_free(a);

  // plain threads on both ends.
  a = make_chan(Nums, 0);
  pthread_t producer;
  pthread_create(&producer, nullptr, plain_producer, &a);
  uint64_t sum = 0;
  range(x, a) sum += x;
  CHECK(sum == 500'500);
  pthread_join(producer, nullptr);
  chan_free(a);

  a = make_chan(Nums, 2);
  pthread_t consumer;
  pthread_create(&consumer, nullptr, plain_consumer, &a);
  for (uint64_t i = 1; i <= 1000; i++) chan_send(a, i);
  chan_close(a);
  void *got;
  pthread_join(consumer, &got);
  CHECK((uintptr_t)got == 500'500);
  chan_free(a);
  return 0;
}

int main(void) {
  CHECK(CINC_RUN(body, nullptr, .workers = 4) == 0);
  return check_done("channels");
}
