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
#include <inttypes.h>
#include <limits.h>
#include <errno.h>

#include "dag.h"
#include <time.h>

#define DAG_NODE_INDEX_INVALID UINT32_MAX
#define DAG_FICTITIOUS_SOURCE_ID UINT32_MAX
#define DAG_FICTITIOUS_SINK_ID (UINT32_MAX - 1)

typedef struct {
	dag_node_t **nodes;
	uint32_t count;
} node_array_t;

static inline uint32_t dag_list_len(struct spa_list *list)
{
	uint32_t len = 0;
	dag_node_t *pos;
	spa_list_for_each(pos, list, link) len++;
	return len;
}

static bool dag_is_reserved_fictitious_id(uint32_t id)
{
	return id == DAG_FICTITIOUS_SOURCE_ID || id == DAG_FICTITIOUS_SINK_ID;
}

static void dag_invalidate_schedule(dag_t *g)
{
	dag_node_t *n;

	if (!g)
		return;

	spa_list_for_each(n, &g->nodes, link) {
		n->deadline_assigned = false;
		n->deadline = 0;
		n->cumulative_deadline = 0;
		n->local_deadline = 0;
		n->remaining_deadline = 0;
		n->cpu = DAG_CPU_INVALID;
	}
}

/* Single entry point for marking the DAG dirty after a successful
 * mutation: clears previously-computed schedule (deadlines and CPUs)
 * and sets g->dirty so the next dag_foreach_node triggers a
 * recalculation. Callers MUST call this only on actual change so a
 * no-op set (same wcet, same period) does not invalidate a valid
 * cached schedule. */
static void dag_mark_dirty(dag_t *g)
{
	if (!g)
		return;
	g->dirty = true;
	dag_invalidate_schedule(g);
}

static void dag_free_unrelated(dag_t *g)
{
	uint32_t i;

	if (!g || !g->unrelated)
		return;

	for (i = 0; i < g->unrelated_capacity; i++)
		free(g->unrelated[i]);

	free(g->unrelated);
	g->unrelated = NULL;
	g->unrelated_size = 0;
	g->unrelated_capacity = 0;
}

static void dag_free_indexed_nodes(dag_t *g)
{
	free(g->indexed_nodes);
	g->indexed_nodes = NULL;
	g->indexed_count = 0;
}

static void dag_free_groups(dag_t *g)
{
	uint32_t i;

	if (!g)
		return;

	if (g->group_members) {
		for (i = 0; i < g->group_count; i++)
			free(g->group_members[i]);
		free(g->group_members);
		g->group_members = NULL;
	}
	if (g->group_node_succ) {
		for (i = 0; i < g->group_count; i++)
			free(g->group_node_succ[i]);
		free(g->group_node_succ);
		g->group_node_succ = NULL;
	}
	free(g->node_group_index);
	g->node_group_index = NULL;
	free(g->group_rep_node_index);
	g->group_rep_node_index = NULL;
	g->group_count = 0;
}

/* Per-recalc scratch buffer lifecycle. Allocated to the dense-index
 * cardinality (which is itself a one-shot allocation per recalc),
 * so the scratch buffers cost is amortised over the entire recalc
 * regardless of how many compute_longest_path / discount-loop calls
 * fire. Both pointers are NULL between recalcs. */
static int dag_workspace_alloc(dag_t *g, uint32_t capacity)
{
	if (g->ws_capacity >= capacity)
		return 0;

	free(g->ws_path);
	free(g->ws_excluded);
	g->ws_path = NULL;
	g->ws_excluded = NULL;
	g->ws_capacity = 0;

	if (capacity == 0)
		return 0;

	g->ws_path = calloc(capacity, sizeof(*g->ws_path));
	g->ws_excluded = calloc(capacity, sizeof(*g->ws_excluded));
	if (!g->ws_path || !g->ws_excluded) {
		free(g->ws_path);
		free(g->ws_excluded);
		g->ws_path = NULL;
		g->ws_excluded = NULL;
		errno = ENOMEM;
		return -1;
	}
	g->ws_capacity = capacity;
	return 0;
}

static void dag_workspace_free(dag_t *g)
{
	free(g->ws_path);
	free(g->ws_excluded);
	g->ws_path = NULL;
	g->ws_excluded = NULL;
	g->ws_capacity = 0;
}

static void dag_invalidate_analysis(dag_t *g)
{
	dag_node_t *n;

	if (!g)
		return;

	spa_list_for_each(n, &g->nodes, link) {
		n->index = DAG_NODE_INDEX_INVALID;
		n->longest_len = 0;
		n->longest_next = -1;
		free(n->successors);
		n->successors = NULL;
	}

	dag_free_unrelated(g);
	dag_free_groups(g);
	dag_free_indexed_nodes(g);
	dag_workspace_free(g);
}

dag_t *dag_create(uint64_t period, uint64_t deadline, double admission_ceiling,
		uint32_t num_cpus, const double *relative_capacity)
{
	if (period == 0 || num_cpus == 0) {
		errno = EINVAL;
		return NULL;
	}
	/* Timing contract: deadline must be strictly positive and no
	 * greater than the period. A relative deadline beyond the
	 * period would let a job overrun into the next period's slack,
	 * which the kernel rejects. Reject before any allocation so
	 * the caller doesn't have to free a partially-built DAG. */
	if (deadline == 0 || deadline > period) {
		errno = EINVAL;
		return NULL;
	}
	/* Non-finite (NaN, +/-Inf) ceilings would slip past naive < / >
	 * comparisons (NaN compares false to everything). isfinite()
	 * catches those before they corrupt the admission arithmetic. */
	if (!isfinite(admission_ceiling) ||
			admission_ceiling <= 0.0 || admission_ceiling > 1.0) {
		errno = EINVAL;
		return NULL;
	}
	if (relative_capacity != NULL) {
		for (uint32_t i = 0; i < num_cpus; i++) {
			if (!isfinite(relative_capacity[i]) ||
					relative_capacity[i] <= 0.0 ||
					relative_capacity[i] > 1.0) {
				errno = EINVAL;
				return NULL;
			}
		}
	}

	dag_t *g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;

	g->relative_capacity = calloc(num_cpus, sizeof(*g->relative_capacity));
	if (!g->relative_capacity) {
		free(g);
		return NULL;
	}
	for (uint32_t i = 0; i < num_cpus; i++) {
		g->relative_capacity[i] = relative_capacity != NULL ?
			relative_capacity[i] : 1.0;
	}

	g->period = period;
	g->deadline = deadline;
	g->admission_ceiling = admission_ceiling;
	g->num_cpus = num_cpus;
	g->dirty = false;
	spa_list_init(&g->nodes);
	spa_list_init(&g->edges);

	return g;
}

void dag_destroy(dag_t *g)
{
	if (!g)
		return;

	dag_invalidate_analysis(g);

	/* Remove all edges */
	dag_edge_t *e, *etmp;
	spa_list_for_each_safe(e, etmp, &g->edges, link) {
		spa_list_remove(&e->link);
		if (e->src)
			spa_list_remove(&e->src_link);
		if (e->dst)
			spa_list_remove(&e->dst_link);
		free(e);
	}

	/* Remove all nodes */
	dag_node_t *n, *ntmp;
	spa_list_for_each_safe(n, ntmp, &g->nodes, link) {
		spa_list_remove(&n->link);
		free(n);
	}

	free(g->nodes_by_id);
	free(g->relative_capacity);
	free(g);
}

int dag_set_global_period_deadline(dag_t *g, uint64_t period, uint64_t deadline)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}
	/* Same contract as dag_create: period > 0, 0 < deadline <= period. */
	if (period == 0 || deadline == 0 || deadline > period) {
		errno = EINVAL;
		return -1;
	}

	if (g->period == period && g->deadline == deadline)
		return 0;

	g->period = period;
	g->deadline = deadline;
	dag_mark_dirty(g);
	return 0;
}

/* nodes_by_id starts at this capacity; doubles on overflow.
 * Sized so that the typical audio graph (a few dozen nodes per
 * driver) never reallocates after the first round. */
#define DAG_NODES_BY_ID_INITIAL_CAP 16u

/* Binary search for `id` in g->nodes_by_id. Returns the index of
 * the matching slot if found, or the index where `id` would be
 * inserted to keep the array sorted (caller checks the slot's id
 * to distinguish). */
