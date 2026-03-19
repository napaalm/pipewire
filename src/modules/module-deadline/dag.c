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

typedef struct {
	dag_node_t **nodes;
	int count;
} node_array_t;

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

static int find_node_index(const node_array_t *arr, dag_node_t *node)
{
	for (int i = 0; i < arr->count; i++) {
		if (arr->nodes[i] == node)
			return i;
	}

	return -1;
}

static inline uint64_t node_deadline_weight(const dag_node_t *node)
{
	/* The integer scheduler parameters exported by this library must stay
	 * strictly positive, so every node consumes at least one deadline unit.
	 */
	return node->wcet > 0 ? node->wcet : DAG_MIN_RELATIVE_DEADLINE;
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

dag_t *dag_create(uint64_t period, uint64_t deadline, float utilization, uint32_t num_cpus)
{
	if (period == 0 || deadline == 0 || utilization <= 0.0f || utilization > 1.0f || num_cpus == 0) {
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
	if (period == 0 || deadline == 0) { errno = EINVAL; return -1; }

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

	src_idx = find_node_index(&arr, src);
	dst_idx = find_node_index(&arr, dst);
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
			int v = find_node_index(&arr, e->dst);

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

static int topological_sort(dag_t *g, dag_node_t **out, int *out_count)
{
	node_array_t arr = dag_nodes_to_array(g);
	int *indegree = NULL;
	int *queue = NULL;
	int front = 0, back = 0;
	int idx = 0;
	int result = -1;

	*out_count = 0;

	if (arr.count < 0)
		return -1;
	if (arr.count == 0) {
		free(arr.nodes);
		return 0;
	}

	indegree = calloc((size_t)arr.count, sizeof(*indegree));
	queue = calloc((size_t)arr.count, sizeof(*queue));
	if (!indegree || !queue)
		goto out;

	for (int i = 0; i < arr.count; i++) {
		dag_edge_t *e;

		spa_list_for_each(e, &arr.nodes[i]->incoming, dst_link)
			indegree[i]++;
	}

	for (int i = 0; i < arr.count; i++) {
		if (indegree[i] == 0)
			queue[back++] = i;
	}

	while (front < back) {
		int u = queue[front++];
		dag_edge_t *edge;

		out[idx++] = arr.nodes[u];
		spa_list_for_each(edge, &arr.nodes[u]->outgoing, src_link) {
			int v = find_node_index(&arr, edge->dst);

			if (v < 0) {
				errno = EFAULT;
				goto out;
			}

			indegree[v]--;
			if (indegree[v] == 0)
				queue[back++] = v;
		}
	}

	if (idx != arr.count) {
		/* A partial order here means the graph contains a cycle. */
		errno = ELOOP;
		goto out;
	}

	*out_count = idx;
	result = 0;

out:
	free(queue);
	free(indegree);
	free(arr.nodes);
	return result;
}

static int find_sources_and_sinks(dag_t *g, dag_node_t ***sources, int *nsources,
		dag_node_t ***sinks, int *nsinks)
{
	size_t count = dag_list_len(&g->nodes);
	dag_node_t **sarr = NULL;
	dag_node_t **tarr = NULL;
	int si = 0, ti = 0;

	if (count > 0) {
		sarr = calloc(count, sizeof(*sarr));
		tarr = calloc(count, sizeof(*tarr));
		if (!sarr || !tarr) {
			free(sarr);
			free(tarr);
			return -1;
		}
	}

	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		bool has_in = !spa_list_is_empty(&n->incoming);
		bool has_out = !spa_list_is_empty(&n->outgoing);

		if (!has_in)
			sarr[si++] = n;
		if (!has_out)
			tarr[ti++] = n;
	}

	*sources = sarr;
	*nsources = si;
	*sinks = tarr;
	*nsinks = ti;
	return 0;
}

static int compute_graph_critical_bounds(const node_array_t *topo,
		uint64_t *critical_wcet_out,
		uint64_t *critical_deadline_weight_out)
{
	uint64_t *wcet_dist = NULL;
	uint64_t *deadline_dist = NULL;
	uint64_t critical_wcet = 0;
	uint64_t critical_deadline_weight = 0;
	int result = -1;

	*critical_wcet_out = 0;
	*critical_deadline_weight_out = 0;

	if (topo->count == 0)
		return 0;

	wcet_dist = calloc((size_t) topo->count, sizeof(*wcet_dist));
	deadline_dist = calloc((size_t) topo->count, sizeof(*deadline_dist));
	if (!wcet_dist || !deadline_dist)
		goto out;

	for (int i = 0; i < topo->count; i++) {
		uint64_t best_pred_wcet = 0;
		uint64_t best_pred_deadline = 0;
		dag_edge_t *edge;

		spa_list_for_each(edge, &topo->nodes[i]->incoming, dst_link) {
			int pred_idx = find_node_index(topo, edge->src);

			if (pred_idx < 0) {
				errno = EFAULT;
				goto out;
			}

			if (wcet_dist[pred_idx] > best_pred_wcet)
				best_pred_wcet = wcet_dist[pred_idx];
			if (deadline_dist[pred_idx] > best_pred_deadline)
				best_pred_deadline = deadline_dist[pred_idx];
		}

		if (add_u64_checked(best_pred_wcet, topo->nodes[i]->wcet,
					&wcet_dist[i]) < 0)
			goto out;
		if (add_u64_checked(best_pred_deadline,
					node_deadline_weight(topo->nodes[i]),
					&deadline_dist[i]) < 0)
			goto out;

		if (wcet_dist[i] > critical_wcet)
			critical_wcet = wcet_dist[i];
		if (deadline_dist[i] > critical_deadline_weight)
			critical_deadline_weight = deadline_dist[i];
	}

	*critical_wcet_out = critical_wcet;
	*critical_deadline_weight_out = critical_deadline_weight;
	result = 0;

out:
	free(deadline_dist);
	free(wcet_dist);
	return result;
}

static int compute_longest_path(dag_t *g, dag_node_t *src, dag_node_t *dst,
		dag_node_t ***path_out, int *path_len_out, uint64_t *path_wcet_out)
{
	dag_node_t **topo = NULL;
	uint64_t *dist = NULL;
	int *parent = NULL;
	bool *reachable = NULL;
	int topo_len = 0;
	int src_idx = -1, dst_idx = -1;
	int result = -1;

	*path_out = NULL;
	*path_len_out = 0;
	*path_wcet_out = 0;

	if (src == dst) {
		dag_node_t **p = malloc(sizeof(*p));

		if (!p)
			return -1;

		p[0] = src;
		*path_out = p;
		*path_len_out = 1;
		*path_wcet_out = src->wcet;
		return PATH_FOUND;
	}

	topo = calloc(dag_list_len(&g->nodes), sizeof(*topo));
	if (!topo)
		return -1;

	if (topological_sort(g, topo, &topo_len) < 0)
		goto out;
	if (topo_len == 0) {
		result = PATH_UNREACHABLE;
		goto out;
	}

	for (int i = 0; i < topo_len; i++) {
		if (topo[i] == src)
			src_idx = i;
		if (topo[i] == dst)
			dst_idx = i;
	}

	if (src_idx < 0 || dst_idx < 0 || src_idx > dst_idx) {
		result = PATH_UNREACHABLE;
		goto out;
	}

	dist = calloc((size_t)topo_len, sizeof(*dist));
	parent = malloc((size_t)topo_len * sizeof(*parent));
	reachable = calloc((size_t)topo_len, sizeof(*reachable));
	if (!dist || !parent || !reachable)
		goto out;

	for (int i = 0; i < topo_len; i++)
		parent[i] = -1;

	reachable[src_idx] = true;
	dist[src_idx] = topo[src_idx]->wcet;

	for (int i = src_idx; i <= dst_idx; i++) {
		dag_node_t *u = topo[i];
		dag_edge_t *e;

		if (!reachable[i])
			continue;

		spa_list_for_each(e, &u->outgoing, src_link) {
			int v = -1;

			for (int k = i + 1; k <= dst_idx; k++) {
				if (topo[k] == e->dst) {
					v = k;
					break;
				}
			}
			if (v >= 0 && (!reachable[v] ||
					dist[i] + e->dst->wcet > dist[v])) {
				reachable[v] = true;
				dist[v] = dist[i] + e->dst->wcet;
				parent[v] = i;
			}
		}
	}

	if (!reachable[dst_idx]) {
		result = PATH_UNREACHABLE;
		goto out;
	}

	int path_len = 0;
	for (int cur = dst_idx; cur != -1; cur = parent[cur])
		path_len++;

	dag_node_t **path = malloc((size_t)path_len * sizeof(*path));
	if (!path)
		goto out;

	for (int cur = dst_idx, pos = path_len - 1; cur != -1; cur = parent[cur])
		path[pos--] = topo[cur];

	*path_out = path;
	*path_len_out = path_len;
	*path_wcet_out = dist[dst_idx];
	result = PATH_FOUND;

out:
	free(reachable);
	free(parent);
	free(dist);
	free(topo);
	return result;
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

static int assign_deadlines_recursive(dag_t *g, dag_node_t *src, dag_node_t *dst, uint64_t D)
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
	dag_node_t **P = NULL;
	int path_len = 0;
	uint64_t L = 0;
	uint64_t path_deadline_weight = 0;
	int path_result = compute_longest_path(g, src, dst, &P, &path_len, &L);
	if (path_result < 0)
		return -1;
	if (path_result == PATH_UNREACHABLE)
		return 0;
	if (compute_path_deadline_weight(P, path_len, &path_deadline_weight) < 0) {
		free(P);
		return -1;
	}

	if (D == 0) {
		for (int i = 0; i < path_len; i++) {
			if (!P[i]->deadline_assigned) {
				free(P);
				errno = EAGAIN;
				return -1;
			}
		}

		free(P);
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
	bool *excluded = calloc((size_t)path_len, sizeof(*excluded));
	if (!excluded) {
		free(P);
		return -1;
	}

	bool changed = true;
	while (changed) {
		changed = false;
		if (residual_deadline_weight == 0)
			break;

		for (int i=0; i<path_len; i++) {
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
					free(excluded);
					free(P);
					return -1;
				}
				if (ni == src) {
					src_discounted = true;
					discounted_src_deadline = ni->deadline;
				}
				excluded[i] = true;
				if (validate_deadline_budget(residual_deadline, residual_wcet,
							residual_deadline_weight) < 0 &&
						residual_deadline_weight != 0) {
					free(excluded);
					free(P);
					return -1;
				}
				changed = true;
				break;
			}
		}
	}

	if (residual_deadline_weight == 0) {
		free(excluded);
		free(P);
		return 0;
	}
	if (validate_deadline_budget(residual_deadline, residual_wcet,
				residual_deadline_weight) < 0) {
		free(excluded);
		free(P);
		return -1;
	}

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
	if (assign_or_tighten_deadline(n_src, d_prime_src) < 0) {
		free(excluded);
		free(P);
		return -1;
	}
	
	dag_node_t *n_dst = P[path_len - 1];
	uint64_t dst_weight = node_deadline_weight(n_dst);
	uint64_t d_prime_dst = proportional_deadline(residual_deadline,
			dst_weight, residual_deadline_weight);
	if (assign_or_tighten_deadline(n_dst, d_prime_dst) < 0) {
		free(excluded);
		free(P);
		return -1;
	}

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
					&D_residual) < 0) {
			free(excluded);
			free(P);
			return -1;
		}
	} else {
		if (sub_u64_checked(residual_deadline, n_src->deadline,
					&D_residual) < 0) {
			free(excluded);
			free(P);
			return -1;
		}
	}

	dag_edge_t *e;
	spa_list_for_each(e, &src->outgoing, src_link) {
		if (e->dst != dst) {
			int reaches = node_reaches(g, e->dst, dst);

			/* For generic DAGs, a source can fan out toward different sinks.
			 * Recurse only into successors that still belong to this source ->
			 * sink subproblem.
			 */
			if (reaches < 0) {
				free(excluded);
				free(P);
				return -1;
			}
			if (!reaches)
				continue;

			int r = assign_deadlines_recursive(g, e->dst, dst, D_residual);
			if (r < 0) {
				free(excluded);
				free(P);
				return r;
			}
		}
	}

	free(excluded);
	free(P);
	return 0;
}

