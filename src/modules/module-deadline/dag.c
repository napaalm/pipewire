/*
 * dag.c
 *
 * Library to calculate P-EDF scheduling parameters on a DAG of tasks.
 *
 * API inspired by the DAG implementation from the paper
 * "Multi-Criteria Optimization of Real-Time DAGs on Heterogeneous Platforms under P-EDF"
 * T. Cucinotta, A. Amory, G. Ara, F. Paladino, M. Di Natale
 * https://doi.org/10.1145/3592609
 *
 * Copyright (C) 2024 Antonio Napolitano and Francesco Barcherini
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <limits.h>

#include "dag.h"

static inline size_t dag_list_len(struct spa_list *list)
{
	size_t len = 0;
	dag_node_t *pos;
	spa_list_for_each(pos, list, link) len++;
	return len;
}

#define DAG_MIN_RELATIVE_DEADLINE UINT64_C(1)
/*
 * CPU-load and utilization comparisons operate on doubles derived from exact
 * integer WCET/deadline values. Keep a small tolerance local to those checks
 * so admission decisions do not flip on last-bit rounding noise.
 */
#define DAG_LOAD_EPSILON (64.0 * DBL_EPSILON)

typedef struct {
	dag_node_t **nodes;
	int count;
} node_array_t;

struct node_index_entry {
	dag_node_t *node;
	int index;
};

struct density_order_entry {
	int index;
	double density;
};

struct cpu_assignment_info {
	dag_node_t *node;
	int index;
	double density;
};

struct dag_recompute_workspace {
	node_array_t topo;
	struct node_index_entry *node_index;
	int *indegree;
	int *queue;
	uint64_t *wcet_dist;
	uint64_t *deadline_dist;
	uint64_t *path_dist;
	int *path_parent;
	bool *path_reachable;
	dag_node_t **path_nodes;
	bool *path_excluded;
	bool *reachable;
	bool *related;
	double *density;
	struct cpu_assignment_info *cpu_info;
	uint32_t *placement;
	struct density_order_entry *cpu_candidates;
	dag_node_t **sources;
	dag_node_t **sinks;
	int source_count;
	int sink_count;
};

static node_array_t dag_nodes_to_array(dag_t *g)
{
	node_array_t arr = {
		.nodes = NULL,
		.count = 0,
	};
	size_t count = dag_list_len(&g->nodes);

	if (count > INT_MAX) {
		errno = EOVERFLOW;
		arr.count = -1;
		return arr;
	}
	if (count == 0)
		return arr;

	arr.nodes = calloc(count, sizeof(*arr.nodes));
	if (!arr.nodes) {
		arr.count = -1;
		return arr;
	}
	arr.count = (int) count;

	dag_node_t *n;
	int i = 0;
	spa_list_for_each(n, &g->nodes, link)
		arr.nodes[i++] = n;

	return arr;
}

static int compare_node_index_entry(const void *a, const void *b)
{
	const struct node_index_entry *entry_a = a;
	const struct node_index_entry *entry_b = b;
	uintptr_t ptr_a = (uintptr_t) entry_a->node;
	uintptr_t ptr_b = (uintptr_t) entry_b->node;

	if (ptr_a < ptr_b)
		return -1;
	if (ptr_a > ptr_b)
		return 1;
	return 0;
}

static void build_node_index_map(struct node_index_entry *entries,
		dag_node_t **nodes, int count)
{
	for (int i = 0; i < count; i++) {
		entries[i].node = nodes[i];
		entries[i].index = i;
	}

	qsort(entries, (size_t) count, sizeof(*entries), compare_node_index_entry);
}

static int lookup_node_index(const struct node_index_entry *entries, int count,
		dag_node_t *node)
{
	struct node_index_entry key = {
		.node = node,
		.index = 0,
	};
	const struct node_index_entry *entry;

	entry = bsearch(&key, entries, (size_t) count, sizeof(*entries),
			compare_node_index_entry);
	if (!entry)
		return -1;

	return entry->index;
}

static int validate_global_timing_contract(uint64_t period, uint64_t deadline)
{
	if (period == 0 || deadline == 0 || deadline > period) {
		errno = EINVAL;
		return -1;
	}

	return 0;
}

static int validate_node_wcet(uint64_t wcet)
{
	if (wcet < DAG_MIN_RELATIVE_DEADLINE) {
		errno = EINVAL;
		return -1;
	}

	return 0;
}

static inline uint64_t node_deadline_weight(const dag_node_t *node)
{
	/* Public mutators reject zero-WCET nodes, so the proportional split
	 * remains based on WCET while still keeping deadlines strictly positive.
	 */
	return node->wcet;
}

static int add_u64_checked(uint64_t a, uint64_t b, uint64_t *out)
{
	if (UINT64_MAX - a < b) {
		errno = EOVERFLOW;
		return -1;
	}

	*out = a + b;
	return 0;
}

static int sub_u64_checked(uint64_t value, uint64_t decrement, uint64_t *out)
{
	if (decrement > value) {
		errno = EAGAIN;
		return -1;
	}

	*out = value - decrement;
	return 0;
}

static int validate_deadline_budget(uint64_t deadline_budget,
		uint64_t path_wcet, uint64_t path_deadline_weight)
{
	/* A path is infeasible if the remaining deadline cannot cover either the
	 * critical-path WCET itself or the minimum positive per-node deadlines.
	 */
	if (deadline_budget < path_wcet || deadline_budget < path_deadline_weight) {
		errno = EAGAIN;
		return -1;
	}

	return 0;
}

