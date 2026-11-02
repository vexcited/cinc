#include "check.h"

#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef chan(int) Gate;

static _Atomic(uint32_t) started;

// waits until the gate closes, so every task of a round is alive at once.
static int hold(Gate gate) {
  started++;
  int v;
  return chan_recv(gate, &v) == CINC_ERROR_CLOSED ? 0 : 1;
}
spawnable(hold, Gate);

static int round_of(uint32_t n) {
  Gate gate = make_chan(Gate, 0);
  int bad = 0;
  concurrently {
    future(hold) tasks[1000];
    started = 0;
    for (uint32_t i = 0; i < n; i++) tasks[i] = spawn_on(i, hold(gate));
    while (started < n) cinc_sleep(1);
    chan_close(gate);
    for (uint32_t i = 0; i < n; i++) bad += await(tasks[i]);
  }
  chan_free(gate);
  return bad;
}

static int nothing(void) { return 1; }
spawnable(nothing);

// uses most of a 64 KiB stack.
static int deep(int depth) {
  volatile char pad[1024];
  memset((char *)pad, depth, sizeof pad);
  return depth ? deep(depth - 1) + pad[0] - depth : 0;
}
spawnable(deep, int);

static int forever(int depth) {
  volatile char pad[4096];
  pad[0] = (char)depth;
  if (depth < 0) return 0;
  return forever(depth + 1) + pad[0];
}
spawnable(forever, int);

static int body(void *) {
  // warm up: 500 tasks alive at once on each of two workers.
  CHECK(round_of(1000) == 0);
  cinc_stats warm = cinc_runtime_stats();
  CHECK(warm.stacks_mapped == 1001);
  // every stack is back in a pool but the one running this.
  CHECK(warm.stacks_idle + 1 == warm.stacks_mapped);

  // the same load again maps nothing more.
  for (int round = 0; round < 5; round++) CHECK(round_of(1000) == 0);
  CHECK(cinc_runtime_stats().stacks_mapped == warm.stacks_mapped);

  // and a hundred thousand short ones, one after the other, reuse the same few stacks.
  int ok = 0;
  for (int i = 0; i < 100'000; i++) concurrently {
      ok += await(spawn(nothing()));
    }
  CHECK(ok == 100'000);
  CHECK(cinc_runtime_stats().stacks_mapped == warm.stacks_mapped);

  concurrently {
    CHECK(await(spawn(deep(40))) == 0);
  }
  return 0;
}

static int overflow(void *) {
  concurrently {
    spawn(forever(0));
  }
  return 0;
}

int main(void) {
  // first, while there is a single thread to fork: running off the end of a stack hits its guard page.
  pid_t pid = fork();
  if (pid == 0) {
    alarm(10);
    _exit(CINC_RUN(overflow, nullptr, .workers = 1));
  }
  int status;
  waitpid(pid, &status, 0);
  CHECK(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0));
  // asan reports the overflow itself and aborts its own way.
  if (WIFSIGNALED(status) && !CILO_ASAN) CHECK(WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);

  CHECK(CINC_RUN(body, nullptr, .workers = 2) == 0);
  return check_done("stacks");
}
