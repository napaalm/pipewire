/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */
/***
  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
***/

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <stdbool.h>
#include <sched.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <linux/sched.h>
#include <pthread.h>

#include "config.h"

#include "module-deadline/dag.h"
#include "module-deadline/wcet_sketch.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/atomic.h>

#include <pipewire/private.h>
#include <pipewire/impl.h>
#include <pipewire/thread.h>
#include <pipewire/thread-loop.h>

/** \page page_module_deadline Deadline
 *
 * The `deadline` module enables the use of the SCHED_DEADLINE scheduling policy
 * on processing threads of nodes in the graph.
 *
 * The module determines dynamic parameters for the SCHED_DEADLINE policy by
 * analyzing the graph of nodes in the context. It calculates the worst-case
 * execution time (WCET) of each node and the period of the processing thread.
 * It then uses some heuristics to determine the runtime and deadline of each
 * thread, based on the WCETs and the graph latency constraints.
 * It also distributes the threads across the available CPUs, using a worst-fit
 * strategy.
 *
 * Note: This module depends on the context.dynamic-data-loops property. If the
 * property is not set on all configurations, the module will refuse to work.
 * Also, to use the SCHED_DEADLINE policy, the pipewire executable must be given
 * the CAP_SYS_NICE capability.
 *
 * ## Module Name
 *
 * `libpipewire-module-deadline`
 *
 * ## Module Options
 *
 * - `cpus.available`: The list of CPUs to which the threads are bound.
 * - `cpus.utilization`: The maximum CPU utilization (per core) that DEADLINE
 *                       threads are allowed to consume. The default is 0.95.
 * - `wcet.window-size`: Number of recent driver-completion cycles whose
 *                       runtime samples are retained per node. Counts
 *                       cycles, not time -- the sketch sees exactly one
 *                       sample per cycle so the same value behaves
 *                       consistently across rates and quanta. The
 *                       effective window oscillates between window-size/2
 *                       and window-size cycles because of the two-digest
 *                       rotation. Larger windows produce more stable
 *                       budgets but adapt more slowly to workload changes
 *                       (track switch, filter parameter change); smaller
 *                       windows track changes faster but are noisier.
 *                       Default 512.
 * - `wcet.quantile`:    The tail quantile of the per-node runtime
 *                       distribution reported as the SCHED_DEADLINE
 *                       runtime budget. Must lie strictly in (0, 1).
 *                       0.999 means roughly one budget overrun per 1000
 *                       cycles in steady state; SCHED_FLAG_RECLAIM (GRUB)
 *                       absorbs occasional overruns on graphs that are
 *                       not saturated. Note the coupling with
 *                       wcet.window-size: any q such that
 *                       (1 - q) * window-size < 1 reports approximately
 *                       the maximum observed sample, so to extract a
 *                       quantile distinct from peak-hold the window must
 *                       grow proportionally with the tightness of q.
 *                       Default 0.999.
 * - `wcet.compression`: t-digest compression parameter delta -- accuracy
 *                       vs. memory trade-off. Higher gives more
 *                       centroids and finer tail estimation. Per Dunning
 *                       & Ertl (arXiv:1902.04023, Figure 8) absolute
 *                       quantile error scales as 1/delta^2 below the
 *                       knee (delta around 100-200 for tail quantiles)
 *                       and as 1/sqrt(delta) above; 100 sits at the
 *                       knee and is the value the paper recommends for
 *                       general use. Memory footprint at delta=100 is
 *                       roughly 11 kB per node sketch. Default 100.
 * - `wcet.min-samples`: Bootstrap threshold. Until this many samples have
 *                       been observed for a node the module falls back
 *                       to plain peak-hold (max of recent runtimes)
 *                       instead of querying the sketch quantile, which
 *                       protects against a sketch quantile collapsing
 *                       the budget from a few early atypical samples.
 *                       Default 32.
 * - `recalc.sync`:      If true (default false), run the parameter
 *                       recalculation synchronously on the driver's RT
 *                       data-loop thread, as the module did before the
 *                       async refactor. Useful only for benchmarking
 *                       the cost the async path removes from the RT
 *                       path. Production should leave it false.
 * - `recalc.rt-prio`:   SCHED_FIFO priority of the async recalculation
 *                       worker thread. Must be strictly lower than the
 *                       audio processing priority (PipeWire defaults
 *                       client=83, server=88; min configurable=11), so
 *                       the worker never preempts audio. The worker is
 *                       also pinned away from the deadline cores
 *                       (cpus.available) so it never shares CPU time
 *                       with the SCHED_DEADLINE threads it configures.
 *                       Default 5.
 * - `recalc.fake-delay-us`: Debug knob. If non-zero, the worker sleeps
 *                       this many microseconds inside each recalc
 *                       callback to simulate a slow analysis stage and
 *                       exercise the CAS-coalesced "many RT cycles per
 *                       worker wake" path. Default 0.
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-deadline
 *     args = {
 *         cpus.available   = [ 0 1 2 3 ]
 *         cpus.utilization = 0.95
 *     }
 *     flags = [ ifexists nofail ]
 * }
 * ]
 *\endcode
 */

#define NAME "deadline"

#define MAX_CPUS 128

/* ---------------------------------------------------------------------
 * WCET-sketch defaults
 *
 * All four knobs below are rate- and quantum-independent. The sketch
 * receives exactly one sample per driver-completion cycle, so where
 * the text says "samples" it always means cycles of the audio graph,
 * not seconds. The same default values therefore behave consistently
 * whether the graph runs at 8-frame quanta at 48 kHz or at 1024-frame
 * quanta at 96 kHz.
 *
 * The choices were re-evaluated against the design discussion of
 * Dunning & Ertl, arXiv:1902.04023 (the paper this sketch implements),
 * and Cucinotta & Palopoli, "QoS Management Through Adaptive
 * Reservations", Real-Time Systems 41(1), 2009.
 * --------------------------------------------------------------------- */

/* Sliding-window length, in graph cycles.
 *
 * The sketch keeps the most recent ~window-size cycles' runtime
 * samples and reports the configured quantile over them. Two t-digests
 * alternate: incoming samples go into "cur"; once cur has window-size/2
 * samples we rotate (prev <- cur, cur empty). Queries answer over
 * prev union cur, so the effective window oscillates between
 * window-size/2 (just after a rotation) and window-size (just before
 * the next).
 *
 * Smaller window: the quantile estimate has higher variance, the
 *   SCHED_DEADLINE budget can oscillate from cycle to cycle, and brief
 *   stationarity windows in the input are less well represented.
 * Larger window: smoother budget, but slower to react to genuine
 *   workload shifts (audio source change, filter reconfiguration).
 *
 * 512 cycles is roughly 85 ms at 48 kHz with an 8-frame quantum and
 * about 10 s at the same rate with a 1024-frame quantum; in both
 * regimes it captures enough samples for the tail estimate to be
 * stable without lagging structural workload changes that matter for
 * audio (those occur on hundreds-of-ms to seconds timescales). */
#define WCET_DEFAULT_WINDOW_SIZE	512u

