#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "internal.h"

#include <errno.h>
#include <stdcountof.h>
#include <stddefer.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#if !CINC__KQUEUE && !defined(_WIN32)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif

#if defined(_WIN32)

/* ========================================================================================================== */
/* windows reports socket readiness through IOCTL_AFD_POLL on the afd device, answered on an io completion    */
/* port, which is what WSAPoll uses inside and what wepoll and mio use too. each wait sends a poll of its     */
/* own, so a socket closed behind cinc's back answers it with AFD_POLL_LOCAL_CLOSE and leaves nothing stale.  */
/* ========================================================================================================== */

enum : ULONG {
  AFD_POLL_RECEIVE = 0x0001,
  AFD_POLL_SEND = 0x0004,
  AFD_POLL_DISCONNECT = 0x0008,
  AFD_POLL_ABORT = 0x0010,
  AFD_POLL_LOCAL_CLOSE = 0x0020,
  AFD_POLL_ACCEPT = 0x0080,
  AFD_POLL_CONNECT_FAIL = 0x0100,
  IOCTL_AFD_POLL = 0x00012024,
};

typedef struct afd_poll_info {
  LARGE_INTEGER timeout;
  ULONG count;
  ULONG exclusive;
  struct {
    HANDLE socket;
    ULONG events;
    NTSTATUS status;
  } handles[1];
} afd_poll_info;

// in ntdll since windows 8, which the sdk headers leave out.
NTSTATUS NTAPI NtCreateWaitCompletionPacket(HANDLE *packet, ACCESS_MASK access, OBJECT_ATTRIBUTES *attrs);
NTSTATUS NTAPI NtAssociateWaitCompletionPacket(HANDLE packet, HANDLE port, HANDLE target, void *key, void *context,
                                               NTSTATUS status, ULONG_PTR information, BOOLEAN *signaled);
NTSTATUS NTAPI NtCancelWaitCompletionPacket(HANDLE packet, BOOLEAN remove_signaled);

// the kernel writes the answer here, so it stays with the worker until it came, whether its waiter left or not.
typedef struct afd_poll {
  IO_STATUS_BLOCK iosb;
  afd_poll_info info;
  struct afd_poll *next;
  // null once the waiter left.
  cinc_task *task;
  uint8_t got;
  bool done;
} afd_poll;

static afd_poll *poll_get(cinc__worker *w) {
  afd_poll *p = w->polls;
  if (!p) return cilo_malloc(sizeof *p);
  w->polls = p->next;
  return p;
}

static void poll_put(cinc__worker *w, afd_poll *p) {
  p->next = w->polls;
  w->polls = p;
}

static ULONG events_of(uint8_t want) {
  ULONG e = AFD_POLL_ABORT | AFD_POLL_LOCAL_CLOSE | AFD_POLL_CONNECT_FAIL;
  if (want & CINC__R) e |= AFD_POLL_RECEIVE | AFD_POLL_DISCONNECT | AFD_POLL_ACCEPT;
  if (want & CINC__W) e |= AFD_POLL_SEND;
  return e;
}

// an error or a close wakes both sides, which then see it for themselves.
static uint8_t ready_of(ULONG e) {
  enum : ULONG { BOTH = AFD_POLL_ABORT | AFD_POLL_LOCAL_CLOSE | AFD_POLL_CONNECT_FAIL };
  uint8_t r = (e & (AFD_POLL_RECEIVE | AFD_POLL_DISCONNECT | AFD_POLL_ACCEPT | BOTH) ? CINC__R : 0) |
              (e & (AFD_POLL_SEND | BOTH) ? CINC__W : 0);
  return r ? r : CINC__R | CINC__W;
}

bool cinc__poller_init(cinc__worker *w) {
  cinc__net_init();
  w->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
  if (!w->iocp) return false;
  static const WCHAR name[] = L"\\Device\\Afd\\cinc";
  UNICODE_STRING path = {.Length = sizeof name - sizeof(WCHAR), .MaximumLength = sizeof name, .Buffer = (PWSTR)name};
  OBJECT_ATTRIBUTES attrs = {.Length = sizeof attrs, .ObjectName = &path};
  IO_STATUS_BLOCK iosb;
  if (NtCreateFile(&w->afd, SYNCHRONIZE, &attrs, &iosb, nullptr, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0,
                   nullptr, 0) < 0) {
    w->afd = nullptr;
    errno = ENODEV;
    return false;
  }
  if (!CreateIoCompletionPort(w->afd, w->iocp, 0, 0)) return false;
  SetFileCompletionNotificationModes(w->afd, FILE_SKIP_SET_EVENT_ON_HANDLE);
  // the clock ticks every 15.6 ms, this timer wakes within a millisecond. it never runs a completion routine, so its
  // expiry comes to the port as a packet. both are missing before windows 10 1803, waits then round to milliseconds.
  w->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (w->timer && NtCreateWaitCompletionPacket(&w->packet, GENERIC_ALL, nullptr) < 0) w->packet = nullptr;
  return true;
}

