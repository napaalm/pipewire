/*
 * contracted.c
 *
 * Implementation of the contracted-DAG types declared in
 * contracted.h. Pure data plumbing: no PipeWire runtime symbols,
 * no syscalls. Tests drive the builder with synthetic graphs and
 * verify shape; the analysis layer consumes the result.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "contracted.h"
#include "dag.h"

contracted_dag_t *contracted_dag_create(uint64_t period_ns, uint64_t deadline_ns)
{
	contracted_dag_t *cg = calloc(1, sizeof(*cg));
	if (cg == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	spa_list_init(&cg->nodes);
	spa_list_init(&cg->edges);
	cg->period_ns = period_ns;
	cg->deadline_ns = deadline_ns;
	return cg;
}

void contracted_dag_destroy(contracted_dag_t *cg)
{
	contracted_node_t *cn, *tmp_n;
	contracted_edge_t *ce, *tmp_e;
	struct contracted_member *m, *tmp_m;
	struct contracted_edge_meta *em, *tmp_em;

	if (cg == NULL)
		return;

	spa_list_for_each_safe(ce, tmp_e, &cg->edges, link) {
		spa_list_remove(&ce->link);
		/* src_link / dst_link are also being torn down here;
		 * removing them keeps spa_list invariants for the
		 * node-side iteration below, even though the nodes
		 * themselves are freed next. */
		spa_list_remove(&ce->src_link);
		spa_list_remove(&ce->dst_link);
		spa_list_for_each_safe(em, tmp_em, &ce->originals, link) {
			spa_list_remove(&em->link);
			free(em);
		}
		free(ce);
	}
	spa_list_for_each_safe(cn, tmp_n, &cg->nodes, link) {
		spa_list_for_each_safe(m, tmp_m, &cn->members, link) {
			spa_list_remove(&m->link);
			free(m);
		}
		spa_list_remove(&cn->link);
		free(cn);
	}
	free(cg);
}