/* Tail quantile reported as the WCET budget.
 *
 * The sketch returns the q-quantile of the runtime samples it holds;
 * module-deadline uses that value (with the existing *1.05 safety
 * margin from the pre-existing code) as the SCHED_DEADLINE runtime
 * budget.
 *
 * Lower q: tighter budget, more frequent overruns. At 0.99 we expect
 *   one overrun every 100 cycles, audible as xruns under load.
 * Higher q: looser budget, approaches peak-hold and becomes wasteful
 *   of CPU reservation.
 *
 * 0.999 yields roughly one expected overrun per 1000 cycles. With
 * SCHED_FLAG_RECLAIM enabled (we already set it in set_deadline_sched)
 * the kernel's GRUB mechanism reclaims slack from other deadline tasks
 * to cover such overruns on non-saturated graphs (Abeni, Lelli,
 * Scordino, Palopoli, "Greedy CPU Reclaiming for SCHED_DEADLINE",
 * RTLWS 2014; Lelli, Scordino, Abeni, Faggioli, "Deadline scheduling
 * in the Linux kernel", SP&E 46(6), 2016).
 *
 * The coupling between window-size and quantile matters. For the
 * sketch to return a tail value distinct from the observed maximum
 * we need at least one sample above q in the window, i.e. roughly
 * (1 - q) * window-size >= 1. With window=512:
 *   q = 0.99   -> expected 5 samples above the threshold (meaningful
 *                 tail estimate)
 *   q = 0.999  -> expected 0.5 samples above (the sketch effectively
 *                 returns the max-observed, interpolated)
 *   q = 0.9999 -> the sketch is functionally peak-hold within this
 *                 window
 * 0.999 is chosen as the practical sweet spot: tight enough that the
 * sketch tracks tail movement, loose enough that audible overruns are
 * rare and absorbable by GRUB. Tightening past 0.999 only helps if
 * the window is also enlarged. */
#define WCET_DEFAULT_QUANTILE		0.999

/* t-digest compression parameter (delta in the paper, eq. 3 / 7).
 *
 * Bounds the number of centroids the digest retains -- at most
 * ceil(delta) clusters in a fully merged digest. Higher delta gives
 * more centroids, finer tail resolution and proportionally more memory.
 *
 * Per Figure 8 of the paper, mean absolute quantile error decays like
 *   1 / delta^2     for delta below the knee (delta ~50-200 for the
 *                   tail quantiles we care about)
 *   1 / sqrt(delta) for delta above the knee
 * 100 sits at the knee and is the value Dunning & Ertl recommend as a
 * general-purpose default. Going to 200 would buy roughly 4x lower
 * tail error at 2x the memory; below 50 errors grow noticeably.
 *
 * Memory at delta=100, per node:
 *   2 digests * (101 centroid_t @ 16 B + 505 buffer slot @ 8 B)
 *   ~= 11 kB. For 30 nodes in a graph, ~330 kB total -- negligible. */
#define WCET_DEFAULT_COMPRESSION	100.0

/* Async worker defaults. */

/* SCHED_FIFO priority of the worker thread. Strictly lower than every
 * audio-processing thread, so the worker cannot preempt audio. The
 * meson options rtprio-server/rtprio-client clamp PipeWire's audio
 * threads to >=11; we pick 5 to leave clear headroom on both sides
 * (>0 means SCHED_FIFO at all; <11 means audio threads always win). */
#define WORKER_DEFAULT_RT_PRIO		5

/* Per-driver SPSC sample ring capacity, in slots of struct sample.
 * Power of two for cheap modulo. Sized for the case where the worker
 * is a few periods behind the RT thread: at 48 kHz with an 8-frame
 * quantum every cycle emits ~30 samples; 4096 slots = ~130 cycles
 * worth, well beyond any plausible worker lag. Overflow policy is
 * drop-oldest (we just stop writing), which the WCET sketch absorbs
 * naturally. */
#define WORKER_RING_CAPACITY		4096u

/* Maximum nodes/edges a topology snapshot can hold. The snapshot
 * lives on the driver sentinel; arrays are realloc()d at snapshot
 * time on the main loop if needed. Initial value is just the starting
 * allocation. */
#define TOPO_INITIAL_NODES		64u
#define TOPO_INITIAL_EDGES		128u

/* Bootstrap threshold, in cycles.
 *
 * Below this number of samples the module ignores the sketch quantile
 * and falls back to plain peak-hold (n->wcet = max(n->wcet, runtime)),
 * matching the pre-existing behaviour. Above the threshold the sketch
 * quantile takes over.
 *
 * The rationale is conservatism on cold start: with very few samples
 * the empirical distribution is not representative of steady state,
 * and a high-quantile query on a thin tail can return a value that is
 * unrealistically small. Peak-hold guarantees we never under-budget
 * relative to anything we have already observed.
 *
 * 32 is short -- ~5 ms at 48 kHz with an 8-frame quantum -- so the
 * sketch takes over quickly. Increasing this makes startup more
 * conservative (and the initial reservation larger) without changing
 * steady-state behaviour. */
#define WCET_DEFAULT_MIN_SAMPLES	32u

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	"( cpu.available=<list of CPUs> ) "	\
			"( cpu.utilization=<percentage> ) "

/* PipeWire does not assume glibc. Detect the scheduler ABI pieces at
 * configure time and provide only the missing Linux fallbacks here.
 *
 * Before libc headers exposed struct sched_attr, the layout came from the
 * Linux sched_setattr(2) kernel ABI.
 */
#if !HAVE_STRUCT_SCHED_ATTR
struct sched_attr {
	uint32_t size;
	uint32_t sched_policy;
	uint64_t sched_flags;
	int32_t sched_nice;
	uint32_t sched_priority;
	uint64_t sched_runtime;
	uint64_t sched_deadline;
	uint64_t sched_period;
	uint32_t sched_util_min;
	uint32_t sched_util_max;
};
#endif

#if !HAVE_SCHED_SETATTR
static int sched_setattr(pid_t tid, struct sched_attr *attr, unsigned int flags)
{
	return syscall(SYS_sched_setattr, tid, attr, flags);
}
#endif

#if !HAVE_SCHED_GETATTR
static SPA_UNUSED int sched_getattr(pid_t tid, struct sched_attr *attr,
		unsigned int size, unsigned int flags)
{
	return syscall(SYS_sched_getattr, tid, attr, size, flags);
}
#endif

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Antonio Napolitano <antonio.napolitano@santannapisa.it> and Francesco Barcherini <francesco.barcherini@santannapisa.it>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Use SCHED_DEADLINE for processing threads" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

/* A single RT->worker sample. node_id+period_ns are enough for the
 * worker to look up (or lazily create) the node estimator and to
 * detect period changes. The RT thread writes; the worker reads. */
struct sample {
	uint32_t node_id;
	uint32_t _pad;
	uint64_t runtime_ns;
	uint64_t period_ns;
};

/* Topology snapshot for one driver. Populated only on the main loop
 * (inside snapshot_topology() dispatched via pw_loop_invoke); read
 * only by the worker, between the moment the main-loop callback
 * returns and the moment we issue the next snapshot request. No
 * other reader exists, so no locking. */
struct topo_node {
	uint32_t id;
	pid_t    tid;
};
struct topo_edge {
	uint32_t src;
	uint32_t dst;
};
struct topo_snap {
	struct topo_node *nodes;
	struct topo_edge *edges;
	uint32_t n_nodes, nodes_cap;
	uint32_t n_edges, edges_cap;
	uint64_t period;
	bool     ok;
	uint32_t pending;  /* atomic, CAS-coalesces async snapshot invokes */
};

struct node {
	struct spa_list link;
	struct impl *impl;