static uint32_t dag_nodes_by_id_bsearch(dag_t *g, uint32_t id)
{
	uint32_t lo = 0;
	uint32_t hi = g->nodes_by_id_count;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (g->nodes_by_id[mid]->id < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Insert `n` into g->nodes_by_id, keeping the array sorted by id.
 * Geometric realloc: starts at 16, doubles on overflow. On
 * allocation failure leaves the array untouched and returns -1
 * with errno=ENOMEM. */
static int dag_nodes_by_id_insert(dag_t *g, dag_node_t *n)
{
	uint32_t pos;

	if (g->nodes_by_id_count == g->nodes_by_id_cap) {
		uint32_t new_cap = g->nodes_by_id_cap ?
				g->nodes_by_id_cap * 2 :
				DAG_NODES_BY_ID_INITIAL_CAP;
		dag_node_t **resized = realloc(g->nodes_by_id,
				(size_t)new_cap * sizeof(*resized));

		if (!resized) {
			errno = ENOMEM;
			return -1;
		}
		g->nodes_by_id = resized;
		g->nodes_by_id_cap = new_cap;
	}

	pos = dag_nodes_by_id_bsearch(g, n->id);
	if (pos < g->nodes_by_id_count) {
		memmove(&g->nodes_by_id[pos + 1], &g->nodes_by_id[pos],
				(g->nodes_by_id_count - pos) *
				sizeof(*g->nodes_by_id));
	}
	g->nodes_by_id[pos] = n;
	g->nodes_by_id_count++;
	return 0;
}

/* Remove n's slot from g->nodes_by_id. The slot is found by
 * bsearch and a pointer-equality check. Returns 0 on success
 * (also when the entry was absent), preserves sort order. */
static void dag_nodes_by_id_remove(dag_t *g, dag_node_t *n)
{
	uint32_t pos;

	if (g->nodes_by_id_count == 0)
		return;
	pos = dag_nodes_by_id_bsearch(g, n->id);
	if (pos >= g->nodes_by_id_count || g->nodes_by_id[pos] != n)
		return;
	if (pos < g->nodes_by_id_count - 1) {
		memmove(&g->nodes_by_id[pos], &g->nodes_by_id[pos + 1],
				(g->nodes_by_id_count - pos - 1) *
				sizeof(*g->nodes_by_id));
	}
	g->nodes_by_id_count--;
}

dag_node_t *dag_find_node(dag_t *g, uint32_t id)
{
	uint32_t pos;

	if (!g || g->nodes_by_id_count == 0)
		return NULL;
	pos = dag_nodes_by_id_bsearch(g, id);
	if (pos >= g->nodes_by_id_count)
		return NULL;
	return g->nodes_by_id[pos]->id == id ? g->nodes_by_id[pos] : NULL;
}

/* Internal alias kept for readability of the rest of the file --
 * the public name lives in dag.h. */
static dag_node_t *find_node(dag_t *g, uint32_t id)
{
	return dag_find_node(g, id);
}

static dag_node_t *dag_find_fictitious_source(dag_t *g)
{
	return find_node(g, DAG_FICTITIOUS_SOURCE_ID);
}

static dag_node_t *dag_find_fictitious_sink(dag_t *g)
{
	return find_node(g, DAG_FICTITIOUS_SINK_ID);
}

static bool dag_nodes_are_related(dag_node_t *a, dag_node_t *b)
{
	if (!a || !b || !a->successors || !b->successors)
		return false;

	return bitset_test(a->successors, b->index) ||
		bitset_test(b->successors, a->index);
}

int dag_add_node(dag_t *g, uint32_t id, uint64_t wcet, pid_t tid, bool fictitious)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}
	if (!fictitious && dag_is_reserved_fictitious_id(id)) {
		errno = EINVAL;
		return -1;
	}
	if (wcet == 0 && !fictitious) {
		/* Transient: a freshly registered follower can race ahead
		 * of the first measured cycle. The reconciler retries on
		 * the next driver completion once prev_run_time lands. */
		pw_log_debug("Cannot add node %u with wcet=0", id);
		errno = EINVAL;
		return -1;
	}
	if (find_node(g, id)) {
		errno = EEXIST;
		return -1;
	}

	dag_node_t *n = calloc(1, sizeof(*n));
	if (!n)
		return -1;

	n->id = id;
	n->index = DAG_NODE_INDEX_INVALID;
	n->wcet = wcet;
	n->tid = tid;

	n->fictitious = fictitious;
	n->remaining_deadline = 0;
	n->longest_len = 0;
	n->longest_next = -1;
	n->successors = NULL;
	n->deadline = 0;
	n->cumulative_deadline = 0;
	n->local_deadline = 0;
	n->cpu = DAG_CPU_INVALID;
	n->deadline_assigned = false;
	spa_list_init(&n->outgoing);
	spa_list_init(&n->incoming);
	spa_list_append(&g->nodes, &n->link);

	/* Maintain the persistent id-index. On ENOMEM, roll back the
	 * list insertion so the graph is left exactly as the caller
	 * found it. */
	if (dag_nodes_by_id_insert(g, n) < 0) {
		spa_list_remove(&n->link);
		free(n);
		return -1;
	}

	dag_invalidate_analysis(g);
	dag_mark_dirty(g);
	return 0;
}

static int dag_remove_node_ptr(dag_t *g, dag_node_t *n)
{
	if (!g || !n) {
		errno = EINVAL;
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

	dag_nodes_by_id_remove(g, n);
	spa_list_remove(&n->link);
	free(n->successors);
	free(n);
	return 0;
}

static int dag_remove_fictitious_nodes(dag_t *g)
{
	dag_node_t *n, *tmp;

	if (!g)
		return -1;

	spa_list_for_each_safe(n, tmp, &g->nodes, link) {
		if (!n->fictitious)
			continue;
		if (dag_remove_node_ptr(g, n) < 0)
			return -1;
	}

	return 0;
}

int dag_remove_node(dag_t *g, uint32_t id)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}

	dag_node_t *n = find_node(g, id);
	if (!n) {
		errno = ENOENT;
		return -1;
	}

	if (dag_remove_node_ptr(g, n) < 0)
		return -1;

	dag_invalidate_analysis(g);
	dag_mark_dirty(g);
	return 0;
}

/* Internal reachability check: returns 1 if `src` can reach `dst`
 * by following any directed path in the current DAG, 0 otherwise,
 * -1 on allocation failure (errno set). Independent of the indexed-
 * nodes cache so the caller may invoke it during graph mutation
 * (when the cache is invalidated). DFS over an explicit stack to
 * avoid recursion depth limits for large graphs.
 *
 * src == dst returns 1 by convention -- callers use this to detect
 * self-cycles that would-be edges close. */
static int dag_node_reaches(dag_t *g, dag_node_t *src, dag_node_t *dst)
{
	dag_node_t **stack;
	dag_node_t **all_nodes;
	bool *visited;
	uint32_t n_nodes, stack_len = 0;
	int result = 0;
	uint32_t i;
	dag_node_t *iter;

	if (src == dst)
		return 1;

	n_nodes = dag_list_len(&g->nodes);
	if (n_nodes == 0)
		return 0;

	all_nodes = calloc(n_nodes, sizeof(*all_nodes));
	visited = calloc(n_nodes, sizeof(*visited));
	stack = calloc(n_nodes, sizeof(*stack));
	if (!all_nodes || !visited || !stack) {
		free(all_nodes);
		free(visited);
		free(stack);
		errno = ENOMEM;
		return -1;
	}

	i = 0;
	spa_list_for_each(iter, &g->nodes, link)
		all_nodes[i++] = iter;

	for (i = 0; i < n_nodes; i++) {
		if (all_nodes[i] == src) {
			visited[i] = true;
			stack[stack_len++] = src;
			break;
		}
	}

	while (stack_len > 0) {
		dag_node_t *u = stack[--stack_len];
		dag_edge_t *e;

		spa_list_for_each(e, &u->outgoing, src_link) {
			if (e->dst == dst) {
				result = 1;
				goto out;
			}
			for (i = 0; i < n_nodes; i++) {
				if (all_nodes[i] == e->dst) {
					if (!visited[i]) {
						visited[i] = true;
						stack[stack_len++] = e->dst;
					}
					break;
				}
			}
		}
	}

out:
	free(stack);
	free(visited);
	free(all_nodes);
	return result;
}

bool dag_has_cycle(dag_t *g)
{
	dag_node_t *n;

	if (!g)
		return false;

	/* A graph is cyclic iff some node can reach itself through at
	 * least one outgoing edge. Walk per node; each walk skips its
	 * own start so a single self-edge is detected as a cycle too
	 * (it would already have been rejected at add time, but the
	 * predicate has to handle a graph populated by a code path that
	 * bypasses dag_add_edge -- e.g. an internal mutation gone
	 * wrong). */
	spa_list_for_each(n, &g->nodes, link) {
		dag_edge_t *e;

		spa_list_for_each(e, &n->outgoing, src_link) {
			int r = dag_node_reaches(g, e->dst, n);

			if (r < 0)
				return false;
			if (r > 0)
				return true;
		}
	}

	return false;
}

int dag_add_edge(dag_t *g, uint32_t src_id, uint32_t dst_id)
{
	int reaches;

	if (!g) {
		errno = EINVAL;
		return -1;
	}

	dag_node_t *src = find_node(g, src_id);
	dag_node_t *dst = find_node(g, dst_id);
	if (!src || !dst) {
		errno = ENOENT;
		return -1;
	}

	/* Self-loop: a node cannot precede itself within a single
	 * period. We reject the edge before any mutation. */
	if (src == dst) {
		errno = EINVAL;
		return -1;
	}

	/* Check if edge already exists */
	dag_edge_t *e;
	spa_list_for_each(e, &g->edges, link) {
		if (e->src == src && e->dst == dst) {
			errno = EEXIST;
			return -1;
		}
	}

	/* Cycle check: if dst already reaches src by some path, then
	 * adding src->dst would close a cycle. Reject with -ELOOP and
	 * leave the graph untouched. PipeWire feedback links are
	 * pre-filtered by the topology-snapshot layer, so a cycle
	 * arriving here means the filter missed a case -- the library
	 * fails safe rather than producing wrong scheduling. */
	reaches = dag_node_reaches(g, dst, src);
	if (reaches < 0)
		return -1;
	if (reaches > 0) {
		errno = ELOOP;
		return -1;
	}

	e = calloc(1, sizeof(*e));
	if (!e)
		return -1;

	e->src = src;
	e->dst = dst;
	spa_list_append(&g->edges, &e->link);
	spa_list_append(&src->outgoing, &e->src_link);
	spa_list_append(&dst->incoming, &e->dst_link);

	dag_invalidate_analysis(g);
	dag_mark_dirty(g);
	return 0;
}

int dag_remove_edge(dag_t *g, uint32_t src_id, uint32_t dst_id)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}

	dag_node_t *src = find_node(g, src_id);
	dag_node_t *dst = find_node(g, dst_id);
	if (!src || !dst) {
		errno = ENOENT;
		return -1;
	}

	dag_edge_t *e, *etmp;
	spa_list_for_each_safe(e, etmp, &g->edges, link) {
		if (e->src == src && e->dst == dst) {
			spa_list_remove(&e->link);
			spa_list_remove(&e->src_link);
			spa_list_remove(&e->dst_link);
			free(e);
			dag_invalidate_analysis(g);
			dag_mark_dirty(g);
			return 0;
		}
	}

	errno = ENOENT;
	return -1;
}

int dag_set_node_wcet(dag_t *g, uint32_t id, uint64_t wcet)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}

	dag_node_t *n = find_node(g, id);
	if (!n) {
		errno = ENOENT;
		return -1;
	}

	/* Reject zero-WCET sets the same way dag_add_node does, except
	 * for fictitious nodes (internal endpoints only -- the public
	 * dag_set_node_wcet should never be called on those). */
	if (wcet == 0 && !n->fictitious) {
		errno = EINVAL;
		return -1;
	}

	if (n->wcet == wcet)
		return 0;

	n->wcet = wcet;
	dag_mark_dirty(g);
	return 0;
}

int dag_set_node_group(dag_t *g, uint32_t id, uint32_t group_id)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}

	dag_node_t *n = find_node(g, id);
	if (!n) {
		errno = ENOENT;
		return -1;
	}

	if (n->group_id == group_id)
		return 0;

	n->group_id = group_id;
	/* Group changes only affect CPU placement, not deadlines or
	 * topology -- but the existing dirty bit is the simplest way
	 * to force the next dag_foreach_node to re-run assign_cpus.
	 * The cost of redoing the deadline split is negligible relative
	 * to a fresh recomputation; the alternative (a finer-grained
	 * "cpu-dirty" flag) would not pay back the bookkeeping. */
	dag_mark_dirty(g);
	return 0;
}

/***********************************************************************
 * Scheduling Parameter Computation
 ***********************************************************************/

