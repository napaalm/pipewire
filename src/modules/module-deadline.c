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

#include "module-deadline/cpu_topology.h"
#include "module-deadline/dag.h"

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
 * - `cpus.smt-policy`:  How to handle SMT-paired logical CPUs within
 *                       `cpus.available`. `strict` (the default) refuses
 *                       the module load if any two CPUs in the set share
 *                       a physical core: the kernel admission is per
 *                       logical CPU and would silently overcommit the
 *                       physical capacity. `dedupe` keeps the lowest-id
 *                       sibling per core and drops the others. `ignore`
 *                       accepts the set unchanged and logs a warning;
 *                       only useful for diagnostic comparisons.
 * - `cpus.dvfs-policy`: Which cpufreq frequency to use when computing
 *                       per-CPU capacity. `conservative` (the default)
 *                       uses `cpuinfo_min_freq`: any budget that fits
 *                       at analysis time is guaranteed to fit at
 *                       runtime regardless of governor behaviour, since
 *                       real throughput exceeds the min-freq assumption
 *                       whenever the governor parks the CPU higher.
 *                       `assume-max` uses `cpuinfo_max_freq`; admission
 *                       is tighter but a governor that parks below
 *                       max_freq can cause deadline overruns.
 * - `cpus.topology-override`: Hidden test affordance. JSON string
 *                       describing the per-CPU topology (capacities,
 *                       core ids, freqs) that bypasses sysfs probing.
 *                       Used by the heterogeneous live test to fake
 *                       a two-class capacity layout on a homogeneous
 *                       host. See cpu_topology_from_json for the
 *                       schema.
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
 * - `recalc.persistent`: If true (default), the worker keeps a
 *                       persistent DAG across wakes and reconciles
 *                       it incrementally. A no-op cycle (no
 *                       topology change, no significant WCET
 *                       drift) does zero analysis work. If false,
 *                       the worker falls back to the legacy
 *                       destroy-and-rebuild path -- a permanent
 *                       kill switch for the persistent-DAG code,
 *                       intended only for diagnostic comparisons.
 * - `wcet.recalc-threshold`: Fractional WCET drift required to
 *                       update the cached DAG node and trigger a
 *                       recalc. With the default 0.01 a follower
 *                       whose reported WCET drifts by less than
 *                       1 % from the cached value is treated as a
 *                       no-op (the cached schedule stays valid).
 *                       Setting to 0 disables the gate (every
 *                       reported change marks dirty). Must be in
 *                       [0, 1).
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

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	"( cpu.available=<list of CPUs> ) "	\
			"( cpu.utilization=<percentage> ) "

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
	/* Per-CPU relative_capacity vector aligned with cpus[],
	 * derived from cpu_topology at module init. reconcile_init
	 * forwards a NULL here as the homogeneous (all-1.0) identity;
	 * a non-NULL vector is passed through to dag_create. */
	double *relative_capacity;
	/* The probed (or JSON-overridden) topology. Kept alive for the
	 * lifetime of the module so the per-CPU diagnostic fields are
	 * available for logging at any point. */
	struct cpu_topology topology;
	enum cpu_smt_policy  smt_policy;
	enum cpu_dvfs_policy dvfs_policy;

	/* WCET estimator configuration; see module-options doc above. */
	uint32_t sketch_window_size;
	double   sketch_quantile;
	double   sketch_compression;
	uint32_t sketch_min_samples;

	/* Persistent-DAG path on the worker (default). When false the
	 * worker still runs but reconcile_apply takes the legacy
	 * destroy-and-rebuild path -- a permanent kill switch for the
	 * persistent-DAG code. */
	bool                  recalc_persistent;

	/* WCET drift threshold: a follower's reported WCET must change
	 * by more than this fraction of the current value to trigger
	 * dag_set_node_wcet (and therefore a recalc). Default 0.01;
	 * 0.0 = always recalc. Must stay strictly below 1.0. */
	double                wcet_recalc_threshold;

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

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);

	free(impl->nodes_by_id);
	free(impl->relative_capacity);
	cpu_topology_destroy(&impl->topology);
	sched_groups_fini(&impl->sched_groups);
	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

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

int sched_setattr(pid_t pid, const struct sched_attr *attr, unsigned int flags) {
	return syscall(SYS_sched_setattr, pid, attr, flags);
}

int sched_getattr(pid_t pid, struct sched_attr *attr, unsigned int size, unsigned int flags) {
	return syscall(SYS_sched_getattr, pid, attr, size, flags);
}

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

		if (n->period == period)
			n->wcet = SPA_MAX(n->wcet, runtime);
		else
			n->wcet = runtime;
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

static enum cpu_smt_policy parse_smt_policy(const char *s)
{
	if (s == NULL)
		return CPU_SMT_STRICT;
	if (strcmp(s, "strict") == 0)
		return CPU_SMT_STRICT;
	if (strcmp(s, "dedupe") == 0)
		return CPU_SMT_DEDUPE;
	if (strcmp(s, "ignore") == 0)
		return CPU_SMT_IGNORE;
	pw_log_warn("cpus.smt-policy '%s' not recognised; using 'strict'", s);
	return CPU_SMT_STRICT;
}

static enum cpu_dvfs_policy parse_dvfs_policy(const char *s)
{
	if (s == NULL)
		return CPU_DVFS_CONSERVATIVE;
	if (strcmp(s, "conservative") == 0)
		return CPU_DVFS_CONSERVATIVE;
	if (strcmp(s, "assume-max") == 0)
		return CPU_DVFS_ASSUME_MAX;
	pw_log_warn("cpus.dvfs-policy '%s' not recognised; using 'conservative'", s);
	return CPU_DVFS_CONSERVATIVE;
}

