#include <cinc/cinc.h>
#include <pthread.h>
#include <stdcountof.h>
#include <stdio.h>
#include <string.h>

// a pretend key value store, answering `GET key\r\n` with one line.
#define KV_PORT 7379

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
  if (kv_open(&c, "127.0.0.1", KV_PORT))
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
  cinc_error e = kv_open(&c, "127.0.0.1", KV_PORT);
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
      for (size_t i = 0; i < countof(keys); i++)
        CINC_SPAWN(lookup(keys[i]));
    }
  } else {
    return 1;
  }
  return 0;
}

/* ========================================================================================================== */
/* the server is a plain thread too, so this program needs nothing else running.                              */
/* ========================================================================================================== */

static void *kv_serve(void *arg) {
  int listener = *(int *)arg, fd;
  while (cinc_accept(listener, &fd) == CINC_OK) {
    char line[256], reply[300];
    size_t got;
    // a request fits in one read here, a real server would buffer.
    if (cinc_read(fd, line, sizeof line - 1, &got) == CINC_OK) {
      line[got] = 0;
      line[strcspn(line, "\r\n")] = 0;
      int n = snprintf(reply, sizeof reply, "value of %s", line + 4);
      cinc_write_all(fd, reply, (size_t)n);
    }
    cinc_close(fd);
  }
  return nullptr;
}

int main(void) {
  int listener;
  if (cinc_listen("127.0.0.1:7379", false, &listener))
    return 1;
  pthread_t server;
  pthread_create(&server, nullptr, kv_serve, &listener);
  // the same client, called from a plain main and then from tasks.
  return blocking_main() || CINC_RUN(async_main, nullptr);
}