/* Helper structures and functions for topological sort and longest path */

static node_array_t dag_nodes_to_array(dag_t *g)
{
	uint32_t count = dag_list_len(&g->nodes);
	node_array_t arr;
	uint32_t i = 0;
	dag_node_t *n;

	arr.count = count;
	arr.nodes = malloc((size_t)count * sizeof(dag_node_t *));
	if (!arr.nodes && count > 0)
		return arr;

	spa_list_for_each(n, &g->nodes, link)
		arr.nodes[i++] = n;

	return arr;
}

static int topological_sort(dag_t *g, dag_node_t **out)
{
	node_array_t arr = dag_nodes_to_array(g);
	uint32_t front = 0, back = 0, idx = 0;

	if (arr.count == 0) {
		free(arr.nodes);
		return 0;
	}
	if (!arr.nodes)
		return -1;

	int *indegree = calloc(arr.count, sizeof(int));
	if (!indegree) {
		free(arr.nodes);
		return -1;
	}

	for (uint32_t i = 0; i < arr.count; i++) {
		dag_edge_t *e;
		int in_deg = 0;

		spa_list_for_each(e, &arr.nodes[i]->incoming, dst_link)
			in_deg++;

		indegree[i] = in_deg;
	}

	int *queue = calloc(arr.count, sizeof(int));
	if (!queue) {
		free(indegree);
		free(arr.nodes);
		return -1;
	}

	for (uint32_t i = 0; i < arr.count; i++) {
		if (indegree[i] == 0)
			queue[back++] = (int)i;
	}

	while (front < back) {
		int u = queue[front++];
		dag_edge_t *edge;

		out[idx++] = arr.nodes[u];

		spa_list_for_each(edge, &arr.nodes[u]->outgoing, src_link) {
			int v = -1;

			for (uint32_t k = 0; k < arr.count; k++) {
				if (arr.nodes[k] == edge->dst) {
					v = (int)k;
					break;
				}
			}
			if (v >= 0) {
				indegree[v]--;
				if (indegree[v] == 0)
					queue[back++] = v;
			}
		}
	}

	free(queue);
	free(indegree);
	free(arr.nodes);

	/* Partial order out of Kahn's algorithm == cyclic input. The
	 * library promises this never happens because dag_add_edge
	 * rejects cycle-creating edges, but a defensive ELOOP here
	 * still lets dag_recalculate fail safe rather than producing
	 * a meaningless schedule on a corrupted graph. */
	if (idx != arr.count) {
		errno = ELOOP;
		return -1;
	}

	return (int)idx;
}

/* Sources are real nodes with no real incoming edge (a node whose
 * only incoming edges come from a fictitious endpoint also qualifies);
 * sinks are real nodes with no real outgoing edge. An isolated real
 * node has neither and is classified as both source AND sink, so the
 * fictitious-endpoint pass connects it to both fic_src and fic_sink
 * and the recalc treats it as its own one-node subproblem.
 *
 * The arrays are not partitioned by component. dag_recalculate uses
 * them only to know which real nodes need a fictitious-source edge
 * (and a fictitious-sink edge); component partitioning falls out
 * naturally from the longest-path computation in the per-node
 * iterative assignment, which traverses only one component at a
 * time via the longest_next chain. */
static void find_sources_and_sinks(dag_t *g, dag_node_t ***sources, uint32_t *nsources,
		dag_node_t ***sinks, uint32_t *nsinks)
{
	uint32_t count = dag_list_len(&g->nodes);
	dag_node_t **sarr = calloc(count, sizeof(*sarr));
	dag_node_t **tarr = calloc(count, sizeof(*tarr));
	uint32_t si = 0, ti = 0;
	dag_node_t *n;

	spa_list_for_each(n, &g->nodes, link) {
		dag_edge_t *e;
		bool has_in = false;
		bool has_out = false;

		if (n->fictitious)
			continue;

		spa_list_for_each(e, &n->incoming, dst_link) {
			if (!e->src->fictitious) {
				has_in = true;
				break;
			}
		}

		spa_list_for_each(e, &n->outgoing, src_link) {
			if (!e->dst->fictitious) {
				has_out = true;
				break;
			}
		}

		if (!has_in)
			sarr[si++] = n;
		if (!has_out)
			tarr[ti++] = n;
	}

	*sources = sarr;
	*nsources = si;
	*sinks = tarr;
	*nsinks = ti;
}

static int dag_add_fictitious_endpoints(dag_t *g, dag_node_t **sources, uint32_t nsources,
		dag_node_t **sinks, uint32_t nsinks)
{
	if (dag_add_node(g, DAG_FICTITIOUS_SOURCE_ID, 0, -1, true) < 0)
		return -1;

	if (dag_add_node(g, DAG_FICTITIOUS_SINK_ID, 0, -1, true) < 0)
		return -1;

	for (uint32_t i = 0; i < nsources; i++) {
		if (dag_add_edge(g, DAG_FICTITIOUS_SOURCE_ID, sources[i]->id) < 0)
			return -1;
	}

	for (uint32_t i = 0; i < nsinks; i++) {
		if (dag_add_edge(g, sinks[i]->id, DAG_FICTITIOUS_SINK_ID) < 0)
			return -1;
	}

	return 0;
}

static void dag_populate_longest_paths(dag_t *g)
{
	for (uint32_t i = g->indexed_count; i > 0; i--) {
		dag_node_t *node = g->indexed_nodes[i - 1];
		dag_edge_t *e;
		uint64_t best_len = 0;
		int best_next = -1;

		node->longest_len = node->wcet;
		node->longest_next = -1;

		spa_list_for_each(e, &node->outgoing, src_link) {
			dag_node_t *child = e->dst;
			if (best_next < 0 || child->longest_len > best_len) {
				best_len = child->longest_len;
				best_next = (int)child->index;
			}
		}

		if (best_next >= 0) {
			node->longest_len = node->wcet + best_len;
			node->longest_next = best_next;
		}
	}
}

/* Materialize the longest path from `src` to `dst` into the
 * caller-provided buffer `path_out` (which must be at least
 * indexed_count entries large -- the recalc workspace buffer is
 * sized this way). Returns the path's cumulative WCET, or 0 on
 * any failure (no path, NULL inputs). The path length is written
 * to *path_len_out.
 *
 * The buffer is NOT allocated here; the caller owns it. This is
 * how the per-recalc workspace removes per-call malloc/free churn
 * across the iterative deadline assignment. */
static uint64_t compute_longest_path(dag_t *g, dag_node_t *src, dag_node_t *dst,
		dag_node_t **path_out, int *path_len_out)
{
	uint64_t total = 0;
	dag_node_t *node;
	uint32_t count = 0;

	*path_len_out = 0;

	if (!g || !src || !dst || !path_out)
		return 0;

	if (src == dst) {
		path_out[0] = src;
		*path_len_out = 1;
		return src->wcet;
	}

	node = src;
	while (node != NULL) {
		count++;
		if (count > g->indexed_count)
			return 0;
		if (node == dst)
			break;
		if (node->longest_next < 0 || (uint32_t)node->longest_next >= g->indexed_count)
			return 0;
		node = g->indexed_nodes[node->longest_next];
	}

	if (node != dst)
		return 0;

	node = src;
	for (uint32_t i = 0; i < count; i++) {
		path_out[i] = node;
		total += node->wcet;
		if (node == dst)
			break;
		node = g->indexed_nodes[node->longest_next];
	}

	*path_len_out = (int)count;
	return total;
}

static int dag_build_indexed_nodes(dag_t *g)
{
	uint32_t count = dag_list_len(&g->nodes);
	int length;

	if (count == 0)
		return 0;

	g->indexed_nodes = calloc(count, sizeof(*g->indexed_nodes));
	if (!g->indexed_nodes)
		return -1;

	length = topological_sort(g, g->indexed_nodes);
	if (length < 0 || (uint32_t)length != count) {
		dag_free_indexed_nodes(g);
		errno = EINVAL;
		return -1;
	}

	g->indexed_count = count;
	for (uint32_t i = 0; i < count; i++)
		g->indexed_nodes[i]->index = i;

	/* Size the per-recalc workspace once at the same point we size
	 * the indexed_nodes cache. All subsequent
	 * compute_longest_path / assign_path_head_deadline calls reuse
	 * the same buffer, eliminating per-call malloc/free churn for
	 * graphs that need many longest-path queries (one per real
	 * node in the iterative deadline assignment). */
	if (dag_workspace_alloc(g, count) < 0) {
		dag_free_indexed_nodes(g);
		return -1;
	}

	return 0;
}

static int dag_build_successors(dag_t *g)
{
	uint32_t count = g->indexed_count;

	if (count == 0)
		return 0;

	for (uint32_t i = 0; i < count; i++) {
		g->indexed_nodes[i]->successors = bitset_alloc((int)count);
		if (!g->indexed_nodes[i]->successors)
			return -1;
	}

	for (uint32_t i = count; i > 0; i--) {
		dag_node_t *node = g->indexed_nodes[i - 1];
		dag_edge_t *e;

		bitset_zero(node->successors, (int)count);
		bitset_set(node->successors, node->index);

		spa_list_for_each(e, &node->outgoing, src_link)
			bitset_or(node->successors, e->dst->successors, (int)count);
	}

	return 0;
}

/* Build the co-location group caches consumed by dag_comp_unrelated.
 *
 * Every real indexed node is assigned a dense group index: nodes
 * sharing a non-zero dag_node::group_id share the same dense group;
 * group_id == 0 (the default) makes the node its own singleton group.
 * Fictitious nodes are left out (node_group_index[i] = UINT32_MAX).
 *
 * For each group we materialise:
 *   - group_rep_node_index: the lowest-index member (the topologically
 *     earliest one, since indexed_nodes is in topo order). Used as the
 *     candidate in the antichain branch-and-bound.
 *   - group_members: bitset (over indexed_count) of every member.
 *     Used to expand a representative-only antichain into the full
 *     per-node bitset stored in g->unrelated, so the worst-fit pass
 *     still accumulates each member's utilisation contribution.
 *   - group_node_succ: union of every member's node-level successors
 *     bitset. Used as the group-level reachability test: a candidate
 *     v's group is related to a chosen u's group iff
 *     group_node_succ[g(u)] intersects group_members[g(v)] (or vice
 *     versa). Since each member's successors mask includes itself,
 *     this also captures intra-group membership without a special
 *     case.
 *
 * Singleton-group case (no non-zero group_ids in the DAG): the result
 * is equivalent to the pre-collapse algorithm -- each rep equals its
 * sole member, every group_node_succ equals the member's successors
 * mask, and expansion is a no-op. So workloads that never invoke
 * chain-merge or subgraph-fusion see no behavioural change.
 *
 * The function is called by dag_build_analysis after
 * dag_build_successors and before dag_comp_unrelated. Returns 0 on
 * success, -1 with errno=ENOMEM on allocation failure; partial state
 * is freed via dag_free_groups (already wired into
 * dag_invalidate_analysis). */
