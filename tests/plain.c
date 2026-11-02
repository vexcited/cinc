#include "check.h"

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

static int order[8], filled;

static int note(int x) {
  order[filled++] = x;
  return x * 10;
}
spawnable(note, int);

static void touch(void *x) { *(int *)x += 1; }

static int echo_once(int listener) {
  int fd;
  if (cinc_accept(listener, &fd)) return 1;
  char buf[64];
  size_t n;
  cinc_error e = cinc_read(fd, buf, sizeof buf, &n);
  if (!e) e = cinc_write_all(fd, buf, n);
  cinc_close(fd);
  return e;
}

static void *server(void *p) { return (void *)(intptr_t)echo_once(*(int *)p); }

typedef chan(int) Ints;

static void *producer(void *p) {
  Ints *ch = p;
  for (int i = 1; i <= 100; i++) chan_send(*ch, i);
  chan_close(*ch);
  return nullptr;
}

typedef struct Counter {
  cinc_mutex lock;
  int n;
} Counter;

static void *count(void *p) {
  Counter *c = p;
  for (int i = 0; i < 10'000; i++) locked(&c->lock) c->n++;
  return nullptr;
}

static int inner(void *) { return cinc_in_task() ? 1 : 2; }

int main(void) {
  CHECK(!cinc_in_task());
  CHECK(cinc_workers() == 1);
  CHECK(cinc_worker() == 0);
  CHECK(cinc_runtime_stats().stacks_mapped == 0);
  CHECK(cinc_arena() != nullptr);

  // sleeps block the thread.
  uint64_t t0 = cinc_now_ms();
  CHECK(cinc_sleep(20) == CINC_OK);
  CHECK(elapsed_ms(t0) >= 20);
  CHECK(cinc_yield() == CINC_OK);

  // spawns run inline, one after the other.
  concurrently {
    auto a = spawn(note(1));
    auto b = spawn(note(2));
    CHECK(filled == 2 && order[0] == 1 && order[1] == 2);
    CHECK(await(a) == 10 && await(b) == 20);
    CHECK(cinc_join(a.task) == CINC_OK);
  }
  auto d = spawn(note(3));
  CHECK(d.task == nullptr && filled == 3);
  cinc_group g = {};
  spawn_in(&g, note(4));
  CHECK(filled == 4 && cinc_group_wait(&g) == CINC_OK);

  // deadlines work the same.
  bool expired = false;
  t0 = cinc_now_ms();
  within(20) {
    CHECK(cinc_sleep(1000) == CINC_ERROR_TIMEOUT);
  } else {
    expired = true;
  }
  CHECK(expired);
  CHECK(elapsed_ms(t0) < 500);

  // blocking runs right here.
  int x = 0;
  CHECK(cinc_blocking(touch, &x) == CINC_OK);
  blocking {
    x++;
  }
  CHECK(x == 2);

  // a pipe read honors the deadline, then gets what was written.
  int p[2];
  CHECK(pipe(p) == 0);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  char c;
  size_t got;
  within(20) {
    CHECK(cinc_read(p[0], &c, 1, &got) == CINC_ERROR_TIMEOUT);
  }
  CHECK(cinc_write_all(p[1], "z", 1) == CINC_OK);
  CHECK(cinc_read(p[0], &c, 1, &got) == CINC_OK && got == 1 && c == 'z');
  cinc_close(p[1]);
  CHECK(cinc_read(p[0], &c, 1, &got) == CINC_ERROR_CLOSED && got == 0);
  cinc_close(p[0]);

  // tcp between two plain threads.
  int listener;
  CHECK(cinc_listen("127.0.0.1:0", false, &listener) == CINC_OK);
  pthread_t srv;
  pthread_create(&srv, nullptr, server, &listener);
  int fd;
  CHECK(cinc_connect("127.0.0.1", cinc_local_port(listener), &fd) == CINC_OK);
  char buf[8] = {};
  CHECK(cinc_write_all(fd, "hello", 5) == CINC_OK);
  CHECK(cinc_read_exact(fd, buf, 5, nullptr) == CINC_OK && !memcmp(buf, "hello", 5));
  cinc_close(fd);
  void *rc;
  pthread_join(srv, &rc);
  CHECK(rc == nullptr);
  cinc_close(listener);

  // channels and mutexes between plain threads.
  Ints ch = make_chan(Ints, 0);
  pthread_t prod;
  pthread_create(&prod, nullptr, producer, &ch);
  int sum = 0;
  range(v, ch) sum += v;
  CHECK(sum == 5050);
  pthread_join(prod, nullptr);
  chan_free(ch);

  Counter counter = {};
  pthread_t threads[4];
  for (int i = 0; i < 4; i++) pthread_create(&threads[i], nullptr, count, &counter);
  for (int i = 0; i < 4; i++) pthread_join(threads[i], nullptr);
  CHECK(counter.n == 40'000);

  // a runtime from a plain thread, back to plain afterwards.
  CHECK(cinc_run(nullptr, inner, nullptr) == 1);
  CHECK(!cinc_in_task());
  return check_done("plain");
}
