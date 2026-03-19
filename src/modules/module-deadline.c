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
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <stdbool.h>
#include <math.h>
#include <sched.h>
#include <sys/syscall.h>
#include <linux/sched.h>

#include "config.h"

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
#define DEFAULT_CPU_UTILIZATION 0.95

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	"( cpus.available=<list of CPUs> ) "	\
			"( cpus.utilization=<percentage> ) "

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

	bool listener_active:1;
	bool seen_in_graph:1;

	uint64_t wcet;
	uint64_t period;
};

struct graph_snapshot_node {
	struct node *state;
	uint32_t id;
	uint64_t wcet;
	pid_t tid;
};

struct graph_snapshot_edge {
	uint32_t src_id;
	uint32_t dst_id;
};

struct graph_snapshot {
	uint64_t period;
	struct graph_snapshot_node *nodes;
	size_t node_count;
	size_t node_capacity;
	struct graph_snapshot_edge *edges;
	size_t edge_count;
	size_t edge_capacity;
};

struct impl {
	struct pw_context *context;
	struct pw_properties *props;

	struct pw_impl_module *module;

	struct spa_hook module_listener;
	struct spa_hook context_listener;

	int n_cpus;
	int cpus[MAX_CPUS];
	double cpu_utilization;

	/* Persistent scheduling DAG for the last synchronized driver graph. */
	dag_t *dag;
	struct pw_impl_node *dag_driver;

	struct spa_list node_list;
};

static const struct pw_impl_node_rt_events node_rt_events;

static struct node *find_cached_node(struct impl *impl, struct pw_impl_node *node)
{
	struct node *n;
	spa_list_for_each(n, &impl->node_list, link) {
		if (n->node == node)
			return n;
	}
	return NULL;
}

static struct node *ensure_cached_node(struct impl *impl, struct pw_impl_node *node)
{
	struct node *n;

	n = find_cached_node(impl, node);
	if (n != NULL)
		return n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		return NULL;

	n->impl = impl;
	n->node = node;
	spa_list_append(&impl->node_list, &n->link);

	return n;
}

static void set_driver_hook_state(struct node *n, bool enabled)
{
	struct pw_node_activation *activation = n->node->rt.target.activation;

	if (enabled && !n->listener_active) {
		if (activation == NULL) {
			pw_log_warn("node %u has no RT activation, can't enable deadline profiling",
					n->node->info.id);
			return;
		}
		SPA_FLAG_SET(activation->flags, PW_NODE_ACTIVATION_FLAG_PROFILER);
		pw_impl_node_add_rt_listener(n->node, &n->node_rt_listener, &node_rt_events, n);
		n->listener_active = true;
	} else if (!enabled && n->listener_active) {
		if (activation != NULL)
			SPA_FLAG_CLEAR(activation->flags, PW_NODE_ACTIVATION_FLAG_PROFILER);
		pw_impl_node_remove_rt_listener(n->node, &n->node_rt_listener);
		n->listener_active = false;
	}
}

static void destroy_cached_node(struct node *n)
{
	set_driver_hook_state(n, false);
	spa_list_remove(&n->link);
	free(n);
}

static void prune_stale_cached_nodes(struct impl *impl)
{
	struct node *n, *tmp;

	spa_list_for_each_safe(n, tmp, &impl->node_list, link) {
		if (!n->listener_active && !n->seen_in_graph)
			destroy_cached_node(n);
	}
}