static int dag_build_groups(dag_t *g)
{
	uint32_t count = g->indexed_count;
	uint32_t i, j;
	uint32_t group_count = 0;
	uint32_t *group_source_id = NULL;

	if (count == 0)
		return 0;

	g->node_group_index = malloc((size_t)count * sizeof(*g->node_group_index));
	if (!g->node_group_index) {
		errno = ENOMEM;
		return -1;
	}
	for (i = 0; i < count; i++)
		g->node_group_index[i] = UINT32_MAX;

	/* Worst case is one dense group per real node (every node
	 * ungrouped, group_id == 0); allocate to that ceiling so we can
	 * resolve duplicates in a single pass without rehashing. */
	group_source_id = malloc((size_t)count * sizeof(*group_source_id));
	if (!group_source_id) {
		dag_free_groups(g);
		errno = ENOMEM;
		return -1;
	}

	for (i = 0; i < count; i++) {
		dag_node_t *n = g->indexed_nodes[i];
		uint32_t gid;
		uint32_t dense = UINT32_MAX;

		if (n->fictitious)
			continue;

		gid = n->group_id;
		if (gid != 0) {
			for (j = 0; j < group_count; j++) {
				if (group_source_id[j] == gid) {
					dense = j;
					break;
				}
			}
		}
		if (dense == UINT32_MAX) {
			dense = group_count++;
			/* Singletons share source-id 0 in the lookup
			 * table but never collide because we only consult
			 * the table when gid != 0. */
			group_source_id[dense] = gid;
		}
		g->node_group_index[i] = dense;
	}

	g->group_count = group_count;
	if (group_count == 0) {
		free(group_source_id);
		return 0;
	}

	g->group_members = calloc(group_count, sizeof(*g->group_members));
	g->group_node_succ = calloc(group_count, sizeof(*g->group_node_succ));
	g->group_rep_node_index = malloc((size_t)group_count *
			sizeof(*g->group_rep_node_index));
	if (!g->group_members || !g->group_node_succ || !g->group_rep_node_index) {
		free(group_source_id);
		dag_free_groups(g);
		errno = ENOMEM;
		return -1;
	}
	for (i = 0; i < group_count; i++) {
		g->group_rep_node_index[i] = UINT32_MAX;
		g->group_members[i] = bitset_alloc((int)count);
		g->group_node_succ[i] = bitset_alloc((int)count);
		if (!g->group_members[i] || !g->group_node_succ[i]) {
			free(group_source_id);
			dag_free_groups(g);
			errno = ENOMEM;
			return -1;
		}
	}

	for (i = 0; i < count; i++) {
		uint32_t dense = g->node_group_index[i];
		if (dense == UINT32_MAX)
			continue;
		bitset_set(g->group_members[dense], i);
		bitset_or(g->group_node_succ[dense],
				g->indexed_nodes[i]->successors, (int)count);
		if (g->group_rep_node_index[dense] == UINT32_MAX)
			g->group_rep_node_index[dense] = i;
	}

	free(group_source_id);
	return 0;
}

static int dag_ensure_unrelated_capacity(dag_t *g, uint32_t needed)
{
	if (g->unrelated_capacity >= needed)
		return 0;

	uint32_t new_capacity = g->unrelated_capacity ? g->unrelated_capacity : 16;
	while (new_capacity < needed) {
		if (new_capacity > UINT32_MAX / 2) {
			errno = ENOMEM;
			return -1;
		}
		new_capacity *= 2;
	}

	bitset_t **unrelated = realloc(g->unrelated, (size_t)new_capacity * sizeof(*unrelated));
	if (!unrelated)
		return -1;

	for (uint32_t i = g->unrelated_capacity; i < new_capacity; i++)
		unrelated[i] = NULL;

	g->unrelated = unrelated;
	g->unrelated_capacity = new_capacity;
	return 0;
}

static int dag_add_unrelated(dag_t *g, bitset_t *set)
{
	for (uint32_t i = 0; i < g->unrelated_size; i++) {
		if (bitset_includes(g->unrelated[i], set, (int)g->indexed_count)) {
			return 0;
		} else if (bitset_includes(set, g->unrelated[i], (int)g->indexed_count)) {
			bitset_cpy(g->unrelated[i], set, (int)g->indexed_count);
			return 0;
		}
	}

	if (dag_ensure_unrelated_capacity(g, g->unrelated_size + 1) < 0)
		return -1;

	if (!g->unrelated[g->unrelated_size]) {
		g->unrelated[g->unrelated_size] = bitset_alloc((int)g->indexed_count);
		if (!g->unrelated[g->unrelated_size])
			return -1;
	}

	bitset_cpy(g->unrelated[g->unrelated_size], set, (int)g->indexed_count);
	g->unrelated_size++;
	return 1;
}

static void dag_unrelated_squash(dag_t *g)
{
	for (uint32_t s1 = 0; s1 < g->unrelated_size; s1++) {
		for (uint32_t s2 = s1 + 1; s2 < g->unrelated_size; s2++) {
			if (bitset_includes(g->unrelated[s1], g->unrelated[s2], (int)g->indexed_count)) {
				bitset_t *tmp = g->unrelated[s2];
				g->unrelated[s2] = g->unrelated[g->unrelated_size - 1];
				g->unrelated[g->unrelated_size - 1] = tmp;
				g->unrelated_size--;
				s2--;
			} else if (bitset_includes(g->unrelated[s2], g->unrelated[s1], (int)g->indexed_count)) {
				bitset_t *tmp = g->unrelated[s1];
				g->unrelated[s1] = g->unrelated[g->unrelated_size - 1];
				g->unrelated[g->unrelated_size - 1] = tmp;
				g->unrelated_size--;
				s1--;
				break;
			}
		}
	}
}

/* Group-level reachability mask for `n`. When the group caches are
 * present, returns the union of node-level successors over every
 * member of n's group; otherwise falls back to n's own successors.
 * Equivalent to n->successors for singleton groups, so the
 * no-grouping case sees no behavioural change. */
static inline bitset_t *dag_antichain_succ_mask(dag_t *g, dag_node_t *n)
{
	if (!n)
		return NULL;
	if (g->node_group_index && n->index != DAG_NODE_INDEX_INVALID) {
		uint32_t gi = g->node_group_index[n->index];
		if (gi != UINT32_MAX)
			return g->group_node_succ[gi];
	}
	return n->successors;
}

/* Group-level "are these two nodes related?" predicate. Two nodes are
 * related when their groups are: some member of one's group is
 * reachable from some member of the other's. The intersect-based
 * check accounts for the case where a representative is unreachable
 * but a non-representative member is; testing the representatives
 * alone would miss that. For singleton groups this reduces exactly
 * to dag_nodes_are_related (each group_node_succ equals its sole
 * member's successors mask, and group_members is a single-bit set
 * pointing at the rep), so the fallback path is just a defensive
 * shortcut when the caches are absent. */
static bool dag_antichain_related(dag_t *g, dag_node_t *a, dag_node_t *b)
{
	if (!a || !b || !a->successors || !b->successors)
		return false;

	if (g->node_group_index &&
			a->index != DAG_NODE_INDEX_INVALID &&
			b->index != DAG_NODE_INDEX_INVALID) {
		uint32_t ga = g->node_group_index[a->index];
		uint32_t gb = g->node_group_index[b->index];

		if (ga != UINT32_MAX && gb != UINT32_MAX) {
			return bitset_intersects(g->group_node_succ[ga],
					g->group_members[gb],
					(int)g->indexed_count) ||
				bitset_intersects(g->group_node_succ[gb],
					g->group_members[ga],
					(int)g->indexed_count);
		}
	}

	return dag_nodes_are_related(a, b);
}

static void dag_build_antichain_initial_stack(dag_t *g, bitset_t *stack)
{
	bitset_zero(stack, (int)g->indexed_count);

	if (g->group_count > 0) {
		/* Only one bit per group: the representative. The antichain
		 * enumeration's branch-and-bound thus has |groups|, not
		 * |real nodes|, candidates at every level; for merged
		 * chains or fused subgraphs that ratio can be a large
		 * constant. Other group members are not in the stack and
		 * are re-introduced into the emitted bitsets only via the
		 * post-recursion expansion in dag_comp_unrelated_recur. */
		for (uint32_t i = 0; i < g->group_count; i++) {
			uint32_t rep = g->group_rep_node_index[i];
			if (rep != UINT32_MAX)
				bitset_set(stack, rep);
		}
		return;
	}

	for (uint32_t i = 0; i < g->indexed_count; i++) {
		if (!g->indexed_nodes[i]->fictitious)
			bitset_set(stack, i);
	}
}

static void dag_build_antichain_next_stack(dag_t *g, bitset_t *dst,
		bitset_t *src, uint32_t chosen_index)
{
	dag_node_t *chosen = g->indexed_nodes[chosen_index];
	bitset_t *chosen_succ = dag_antichain_succ_mask(g, chosen);
	int candidate;

	bitset_cpy(dst, src, (int)g->indexed_count);
	bitset_nclear(dst, 0, (int)chosen_index);
	bitset_andnot(dst, chosen_succ, (int)g->indexed_count);

	for (candidate = bitset_next_set(dst, (int)g->indexed_count, -1);
			candidate >= 0;
			candidate = bitset_next_set(dst, (int)g->indexed_count, candidate)) {
		dag_node_t *node = g->indexed_nodes[candidate];

		if (node->fictitious || dag_antichain_related(g, node, chosen))
			bitset_clear(dst, candidate);
	}
}

static bool dag_antichain_is_redundant(dag_t *g, bitset_t *stack,
		int last_added, uint32_t chosen_index)
{
	dag_node_t *chosen = g->indexed_nodes[chosen_index];
	int candidate = bitset_next_set(stack, (int)g->indexed_count, last_added);

	while (candidate >= 0 && (uint32_t)candidate < chosen_index) {
		if (!dag_antichain_related(g, g->indexed_nodes[candidate], chosen))
			return true;
		candidate = bitset_next_set(stack, (int)g->indexed_count, candidate);
	}

	return false;
}

/* Expand a representative-only antichain bitset into its full per-node
 * membership: every set bit in `src` is mapped to its group's member
 * bitset and ORed into `dst`. The downstream worst-fit pass tests
 * per-node bits, so the stored unrelated sets must carry every
 * member, not just the representative. For singleton groups this
 * leaves the input unchanged; the no-group branch in
 * dag_comp_unrelated_recur skips the call entirely. */