static int validate_graph_input_contract(const dag_t *g)
{
	const dag_node_t *node;

	if (validate_global_timing_contract(g->period, g->deadline) < 0)
		return -1;

	spa_list_for_each(node, &g->nodes, link) {
		if (validate_node_wcet(node->wcet) < 0)
			return -1;
	}

	return 0;
}

static int validate_assigned_node_parameters(const dag_t *g,
		const dag_node_t *node)
{
	if (!node->deadline_assigned ||
			node->deadline < DAG_MIN_RELATIVE_DEADLINE ||
			node->deadline > g->deadline ||
			node->deadline > g->period ||
			node->wcet < DAG_MIN_RELATIVE_DEADLINE) {
		errno = EFAULT;
		return -1;
	}

	return 0;
}

static int validate_assigned_schedule(const dag_t *g)
{
	const dag_node_t *node;

	spa_list_for_each(node, &g->nodes, link) {
		if (validate_assigned_node_parameters(g, node) < 0)
			return -1;
	}

	return 0;
}

static inline bool double_greater_eps(double value, double limit)
{
	return value > limit + DAG_LOAD_EPSILON;
}

static inline bool double_less_eps(double value, double limit)
{
	return value + DAG_LOAD_EPSILON < limit;
}

static inline bool double_equal_eps(double a, double b)
{
	return fabs(a - b) <= DAG_LOAD_EPSILON;
}

static double subtract_density_bound(double value, double decrement)
{
	double result = value - decrement;

	/* The remaining-density bound is a sum of non-negative densities.
	 * Clamp only tiny negative roundoff back to zero; larger negatives still
	 * signal an internal accounting bug instead of being hidden.
	 */
	if (result < 0.0 && fabs(result) <= DAG_LOAD_EPSILON)
		return 0.0;

	return result;
}

static void clear_assignments(dag_t *g)
{
	dag_node_t *n;

	spa_list_for_each(n, &g->nodes, link) {
		n->deadline_assigned = false;
		n->deadline = 0;
		n->cpu = DAG_CPU_INVALID;
	}
}

static void mark_dag_dirty(dag_t *g)
{
	g->dirty = true;
	clear_assignments(g);
}

dag_t *dag_create(uint64_t period, uint64_t deadline, double utilization, uint32_t num_cpus)
{
	if (validate_global_timing_contract(period, deadline) < 0)
		return NULL;
	if (!isfinite(utilization) || utilization <= 0.0 ||
			utilization > 1.0 || num_cpus == 0) {
		errno = EINVAL;
		return NULL;
	}

	dag_t *g = calloc(1, sizeof(*g));
	if (!g) return NULL;

	g->period = period;
	g->deadline = deadline;
	g->utilization = utilization;
	g->num_cpus = num_cpus;
	g->dirty = false;
	spa_list_init(&g->nodes);
	spa_list_init(&g->edges);

	return g;
}

void dag_destroy(dag_t *g)
{
	if (!g) return;

	/* Remove all edges */
	dag_edge_t *e, *etmp;
	spa_list_for_each_safe(e, etmp, &g->edges, link) {
		spa_list_remove(&e->link);
		if (e->src) spa_list_remove(&e->src_link);
		if (e->dst) spa_list_remove(&e->dst_link);
		free(e);
	}

	/* Remove all nodes */
	dag_node_t *n, *ntmp;
	spa_list_for_each_safe(n, ntmp, &g->nodes, link) {
		spa_list_remove(&n->link);
		free(n);
	}

	free(g);
}

int dag_set_global_period_deadline(dag_t *g, uint64_t period, uint64_t deadline)
{
	if (!g) { errno = EINVAL; return -1; }
	if (validate_global_timing_contract(period, deadline) < 0)
		return -1;

	if (g->period != period || g->deadline != deadline)
		mark_dag_dirty(g);
	g->period = period;
	g->deadline = deadline;
	return 0;
}

static dag_node_t *find_node(dag_t *g, uint32_t id)
{
	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		if (n->id == id) return n;
	}
	return NULL;
}

static int node_reaches(dag_t *g, dag_node_t *src, dag_node_t *dst)
{
	node_array_t arr;
	struct node_index_entry *node_index = NULL;
	bool *visited = NULL;
	int *stack = NULL;
	int src_idx, dst_idx;
	int stack_len = 0;
	int result = 0;

	if (src == dst)
		return 1;

	arr = dag_nodes_to_array(g);
	if (arr.count < 0)
		return -1;
	if (arr.count == 0)
		return 0;

	node_index = calloc((size_t) arr.count, sizeof(*node_index));
	if (!node_index)
		goto out;
	build_node_index_map(node_index, arr.nodes, arr.count);

	src_idx = lookup_node_index(node_index, arr.count, src);
	dst_idx = lookup_node_index(node_index, arr.count, dst);
	if (src_idx < 0 || dst_idx < 0) {
		errno = EFAULT;
		result = -1;
		goto out;
	}

	visited = calloc((size_t)arr.count, sizeof(*visited));
	stack = calloc((size_t)arr.count, sizeof(*stack));
	if (!visited || !stack) {
		result = -1;
		goto out;
	}

	visited[src_idx] = true;
	stack[stack_len++] = src_idx;

	while (stack_len > 0) {
		int u = stack[--stack_len];
		dag_edge_t *e;

		spa_list_for_each(e, &arr.nodes[u]->outgoing, src_link) {
			int v = lookup_node_index(node_index, arr.count, e->dst);

			if (v < 0) {
				errno = EFAULT;
				result = -1;
				goto out;
			}
			if (v == dst_idx) {
				result = 1;
				goto out;
			}
			if (!visited[v]) {
				visited[v] = true;
				stack[stack_len++] = v;
			}
		}
	}

out:
	free(stack);
	free(visited);
	free(node_index);
	free(arr.nodes);
	return result;
}

