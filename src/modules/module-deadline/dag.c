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

static void clear_assignments(dag_t *g)
{
	dag_node_t *n;

	spa_list_for_each(n, &g->nodes, link) {
		n->deadline_assigned = false;
		n->deadline = 0;
		n->cpu = 0;
	}
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
	n->deadline_assigned = false;
	spa_list_init(&n->outgoing);
	spa_list_init(&n->incoming);
	spa_list_append(&g->nodes, &n->link);
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

static inline uint64_t saturating_sub_u64(uint64_t value, uint64_t decrement)
{
	/* A discounted node can consume the whole residual budget/path length. */
	return decrement >= value ? 0 : value - decrement;
}

static inline uint64_t proportional_deadline(uint64_t deadline_budget, uint64_t wcet, uint64_t path_wcet)
{
	if (deadline_budget == 0 || wcet == 0 || path_wcet == 0)
		return 0;

	return (uint64_t)floor((double)deadline_budget * ((double)wcet / (double)path_wcet));
}

static inline void assign_or_tighten_deadline(dag_node_t *node, uint64_t deadline)
{
	if (!node->deadline_assigned) {
		node->deadline = deadline;
		node->deadline_assigned = true;
	} else if (node->deadline > deadline) {
		node->deadline = deadline;
	}
}

static int assign_deadlines_recursive(dag_t *g, dag_node_t *src, dag_node_t *dst, uint64_t D)
{
	if (src == dst) {
		assign_or_tighten_deadline(src, D);
		return 0;
	}

	/* Phase A: compute the critical path for this subproblem. */
	dag_node_t **P = NULL;
	int path_len = 0;
	uint64_t L = 0;
	int path_result = compute_longest_path(g, src, dst, &P, &path_len, &L);
	if (path_result < 0)
		return -1;
	if (path_result == PATH_UNREACHABLE)
		return 0;

	/* Phase B: discount already-assigned tighter deadlines from the
	 * residual budget and residual path WCET before assigning this level.
	 */
	uint64_t residual_deadline = D;
	uint64_t residual_wcet = L;
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
		if (residual_deadline == 0 || residual_wcet == 0)
			break;

		for (int i=0; i<path_len; i++) {
			if (excluded[i])
				continue;

			dag_node_t *ni = P[i];
			uint64_t d_prime = proportional_deadline(residual_deadline,
					ni->wcet, residual_wcet);

			if (ni->deadline_assigned && ni->deadline < d_prime) {
				residual_deadline = saturating_sub_u64(residual_deadline,
						ni->deadline);
				residual_wcet = saturating_sub_u64(residual_wcet,
						ni->wcet);
				if (ni == src) {
					src_discounted = true;
					discounted_src_deadline = ni->deadline;
				}
				excluded[i] = true;
				changed = true;
				break;
			}
		}
	}

	if (residual_deadline == 0 || residual_wcet == 0) {
		free(excluded);
		free(P);
		return 0;
	}

	/* Phase C: assign/tighten the endpoints with the updated residual budget
	 * and recurse with the budget left after the current source.
	 *
	 * The recursive budget must be derived from residual_deadline, not from
	 * the original D input. Using the stale input would re-introduce deadline
	 * budget already discounted above for tighter pre-assigned nodes.
	 */
	dag_node_t *n_src = P[0];
	uint64_t d_prime_src = proportional_deadline(residual_deadline,
			n_src->wcet, residual_wcet);
	assign_or_tighten_deadline(n_src, d_prime_src);
	
	dag_node_t *n_dst = P[path_len - 1];
	uint64_t d_prime_dst = proportional_deadline(residual_deadline,
			n_dst->wcet, residual_wcet);
	assign_or_tighten_deadline(n_dst, d_prime_dst);

	uint64_t D_residual = residual_deadline;
	if (src_discounted) {
		/* Phase B already removed the source from residual_deadline. If
		 * phase C tightened it again, refund the delta so descendants see
		 * the true post-source residual budget.
		 */
		D_residual += discounted_src_deadline - n_src->deadline;
	} else {
		D_residual = saturating_sub_u64(D_residual, n_src->deadline);
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

static int assign_cpus(dag_t *g)
{
	size_t count = dag_list_len(&g->nodes);
	if (count == 0) return 0;

	dag_node_t *node;
	dag_node_t **arr = malloc(count*sizeof(*arr));
	int i=0;
	spa_list_for_each(node, &g->nodes, link) {
		arr[i++] = node;
	}

	for (size_t j=0; j<count; j++) {
		if (!arr[j]->deadline_assigned) {
			free(arr);
			errno = EFAULT;
			return -1;
		}
	}

	struct {
		dag_node_t *n;
		double util;
	} *info = malloc(count*sizeof(*info));

	for (size_t j=0; j<count; j++) {
		double denom = (double)((arr[j]->deadline < g->period) ? arr[j]->deadline : g->period);
		if (denom == 0) {
			free(info);
			free(arr);
			errno = EFAULT;
			return -1;
		}
		info[j].n = arr[j];
		info[j].util = ((double)arr[j]->wcet) / denom;
	}

	for (size_t x=0; x<count-1; x++) {
		for (size_t y=x+1; y<count; y++) {
			if (info[x].util < info[y].util) {
				double tmpu = info[x].util; info[x].util = info[y].util; info[y].util = tmpu;
				dag_node_t *tmpn = info[x].n; info[x].n = info[y].n; info[y].n = tmpn;
			}
		}
	}

	double *cpu_util = calloc(g->num_cpus, sizeof(double));
	if (!cpu_util) {
		free(info);
		free(arr);
		return -1;
	}

	for (size_t j=0; j<count; j++) {
		double u = info[j].util;
		int chosen = -1;
		double max_margin = -1.0;
		for (uint32_t c=0; c<g->num_cpus; c++) {
			double margin = g->utilization - cpu_util[c];
			if (margin >= u && margin > max_margin) {
				max_margin = margin;
				chosen = c;
			}
		}
		if (chosen < 0) {
			free(cpu_util);
			free(info);
			free(arr);
			errno = EAGAIN;
			return -1;
		}
		cpu_util[chosen] += u;
		info[j].n->cpu = chosen;
	}

	free(cpu_util);
	free(info);
	free(arr);
	return 0;
}

int dag_recalculate(dag_t *g)
{
	dag_node_t **sources = NULL, **sinks = NULL, **topo = NULL;
	size_t node_count;
	int nsources = 0, nsinks = 0;
	int topo_len = 0;
	int res = -1;

	if (!g) { errno = EINVAL; return -1; }

	if (spa_list_is_empty(&g->nodes))
		return 0;

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

out:
	free(topo);
	free(sources);
	free(sinks);
	if (res < 0)
		clear_assignments(g);
	return res;
}

int dag_foreach_node(dag_t *g, dag_node_callback_t cb, void *data)
{
	if (!g || !cb) { errno = EINVAL; return -1; }
	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		if (!n->deadline_assigned) {
			if (dag_recalculate(g) < 0) return -1;
			break;
		}
	}

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
