#include "check.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct Big {
  alignas(64) double v[8];
} Big;

static int square(int x) {
  cinc_yield();
  return x * x;
}
spawnable(square, int);

static void bump(_Atomic(int) *n) {
  cinc_sleep(1);
  atomic_fetch_add(n, 1);
}
spawnable(bump, _Atomic(int) *);

static Big make_big(double x, int n) {
  Big b = {};
  for (int i = 0; i < 8; i++) b.v[i] = x * (i + n);
  return b;
}
spawnable(make_big, double, int);

static uint32_t where(void) {
  cinc_sleep(1);
  return cinc_worker();
}
spawnable(where);

static long sum8(int a, int b, int c, int d, int e, int f, int g, long h) { return a + b + c + d + e + f + g + h; }
spawnable(sum8, int, int, int, int, int, int, int, long);

static void late(_Atomic(int) *n) {
  cinc_sleep(30);
  atomic_fetch_add(n, 1);
}
spawnable(late, _Atomic(int) *);

static int until_cancelled(_Atomic(int) *cancelled) {
  cinc_error e;
  while ((e = cinc_sleep(1000)) == CINC_OK) {
  }
  if (e == CINC_ERROR_CANCELLED) atomic_fetch_add(cancelled, 1);
  return e;
}
spawnable(until_cancelled, _Atomic(int) *);

static int inner_run(void *arg) { return *(int *)arg + 1; }

static _Atomic(int) detached_done;

static int body(void *) {
  CHECK(cinc_in_task());
  CHECK(cinc_workers() == 4);

  concurrently {
    auto a = spawn(square(7));
    auto b = spawn(make_big(1.5, 2));
    CHECK(await(a) == 49);
    Big big = await(b);
    CHECK(big.v[3] == 1.5 * 5);
    CHECK((uintptr_t)b.result % 64 == 0);
    CHECK(cinc_join(a.task) == CINC_OK);
    CHECK(await(spawn(sum8(1, 2, 3, 4, 5, 6, 7, 8))) == 36);
  }

  // void functions await to nothing, and unawaited tasks are waited for by the block.
  _Atomic(int) n = 0;
  concurrently {
    auto v = spawn(bump(&n));
    await(v);
    CHECK(n == 1);
    for (int i = 0; i < 100; i++) spawn(bump(&n));
  }
  CHECK(n == 101);

  // many tasks, each on the worker it was sent to.
  concurrently {
    future(where) at[64];
    for (uint32_t i = 0; i < 64; i++) at[i] = spawn_on(i, where());
    for (uint32_t i = 0; i < 64; i++) CHECK(await(at[i]) == i % 4);
    future(square) sq[1000];
    for (int i = 0; i < 1000; i++) sq[i] = spawn_on((uint32_t)i, square(i));
    long total = 0;
    for (int i = 0; i < 1000; i++) total += await(sq[i]);
    CHECK(total == 332'833'500);
  }

  // outside a block a spawn is detached: no task to wait for, and cinc_run waits for it instead.
  auto d = spawn(late(&detached_done));
  CHECK(d.task == nullptr);
  CHECK(cinc_join(d.task) == CINC_ERROR_CLOSED);
  // awaiting it is a bug, caught at once.
  pid_t pid = fork();
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 2);
    await(d);
    _exit(0);
  }
  int status;
  waitpid(pid, &status, 0);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

  // groups.
  cinc_group g = {};
  _Atomic(int) m = 0;
  for (uint32_t i = 0; i < 200; i++) spawn_in_on(&g, i, bump(&m));
  CHECK(cinc_group_wait(&g) == CINC_OK);
  CHECK(m == 200);
  CHECK(cinc_group_live(&g) == 0);

  cinc_group stuck = {};
  _Atomic(int) cancelled = 0;
  for (uint32_t i = 0; i < 40; i++) spawn_in_on(&stuck, i, until_cancelled(&cancelled));
  within(30) {
    CHECK(cinc_group_wait(&stuck) == CINC_ERROR_TIMEOUT);
  }
  CHECK(cinc_group_live(&stuck) == 40);
  cinc_group_cancel(&stuck);
  CHECK(cinc_group_wait(&stuck) == CINC_OK);
  CHECK(cancelled == 40);
  // spawned into a cancelled group, it starts cancelled.
  spawn_in(&stuck, until_cancelled(&cancelled));
  CHECK(cinc_group_wait(&stuck) == CINC_OK);
  CHECK(cancelled == 41);

  // a runtime inside a task runs inline.
  int x = 41;
  CHECK(cinc_run(nullptr, inner_run, &x) == 42);
  return 7;
}

int main(void) {
  CHECK(cinc_run(&(cinc_options){.workers = 4}, body, nullptr) == 7);
  CHECK(detached_done == 1);
  // and again, a runtime is not a one off.
  detached_done = 0;
  CHECK(CINC_RUN(body, nullptr, .workers = 4, .stack_size = 128 << 10) == 7);
  CHECK(detached_done == 1);
  return check_done("tasks");
}