	struct pw_impl_node *node;        /* may be NULL on worker-created
					   * follower entries; lookup by id
					   * goes through `node_id` below. */
	uint32_t node_id;                 /* always set; mirrors node->info.id
					   * when `node` is known. */
	struct spa_hook node_rt_listener;

	bool enabled:1;
	bool is_driver:1;

	/* Per-node WCET estimator. Only meaningful for follower nodes
	 * (is_driver=false); the driver sentinel keeps zeros. */
	uint64_t wcet;
	uint64_t period;
	wcet_sketch_t sketch;
	bool sketch_ready;

	/* Per-driver async state. Only valid when is_driver=true. */
	/* SPSC sample ring: producer is this driver's data-loop thread
	 * (inside the complete/incomplete RT hook), consumer is the
	 * impl->worker thread. */
	struct spa_ringbuffer ring;          /* byte indices into ring_slots */
	struct sample        *ring_slots;
	uint32_t              ring_capacity; /* count of slots */
	uint64_t              ring_dropped;  /* monotonic; written by RT */

	/* Coalescing flag: RT CAS 0->1 before signaling worker; worker
	 * CAS->0 at start of its callback. Atomic. */
	uint32_t              recalc_inflight;

	/* Most recent topology snapshot of the targets scheduled by
	 * this driver. Filled by the main-loop callback; consumed by
	 * the worker. */
	struct topo_snap      topo;

	/* Per-driver RT-hook timing histogram. Log2-bucket of CPU-time
	 * nanoseconds spent in the RT hook body. Worker reads at
	 * destroy; RT writer otherwise has exclusive access. */
	uint64_t              hist_buckets[32];
	uint64_t              hist_count;
	uint64_t              hist_sum_ns;
	uint64_t              hist_min_ns;
	uint64_t              hist_max_ns;

	/* Per-driver worker per-wake timing histogram. Same log2 shape
	 * as the RT-hook histogram so a live A/B summary can interleave
	 * the two tables. Sampled around the body of worker_recalc_one;
	 * the producer is the deadline-recalc worker thread, which is
	 * also the only reader except at driver_removed / module_destroy.
	 * Untouched in sync mode (where the worker is not used). */
	uint64_t              wk_hist_buckets[32];
	uint64_t              wk_hist_count;
	uint64_t              wk_hist_sum_ns;
	uint64_t              wk_hist_min_ns;
	uint64_t              wk_hist_max_ns;
};

struct impl {
	struct pw_context *context;
	struct pw_properties *props;

	struct pw_impl_module *module;

	struct spa_hook module_listener;
	struct spa_hook context_listener;

	int n_cpus;
	int cpus[MAX_CPUS];
	float cpu_utilization;

	/* WCET estimator configuration; see module-options doc above. */
	uint32_t sketch_window_size;
	double   sketch_quantile;
	double   sketch_compression;
	uint32_t sketch_min_samples;

	/* Async worker. Created at init when deadline policy is available;
	 * NULL when sync_mode is true. */
	bool                  sync_mode;
	int                   worker_rt_prio;
	uint32_t              worker_fake_delay_us;
	struct pw_thread_loop *worker_tloop;
	struct pw_loop       *worker_loop;
	struct pw_loop       *main_loop;
	/* Worker wake-up is an eventfd source signaled by the RT hook
	 * with a single eventfd_write (sub-microsecond cross-CPU). The
	 * source must be added before pw_thread_loop_start (see init):
	 * sources added from inside a worker invoke proved unreliable
	 * on this host -- the worker's epoll set did not observe the
	 * newly-registered fd. */
	struct spa_source    *worker_wake;
	cpu_set_t             worker_affinity; /* online CPUs - cpus.available */

	struct spa_list node_list;
};

static void hist_dump(const char *who, struct node *drv);

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct node *n, *tmp;

	/* Stop emitting RT signals first: drop the context listener so no
	 * more drivers are added, then disable existing drivers so no
	 * new samples are pushed into the rings. */
	spa_hook_remove(&impl->context_listener);
	spa_list_for_each(n, &impl->node_list, link) {
		if (n->is_driver && n->enabled) {
			SPA_FLAG_CLEAR(n->node->rt.target.activation->flags,
				       PW_NODE_ACTIVATION_FLAG_PROFILER);
			pw_impl_node_remove_rt_listener(n->node, &n->node_rt_listener);
			n->enabled = false;
		}
	}

	/* Stop the worker before tearing down per-driver buffers it
	 * might still touch. Sources are owned by the worker loop and
	 * are destroyed when the loop is destroyed -- but we destroy
	 * them explicitly here so the order is unambiguous: no more
	 * wakeups can be processed after pw_thread_loop_stop returns. */
	if (impl->worker_tloop) {
		pw_thread_loop_stop(impl->worker_tloop);
		if (impl->worker_wake && impl->worker_loop)
			pw_loop_destroy_source(impl->worker_loop, impl->worker_wake);
		impl->worker_wake = NULL;
		pw_thread_loop_destroy(impl->worker_tloop);
		impl->worker_tloop = NULL;
		impl->worker_loop = NULL;
	}

	/* Dump per-driver timing histograms for the live A/B test, then
	 * release driver/follower bookkeeping. */
	spa_list_for_each_safe(n, tmp, &impl->node_list, link) {
		hist_dump("destroy", n);
		if (n->sketch_ready)
			wcet_sketch_fini(&n->sketch);
		free(n->ring_slots);
		free(n->topo.nodes);
		free(n->topo.edges);
		spa_list_remove(&n->link);
		free(n);
	}

	spa_hook_remove(&impl->module_listener);

	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static bool can_use_deadline_policy(void)
{
	struct __user_cap_header_struct hdr;
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
	unsigned int index, mask;

	spa_zero(hdr);
	spa_zero(data);
	hdr.version = _LINUX_CAPABILITY_VERSION_3;
	hdr.pid = 0;

	if (syscall(SYS_capget, &hdr, data) != 0) {
		switch (errno) {
		case EPERM:
		case EACCES:
			pw_log_info("deadline scheduling unavailable: capget() permission denied");
			break;
		case ENOSYS:
		case EOPNOTSUPP:
			pw_log_info("deadline scheduling unavailable: capget() not supported");
			break;
		default:
			pw_log_info("deadline scheduling unavailable: capget() failed: %m");
			break;
		}
		return false;
	}

	index = CAP_SYS_NICE / 32;
	mask = 1U << (CAP_SYS_NICE % 32);

	if ((data[index].effective & mask) != 0) {
		pw_log_debug("CAP_SYS_NICE available for current thread");
		return true;
	}

	pw_log_info("deadline scheduling unavailable: CAP_SYS_NICE not in effective set");
	return false;
}

static int set_deadline_sched(pid_t tid, uint64_t runtime, uint64_t deadline, uint64_t period)
{
	int ret = 0;

	if (runtime == 0 || deadline > period) {
		pw_log_warn("invalid DEADLINE attributes for tid %d: r:%lu d:%lu p:%lu", tid, runtime, deadline, period);
		errno = EINVAL;
		return -1;
	}

	struct sched_attr attr = {0};

	attr.sched_policy = SCHED_DEADLINE;
	attr.sched_runtime = runtime;
	attr.sched_deadline = deadline;
	attr.sched_period = period;

	attr.sched_flags |= SCHED_FLAG_RECLAIM;

	ret = sched_setattr(tid, &attr, 0);

	if (ret) {
		if (errno == EINVAL)
			pw_log_warn("invalid DEADLINE attributes for tid %d: r:%lu d:%lu p:%lu", tid, runtime, deadline, period);
		else
			pw_log_error("failed to set DEADLINE attributes for tid %d: %s", tid, strerror(errno));
	}
	else
		pw_log_debug("set DEADLINE scheduling for tid %d: r:%lu d:%lu p:%lu", tid, runtime, deadline, period);

	return ret;
}

