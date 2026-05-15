/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_CYCLE_COUNTER_H
#define PIPEWIRE_CYCLE_COUNTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#if defined(__linux__)
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#endif

#include <pipewire/log.h>
#include <spa/utils/defs.h>

/* Per-thread perf_event-based CPU-cycle counter, shared between the
 * daemon's process_node() path and the pipewire-jack client cycle path.
 *
 * The counter binds to the calling thread (PERF_COUNT_HW_CPU_CYCLES,
 * pid=0, exclude_kernel/exclude_hv/exclude_idle = 1). A delta read
 * across one processing cycle is then exactly the cycles that thread
 * spent doing its own work -- a frequency-invariant work estimate
 * that complements the wall-clock CLOCK_THREAD_CPUTIME_ID measurement
 * the same code path already records.
 *
 * Helpers must be called from the thread we want to measure
 * (perf_event_open with pid=0 attaches to the caller). */

/* Cached check of /proc/sys/kernel/perf_event_paranoid. Returns true
 * when the kernel allows the unprivileged perf_event_open() this code
 * uses. Threshold is "<= 1":
 *
 *   3   no perf_event_open() at all
 *   2   user can profile user space but not kernel; per-CPU events refused
 *   1   user can profile both user and kernel space
 *   0   per-CPU events allowed
 *  -1   raw access allowed
 *
 * On a host pinned at 2 the open will usually still succeed for
 * thread-attached events with exclude_kernel set, but distros vary;
 * gating on paranoid <= 1 is the safe, explicit contract. The result
 * is cached for the lifetime of the process. */
static inline bool pw_cycle_counter_supported(void)
{
#if defined(__linux__)
	static int cached = -1;
	if (SPA_LIKELY(cached != -1))
		return cached != 0;
	FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
	if (f == NULL) {
		cached = 0;
		return false;
	}
	int paranoid = 99;
	int n = fscanf(f, "%d", &paranoid);
	fclose(f);
	cached = (n == 1 && paranoid <= 1) ? 1 : 0;
	pw_log_info("perf_event_paranoid=%d cycles_supported=%s",
			paranoid, cached ? "true" : "false");
	return cached != 0;
#else
	return false;
#endif
}

/* Open a thread-attached PERF_COUNT_HW_CPU_CYCLES counter excluding
 * kernel / hypervisor / idle time. Returns the fd on success, -1 on
 * failure. Must be called from the thread we want to measure. */
static inline int pw_cycle_counter_open(void)
{
#if defined(__linux__)
	struct perf_event_attr attr;
	int fd;

	if (!pw_cycle_counter_supported())
		return -1;

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.type = PERF_TYPE_HARDWARE;
	attr.config = PERF_COUNT_HW_CPU_CYCLES;
	attr.disabled = 0;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	attr.exclude_idle = 1;
	/* pid=0 means "this thread", cpu=-1 means "any CPU". */
	fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
	if (fd < 0) {
		pw_log_debug("perf_event_open cycles failed: %m");
		return -1;
	}
	return fd;
#else
	return -1;
#endif
}

/* Read the cumulative cycle count from the perf_event_open fd. On any
 * failure return 0; consumers must treat zero as "no perf data" and
 * fall back to the wall-clock prev_run_time estimate. */
static inline uint64_t pw_cycle_counter_read(int fd)
{
	uint64_t v = 0;
	if (fd < 0)
		return 0;
	if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
		return 0;
	return v;
}

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_CYCLE_COUNTER_H */