int dag_add_node(dag_t *g, uint32_t id, uint64_t wcet, pid_t tid)
{
	if (!g) { errno = EINVAL; return -1; }
	if (find_node(g, id)) {
		errno = EEXIST;
		return -1;
	}
	if (validate_node_wcet(wcet) < 0)
		return -1;
	dag_node_t *n = calloc(1, sizeof(*n));
	if (!n) return -1;
	n->id = id;
	n->wcet = wcet;
	n->tid = tid;
	n->deadline = 0;
	n->cpu = DAG_CPU_INVALID;
	n->deadline_assigned = false;
	spa_list_init(&n->outgoing);
	spa_list_init(&n->incoming);
	spa_list_append(&g->nodes, &n->link);
	mark_dag_dirty(g);
	return 0;
}

int dag_remove_node(dag_t *g, uint32_t id)
{
	if (!g) { errno = EINVAL; return -1; }
	dag_node_t *n = find_node(g, id);
	if (!n) {
		errno = ENOENT;
		return -1;
	}

	/* Remove all edges involving n */
	dag_edge_t *e, *etmp;
	spa_list_for_each_safe(e, etmp, &n->incoming, dst_link) {
		spa_list_remove(&e->link);
		spa_list_remove(&e->dst_link);
		spa_list_remove(&e->src_link);
		free(e);
	}
	spa_list_for_each_safe(e, etmp, &n->outgoing, src_link) {
		spa_list_remove(&e->link);
		spa_list_remove(&e->src_link);
		spa_list_remove(&e->dst_link);
		free(e);
	}

	spa_list_remove(&n->link);
	free(n);
	mark_dag_dirty(g);
	return 0;
}

int dag_add_edge(dag_t *g, uint32_t src_id, uint32_t dst_id)
{
	int reaches;

	if (!g) { errno = EINVAL; return -1; }
	dag_node_t *src = find_node(g, src_id);
	dag_node_t *dst = find_node(g, dst_id);
	if (!src || !dst) { errno = ENOENT; return -1; }
	if (src == dst) { errno = ELOOP; return -1; }

	/* Check if edge already exists */
	dag_edge_t *e;
	spa_list_for_each(e, &g->edges, link) {
		if (e->src == src && e->dst == dst) {
			errno = EEXIST;
			return -1;
		}
	}

	reaches = node_reaches(g, dst, src);
	if (reaches < 0)
		return -1;
	if (reaches) {
		errno = ELOOP;
		return -1;
	}

	e = calloc(1, sizeof(*e));
	if (!e) return -1;
	e->src = src;
	e->dst = dst;
	spa_list_append(&g->edges, &e->link);
	spa_list_append(&src->outgoing, &e->src_link);
	spa_list_append(&dst->incoming, &e->dst_link);
	mark_dag_dirty(g);
	return 0;
}

int dag_remove_edge(dag_t *g, uint32_t src_id, uint32_t dst_id)
{
	if (!g) { errno = EINVAL; return -1; }
	dag_node_t *src = find_node(g, src_id);
	dag_node_t *dst = find_node(g, dst_id);
	if (!src || !dst) { errno = ENOENT; return -1; }

	dag_edge_t *e, *etmp;
	spa_list_for_each_safe(e, etmp, &g->edges, link) {
		if (e->src == src && e->dst == dst) {
			spa_list_remove(&e->link);
			spa_list_remove(&e->src_link);
			spa_list_remove(&e->dst_link);
			free(e);
			mark_dag_dirty(g);
			return 0;
		}
	}
	errno = ENOENT;
	return -1;
}

int dag_set_node_wcet(dag_t *g, uint32_t id, uint64_t wcet)
{
	if (!g) { errno = EINVAL; return -1; }
	dag_node_t *n = find_node(g, id);
	if (!n) { errno = ENOENT; return -1; }
	if (validate_node_wcet(wcet) < 0)
		return -1;
	if (n->wcet != wcet)
		mark_dag_dirty(g);
	n->wcet = wcet;
	return 0;
}

/***********************************************************************
 * Scheduling Parameter Computation
 ***********************************************************************/

enum path_search_result {
	PATH_FOUND = 0,
	PATH_UNREACHABLE = 1,
};

static void clear_recompute_workspace(struct dag_recompute_workspace *ws)
{
	free(ws->sinks);
	free(ws->sources);
	free(ws->cpu_candidates);
	free(ws->placement);
	free(ws->cpu_info);
	free(ws->density);
	free(ws->related);
	free(ws->reachable);
	free(ws->path_excluded);
	free(ws->path_nodes);
	free(ws->path_reachable);
	free(ws->path_parent);
	free(ws->path_dist);
	free(ws->deadline_dist);
	free(ws->wcet_dist);
	free(ws->queue);
	free(ws->indegree);
	free(ws->node_index);
	free(ws->topo.nodes);
	memset(ws, 0, sizeof(*ws));
}

