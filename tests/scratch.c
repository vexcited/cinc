#include "check.h"

#include <cilo/dsl.h>
#include <string.h>

// every task fills its scratch arena with its own byte, waits while others do the same on this worker, then
// checks nothing was overwritten or rewound under it.
static int fill(int id) {
  int bad = 0;
  cilo_arena *mine = nullptr;
  scratch(tmp) {
    mine = tmp;
    char *label = aprintf(tmp, "task %d", id);
    unsigned char *bytes = new_array(tmp, unsigned char, 4096);
    memset(bytes, id, 4096);
    for (int round = 0; round < 10; round++) {
      cinc_yield();
      if (round % 3 == 0) cinc_sleep(1);
      // a nested scratch block takes another arena, and leaves this one alone.
      scratch(inner, tmp) {
        bad += inner == tmp;
        memset(new_array(inner, unsigned char, 4096), 0xff, 4096);
      }
    }
    char expected[32];
    snprintf(expected, sizeof expected, "task %d", id);
    bad += strcmp(label, expected) != 0;
    for (int i = 0; i < 4096; i++) bad += bytes[i] != (unsigned char)id;
  }
  // the arena of the task is its own too, and so is its frame arena.
  char *kept = aprintf(cinc_arena(), "%d", id);
  cinc_yield();
  bad += atoi(kept) != id;
  (void)mine;
  return bad;
}
spawnable(fill, int);

static cilo_arena *which_scratch(void) {
  cilo_arena *a = nullptr;
  scratch(tmp) a = tmp;
  return a;
}
spawnable(which_scratch);

static int body(void *) {
  int bad = 0;
  concurrently {
    future(fill) tasks[64];
    for (int i = 0; i < 64; i++) tasks[i] = spawn(fill(i));
    for (int i = 0; i < 64; i++) bad += await(tasks[i]);
  }
  CHECK(bad == 0);

  // two live tasks never share scratch arenas.
  concurrently {
    auto a = spawn(which_scratch());
    auto b = spawn(which_scratch());
    cilo_arena *mine = which_scratch();
    cilo_arena *pa = await(a), *pb = await(b);
    CHECK(pa != mine && pb != mine);
  }

  // across a blocking block, scratch stays the task's.
  scratch(tmp) {
    char *s = aprintf(tmp, "before");
    blocking {
      scratch(other, tmp) {
        CHECK(other != tmp);
        (void)aprintf(other, "inside");
      }
    }
    CHECK(strcmp(s, "before") == 0);
  }
  return 0;
}

int main(void) {
  // one worker, so every task shares a thread.
  CHECK(CINC_RUN(body, nullptr, .workers = 1) == 0);
  // the thread's own scratch is back afterwards.
  scratch(tmp) {
    char *s = aprintf(tmp, "%s", "plain");
    CHECK(strcmp(s, "plain") == 0);
  }
  return check_done("scratch");
}
