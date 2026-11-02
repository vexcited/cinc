#include <cinc/dsl.h>
#include <stdio.h>

typedef struct Cart {
  uint32_t products[3];
} Cart;

// a pretend database lookup, nothing about it is async.
static double price_of(uint32_t product) {
  if (cinc_sleep(100 + product % 7 * 50))
    return 0;
  return product * 1.5;
}
spawnable(price_of, uint32_t);

static void audit(const char *what, double amount) {
  cinc_sleep(10);
  printf("audit: %s %.2f\n", what, amount);
}
spawnable(audit, const char *, double);

// spawns its own tasks: concurrently waits for them, so none outlives the function.
static double cart_total(Cart cart) {
  double total = 0;
  concurrently {
    future(price_of) prices[3];
    for (int i = 0; i < 3; i++)
      prices[i] = spawn(price_of(cart.products[i]));
    for (int i = 0; i < 3; i++)
      total += await(prices[i]);
    // not awaited, the end of the block waits for it.
    spawn(audit("cart", total));
  }
  return total;
}

CINC_MAIN(.workers = 2) {
  double total = 0;

  // the lookups overlap, so this takes the slowest one rather than their sum.
  within(400) {
    total = cart_total((Cart){.products = {4, 8, 15}});
  } else {
    puts("too slow, every pending lookup got CINC_ERROR_TIMEOUT");
    return 1;
  }
  printf("total %.2f\n", total);

  // the first answer wins: breaking out of concurrently cancels the others.
  concurrently {
    auto fast = spawn(price_of(7));
    auto slow = spawn(price_of(6));
    printf("fastest %.2f\n", await(fast));
    cancel(slow);
  }

  // cpu work goes to the blocking pool, the worker keeps running other tasks meanwhile.
  uint64_t h = 1469598103934665603ull;
  blocking {
    for (int round = 0; round < 1'000'000; round++)
      h = (h ^ (uint64_t)round) * 1099511628211ull;
  }
  printf("hash %llu\n", (unsigned long long)h);
  return 0;
}
