/*
 * fiber_canary_test.c — E3 check for heap-stack fibers (Switch, and the Mac
 * Switch-stub build): the stack comes from memalign/posix_memalign, the guard
 * region below it is a canary, fiber_guard_intact() notices a write into it,
 * and fiber_stack_high_water() measures use. Built with ARCUS_FAKE_SWITCH=ON.
 */
#include "fiber_compat.h"

#include <stdio.h>
#include <string.h>

static fiber_t s_main, s_fib;
static volatile int s_step;

static volatile int s_sink;

static void __attribute__((noinline)) use_stack(int depth)
{
    volatile char buf[1024];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (char)depth;
    if (depth > 0) use_stack(depth - 1);
    s_sink += buf[depth];                /* not a tail call */
}

static void entry(void *arg)
{
    (void)arg;
    use_stack(40);                       /* ~40 KB deep */
    s_step = 1;
    fiber_switch(s_main);
    uintptr_t lo = 0, top = 0;
    fiber_stack_range(s_fib, &lo, &top);
    *(volatile unsigned char *)(lo - 64) = 0;   /* what an overflow would do */
    s_step = 2;
    for (;;) fiber_switch(s_main);
}

int main(void)
{
    int fails = 0;
    s_main = fiber_convert_thread();
    s_fib = fiber_create(1u << 20, 1u << 20, entry, NULL);
    if (!s_fib) { puts("FAIL fiber_create"); return 1; }
    uintptr_t lo = 0, top = 0;
    fiber_stack_range(s_fib, &lo, &top);
    printf("stack %zu bytes, 16-byte aligned top: %s\n", (size_t)(top - lo), top % 16 ? "no" : "yes");
    fails += (top - lo) != (1u << 20) || top % 16;

    fiber_switch(s_fib);
    size_t hw = fiber_stack_high_water(s_fib);
    int intact = fiber_guard_intact(s_fib);
    printf("after 40 KB of use: high water %zu, guard %s\n", hw, intact ? "intact" : "overwritten");
    fails += !(s_step == 1 && hw >= 40 * 1024 && hw < 64 * 1024 && intact);

    fiber_switch(s_fib);
    intact = fiber_guard_intact(s_fib);
    printf("after a write below the stack: guard %s\n", intact ? "intact" : "overwritten");
    fails += !(s_step == 2 && !intact);

    fiber_destroy(s_fib);
    fiber_revert_thread();
    puts(fails ? "FAIL" : "PASS");
    return fails != 0;
}