static int set_cpu_affinity(pid_t tid, int cpu)
{
	int ret = 0;

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);

	ret = sched_setaffinity(tid, sizeof(cpuset), &cpuset);
	if (ret < 0) {
		if (errno == EINVAL)
			pw_log_warn("invalid affinity for tid %d: cpu %d", tid, cpu);
		else
			pw_log_error("failed to set affinity for tid %d: %s", tid, strerror(errno));
	}
	return ret;
}

static void sched_cb(void *data, pid_t tid, uint64_t runtime, uint64_t deadline, uint64_t period, uint32_t cpu)
{
	struct impl *impl = data;
	set_deadline_sched(tid, runtime, deadline, period);
	set_cpu_affinity(tid, impl->cpus[cpu]);
}

static struct node *find_node(struct impl *impl, struct pw_impl_node *node)
{
	struct node *n;
	spa_list_for_each(n, &impl->node_list, link) {
		if (n->node == node)
			return n;
	}
	return NULL;
}

static struct node *find_node_by_id(struct impl *impl, uint32_t id)
{
	struct node *n;
	spa_list_for_each(n, &impl->node_list, link) {
		if (!n->is_driver && n->node_id == id)
			return n;
	}
	return NULL;
}

/* Log2 bucket of x. Used to bin RT-hook timings cheaply (no log call,
 * no division). bucket(0) returns 0; bucket(1) returns 0;
 * bucket(2..3) returns 1; bucket(4..7) returns 2; ...; bucket(>=2^31)
 * is clamped to 31. */
static inline uint32_t log2_bucket(uint64_t x)
{
	if (x <= 1)
		return 0;
	uint32_t b = 63 - __builtin_clzll(x);
	return b > 31 ? 31 : b;
}

static inline uint64_t now_thread_cputime_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static void hist_record(struct node *drv, uint64_t dur_ns)
{
	if (dur_ns == 0)
		return;
	drv->hist_buckets[log2_bucket(dur_ns)]++;
	drv->hist_count++;
	drv->hist_sum_ns += dur_ns;
	if (drv->hist_min_ns == 0 || dur_ns < drv->hist_min_ns)
		drv->hist_min_ns = dur_ns;
	if (dur_ns > drv->hist_max_ns)
		drv->hist_max_ns = dur_ns;
}

static void worker_hist_record(struct node *drv, uint64_t dur_ns)
{
	if (dur_ns == 0)
		return;
	drv->wk_hist_buckets[log2_bucket(dur_ns)]++;
	drv->wk_hist_count++;
	drv->wk_hist_sum_ns += dur_ns;
	if (drv->wk_hist_min_ns == 0 || dur_ns < drv->wk_hist_min_ns)
		drv->wk_hist_min_ns = dur_ns;
	if (dur_ns > drv->wk_hist_max_ns)
		drv->wk_hist_max_ns = dur_ns;
}

static void hist_dump(const char *who, struct node *drv)
{
	if (drv->hist_count == 0 && drv->wk_hist_count == 0)
		return;
	if (drv->hist_count > 0) {
		pw_log_info("deadline RT-hook timing [%s drv-node=%d mode=%s]:",
			    who,
			    drv->node ? drv->node->info.id : (uint32_t)-1,
			    drv->impl->sync_mode ? "sync" : "async");
		pw_log_info("  cycles=%lu min=%luns max=%luns avg=%luns",
			    drv->hist_count, drv->hist_min_ns, drv->hist_max_ns,
			    drv->hist_sum_ns / drv->hist_count);
		pw_log_info("  bucket counts (lo bound ns -> count):");
		for (uint32_t i = 0; i < 32; i++) {
			if (drv->hist_buckets[i] == 0)
				continue;
			pw_log_info("    >=%luns -> %lu", (uint64_t)1 << i,
				    drv->hist_buckets[i]);
		}
		drv->ring_dropped = SPA_ATOMIC_LOAD(drv->ring_dropped);
		if (drv->ring_dropped)
			pw_log_info("  ring-dropped samples (cumulative): %lu",
				    drv->ring_dropped);
	}
	if (drv->wk_hist_count > 0) {
		pw_log_info("deadline worker per-wake timing [%s drv-node=%d mode=%s]:",
			    who,
			    drv->node ? drv->node->info.id : (uint32_t)-1,
			    drv->impl->sync_mode ? "sync" : "async");
		pw_log_info("  wakes=%lu min=%luns max=%luns avg=%luns",
			    drv->wk_hist_count, drv->wk_hist_min_ns, drv->wk_hist_max_ns,
			    drv->wk_hist_sum_ns / drv->wk_hist_count);
		pw_log_info("  bucket counts (lo bound ns -> count):");
		for (uint32_t i = 0; i < 32; i++) {
			if (drv->wk_hist_buckets[i] == 0)
				continue;
			pw_log_info("    >=%luns -> %lu", (uint64_t)1 << i,
				    drv->wk_hist_buckets[i]);
		}
	}
}

static SPA_UNUSED bool is_audio_source_media_class(const char *media_class)
{
	return media_class != NULL &&
		(spa_strstartswith(media_class, "Audio/Source") ||
		 spa_strstartswith(media_class, "Stream/Output/Audio"));
}

static SPA_UNUSED bool is_audio_sink_media_class(const char *media_class)
{
	return media_class != NULL &&
		(spa_strstartswith(media_class, "Audio/Sink") ||
		 spa_strstartswith(media_class, "Stream/Input/Audio"));
}

static inline uint64_t get_runtime_ns(struct pw_impl_node *node, struct pw_node_activation *na)
{
	uint64_t runtime = SPA_ATOMIC_LOAD(na->prev_run_time);
	
	if (runtime == 0 || runtime > UINT64_MAX / 2) {
		pw_log_warn("invalid runtime %lu for node %d, using 0 instead", runtime, node->info.id);
		return 0;
	}

	return runtime;
}

/* Apply one sample to a follower's estimator. Worker-thread or RT-
 * thread (in sync mode); never both for a given node. */
static void apply_sample(struct impl *impl, struct node *n, uint64_t runtime, uint64_t period)
{
	if (!n->sketch_ready) {
		if (wcet_sketch_init(&n->sketch,
				     impl->sketch_window_size,
				     impl->sketch_compression,
				     impl->sketch_quantile) == 0) {
			n->sketch_ready = true;
		} else {
			pw_log_warn("node %d: WCET sketch init failed; using peak-hold",
				    n->node ? n->node->info.id : (uint32_t)-1);
		}
	}

	if (n->period != period) {
		if (n->sketch_ready)
			wcet_sketch_reset(&n->sketch);
		n->wcet = 0;
	}

	if (runtime > 0 && n->sketch_ready)
		wcet_sketch_add(&n->sketch, (double)runtime);

	if (!n->sketch_ready ||
	    wcet_sketch_count(&n->sketch) < impl->sketch_min_samples) {
		n->wcet = SPA_MAX(n->wcet, runtime);
	} else {
		double q = wcet_sketch_quantile(&n->sketch);
		if (q > 0.0 && q < (double)UINT64_MAX)
			n->wcet = (uint64_t)q;
		else
			n->wcet = SPA_MAX(n->wcet, runtime);
	}

	n->period = period;
}

