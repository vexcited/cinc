#include <cilo/dsl.h>
#include <cinc/dsl.h>
#include <stdio.h>

typedef struct Job {
  uint32_t id;
  const char *path;
} Job;

typedef chan(Job) Jobs;

typedef struct Stats {
  cinc_mutex lock;
  uint32_t done;
  uint64_t bytes;
} Stats;

static long file_size(const char *path) {
  long size = 0;
  // file systems have no readiness to wait on, so file io goes to the blocking pool.
  blocking {
    FILE *f = fopen(path, "rb");
    if (f && fseek(f, 0, SEEK_END) == 0)
      size = ftell(f);
    if (f)
      fclose(f);
  }
  return size;
}

static cinc_error consume(Jobs jobs, Stats *stats) {
  range(job, jobs) {
    // scratch arenas belong to the task, not the thread, so this block may wait in file_size while another task
    // on the same worker uses its own.
    scratch(tmp) {
      char *label = aprintf(tmp, "#%u %s", job.id, job.path);
      long size = file_size(job.path);
      // consumers run on different workers, so the shared stats need the lock.
      locked(&stats->lock) {
        stats->done++;
        stats->bytes += size > 0 ? (uint64_t)size : 0;
      }
      printf("%s: %ld bytes\n", label, size);
    }
  }
  // the channel was closed and drained, or we were cancelled.
  return cinc_check();
}
spawnable(consume, Jobs, Stats *);

CINC_MAIN(.workers = 4) {
  static const char *const paths[] = {"/etc/hosts", "/etc/passwd", "/etc/shells", "/etc/protocols"};
  Stats stats = {};
  Jobs jobs = make_chan(Jobs, 16);

  concurrently {
    for (uint32_t w = 0; w < cinc_workers(); w++)
      spawn_on(w, consume(jobs, &stats));
    for (uint32_t i = 0; i < sizeof paths / sizeof *paths; i++)
      if (chan_send(jobs, (Job){.id = i, .path = paths[i]}))
        break;
    // the consumers drain what is left and return, which ends the block.
    chan_close(jobs);
  }

  chan_free(jobs);
  printf("%u files, %llu bytes\n", stats.done, (unsigned long long)stats.bytes);
  return 0;
}
