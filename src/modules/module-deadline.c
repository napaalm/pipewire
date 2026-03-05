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

		uint64_t runtime = node->async ? na->prev_run_time : na->finish_cputime - na->awake_cputime;
		if (runtime > period)
			pw_log_warn("node %d runtime %lu exceeds period %lu", node->info.id, runtime, period);

		if (n->period == period)
			n->wcet = SPA_MAX(n->wcet, runtime);
		else
			n->wcet = runtime;
		if (n->wcet == 0) {
			abort = true;
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

		dag_add_node(dag, node->info.id, (uint64_t)(n->wcet * 1.05), tid);
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

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));
	pw_impl_module_update_properties(module, &props->dict);

	pw_context_add_listener(impl->context, &impl->context_listener, &context_events, impl);

	goto done;

error:
	free(impl);
done:
	pw_properties_free(props);

	return res;
}
