#ifndef CINC_CHECK_H
#define CINC_CHECK_H

#include <cinc/dsl.h>
#include <stdatomic.h>
#include <stdio.h>

static _Atomic(int) failures;

#define CHECK(cond)                                                                                              \
  do {                                                                                                           \
    if (!(cond)) {                                                                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                 \
      atomic_fetch_add(&failures, 1);                                                                            \
    }                                                                                                            \
  } while (0)

static inline int check_done(const char *name) {
  int n = atomic_load(&failures);
  fprintf(n ? stderr : stdout, "%s: %s\n", name, n ? "failed" : "ok");
  return n != 0;
}

static inline uint64_t elapsed_ms(uint64_t since) { return cinc_now_ms() - since; }

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <stdlib.h>
#include <windows.h>

// windows has no fork: the test runs itself again with `what` as its argument, and gives its exit code.
static inline unsigned long check_child(const char *what) {
  char self[MAX_PATH];
  GetModuleFileNameA(nullptr, self, sizeof self);
  return (unsigned long)_spawnl(_P_WAIT, self, "child", what, nullptr);
}

// in the child: a crash or an abort ends it with its code, without a dialog.
static inline void check_quiet(void) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
#endif

#endif
