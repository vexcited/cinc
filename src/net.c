#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#if defined(_WIN32)

/* ========================================================================================================== */
/* winsock. an fd is a SOCKET cast to int, whose low 32 bits are all a handle has. winsock keeps its own      */
/* error, which goes to errno so the code below reads the same everywhere.                                    */
/* ========================================================================================================== */

static pthread_once_t wsa_once = PTHREAD_ONCE_INIT;

static void wsa_start(void) {
  WSADATA data;
  WSAStartup(MAKEWORD(2, 2), &data);
}

void cinc__net_init(void) { pthread_once(&wsa_once, wsa_start); }

int cinc__errno_of(int wsa_error) {
  switch (wsa_error) {
  case WSAEINTR: return EINTR;
  case WSAEBADF: return EBADF;
  case WSAEACCES: return EACCES;
  case WSAEFAULT: return EFAULT;
  case WSAEINVAL: return EINVAL;
  case WSAEMFILE: return EMFILE;
  case WSAEWOULDBLOCK: return EWOULDBLOCK;
  case WSAEINPROGRESS: return EINPROGRESS;
  case WSAEALREADY: return EALREADY;
  case WSAENOTSOCK: return ENOTSOCK;
  case WSAEDESTADDRREQ: return EDESTADDRREQ;
  case WSAEMSGSIZE: return EMSGSIZE;
  case WSAEPROTOTYPE: return EPROTOTYPE;
  case WSAENOPROTOOPT: return ENOPROTOOPT;
  case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
  case WSAEOPNOTSUPP: return EOPNOTSUPP;
  case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
  case WSAEADDRINUSE: return EADDRINUSE;
  case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
  case WSAENETDOWN: return ENETDOWN;
  case WSAENETUNREACH: return ENETUNREACH;
  case WSAENETRESET: return ENETRESET;
  case WSAECONNABORTED: return ECONNABORTED;
  case WSAECONNRESET: return ECONNRESET;
  case WSAENOBUFS: return ENOBUFS;
  case WSAEISCONN: return EISCONN;
  case WSAENOTCONN: return ENOTCONN;
  case WSAESHUTDOWN: return EPIPE;
  case WSAETIMEDOUT: return ETIMEDOUT;
  case WSAECONNREFUSED: return ECONNREFUSED;
  case WSAENAMETOOLONG: return ENAMETOOLONG;
  case WSAEHOSTDOWN:
  case WSAEHOSTUNREACH: return EHOSTUNREACH;
  default: return EIO;
  }
}

static SOCKET sock_of(int fd) { return (SOCKET)(intptr_t)fd; }

static int failed(void) {
  errno = cinc__errno_of(WSAGetLastError());
  return -1;
}

static int clamp(size_t len) { return len > INT_MAX ? INT_MAX : (int)len; }

#endif

static void opt(int fd, int level, int name, int value) {
#if defined(_WIN32)
  setsockopt(sock_of(fd), level, name, (const char *)&value, sizeof value);
#else
  setsockopt(fd, level, name, &value, sizeof value);
#endif
}

static void close_socket(int fd) {
#if defined(_WIN32)
  closesocket(sock_of(fd));
#else
  close(fd);
#endif
}

/* ========================================================================================================== */
/* reading and writing. every call tries the system call first and waits only when it would block.            */
/* ========================================================================================================== */

static bool closed_by_peer(int err) {
  return err == ECONNRESET || err == ECONNABORTED || err == EPIPE || err == ENOTCONN;
}

// sockets first, on windows then the crt's descriptors.
static ssize_t get(int fd, void *buf, size_t len) {
#if defined(_WIN32)
  int n = recv(sock_of(fd), buf, clamp(len), 0);
  if (n >= 0) return n;
  if (WSAGetLastError() == WSAENOTSOCK) return _read(fd, buf, (unsigned)clamp(len));
  return failed();
#else
  return read(fd, buf, len);
#endif
}

