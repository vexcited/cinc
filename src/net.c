#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

/* ========================================================================================================== */
/* reading and writing. every call tries the system call first and waits only when it would block.            */
/* ========================================================================================================== */

static bool closed_by_peer(int err) { return err == ECONNRESET || err == EPIPE || err == ENOTCONN; }

cinc_error cinc_read(int fd, void *buf, size_t len, size_t *got) {
  *got = 0;
  cinc_error e = cinc_check();
  if (e) return e;
  for (;;) {
    ssize_t n = read(fd, buf, len);
    if (n > 0) {
      *got = (size_t)n;
      return CINC_OK;
    }
    if (n == 0) return len ? CINC_ERROR_CLOSED : CINC_OK;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if ((e = cinc_wait_fd(fd, CINC_READABLE))) return e;
      continue;
    }
    return closed_by_peer(errno) ? CINC_ERROR_CLOSED : CINC_ERROR_IO;
  }
}

cinc_error cinc_read_exact(int fd, void *buf, size_t len, size_t *got) {
  size_t done = 0;
  cinc_error e = CINC_OK;
  while (done < len) {
    size_t n;
    if ((e = cinc_read(fd, (char *)buf + done, len - done, &n))) break;
    done += n;
  }
  if (got) *got = done;
  return e;
}

// send keeps SIGPIPE away on Linux, SO_NOSIGPIPE does on the sockets cinc makes elsewhere. pipes and files take
// write.
static ssize_t put(int fd, const void *buf, size_t len, bool *socket) {
  if (*socket) {
    ssize_t n = send(fd, buf, len, SEND_FLAGS);
    if (n >= 0 || errno != ENOTSOCK) return n;
    *socket = false;
  }
  return write(fd, buf, len);
}

cinc_error cinc_write_all(int fd, const void *buf, size_t len) {
  cinc_error e = cinc_check();
  if (e) return e;
  bool socket = true;
  const char *p = buf;
  while (len) {
    ssize_t n = put(fd, p, len, &socket);
    if (n >= 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if ((e = cinc_wait_fd(fd, CINC_WRITABLE))) return e;
      continue;
    }
    return closed_by_peer(errno) ? CINC_ERROR_CLOSED : CINC_ERROR_IO;
  }
  return CINC_OK;
}

cinc_error cinc_writev_all(int fd, const struct iovec *iov, int count) {
  cinc_error e = cinc_check();
  if (e) return e;
  struct iovec local[64];
  bool socket = true;
  while (count > 0) {
    // a window of the caller's buffers, advanced in place as bytes go out.
    int n = count < 64 ? count : 64;
    memcpy(local, iov, (size_t)n * sizeof *iov);
    struct iovec *v = local;
    while (n > 0) {
      ssize_t wrote;
      if (socket) {
        struct msghdr msg = {.msg_iov = v, .msg_iovlen = n};
        wrote = sendmsg(fd, &msg, SEND_FLAGS);
        if (wrote < 0 && errno == ENOTSOCK) {
          socket = false;
          continue;
        }
      } else {
        wrote = writev(fd, v, n);
      }
      if (wrote < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if ((e = cinc_wait_fd(fd, CINC_WRITABLE))) return e;
          continue;
        }
        return closed_by_peer(errno) ? CINC_ERROR_CLOSED : CINC_ERROR_IO;
      }
      size_t left = (size_t)wrote;
      while (n > 0 && left >= v->iov_len) {
        left -= v->iov_len;
        v++;
        n--;
      }
      if (n > 0) {
        v->iov_base = (char *)v->iov_base + left;
        v->iov_len -= left;
      }
    }
    int done = count < 64 ? count : 64;
    iov += done;
    count -= done;
  }
  return CINC_OK;
}

/* ========================================================================================================== */
/* sockets.                                                                                                   */
/* ========================================================================================================== */

#if !defined(SOCK_NONBLOCK) || !defined(__linux__)
static void set_flags(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}
#endif

static int new_socket(int family) {
#if defined(SOCK_NONBLOCK)
  int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
  int fd = socket(family, SOCK_STREAM, 0);
  if (fd >= 0) set_flags(fd);
#endif
  return fd;
}