/* ------------------------------------------------------------------
 * Sync path: original behaviour. Builds the DAG, runs P-EDF analysis
 * and applies SCHED_DEADLINE/affinity all from the driver's data-loop
 * thread, inside the RT hook. Kept as a benchmark/escape hatch via
 * the recalc.sync=true option.
 * ------------------------------------------------------------------ */
/* Helper: is `id` in the kept-set? Tiny linear scan, suitable for the
 * small follower counts a driver typically sees. */
static bool id_is_kept(const uint32_t *ids, uint32_t n, uint32_t id)
{
	for (uint32_t i = 0; i < n; i++)
		if (ids[i] == id)
			return true;
	return false;
}

static void recalc_params_sync(struct node *drv)
{
	struct pw_impl_node *node = drv->node;
	struct impl *impl = drv->impl;
	struct pw_node_target *t;
	struct pw_impl_node *node2;

	if (node->target_rate.denom == 0 || node->target_quantum == 0)
		return;

	uint64_t period = SPA_NSEC_PER_SEC * node->target_quantum / node->target_rate.denom;

	dag_t *dag = dag_create(period, period, impl->cpu_utilization, impl->n_cpus);

	/* Soft-failure: collect the ids of nodes successfully added to
	 * the DAG so we can drop edges that reference skipped nodes. */
	uint32_t kept_ids[256];
	uint32_t n_kept = 0;

	spa_list_for_each(t, &node->rt.target_list, link) {
		struct pw_impl_node *tnode = t->node;
		struct pw_node_activation *na;
		pid_t tid = -1;

		struct node *n = find_node(impl, tnode);
		if (n == NULL) {
			n = calloc(1, sizeof(*n));
			n->impl = impl;
			n->node = tnode;
			n->node_id = tnode->info.id;
			n->enabled = true;
			spa_list_insert(&impl->node_list, &n->link);
		}

		na = t->activation;
		uint64_t runtime = get_runtime_ns(tnode, na);
		if (runtime > period)
			pw_log_warn("node %d runtime %lu exceeds period %lu",
				    tnode->info.id, runtime, period);

		apply_sample(impl, n, runtime, period);

		if (n->wcet == 0 || (uint64_t)(n->wcet * 1.05) == 0) {
			pw_log_debug("sync: skipping node %d (wcet=0)", tnode->info.id);
			continue;
		}

		if (pw_properties_get_bool(tnode->properties, PW_KEY_NODE_LOOP_DYNAMIC, false)) {
			tid = pw_properties_get_int32(tnode->properties, PW_KEY_NODE_LOOP_TID, -1);
			if (tid == -1) {
				pw_log_error("node %d has no TID", tnode->info.id);
				continue;
			}
		} else {
			pw_log_error("node %d is not a dynamic loop", tnode->info.id);
			continue;
		}

		dag_add_node(dag, tnode->info.id, (uint64_t)(n->wcet * 1.05), tid, false);
		if (n_kept < SPA_N_ELEMENTS(kept_ids))
			kept_ids[n_kept++] = tnode->info.id;
	}

	if (n_kept == 0) {
		dag_destroy(dag);
		return;
	}

	spa_list_for_each(t, &node->rt.target_list, link) {
		struct pw_impl_node *tnode = t->node;
		struct pw_impl_port *p;
		struct pw_impl_link *l;
		if (!id_is_kept(kept_ids, n_kept, tnode->info.id))
			continue;
		spa_list_for_each(p, &tnode->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				node2 = l->input->node;
				if (!id_is_kept(kept_ids, n_kept, node2->info.id))
					continue;
				dag_add_edge(dag, tnode->info.id, node2->info.id);
			}
		}
	}

	dag_recalculate(dag);
	dag_foreach_node(dag, sched_cb, impl);

	dag_destroy(dag);
}

/* ------------------------------------------------------------------
 * Async path: the RT hook just pushes per-target samples to a per-
 * driver SPSC ring and signals the worker via an eventfd. All the
 * heavy work (sketch updates, DAG build, P-EDF analysis, syscalls)
 * happens on the worker thread, which runs at a low SCHED_FIFO prio
 * and is CPU-pinned away from the deadline cores so it never shares
 * a CPU with the threads it configures.
 * ------------------------------------------------------------------ */

/* RT-context: push samples into drv's ring. Drops if full (worker is
 * lagging); the WCET sketch tail stats absorb the loss naturally. */
static void rt_push_samples(struct node *drv, uint64_t period)
{
	struct pw_impl_node *node = drv->node;
	struct pw_node_target *t;
	uint32_t buf_size_bytes = drv->ring_capacity * sizeof(struct sample);

	spa_list_for_each(t, &node->rt.target_list, link) {
		struct pw_impl_node *tnode = t->node;
		uint64_t runtime = get_runtime_ns(tnode, t->activation);

		uint32_t widx;
		int32_t filled = spa_ringbuffer_get_write_index(&drv->ring, &widx);
		if (filled < 0 || (uint32_t)filled >= buf_size_bytes) {
			drv->ring_dropped++;
			continue;
		}
		uint32_t offset = (widx % buf_size_bytes);
		/* Slot-aligned writes only; capacity is power of two of slot
		 * size so offset always lands on a slot boundary. */
		struct sample *s = (struct sample *)((uint8_t *)drv->ring_slots + offset);
		s->node_id = tnode->info.id;
		s->_pad = 0;
		s->runtime_ns = runtime;
		s->period_ns = period;
		spa_ringbuffer_write_update(&drv->ring, widx + sizeof(struct sample));
	}
}

/* Worker-context: drain everything the producer wrote since last time. */
static void worker_drain_samples(struct impl *impl, struct node *drv)
{
	uint32_t buf_size_bytes = drv->ring_capacity * sizeof(struct sample);
	uint32_t ridx;
	int32_t avail = spa_ringbuffer_get_read_index(&drv->ring, &ridx);
	if (avail <= 0)
		return;

	/* Process slot-by-slot. */
	uint32_t processed = 0;
	while ((uint32_t)avail - processed >= sizeof(struct sample)) {
		uint32_t offset = ((ridx + processed) % buf_size_bytes);
		const struct sample *s = (const struct sample *)
			((uint8_t *)drv->ring_slots + offset);

		struct node *n = find_node_by_id(impl, s->node_id);
		if (n == NULL) {
			n = calloc(1, sizeof(*n));
			if (n) {
				n->impl = impl;
				n->node_id = s->node_id;
				n->enabled = true;
				/* n->node stays NULL: the worker never
				 * dereferences pw_impl_node *; the
				 * topology snapshot carries the id+tid
				 * we need to apply DEADLINE. */
				spa_list_insert(&impl->node_list, &n->link);
			}
		}
		if (n)
			apply_sample(impl, n, s->runtime_ns, s->period_ns);

		processed += sizeof(struct sample);
	}
	spa_ringbuffer_read_update(&drv->ring, ridx + processed);
	pw_log_trace("worker drained %u samples (drv-node=%d)",
		     processed / (uint32_t)sizeof(struct sample),
		     drv->node ? drv->node->info.id : (uint32_t)-1);
}

/* Main-loop context: walk the driver's follower list and the
 * follower ports/links to capture a self-contained topology snapshot
 * that the worker can consume without further main-loop access. */