void cinc__poller_wake(cinc__worker *w) { PostQueuedCompletionStatus(w->iocp, 0, 0, nullptr); }

void cinc__poller_wait(cinc__worker *w, uint64_t timeout_ns) {
  OVERLAPPED_ENTRY evs[256];
  ULONG n = 0;
  DWORD ms = timeout_ns ? INFINITE : 0;
  bool armed = false;
  if (timeout_ns && timeout_ns != CINC__NEVER) {
    // relative and in 100 ns, rounded up so it never fires early.
    LARGE_INTEGER due = {.QuadPart = -(LONGLONG)(timeout_ns / 100 + 1)};
    BOOLEAN signaled = FALSE;
    armed = w->packet && SetWaitableTimer(w->timer, &due, 0, nullptr, nullptr, FALSE) &&
            NtAssociateWaitCompletionPacket(w->packet, w->iocp, w->timer, nullptr, nullptr, 0, 0, &signaled) >= 0;
    if (armed && signaled) ms = 0;
    else if (!armed && timeout_ns < (uint64_t)(INFINITE - 1) * 1'000'000u)
      ms = (DWORD)((timeout_ns + 999'999u) / 1'000'000u);
    else if (!armed) ms = INFINITE - 1;
  }
  BOOL got = GetQueuedCompletionStatusEx(w->iocp, evs, countof(evs), &n, ms, FALSE);
  // a packet left in the port would end the next wait early.
  if (armed) NtCancelWaitCompletionPacket(w->packet, TRUE);
  if (!got) return;
  for (ULONG i = 0; i < n; i++) {
    afd_poll *p = (afd_poll *)evs[i].lpOverlapped;
    // a wake up, or the timer.
    if (!p) continue;
    w->inflight--;
    if (!p->task) {
      poll_put(w, p);
      continue;
    }
    NTSTATUS st = p->iosb.Status;
    p->got = ready_of(st >= 0 && p->info.count ? p->info.handles[0].events : ~(ULONG)0);
    p->done = true;
    cinc__unpark(p->task);
  }
}

void cinc__poller_close(cinc__worker *w) {
  if (w->afd) {
    // a poll still in the kernel writes its answer: it comes back before its memory goes, or stays leaked.
    if (w->inflight) CancelIoEx(w->afd, nullptr);
    for (int tries = 0; w->inflight && tries < 100; tries++) cinc__poller_wait(w, 10'000'000);
    CloseHandle(w->afd);
  }
  for (afd_poll *p = w->polls, *next; p; p = next) {
    next = p->next;
    cilo_mfree(p);
  }
  if (w->packet) CloseHandle(w->packet);
  if (w->timer) CloseHandle(w->timer);
  if (w->iocp) CloseHandle(w->iocp);
}

static cinc_error wait_worker(cinc__thread *th, cinc_task *t, int fd, uint8_t want) {
  cinc__worker *w = th->worker;
  // past layered providers, to the socket afd knows.
  SOCKET base;
  DWORD bytes;
  if (WSAIoctl((SOCKET)(intptr_t)fd, SIO_BASE_HANDLE, nullptr, 0, &base, sizeof base, &bytes, nullptr, nullptr)) {
    int err = WSAGetLastError();
    // a crt descriptor: files are always ready.
    if (err == WSAENOTSOCK) return CINC_OK;
    errno = cinc__errno_of(err);
    return CINC_ERROR_IO;
  }
  afd_poll *p = poll_get(w);
  *p = (afd_poll){
      .iosb.Status = STATUS_PENDING,
      .info = {.timeout.QuadPart = INT64_MAX,
               .count = 1,
               .handles[0] = {.socket = (HANDLE)base, .events = events_of(want)}},
      .task = t,
  };
  NTSTATUS st = NtDeviceIoControlFile(w->afd, nullptr, nullptr, p, &p->iosb, IOCTL_AFD_POLL, &p->info, sizeof p->info,
                                      &p->info, sizeof p->info);
  // only an error leaves no completion behind.
  if ((ULONG)st >> 30 == 3) {
    poll_put(w, p);
    errno = EBADF;
    return CINC_ERROR_IO;
  }
  w->inflight++;
  w->io_waiters++;
  cinc_error result = CINC_OK;
  while (!p->done) {
    cinc__park(th, t, t->deadline);
    if (p->done) break;
    if ((result = cinc__interrupted(t))) break;
  }
  w->io_waiters--;
  if (p->done) {
    poll_put(w, p);
  } else {
    // its completion still comes, and puts it back.
    p->task = nullptr;
    CancelIoEx(w->afd, (OVERLAPPED *)&p->iosb);
  }
  return result;
}

static cinc_error wait_thread(cinc__thread *th, cinc_task *t, int fd, uint8_t want) {
  WSAPOLLFD p = {.fd = (SOCKET)(intptr_t)fd,
                 .events = (short)((want & CINC__R ? POLLRDNORM : 0) | (want & CINC__W ? POLLWRNORM : 0))};
  for (;;) {
    cinc_error e = cinc__interrupted(t);
    if (e) return e;
    int ms = -1;
    if (t->deadline != CINC__NEVER) {
      uint64_t now = cinc__now_ns();
      ms = now >= t->deadline ? 0 : (int)((t->deadline - now + 999'999u) / 1'000'000u);
    }
    // a task on the blocking pool looks at its cancellation now and then.
    if (th->mode == CINC__BLOCKING && (ms < 0 || ms > 100)) ms = 100;
    int n = WSAPoll(&p, 1, ms);
    int err = n < 0 ? WSAGetLastError() : n > 0 && p.revents & POLLNVAL ? WSAENOTSOCK : 0;
    if (!err) {
      if (n > 0) return CINC_OK;
      continue;
    }
    if (err == WSAEINTR) continue;
    // a crt descriptor, which is always ready, or a socket that is gone.
    SOCKET base;
    DWORD bytes;
    if (err == WSAENOTSOCK && WSAIoctl(p.fd, SIO_BASE_HANDLE, nullptr, 0, &base, sizeof base, &bytes, nullptr, nullptr))
      return CINC_OK;
    errno = err == WSAENOTSOCK ? EBADF : cinc__errno_of(err);
    return CINC_ERROR_IO;
  }
}

void cinc_close(int fd) {
  // its polls answer with AFD_POLL_LOCAL_CLOSE, which wakes their waiters on every worker.
  if (closesocket((SOCKET)(intptr_t)fd) && WSAGetLastError() == WSAENOTSOCK) _close(fd);
}

#else

/* ========================================================================================================== */
/* the per worker table of fds with waiters. registrations are one shot and redone on every wait, so a        */
/* descriptor closed behind cinc's back, as libpq does, never leaves a stale one.                             */
/* ========================================================================================================== */

static cinc__fdent *fd_entry(cinc__worker *w, int fd) {
  if (CILO_UNLIKELY((uint32_t)fd >= w->nfds)) {
    uint32_t n = w->nfds ? w->nfds : 64;
    while (n <= (uint32_t)fd) n *= 2;
    w->fds = cilo_mrealloc(w->fds, n * sizeof *w->fds);
    memset(w->fds + w->nfds, 0, (n - w->nfds) * sizeof *w->fds);
    w->nfds = n;
  }
  return &w->fds[fd];
}

static uint8_t fd_wanted(const cinc__fdent *e) {
  uint8_t want = 0;
  for (cinc__fdwait *n = e->waiters; n; n = n->next)
    if (!n->got) want |= n->want;
  return want;
}

#if CINC__KQUEUE

static void process(cinc__worker *w, const struct kevent *evs, int n);

static void change(cinc__worker *w, int fd, int16_t filter) {
  if (CILO_UNLIKELY(w->nchanges == (int)countof(w->changes))) {
    struct kevent evs[256];
    struct timespec zero = {};
    int got = kevent(w->pfd, w->changes, w->nchanges, evs, (int)countof(evs), &zero);
    w->nchanges = 0;
    if (got > 0) process(w, evs, got);
  }
  EV_SET(&w->changes[w->nchanges++], (uintptr_t)fd, filter, EV_ADD | EV_ONESHOT, 0, 0, nullptr);
}

// kqueue takes the registration with the next poll, which a parking task always reaches.
static int arm(cinc__worker *w, int fd, cinc__fdent *, uint8_t want, uint8_t added) {
  if (added & CINC__R) change(w, fd, EVFILT_READ);
  if (added & CINC__W) change(w, fd, EVFILT_WRITE);
  (void)want;
  return 0;
}

#else

static int arm(cinc__worker *w, int fd, cinc__fdent *e, uint8_t want, uint8_t) {
  struct epoll_event ev = {
      .events = EPOLLONESHOT | (want & CINC__R ? EPOLLIN | EPOLLRDHUP : 0u) | (want & CINC__W ? EPOLLOUT : 0u),
      .data.fd = fd,
  };
  int op = e->added ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if (epoll_ctl(w->pfd, op, fd, &ev) < 0) {
    if (errno == ENOENT) op = EPOLL_CTL_ADD;
    else if (errno == EEXIST) op = EPOLL_CTL_MOD;
    else return errno;
    if (epoll_ctl(w->pfd, op, fd, &ev) < 0) return errno;
  }
  e->added = true;
  return 0;
}

#endif

static void fd_ready(cinc__worker *w, int fd, uint8_t ready) {
  if (fd < 0 || (uint32_t)fd >= w->nfds) return;
  cinc__fdent *e = &w->fds[fd];
  uint8_t rest = 0;
  for (cinc__fdwait *n = e->waiters; n; n = n->next) {
    if (n->got) continue;
    if (n->want & ready) {
      n->got = n->want & ready;
      cinc__unpark(n->task);
    } else {
      rest |= n->want;
    }
  }
#if !CINC__KQUEUE
  // the one shot registration is spent for both directions, the other side registers again.
  if (rest && arm(w, fd, e, rest, rest)) fd_ready(w, fd, rest);
#else
  (void)rest;
#endif
}

/* ========================================================================================================== */
/* the poller.                                                                                                */
/* ========================================================================================================== */

#if CINC__KQUEUE

bool cinc__poller_init(cinc__worker *w) {
  w->efd = -1;
  w->pfd = kqueue();
  if (w->pfd < 0) return false;
  fcntl(w->pfd, F_SETFD, FD_CLOEXEC);
  struct kevent ev;
  EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  return kevent(w->pfd, &ev, 1, nullptr, 0, nullptr) == 0;
}

void cinc__poller_close(cinc__worker *w) { close(w->pfd); }

void cinc__poller_wake(cinc__worker *w) {
  struct kevent ev;
  EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
  kevent(w->pfd, &ev, 1, nullptr, 0, nullptr);
}

static void process(cinc__worker *w, const struct kevent *evs, int n) {
  for (int i = 0; i < n; i++) {
    const struct kevent *ev = &evs[i];
    if (ev->filter == EVFILT_USER) continue;
    uint8_t ready = ev->flags & EV_ERROR ? CINC__R | CINC__W : ev->filter == EVFILT_READ ? CINC__R : CINC__W;
    fd_ready(w, (int)ev->ident, ready);
  }
}

void cinc__poller_wait(cinc__worker *w, uint64_t timeout_ns) {
  struct kevent evs[256];
  struct timespec ts = {.tv_sec = (time_t)(timeout_ns / 1'000'000'000u),
                        .tv_nsec = (long)(timeout_ns % 1'000'000'000u)};
  int n = kevent(w->pfd, w->changes, w->nchanges, evs, (int)countof(evs), timeout_ns == CINC__NEVER ? nullptr : &ts);
  int lost = n < 0 && errno != EINTR && w->nchanges;
  w->nchanges = 0;
  if (n > 0) process(w, evs, n);
  if (lost)
    // the registrations may not have made it, everyone waiting tries again.
    for (uint32_t fd = 0; fd < w->nfds; fd++)
      if (w->fds[fd].waiters) fd_ready(w, (int)fd, CINC__R | CINC__W);
}

#else

bool cinc__poller_init(cinc__worker *w) {
  w->pfd = epoll_create1(EPOLL_CLOEXEC);
  if (w->pfd < 0) return false;
  w->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (w->efd < 0) return false;
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = w->efd};
  return epoll_ctl(w->pfd, EPOLL_CTL_ADD, w->efd, &ev) == 0;
}

void cinc__poller_close(cinc__worker *w) {
  close(w->efd);
  close(w->pfd);
}

void cinc__poller_wake(cinc__worker *w) {
  uint64_t one = 1;
  [[maybe_unused]] ssize_t n = write(w->efd, &one, sizeof one);
}

void cinc__poller_wait(cinc__worker *w, uint64_t timeout_ns) {
  struct epoll_event evs[256];
  // epoll counts in milliseconds, rounded up so a timer never fires early.
  int ms = -1;
  if (timeout_ns != CINC__NEVER)
    ms = timeout_ns >= (uint64_t)INT32_MAX * 1'000'000u ? INT32_MAX : (int)((timeout_ns + 999'999u) / 1'000'000u);
  int n = epoll_wait(w->pfd, evs, (int)countof(evs), ms);
  for (int i = 0; i < n; i++) {
    int fd = evs[i].data.fd;
    uint32_t e = evs[i].events;
    if (fd == w->efd) {
      uint64_t count;
      [[maybe_unused]] ssize_t r = read(w->efd, &count, sizeof count);
      continue;
    }
    uint8_t ready = (e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR) ? CINC__R : 0) |
                    (e & (EPOLLOUT | EPOLLHUP | EPOLLERR) ? CINC__W : 0);
    fd_ready(w, fd, ready);
  }
}

#endif

/* ========================================================================================================== */
/* waiting on a descriptor.                                                                                   */
/* ========================================================================================================== */

static cinc_error wait_worker(cinc__thread *th, cinc_task *t, int fd, uint8_t want) {
  cinc__worker *w = th->worker;
  cinc__fdwait n = {.task = t, .want = want};
  cinc__fdent *e = fd_entry(w, fd);
  uint8_t before = fd_wanted(e);
  n.next = e->waiters;
  if (e->waiters) e->waiters->prev = &n;
  e->waiters = &n;
  w->io_waiters++;
  defer {
    // the table may have moved while parked.
    cinc__fdent *entry = &w->fds[fd];
    if (n.prev) n.prev->next = n.next;
    else entry->waiters = n.next;
    if (n.next) n.next->prev = n.prev;
    w->io_waiters--;
  }
  // kqueue filters are separate, epoll registers the union of every waiter on the fd.
  int err = arm(w, fd, e, before | want, CINC__KQUEUE ? want : (uint8_t)(before | want));
  if (err) {
    // regular files cannot be polled and are always ready.
    errno = err;
    return err == EPERM ? CINC_OK : CINC_ERROR_IO;
  }
  for (;;) {
    cinc__park(th, t, t->deadline);
    if (n.got) return CINC_OK;
    cinc_error result = cinc__interrupted(t);
    if (result) return result;
  }
}

static cinc_error wait_thread(cinc__thread *th, cinc_task *t, int fd, uint8_t want) {
  struct pollfd p = {.fd = fd, .events = (short)((want & CINC__R ? POLLIN : 0) | (want & CINC__W ? POLLOUT : 0))};
  for (;;) {
    cinc_error e = cinc__interrupted(t);
    if (e) return e;
    int ms = -1;
    if (t->deadline != CINC__NEVER) {
      uint64_t now = cinc__now_ns();
      ms = now >= t->deadline ? 0 : (int)((t->deadline - now + 999'999u) / 1'000'000u);
    }
    // a task on the blocking pool looks at its cancellation now and then.
    if (th->mode == CINC__BLOCKING && (ms < 0 || ms > 100)) ms = 100;
    int n = poll(&p, 1, ms);
    if (n > 0) {
      if (p.revents & POLLNVAL) {
        errno = EBADF;
        return CINC_ERROR_IO;
      }
      return CINC_OK;
    }
    if (n < 0 && errno != EINTR) return CINC_ERROR_IO;
  }
}

void cinc_close(int fd) {
  cinc__thread *th = cinc__self();
  cinc__worker *w = th->worker;
  if (w && fd >= 0 && (uint32_t)fd < w->nfds) {
    // waiters wake up and see the closed descriptor for themselves.
    fd_ready(w, fd, CINC__R | CINC__W);
    w->fds[fd].added = false;
  }
  close(fd);
}

#endif

cinc_error cinc_wait_fd(int fd, cinc_io events) {
  cinc__thread *th = cinc__self();
  cinc_task *t = cinc__cur(th);
  cinc_error e = cinc__interrupted(t);
  if (e) return e;
  if (fd < 0) {
    errno = EBADF;
    return CINC_ERROR_IO;
  }
  return cinc__in_worker(th) ? wait_worker(th, t, fd, events) : wait_thread(th, t, fd, events);
}