static void dag_unrelated_expand_groups(dag_t *g, bitset_t *dst, bitset_t *src)
{
	bitset_cpy(dst, src, (int)g->indexed_count);
	if (g->group_count == 0 || !g->node_group_index)
		return;

	for (int i = bitset_next_set(src, (int)g->indexed_count, -1);
			i >= 0;
			i = bitset_next_set(src, (int)g->indexed_count, i)) {
		uint32_t gi = g->node_group_index[i];
		if (gi == UINT32_MAX)
			continue;
		bitset_or(dst, g->group_members[gi], (int)g->indexed_count);
	}
}

static int dag_comp_unrelated_recur(dag_t *g, bitset_t *curr_cut,
		bitset_t *stack, int last_added)
{
	int candidate = bitset_next_set(stack, (int)g->indexed_count, last_added);

	while (candidate >= 0) {
		bitset_decl_cpy(new_cut, curr_cut, g->indexed_count);
		bitset_decl_zero(new_stack, (int)g->indexed_count);

		bitset_set(new_cut, candidate);
		dag_build_antichain_next_stack(g, new_stack, stack, (uint32_t)candidate);

		if (bitset_empty(new_stack, (int)g->indexed_count)) {
			if (!dag_antichain_is_redundant(g, stack, last_added, (uint32_t)candidate)) {
				bitset_decl_zero(expanded, (int)g->indexed_count);
				dag_unrelated_expand_groups(g, expanded, new_cut);
				if (dag_add_unrelated(g, expanded) < 0)
					return -1;
			}
		} else if (dag_comp_unrelated_recur(g, new_cut, new_stack, candidate) < 0) {
			return -1;
		}

		candidate = bitset_next_set(stack, (int)g->indexed_count, candidate);
	}

	return 0;
}

static int dag_comp_unrelated(dag_t *g)
{
	bitset_decl_zero(curr_cut, (int)g->indexed_count);
	bitset_decl_zero(stack, (int)g->indexed_count);

	dag_build_antichain_initial_stack(g, stack);
	if (bitset_empty(stack, (int)g->indexed_count))
		return 0;

	if (dag_comp_unrelated_recur(g, curr_cut, stack, -1) < 0)
		return -1;

	dag_unrelated_squash(g);
	return 0;
}

/* Epsilon used when comparing derived per-CPU loads against the
 * configured utilization cap. The cap is an exact user-provided
 * value (e.g. 0.95); accumulated cpu_set_util sums combine
 * (uint64) wcet / (uint64) deadline ratios in IEEE-754 double, so
 * a sum that should equal the cap exactly may differ by a unit in
 * the last place. A floor of 1 ULP at scale 1.0 is ~2.2e-16, but
 * we use a slightly looser epsilon of 1e-12 to absorb the
 * accumulation of many divisions in graphs with up to a few dozen
 * nodes -- still tight enough that no genuinely overloaded graph
 * can sneak past it. */
#define DAG_LOAD_EPSILON 1e-12

/* Minimum positive relative-deadline unit reserved per real node.
 * A successful dag_recalculate must never export a zero relative
 * deadline (the kernel's SCHED_DEADLINE policy rejects deadline=0
 * outright), so every node consumes at least this many ns from the
 * global deadline budget. */
#define DAG_MIN_RELATIVE_DEADLINE UINT64_C(1)

/* Sum of integer deadline weights across every real node. Used to
 * bound infeasibility: a graph whose N * 1ns minimum exceeds the
 * global deadline cannot produce any positive per-node deadline,
 * however small the WCETs are. Saturates at UINT64_MAX. */
static uint64_t dag_min_deadline_reservation(dag_t *g)
{
	dag_node_t *n;
	uint64_t total = 0;

	spa_list_for_each(n, &g->nodes, link) {
		if (n->fictitious)
			continue;
		if (UINT64_MAX - total < DAG_MIN_RELATIVE_DEADLINE)
			return UINT64_MAX;
		total += DAG_MIN_RELATIVE_DEADLINE;
	}

	return total;
}

/* Critical-path WCET across every real source-to-sink path. Reads
 * longest_len, which dag_populate_longest_paths has already
 * computed; the analyser's fictitious source has longest_len equal
 * to (max child longest_len), and every real source has
 * longest_len = its wcet + best downstream chain, so the max
 * longest_len across real nodes is the critical-path WCET of the
 * DAG. */
static uint64_t dag_critical_path_wcet(dag_t *g)
{
	uint64_t crit = 0;
	uint32_t i;

	for (i = 0; i < g->indexed_count; i++) {
		dag_node_t *n = g->indexed_nodes[i];

		if (n->fictitious)
			continue;
		if (n->longest_len > crit)
			crit = n->longest_len;
	}

	return crit;
}

/* Feasibility test. Run between dag_build_analysis and the
 * deadline-splitting step. Two failure modes:
 *   1. Critical-path WCET exceeds the global end-to-end deadline.
 *      No assignment can hide the fact that the heavy chain alone
 *      already overruns the budget.
 *   2. N * 1ns minimum reservation exceeds the global deadline.
 *      Even a graph whose WCETs were all zero needs at least 1ns
 *      per node to export a positive relative deadline.
 *
 * Returns 0 on feasible, -1 on infeasible with errno=EAGAIN. The
 * caller is responsible for marking the DAG dirty and clearing
 * assignments on failure (dag_recalculate already does this on any
 * pre-CPU-assignment failure). */
static int dag_check_feasibility(dag_t *g)
{
	uint64_t critical = dag_critical_path_wcet(g);
	uint64_t min_reservation = dag_min_deadline_reservation(g);

	/* The critical path's WCET is measured at the reference (fastest)
	 * CPU. To guarantee schedulability regardless of which CPUs the
	 * placer ends up using, scale the available deadline budget by
	 * the slowest CPU in the set: if the path lands on the slowest
	 * CPU it runs longer by a factor of 1 / min_relative_capacity,
	 * so its scaled WCET must still fit in the global deadline.
	 * Strictly conservative: a workload whose critical path actually
	 * lands on a fast CPU is rejected if it does not also fit on the
	 * slowest. The trade-off is acceptable because the slowest CPU
	 * is the only way to bound the worst-case placement without
	 * solving the full assignment problem at feasibility time. */
	double min_rel_cap = g->relative_capacity[0];
	for (uint32_t i = 1; i < g->num_cpus; i++) {
		if (g->relative_capacity[i] < min_rel_cap)
			min_rel_cap = g->relative_capacity[i];
	}
	double scaled_deadline = (double)g->deadline * min_rel_cap;

	if ((double)critical > scaled_deadline) {
		pw_log_warn("DAG critical path %" PRIu64 " ns exceeds "
				"slowest-CPU-scaled deadline %.0f ns "
				"(global deadline %" PRIu64 " ns, "
				"min relative capacity %.3f)",
				critical, scaled_deadline, g->deadline, min_rel_cap);
		errno = EAGAIN;
		return -1;
	}
	if (min_reservation > g->deadline) {
		pw_log_warn("DAG minimum per-node deadline reservation %"
				PRIu64 " ns exceeds global deadline %" PRIu64 " ns",
				min_reservation, g->deadline);
		errno = EAGAIN;
		return -1;
	}

	return 0;
}

static int dag_build_analysis(dag_t *g, dag_node_t **sources, uint32_t nsources, dag_node_t **sinks, uint32_t nsinks)
{
	if (dag_remove_fictitious_nodes(g) < 0)
		goto error;

	dag_invalidate_analysis(g);

	if (dag_add_fictitious_endpoints(g, sources, nsources, sinks, nsinks) < 0)
		goto error;

	if (dag_build_indexed_nodes(g) < 0)
		goto error;

	if (dag_build_successors(g) < 0)
		goto error;

	if (dag_build_groups(g) < 0)
		goto error;

	dag_populate_longest_paths(g);

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	if (dag_comp_unrelated(g) < 0)
		goto error;
	clock_gettime(CLOCK_MONOTONIC, &end);
	double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
	pw_log_debug("Unrelated set computation took %.6f seconds", elapsed);

	return 0;

error:
	dag_remove_fictitious_nodes(g);
	dag_invalidate_analysis(g);
	return -1;
}

/* Subtract `decrement` from `value` with a floor of 0. Used when the
 * deadline budget along a path is being depleted by nodes that have
 * already been assigned a tighter deadline by a sibling subproblem:
 * the discounted node can in principle consume the entire residual
 * budget, and any leftover must clamp to 0 instead of wrapping below
 * UINT64_MAX. */
static inline uint64_t saturating_sub_u64(uint64_t value, uint64_t decrement)
{
	return decrement >= value ? 0 : value - decrement;
}

/* Proportional split of `deadline_budget` according to a node's share
 * of the path's residual WCET. Returns 0 on any of the degenerate
 * inputs (budget=0, wcet=0, residual path=0); the caller treats 0 as
 * "do not tighten". */
static inline uint64_t proportional_deadline(uint64_t deadline_budget,
		uint64_t wcet, uint64_t path_wcet)
{
	if (deadline_budget == 0 || wcet == 0 || path_wcet == 0)
		return 0;

	return (uint64_t)floor((double)deadline_budget *
			((double)wcet / (double)path_wcet));
}

/* Assign `deadline` to `node`, or tighten if it already had one and
 * the new value is smaller. Never relaxes an existing assignment --
 * the splitter only ever discovers new tighter constraints. */
static inline void assign_or_tighten_deadline(dag_node_t *node, uint64_t deadline)
{
	if (!node->deadline_assigned) {
		node->deadline = deadline;
		node->deadline_assigned = true;
	} else if (node->deadline > deadline) {
		node->deadline = deadline;
	}
}

/* Phase A/B/C structure of the deadline splitter for one source-to-
 * sink path:
 *   A) compute_longest_path gives us the path; the caller provides
 *      it.
 *   B) discount any node that already has a tighter assigned deadline
 *      from a sibling subproblem. Those nodes are excluded from the
 *      proportional split and their WCET + assigned-deadline are
 *      saturating-subtracted from the residual path WCET and the
 *      residual budget respectively. The residual budget must NOT be
 *      taken back from the caller's view of "deadline_budget" (the
 *      previous code re-introduced that budget into recursive
 *      subproblems and over-allocated downstream).
 *   C) split the leftover budget proportionally and tighten path[0]
 *      (this is where the splitter is called once per topo-order
 *      source on a path to fictitious sink).
 *
 * On exit, *assigned_head (if non-NULL) gets path[0]->deadline so the
 * caller can update its own remaining_deadline accounting using the
 * actual assignment, which may be lower than what a naive
 * proportional split would have produced.
 */