static int build_topological_order(dag_t *g, struct dag_recompute_workspace *ws)
{
	node_array_t arr;
	int front = 0, back = 0;
	int idx = 0;

	arr = dag_nodes_to_array(g);
	if (arr.count < 0)
		return -1;
	if (arr.count != ws->topo.count) {
		free(arr.nodes);
		errno = EFAULT;
		return -1;
	}
	if (arr.count == 0) {
		free(arr.nodes);
		return 0;
	}

	build_node_index_map(ws->node_index, arr.nodes, arr.count);
	memset(ws->indegree, 0, (size_t) arr.count * sizeof(*ws->indegree));

	for (int i = 0; i < arr.count; i++) {
		dag_edge_t *edge;

		spa_list_for_each(edge, &arr.nodes[i]->incoming, dst_link)
			ws->indegree[i]++;
	}

	for (int i = 0; i < arr.count; i++) {
		if (ws->indegree[i] == 0)
			ws->queue[back++] = i;
	}

	while (front < back) {
		int u = ws->queue[front++];
		dag_edge_t *edge;

		ws->topo.nodes[idx++] = arr.nodes[u];
		spa_list_for_each(edge, &arr.nodes[u]->outgoing, src_link) {
			int v = lookup_node_index(ws->node_index, arr.count, edge->dst);

			if (v < 0) {
				free(arr.nodes);
				errno = EFAULT;
				return -1;
			}

			ws->indegree[v]--;
			if (ws->indegree[v] == 0)
				ws->queue[back++] = v;
		}
	}

	free(arr.nodes);

	if (idx != ws->topo.count) {
		/* A partial order here means the graph contains a cycle. */
		errno = ELOOP;
		return -1;
	}

	build_node_index_map(ws->node_index, ws->topo.nodes, ws->topo.count);
	return 0;
}

static int build_reachability_matrix(struct dag_recompute_workspace *ws)
{
	size_t row_len = (size_t) ws->topo.count;

	memset(ws->reachable, 0, row_len * row_len * sizeof(*ws->reachable));
	memset(ws->related, 0, row_len * row_len * sizeof(*ws->related));

	for (int i = ws->topo.count - 1; i >= 0; i--) {
		dag_edge_t *edge;
		bool *row = &ws->reachable[(size_t) i * row_len];

		spa_list_for_each(edge, &ws->topo.nodes[i]->outgoing, src_link) {
			int dst_idx = lookup_node_index(ws->node_index, ws->topo.count,
					edge->dst);

			if (dst_idx < 0) {
				errno = EFAULT;
				return -1;
			}

			row[dst_idx] = true;
			for (int k = 0; k < ws->topo.count; k++) {
				if (ws->reachable[(size_t) dst_idx * row_len + (size_t) k])
					row[k] = true;
			}
		}
	}

	for (int i = 0; i < ws->topo.count; i++) {
		ws->related[(size_t) i * row_len + (size_t) i] = true;
		for (int j = i + 1; j < ws->topo.count; j++) {
			bool pair_related =
				ws->reachable[(size_t) i * row_len + (size_t) j] ||
				ws->reachable[(size_t) j * row_len + (size_t) i];

			ws->related[(size_t) i * row_len + (size_t) j] = pair_related;
			ws->related[(size_t) j * row_len + (size_t) i] = pair_related;
		}
	}

	return 0;
}

static int init_recompute_workspace(dag_t *g, struct dag_recompute_workspace *ws)
{
	size_t node_count = dag_list_len(&g->nodes);
	size_t matrix_elems = 0;

	memset(ws, 0, sizeof(*ws));

	if (node_count > INT_MAX) {
		errno = EOVERFLOW;
		return -1;
	}

	ws->topo.count = (int) node_count;
	if (ws->topo.count == 0)
		return 0;

	if ((size_t) ws->topo.count > SIZE_MAX / (size_t) ws->topo.count) {
		errno = EOVERFLOW;
		return -1;
	}
	matrix_elems = (size_t) ws->topo.count * (size_t) ws->topo.count;

	/* Cache the topological view and reusable scratch buffers for the whole
	 * recomputation so longest-path queries and CPU-load preparation do not
	 * rebuild the same topology data or churn temporary allocations.
	 */
	ws->topo.nodes = calloc(node_count, sizeof(*ws->topo.nodes));
	ws->node_index = calloc(node_count, sizeof(*ws->node_index));
	ws->indegree = calloc(node_count, sizeof(*ws->indegree));
	ws->queue = calloc(node_count, sizeof(*ws->queue));
	ws->wcet_dist = calloc(node_count, sizeof(*ws->wcet_dist));
	ws->deadline_dist = calloc(node_count, sizeof(*ws->deadline_dist));
	ws->path_dist = calloc(node_count, sizeof(*ws->path_dist));
	ws->path_parent = malloc(node_count * sizeof(*ws->path_parent));
	ws->path_reachable = calloc(node_count, sizeof(*ws->path_reachable));
	ws->path_nodes = calloc(node_count, sizeof(*ws->path_nodes));
	ws->path_excluded = calloc(node_count, sizeof(*ws->path_excluded));
	ws->reachable = calloc(matrix_elems, sizeof(*ws->reachable));
	ws->related = calloc(matrix_elems, sizeof(*ws->related));
	ws->density = calloc(node_count, sizeof(*ws->density));
	ws->cpu_info = calloc(node_count, sizeof(*ws->cpu_info));
	ws->placement = malloc(node_count * sizeof(*ws->placement));
	ws->cpu_candidates = calloc(node_count, sizeof(*ws->cpu_candidates));
	ws->sources = calloc(node_count, sizeof(*ws->sources));
	ws->sinks = calloc(node_count, sizeof(*ws->sinks));
	if (!ws->topo.nodes || !ws->node_index || !ws->indegree || !ws->queue ||
			!ws->wcet_dist || !ws->deadline_dist || !ws->path_dist ||
			!ws->path_parent || !ws->path_reachable || !ws->path_nodes ||
			!ws->path_excluded || !ws->reachable || !ws->related ||
			!ws->density || !ws->cpu_info || !ws->placement ||
			!ws->cpu_candidates || !ws->sources || !ws->sinks)
		goto fail;

	if (build_topological_order(g, ws) < 0 || build_reachability_matrix(ws) < 0)
		goto fail;

	return 0;

fail:
	clear_recompute_workspace(ws);
	return -1;
}

