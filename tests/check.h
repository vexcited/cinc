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

#endif