struct snapshot_arg {
	struct node *drv;
};

static int snapshot_topology_main(struct spa_loop *loop SPA_UNUSED,
				   bool async SPA_UNUSED, uint32_t seq SPA_UNUSED,
				   const void *data, size_t size SPA_UNUSED,
				   void *user_data SPA_UNUSED)
{
	const struct snapshot_arg *a = data;
	struct node *drv = a->drv;
	struct pw_impl_node *dnode = drv->node;
	struct topo_snap *t = &drv->topo;

	t->ok = false;
	t->n_nodes = 0;
	t->n_edges = 0;

	if (dnode->target_rate.denom == 0 || dnode->target_quantum == 0) {
		SPA_ATOMIC_STORE(t->pending, 0);
		return 0;
	}
	t->period = SPA_NSEC_PER_SEC * dnode->target_quantum / dnode->target_rate.denom;

	struct pw_impl_node *follower;
	spa_list_for_each(follower, &dnode->follower_list, follower_link) {
		if (follower == dnode)
			continue;
		if (!pw_properties_get_bool(follower->properties,
					    PW_KEY_NODE_LOOP_DYNAMIC, false))
			continue;
		pid_t tid = pw_properties_get_int32(follower->properties,
						    PW_KEY_NODE_LOOP_TID, -1);
		if (tid == -1)
			continue;

		if (t->n_nodes >= t->nodes_cap) {
			uint32_t newcap = t->nodes_cap ? t->nodes_cap * 2 : TOPO_INITIAL_NODES;
			struct topo_node *nn = realloc(t->nodes, newcap * sizeof(*nn));
			if (!nn)
				return -ENOMEM;
			t->nodes = nn;
			t->nodes_cap = newcap;
		}
		t->nodes[t->n_nodes].id = follower->info.id;
		t->nodes[t->n_nodes].tid = tid;
		t->n_nodes++;
	}

	/* Edges: for each follower, walk output ports -> links -> input node. */
	spa_list_for_each(follower, &dnode->follower_list, follower_link) {
		if (follower == dnode)
			continue;
		struct pw_impl_port *p;
		struct pw_impl_link *l;
		spa_list_for_each(p, &follower->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				if (!l->input || !l->input->node)
					continue;
				if (t->n_edges >= t->edges_cap) {
					uint32_t newcap = t->edges_cap ? t->edges_cap * 2 : TOPO_INITIAL_EDGES;
					struct topo_edge *ne = realloc(t->edges, newcap * sizeof(*ne));
					if (!ne)
						return -ENOMEM;
					t->edges = ne;
					t->edges_cap = newcap;
				}
				t->edges[t->n_edges].src = follower->info.id;
				t->edges[t->n_edges].dst = l->input->node->info.id;
				t->n_edges++;
			}
		}
	}

	t->ok = true;
	SPA_ATOMIC_STORE(t->pending, 0);
	return 0;
}

/* Worker-context: rebuild and reapply the DAG using the freshest
 * topology snapshot + current per-node sketch budgets.
 *
 * Soft-failure: a single follower that never produces a non-zero
 * runtime (e.g. an idle source: a synth client whose plugin failed
 * to start, a paused stream, a node currently disconnected from any
 * input) used to abort the whole driver's recalc, leaving every
 * other follower on the default SCHED_FIFO scheduling. Instead,
 * skip the broken node from the DAG and apply SCHED_DEADLINE to
 * everyone else. Drop any edge that referenced the skipped node;
 * the DAG library treats the resulting sub-graph as the workload
 * to schedule. */
static void worker_apply_dag(struct impl *impl, struct node *drv)
{
	struct topo_snap *t = &drv->topo;
	if (!t->ok || t->n_nodes == 0)
		return;

	dag_t *dag = dag_create(t->period, t->period, impl->cpu_utilization, impl->n_cpus);

	/* keep[i] = whether t->nodes[i] made it into the DAG */
	bool *keep = calloc(t->n_nodes, sizeof(bool));
	if (keep == NULL) {
		dag_destroy(dag);
		return;
	}

	uint32_t skipped = 0;
	for (uint32_t i = 0; i < t->n_nodes; i++) {
		struct node *n = find_node_by_id(impl, t->nodes[i].id);
		if (n == NULL || n->wcet == 0 || (uint64_t)(n->wcet * 1.05) == 0) {
			pw_log_debug("worker: skipping node %u (wcet=%lu) -- "
				     "applying DEADLINE to remaining nodes",
				     t->nodes[i].id, n ? n->wcet : 0);
			skipped++;
			continue;
		}
		dag_add_node(dag, t->nodes[i].id,
			     (uint64_t)(n->wcet * 1.05), t->nodes[i].tid, false);
		keep[i] = true;
	}

	if (t->n_nodes - skipped == 0) {
		/* Nothing to schedule yet: every follower is missing
		 * runtime samples. Try again next tick. */
		free(keep);
		dag_destroy(dag);
		return;
	}

	for (uint32_t i = 0; i < t->n_edges; i++) {
		/* Map src/dst to the keep[] index. Linear scan over a
		 * handful of nodes; topology is small. */
		bool src_kept = false, dst_kept = false;
		for (uint32_t j = 0; j < t->n_nodes; j++) {
			if (!keep[j])
				continue;
			if (t->nodes[j].id == t->edges[i].src)
				src_kept = true;
			if (t->nodes[j].id == t->edges[i].dst)
				dst_kept = true;
		}
		if (src_kept && dst_kept)
			dag_add_edge(dag, t->edges[i].src, t->edges[i].dst);
	}

	dag_recalculate(dag);
	dag_foreach_node(dag, sched_cb, impl);

	free(keep);
	dag_destroy(dag);
}

/* Worker-context: full recalc cycle for one driver. Drains samples,
 * applies the DAG using the most recent topology snapshot, then
 * kicks off a *non-blocking* refresh on the main loop for the next
 * cycle. Non-blocking is essential: module_destroy runs on the main
 * loop and would otherwise deadlock against a worker still parked in
 * a synchronous main-loop invoke. The first wake uses topo.ok=false
 * (skips apply); subsequent wakes use the topology refreshed by the
 * previous wake's async invoke. Graph mutations are rare so trailing
 * by one cycle is acceptable. */
static void worker_recalc_one(struct impl *impl, struct node *drv)
{
	uint64_t t0;

	if (impl->worker_fake_delay_us > 0) {
		struct timespec ts = {
			.tv_sec  = impl->worker_fake_delay_us / 1000000u,
			.tv_nsec = (impl->worker_fake_delay_us % 1000000u) * 1000u,
		};
		nanosleep(&ts, NULL);
	}

	/* Per-wake CPU-time measurement, log2-bucketed alongside the
	 * RT-hook histogram. fake-delay-us is excluded (nanosleep above
	 * does not advance CLOCK_THREAD_CPUTIME_ID). */
	t0 = now_thread_cputime_ns();

	worker_drain_samples(impl, drv);

	worker_apply_dag(impl, drv);

	{
		uint64_t t1 = now_thread_cputime_ns();
		if (t1 > t0)
			worker_hist_record(drv, t1 - t0);
	}

	/* Coalesce snapshot invokes: only queue a new one if no previous
	 * one is still pending. The main-loop callback clears the flag
	 * after writing. */
	if (SPA_ATOMIC_CAS(drv->topo.pending, 0, 1)) {
		struct snapshot_arg a = { .drv = drv };
		pw_loop_invoke(impl->main_loop, snapshot_topology_main, 0,
			       &a, sizeof(a), false, NULL);
	}
}