static int find_sources_and_sinks(dag_t *g, struct dag_recompute_workspace *ws)
{
	dag_node_t *node;

	ws->source_count = 0;
	ws->sink_count = 0;

	spa_list_for_each(node, &g->nodes, link) {
		bool has_in = !spa_list_is_empty(&node->incoming);
		bool has_out = !spa_list_is_empty(&node->outgoing);

		if (!has_in)
			ws->sources[ws->source_count++] = node;
		if (!has_out)
			ws->sinks[ws->sink_count++] = node;
	}

	return 0;
}

static int node_reaches_in_workspace(const struct dag_recompute_workspace *ws,
		dag_node_t *src, dag_node_t *dst)
{
	int src_idx, dst_idx;

	if (src == dst)
		return 1;

	src_idx = lookup_node_index(ws->node_index, ws->topo.count, src);
	dst_idx = lookup_node_index(ws->node_index, ws->topo.count, dst);
	if (src_idx < 0 || dst_idx < 0) {
		errno = EFAULT;
		return -1;
	}

	return ws->reachable[(size_t) src_idx * (size_t) ws->topo.count +
			(size_t) dst_idx] ? 1 : 0;
}

static int compute_graph_critical_bounds(struct dag_recompute_workspace *ws,
		uint64_t *critical_wcet_out,
		uint64_t *critical_deadline_weight_out)
{
	uint64_t critical_wcet = 0;
	uint64_t critical_deadline_weight = 0;

	*critical_wcet_out = 0;
	*critical_deadline_weight_out = 0;

	if (ws->topo.count == 0)
		return 0;

	memset(ws->wcet_dist, 0, (size_t) ws->topo.count * sizeof(*ws->wcet_dist));
	memset(ws->deadline_dist, 0,
			(size_t) ws->topo.count * sizeof(*ws->deadline_dist));

	for (int i = 0; i < ws->topo.count; i++) {
		uint64_t best_pred_wcet = 0;
		uint64_t best_pred_deadline = 0;
		dag_edge_t *edge;

		spa_list_for_each(edge, &ws->topo.nodes[i]->incoming, dst_link) {
			int pred_idx = lookup_node_index(ws->node_index, ws->topo.count,
					edge->src);

			if (pred_idx < 0) {
				errno = EFAULT;
				return -1;
			}

			if (ws->wcet_dist[pred_idx] > best_pred_wcet)
				best_pred_wcet = ws->wcet_dist[pred_idx];
			if (ws->deadline_dist[pred_idx] > best_pred_deadline)
				best_pred_deadline = ws->deadline_dist[pred_idx];
		}

		if (add_u64_checked(best_pred_wcet, ws->topo.nodes[i]->wcet,
					&ws->wcet_dist[i]) < 0)
			return -1;
		if (add_u64_checked(best_pred_deadline,
					node_deadline_weight(ws->topo.nodes[i]),
					&ws->deadline_dist[i]) < 0)
			return -1;

		if (ws->wcet_dist[i] > critical_wcet)
			critical_wcet = ws->wcet_dist[i];
		if (ws->deadline_dist[i] > critical_deadline_weight)
			critical_deadline_weight = ws->deadline_dist[i];
	}

	*critical_wcet_out = critical_wcet;
	*critical_deadline_weight_out = critical_deadline_weight;
	return 0;
}

static int compute_longest_path(struct dag_recompute_workspace *ws,
		dag_node_t *src, dag_node_t *dst, int *path_len_out,
		uint64_t *path_wcet_out)
{
	int src_idx = -1, dst_idx = -1;
	*path_len_out = 0;
	*path_wcet_out = 0;

	if (src == dst) {
		ws->path_nodes[0] = src;
		*path_len_out = 1;
		*path_wcet_out = src->wcet;
		return PATH_FOUND;
	}

	src_idx = lookup_node_index(ws->node_index, ws->topo.count, src);
	dst_idx = lookup_node_index(ws->node_index, ws->topo.count, dst);
	if (src_idx < 0 || dst_idx < 0 || src_idx > dst_idx)
		return PATH_UNREACHABLE;
	if (!ws->reachable[(size_t) src_idx * (size_t) ws->topo.count +
			(size_t) dst_idx])
		return PATH_UNREACHABLE;

	memset(ws->path_dist, 0,
			(size_t) ws->topo.count * sizeof(*ws->path_dist));
	memset(ws->path_reachable, 0,
			(size_t) ws->topo.count * sizeof(*ws->path_reachable));
	for (int i = 0; i < ws->topo.count; i++)
		ws->path_parent[i] = -1;

	ws->path_reachable[src_idx] = true;
	ws->path_dist[src_idx] = ws->topo.nodes[src_idx]->wcet;

	for (int i = src_idx; i <= dst_idx; i++) {
		dag_node_t *u = ws->topo.nodes[i];
		dag_edge_t *e;

		if (!ws->path_reachable[i])
			continue;

		spa_list_for_each(e, &u->outgoing, src_link) {
			int v = lookup_node_index(ws->node_index, ws->topo.count,
					e->dst);

			if (v < 0) {
				errno = EFAULT;
				return -1;
			}
			if (v > dst_idx)
				continue;

			if (!ws->path_reachable[v] ||
					ws->path_dist[i] + e->dst->wcet >
					ws->path_dist[v]) {
				ws->path_reachable[v] = true;
				ws->path_dist[v] = ws->path_dist[i] + e->dst->wcet;
				ws->path_parent[v] = i;
			}
		}
	}

	if (!ws->path_reachable[dst_idx])
		return PATH_UNREACHABLE;

	int path_len = 0;
	for (int cur = dst_idx; cur != -1; cur = ws->path_parent[cur])
		path_len++;

	for (int cur = dst_idx, pos = path_len - 1; cur != -1;
			cur = ws->path_parent[cur])
		ws->path_nodes[pos--] = ws->topo.nodes[cur];
	*path_len_out = path_len;
	*path_wcet_out = ws->path_dist[dst_idx];
	return PATH_FOUND;
}