static void no_delay(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

typedef struct resolving {
  cinc__job job;
  struct addrinfo hints;
  struct addrinfo *res;
  int rc;
  char port[8];
  char host[];
} resolving;

static void resolve_run(void *p) {
  resolving *r = p;
  r->rc = getaddrinfo(r->host[0] ? r->host : nullptr, r->port, &r->hints, &r->res);
}

static void resolve_abandon(cinc__job *j) {
  resolving *r = (resolving *)j;
  if (!r->rc && r->res) freeaddrinfo(r->res);
  cilo_mfree(r);
}

// numeric hosts are parsed right here. names go to the blocking pool, which the task may give up on when its
// deadline expires: the job then frees itself.
static cinc_error resolve(const char *host, size_t host_len, uint16_t port, bool passive, struct addrinfo **out) {
  resolving *r = cilo_calloc(1, sizeof *r + host_len + 1);
  memcpy(r->host, host, host_len);
  snprintf(r->port, sizeof r->port, "%u", port);
  r->hints = (struct addrinfo){.ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0)};
  r->hints.ai_flags |= AI_NUMERICHOST;
  r->rc = getaddrinfo(r->host[0] ? r->host : nullptr, r->port, &r->hints, &r->res);
  if (r->rc == EAI_NONAME && r->host[0]) {
    r->hints.ai_flags &= ~AI_NUMERICHOST;
    r->hints.ai_flags |= AI_ADDRCONFIG;
    cinc__thread *th = cinc__self();
    if (cinc__in_worker(th)) {
      r->job = (cinc__job){.fn = resolve_run, .arg = r, .abandon = resolve_abandon};
      cinc_error e = cinc__offload(th, th->current, &r->job, true);
      if (e) return e;
    } else {
      resolve_run(r);
    }
  }
  cinc_error e = CINC_OK;
  if (r->rc) {
    errno = r->rc == EAI_SYSTEM ? errno : EHOSTUNREACH;
    e = CINC_ERROR_IO;
  } else {
    *out = r->res;
  }
  cilo_mfree(r);
  return e;
}

cinc_error cinc_connect(const char *host, uint16_t port, int *fd) {
  *fd = -1;
  cinc_error e = cinc_check();
  if (e) return e;
  struct addrinfo *list;
  if ((e = resolve(host, strlen(host), port, false, &list))) return e;
  int err = ECONNREFUSED;
  for (struct addrinfo *a = list; a; a = a->ai_next) {
    int s = new_socket(a->ai_family);
    if (s < 0) {
      err = errno;
      continue;
    }
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int r = connect(s, a->ai_addr, a->ai_addrlen);
    if (r < 0 && errno == EINPROGRESS) {
      if ((e = cinc_wait_fd(s, CINC_WRITABLE))) {
        cinc_close(s);
        break;
      }
      socklen_t len = sizeof err;
      r = getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err ? -1 : 0;
      if (r < 0 && !err) err = errno;
    } else if (r < 0) {
      err = errno;
    }
    if (r == 0) {
      no_delay(s);
      *fd = s;
      break;
    }
    cinc_close(s);
  }
  freeaddrinfo(list);
  if (e) return e;
  if (*fd < 0) {
    errno = err;
    return CINC_ERROR_IO;
  }
  return CINC_OK;
}

static int listen_on(const struct sockaddr *addr, socklen_t len, bool reuse_port, bool dual) {
  int s = new_socket(addr->sa_family);
  if (s < 0) return -1;
  int one = 1, zero = 0;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#if defined(SO_REUSEPORT)
  if (reuse_port) setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#else
  (void)reuse_port;
#endif
  if (addr->sa_family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, dual ? &zero : &one, sizeof one);
  if (bind(s, addr, len) < 0 || listen(s, 4096) < 0) {
    int err = errno;
    close(s);
    errno = err;
    return -1;
  }
  return s;
}

