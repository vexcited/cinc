# cinc

*Everything in cinc.* A fiber runtime for C libraries, part of CiKo.

Tasks are plain C functions on their own stack, so concurrent code reads like blocking code. Every call that
waits parks the task inside a runtime and blocks the thread outside one: a library written against cinc has no
async "color" and works for both kinds of callers.

```c
#include <cinc/dsl.h>

double price_of(uint32_t product) {                    // a plain function
  cinc_sleep(100);                                     // parks the task, or blocks a plain thread
  return product * 1.5;
}
spawnable(price_of, uint32_t);                         // checked against the real signature

CINC_MAIN(.workers = 4) {
  double total = 0;
  within(400) {
    concurrently {                                     // no task outlives the block
      auto a = spawn(price_of(4));
      auto b = spawn(price_of(8));
      total = await(a) + await(b);
    }
  } else {
    return 1;                                          // the deadline expired
  }
  return 0;
}
```

| Go / tokio / Trio | cinc |
| --- | --- |
| `async fn` | any function, made spawnable with `spawnable(fn, param types...)` |
| `go f(x)`, `tokio::spawn`, `.await` | `spawn(f(x))`, `await(future)`, `cancel(future)` |
| Trio nursery, `JoinSet` | `concurrently { ... }`, `break` or `return` cancels the rest |
| `tokio::time::timeout` | `within(ms) { ... } else { ... }` |
| `spawn_blocking` | `blocking { ... }` |
| `make(chan T, n)`, `for v := range ch` | `typedef chan(T) Ts;`, `make_chan(Ts, n)`, `range(v, ch) { ... }` |
| `ch <- v`, `v := <-ch`, `close(ch)` | `chan_send(ch, v)`, `chan_recv(ch, &v)`, `chan_close(ch)` |
| `select { case v := <-a: ... case b <- x: ... }` | `switch (chan_select(&e, on_recv(a, &v), on_send(b, x))) { case 0: ... }` |
| `select` with a `default` case | `chan_try_select(&e, ...)`, -1 when no case is ready |
| `tokio::sync::Mutex` | `cinc_mutex` with `locked(&m) { ... }` |
| `tokio::sync::Semaphore` | `cinc_sem s = {.permits = 4};` with `acquired(&s) { ... } else { ... }` |
| `go f(x)` with a `sync.WaitGroup` | `spawn_in(&group, f(x))`, `cinc_group_wait`, `cinc_group_cancel` |
| `#[tokio::main]` | `CINC_MAIN(.workers = 4) { ... }` |
| `AsyncRead`, `TcpStream` | `cinc_wait_fd`, `cinc_read`, `cinc_write_all`, `cinc_connect` |
| `tokio::signal::ctrl_c` | `cinc_wait_shutdown(&signo)` |

`spawnable` never touches the function, so functions from other libraries can be spawned too. It takes up to 8
parameters, and futures of void functions await to nothing. Arguments are evaluated by the caller, like a call.

Timeouts and cancellation never unwind: once they fire, every waiting call returns `CINC_ERROR_TIMEOUT` or
`CINC_ERROR_CANCELLED`, and the code leaves the usual way, cleanups included. A task inherits the deadline it was
spawned under, and cancelling a task cancels what it spawned in its blocks.

Only a wait is interrupted. A channel send or receive, a select or a semaphore acquire that can go on right away does, in a cancelled task too, like a tokio future that is ready on its first poll. So a cancelled task still hands a permit back through a channel, rings a one slot channel used as a bell, or drains what is buffered, and `within(0) { ... }` turns any of them into a try. A loop that never waits checks `cinc_check()`.

A spawn outside any `concurrently` block is detached, like `go f(x)`: it runs until it returns and `cinc_run` waits
for it, but there is nothing to await. `spawn_in(&group, f(x))` detaches into a group that can be waited for or
cancelled, which is what an accept loop wants.

## Select and semaphores

```c
cinc_error e;
for (;;) {
  switch (chan_select(&e, on_recv(jobs, &job), on_recv(quit, &signo))) {
  case 0:
    if (e) return 0;                                   // jobs is closed and drained
    run(job);
    break;
  case 1:
    return 0;
  default:
    return e;                                          // cancelled or past the deadline
  }
}
```

A select takes up to 16 cases and runs one of those that can go on, at random so none starves, or waits for the first one. A case on a closed channel runs with `CINC_ERROR_CLOSED` in `e`, and a case on a null channel never runs, which turns it off like a nil channel in Go. Channels are locked in address order, so selects on overlapping channels never wait on each other, and a select on both ends of a rendezvous channel meets another select.

```c
static cinc_sem seats = {.permits = 4};               // at most four hashes at once

acquired(&seats) {
  cinc_blocking(hash, &job);
} else {
  return cinc_check();                                 // interrupted while waiting for a seat
}
```

`cinc_sem_release` never waits and never fails, and waiters get permits in the order they came.

## Memory

cinc allocates through [cilo](../ciko-cilo). Each task owns an arena, `cinc_arena()`, dropped when it returns.
Spawned frames are bumped from a second arena of the task, rewound when the block ends, so a spawn costs no malloc.
Both start on the page under the task's stack. Finished tasks give their stack and their chunks back to their
worker, so once warm the runtime maps nothing. Every task also gets its own set of scratch arenas, bound on each
switch, so a `scratch(tmp) { }` block may wait without another task on the same worker rewinding it.

## In a library