/* Worker-thread self-setup: SCHED_FIFO + CPU affinity. The poll
 * timer is registered separately from the main thread (before the
 * worker starts running its loop) -- registering it from inside a
 * worker invoke proved unreliable in earlier iterations of this
 * file. Sched failures are warnings; the worker still runs, just
 * at SCHED_OTHER. */
struct worker_setup_arg {
	cpu_set_t aff;
	int       rt_prio;
};

static int worker_setup(struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
			uint32_t seq SPA_UNUSED, const void *data, size_t size SPA_UNUSED,
			void *user_data SPA_UNUSED)
{
	const struct worker_setup_arg *p = data;
	struct sched_param sp = { .sched_priority = p->rt_prio };
	int rc;

	if (sched_setaffinity(0, sizeof(p->aff), &p->aff) != 0)
		pw_log_warn("deadline-recalc: sched_setaffinity failed: %m");

	rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	if (rc != 0)
		pw_log_warn("deadline-recalc: pthread_setschedparam(FIFO,%d) failed: %s",
			    p->rt_prio, strerror(rc));

	int actual_policy = -1;
	struct sched_param actual_sp = {0};
	pthread_getschedparam(pthread_self(), &actual_policy, &actual_sp);
	pid_t tid = (pid_t)syscall(SYS_gettid);

	pw_log_info("deadline-recalc worker tid=%d: SCHED_FIFO prio %d (actual policy=%d, "
		    "FIFO=%d), affinity=%d cpus",
		    tid, p->rt_prio, actual_policy, SCHED_FIFO,
		    CPU_COUNT(&p->aff));
	return 0;
}

/* Worker-thread context: drain every driver whose recalc_inflight
 * flag was raised on the RT side since the previous wakeup. The
 * worker is woken by pw_loop_signal_event on the impl->worker_wake
 * eventfd source -- the RT hook calls signal_event once on every
 * 0->1 CAS of recalc_inflight, so a single wakeup covers all of the
 * RT cycles that piled up while the worker was busy. */
static void worker_wake_func(void *data, uint64_t count SPA_UNUSED)
{
	struct impl *impl = data;
	struct node *drv;

	spa_list_for_each(drv, &impl->node_list, link) {
		if (!drv->is_driver)
			continue;
		uint32_t inflight = SPA_ATOMIC_XCHG(drv->recalc_inflight, 0);
		if (inflight == 0)
			continue;
		worker_recalc_one(impl, drv);
	}
}

/* RT-context dispatcher: measures CPU time and routes to sync or
 * async body. Kept short so async-mode hook never grows beyond a
 * ring write + an eventfd signal. */
static void rt_hook(void *data)
{
	struct node *drv = data;
	struct pw_impl_node *node = drv->node;
	struct impl *impl = drv->impl;

	if (node->target_rate.denom == 0 || node->target_quantum == 0)
		return;

	uint64_t t0 = now_thread_cputime_ns();

	if (impl->sync_mode) {
		recalc_params_sync(drv);
	} else {
		uint64_t period = SPA_NSEC_PER_SEC * node->target_quantum
				  / node->target_rate.denom;
		rt_push_samples(drv, period);
		/* CAS 0->1 to coalesce many RT cycles into one worker
		 * pass; only signal the eventfd on the 0->1 transition.
		 * If the worker is already scheduled (CAS fails because
		 * inflight=1), the existing wakeup will pick up our
		 * just-written sample on its next pass. The worker
		 * XCHG-resets the flag at the start of its callback, so
		 * the very next RT cycle after the worker starts
		 * re-arms the wakeup. */
		if (SPA_ATOMIC_CAS(drv->recalc_inflight, 0, 1))
			pw_loop_signal_event(impl->worker_loop, impl->worker_wake);
	}

	uint64_t t1 = now_thread_cputime_ns();
	if (t1 > t0)
		hist_record(drv, t1 - t0);
}

static const struct pw_impl_node_rt_events node_rt_events = {
	PW_VERSION_IMPL_NODE_RT_EVENTS,
	.complete = rt_hook,
	.incomplete = rt_hook,
};

static void set_driver_hook_state(struct node *n, bool enabled)
{
	if (enabled && !n->enabled) {
		SPA_FLAG_SET(n->node->rt.target.activation->flags, PW_NODE_ACTIVATION_FLAG_PROFILER);
		pw_impl_node_add_rt_listener(n->node, &n->node_rt_listener, &node_rt_events, n);
	} else if (!enabled && n->enabled) {
		SPA_FLAG_CLEAR(n->node->rt.target.activation->flags, PW_NODE_ACTIVATION_FLAG_PROFILER);
		pw_impl_node_remove_rt_listener(n->node, &n->node_rt_listener);
	}
	n->enabled = enabled;
}

static void context_driver_added(void *data, struct pw_impl_node *node)
{
	struct impl *impl = data;
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		return;

	n->impl = impl;
	n->node = node;
	n->node_id = node->info.id;
	n->is_driver = true;

	/* Allocate the per-driver SPSC sample ring. Worker reads, RT
	 * thread writes. Drop-oldest on overflow. */
	n->ring_capacity = WORKER_RING_CAPACITY;
	n->ring_slots = calloc(n->ring_capacity, sizeof(struct sample));
	if (n->ring_slots == NULL) {
		pw_log_warn("driver %d: failed to allocate sample ring; module disabled for this driver",
			    node->info.id);
		free(n);
		return;
	}
	spa_ringbuffer_init(&n->ring);

	spa_list_append(&impl->node_list, &n->link);
	set_driver_hook_state(n, true);

	/* Async mode: prime the first topology snapshot so the very
	 * first worker wake has a populated topo to work with. The
	 * driver may not have a period yet (target_rate/quantum=0 until
	 * negotiated), in which case snapshot_topology_main sets
	 * topo.ok=false and the worker will retry next wake. */
	if (!impl->sync_mode && impl->main_loop != NULL) {
		struct snapshot_arg a = { .drv = n };
		SPA_ATOMIC_STORE(n->topo.pending, 1);
		snapshot_topology_main(NULL, false, 0, &a, sizeof(a), NULL);
	}
}

static void context_driver_removed(void *data, struct pw_impl_node *node)
{
	struct impl *impl = data;
	struct node *n;

	n = find_node(impl, node);
	if (n == NULL)
		return;

	set_driver_hook_state(n, false);

	/* After remove_rt_listener returns, no more RT callbacks for
	 * this driver are in flight. Tear down its async state. */
	hist_dump("driver-removed", n);
	if (n->sketch_ready)
		wcet_sketch_fini(&n->sketch);
	free(n->ring_slots);
	free(n->topo.nodes);
	free(n->topo.edges);
	spa_list_remove(&n->link);
	free(n);
}

static const struct pw_context_events context_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.driver_added = context_driver_added,
	.driver_removed = context_driver_removed,
};