contracted_node_t *contracted_dag_add_node(contracted_dag_t *cg)
{
	contracted_node_t *cn;

	if (cg == NULL) {
		errno = EINVAL;
		return NULL;
	}
	cn = calloc(1, sizeof(*cn));
	if (cn == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	cn->id = cg->n_nodes++;
	cn->cpu = -1;
	spa_list_init(&cn->members);
	spa_list_init(&cn->preds);
	spa_list_init(&cn->succs);
	spa_list_append(&cg->nodes, &cn->link);
	return cn;
}

int contracted_node_add_member(contracted_node_t *cn, uint32_t id,
		pid_t tid, uint64_t wcet_ns)
{
	struct contracted_member *m;

	if (cn == NULL)
		return -EINVAL;
	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return -ENOMEM;
	m->id = id;
	m->tid = tid;
	m->wcet_ns = wcet_ns;
	spa_list_append(&cn->members, &m->link);
	cn->n_members++;
	cn->is_fusion_group = cn->n_members > 1;
	return 0;
}

int contracted_node_set_overhead(contracted_node_t *cn,
		const struct contracted_overhead_components *components)
{
	if (cn == NULL || components == NULL)
		return -EINVAL;

	cn->overhead = *components;
	/* Aggregation rule: every measured cost lifts the budget;
	 * wakeup_savings_ns is observational only and does NOT enter
	 * overhead_ns. The analysis layer can only safely shrink the
	 * budget when a future model explicitly proves the saving
	 * applies on every activation, which is out of scope for the
	 * fusion-as-contraction step. */
	cn->overhead_ns = components->group_dispatch_ns
			+ components->internal_topo_ns
			+ components->activation_pending_ns
			+ components->buffer_port_iter_ns;
	return 0;
}

uint64_t contracted_node_effective_wcet(const contracted_node_t *cn)
{
	if (cn == NULL)
		return 0;
	return cn->wcet_ns + cn->overhead_ns;
}

int contracted_node_observe_macro_runtime(contracted_node_t *cn,
		uint64_t observed_macro_runtime_ns)
{
	uint64_t residual;

	if (cn == NULL)
		return -EINVAL;
	if (observed_macro_runtime_ns == 0)
		return 0;

	residual = observed_macro_runtime_ns > cn->wcet_ns
			? observed_macro_runtime_ns - cn->wcet_ns
			: 0;
	if (residual > cn->overhead.internal_topo_ns) {
		cn->overhead.internal_topo_ns = residual;
		cn->overhead_ns = cn->overhead.group_dispatch_ns
				+ cn->overhead.internal_topo_ns
				+ cn->overhead.activation_pending_ns
				+ cn->overhead.buffer_port_iter_ns;
	}
	return 0;
}

int contracted_node_observe_macro_completion(contracted_node_t *cn,
		uint64_t timestamp_ns)
{
	if (cn == NULL)
		return -EINVAL;
	cn->macro_completion_ns = timestamp_ns;
	cn->macro_completion_count++;
	return 0;
}

int contracted_dag_add_edge(contracted_dag_t *cg,
		contracted_node_t *src, contracted_node_t *dst)
{
	contracted_edge_t *ce;

	if (cg == NULL || src == NULL || dst == NULL)
		return -EINVAL;
	if (src == dst)
		return -EINVAL;

	/* Deduplicate parallel edges between the same two macro-nodes.
	 * Walk the source's successor list; a sub-millisecond cost
	 * for the graph sizes the audio path produces. */
	spa_list_for_each(ce, &src->succs, src_link) {
		if (ce->dst == dst)
			return 0;
	}

	ce = calloc(1, sizeof(*ce));
	if (ce == NULL)
		return -ENOMEM;
	ce->src = src;
	ce->dst = dst;
	spa_list_init(&ce->originals);
	spa_list_append(&cg->edges, &ce->link);
	spa_list_append(&src->succs, &ce->src_link);
	spa_list_append(&dst->preds, &ce->dst_link);
	cg->n_edges++;
	return 0;
}

int contracted_edge_add_meta(contracted_edge_t *ce,
		uint32_t edge_id, uint32_t src_port, uint32_t dst_port,
		uint32_t flags)
{
	struct contracted_edge_meta *em;

	if (ce == NULL)
		return -EINVAL;
	em = calloc(1, sizeof(*em));
	if (em == NULL)
		return -ENOMEM;
	em->edge_id = edge_id;
	em->src_port = src_port;
	em->dst_port = dst_port;
	em->flags = flags;
	spa_list_append(&ce->originals, &em->link);
	ce->n_originals++;
	ce->flags_union |= flags;
	return 0;
}

contracted_edge_t *contracted_dag_find_edge(contracted_dag_t *cg,
		contracted_node_t *src, contracted_node_t *dst)
{
	contracted_edge_t *ce;

	if (cg == NULL || src == NULL || dst == NULL)
		return NULL;
	spa_list_for_each(ce, &src->succs, src_link) {
		if (ce->dst == dst)
			return ce;
	}
	return NULL;
}

/* DFS-based cycle detector. Two colour bits per node, recorded in
 * a side array since contracted_node has no scratch space; the
 * iteration is bounded by n_nodes so the bookkeeping cost is
 * O(V + E). */
enum dfs_colour {
	DFS_WHITE = 0,
	DFS_GREY  = 1,
	DFS_BLACK = 2,
};

static bool dfs_has_cycle(const contracted_dag_t *cg,
		const contracted_node_t *root,
		uint8_t *colour, contracted_node_t **stack,
		uint32_t stack_cap)
{
	uint32_t top = 0;
	(void)cg;

	stack[top++] = (contracted_node_t *)root;
	colour[root->id] = DFS_GREY;

	while (top > 0) {
		contracted_node_t *cn = stack[top - 1];
		bool advanced = false;
		contracted_edge_t *ce;

		spa_list_for_each(ce, &cn->succs, src_link) {
			uint8_t c = colour[ce->dst->id];
			if (c == DFS_GREY)
				return true;
			if (c == DFS_WHITE) {
				colour[ce->dst->id] = DFS_GREY;
				if (top >= stack_cap)
					return true;
				stack[top++] = ce->dst;
				advanced = true;
				break;
			}
		}
		if (!advanced) {
			colour[cn->id] = DFS_BLACK;
			top--;
		}
	}
	return false;
}

/* Resolve a member id to its 0-based owner index by linear scan.
 * The input arrays are small (a single driver's follower set
 * rarely exceeds a few dozen entries), so the n^2 cost stays in
 * the noise next to the placer's own work; if the audio graph
 * ever grows to where this matters, a sorted index can be added
 * without changing the API. */
static int find_member_idx(const struct contracted_member_input *members,
		uint32_t n_members, uint32_t id, uint32_t *out_idx)
{
	uint32_t i;
	for (i = 0; i < n_members; i++) {
		if (members[i].id == id) {
			*out_idx = i;
			return 0;
		}
	}
	return -ENOTRECOVERABLE;
}

int contracted_dag_build(uint64_t period_ns, uint64_t deadline_ns,
		const struct contracted_member_input *members,
		const uint32_t *group_id,
		uint32_t n_members,
		const struct contracted_edge_input *edges,
		uint32_t n_edges,
		contracted_dag_t **out)
{
	contracted_dag_t *cg = NULL;
	contracted_node_t **owner = NULL; /* owner[i] = macro-node containing
					   * the i-th original member */
	uint32_t i;
	int r = 0;

	if (out == NULL || (n_members > 0 && (members == NULL || group_id == NULL)))
		return -EINVAL;
	if (n_edges > 0 && edges == NULL)
		return -EINVAL;

	/* Duplicate-id guard: catches caller bugs early instead of
	 * letting them surface as a silently wrong group assignment. */
	for (i = 0; i < n_members; i++) {
		uint32_t j;
		for (j = i + 1; j < n_members; j++) {
			if (members[i].id == members[j].id)
				return -EINVAL;
		}
	}

	cg = contracted_dag_create(period_ns, deadline_ns);
	if (cg == NULL)
		return -ENOMEM;

	owner = calloc(n_members, sizeof(*owner));
	if (n_members > 0 && owner == NULL) {
		r = -ENOMEM;
		goto fail;
	}

	/* First pass: every member with group_id == 0 gets its own
	 * singleton macro-node. */
	for (i = 0; i < n_members; i++) {
		if (group_id[i] != 0)
			continue;
		owner[i] = contracted_dag_add_node(cg);
		if (owner[i] == NULL) {
			r = -ENOMEM;
			goto fail;
		}
		if ((r = contracted_node_add_member(owner[i],
				members[i].id, members[i].tid,
				members[i].wcet_ns)) < 0)
			goto fail;
		owner[i]->wcet_ns += members[i].wcet_ns;
	}

	/* Second pass: cluster members with matching non-zero
	 * group_ids. The first occurrence of each group_id mints a
	 * fresh macro-node; subsequent occurrences fold into it. */
	for (i = 0; i < n_members; i++) {
		uint32_t gid = group_id[i];
		uint32_t j;
		contracted_node_t *cn = NULL;
		if (gid == 0)
			continue;
		for (j = 0; j < i; j++) {
			if (group_id[j] == gid) {
				cn = owner[j];
				break;
			}
		}
		if (cn == NULL) {
			cn = contracted_dag_add_node(cg);
			if (cn == NULL) {
				r = -ENOMEM;
				goto fail;
			}
		}
		if ((r = contracted_node_add_member(cn,
				members[i].id, members[i].tid,
				members[i].wcet_ns)) < 0)
			goto fail;
		cn->wcet_ns += members[i].wcet_ns;
		owner[i] = cn;
	}

	/* Edge contraction. Each original edge becomes either an
	 * internal-and-dropped edge or a contracted edge between two
	 * distinct macro-nodes; add_edge dedupes parallel copies. Per-
	 * original metadata is preserved on the contracted edge so the
	 * diagnostic dumps can list every collapsed original. */
	for (i = 0; i < n_edges; i++) {
		uint32_t s_idx, d_idx;
		contracted_node_t *src, *dst;
		contracted_edge_t *ce;
		bool has_meta;
		if ((r = find_member_idx(members, n_members,
				edges[i].src_id, &s_idx)) < 0)
			goto fail;
		if ((r = find_member_idx(members, n_members,
				edges[i].dst_id, &d_idx)) < 0)
			goto fail;
		src = owner[s_idx];
		dst = owner[d_idx];
		if (src == dst)
			continue;
		if ((r = contracted_dag_add_edge(cg, src, dst)) < 0)
			goto fail;
		has_meta = edges[i].edge_id != 0 || edges[i].src_port != 0
				|| edges[i].dst_port != 0 || edges[i].flags != 0;
		if (!has_meta)
			continue;
		ce = contracted_dag_find_edge(cg, src, dst);
		if (ce == NULL) {
			/* contracted_dag_add_edge just succeeded so this
			 * cannot happen, but the defensive guard makes the
			 * error path explicit. */
			r = -ENOTRECOVERABLE;
			goto fail;
		}
		if ((r = contracted_edge_add_meta(ce, edges[i].edge_id,
				edges[i].src_port, edges[i].dst_port,
				edges[i].flags)) < 0)
			goto fail;
	}

	free(owner);
	*out = cg;
	return 0;

fail:
	free(owner);
	contracted_dag_destroy(cg);
	*out = NULL;
	return r;
}

bool contracted_dag_has_cycle(const contracted_dag_t *cg)
{
	uint8_t *colour;
	contracted_node_t **stack;
	contracted_node_t *cn;
	bool result = false;

	if (cg == NULL || cg->n_nodes == 0)
		return false;

	colour = calloc(cg->n_nodes, sizeof(*colour));
	stack = calloc(cg->n_nodes, sizeof(*stack));
	if (colour == NULL || stack == NULL) {
		free(colour);
		free(stack);
		return true;
	}

	spa_list_for_each(cn, &cg->nodes, link) {
		if (colour[cn->id] != DFS_WHITE)
			continue;
		if (dfs_has_cycle(cg, cn, colour, stack, cg->n_nodes)) {
			result = true;
			break;
		}
	}

	free(colour);
	free(stack);
	return result;
}

/* --- contracted_dag <-> dag_t bridge --- */

/* Return the leader (lowest-id) member's tid, or -1 on empty
 * macro-node. The leader convention matches sched_groups: the
 * lowest-id member acts as the cache anchor and identifies the
 * shared data-loop TID. */
static pid_t macro_leader_tid(const contracted_node_t *cn)
{
	struct contracted_member *m;
	struct contracted_member *leader = NULL;

	spa_list_for_each(m, &cn->members, link) {
		if (leader == NULL || m->id < leader->id)
			leader = m;
	}
	return leader != NULL ? leader->tid : (pid_t)-1;
}

int contracted_dag_to_dag(const contracted_dag_t *cg,
		double admission_ceiling, uint32_t num_cpus,
		const double *relative_capacity,
		struct dag **out)
{
	struct dag *g = NULL;
	contracted_node_t *cn;
	contracted_edge_t *ce;
	int r;

	if (cg == NULL || out == NULL)
		return -EINVAL;
	*out = NULL;

	g = dag_create(cg->period_ns, cg->deadline_ns, admission_ceiling,
			num_cpus, relative_capacity);
	if (g == NULL)
		return -errno != 0 ? -errno : -ENOMEM;

	/* One dag_node per macro-node. Real nodes only: the
	 * deadline-splitter's own fictitious source/sink will be
	 * added by dag_recalculate as before. */
	spa_list_for_each(cn, &cg->nodes, link) {
		uint64_t wcet = contracted_node_effective_wcet(cn);
		pid_t tid = macro_leader_tid(cn);
		r = dag_add_node(g, cn->id, wcet, tid, /*fictitious*/ false);
		if (r < 0) {
			dag_destroy(g);
			return r;
		}
	}
	spa_list_for_each(ce, &cg->edges, link) {
		r = dag_add_edge(g, ce->src->id, ce->dst->id);
		if (r < 0) {
			dag_destroy(g);
			return r;
		}
	}

	*out = g;
	return 0;
}

int contracted_dag_apply_dag_schedule(contracted_dag_t *cg,
		const struct dag *g)
{
	contracted_node_t *cn;

	if (cg == NULL || g == NULL)
		return -EINVAL;

	spa_list_for_each(cn, &cg->nodes, link) {
		/* dag_find_node casts away const because it maintains
		 * a lazy id-index; the function does not mutate the
		 * graph topology, only the caches. The const_cast is
		 * intentional and confined to this bridge. */
		dag_node_t *dn = dag_find_node((struct dag *)g, cn->id);
		if (dn == NULL)
			continue;
		cn->runtime_budget_ns = dn->wcet;
		cn->cumulative_deadline_ns = dn->cumulative_deadline;
		cn->local_deadline_ns = dn->local_deadline;
		cn->cpu = (dn->cpu == DAG_CPU_INVALID) ? -1 : (int)dn->cpu;
	}
	return 0;
}