static void reset_persistent_dag(struct impl *impl)
{
	dag_destroy(impl->dag);
	impl->dag = NULL;
	impl->dag_driver = NULL;
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct node *n, *tmp;

	spa_hook_remove(&impl->context_listener);
	spa_hook_remove(&impl->module_listener);
	reset_persistent_dag(impl);

	spa_list_for_each_safe(n, tmp, &impl->node_list, link)
		destroy_cached_node(n);

	pw_properties_free(impl->props);
	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int set_deadline_sched(pid_t tid, uint64_t runtime, uint64_t deadline, uint64_t period)
{
	int ret = 0;

	if (tid <= 0 || runtime == 0 || deadline == 0 || period == 0 ||
			runtime > deadline || deadline > period) {
		pw_log_warn("invalid DEADLINE attributes for tid %d: runtime:%"PRIu64
				" deadline:%"PRIu64" period:%"PRIu64,
				tid, runtime, deadline, period);
		errno = EINVAL;
		return -1;
	}

	struct sched_attr attr = {0};

	attr.size = sizeof(attr);
	attr.sched_policy = SCHED_DEADLINE;
	attr.sched_runtime = runtime;
	attr.sched_deadline = deadline;
	attr.sched_period = period;
	attr.sched_flags = SCHED_FLAG_RECLAIM;

	ret = sched_setattr(tid, &attr, 0);

	if (ret) {
		if (errno == EINVAL)
			pw_log_warn("invalid DEADLINE attributes for tid %d: runtime:%"PRIu64
					" deadline:%"PRIu64" period:%"PRIu64,
					tid, runtime, deadline, period);
		else
			pw_log_error("failed to set DEADLINE attributes for tid %d: %m", tid);
	}

	return ret;
}

static int set_cpu_affinity(pid_t tid, int cpu)
{
	int ret = 0;

	if (tid <= 0 || cpu < 0 || cpu >= CPU_SETSIZE) {
		pw_log_warn("invalid affinity request for tid %d: cpu %d", tid, cpu);
		errno = EINVAL;
		return -1;
	}

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);

	ret = sched_setaffinity(tid, sizeof(cpuset), &cpuset);
	if (ret < 0) {
		if (errno == EINVAL)
			pw_log_warn("invalid affinity for tid %d: cpu %d", tid, cpu);
		else
			pw_log_error("failed to set affinity for tid %d: %m", tid);
	}
	return ret;
}

static void sched_cb(void *data, pid_t tid, uint64_t runtime, uint64_t deadline, uint64_t period, uint32_t cpu)
{
	struct impl *impl = data;

	if (cpu >= (uint32_t) impl->n_cpus) {
		pw_log_warn("invalid CPU index %u for tid %d", cpu, tid);
		return;
	}

	set_deadline_sched(tid, runtime, deadline, period);
	set_cpu_affinity(tid, impl->cpus[cpu]);
}