/* Probe (or JSON-override) the per-CPU topology, apply the SMT policy
 * (strict refusal aborts module init), and shrink impl->cpus[] /
 * impl->n_cpus to match if DEDUPE dropped siblings. On success
 * impl->relative_capacity is populated, aligned with impl->cpus[],
 * and the per-CPU diagnostic block has been logged. Returns 0 on
 * success, -1 on refusal or sysfs failure. */
static int build_cpu_topology(struct impl *impl, struct pw_properties *props)
{
	uint32_t cpus[MAX_CPUS];
	uint32_t i;
	const char *override;
	int rc;

	if (impl->n_cpus <= 0) {
		pw_log_warn("cpus.available is empty; "
				"deadline scheduling will not be applied");
		return -1;
	}

	impl->smt_policy  = parse_smt_policy(
			pw_properties_get(props, "cpus.smt-policy"));
	impl->dvfs_policy = parse_dvfs_policy(
			pw_properties_get(props, "cpus.dvfs-policy"));

	for (i = 0; i < (uint32_t)impl->n_cpus; i++)
		cpus[i] = (uint32_t)impl->cpus[i];

	override = pw_properties_get(props, "cpus.topology-override");
	if (override != NULL && override[0] != '\0') {
		rc = cpu_topology_from_json(override, impl->dvfs_policy,
				&impl->topology);
	} else {
		rc = cpu_topology_probe(cpus, (uint32_t)impl->n_cpus,
				impl->dvfs_policy, &impl->topology);
	}
	if (rc < 0) {
		pw_log_error("cpu-topology: probe/override failed: %m");
		return -1;
	}

	uint32_t off_a = 0, off_b = 0;
	if (cpu_topology_apply_smt_policy(&impl->topology, impl->smt_policy,
				&off_a, &off_b) < 0) {
		/* STRICT refusal: name the offending pair and the physical
		 * core they share so the operator can edit cpus.available
		 * deliberately. */
		uint32_t core = 0;
		for (i = 0; i < impl->topology.num_cpus; i++) {
			if (impl->topology.cpus[i].cpu_id == off_a) {
				core = impl->topology.cpus[i].core_id;
				break;
			}
		}
		pw_log_error("cpus.smt-policy=strict refuses cpus.available: "
				"cpu%u and cpu%u are SMT siblings on physical core %u",
				off_a, off_b, core);
		cpu_topology_destroy(&impl->topology);
		return -1;
	}

	/* Re-derive impl->cpus[] from the (possibly shrunken) topology
	 * so DEDUPE actually removes siblings from the deadline-CPU set
	 * the rest of the module sees. The topology preserves the order
	 * in which CPUs were originally listed in cpus.available, so the
	 * resulting impl->cpus[] is "the original list minus the dropped
	 * siblings", which is what the operator would have written had
	 * they known about the duplicate. */
	for (i = 0; i < impl->topology.num_cpus; i++)
		impl->cpus[i] = (int)impl->topology.cpus[i].cpu_id;
	impl->n_cpus = (int)impl->topology.num_cpus;

	free(impl->relative_capacity);
	impl->relative_capacity = calloc(impl->topology.num_cpus,
			sizeof(*impl->relative_capacity));
	if (!impl->relative_capacity) {
		cpu_topology_destroy(&impl->topology);
		return -1;
	}
	for (i = 0; i < impl->topology.num_cpus; i++)
		impl->relative_capacity[i] =
			impl->topology.cpus[i].relative_capacity;

	pw_log_info("cpu-topology: smt-policy=%s dvfs-policy=%s num_cpus=%u",
			impl->smt_policy == CPU_SMT_STRICT ? "strict" :
			impl->smt_policy == CPU_SMT_DEDUPE ? "dedupe" : "ignore",
			impl->dvfs_policy == CPU_DVFS_CONSERVATIVE ?
				"conservative" : "assume-max",
			impl->topology.num_cpus);
	for (i = 0; i < impl->topology.num_cpus; i++) {
		const struct cpu_info *ci = &impl->topology.cpus[i];
		pw_log_info("cpu-topology: cpu%u core=%u island=%u "
				"raw_cap=%" PRIu64 " min_freq_khz=%" PRIu64
				" max_freq_khz=%" PRIu64
				" relative_capacity=%.3f",
				ci->cpu_id, ci->core_id, ci->island_id,
				ci->raw_capacity, ci->min_freq_khz,
				ci->max_freq_khz, ci->relative_capacity);
	}
	return 0;
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

	if (build_cpu_topology(impl, props) < 0) {
		/* Strict refusal or sysfs failure: disable deadline policy
		 * but keep the module loaded so PipeWire can still run. */
		pw_log_warn("deadline scheduling disabled (cpu topology probe failed)");
		goto done;
	}

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

	impl->recalc_persistent = pw_properties_get_bool(props, "recalc.persistent", true);
	impl->wcet_recalc_threshold = 0.01;
	if ((s = pw_properties_get(props, "wcet.recalc-threshold")) != NULL) {
		char *end;
		double v = strtod(s, &end);
		if (end != s && v >= 0.0 && v < 1.0)
			impl->wcet_recalc_threshold = v;
		else
			pw_log_warn("wcet.recalc-threshold %s ignored", s);
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

	pw_context_add_listener(impl->context, &impl->context_listener, &context_events, impl);

	goto done;

error:
	free(impl);
done:
	pw_properties_free(props);

	return res;
}