static void parse_cpus(struct impl *impl, const char *cpus_str)
{
	struct spa_json it[3];
	int i = 0, v;

	spa_json_init(&it[0], cpus_str, strlen(cpus_str));
	if (spa_json_enter_array(&it[0], &it[1]) <= 0)
		spa_json_init(&it[1], cpus_str, strlen(cpus_str));

	while (spa_json_get_int(&it[1], &v) > 0) {
		if (i >= MAX_CPUS)
			break;
		if (v >= 0) {
			impl->cpus[i] = v;
			i++;
		}
	}
	impl->n_cpus = i;
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_properties *props;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	spa_list_init(&impl->node_list);

	pw_log_debug("module %p: new", impl);

	props = args ? pw_properties_new_string(args) : pw_properties_new(NULL, NULL);
	if (!props) {
		res = -errno;
		goto error;
	}
	impl->context = context;
	impl->props = props;

	parse_cpus(impl, pw_properties_get(props, "cpus.available"));

	const char *cpu_utilization_str = pw_properties_get(props, "cpus.utilization");
	spa_json_parse_float(cpu_utilization_str, strlen(cpu_utilization_str), &impl->cpu_utilization);

	impl->sketch_window_size = WCET_DEFAULT_WINDOW_SIZE;
	impl->sketch_quantile = WCET_DEFAULT_QUANTILE;
	impl->sketch_compression = WCET_DEFAULT_COMPRESSION;
	impl->sketch_min_samples = WCET_DEFAULT_MIN_SAMPLES;

	const char *s;
	if ((s = pw_properties_get(props, "wcet.window-size")) != NULL) {
		char *end;
		unsigned long v = strtoul(s, &end, 10);
		if (end != s && v >= 2 && v <= UINT32_MAX)
			impl->sketch_window_size = (uint32_t)v;
		else
			pw_log_warn("wcet.window-size %s ignored", s);
	}
	if ((s = pw_properties_get(props, "wcet.quantile")) != NULL) {
		char *end;
		double v = strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->sketch_quantile = v;
		else
			pw_log_warn("wcet.quantile %s ignored", s);
	}
	if ((s = pw_properties_get(props, "wcet.compression")) != NULL) {
		char *end;
		double v = strtod(s, &end);
		if (end != s && v > 0.0)
			impl->sketch_compression = v;
		else
			pw_log_warn("wcet.compression %s ignored", s);
	}
	if ((s = pw_properties_get(props, "wcet.min-samples")) != NULL) {
		char *end;
		unsigned long v = strtoul(s, &end, 10);
		if (end != s && v <= UINT32_MAX)
			impl->sketch_min_samples = (uint32_t)v;
		else
			pw_log_warn("wcet.min-samples %s ignored", s);
	}

	impl->sync_mode = pw_properties_get_bool(props, "recalc.sync", false);
	impl->worker_rt_prio = WORKER_DEFAULT_RT_PRIO;
	if ((s = pw_properties_get(props, "recalc.rt-prio")) != NULL) {
		char *end;
		long v = strtol(s, &end, 10);
		if (end != s && v >= 1 && v <= 99)
			impl->worker_rt_prio = (int)v;
		else
			pw_log_warn("recalc.rt-prio %s ignored", s);
	}
	impl->worker_fake_delay_us = 0;
	if ((s = pw_properties_get(props, "recalc.fake-delay-us")) != NULL) {
		char *end;
		unsigned long v = strtoul(s, &end, 10);
		if (end != s)
			impl->worker_fake_delay_us = (uint32_t)v;
		else
			pw_log_warn("recalc.fake-delay-us %s ignored", s);
	}

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);
	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));
	pw_impl_module_update_properties(module, &props->dict);

	const char *disable_deadline = getenv("PIPEWIRE_DISABLE_MODULE_DEADLINE");
	if (disable_deadline != NULL && disable_deadline[0] != '\0' && strcmp(disable_deadline, "0") != 0) {
		pw_log_info("deadline scheduling disabled by PIPEWIRE_DISABLE_MODULE_DEADLINE");
		goto done;
	}

	if (!can_use_deadline_policy()) {
		pw_log_warn("deadline scheduling disabled");
		goto done;
	}

	impl->main_loop = pw_context_get_main_loop(impl->context);

	/* Async worker setup. Computed in two pieces:
	 * - CPU affinity mask: daemon's current online set MINUS the
	 *   deadline cores (cpus.available). Worker must never share a
	 *   CPU with a thread it has just promoted to SCHED_DEADLINE.
	 * - SCHED_FIFO priority: strictly below PipeWire's audio
	 *   threads (default 88; min configurable 11) so the worker
	 *   never preempts audio. */
	if (!impl->sync_mode) {
		CPU_ZERO(&impl->worker_affinity);
		cpu_set_t base;
		CPU_ZERO(&base);
		if (sched_getaffinity(0, sizeof(base), &base) == 0) {
			for (int c = 0; c < CPU_SETSIZE; c++)
				if (CPU_ISSET(c, &base))
					CPU_SET(c, &impl->worker_affinity);
		} else {
			pw_log_warn("sched_getaffinity failed: %m; worker will run on all CPUs");
			for (int c = 0; c < CPU_SETSIZE; c++)
				CPU_SET(c, &impl->worker_affinity);
		}
		for (int i = 0; i < impl->n_cpus; i++) {
			if (impl->cpus[i] >= 0 && impl->cpus[i] < CPU_SETSIZE)
				CPU_CLR(impl->cpus[i], &impl->worker_affinity);
		}
		if (CPU_COUNT(&impl->worker_affinity) == 0) {
			pw_log_warn("no housekeeping CPUs left for worker; falling back to all");
			for (int c = 0; c < CPU_SETSIZE; c++)
				CPU_SET(c, &impl->worker_affinity);
		}

		impl->worker_tloop = pw_thread_loop_new("deadline-recalc", NULL);
		if (impl->worker_tloop == NULL) {
			pw_log_error("failed to create deadline-recalc thread loop: %m");
			goto worker_failed;
		}
		impl->worker_loop = pw_thread_loop_get_loop(impl->worker_tloop);

		/* Register the wake source BEFORE starting the worker
		 * thread. Sources added mid-iteration from within a
		 * worker_setup invoke proved unreliable on this host:
		 * the worker's epoll_wait would not observe the newly
		 * added fd. Adding from main, pre-start, guarantees the
		 * very first pw_loop_iterate already polls it. */
		impl->worker_wake = pw_loop_add_event(impl->worker_loop,
						      worker_wake_func, impl);
		if (impl->worker_wake == NULL) {
			pw_log_error("failed to add deadline-recalc wake source: %m");
			pw_thread_loop_destroy(impl->worker_tloop);
			impl->worker_tloop = NULL;
			goto worker_failed;
		}

		if (pw_thread_loop_start(impl->worker_tloop) < 0) {
			pw_log_error("failed to start deadline-recalc thread: %m");
			pw_loop_destroy_source(impl->worker_loop, impl->worker_wake);
			impl->worker_wake = NULL;
			pw_thread_loop_destroy(impl->worker_tloop);
			impl->worker_tloop = NULL;
			goto worker_failed;
		}

		/* Apply scheduling+affinity to the worker thread itself
		 * via a synchronous invoke. Failure is non-fatal: the
		 * worker still works, just at SCHED_OTHER. */
		struct worker_setup_arg setup = {
			.aff = impl->worker_affinity,
			.rt_prio = impl->worker_rt_prio,
		};
		(void)pw_loop_invoke(impl->worker_loop, worker_setup, 0,
				     &setup, sizeof(setup), true, NULL);
	}

worker_failed:
	pw_context_add_listener(impl->context, &impl->context_listener, &context_events, impl);

	goto done;

error:
	free(impl);
done:
	pw_properties_free(props);

	return res;
}
