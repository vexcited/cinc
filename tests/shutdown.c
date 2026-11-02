#include "check.h"

#include <signal.h>
#include <unistd.h>

static int waiter(void) {
  int signo = 0;
  cinc_error e = cinc_wait_shutdown(&signo);
  return e ? -1 : signo;
}
spawnable(waiter);

static int body(void *) {
  concurrently {
    auto a = spawn(waiter());
    auto b = spawn_on(1, waiter());
    cinc_sleep(20);
    kill(getpid(), SIGTERM);
    CHECK(await(a) == SIGTERM);
    CHECK(await(b) == SIGTERM);
  }
  // once it came, every later wait returns at once.
  within(100) {
    concurrently {
      CHECK(await(spawn_on(2, waiter())) == SIGTERM);
    }
  }
  return 0;
}

int main(void) {
  CHECK(CINC_RUN(body, nullptr, .workers = 3) == 0);
  // on a plain thread as well.
  int signo = 0;
  CHECK(cinc_wait_shutdown(&signo) == CINC_OK && signo == SIGTERM);
  return check_done("shutdown");
}
