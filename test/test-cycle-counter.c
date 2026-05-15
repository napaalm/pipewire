/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano */
/* SPDX-License-Identifier: MIT */

/* Smoke tests for the per-thread perf_event-based cycle counter
 * helpers in src/pipewire/cycle-counter.h. The helpers are gated at
 * runtime on /proc/sys/kernel/perf_event_paranoid, so the test
 * adapts: when the kernel forbids the open it asserts the documented
 * "no perf data" semantics; when the open is allowed it verifies the
 * counter advances across an observable loop. */

#include "pwtest.h"

#include <pipewire/pipewire.h>
#include <pipewire/cycle-counter.h>

PWTEST(cycle_counter_supported_is_consistent)
{
	/* The supported() helper caches its first answer; a second call
	 * must agree with the first. The actual value is environment
	 * dependent, but the helper must be deterministic per process. */
	bool a = pw_cycle_counter_supported();
	bool b = pw_cycle_counter_supported();
	pwtest_int_eq((int)a, (int)b);
	return PWTEST_PASS;
}

PWTEST(cycle_counter_read_invalid_fd_is_zero)
{
	/* read() on a sentinel fd must return 0, the documented "no
	 * perf data" signal that consumers fall back on. */
	pwtest_int_eq((int)pw_cycle_counter_read(-1), 0);
	pwtest_int_eq((int)pw_cycle_counter_read(-2), 0);
	return PWTEST_PASS;
}

PWTEST(cycle_counter_open_close)
{
	int fd = pw_cycle_counter_open();
	if (!pw_cycle_counter_supported()) {
		/* When perf is disabled the open contract is "<0", and
		 * any fallback fd we already had stays unusable. */
		pwtest_int_lt(fd, 0);
		return PWTEST_PASS;
	}
	if (fd < 0) {
		/* Supported says yes, but a sandbox or seccomp policy
		 * still blocked it. Treat as a soft skip: the helper's
		 * <0 contract is what matters here. */
		return PWTEST_SKIP;
	}
	/* A fresh counter must read >= 0 (it's monotonic). */
	uint64_t v = pw_cycle_counter_read(fd);
	pwtest_int_ge((int64_t)v, 0);
	close(fd);
	return PWTEST_PASS;
}

PWTEST(cycle_counter_advances)
{
	int fd = pw_cycle_counter_open();
	if (fd < 0)
		return PWTEST_SKIP;

	uint64_t before = pw_cycle_counter_read(fd);
	/* Burn a few thousand cycles inline. The volatile sink stops
	 * the compiler from constant-folding the loop away. */
	volatile uint64_t sink = 0;
	for (int i = 0; i < 200000; i++)
		sink += i * 31u;
	(void)sink;
	uint64_t after = pw_cycle_counter_read(fd);

	close(fd);

	/* exclude_idle keeps the counter paused while the kernel parks
	 * the thread, so even a preempted run must show some forward
	 * progress: the loop above does real user-space work. */
	pwtest_int_gt((int64_t)(after - before), 0);
	return PWTEST_PASS;
}

PWTEST_SUITE(cycle_counter)
{
	pwtest_add(cycle_counter_supported_is_consistent, PWTEST_NOARG);
	pwtest_add(cycle_counter_read_invalid_fd_is_zero, PWTEST_NOARG);
	pwtest_add(cycle_counter_open_close, PWTEST_NOARG);
	pwtest_add(cycle_counter_advances, PWTEST_NOARG);
	return PWTEST_PASS;
}