static int compute_path_deadline_weight(dag_node_t **path, int path_len,
		uint64_t *path_deadline_weight_out)
{
	uint64_t total = 0;

	*path_deadline_weight_out = 0;

	for (int i = 0; i < path_len; i++) {
		if (add_u64_checked(total, node_deadline_weight(path[i]), &total) < 0)
			return -1;
	}

	*path_deadline_weight_out = total;
	return 0;
}

static inline uint64_t proportional_deadline(uint64_t deadline_budget,
		uint64_t node_weight, uint64_t path_deadline_weight)
{
	if (deadline_budget == 0 || node_weight == 0 || path_deadline_weight == 0)
		return 0;

	return (uint64_t) (((unsigned __int128) deadline_budget *
			(unsigned __int128) node_weight) /
			(unsigned __int128) path_deadline_weight);
}

static int assign_or_tighten_deadline(dag_node_t *node, uint64_t deadline)
{
	if (deadline < DAG_MIN_RELATIVE_DEADLINE) {
		errno = EAGAIN;
		return -1;
	}

	if (!node->deadline_assigned) {
		node->deadline = deadline;
		node->deadline_assigned = true;
	} else if (node->deadline > deadline) {
		node->deadline = deadline;
	}

	return 0;
}

static int assign_deadlines_recursive(dag_t *g,
		struct dag_recompute_workspace *ws, dag_node_t *src,
		dag_node_t *dst, uint64_t D)
{
	if (src == dst) {
		if (D == 0 && src->deadline_assigned)
			return 0;

		if (validate_deadline_budget(D, src->wcet,
					node_deadline_weight(src)) < 0)
			return -1;

		return assign_or_tighten_deadline(src, D);
	}

	/* Phase A: compute the critical path for this subproblem. */
	dag_node_t **P = ws->path_nodes;
	int path_len = 0;
	uint64_t L = 0;
	uint64_t path_deadline_weight = 0;
	int path_result = compute_longest_path(ws, src, dst, &path_len, &L);
	if (path_result < 0)
		return -1;
	if (path_result == PATH_UNREACHABLE)
		return 0;
	if (compute_path_deadline_weight(P, path_len, &path_deadline_weight) < 0)
		return -1;

	if (D == 0) {
		for (int i = 0; i < path_len; i++) {
			if (!P[i]->deadline_assigned) {
				errno = EAGAIN;
				return -1;
			}
		}

		return 0;
	}

	/* Phase B: discount already-assigned tighter deadlines from the
	 * residual budget, residual path WCET and minimum positive-deadline
	 * budget before assigning this level.
	 */
	uint64_t residual_deadline = D;
	uint64_t residual_wcet = L;
	uint64_t residual_deadline_weight = path_deadline_weight;
	bool src_discounted = false;
	uint64_t discounted_src_deadline = 0;
	bool *excluded = ws->path_excluded;

	memset(excluded, 0, (size_t) path_len * sizeof(*excluded));

	bool changed = true;
	while (changed) {
		changed = false;
		if (residual_deadline_weight == 0)
			break;

		for (int i = 0; i < path_len; i++) {
			if (excluded[i])
				continue;

			dag_node_t *ni = P[i];
			uint64_t node_weight = node_deadline_weight(ni);
			uint64_t d_prime = proportional_deadline(residual_deadline,
					node_weight, residual_deadline_weight);

			if (ni->deadline_assigned && ni->deadline < d_prime) {
				if (sub_u64_checked(residual_deadline, ni->deadline,
							&residual_deadline) < 0 ||
						sub_u64_checked(residual_wcet, ni->wcet,
							&residual_wcet) < 0 ||
						sub_u64_checked(residual_deadline_weight,
							node_weight,
							&residual_deadline_weight) < 0) {
					return -1;
				}
				if (ni == src) {
					src_discounted = true;
					discounted_src_deadline = ni->deadline;
				}
				excluded[i] = true;
				if (validate_deadline_budget(residual_deadline, residual_wcet,
							residual_deadline_weight) < 0 &&
						residual_deadline_weight != 0)
					return -1;
				changed = true;
				break;
			}
		}
	}

	if (residual_deadline_weight == 0)
		return 0;
	if (validate_deadline_budget(residual_deadline, residual_wcet,
				residual_deadline_weight) < 0)
		return -1;

	/* Phase C: assign/tighten the endpoints with the updated residual budget
	 * and recurse with the budget left after the current source.
	 *
	 * The recursive budget must be derived from residual_deadline, not from
	 * the original D input. Using the stale input would re-introduce deadline
	 * budget already discounted above for tighter pre-assigned nodes. The
	 * same updated residual budget also carries the minimum positive-deadline
	 * reservation for the nodes still left on this path.
	 */
	dag_node_t *n_src = P[0];
	uint64_t src_weight = node_deadline_weight(n_src);
	uint64_t d_prime_src = proportional_deadline(residual_deadline,
			src_weight, residual_deadline_weight);
	if (assign_or_tighten_deadline(n_src, d_prime_src) < 0)
		return -1;

	dag_node_t *n_dst = P[path_len - 1];
	uint64_t dst_weight = node_deadline_weight(n_dst);
	uint64_t d_prime_dst = proportional_deadline(residual_deadline,
			dst_weight, residual_deadline_weight);
	if (assign_or_tighten_deadline(n_dst, d_prime_dst) < 0)
		return -1;

	uint64_t D_residual = 0;
	if (src_discounted) {
		uint64_t refund = 0;

		/* Phase B already removed the source from residual_deadline. If
		 * phase C tightened it again, refund the delta so descendants see
		 * the true post-source residual budget.
		 */
		if (discounted_src_deadline < n_src->deadline ||
				sub_u64_checked(discounted_src_deadline,
					n_src->deadline, &refund) < 0 ||
				add_u64_checked(residual_deadline, refund,
					&D_residual) < 0)
			return -1;
	} else {
		if (sub_u64_checked(residual_deadline, n_src->deadline,
					&D_residual) < 0)
			return -1;
	}

	/* The workspace path buffers are scratch-only. Finish the current level
	 * before recursing because child subproblems overwrite the same buffers.
	 */
	dag_edge_t *e;
	spa_list_for_each(e, &src->outgoing, src_link) {
		if (e->dst != dst) {
			int reaches = node_reaches_in_workspace(ws, e->dst, dst);

			/* For generic DAGs, a source can fan out toward different sinks.
			 * Recurse only into successors that still belong to this source ->
			 * sink subproblem.
			 */
			if (reaches < 0)
				return -1;
			if (!reaches)
				continue;

			int r = assign_deadlines_recursive(g, ws, e->dst, dst,
					D_residual);
			if (r < 0)
				return r;
		}
	}

	return 0;
}