cinc_error cinc_listen(const char *bind, bool reuse_port, int *fd) {
  *fd = -1;
  const char *colon = strrchr(bind, ':');
  if (!colon) {
    errno = EINVAL;
    return CINC_ERROR_IO;
  }
  char *end;
  unsigned long port = strtoul(colon + 1, &end, 10);
  if (*end || end == colon + 1 || port > 65535) {
    errno = EINVAL;
    return CINC_ERROR_IO;
  }
  const char *host = bind;
  size_t host_len = (size_t)(colon - bind);
  if (host_len >= 2 && host[0] == '[' && host[host_len - 1] == ']') {
    host++;
    host_len -= 2;
  } else if (memchr(host, ':', host_len)) {
    // an IPv6 address needs its brackets, or the port is ambiguous.
    errno = EINVAL;
    return CINC_ERROR_IO;
  }
  bool any = !host_len || (host_len == 1 && host[0] == '*');

  if (any || (host_len == 2 && !memcmp(host, "::", 2))) {
    // one IPv6 socket that takes IPv4 too, or IPv4 alone where the host has no IPv6, as some containers do.
    struct sockaddr_in6 six = {.sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port), .sin6_addr = in6addr_any};
    *fd = listen_on((struct sockaddr *)&six, sizeof six, reuse_port, true);
    if (*fd >= 0 || !any) return *fd >= 0 ? CINC_OK : CINC_ERROR_IO;
    struct sockaddr_in four = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    *fd = listen_on((struct sockaddr *)&four, sizeof four, reuse_port, false);
    return *fd >= 0 ? CINC_OK : CINC_ERROR_IO;
  }

  struct addrinfo *list;
  cinc_error e = resolve(host, host_len, (uint16_t)port, true, &list);
  if (e) return e;
  int err = EADDRNOTAVAIL;
  // IPv4 first: "localhost" then serves 127.0.0.1, which every client tries in the end.
  for (int family = AF_INET; *fd < 0 && family; family = family == AF_INET ? AF_INET6 : 0)
    for (struct addrinfo *a = list; a && *fd < 0; a = a->ai_next) {
      if (a->ai_family != family) continue;
      *fd = listen_on(a->ai_addr, a->ai_addrlen, reuse_port, false);
      if (*fd < 0) err = errno;
    }
  freeaddrinfo(list);
  if (*fd >= 0) return CINC_OK;
  errno = err;
  return CINC_ERROR_IO;
}

uint16_t cinc_local_port(int fd) {
  struct sockaddr_storage ss;
  socklen_t len = sizeof ss;
  if (getsockname(fd, (struct sockaddr *)&ss, &len) < 0) return 0;
  if (ss.ss_family == AF_INET) return ntohs(((struct sockaddr_in *)&ss)->sin_port);
  if (ss.ss_family == AF_INET6) return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
  return 0;
}

cinc_error cinc_accept(int listener, int *fd) {
  *fd = -1;
  cinc_error e = cinc_check();
  if (e) return e;
  for (;;) {
#if defined(__linux__)
    int s = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    int s = accept(listener, nullptr, nullptr);
    if (s >= 0) set_flags(s);
#endif
    if (s >= 0) {
      no_delay(s);
      *fd = s;
      return CINC_OK;
    }
    if (errno == EINTR || errno == ECONNABORTED) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return CINC_ERROR_IO;
    if ((e = cinc_wait_fd(listener, CINC_READABLE))) return e;
  }
}

/* ========================================================================================================== */
/* shutdown signals: the handler writes to a pipe nobody reads, so it stays readable for every waiter.        */
/* ========================================================================================================== */

static int signal_pipe[2] = {-1, -1};
static volatile sig_atomic_t signal_last;
static volatile sig_atomic_t signal_count;
static pthread_once_t signal_once = PTHREAD_ONCE_INIT;

static void on_signal(int signo) {
  int saved = errno;
  if (signal_count++) {
    signal(signo, SIG_DFL);
    raise(signo);
  }
  signal_last = signo;
  [[maybe_unused]] ssize_t n = write(signal_pipe[1], "!", 1);
  errno = saved;
}

static void signal_install(void) {
  if (pipe(signal_pipe) < 0) return;
  for (int i = 0; i < 2; i++) {
    fcntl(signal_pipe[i], F_SETFL, fcntl(signal_pipe[i], F_GETFL) | O_NONBLOCK);
    fcntl(signal_pipe[i], F_SETFD, FD_CLOEXEC);
  }
  struct sigaction sa = {.sa_handler = on_signal, .sa_flags = SA_RESTART};
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

cinc_error cinc_wait_shutdown(int *signo) {
  pthread_once(&signal_once, signal_install);
  if (signal_pipe[0] < 0) return CINC_ERROR_IO;
  cinc_error e = cinc_wait_fd(signal_pipe[0], CINC_READABLE);
  if (!e && signo) *signo = signal_last;
  return e;
}