static int measure_node_runtime(struct pw_impl_node *node,
		struct pw_node_activation *activation,
		uint64_t period, uint64_t *runtime)
{
	uint64_t measured;

	if (runtime == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (node->async) {
		measured = activation->prev_run_time;
	} else {
		if (activation->finish_cputime < activation->awake_cputime) {
			pw_log_warn("node %u reported inconsistent runtime timestamps: finish:%"PRIu64
					" awake:%"PRIu64,
					node->info.id,
					activation->finish_cputime,
					activation->awake_cputime);
			errno = ERANGE;
			return -1;
		}
		measured = activation->finish_cputime - activation->awake_cputime;
	}

	if (measured > period) {
		pw_log_warn("node %u runtime %"PRIu64" exceeds period %"PRIu64,
				node->info.id, measured, period);
	}

	*runtime = measured;
	return 0;
}

static uint64_t inflate_wcet(uint64_t wcet)
{
	uint64_t margin = wcet / 20;

	if (wcet > UINT64_MAX - margin)
		return UINT64_MAX;
	return wcet + margin;
}

static int get_dynamic_loop_tid(struct pw_impl_node *node, pid_t *tid)
{
	if (!pw_properties_get_bool(node->properties, PW_KEY_NODE_LOOP_DYNAMIC, false)) {
		pw_log_warn("node %u is not using a dynamic data loop",
				node->info.id);
		errno = EINVAL;
		return -1;
	}

	*tid = pw_properties_get_int32(node->properties, PW_KEY_NODE_LOOP_TID, -1);
	if (*tid <= 0) {
		pw_log_warn("node %u has no valid loop TID", node->info.id);
		errno = EINVAL;
		return -1;
	}

	return 0;
}

static void clear_graph_snapshot(struct graph_snapshot *snapshot)
{
	free(snapshot->edges);
	free(snapshot->nodes);
	memset(snapshot, 0, sizeof(*snapshot));
}

static int ensure_snapshot_capacity(void **items, size_t item_size,
		size_t *capacity, size_t count)
{
	size_t new_capacity;
	void *tmp;

	if (count < *capacity)
		return 0;

	new_capacity = *capacity > 0 ? *capacity * 2 : 8;
	while (count >= new_capacity) {
		if (new_capacity > SIZE_MAX / 2) {
			errno = ENOMEM;
			return -1;
		}
		new_capacity *= 2;
	}

	if (item_size != 0 && new_capacity > SIZE_MAX / item_size) {
		errno = ENOMEM;
		return -1;
	}

	tmp = realloc(*items, new_capacity * item_size);
	if (tmp == NULL)
		return -1;

	*items = tmp;
	*capacity = new_capacity;
	return 0;
}

static int snapshot_add_node(struct graph_snapshot *snapshot, struct node *state,
		uint32_t id, uint64_t wcet, pid_t tid)
{
	size_t i;

	for (i = 0; i < snapshot->node_count; i++) {
		if (snapshot->nodes[i].id != id)
			continue;

		snapshot->nodes[i].state = state;
		snapshot->nodes[i].wcet = wcet;
		snapshot->nodes[i].tid = tid;
		return 0;
	}

	if (ensure_snapshot_capacity((void **) &snapshot->nodes,
			sizeof(*snapshot->nodes), &snapshot->node_capacity,
			snapshot->node_count) < 0)
		return -1;

	snapshot->nodes[snapshot->node_count++] = (struct graph_snapshot_node) {
		.state = state,
		.id = id,
		.wcet = wcet,
		.tid = tid,
	};

	return 0;
}

static bool snapshot_has_node(const struct graph_snapshot *snapshot, uint32_t id)
{
	size_t i;

	for (i = 0; i < snapshot->node_count; i++) {
		if (snapshot->nodes[i].id == id)
			return true;
	}
	return false;
}

static int snapshot_add_edge(struct graph_snapshot *snapshot, uint32_t src_id,
		uint32_t dst_id)
{
	size_t i;

	for (i = 0; i < snapshot->edge_count; i++) {
		if (snapshot->edges[i].src_id == src_id &&
				snapshot->edges[i].dst_id == dst_id)
			return 0;
	}

	if (ensure_snapshot_capacity((void **) &snapshot->edges,
			sizeof(*snapshot->edges), &snapshot->edge_capacity,
			snapshot->edge_count) < 0)
		return -1;

	snapshot->edges[snapshot->edge_count++] = (struct graph_snapshot_edge) {
		.src_id = src_id,
		.dst_id = dst_id,
	};

	return 0;
}

static bool snapshot_has_edge(const struct graph_snapshot *snapshot,
		uint32_t src_id, uint32_t dst_id)
{
	size_t i;

	for (i = 0; i < snapshot->edge_count; i++) {
		if (snapshot->edges[i].src_id == src_id &&
				snapshot->edges[i].dst_id == dst_id)
			return true;
	}
	return false;
}

static void mark_snapshot_nodes_seen(struct impl *impl,
		const struct graph_snapshot *snapshot)
{
	struct node *state;
	size_t i;

	spa_list_for_each(state, &impl->node_list, link)
		state->seen_in_graph = false;

	for (i = 0; i < snapshot->node_count; i++)
		snapshot->nodes[i].state->seen_in_graph = true;
}

static dag_node_t *find_dag_node(dag_t *dag, uint32_t id)
{
	dag_node_t *node;

	if (dag == NULL)
		return NULL;

	spa_list_for_each(node, &dag->nodes, link) {
		if (node->id == id)
			return node;
	}
	return NULL;
}

static int ensure_persistent_dag(struct impl *impl, uint64_t period)
{
	if (impl->dag == NULL) {
		impl->dag = dag_create(period, period, impl->cpu_utilization,
				impl->n_cpus);
		if (impl->dag == NULL)
			return -1;

		pw_log_debug("created persistent deadline DAG");
		return 0;
	}

	return dag_set_global_period_deadline(impl->dag, period, period);
}

static int sync_snapshot_nodes_to_dag(struct impl *impl,
		const struct graph_snapshot *snapshot)
{
	size_t i;

	for (i = 0; i < snapshot->node_count; i++) {
		const struct graph_snapshot_node *entry = &snapshot->nodes[i];
		dag_node_t *dag_node = find_dag_node(impl->dag, entry->id);

		if (dag_node == NULL) {
			if (dag_add_node(impl->dag, entry->id, entry->wcet,
					entry->tid) < 0)
				return -1;
			continue;
		}

		dag_node->tid = entry->tid;
		if (dag_set_node_wcet(impl->dag, entry->id, entry->wcet) < 0)
			return -1;
	}

	return 0;
}

static int remove_stale_dag_edges(struct impl *impl,
		const struct graph_snapshot *snapshot)
{
	dag_edge_t *edge, *tmp;

	spa_list_for_each_safe(edge, tmp, &impl->dag->edges, link) {
		if (snapshot_has_edge(snapshot, edge->src->id, edge->dst->id))
			continue;
		if (dag_remove_edge(impl->dag, edge->src->id, edge->dst->id) < 0)
			return -1;
	}

	return 0;
}

static int add_snapshot_edges_to_dag(struct impl *impl,
		const struct graph_snapshot *snapshot)
{
	size_t i;

	for (i = 0; i < snapshot->edge_count; i++) {
		const struct graph_snapshot_edge *entry = &snapshot->edges[i];

		if (dag_add_edge(impl->dag, entry->src_id, entry->dst_id) == 0)
			continue;
		if (errno == EEXIST)
			continue;
		return -1;
	}

	return 0;
}

static int remove_stale_dag_nodes(struct impl *impl,
		const struct graph_snapshot *snapshot)
{
	dag_node_t *node, *tmp;

	spa_list_for_each_safe(node, tmp, &impl->dag->nodes, link) {
		if (snapshot_has_node(snapshot, node->id))
			continue;
		if (dag_remove_node(impl->dag, node->id) < 0)
			return -1;
	}

	return 0;
}

/*
 * Synchronize the module-owned DAG with the latest PipeWire driver snapshot.
 * The steady-state path updates nodes and edges incrementally so the DAG
 * library can reuse its dirty-state across recalculations.
 */
static int sync_dag_from_snapshot(struct impl *impl, struct pw_impl_node *driver,
		const struct graph_snapshot *snapshot)
{
	if (ensure_persistent_dag(impl, snapshot->period) < 0)
		return -1;
	if (sync_snapshot_nodes_to_dag(impl, snapshot) < 0)
		return -1;
	if (remove_stale_dag_edges(impl, snapshot) < 0)
		return -1;
	if (add_snapshot_edges_to_dag(impl, snapshot) < 0)
		return -1;
	if (remove_stale_dag_nodes(impl, snapshot) < 0)
		return -1;

	impl->dag_driver = driver;
	return 0;
}

/*
 * Rebuild the persistent DAG only as recovery if incremental synchronization
 * failed after mutating it. This avoids leaving a partially synchronized DAG
 * behind while keeping the normal path incremental.
 */
static int rebuild_dag_from_snapshot(struct impl *impl, struct pw_impl_node *driver,
		const struct graph_snapshot *snapshot)
{
	dag_t *dag;
	size_t i;

	dag = dag_create(snapshot->period, snapshot->period,
			impl->cpu_utilization, impl->n_cpus);
	if (dag == NULL)
		return -1;

	for (i = 0; i < snapshot->node_count; i++) {
		const struct graph_snapshot_node *entry = &snapshot->nodes[i];

		if (dag_add_node(dag, entry->id, entry->wcet, entry->tid) < 0)
			goto error;
	}

	for (i = 0; i < snapshot->edge_count; i++) {
		const struct graph_snapshot_edge *entry = &snapshot->edges[i];

		if (dag_add_edge(dag, entry->src_id, entry->dst_id) < 0)
			goto error;
	}

	dag_destroy(impl->dag);
	impl->dag = dag;
	impl->dag_driver = driver;
	return 0;

error:
	dag_destroy(dag);
	return -1;
}

/* Build a complete view of the current driver graph before mutating impl->dag. */
static int collect_driver_graph(struct impl *impl, struct pw_impl_node *driver,
		uint64_t period, struct graph_snapshot *snapshot)
{
	struct pw_node_target *t;

	snapshot->period = period;

	spa_list_for_each(t, &driver->rt.target_list, link) {
		struct pw_impl_node *node = t->node;
		struct pw_node_activation *activation = t->activation;
		struct node *state;
		pid_t tid = -1;
		uint64_t runtime;

		if (activation == NULL) {
			pw_log_warn("node %u has no activation data, skipping deadline update",
					node->info.id);
			errno = EINVAL;
			return -1;
		}

		state = ensure_cached_node(impl, node);
		if (state == NULL) {
			pw_log_error("can't allocate deadline state for node %u: %m",
					node->info.id);
			return -1;
		}

		if (measure_node_runtime(node, activation, period, &runtime) < 0)
			return -1;

		if (state->period == period)
			state->wcet = SPA_MAX(state->wcet, runtime);
		else
			state->wcet = runtime;
		state->period = period;

		if (state->wcet == 0) {
			pw_log_debug("node %u has no non-zero runtime sample yet, skipping deadline update",
					node->info.id);
			errno = EAGAIN;
			return -1;
		}

		if (get_dynamic_loop_tid(node, &tid) < 0)
			return -1;

		if (snapshot_add_node(snapshot, state, node->info.id,
				inflate_wcet(state->wcet), tid) < 0)
			return -1;
	}

	spa_list_for_each(t, &driver->rt.target_list, link) {
		struct pw_impl_node *node = t->node;
		struct pw_impl_port *port;
		struct pw_impl_link *link;

		spa_list_for_each(port, &node->output_ports, link) {
			spa_list_for_each(link, &port->links, output_link) {
				struct pw_impl_node *dst = link->input->node;

				if (!snapshot_has_node(snapshot, node->info.id) ||
						!snapshot_has_node(snapshot, dst->info.id))
					continue;

				if (snapshot_add_edge(snapshot, node->info.id,
						dst->info.id) < 0)
					return -1;
			}
		}
	}

	return 0;
}

static void recalc_params(void *data)
{
	struct node *n = data;
	struct pw_impl_node *driver = n->node;
	struct impl *impl = n->impl;
	struct graph_snapshot snapshot = { 0 };
	uint64_t period;
	bool snapshot_ready = false;

	if (impl->n_cpus <= 0) {
		pw_log_error("no CPUs available for deadline scheduling");
		return;
	}

	if (driver->target_rate.denom == 0 || driver->target_quantum == 0)
		return;

	period = SPA_NSEC_PER_SEC * driver->target_quantum / driver->target_rate.denom;
	if (period == 0) {
		pw_log_warn("node %u computed an invalid zero period", driver->info.id);
		return;
	}

	if (collect_driver_graph(impl, driver, period, &snapshot) < 0)
		goto out;

	mark_snapshot_nodes_seen(impl, &snapshot);
	snapshot_ready = true;

	if (sync_dag_from_snapshot(impl, driver, &snapshot) < 0) {
		int saved_errno = errno;

		errno = saved_errno;
		pw_log_warn("failed to synchronize DAG for node %u incrementally: %m",
				driver->info.id);

		if (rebuild_dag_from_snapshot(impl, driver, &snapshot) < 0) {
			saved_errno = errno;
			errno = saved_errno;
			pw_log_warn("failed to recover DAG state for node %u: %m",
					driver->info.id);
			reset_persistent_dag(impl);
			goto out;
		}
	}

	if (impl->dag->dirty && dag_recalculate(impl->dag) < 0) {
		pw_log_warn("failed to compute deadline parameters for node %u: %m",
				driver->info.id);
		goto out;
	}
	if (dag_foreach_node(impl->dag, sched_cb, impl) < 0) {
		pw_log_warn("failed to apply deadline parameters for node %u: %m",
				driver->info.id);
		goto out;
	}

out:
	clear_graph_snapshot(&snapshot);
	if (snapshot_ready)
		prune_stale_cached_nodes(impl);
}

static const struct pw_impl_node_rt_events node_rt_events = {
	PW_VERSION_IMPL_NODE_RT_EVENTS,
	.complete = recalc_params,
	.incomplete = recalc_params,
};

static void context_driver_added(void *data, struct pw_impl_node *node)
{
	struct impl *impl = data;
	struct node *n;

	n = ensure_cached_node(impl, node);
	if (n == NULL)
		return;

	set_driver_hook_state(n, true);
}

static void context_driver_removed(void *data, struct pw_impl_node *node)
{
	struct impl *impl = data;
	struct node *n;

	n = find_cached_node(impl, node);
	if (n == NULL)
		return;

	if (impl->dag_driver == node)
		reset_persistent_dag(impl);

	destroy_cached_node(n);
}

static const struct pw_context_events context_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.driver_added = context_driver_added,
	.driver_removed = context_driver_removed,
};

