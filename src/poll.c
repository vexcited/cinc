#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdcountof.h>
#include <stddefer.h>
#include <string.h>
#include <unistd.h>

#if !CINC__KQUEUE
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif

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