static void sort_indices_by_density_desc(int *indices, int count, const double *density)
{
	for (int i = 1; i < count; i++) {
		int cur = indices[i];
		int j = i - 1;

		while (j >= 0 && density[indices[j]] < density[cur]) {
			indices[j + 1] = indices[j];
			j--;
		}
		indices[j + 1] = cur;
	}
}

static int build_relatedness_matrix(dag_t *g, dag_node_t ***nodes_out,
		bool **related_out, double **density_out, int *count_out)
{
	size_t node_count;
	node_array_t topo = {
		.nodes = NULL,
		.count = 0,
	};
	bool *reachable = NULL;
	bool *related = NULL;
	double *density = NULL;
	size_t matrix_elems;
	int result = -1;

	*nodes_out = NULL;
	*related_out = NULL;
	*density_out = NULL;
	*count_out = 0;

	node_count = dag_list_len(&g->nodes);
	if (node_count > INT_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	topo.count = (int) node_count;
	if (topo.count == 0)
		return 0;

	topo.nodes = calloc((size_t) topo.count, sizeof(*topo.nodes));
	if (!topo.nodes)
		return -1;

	if (topological_sort(g, topo.nodes, &topo.count) < 0)
		goto out;

	if ((size_t) topo.count > SIZE_MAX / (size_t) topo.count) {
		errno = EOVERFLOW;
		goto out;
	}

	matrix_elems = (size_t) topo.count * (size_t) topo.count;
	reachable = calloc(matrix_elems, sizeof(*reachable));
	related = calloc(matrix_elems, sizeof(*related));
	density = calloc((size_t) topo.count, sizeof(*density));
	if (!reachable || !related || !density)
		goto out;

	for (int i = 0; i < topo.count; i++) {
		uint64_t denom = topo.nodes[i]->deadline < g->period ?
				topo.nodes[i]->deadline : g->period;

		/* Deadline splitting already validated that every assigned node has
		 * a strictly positive relative deadline. Reaching denom == 0 here
		 * means the internal state is inconsistent.
		 */
		if (!topo.nodes[i]->deadline_assigned || denom == 0) {
			errno = EFAULT;
			goto out;
		}

		density[i] = (double) topo.nodes[i]->wcet / (double) denom;
	}

	for (int i = topo.count - 1; i >= 0; i--) {
		dag_edge_t *e;

		spa_list_for_each(e, &topo.nodes[i]->outgoing, src_link) {
			int dst_idx = find_node_index(&topo, e->dst);

			if (dst_idx < 0) {
				errno = EFAULT;
				goto out;
			}

			reachable[(size_t) i * (size_t) topo.count + (size_t) dst_idx] = true;
			for (int k = 0; k < topo.count; k++) {
				if (reachable[(size_t) dst_idx * (size_t) topo.count + (size_t) k]) {
					reachable[(size_t) i * (size_t) topo.count + (size_t) k] = true;
				}
			}
		}
	}

	for (int i = 0; i < topo.count; i++) {
		related[(size_t) i * (size_t) topo.count + (size_t) i] = true;
		for (int j = i + 1; j < topo.count; j++) {
			bool pair_related =
				reachable[(size_t) i * (size_t) topo.count + (size_t) j] ||
				reachable[(size_t) j * (size_t) topo.count + (size_t) i];

			related[(size_t) i * (size_t) topo.count + (size_t) j] = pair_related;
			related[(size_t) j * (size_t) topo.count + (size_t) i] = pair_related;
		}
	}

	*nodes_out = topo.nodes;
	*related_out = related;
	*density_out = density;
	*count_out = topo.count;
	result = 0;

out:
	free(reachable);
	if (result < 0) {
		free(density);
		free(related);
		free(topo.nodes);
	}
	return result;
}

static void search_max_unrelated_density(const bool *related, const double *density,
		int node_count, const int *candidates, int candidate_count,
		double current_density, double remaining_density, double *best_density)
{
	int v;
	double remaining_without_v;
	int include_count = 0;
	double include_remaining = 0.0;
	int next_candidates[candidate_count > 1 ? candidate_count - 1 : 1];

	if (current_density > *best_density)
		*best_density = current_density;

	if (candidate_count == 0 ||
			current_density + remaining_density <= *best_density)
		return;

	v = candidates[0];
	remaining_without_v = remaining_density - density[v];

	for (int i = 1; i < candidate_count; i++) {
		int candidate = candidates[i];

		if (!related[(size_t) v * (size_t) node_count + (size_t) candidate]) {
			next_candidates[include_count++] = candidate;
			include_remaining += density[candidate];
		}
	}

	search_max_unrelated_density(related, density, node_count,
			next_candidates, include_count,
			current_density + density[v], include_remaining,
			best_density);

	if (current_density + remaining_without_v <= *best_density)
		return;

	search_max_unrelated_density(related, density, node_count,
			&candidates[1], candidate_count - 1,
			current_density, remaining_without_v, best_density);
}

static double compute_cpu_load_exact(const bool *related, const double *density,
		const uint32_t *placement, int node_count, uint32_t cpu,
		int tentative_idx, uint32_t tentative_cpu)
{
	double remaining_density = 0.0;
	double best_density = 0.0;
	int candidate_count = 0;
	int candidates[node_count > 0 ? node_count : 1];

	for (int i = 0; i < node_count; i++) {
		uint32_t assigned_cpu = placement[i];

		if (i == tentative_idx)
			assigned_cpu = tentative_cpu;

		if (assigned_cpu != cpu)
			continue;

		candidates[candidate_count++] = i;
		remaining_density += density[i];
	}

	if (candidate_count == 0)
		return 0.0;

	sort_indices_by_density_desc(candidates, candidate_count, density);
	search_max_unrelated_density(related, density, node_count,
			candidates, candidate_count, 0.0,
			remaining_density, &best_density);
	return best_density;
}

static int assign_cpus(dag_t *g)
{
	dag_node_t **nodes = NULL;
	bool *related = NULL;
	double *density = NULL;
	uint32_t *placement = NULL;
	int node_count = 0;
	int result = -1;

	struct cpu_assignment_info {
		dag_node_t *node;
		int index;
		double density;
	} *info = NULL;

	/* For a single DAG, the load of one CPU is the maximum total density of
	 * any pairwise unrelated set assigned there, not the raw sum of all node
	 * densities. We build the exact reachability closure once, then solve the
	 * maximum-weight unrelated set problem exactly for each tentative CPU.
	 */
	if (build_relatedness_matrix(g, &nodes, &related, &density, &node_count) < 0)
		return -1;
	if (node_count == 0)
		return 0;

	info = calloc((size_t) node_count, sizeof(*info));
	placement = malloc((size_t) node_count * sizeof(*placement));
	if (!info || !placement)
		goto out;

	for (int i = 0; i < node_count; i++) {
		info[i].node = nodes[i];
		info[i].index = i;
		info[i].density = density[i];
		placement[i] = UINT32_MAX;
	}

	for (int x = 0; x < node_count - 1; x++) {
		for (int y = x + 1; y < node_count; y++) {
			if (info[x].density < info[y].density) {
				struct cpu_assignment_info tmp = info[x];

				info[x] = info[y];
				info[y] = tmp;
			}
		}
	}

	for (int i = 0; i < node_count; i++) {
		int chosen_cpu = -1;
		double chosen_load = DBL_MAX;

		for (uint32_t cpu = 0; cpu < g->num_cpus; cpu++) {
			double load = compute_cpu_load_exact(related, density,
					placement, node_count, cpu,
					info[i].index, cpu);

			if (load < chosen_load) {
				chosen_load = load;
				chosen_cpu = (int) cpu;
			}
		}

		if (chosen_cpu < 0 || chosen_load > (double) g->utilization) {
			errno = EAGAIN;
			goto out;
		}

		placement[info[i].index] = (uint32_t) chosen_cpu;
		info[i].node->cpu = (uint32_t) chosen_cpu;
	}

	result = 0;

out:
	free(placement);
	free(info);
	free(density);
	free(related);
	free(nodes);
	return result;
}

int dag_recalculate(dag_t *g)
{
	dag_node_t **sources = NULL, **sinks = NULL, **topo = NULL;
	node_array_t topo_arr = {
		.nodes = NULL,
		.count = 0,
	};
	size_t node_count;
	int nsources = 0, nsinks = 0;
	int topo_len = 0;
	int res = -1;
	uint64_t critical_wcet = 0;
	uint64_t critical_deadline_weight = 0;

	if (!g) { errno = EINVAL; return -1; }

	g->dirty = true;

	if (spa_list_is_empty(&g->nodes)) {
		g->dirty = false;
		return 0;
	}

	clear_assignments(g);

	node_count = dag_list_len(&g->nodes);
	topo = calloc(node_count, sizeof(*topo));
	if (!topo)
		goto out;

	if (topological_sort(g, topo, &topo_len) < 0)
		goto out;
	if ((size_t)topo_len != node_count) {
		errno = EFAULT;
		goto out;
	}
	topo_arr.nodes = topo;
	topo_arr.count = topo_len;
	/* Fail before deadline splitting or CPU placement if the DAG-wide
	 * critical path already exceeds the end-to-end budget.
	 */
	if (compute_graph_critical_bounds(&topo_arr, &critical_wcet,
				&critical_deadline_weight) < 0)
		goto out;
	if (validate_deadline_budget(g->deadline, critical_wcet,
				critical_deadline_weight) < 0)
		goto out;

	free(topo);
	topo = NULL;

	if (find_sources_and_sinks(g, &sources, &nsources, &sinks, &nsinks) < 0)
		goto out;
	if (nsources == 0 || nsinks == 0) {
		errno = EINVAL;
		goto out;
	}

	for (int i = 0; i < nsources; i++) {
		for (int j = 0; j < nsinks; j++) {
			int reaches = node_reaches(g, sources[i], sinks[j]);

			/* Generic DAGs can have multiple sources/sinks or disconnected
			 * components. Only reachable source/sink pairs participate in
			 * deadline splitting; unreachable combinations are ignored.
			 */
			if (reaches < 0)
				goto out;
			if (!reaches)
				continue;

			if (assign_deadlines_recursive(g, sources[i], sinks[j],
					g->deadline) < 0)
				goto out;
		}
	}

	if (assign_cpus(g) < 0)
		goto out;

	res = 0;
	g->dirty = false;

out:
	free(topo);
	free(sources);
	free(sinks);
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
