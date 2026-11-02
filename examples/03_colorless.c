#include <cinc/cinc.h>
#include <stdio.h>

// a pretend key value store, answering `GET key\r\n` with one line.
typedef struct kv {
  int fd;
} kv;

static cinc_error kv_open(kv *c, const char *host, uint16_t port) {
  return cinc_connect(host, port, &c->fd);
}

static cinc_error kv_get(kv *c, const char *key, char *out, size_t cap) {
  char line[256];
  int n = snprintf(line, sizeof line, "GET %s\r\n", key);
  size_t got = 0;
  cinc_error e = cinc_write_all(c->fd, line, (size_t)n);
  if (!e)
    e = cinc_read(c->fd, out, cap - 1, &got);
  out[got] = 0;
  return e;
}

static void kv_close(kv *c) {
  cinc_close(c->fd);
}

/* ========================================================================================================== */
/* a plain program.                                                                                           */
/* ========================================================================================================== */

static int blocking_main(void) {
  kv c;
  char value[128];
  if (kv_open(&c, "127.0.0.1", 7000))
    return 1;
  cinc_error e = kv_get(&c, "motd", value, sizeof value);
  if (!e)
    puts(value);
  kv_close(&c);
  return e != CINC_OK;
}

/* ========================================================================================================== */
/* the same client in a runtime: a connection per task, and one deadline over all of them.                    */
/* ========================================================================================================== */

static cinc_error lookup(const char *key) {
  kv c;
  char value[128];
  cinc_error e = kv_open(&c, "127.0.0.1", 7000);
  if (e)
    return e;
  e = kv_get(&c, key, value, sizeof value);
  if (!e)
    printf("%s = %s\n", key, value);
  kv_close(&c);
  return e;
}
CINC_SPAWNABLE(lookup, const char *);

static int async_main(void *) {
  static const char *const keys[] = {"motd", "users", "version", "uptime"};

  CINC_WITHIN(500) {
    CINC_CONCURRENTLY {
      for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        CINC_SPAWN(lookup(keys[i]));
    }
  } else {
    return 1;
  }
  return 0;
}

int main(int argc, char **) {
  return argc > 1 ? CINC_RUN(async_main, nullptr) : blocking_main();
}
