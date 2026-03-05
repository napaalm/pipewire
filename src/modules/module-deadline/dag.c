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
	if (!g) { errno = EINVAL; return -1; }
	dag_node_t *src = find_node(g, src_id);
	dag_node_t *dst = find_node(g, dst_id);
	if (!src || !dst) { errno = ENOENT; return -1; }

	/* Check if edge already exists */
	dag_edge_t *e;
	spa_list_for_each(e, &g->edges, link) {
		if (e->src == src && e->dst == dst) {
			errno = EEXIST;
			return -1;
		}
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

/* Helper structures and functions for topological sort and longest path */

typedef struct {
	dag_node_t **nodes;
	int count;
} node_array_t;

static node_array_t dag_nodes_to_array(dag_t *g)
{
	size_t count = dag_list_len(&g->nodes);

	node_array_t arr;
	arr.count = count;
	arr.nodes = malloc(count * sizeof(dag_node_t*));

	dag_node_t *n;
	int i=0;
	spa_list_for_each(n, &g->nodes, link) {
		arr.nodes[i++] = n;
	}
	return arr;
}

static int topological_sort(dag_t *g, dag_node_t **out)
{
	node_array_t arr = dag_nodes_to_array(g);
	if (arr.count == 0) {
		free(arr.nodes);
		return 0;
	}

	int *indegree = calloc(arr.count, sizeof(int));
	if (!indegree) {
		free(arr.nodes);
		return -1;
	}

	for (int i=0; i < arr.count; i++) {
		dag_node_t *n = arr.nodes[i];
		int in_deg = 0;
		dag_edge_t *e;
		spa_list_for_each(e, &n->incoming, dst_link) {
			in_deg++;
		}
		indegree[i] = in_deg;
	}

	int *queue = calloc(arr.count, sizeof(int));
	if (!queue) {
		free(indegree);
		free(arr.nodes);
		return -1;
	}

	int front = 0, back = 0;
	for (int i=0; i < arr.count; i++) {
		if (indegree[i] == 0) {
			queue[back++] = i;
		}
	}

	int idx = 0;
	while (front < back) {
		int u = queue[front++];
		out[idx++] = arr.nodes[u];

		dag_edge_t *edge;
		spa_list_for_each(edge, &arr.nodes[u]->outgoing, src_link) {
			int v = -1;
			for (int k=0; k < arr.count; k++) {
				if (arr.nodes[k] == edge->dst) { v = k; break; }
			}
			if (v >= 0) {
				indegree[v]--;
				if (indegree[v] == 0) {
					queue[back++] = v;
				}
			}
		}
	}

	free(queue);
	free(indegree);
	free(arr.nodes);

	return idx;
}

static void find_sources_and_sinks(dag_t *g, dag_node_t ***sources, int *nsources, dag_node_t ***sinks, int *nsinks)
{
	size_t count = dag_list_len(&g->nodes);

	dag_node_t **sarr = calloc(count, sizeof(*sarr));
	dag_node_t **tarr = calloc(count, sizeof(*tarr));
	int si=0, ti=0;

	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		bool has_in = !spa_list_is_empty(&n->incoming);
		bool has_out = !spa_list_is_empty(&n->outgoing);
		if (!has_in) sarr[si++] = n;
		if (!has_out) tarr[ti++] = n;
	}

	*sources = sarr; *nsources = si;
	*sinks = tarr; *nsinks = ti;
}

static uint64_t compute_longest_path(dag_t *g, dag_node_t *src, dag_node_t *dst, dag_node_t ***path_out, int *path_len_out)
{
	if (src == dst) {
		*path_len_out = 1;
		dag_node_t **p = malloc(sizeof(dag_node_t*));
		p[0] = src;
		*path_out = p;
		return src->wcet;
	}

	int n = dag_list_len(&g->nodes);
	if (n == 0) {
		*path_out = NULL;
		*path_len_out = 0;
		return 0;
	}

	dag_node_t **topo = malloc(n*sizeof(dag_node_t*));
	int length = topological_sort(g, topo);

	int src_idx=-1, dst_idx=-1;
	for (int i=0; i<length; i++) {
		if (topo[i] == src) src_idx = i;
		if (topo[i] == dst) dst_idx = i;
	}

	if (src_idx < 0 || dst_idx < 0 || src_idx > dst_idx) {
		free(topo);
		*path_out = NULL;
		*path_len_out = 0;
		return 0;
	}

	uint64_t *dist = calloc(length, sizeof(uint64_t));
	dist[src_idx] = topo[src_idx]->wcet;
	int *parent = malloc(length * sizeof(int));
	for (int i=0; i<length; i++) parent[i] = -1;
	
	for (int i=src_idx; i<=dst_idx; i++) {
		dag_node_t *u = topo[i];
		dag_edge_t *e;
		spa_list_for_each(e, &u->outgoing, src_link) {
			int v = -1;
			for (int k=i+1; k<=dst_idx; k++) {
				if (topo[k] == e->dst) { v = k; break; }
			}
			if (v >= 0) {
				if (dist[i] + e->dst->wcet > dist[v]) {
					dist[v] = dist[i] + e->dst->wcet;
					parent[v] = i;
				}
			}
		}
	}

	uint64_t L = dist[dst_idx];

	int path_len = 0;
	{
		int cur = dst_idx;
		while (cur != -1) {
			path_len++;
			cur = parent[cur];
		}
	}
	dag_node_t **p = malloc(path_len*sizeof(dag_node_t*));
	int pos = path_len -1;
	{
		int cur = dst_idx;
		while (cur != -1) {
			p[pos--] = topo[cur];
			cur = parent[cur];
		}
	}

	free(dist);
	free(parent);
	free(topo);

	*path_out = p;
	*path_len_out = path_len;

	return L;
}