static bool append_cpu(struct impl *impl, int cpu)
{
	int i;

	if (cpu < 0 || cpu >= CPU_SETSIZE)
		return false;

	for (i = 0; i < impl->n_cpus; i++) {
		if (impl->cpus[i] == cpu)
			return true;
	}

	if (impl->n_cpus >= MAX_CPUS)
		return false;

	impl->cpus[impl->n_cpus++] = cpu;
	return true;
}

static int default_cpus_from_affinity(struct impl *impl)
{
	cpu_set_t cpuset;
	int cpu;

	CPU_ZERO(&cpuset);
	if (sched_getaffinity(0, sizeof(cpuset), &cpuset) < 0)
		return -errno;

	impl->n_cpus = 0;
	for (cpu = 0; cpu < CPU_SETSIZE && impl->n_cpus < MAX_CPUS; cpu++) {
		if (CPU_ISSET(cpu, &cpuset))
			append_cpu(impl, cpu);
	}

	return impl->n_cpus > 0 ? 0 : -ENOENT;
}

static int default_cpus_from_online(struct impl *impl)
{
	long online = sysconf(_SC_NPROCESSORS_ONLN);
	int cpu;

	if (online <= 0)
		return -EINVAL;

	impl->n_cpus = 0;
	for (cpu = 0; cpu < online && cpu < CPU_SETSIZE && cpu < MAX_CPUS; cpu++)
		append_cpu(impl, cpu);

	return impl->n_cpus > 0 ? 0 : -EINVAL;
}

