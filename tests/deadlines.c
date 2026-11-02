#include "check.h"

static _Atomic(int) child_result = -1;

static int sleeper(uint64_t ms) {
  cinc_error e = cinc_sleep(ms);
  child_result = e;
  return e;
}
spawnable(sleeper, uint64_t);

static int slow_value(void) {
  cinc_sleep(100);
  return 42;
}
spawnable(slow_value);

static int body(void *) {
  CHECK(cinc_remaining_ms() == UINT64_MAX);

  bool ran_else = false;
  uint64_t t0 = cinc_now_ms();
  within(50) {
    CHECK(cinc_remaining_ms() <= 50);
    CHECK(cinc_sleep(1000) == CINC_ERROR_TIMEOUT);
  } else {
    ran_else = true;
    // the else branch is back under the outer deadline, so it can still wait.
    CHECK(cinc_sleep(1) == CINC_OK);
  }
  CHECK(ran_else);
  uint64_t took = elapsed_ms(t0);
  CHECK(took >= 50 && took < 250);
  CHECK(cinc_remaining_ms() == UINT64_MAX);

  // an inner deadline expires, the outer one does not.
  bool inner = false, outer = false;
  within(1000) {
    within(20) {
      CHECK(cinc_sleep(500) == CINC_ERROR_TIMEOUT);
    } else {
      inner = true;
    }
    CHECK(cinc_sleep(5) == CINC_OK);
  } else {
    outer = true;
  }
  CHECK(inner && !outer);

  // nested deadlines never extend an outer one: both else branches run.
  inner = outer = false;
  t0 = cinc_now_ms();
  within(30) {
    within(1000) {
      CHECK(cinc_remaining_ms() <= 30);
      CHECK(cinc_sleep(500) == CINC_ERROR_TIMEOUT);
    } else {
      inner = true;
    }
    CHECK(cinc_sleep(1) == CINC_ERROR_TIMEOUT);
  } else {
    outer = true;
  }
  CHECK(inner && outer);
  CHECK(elapsed_ms(t0) < 300);

  // a block that finishes in time skips its else.
  ran_else = false;
  within(200) {
    CHECK(cinc_sleep(1) == CINC_OK);
  } else {
    ran_else = true;
  }
  CHECK(!ran_else);

  // children take the deadline of their spawner.
  within(40) {
    concurrently {
      spawn_on(1, sleeper(5000));
    }
  }
  CHECK(child_result == CINC_ERROR_TIMEOUT);

  // an await times out on its own, zeroed, and the block still waits for the task.
  int v = -1;
  t0 = cinc_now_ms();
  concurrently {
    auto a = spawn_on(2, slow_value());
    within(20) {
      v = await(a);
      CHECK(cinc_join(a.task) == CINC_ERROR_TIMEOUT);
    }
    CHECK(await(a) == 42);
  }
  CHECK(v == 0);
  CHECK(elapsed_ms(t0) >= 100);

  // loops that never wait see it through cinc_check.
  ran_else = false;
  within(30) {
    while (!cinc_check()) {
    }
  } else {
    ran_else = true;
  }
  CHECK(ran_else);

  // waits on channels honor it too.
  typedef chan(int) Ints;
  Ints ch = make_chan(Ints, 0);
  int got;
  within(20) {
    CHECK(chan_recv(ch, &got) == CINC_ERROR_TIMEOUT);
    CHECK(chan_send(ch, 1) == CINC_ERROR_TIMEOUT);
  }
  chan_free(ch);

  // a deadline of zero has expired already.
  within(0) {
    CHECK(cinc_yield() == CINC_ERROR_TIMEOUT);
  }
  return 0;
}

int main(void) {
  CHECK(CINC_RUN(body, nullptr, .workers = 3) == 0);
  return check_done("deadlines");
}