static int assign_deadlines_recursive(dag_t *g, dag_node_t *src, dag_node_t *dst, uint64_t D)
{
	if (src == dst) {
		uint64_t d_i = D;
		if (src->deadline_assigned) {
			if (src->deadline > d_i) {
				src->deadline = d_i;
			}
		} else {
			src->deadline = d_i;
			src->deadline_assigned = true;
		}
		return 0;
	}

	dag_node_t **P;
	int path_len;
	uint64_t L = compute_longest_path(g, src, dst, &P, &path_len);
	if (path_len == 0 || L == 0) {
		/* No path */
		free(P);
		return -1;
	}

	uint64_t D_orig = D;
	bool *excluded = calloc((size_t)path_len, sizeof(*excluded));
	if (!excluded) {
		free(P);
		return -1;
	}

	bool changed = true;
	while (changed) {
		changed = false;
		double D_d = (double)D;
		double L_d = (double)L;
		if (D_d <= 0.0 || L_d <= 0.0)
			break;

		for (int i=0; i<path_len; i++) {
			if (excluded[i])
				continue;

			dag_node_t *ni = P[i];
			uint64_t C_i = ni->wcet;
			uint64_t d_prime = (uint64_t)floor(D_d * ((double)C_i / L_d));

			if (ni->deadline_assigned && ni->deadline < d_prime) {
				D = (D > ni->deadline) ? (D - ni->deadline) : 0;
				L = (L > C_i) ? (L - C_i) : 0;
				excluded[i] = true;
				changed = true;
				break;
			}
		}
	}

	double D_d = (double)D;
	double L_d = (double)L;

	if (D_d <= 0.0 || L_d <= 0.0) {
		free(excluded);
		free(P);
		return 0;
	}

	uint64_t assigned_src = 0;
	dag_node_t *n_src = P[0];
	uint64_t C_src = n_src->wcet;
	uint64_t d_prime_src = (uint64_t)floor(D_d * ((double)C_src / L_d));
	if (!n_src->deadline_assigned) {
		n_src->deadline = d_prime_src;
		n_src->deadline_assigned = true;
	} else {
		if (n_src->deadline > d_prime_src) n_src->deadline = d_prime_src;
	}
	assigned_src = n_src->deadline;
	
	dag_node_t *n_dst = P[path_len - 1];
	uint64_t C_dst = n_dst->wcet;
	uint64_t d_prime_dst = (uint64_t)floor(D_d * ((double)C_dst / L_d));
	
	if (!n_dst->deadline_assigned) {
		n_dst->deadline = d_prime_dst;
		n_dst->deadline_assigned = true;
	} else {
		if (n_dst->deadline > d_prime_dst) n_dst->deadline = d_prime_dst;
	}

	dag_edge_t *e;
	spa_list_for_each(e, &src->outgoing, src_link) {
		if (e->dst != dst) {
			uint64_t D_residual = (D_orig > assigned_src) ? (D_orig - assigned_src) : 0;
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
	if (!g) { errno = EINVAL; return -1; }

	if (spa_list_is_empty(&g->nodes)) {
		return 0;
	}

	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		n->deadline_assigned = false;
		n->deadline = 0;
	}

	dag_node_t **sources, **sinks;
	int nsources, nsinks;
	find_sources_and_sinks(g, &sources, &nsources, &sinks, &nsinks);

	if (nsources == 0 || nsinks == 0) {
		free(sources);
		free(sinks);
		errno = EINVAL;
		return -1;
	}

	for (int i=0; i<nsources; i++) {
		for (int j=0; j<nsinks; j++) {
			if (assign_deadlines_recursive(g, sources[i], sinks[j], g->deadline) < 0) {
				free(sources);
				free(sinks);
				return -1;
			}
		}
	}

	free(sources);
	free(sinks);

	if (assign_cpus(g) < 0) return -1;

	return 0;
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