Use `<cinc/cinc.h>` and the `CINC_` names only, the lowercase ones in `<cinc/dsl.h>` are for applications and
`<cinc/dsl_end.h>` drops them. A driver waits with `cinc_wait_fd` or uses `cinc_read` and `cinc_write_all`, and the
same code serves a plain `main` and thousands of tasks, see `examples/03_colorless.c`. A library that only blocks,
like ciphon, is called from a `blocking { }` block.

```cmake
target_link_libraries(app PUBLIC cinc::cinc)
```

## Servers

```c
static void serve(int fd) { ... }                      // one task per connection
spawnable(serve, int);

static int accept_loop(int listener, cinc_group *conns) {
  for (uint32_t next = 0;; next++) {
    int fd;
    cinc_error e = cinc_accept(listener, &fd);         // CINC_ERROR_CANCELLED once main cancels it
    if (e) return e;
    spawn_in_on(conns, next, serve(fd));               // round robin over the workers
  }
}
spawnable(accept_loop, int, cinc_group *);

CINC_MAIN() {
  int listener, signo;
  cinc_group conns = {};
  if (cinc_listen("[::]:8080", false, &listener)) return 1;    // IPv6 and IPv4
  concurrently {
    auto acceptor = spawn(accept_loop(listener, &conns));
    cinc_wait_shutdown(&signo);                        // SIGINT or SIGTERM
    cancel(acceptor);
  }
  within(5000) {
    cinc_group_wait(&conns);                           // let requests in flight finish
  } else {
    cinc_group_cancel(&conns);
    cinc_group_wait(&conns);
  }
  cinc_close(listener);
  return 0;
}
```

Linux spreads connections over sockets that listen on the same port with `reuse_port`, so there a server listens
once per worker and runs one accept loop per worker, each spawning on its own worker with `spawn_in`. macOS hands
every connection to the same socket, so a server listens once and spreads connections itself as above.
`CINC_REUSEPORT_BALANCES` tells which, at compile time. `cinc_listen` takes `"0.0.0.0:8080"`, `"[::]:8080"`,
`":8080"` and names like `"localhost:8080"`. Sockets from `cinc_listen`, `cinc_accept` and `cinc_connect` are non
blocking, with `TCP_NODELAY` and without `SIGPIPE`.

## How it works

- **Workers.** One thread and one event loop each, kqueue on macOS and epoll on Linux, with a run queue, a timer heap
  and a lock free inbox. Other threads wake a worker through `EVFILT_USER` or an eventfd, and only when it sleeps: a
  worker that just ran something spins for 20 µs first. A task stays on its worker. Worker 0 is the thread that
  called `cinc_run`.
- **Switches.** Hand written for aarch64 and x86_64, Mach-O and ELF. They save the callee saved registers, d8 to d15
  on arm64, mxcsr and the x87 control word on x86_64. The first frame of every task has an undefined return
  address, so debuggers and backtraces stop there. Under ASan, switches are annotated for its fake stacks.
- **Stacks.** Mapped with a guard page and kept by the worker that ran the task, up to 1024 per worker.
- **Blocking.** `blocking { }` moves the task's stack to a pool thread and back, and `cinc_blocking(fn, arg)` runs
  `fn` on the pool thread's own stack, for libraries with deep stacks. Names resolve there too.
- **Waits.** Channels, selects, mutexes, semaphores, joins and groups queue their waiters behind a short lock, which a plain thread uses the same way through a condition variable. A select queues one waiter per case and the first waker to claim it wins. A plain thread waits with `poll` and condition variables, and spawns run inline.

`blocking { }` moves a stack between threads, and compilers merge calls to functions declared `const`, like glibc's
`errno` and `pthread_self`, across its edges. Read those in a function the block calls, or use `cinc_blocking`.

## Building and testing

C23 with clang 19 or later, Apple clang 21, or gcc 14, on macOS and Linux, arm64 and x86_64.

```sh
cmake -S . -B build -G Ninja -DFETCHCONTENT_SOURCE_DIR_CILO=../ciko-cilo
ninja -C build && ctest --test-dir build
./build/cinc_bench

cmake --preset asan -DFETCHCONTENT_SOURCE_DIR_CILO=../ciko-cilo  # or -DCINC_SANITIZE="address;undefined"
cmake --build --preset asan && ctest --preset asan
```

The tests cover spawns and awaits, nested blocks left early, deadlines and cancellation, channels, selects, mutexes and semaphores across workers and plain threads, the blocking pool, a TCP echo server with 120 concurrent clients, sleep accuracy, stack reuse, private scratch arenas and every call outside a runtime. The examples run as tests too.

## Performance

`cinc_bench`, nanoseconds per operation, four workers, the best of three runs on an Apple M4 Pro.

| operation | macOS 27 | Linux 6 arm64, Docker |
| --- | --- | --- |
| spawn and await, one at a time | 112 | 123 |
| spawn and join, 1000 in a block | 55 | 56 |
| spawn and join, 1000 over four workers | 221 | 236 |
| context switch, task to scheduler | 19.6 | 19.0 |
| `cinc_yield` between two tasks | 39 | 38 |
| rendezvous channel ping pong, one worker | 129 | 141 |
| rendezvous channel ping pong, two workers | 698 | 1000 |
| buffered channel, producer on another worker | 47 | 46 |
| mutex lock and unlock, uncontended | 2.1 | 2.8 |

A ping pong across workers wakes the other side twice per round trip. While the other worker spins, a wake up is a
few cache line transfers and no system call. One that already slept takes a few microseconds of kernel wake up.