static int compare_density_desc(double a_density, int a_key,
		double b_density, int b_key)
{
	if (a_density < b_density)
		return 1;
	if (a_density > b_density)
		return -1;
	if (a_key > b_key)
		return 1;
	if (a_key < b_key)
		return -1;
	return 0;
}

static int compare_density_order_entry_desc(const void *a, const void *b)
{
	const struct density_order_entry *entry_a = a;
	const struct density_order_entry *entry_b = b;

	/* Visit denser candidates first for the branch-and-bound search.
	 * Break ties by the topological index to keep traversal deterministic.
	 */
	return compare_density_desc(entry_a->density, entry_a->index,
			entry_b->density, entry_b->index);
}

static int compare_cpu_assignment_info_desc(const void *a, const void *b)
{
	const struct cpu_assignment_info *info_a = a;
	const struct cpu_assignment_info *info_b = b;

	/* Place denser tasks first. Equal-density nodes keep topological order. */
	return compare_density_desc(info_a->density, info_a->index,
			info_b->density, info_b->index);
}

static void search_max_unrelated_density(const bool *related, int node_count,
		const struct density_order_entry *candidates, int candidate_count,
		double current_density, double remaining_density, double *best_density)
{
	const struct density_order_entry *current;
	int v;
	double remaining_without_v;
	int include_count = 0;
	double include_remaining = 0.0;
	struct density_order_entry next_candidates[
		candidate_count > 1 ? candidate_count - 1 : 1];

	if (double_greater_eps(current_density, *best_density))
		*best_density = current_density;

	if (candidate_count == 0 ||
			!double_greater_eps(current_density + remaining_density,
				*best_density))
		return;

	current = &candidates[0];
	v = current->index;
	remaining_without_v = subtract_density_bound(remaining_density,
			current->density);

	for (int i = 1; i < candidate_count; i++) {
		const struct density_order_entry *candidate = &candidates[i];

		if (!related[(size_t) v * (size_t) node_count +
				(size_t) candidate->index]) {
			next_candidates[include_count++] = *candidate;
			include_remaining += candidate->density;
		}
	}

	search_max_unrelated_density(related, node_count,
			next_candidates, include_count,
			current_density + current->density, include_remaining,
			best_density);

	if (!double_greater_eps(current_density + remaining_without_v,
			*best_density))
		return;

	search_max_unrelated_density(related, node_count,
			&candidates[1], candidate_count - 1,
			current_density, remaining_without_v, best_density);
}

static double compute_cpu_load_exact(const bool *related, const double *density,
		const uint32_t *placement, int node_count, uint32_t cpu,
		int tentative_idx, uint32_t tentative_cpu,
		struct density_order_entry *candidates)
{
	double remaining_density = 0.0;
	double best_density = 0.0;
	int candidate_count = 0;

	for (int i = 0; i < node_count; i++) {
		uint32_t assigned_cpu = placement[i];

		if (i == tentative_idx)
			assigned_cpu = tentative_cpu;

		if (assigned_cpu != cpu)
			continue;

		candidates[candidate_count].index = i;
		candidates[candidate_count].density = density[i];
		candidate_count++;
		remaining_density += density[i];
	}

	if (candidate_count == 0)
		return 0.0;

	qsort(candidates, (size_t) candidate_count, sizeof(*candidates),
			compare_density_order_entry_desc);
	search_max_unrelated_density(related, node_count,
			candidates, candidate_count, 0.0,
			remaining_density, &best_density);
	return best_density;
}

