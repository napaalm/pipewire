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

struct impl {
	struct pw_context *context;
	struct pw_properties *props;

	struct pw_impl_module *module;

	struct spa_hook module_listener;
	struct spa_hook context_listener;

	int n_cpus;
	int cpus[MAX_CPUS];
	double cpu_utilization;

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

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct node *n, *tmp;

	spa_hook_remove(&impl->context_listener);
	spa_hook_remove(&impl->module_listener);

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

static void recalc_params(void *data)
{
	struct node *n = data;
	struct pw_impl_node *driver = n->node;
	struct impl *impl = n->impl;
	struct pw_node_target *t;
	struct node *state;
	struct node *dst_state;
	dag_t *dag = NULL;
	uint64_t period;

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

	spa_list_for_each(state, &impl->node_list, link)
		state->seen_in_graph = false;

	dag = dag_create(period, period, impl->cpu_utilization, impl->n_cpus);
	if (dag == NULL) {
		pw_log_error("failed to create DAG for node %u: %m", driver->info.id);
		goto out;
	}

	spa_list_for_each(t, &driver->rt.target_list, link) {
		struct pw_impl_node *node = t->node;
		struct pw_node_activation *na;
		pid_t tid = -1;
		uint64_t runtime;

		na = t->activation;
		if (na == NULL) {
			pw_log_warn("node %u has no activation data, skipping deadline update",
					node->info.id);
			goto out;
		}

		state = ensure_cached_node(impl, node);
		if (state == NULL) {
			pw_log_error("can't allocate deadline state for node %u: %m",
					node->info.id);
			goto out;
		}
		state->seen_in_graph = true;

		if (measure_node_runtime(node, na, period, &runtime) < 0)
			goto out;

		if (state->period == period)
			state->wcet = SPA_MAX(state->wcet, runtime);
		else
			state->wcet = runtime;
		state->period = period;

		if (state->wcet == 0) {
			pw_log_debug("node %u has no non-zero runtime sample yet, skipping deadline update",
					node->info.id);
			goto out;
		}

		if (get_dynamic_loop_tid(node, &tid) < 0)
			goto out;

		if (dag_add_node(dag, node->info.id, inflate_wcet(state->wcet), tid) < 0) {
			if (errno == EEXIST)
				continue;
			pw_log_warn("failed to add DAG node %u: %m", node->info.id);
			goto out;
		}
	}

	spa_list_for_each(t, &driver->rt.target_list, link) {
		struct pw_impl_node *node = t->node;
		struct pw_impl_port *p;
		struct pw_impl_link *l;

		state = find_cached_node(impl, node);
		if (state == NULL || !state->seen_in_graph)
			continue;

		spa_list_for_each(p, &node->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				struct pw_impl_node *node2 = l->input->node;

				dst_state = find_cached_node(impl, node2);
				if (dst_state == NULL || !dst_state->seen_in_graph)
					continue;

				if (dag_add_edge(dag, node->info.id, node2->info.id) < 0) {
					if (errno == EEXIST)
						continue;
					pw_log_warn("failed to add DAG edge %u -> %u: %m",
							node->info.id, node2->info.id);
					goto out;
				}
			}
		}
	}

	if (dag_recalculate(dag) < 0) {
		pw_log_warn("failed to compute deadline parameters for node %u: %m",
				driver->info.id);
		goto out;
	}
	if (dag_foreach_node(dag, sched_cb, impl) < 0) {
		pw_log_warn("failed to apply deadline parameters for node %u: %m",
				driver->info.id);
		goto out;
	}

out:
	dag_destroy(dag);
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
