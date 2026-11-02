#include "check.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

// macOS keeps 128 connections waiting for accept at most, past that it resets them.
enum { CLIENTS = 120, ROUNDS = 8 };

static _Atomic(int) served;

static void serve(int fd) {
  int nodelay = 0;
  socklen_t len = sizeof nodelay;
  getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, &len);
  CHECK(nodelay);
  char buf[16 << 10];
  size_t n;
  while (!cinc_read(fd, buf, sizeof buf, &n))
    if (cinc_write_all(fd, buf, n)) break;
  cinc_close(fd);
  served++;
}
spawnable(serve, int);

// accepts on one task and spreads the connections over the workers, the way that works everywhere.
static int accept_loop(int listener, cinc_group *conns) {
  for (uint32_t next = 0;; next++) {
    int fd;
    cinc_error e = cinc_accept(listener, &fd);
    if (e) return e;
    spawn_in_on(conns, next, serve(fd));
  }
}
spawnable(accept_loop, int, cinc_group *);

static int send_all(int fd, const char *buf, size_t len) {
  // two writes in one call, the second split over several buffers.
  size_t half = len / 2;
  size_t third = (len - half) / 3;
  struct iovec iov[3] = {{(void *)(buf + half), third},
                         {(void *)(buf + half + third), third},
                         {(void *)(buf + half + 2 * third), len - half - 2 * third}};
  cinc_error e = cinc_write_all(fd, buf, half);
  return e ? e : cinc_writev_all(fd, iov, 3);
}
spawnable(send_all, int, const char *, size_t);

static int client(uint16_t port, uint32_t id) {
  int fd;
  cinc_error ce = cinc_connect("127.0.0.1", port, &fd);
  if (ce) fprintf(stderr, "connect %d %s\n", ce, strerror(errno));
  if (ce) return 1;
  int bad = 0;
  size_t size = 1 + id * 997 % (256 << 10);
  char *out = malloc(size), *in = malloc(size);
  for (int round = 0; round < ROUNDS && !bad; round++) {
    for (size_t i = 0; i < size; i++) out[i] = (char)(i * 31 + id + (uint32_t)round);
    // writing and reading at once, so neither side's buffers fill up.
    concurrently {
      auto w = spawn(send_all(fd, out, size));
      size_t got;
      cinc_error re = cinc_read_exact(fd, in, size, &got);
      if (re) fprintf(stderr, "read %d %s got %zu of %zu\n", re, strerror(errno), got, size);
      if (re || got != size || memcmp(in, out, size)) bad = 1;
      if (await(w)) bad = 1;
    }
  }
  free(out);
  free(in);
  cinc_close(fd);
  return bad;
}
spawnable(client, uint16_t, uint32_t);

static bool connects(const char *host, uint16_t port) {
  int fd;
  if (cinc_connect(host, port, &fd)) return false;
  cinc_close(fd);
  return true;
}

static int body(void *) {
  int listener;
  CHECK(cinc_listen("127.0.0.1:0", false, &listener) == CINC_OK);
  uint16_t port = cinc_local_port(listener);
  CHECK(port != 0);
  cinc_group conns = {};
  int failed = 0;
  concurrently {
    auto acceptor = spawn(accept_loop(listener, &conns));
    concurrently {
      future(client) clients[CLIENTS];
      for (uint32_t i = 0; i < CLIENTS; i++) clients[i] = spawn_on(1 + i % 3, client(port, i));
      for (uint32_t i = 0; i < CLIENTS; i++) failed += await(clients[i]);
    }
    cancel(acceptor);
    CHECK(cinc_join(acceptor.task) == CINC_ERROR_CANCELLED);
  }
  CHECK(failed == 0);
  CHECK(cinc_group_wait(&conns) == CINC_OK);
  CHECK(served == CLIENTS);

  // a read honors the deadline, and a closed peer is CINC_ERROR_CLOSED rather than SIGPIPE.
  int a, b;
  concurrently {
    auto acceptor = spawn(accept_loop(listener, &conns));
    CHECK(cinc_connect("localhost", port, &a) == CINC_OK);
    char c;
    size_t got;
    within(30) {
      CHECK(cinc_read(a, &c, 1, &got) == CINC_ERROR_TIMEOUT);
    }
    cancel(acceptor);
  }
  cinc_group_cancel(&conns);
  CHECK(cinc_group_wait(&conns) == CINC_OK);
  char *big = calloc(1, 64 << 10);
  cinc_error e = CINC_OK;
  for (int i = 0; i < 64 && !e; i++) e = cinc_write_all(a, big, 64 << 10);
  CHECK(e == CINC_ERROR_CLOSED);
  free(big);
  cinc_close(a);
  cinc_close(listener);

  // nothing listens there anymore.
  errno = 0;
  CHECK(cinc_connect("127.0.0.1", port, &b) == CINC_ERROR_IO && errno == ECONNREFUSED);

  // the binds a server takes.
  CHECK(cinc_listen("0.0.0.0:0", false, &listener) == CINC_OK);
  CHECK(connects("127.0.0.1", cinc_local_port(listener)));
  cinc_close(listener);
  CHECK(cinc_listen("localhost:0", false, &listener) == CINC_OK);
  CHECK(connects("localhost", cinc_local_port(listener)));
  cinc_close(listener);
  CHECK(cinc_listen(":0", false, &listener) == CINC_OK);
  CHECK(connects("127.0.0.1", cinc_local_port(listener)));
  cinc_close(listener);
  if (cinc_listen("[::]:0", false, &listener) == CINC_OK) {
    // dual stack: IPv4 comes in on the IPv6 socket.
    CHECK(connects("127.0.0.1", cinc_local_port(listener)));
    cinc_close(listener);
  }
  CHECK(cinc_listen("::1:80", false, &listener) == CINC_ERROR_IO);
  CHECK(cinc_listen("127.0.0.1:99999", false, &listener) == CINC_ERROR_IO);

  // several sockets on one port with reuse_port.
  int first, second;
  CHECK(cinc_listen("127.0.0.1:0", true, &first) == CINC_OK);
  char bind[32];
  snprintf(bind, sizeof bind, "127.0.0.1:%u", cinc_local_port(first));
  CHECK(cinc_listen(bind, true, &second) == CINC_OK);
  cinc_close(first);
  cinc_close(second);
  return 0;
}

int main(void) {
  CHECK(CINC_RUN(body, nullptr, .workers = 4) == 0);
  return check_done("tcp");
}
