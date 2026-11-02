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
| `tokio::sync::Mutex` | `cinc_mutex` with `locked(&m) { ... }` |
| `#[tokio::main]` | `CINC_MAIN(.workers = 4) { ... }` |
| `AsyncRead`, `TcpStream` | `cinc_wait_fd`, `cinc_read`, `cinc_write_all`, `cinc_connect` |

`spawnable` never touches the function, so functions from other libraries can be spawned too. It takes up to 8
parameters, and futures of void functions await to nothing. Arguments are evaluated by the caller, like a call.

Timeouts and cancellation never unwind: once they fire, every waiting call returns `CINC_ERROR_TIMEOUT` or
`CINC_ERROR_CANCELLED`, and the code leaves the usual way, cleanups included.

## Memory

cinc allocates through [cilo](../ciko-cilo). Each task owns an arena, `cinc_arena()`, dropped when it returns,
and spawned frames are bumped from it, so a spawn costs no malloc. Finished tasks give their stack and their
chunks back to their worker, so once warm the runtime maps nothing. Every task also gets its own pair of
scratch arenas, bound on each switch, so a `scratch(tmp) { }` block may wait without another task on the same
worker rewinding it.

## In a library

Use `<cinc/cinc.h>` and the `CINC_` names only, the lowercase ones in `<cinc/dsl.h>` are for applications and
`<cinc/dsl_end.h>` drops them. A driver waits with `cinc_wait_fd` or uses `cinc_read` and `cinc_write_all`, and the
same code serves a plain `main` and thousands of tasks, see `examples/03_colorless.c`. A library that only blocks,
like ciphon, is called from a `blocking { }` block.

```cmake
target_link_libraries(app PUBLIC cinc::cinc)
```

## Status

A draft: the headers and examples compile, the runtime (scheduler, context switches, poller) is not written yet.

```sh
cmake -S . -B build -G Ninja -DFETCHCONTENT_SOURCE_DIR_CILO=../ciko-cilo
ninja -C build
```