/* `excluded_buf` is a scratch flag array provided by the caller
 * (the per-recalc workspace). The function zeroes the prefix it
 * needs (path_len entries) at entry. The caller does not free it;
 * the workspace owns the buffer.
 */
static int assign_path_head_deadline(dag_node_t **path, int path_len, uint64_t D, uint64_t L,
		bool *excluded_buf, uint64_t *assigned_head)
{
	dag_node_t *n_src;
	uint64_t residual_deadline = D;
	uint64_t residual_wcet = L;
	bool changed;

	if (!path || path_len <= 0 || !excluded_buf) {
		errno = EINVAL;
		return -1;
	}

	n_src = path[0];

	if (L == 0) {
		assign_or_tighten_deadline(n_src, 0);
		if (assigned_head)
			*assigned_head = n_src->deadline;
		return 0;
	}

	memset(excluded_buf, 0, (size_t)path_len * sizeof(*excluded_buf));

	/* Phase B: discount tighter pre-assigned deadlines. The loop
	 * restarts after each discount because the proportional share
	 * of every other node shifts. Discounts use saturating
	 * subtraction on residual budget and residual path length. */
	changed = true;
	while (changed) {
		changed = false;

		if (residual_deadline == 0 || residual_wcet == 0)
			break;

		for (int i = 0; i < path_len; i++) {
			dag_node_t *ni;
			uint64_t d_prime;

			if (excluded_buf[i])
				continue;

			ni = path[i];
			d_prime = proportional_deadline(residual_deadline,
					ni->wcet, residual_wcet);

			if (ni->deadline_assigned && ni->deadline < d_prime) {
				residual_deadline = saturating_sub_u64(
						residual_deadline, ni->deadline);
				residual_wcet = saturating_sub_u64(
						residual_wcet, ni->wcet);
				excluded_buf[i] = true;
				changed = true;
				break;
			}
		}
	}

	if (residual_deadline == 0 || residual_wcet == 0) {
		assign_or_tighten_deadline(n_src, 0);
		if (assigned_head)
			*assigned_head = n_src->deadline;
		return 0;
	}

	/* Phase C: assign/tighten the path head. */
	assign_or_tighten_deadline(n_src,
			proportional_deadline(residual_deadline,
				n_src->wcet, residual_wcet));

	if (assigned_head)
		*assigned_head = n_src->deadline;

	return 0;
}

/* Recursive splitter kept for the documentation it provides on the
 * discount / residual-budget contract. The active analysis uses the
 * iterative form (assign_deadlines_iterative below), which
 * exercises the same helpers; this routine is exercised by the
 * residual_budget_no_double_credit regression test. */