cinc_error cinc_read(int fd, void *buf, size_t len, size_t *got) {
  *got = 0;
  cinc_error e = cinc_check();
  if (e) return e;
  for (;;) {
    ssize_t n = get(fd, buf, len);
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
#if defined(_WIN32)
    int n = send(sock_of(fd), buf, clamp(len), 0);
    if (n >= 0) return n;
    if (WSAGetLastError() != WSAENOTSOCK) return failed();
#else
    ssize_t n = send(fd, buf, len, SEND_FLAGS);
    if (n >= 0 || errno != ENOTSOCK) return n;
#endif
    *socket = false;
  }
#if defined(_WIN32)
  return _write(fd, buf, (unsigned)clamp(len));
#else
  return write(fd, buf, len);
#endif
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
    int n = count < (int)countof(local) ? count : (int)countof(local);
    memcpy(local, iov, (size_t)n * sizeof *iov);
    struct iovec *v = local;
    while (n > 0) {
      ssize_t wrote;
      if (socket) {
#if defined(_WIN32)
        WSABUF bufs[countof(local)];
        for (int i = 0; i < n; i++)
          bufs[i] = (WSABUF){.len = v[i].iov_len > ULONG_MAX ? ULONG_MAX : (ULONG)v[i].iov_len, .buf = v[i].iov_base};
        DWORD sent;
        wrote = WSASend(sock_of(fd), bufs, (DWORD)n, &sent, 0, nullptr, nullptr) ? failed() : (ssize_t)sent;
#else
        struct msghdr msg = {.msg_iov = v, .msg_iovlen = n};
        wrote = sendmsg(fd, &msg, SEND_FLAGS);
#endif
        if (wrote < 0 && errno == ENOTSOCK) {
          socket = false;
          continue;
        }
      } else {
#if defined(_WIN32)
        // the crt writes one buffer at a time.
        wrote = _write(fd, v->iov_base, (unsigned)clamp(v->iov_len));
#else
        wrote = writev(fd, v, n);
#endif
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
    int done = count < (int)countof(local) ? count : (int)countof(local);
    iov += done;
    count -= done;
  }
  return CINC_OK;
}

/* ========================================================================================================== */
/* sockets.                                                                                                   */
/* ========================================================================================================== */

#if defined(_WIN32)

// not inherited by child processes, and non blocking.
static void set_flags(int fd) {
  SetHandleInformation((HANDLE)sock_of(fd), HANDLE_FLAG_INHERIT, 0);
  u_long one = 1;
  ioctlsocket(sock_of(fd), FIONBIO, &one);
}

#elif !defined(SOCK_NONBLOCK) || !defined(__linux__)

static void set_flags(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
  opt(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
}

#endif

[[nodiscard]] static int new_socket(int family) {
#if defined(_WIN32)
  cinc__net_init();
  SOCKET s = WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) return failed();
  int fd = (int)s;
  set_flags(fd);
#elif defined(SOCK_NONBLOCK)
  int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
  int fd = socket(family, SOCK_STREAM, 0);
  if (fd >= 0) set_flags(fd);
#endif
  return fd;
}

static void no_delay(int fd) { opt(fd, IPPROTO_TCP, TCP_NODELAY, 1); }

// the error a connect in progress ended with, as an errno, 0 when it went through.
static int connect_error(int fd) {
  int err = 0;
  socklen_t len = sizeof err;
#if defined(_WIN32)
  if (getsockopt(sock_of(fd), SOL_SOCKET, SO_ERROR, (char *)&err, &len)) return cinc__errno_of(WSAGetLastError());
  return err ? cinc__errno_of(err) : 0;
#else
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return errno;
  return err;
#endif
}

#if defined(_WIN32)

// windows retries a SYN that was refused for two seconds, not worth it on loopback, where libuv turns it off too.
static void refuse_fast(int fd, const struct sockaddr *a) {
  bool loopback = a->sa_family == AF_INET ? ntohl(((const struct sockaddr_in *)a)->sin_addr.s_addr) >> 24 == 127
                                          : IN6_IS_ADDR_LOOPBACK(&((const struct sockaddr_in6 *)a)->sin6_addr);
  if (!loopback) return;
  TCP_INITIAL_RTO_PARAMETERS rto = {.Rtt = TCP_INITIAL_RTO_UNSPECIFIED_RTT,
                                    .MaxSynRetransmissions = TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS};
  DWORD bytes;
  WSAIoctl(sock_of(fd), SIO_TCP_INITIAL_RTO, &rto, sizeof rto, nullptr, 0, &bytes, nullptr, nullptr);
}

#endif

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
[[nodiscard]]
static cinc_error resolve(const char *host, size_t host_len, uint16_t port, bool passive, struct addrinfo **out) {
#if defined(_WIN32)
  cinc__net_init();
#endif
  resolving *r = cilo_calloc(1, sizeof *r + host_len + 1);
  memcpy(r->host, host, host_len);
  snprintf(r->port, sizeof r->port, "%u", port);
  r->hints = (struct addrinfo){.ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0)};
  r->hints.ai_flags |= AI_NUMERICHOST;
  r->rc = getaddrinfo(r->host[0] ? r->host : nullptr, r->port, &r->hints, &r->res);
  if (r->rc == EAI_NONAME && r->host[0]) {
    // no AI_ADDRCONFIG: it hides localhost in a container that only has loopback. an IPv6 address on a host
    // without IPv6 just fails to connect, and the next one is tried.
    r->hints.ai_flags &= ~AI_NUMERICHOST;
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
#if defined(EAI_SYSTEM)
    errno = r->rc == EAI_SYSTEM ? errno : EHOSTUNREACH;
#else
    errno = EHOSTUNREACH;
#endif
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
    opt(s, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
#if defined(_WIN32)
    refuse_fast(s, a->ai_addr);
    int r = connect(sock_of(s), a->ai_addr, (int)a->ai_addrlen) ? failed() : 0;
    // winsock says a connect is on its way with WSAEWOULDBLOCK.
    if (r < 0 && errno == EWOULDBLOCK) errno = EINPROGRESS;
#else
    int r = connect(s, a->ai_addr, a->ai_addrlen);
#endif
    if (r < 0 && errno == EINPROGRESS) {
      if ((e = cinc_wait_fd(s, CINC_WRITABLE))) {
        cinc_close(s);
        break;
      }
      int failure = connect_error(s);
      r = failure ? -1 : 0;
      if (failure) err = failure;
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

[[nodiscard]] static int listen_on(const struct sockaddr *addr, socklen_t len, bool reuse_port, bool dual) {
  int s = new_socket(addr->sa_family);
  if (s < 0) return -1;
#if defined(_WIN32)
  // windows rebinds a port in TIME_WAIT by itself, its SO_REUSEADDR shares the port like SO_REUSEPORT.
  if (reuse_port) opt(s, SOL_SOCKET, SO_REUSEADDR, 1);
#else
  opt(s, SOL_SOCKET, SO_REUSEADDR, 1);
#if defined(SO_REUSEPORT)
  if (reuse_port) opt(s, SOL_SOCKET, SO_REUSEPORT, 1);
#else
  (void)reuse_port;
#endif
#endif
  if (addr->sa_family == AF_INET6) opt(s, IPPROTO_IPV6, IPV6_V6ONLY, !dual);
#if defined(_WIN32)
  bool bound = !bind(sock_of(s), addr, len) && !listen(sock_of(s), SOMAXCONN);
  if (!bound) failed();
#else
  bool bound = bind(s, addr, len) == 0 && listen(s, 4096) == 0;
#endif
  if (!bound) {
    int err = errno;
    close_socket(s);
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
  families: for (int family = AF_INET; family; family = family == AF_INET ? AF_INET6 : 0)
    for (struct addrinfo *a = list; a; a = a->ai_next) {
      if (a->ai_family != family) continue;
      *fd = listen_on(a->ai_addr, a->ai_addrlen, reuse_port, false);
      if (*fd >= 0) break families;
      err = errno;
    }
  freeaddrinfo(list);
  if (*fd >= 0) return CINC_OK;
  errno = err;
  return CINC_ERROR_IO;
}

uint16_t cinc_local_port(int fd) {
  struct sockaddr_storage ss;
  socklen_t len = sizeof ss;
#if defined(_WIN32)
  if (getsockname(sock_of(fd), (struct sockaddr *)&ss, &len)) return 0;
#else
  if (getsockname(fd, (struct sockaddr *)&ss, &len) < 0) return 0;
#endif
  if (ss.ss_family == AF_INET) return ntohs(((struct sockaddr_in *)&ss)->sin_port);
  if (ss.ss_family == AF_INET6) return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
  return 0;
}

cinc_error cinc_accept(int listener, int *fd) {
  *fd = -1;
  cinc_error e = cinc_check();
  if (e) return e;
  for (;;) {
#if defined(_WIN32)
    SOCKET a = accept(sock_of(listener), nullptr, nullptr);
    int s = a == INVALID_SOCKET ? failed() : (int)a;
    if (s >= 0) set_flags(s);
#elif defined(__linux__)
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
    // a connection reset before it was taken is skipped, which windows says with WSAECONNRESET.
    if (errno == EINTR || errno == ECONNABORTED || errno == ECONNRESET) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return CINC_ERROR_IO;
    if ((e = cinc_wait_fd(listener, CINC_READABLE))) return e;
  }
}

/* ========================================================================================================== */
/* shutdown signals: the handler writes to a pipe nobody reads, so it stays readable for every waiter.        */
/* windows polls sockets only, there it is a udp socket that sends to itself.                                 */
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
#if defined(_WIN32)
  send(sock_of(signal_pipe[1]), "!", 1, 0);
#else
  [[maybe_unused]] ssize_t n = write(signal_pipe[1], "!", 1);
#endif
  errno = saved;
}

#if defined(_WIN32)

// a console's ctrl c and ctrl break are SIGINT, its closing, a log off and a shut down SIGTERM. the handler runs
// on a thread of its own, and windows ends the process once it returns from the last three: it waits there for
// the app to finish, until windows' own timeout.
static BOOL WINAPI on_console(DWORD event) {
  if (signal_count) return FALSE;
  bool interrupt = event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
  on_signal(interrupt ? SIGINT : SIGTERM);
  if (!interrupt) Sleep(INFINITE);
  return TRUE;
}

static void signal_install(void) {
  cinc__net_init();
  SOCKET s = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  struct sockaddr_in self = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  int len = sizeof self;
  if (s == INVALID_SOCKET) return;
  if (bind(s, (struct sockaddr *)&self, len) || getsockname(s, (struct sockaddr *)&self, &len) ||
      connect(s, (struct sockaddr *)&self, len)) {
    closesocket(s);
    return;
  }
  set_flags((int)s);
  signal_pipe[0] = signal_pipe[1] = (int)s;
  // raise reaches these, the console the handler below.
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  SetConsoleCtrlHandler(on_console, TRUE);
}

#else

static void signal_install(void) {
  if (pipe(signal_pipe) < 0) return;
  for (size_t i = 0; i < countof(signal_pipe); i++) {
    fcntl(signal_pipe[i], F_SETFL, fcntl(signal_pipe[i], F_GETFL) | O_NONBLOCK);
    fcntl(signal_pipe[i], F_SETFD, FD_CLOEXEC);
  }
  struct sigaction sa = {.sa_handler = on_signal, .sa_flags = SA_RESTART};
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

#endif

cinc_error cinc_wait_shutdown(int *signo) {
  pthread_once(&signal_once, signal_install);
  if (signal_pipe[0] < 0) return CINC_ERROR_IO;
  cinc_error e = cinc_wait_fd(signal_pipe[0], CINC_READABLE);
  if (!e && signo) *signo = signal_last;
  return e;
}