static bool prefer_cpu_choice(double load, uint32_t cpu,
		double best_load, int best_cpu)
{
	if (best_cpu < 0)
		return true;
	if (double_less_eps(load, best_load))
		return true;

	/* When two CPUs yield the same resulting load, keep placement
	 * deterministic by taking the lowest CPU index.
	 */
	return double_equal_eps(load, best_load) && (int) cpu < best_cpu;
}

static int assign_cpus(dag_t *g, struct dag_recompute_workspace *ws)
{
	int node_count = ws->topo.count;

	/* For a single DAG, the load of one CPU is the maximum total density of
	 * any pairwise unrelated set assigned there, not the raw sum of all node
	 * densities. Reuse the topological view and relatedness matrix prepared
	 * once for this recomputation, then solve the maximum-weight unrelated set
	 * problem exactly for each tentative CPU.
	 */
	if (node_count == 0)
		return 0;

	for (int i = 0; i < node_count; i++) {
		uint64_t denom;

		if (validate_assigned_node_parameters(g, ws->topo.nodes[i]) < 0)
			return -1;

		denom = ws->topo.nodes[i]->deadline < g->period ?
				ws->topo.nodes[i]->deadline : g->period;

		/* Deadline/period remain exact uint64_t values; density is the
		 * derived floating-point load used only for CPU admission.
		 */
		ws->density[i] = (double) ws->topo.nodes[i]->wcet / (double) denom;
		ws->cpu_info[i].node = ws->topo.nodes[i];
		ws->cpu_info[i].index = i;
		ws->cpu_info[i].density = ws->density[i];
		ws->placement[i] = UINT32_MAX;
	}

	qsort(ws->cpu_info, (size_t) node_count, sizeof(*ws->cpu_info),
			compare_cpu_assignment_info_desc);

	for (int i = 0; i < node_count; i++) {
		int chosen_cpu = -1;
		double chosen_load = DBL_MAX;

		for (uint32_t cpu = 0; cpu < g->num_cpus; cpu++) {
			double load = compute_cpu_load_exact(ws->related, ws->density,
					ws->placement, node_count, cpu,
					ws->cpu_info[i].index, cpu,
					ws->cpu_candidates);

			if (prefer_cpu_choice(load, cpu, chosen_load, chosen_cpu)) {
				chosen_load = load;
				chosen_cpu = (int) cpu;
			}
		}

		if (chosen_cpu < 0 ||
				double_greater_eps(chosen_load, g->utilization)) {
			errno = EAGAIN;
			return -1;
		}

		ws->placement[ws->cpu_info[i].index] = (uint32_t) chosen_cpu;
		ws->cpu_info[i].node->cpu = (uint32_t) chosen_cpu;
	}

	return 0;
}

int dag_recalculate(dag_t *g)
{
	struct dag_recompute_workspace ws = { 0 };
	int res = -1;
	uint64_t critical_wcet = 0;
	uint64_t critical_deadline_weight = 0;

	if (!g) { errno = EINVAL; return -1; }

	g->dirty = true;

	if (validate_graph_input_contract(g) < 0)
		goto out;

	if (spa_list_is_empty(&g->nodes)) {
		g->dirty = false;
		return 0;
	}

	clear_assignments(g);

	if (init_recompute_workspace(g, &ws) < 0)
		goto out;

	/* Fail before deadline splitting or CPU placement if the DAG-wide
	 * critical path already exceeds the end-to-end budget.
	 */
	if (compute_graph_critical_bounds(&ws, &critical_wcet,
				&critical_deadline_weight) < 0)
		goto out;
	if (validate_deadline_budget(g->deadline, critical_wcet,
				critical_deadline_weight) < 0)
		goto out;

	if (find_sources_and_sinks(g, &ws) < 0)
		goto out;
	if (ws.source_count == 0 || ws.sink_count == 0) {
		errno = EINVAL;
		goto out;
	}

	for (int i = 0; i < ws.source_count; i++) {
		for (int j = 0; j < ws.sink_count; j++) {
			int reaches = node_reaches_in_workspace(&ws, ws.sources[i],
					ws.sinks[j]);

			/* Generic DAGs can have multiple sources/sinks or disconnected
			 * components. Only reachable source/sink pairs participate in
			 * deadline splitting; unreachable combinations are ignored.
			 */
			if (reaches < 0)
				goto out;
			if (!reaches)
				continue;

			if (assign_deadlines_recursive(g, &ws, ws.sources[i],
					ws.sinks[j],
					g->deadline) < 0)
				goto out;
		}
	}

	if (validate_assigned_schedule(g) < 0)
		goto out;

	if (assign_cpus(g, &ws) < 0)
		goto out;

	res = 0;
	g->dirty = false;

out:
	clear_recompute_workspace(&ws);
	if (res < 0) {
		clear_assignments(g);
		g->dirty = true;
	}
	return res;
}

int dag_foreach_node(dag_t *g, dag_node_callback_t cb, void *data)
{
	if (!g || !cb) { errno = EINVAL; return -1; }
	if (g->dirty && dag_recalculate(g) < 0)
		return -1;

	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		cb(data, n->tid, n->wcet, n->deadline, g->period, n->cpu);
	}
	return 0;
}

void dag_print(dag_t *g)
{
	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		pw_log_debug("Node %u|%u with wcet %lu and deadline %lu", n->id, n->tid, n->wcet, n->deadline);
		dag_edge_t *e;
		spa_list_for_each(e, &n->outgoing, src_link) {
			pw_log_debug("  Edge to node %u|%u", e->dst->id, e->dst->tid);
		}
	}
}