static int parse_cpus(struct impl *impl, const char *cpus_str)
{
	struct spa_json it[3];
	int v;

	if (cpus_str == NULL) {
		int res = default_cpus_from_affinity(impl);

		if (res < 0) {
			pw_log_warn("failed to read current CPU affinity mask, using online CPUs instead: %s",
					spa_strerror(-res));
			res = default_cpus_from_online(impl);
		}
		if (res < 0) {
			errno = -res;
			return -1;
		}
		return 0;
	}

	impl->n_cpus = 0;
	spa_json_init(&it[0], cpus_str, strlen(cpus_str));
	if (spa_json_enter_array(&it[0], &it[1]) <= 0)
		spa_json_init(&it[1], cpus_str, strlen(cpus_str));

	while (spa_json_get_int(&it[1], &v) > 0) {
		if (append_cpu(impl, v))
			continue;
		pw_log_warn("ignoring invalid or unsupported CPU index %d", v);
	}

	if (impl->n_cpus == 0) {
		errno = EINVAL;
		return -1;
	}

	return 0;
}

static int parse_cpu_utilization(const char *value, double *utilization)
{
	float parsed;

	if (utilization == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (value == NULL) {
		*utilization = DEFAULT_CPU_UTILIZATION;
		return 0;
	}

	if (spa_json_parse_float(value, strlen(value), &parsed) <= 0 ||
			!isfinite(parsed) || parsed <= 0.0f || parsed > 1.0f) {
		errno = EINVAL;
		return -1;
	}

	*utilization = (double) parsed;
	return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_properties *props = NULL;
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
	impl->module = module;
	impl->cpu_utilization = DEFAULT_CPU_UTILIZATION;
	props = NULL;

	if (parse_cpus(impl, pw_properties_get(impl->props, "cpus.available")) < 0) {
		pw_log_error("invalid cpus.available configuration: %m");
		res = -errno;
		goto error;
	}
	if (parse_cpu_utilization(pw_properties_get(impl->props, "cpus.utilization"),
			&impl->cpu_utilization) < 0) {
		pw_log_error("invalid cpus.utilization configuration: %m");
		res = -errno;
		goto error;
	}

	if (impl->n_cpus <= 0) {
		pw_log_error("module-deadline requires at least one CPU");
		res = -EINVAL;
		goto error;
	}

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));
	pw_impl_module_update_properties(module, &impl->props->dict);

	pw_context_add_listener(impl->context, &impl->context_listener, &context_events, impl);

	return 0;

error:
	pw_properties_free(props);
	if (impl != NULL) {
		pw_properties_free(impl->props);
		free(impl);
	}
	return res;
}
