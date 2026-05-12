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

#include "config.h"

#include "module-deadline/dag.h"
#include "module-deadline/wcet_sketch.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>

#include <pipewire/private.h>
#include <pipewire/impl.h>
#include <pipewire/thread.h>

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

struct node {
	struct spa_list link;
	struct impl *impl;

	struct pw_impl_node *node;
	struct spa_hook node_rt_listener;

	bool enabled:true;

	uint64_t wcet;
	uint64_t period;

	/* Streaming-quantile estimator over a sliding window of recent
	 * runtime samples. Lazily initialized on the first sample seen
	 * for this node; falls back to plain peak-hold (the previous
	 * behaviour) when sketch initialization fails. */
	wcet_sketch_t sketch;
	bool sketch_ready;
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

	struct spa_list node_list;
};

static void module_destroy(void *data)
{
	struct impl *impl = data;
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

static void recalc_params(void *data)
{
	struct node *n = data;
	struct pw_impl_node *node = n->node;
	struct impl *impl = n->impl;
	struct pw_node_target *t;
	struct pw_impl_node *node2;
	bool abort = false;

	if (node->target_rate.denom == 0 || node->target_quantum == 0)
		return;

	uint64_t period = SPA_NSEC_PER_SEC * node->target_quantum / node->target_rate.denom;

	dag_t *dag = dag_create(period, period, impl->cpu_utilization, impl->n_cpus);
	
	spa_list_for_each(t, &node->rt.target_list, link) {
		struct pw_impl_node *node = t->node;
		struct pw_node_activation *na;
		// const char *media_class;
		pid_t tid = -1;

		struct node *n = find_node(impl, node);
        if (n == NULL) {
            n = calloc(1, sizeof(*n));
            n->impl = impl;
            n->node = node;
            n->enabled = true;
            spa_list_insert(&impl->node_list, &n->link);
        }

		na = t->activation;
		uint64_t runtime = get_runtime_ns(node, na);
		if (runtime > period)
			pw_log_warn("node %d runtime %lu exceeds period %lu", node->info.id, runtime, period);

		/* Lazily attach the sliding-window quantile estimator the
		 * first time we see this node. If init fails (OOM), the
		 * node keeps running with plain peak-hold below. */
		if (!n->sketch_ready) {
			if (wcet_sketch_init(&n->sketch,
					     impl->sketch_window_size,
					     impl->sketch_compression,
					     impl->sketch_quantile) == 0) {
				n->sketch_ready = true;
			} else {
				pw_log_warn("node %d: WCET sketch init failed; using peak-hold",
					    node->info.id);
			}
		}

		/* On a quantum/rate change the prior samples no longer
		 * represent the same workload; drop them. The user has
		 * confirmed full reset is the desired semantics. */
		if (n->period != period) {
			if (n->sketch_ready)
				wcet_sketch_reset(&n->sketch);
			n->wcet = 0;
		}

		if (runtime > 0 && n->sketch_ready)
			wcet_sketch_add(&n->sketch, (double)runtime);

		/* Bootstrap: until enough samples have accumulated for the
		 * quantile estimate to be meaningful, keep behaving like
		 * the old peak-hold so that a single early sample cannot
		 * collapse the budget to a too-small number. */
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
		if (n->wcet == 0 || (uint64_t)(n->wcet * 1.05) == 0) {
			abort = true;
			pw_log_warn("Abort trying to add node %d (wcet=0)", node->info.id);
			continue;
		}

		n->period = period;

		if (pw_properties_get_bool(node->properties, PW_KEY_NODE_LOOP_DYNAMIC, false)) {
			tid = pw_properties_get_int32(node->properties, PW_KEY_NODE_LOOP_TID, -1);
			if (tid == -1) {
				pw_log_error("node %d has no TID", node->info.id);
				return;
			}
		} else {
			pw_log_error("node %d is not a dynamic loop", node->info.id);
			return;
		}

		// media_class = pw_properties_get(node->properties, PW_KEY_MEDIA_CLASS);
		dag_add_node(dag, node->info.id, (uint64_t)(n->wcet * 1.05), tid, false);
	}

	spa_list_for_each(t, &node->rt.target_list, link) {
		struct pw_impl_node *node = t->node;
		struct pw_impl_port *p;
		struct pw_impl_link *l;
		spa_list_for_each(p, &node->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				node2 = l->input->node;

				dag_add_edge(dag, node->info.id, node2->info.id);
			}
		}
		
	}

	if (!abort) {
		dag_recalculate(dag);
		dag_foreach_node(dag, sched_cb, impl);
	}

	dag_destroy(dag);
}

static const struct pw_impl_node_rt_events node_rt_events = {
	PW_VERSION_IMPL_NODE_RT_EVENTS,
	.complete = recalc_params,
	.incomplete = recalc_params,
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
	spa_list_append(&impl->node_list, &n->link);

	set_driver_hook_state(n, true);
}

static void context_driver_removed(void *data, struct pw_impl_node *node)
{
	struct impl *impl = data;
	struct node *n;

	n = find_node(impl, node);
	if (n == NULL)
		return;

	set_driver_hook_state(n, false);
	if (n->sketch_ready)
		wcet_sketch_fini(&n->sketch);
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

	pw_context_add_listener(impl->context, &impl->context_listener, &context_events, impl);

	goto done;

error:
	free(impl);
done:
	pw_properties_free(props);

	return res;
}