static SPA_UNUSED int assign_deadlines_recursive(dag_t *g, dag_node_t *src, dag_node_t *dst, uint64_t D)
{
	int path_len;
	uint64_t L;
	uint64_t residual_deadline;
	uint64_t residual_wcet;
	bool src_discounted = false;
	uint64_t discounted_src_deadline = 0;
	bool changed;

	/* The recursive form uses local malloc'd scratch buffers
	 * because it nests (multiple subproblems live simultaneously
	 * on the C stack); the iterative form is the active path and
	 * uses the per-recalc workspace below. */
	dag_node_t **P;
	bool *excluded;

	if (src == dst) {
		assign_or_tighten_deadline(src, D);
		return 0;
	}

	P = calloc(g->indexed_count, sizeof(*P));
	if (!P)
		return -1;
	excluded = calloc(g->indexed_count, sizeof(*excluded));
	if (!excluded) {
		free(P);
		return -1;
	}

	/* Phase A: longest path of this subproblem. */
	L = compute_longest_path(g, src, dst, P, &path_len);
	if (path_len == 0 || L == 0) {
		free(P);
		free(excluded);
		return -1;
	}

	/* Phase B: discount pre-assigned tighter deadlines from the
	 * residual budget and the residual path length. */
	residual_deadline = D;
	residual_wcet = L;

	changed = true;
	while (changed) {
		changed = false;

		if (residual_deadline == 0 || residual_wcet == 0)
			break;

		for (int i = 0; i < path_len; i++) {
			dag_node_t *ni;
			uint64_t d_prime;

			if (excluded[i])
				continue;

			ni = P[i];
			d_prime = proportional_deadline(residual_deadline,
					ni->wcet, residual_wcet);

			if (ni->deadline_assigned && ni->deadline < d_prime) {
				residual_deadline = saturating_sub_u64(
						residual_deadline, ni->deadline);
				residual_wcet = saturating_sub_u64(
						residual_wcet, ni->wcet);
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
		free(P);
		free(excluded);
		return 0;
	}

	/* Phase C: assign/tighten endpoints with the residual budget. */
	{
		dag_node_t *n_src = P[0];
		dag_node_t *n_dst = P[path_len - 1];

		assign_or_tighten_deadline(n_src,
				proportional_deadline(residual_deadline,
					n_src->wcet, residual_wcet));
		assign_or_tighten_deadline(n_dst,
				proportional_deadline(residual_deadline,
					n_dst->wcet, residual_wcet));
	}

	/* Recurse with the post-source residual budget. If the source
	 * was discounted in phase B, then phase C may have tightened it
	 * further; refund that delta so descendants see the true
	 * remaining budget instead of double-charging it. Otherwise the
	 * source consumed `n_src->deadline` from the residual. */
	{
		dag_node_t *n_src = P[0];
		uint64_t D_residual = residual_deadline;
		dag_edge_t *e;

		if (src_discounted)
			D_residual += discounted_src_deadline - n_src->deadline;
		else
			D_residual = saturating_sub_u64(D_residual, n_src->deadline);

		spa_list_for_each(e, &src->outgoing, src_link) {
			if (e->dst == dst)
				continue;
			if (!bitset_test(e->dst->successors, dst->index))
				continue;

			if (assign_deadlines_recursive(g, e->dst, dst, D_residual) < 0) {
				free(P);
				free(excluded);
				return -1;
			}
		}
	}

	free(P);
	free(excluded);
	return 0;
}

static int assign_deadlines_iterative(dag_t *g)
{
	dag_node_t *fictitious_src = dag_find_fictitious_source(g);
	dag_node_t *fictitious_sink = dag_find_fictitious_sink(g);

	if (!fictitious_src || !fictitious_sink) {
		pw_log_error("missing fictitious endpoints for iterative deadline assignment");
		errno = EFAULT;
		return -1;
	}
	if (!g->ws_path || !g->ws_excluded) {
		pw_log_error("missing per-recalc workspace for iterative deadline assignment");
		errno = EFAULT;
		return -1;
	}

	fictitious_src->deadline = 0;
	fictitious_src->deadline_assigned = true;
	fictitious_src->remaining_deadline = g->deadline;

	fictitious_sink->deadline = 0;
	fictitious_sink->deadline_assigned = true;
	fictitious_sink->remaining_deadline = 0;

	for (uint32_t i = 0; i < g->indexed_count; i++) {
		dag_node_t *node = g->indexed_nodes[i];
		uint64_t available_deadline = 0;
		uint64_t assigned_deadline = 0;
		uint64_t path_len;
		bool has_predecessor = false;
		dag_edge_t *e;
		int path_nodes = 0;

		if (node == fictitious_src) {
			available_deadline = g->deadline;
		} else {
			spa_list_for_each(e, &node->incoming, dst_link) {
				uint64_t candidate = e->src->remaining_deadline;

				if (!has_predecessor || candidate < available_deadline)
					available_deadline = candidate;
				has_predecessor = true;
			}

			if (!has_predecessor) {
				pw_log_error("node %u has no predecessor while assigning iterative deadlines",
						node->id);
				errno = EFAULT;
				return -1;
			}
		}

		node->remaining_deadline = available_deadline;
		path_len = compute_longest_path(g, node, fictitious_sink,
				g->ws_path, &path_nodes);
		if (path_nodes == 0 || (node != fictitious_sink && path_len == 0)) {
			pw_log_error("failed to compute longest path from node %u to fictitious sink",
					node->id);
			errno = EFAULT;
			return -1;
		}

		if (assign_path_head_deadline(g->ws_path, path_nodes,
					available_deadline, path_len,
					g->ws_excluded, &assigned_deadline) < 0) {
			return -1;
		}

		node->remaining_deadline = available_deadline > assigned_deadline ?
			available_deadline - assigned_deadline : 0;
	}

	return 0;
}

struct cpu_assignment_info {
	dag_node_t *node;
	uint32_t index;
	double util;
	/* Critical-path priority: WCET of the longest weighted path from
	 * this node to any sink, *inclusive* of the node's own WCET. This
	 * is exactly the HEFT "upward rank" rank_u defined in Topcuoglu,
	 * Hariri, Wu, "Performance-Effective and Low-Complexity Task
	 * Scheduling for Heterogeneous Computing", IEEE TPDS 13(3):260-274,
	 * 2002 (papers/Topcuoglu-HEFT-TPDS2002.pdf), eq. (8), specialised
	 * to communication cost c_{i,j} = 0 -- our DAG models in-process
	 * audio flows where edges carry no measured transfer cost. The
	 * value is read straight from dag_node_t::longest_len, which
	 * dag_populate_longest_paths has already computed as part of the
	 * deadline-splitting pass; no extra pass is needed. */
	uint64_t cp_priority;
};

/* qsort comparator for CPU-assignment-info entries. Primary key:
 * critical-path priority (descending) -- nodes that constrain the
 * longest source-to-sink path are placed first so the worst-fit
 * loop commits to the tightest deadlines while bins are still
 * empty. Secondary key: utilisation density (descending) -- among
 * nodes whose downstream critical paths are equally long the denser
 * one is harder to admit, so it takes precedence in the worst-fit
 * scan. Final tie-break: topological index (ascending) -- a
 * deterministic, ordering-stable tie-breaker that keeps the
 * placement reproducible across runs and across compilers' qsort
 * implementations (which are not stable in general). */
static int compare_cp_priority_desc(uint64_t a_priority, double a_density,
		uint32_t a_key, uint64_t b_priority, double b_density,
		uint32_t b_key)
{
	if (a_priority < b_priority)
		return 1;
	if (a_priority > b_priority)
		return -1;
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

static int compare_cpu_assignment_info_desc(const void *a, const void *b)
{
	const struct cpu_assignment_info *info_a = a;
	const struct cpu_assignment_info *info_b = b;

	/* Place critical-path-heavier tasks first; equal-priority nodes
	 * fall back to density-descending; equal-density nodes keep
	 * topological order. */
	return compare_cp_priority_desc(
			info_a->cp_priority, info_a->util, info_a->index,
			info_b->cp_priority, info_b->util, info_b->index);
}

/* Per-candidate tie-break used by the worst-fit CPU loop in
 * assign_cpus. Returns true if (projected, cpu) is the new winner
 * compared to (best_projected, best_cpu). Lowest projected load
 * wins; equal-load placements pick the lowest CPU index so the
 * output is reproducible. */
static bool prefer_cpu_choice(double projected, uint32_t cpu,
		double best_projected, int best_cpu)
{
	if (best_cpu < 0)
		return true;
	if (projected < best_projected)
		return true;

	/* When two CPUs yield the same projected load, prefer the lowest index. */
	return projected == best_projected && (int) cpu < best_cpu;
}

/* Per-CPU admission accounting. Two tasks are *related* iff either
 * can reach the other through some path (the bitset closure built
 * by dag_build_successors makes this a constant-time test). Tasks
 * on the same CPU that are mutually unrelated can in principle run
 * concurrently in different periods, so their per-CPU densities
 * must be summed; but the relevant quantity for admission is not the
 * raw sum of densities -- it is the maximum total density over any
 * pairwise-unrelated subset assigned to that CPU.
 *
 * The unrelated sets are pre-computed by dag_comp_unrelated (one
 * pass over the antichain enumeration of the partial order); for
 * each CPU we maintain a per-unrelated-set running sum
 * (cpu_set_util[cpu * unrelated_size + s]), and admission checks
 * the worst case across all sets containing the candidate node.
 *
 * Placement order. Candidates are sorted by *critical-path priority*
 * descending: the longest weighted path from the candidate to any
 * sink (its longest_len, i.e. HEFT's upward rank rank_u with zero
 * communication cost -- Topcuoglu et al. 2002,
 * papers/Topcuoglu-HEFT-TPDS2002.pdf, eq. 8 and step 3 of Fig. 2).
 * The intuition is that nodes on the longest chain receive the
 * tightest deadlines from the proportional split (see Saifullah et
 * al. 2014, papers/Saifullah-ParallelRTDAGs-TPDS2014.pdf, for the
 * decomposition rule we already use in assign_deadlines_iterative);
 * placing them first lets the worst-fit search commit to those
 * tight admissions while every CPU bin is still empty. Utilisation
 * density (ratio between WCET and the binding-deadline budget) is
 * the secondary key because among equal-CP nodes the denser one is
 * harder to admit. This is the partitioned-fixed-priority allocation
 * framework studied in Casini et al. 2018,
 * papers/Casini-PartitionedFP-RTSS2018.pdf -- worst-fit/best-fit
 * heuristics where the ordering of the input list dominates the
 * resulting feasibility ratio. */
static int assign_cpus(dag_t *g)
{
	uint32_t count = g->indexed_count;
	uint32_t unrelated_size = g->unrelated_size;
	uint32_t num_cpus = g->num_cpus;
	struct cpu_assignment_info *info;

	if (count == 0)
		return 0;

	for (uint32_t i = 0; i < count; i++) {
		if (g->indexed_nodes[i]->fictitious)
			continue;
		if (!g->indexed_nodes[i]->deadline_assigned) {
			pw_log_error("Node %u has no assigned deadline", g->indexed_nodes[i]->id);
			errno = EFAULT;
			return -1;
		}
	}

	info = calloc(count, sizeof(*info));
	if (!info)
		return -1;

	for (uint32_t i = 0; i < count; i++) {
		if (g->indexed_nodes[i]->fictitious) {
			info[i].node = g->indexed_nodes[i];
			info[i].index = g->indexed_nodes[i]->index;
			info[i].util = 0.0;
			info[i].cp_priority = 0;
			continue;
		}

		double denom = (double)((g->indexed_nodes[i]->deadline < g->period) ?
				g->indexed_nodes[i]->deadline : g->period);
		if (denom == 0.0) {
			pw_log_error("Node %u has zero deadline denominator", g->indexed_nodes[i]->id);
			free(info);
			errno = EFAULT;
			return -1;
		}

		info[i].node = g->indexed_nodes[i];
		info[i].index = g->indexed_nodes[i]->index;
		info[i].util = ((double)g->indexed_nodes[i]->wcet) / denom;
		/* longest_len is set by dag_populate_longest_paths, which
		 * dag_recalculate has already called on this g (either via
		 * dag_build_analysis for a fresh build, or directly on the
		 * cached-analysis path before reaching assign_cpus). It is
		 * inclusive of the node's own WCET and includes the best
		 * downstream chain to a sink; that is the HEFT upward rank. */
		info[i].cp_priority = g->indexed_nodes[i]->longest_len;
	}

	qsort(info, count, sizeof(*info), compare_cpu_assignment_info_desc);

	double *cpu_peak = calloc(num_cpus, sizeof(*cpu_peak));
	double *cpu_set_util = calloc((size_t)num_cpus * unrelated_size, sizeof(*cpu_set_util));
	if (!cpu_peak || (!cpu_set_util && unrelated_size > 0)) {
		free(cpu_set_util);
		free(cpu_peak);
		free(info);
		return -1;
	}

	/* Co-location bookkeeping: group_cpu[group_id] holds the CPU
	 * picked by the first (highest-utilisation) member of the
	 * group, or -1 while the group is still unplaced. Group id 0
	 * means "ungrouped" and never participates; we waste slot 0
	 * for an unbranched index. The array is sized to the max
	 * group id observed in this DAG, which is small in any
	 * realistic workload (one group per merged chain). */
	uint32_t max_group_id = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (info[i].node->fictitious)
			continue;
		if (info[i].node->group_id > max_group_id)
			max_group_id = info[i].node->group_id;
	}

	int *group_cpu = NULL;
	if (max_group_id > 0) {
		group_cpu = malloc((size_t)(max_group_id + 1) * sizeof(int));
		if (!group_cpu) {
			free(cpu_set_util);
			free(cpu_peak);
			free(info);
			return -1;
		}
		for (uint32_t i = 0; i <= max_group_id; i++)
			group_cpu[i] = -1;
	}

	/* cpu_peak and cpu_set_util both accumulate *relative*
	 * utilisation: a node with raw density u placed on CPU c
	 * contributes u / relative_capacity[c]. On the homogeneous
	 * case (all relative_capacity entries == 1.0) this collapses
	 * to the original arithmetic. */
	for (uint32_t i = 0; i < count; i++) {
		if (info[i].node->fictitious)
			continue;

		double u = info[i].util;
		uint32_t gid = info[i].node->group_id;
		int forced_cpu = (gid != 0 && group_cpu != NULL) ? group_cpu[gid] : -1;
		int chosen = -1;
		double chosen_projected = DBL_MAX;

		if (forced_cpu >= 0) {
			/* A previously placed group member already picked
			 * a CPU; we must use it. Check admission on that
			 * single CPU only; if it doesn't fit, the group's
			 * placement is infeasible and the whole DAG fails
			 * EAGAIN -- splitting a group across CPUs is not
			 * allowed because it would invalidate the
			 * thread-merge done by libpipewire. */
			uint32_t c = (uint32_t)forced_cpu;
			double rc = g->relative_capacity[c];
			double u_rel = u / rc;
			double projected = cpu_peak[c] > u_rel ? cpu_peak[c] : u_rel;

			for (uint32_t s = 0; s < unrelated_size; s++) {
				if (!bitset_test(g->unrelated[s], info[i].node->index))
					continue;

				double candidate = cpu_set_util[(size_t)c * unrelated_size + s] + u_rel;
				if (candidate > projected)
					projected = candidate;
			}

			if (projected <= g->admission_ceiling + DAG_LOAD_EPSILON) {
				chosen = (int)c;
				chosen_projected = projected;
			}
		} else {
			for (uint32_t c = 0; c < num_cpus; c++) {
				double rc = g->relative_capacity[c];
				double u_rel = u / rc;
				double projected = cpu_peak[c] > u_rel ? cpu_peak[c] : u_rel;

				for (uint32_t s = 0; s < unrelated_size; s++) {
					if (!bitset_test(g->unrelated[s], info[i].node->index))
						continue;

					double candidate = cpu_set_util[(size_t)c * unrelated_size + s] + u_rel;
					if (candidate > projected)
						projected = candidate;
				}

				if (projected <= g->admission_ceiling + DAG_LOAD_EPSILON &&
						prefer_cpu_choice(projected, c,
							chosen_projected, chosen)) {
					chosen_projected = projected;
					chosen = (int)c;
				}
			}
		}

		if (chosen < 0) {
			free(group_cpu);
			free(cpu_set_util);
			free(cpu_peak);
			free(info);
			errno = EAGAIN;
			return -1;
		}

		info[i].node->cpu = (uint32_t)chosen;
		if (gid != 0 && group_cpu != NULL && group_cpu[gid] < 0)
			group_cpu[gid] = chosen;
		double rc_chosen = g->relative_capacity[chosen];
		double u_rel_chosen = u / rc_chosen;
		double projected = cpu_peak[chosen] > u_rel_chosen ?
			cpu_peak[chosen] : u_rel_chosen;

		for (uint32_t s = 0; s < unrelated_size; s++) {
			size_t offset = (size_t)chosen * unrelated_size + s;
			if (!bitset_test(g->unrelated[s], info[i].node->index))
				continue;

			cpu_set_util[offset] += u_rel_chosen;
			if (cpu_set_util[offset] > projected)
				projected = cpu_set_util[offset];
		}
		cpu_peak[chosen] = projected;
	}

	free(group_cpu);
	free(cpu_set_util);
	free(cpu_peak);
	free(info);
	return 0;
}

double dag_per_cpu_density(const dag_t *g, uint32_t cpu)
{
	double sum = 0.0;
	double rel;
	dag_node_t *n;

	if (g == NULL || cpu >= g->num_cpus)
		return 0.0;
	if (g->period == 0)
		return 0.0;

	rel = g->relative_capacity != NULL ? g->relative_capacity[cpu] : 1.0;
	if (rel <= 0.0)
		return 0.0;

	spa_list_for_each(n, &g->nodes, link) {
		uint64_t d;
		if (n->fictitious)
			continue;
		if (n->cpu == DAG_CPU_INVALID || n->cpu != cpu)
			continue;
		if (n->wcet == 0)
			continue;
		/* min(D_i, T_i): a node whose local deadline is
		 * larger than the period is treated as if D == T per
		 * the kernel's SCHED_DEADLINE clamp. */
		d = n->local_deadline != 0 ? n->local_deadline : g->period;
		if (d > g->period)
			d = g->period;
		if (d == 0)
			continue;
		sum += ((double)n->wcet / (double)d) / rel;
	}
	return sum;
}

bool dag_density_feasible(const dag_t *g,
		double *out_max_density,
		uint32_t *out_failing_cpu)
{
	uint32_t i, worst_cpu = 0;
	double worst = 0.0;

	if (g == NULL)
		return false;

	for (i = 0; i < g->num_cpus; i++) {
		double d = dag_per_cpu_density(g, i);
		if (d > worst) {
			worst = d;
			worst_cpu = i;
		}
	}

	if (out_max_density != NULL)
		*out_max_density = worst;
	if (out_failing_cpu != NULL)
		*out_failing_cpu = worst_cpu;

	return worst <= 1.0;
}

/* Forward topological pass: assign each real node a graph-relative
 * cumulative deadline equal to max(pred.cumulative_deadline) +
 * own splitter slice (node->deadline). Source nodes (no real
 * predecessor) inherit their splitter slice directly. Fictitious
 * endpoints are zeroed; the splitter never schedules them via the
 * kernel.
 *
 * The result is the analysis layer's natural unit: a milestone
 * measured from the driver-graph's activation. The follow-up step
 * dag_compute_local_deadlines() converts it back to the kernel
 * API's relative form.
 *
 * Requires g->indexed_nodes to be populated in topological order
 * (dag_build_analysis already does this for the longest-path
 * passes the splitter consumes). Skips quietly when the cache is
 * absent.
 */
static void dag_assign_cumulative_deadlines(dag_t *g)
{
	if (!g || !g->indexed_nodes)
		return;
	for (uint32_t i = 0; i < g->indexed_count; i++) {
		dag_node_t *n = g->indexed_nodes[i];
		uint64_t max_pred = 0;
		dag_edge_t *e;

		if (n->fictitious) {
			n->cumulative_deadline = 0;
			n->local_deadline = 0;
			continue;
		}

		spa_list_for_each(e, &n->incoming, dst_link) {
			if (e->src->fictitious)
				continue;
			if (e->src->cumulative_deadline > max_pred)
				max_pred = e->src->cumulative_deadline;
		}

		n->cumulative_deadline = max_pred + n->deadline;
	}
}

bool dag_compute_local_deadlines(dag_t *g)
{
	if (!g || !g->indexed_nodes) {
		errno = EINVAL;
		return false;
	}
	for (uint32_t i = 0; i < g->indexed_count; i++) {
		dag_node_t *n = g->indexed_nodes[i];
		uint64_t max_pred = 0;
		dag_edge_t *e;
		bool has_real_pred = false;

		if (n->fictitious) {
			n->local_deadline = 0;
			continue;
		}

		spa_list_for_each(e, &n->incoming, dst_link) {
			if (e->src->fictitious)
				continue;
			/* Monotonicity: every real predecessor's
			 * cumulative deadline must be <= this node's.
			 * Failure means the analysis layer produced
			 * inconsistent cumulative milestones, which would
			 * also break the path-sum constraint. */
			if (e->src->cumulative_deadline > n->cumulative_deadline) {
				pw_log_error("non-monotonic cumulative deadline "
					     "along edge %u -> %u "
					     "(%" PRIu64 " > %" PRIu64 ")",
					     e->src->id, n->id,
					     e->src->cumulative_deadline,
					     n->cumulative_deadline);
				errno = EINVAL;
				return false;
			}
			if (e->src->cumulative_deadline > max_pred)
				max_pred = e->src->cumulative_deadline;
			has_real_pred = true;
		}

		if (!has_real_pred) {
			/* Source node: local equals cumulative -- the
			 * node is released at the graph's activation. */
			n->local_deadline = n->cumulative_deadline;
		} else {
			n->local_deadline = n->cumulative_deadline - max_pred;
		}

		if (n->local_deadline == 0) {
			pw_log_error("node %u has zero local deadline after "
				     "cumulative-to-local conversion", n->id);
			errno = EINVAL;
			return false;
		}
		if (n->local_deadline > g->period) {
			/* The kernel SCHED_DEADLINE contract requires
			 * runtime <= deadline <= period. A local deadline
			 * above the period would let the splitter assign
			 * an unbounded budget; clamp explicitly so the
			 * downstream sched_setattr() validation cannot
			 * see an invalid input. */
			n->local_deadline = g->period;
		}
	}
	return true;
}

int dag_recalculate(dag_t *g)
{
	// register initial time
	struct timespec start_time;
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	int ret;
	bool analysis_cached;

	if (!g) {
		errno = EINVAL;
		return -1;
	}

	if (spa_list_is_empty(&g->nodes)) {
		dag_invalidate_analysis(g);
		g->dirty = false;
		return 0;
	}

	dag_invalidate_schedule(g);

	/* Analysis (indexed_nodes, successors, unrelated sets,
	 * fictitious endpoints) survives across recalcs because
	 * dag_invalidate_analysis is only called on TOPOLOGY changes
	 * (add/remove node, add/remove edge), not on WCET changes.
	 * If the cache is still populated, the topology is the same
	 * as last successful recalc -- we can skip the heavy build
	 * step (most expensively, dag_comp_unrelated) and just
	 * recompute the WCET-dependent state (longest paths) before
	 * re-running the deadline split and CPU placement. This is
	 * the main optimisation behind the persistent-DAG worker
	 * loop's steady-state cost.
	 */
	analysis_cached = (g->indexed_nodes != NULL);

	if (!analysis_cached) {
		dag_node_t **sources, **sinks;
		uint32_t nsources, nsinks;
		find_sources_and_sinks(g, &sources, &nsources, &sinks, &nsinks);

		if (nsources == 0 || nsinks == 0) {
			free(sources);
			free(sinks);
			dag_mark_dirty(g);
			errno = EINVAL;
			return -1;
		}

		ret = dag_build_analysis(g, sources, nsources, sinks, nsinks);
		free(sources);
		free(sinks);
		if (ret != 0) {
			dag_mark_dirty(g);
			return ret > 0 ? dag_recalculate(g) : -1;
		}
	} else {
		/* Refresh longest paths only -- they depend on WCETs
		 * and may have shifted even when topology is stable. */
		dag_populate_longest_paths(g);
	}

	/* Feasibility gate. A graph whose critical path alone exceeds
	 * the global deadline, or whose minimum per-node deadline
	 * reservation does, will fail with EAGAIN before any
	 * assignment runs. The DAG stays dirty so the next caller-side
	 * recalc retries. */
	if (dag_check_feasibility(g) < 0) {
		dag_mark_dirty(g);
		return -1;
	}

	if (assign_deadlines_iterative(g) < 0) {
		dag_mark_dirty(g);
		return -1;
	}

	/* Forward topological pass: now that every real node carries
	 * its splitter-assigned per-node slice, populate the explicit
	 * graph-relative milestone (cumulative_deadline) and the
	 * kernel-relative deadline (local_deadline). The two fields
	 * are derived purely from the splitter's output and the DAG
	 * structure; the legacy `deadline` member is left untouched
	 * for the in-flight callers that have not yet migrated. */
	dag_assign_cumulative_deadlines(g);

	/* Convert the freshly-populated cumulative deadlines to
	 * kernel-API relative deadlines. Monotonicity and
	 * 0 < local <= period are validated here; failure marks the
	 * DAG dirty so a caller-side retry can reassign. */
	if (!dag_compute_local_deadlines(g)) {
		dag_mark_dirty(g);
		errno = EINVAL;
		return -1;
	}

	if (assign_cpus(g) < 0) {
		dag_mark_dirty(g);
		return -1;
	}

	struct timespec end_time;
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	double duration = (end_time.tv_sec - start_time.tv_sec) +
		(end_time.tv_nsec - start_time.tv_nsec) / 1e9;
	pw_log_debug("DAG recalculation completed in %.6f seconds", duration);

	/* Success: the cached schedule is now clean. */
	g->dirty = false;
	return 0;
}

int dag_foreach_node(dag_t *g, dag_node_callback_t cb, void *data)
{
	if (!g || !cb) {
		errno = EINVAL;
		return -1;
	}

	/* Single source of truth: if the DAG was mutated since the
	 * last successful recalc, the dirty bit is set and we run a
	 * fresh pass before emitting any callbacks. A clean DAG short-
	 * circuits without any scan: this is what makes the persistent
	 * DAG path efficient across no-op wakes. */
	if (g->dirty) {
		if (dag_recalculate(g) < 0)
			return -1;
	}

	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		if (n->fictitious)
			continue;
		cb(data, n->id, n->tid, n->wcet,
		   n->cumulative_deadline, n->local_deadline,
		   g->period, n->cpu);
	}

	return 0;
}

void dag_node_dump_unrelated(dag_t *g) {
  char buf[1024];
  size_t off = 0;
  for (uint32_t i = 0; i < g->unrelated_size; i++) {
    int wrote = snprintf(buf + off, sizeof(buf) - off, "{ ");
    if (wrote < 0 || (size_t)wrote >= sizeof(buf) - off) break;
    off += (size_t)wrote;
    int k = 0;
    for (uint32_t j = 0; j < g->indexed_count; j++) {
      if (!bitset_test(g->unrelated[i], j))
        continue;
      wrote = snprintf(buf + off, sizeof(buf) - off,
              "%s%u", k++ > 0 ? ", " : "", j);
      if (wrote < 0 || (size_t)wrote >= sizeof(buf) - off) break;
      off += (size_t)wrote;
    }
    wrote = snprintf(buf + off, sizeof(buf) - off, " }%s",
            i < g->unrelated_size - 1 ? ", " : "");
    if (wrote < 0 || (size_t)wrote >= sizeof(buf) - off) break;
    off += (size_t)wrote;
  }
  pw_log_debug("Unrelated sets: %s", buf);
}

void dag_print(dag_t *g)
{
	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		pw_log_debug("Node %u|%u|%u with wcet %lu and deadline %lu on cpu %u", n->id, n->index, n->tid, n->wcet, n->deadline, n->cpu);
		dag_edge_t *e;
		spa_list_for_each(e, &n->outgoing, src_link)
			pw_log_debug("  Edge to node %u|%u", e->dst->id, e->dst->tid);
		pw_log_debug("  Relatives:");
		for (uint32_t i = 0; i < g->indexed_count; i++) {
			if (dag_nodes_are_related(n, g->indexed_nodes[i]))
				pw_log_debug("    Node %u|%u|%u", g->indexed_nodes[i]->id, g->indexed_nodes[i]->index, g->indexed_nodes[i]->tid);
		}
	}
	pw_log_debug("Unrelated sets:");
	dag_node_dump_unrelated(g);
}
