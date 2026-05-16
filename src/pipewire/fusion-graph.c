/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * fusion-graph: BFS partitioning + critical-path DP + Sarkar 1989
 * §5.3 internalisation criterion, on top of a topology-shaped input
 * API decoupled from pw_impl_node.
 *
 * See fusion-graph.h for the API contract and the rationale for
 * factoring this out of context.c. See fusion-cost.h for citations
 * to the underlying literature.
 */

#include "fusion-graph.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct fusion_node {
	uint32_t id;
	uint64_t wcet_ns;
	uint32_t samples;
	/* Caller-supplied hint: the decision that was last applied to
	 * this node (or its component) in a previous evaluate() pass.
	 * Used by the cost model for hysteresis. SPLIT means "no
	 * prior history", which disables hysteresis. */
	enum pw_fusion_decision prev_decision;

	/* Filled by evaluate(). */
	int32_t  comp_root;          /* dense index of the canonical
	                              * (lowest-id) member of the
	                              * component */
	uint64_t longest_len_ns;     /* longest weighted path ending here
	                              * (inclusive of the node's wcet)
	                              * within the component */
	uint32_t longest_hops;       /* edge count of that path */
	uint32_t indeg;              /* in-degree within the component */
};

struct fusion_edge {
	uint32_t src;
	uint32_t dst;
};

struct pw_fusion_graph {
	struct fusion_node *nodes;
	uint32_t            n_nodes;
	uint32_t            cap_nodes;

	struct fusion_edge *edges;
	uint32_t            n_edges;
	uint32_t            cap_edges;
};

#define INITIAL_NODES_CAP	16u
#define INITIAL_EDGES_CAP	32u

struct pw_fusion_graph *pw_fusion_graph_alloc(uint32_t n_nodes_hint,
		uint32_t n_edges_hint)
{
	struct pw_fusion_graph *g = calloc(1, sizeof(*g));
	if (g == NULL)
		return NULL;

	g->cap_nodes = n_nodes_hint > 0 ? n_nodes_hint : INITIAL_NODES_CAP;
	g->cap_edges = n_edges_hint > 0 ? n_edges_hint : INITIAL_EDGES_CAP;
	g->nodes = calloc(g->cap_nodes, sizeof(*g->nodes));
	g->edges = calloc(g->cap_edges, sizeof(*g->edges));
	if (g->nodes == NULL || g->edges == NULL) {
		pw_fusion_graph_free(g);
		return NULL;
	}
	return g;
}

void pw_fusion_graph_free(struct pw_fusion_graph *g)
{
	if (g == NULL)
		return;
	free(g->nodes);
	free(g->edges);
	free(g);
}

void pw_fusion_graph_reset(struct pw_fusion_graph *g)
{
	if (g == NULL)
		return;
	g->n_nodes = 0;
	g->n_edges = 0;
}

uint32_t pw_fusion_graph_n_nodes(const struct pw_fusion_graph *g)
{
	return g ? g->n_nodes : 0;
}

uint32_t pw_fusion_graph_n_edges(const struct pw_fusion_graph *g)
{
	return g ? g->n_edges : 0;
}

int pw_fusion_graph_add_node(struct pw_fusion_graph *g, uint32_t id,
		uint64_t wcet_ns, uint32_t samples)
{
	uint32_t i;

	if (g == NULL)
		return -EINVAL;

	/* Linear de-dup -- realistic graphs have a few dozen nodes
	 * (audio is typically O(10), worst case <100), and a linear
	 * scan beats the constant overhead of any hash map at that
	 * scale. */
	for (i = 0; i < g->n_nodes; i++) {
		if (g->nodes[i].id == id)
			return -EEXIST;
	}

	if (g->n_nodes == g->cap_nodes) {
		uint32_t new_cap = g->cap_nodes ? g->cap_nodes * 2 :
			INITIAL_NODES_CAP;
		struct fusion_node *r = realloc(g->nodes,
				new_cap * sizeof(*r));
		if (r == NULL)
			return -ENOMEM;
		g->nodes = r;
		memset(g->nodes + g->cap_nodes, 0,
				(new_cap - g->cap_nodes) * sizeof(*r));
		g->cap_nodes = new_cap;
	}

	g->nodes[g->n_nodes].id = id;
	g->nodes[g->n_nodes].wcet_ns = wcet_ns;
	g->nodes[g->n_nodes].samples = samples;
	g->nodes[g->n_nodes].prev_decision = PW_FUSION_DECISION_SPLIT;
	g->nodes[g->n_nodes].comp_root = -1;
	g->n_nodes++;
	return (int)(g->n_nodes - 1);
}

int pw_fusion_graph_set_prev_decision(struct pw_fusion_graph *g,
		uint32_t node_idx, enum pw_fusion_decision prev)
{
	if (g == NULL || node_idx >= g->n_nodes)
		return -EINVAL;
	g->nodes[node_idx].prev_decision = prev;
	return 0;
}

int pw_fusion_graph_add_edge(struct pw_fusion_graph *g,
		uint32_t src_idx, uint32_t dst_idx)
{
	if (g == NULL)
		return -EINVAL;
	if (src_idx >= g->n_nodes || dst_idx >= g->n_nodes)
		return -EINVAL;
	if (src_idx == dst_idx)
		return -EINVAL; /* self-loop would create a cycle */

	if (g->n_edges == g->cap_edges) {
		uint32_t new_cap = g->cap_edges ? g->cap_edges * 2 :
			INITIAL_EDGES_CAP;
		struct fusion_edge *r = realloc(g->edges,
				new_cap * sizeof(*r));
		if (r == NULL)
			return -ENOMEM;
		g->edges = r;
		g->cap_edges = new_cap;
	}

	g->edges[g->n_edges].src = src_idx;
	g->edges[g->n_edges].dst = dst_idx;
	g->n_edges++;
	return 0;
}

/* BFS over undirected eligible edges to partition into weakly
 * connected components. Each entry's comp_root ends up pointing at
 * the dense index of the canonical (lowest-id) member of its
 * component, exactly as in the equivalent pass inside context.c. */
static int partition_components(struct pw_fusion_graph *g)
{
	int32_t *queue;
	uint32_t i, k;

	for (i = 0; i < g->n_nodes; i++)
		g->nodes[i].comp_root = -1;

	queue = calloc(g->n_nodes, sizeof(*queue));
	if (queue == NULL)
		return -ENOMEM;

	for (i = 0; i < g->n_nodes; i++) {
		uint32_t head = 0, tail = 0;
		uint32_t min_idx = i;

		if (g->nodes[i].comp_root >= 0)
			continue;

		g->nodes[i].comp_root = (int32_t)i;
		queue[tail++] = (int32_t)i;

		while (head < tail) {
			int32_t cur = queue[head++];

			if ((uint32_t)cur < min_idx ||
					g->nodes[cur].id < g->nodes[min_idx].id)
				min_idx = (uint32_t)cur;

			for (k = 0; k < g->n_edges; k++) {
				int32_t peer = -1;
				if (g->edges[k].src == (uint32_t)cur)
					peer = (int32_t)g->edges[k].dst;
				else if (g->edges[k].dst == (uint32_t)cur)
					peer = (int32_t)g->edges[k].src;
				else
					continue;
				if (g->nodes[peer].comp_root >= 0)
					continue;
				g->nodes[peer].comp_root = (int32_t)i;
				queue[tail++] = peer;
			}
		}

		/* Canonicalise the component's root to the lowest-id
		 * member so the group name stays stable across calls
		 * regardless of insertion order. */
		if (min_idx != i) {
			uint32_t j;
			for (j = 0; j < g->n_nodes; j++) {
				if (g->nodes[j].comp_root == (int32_t)i)
					g->nodes[j].comp_root = (int32_t)min_idx;
			}
		}
	}

	free(queue);
	return 0;
}

/* Kahn topological sort + DP on a single component identified by its
 * canonical root. Updates longest_len_ns and longest_hops on each
 * member; reports the per-component maxima via out_cp / out_hops. */
static int component_cp(struct pw_fusion_graph *g, int32_t root,
		uint64_t *out_cp, uint32_t *out_hops, uint32_t *out_n)
{
	int32_t *q;
	uint32_t qh = 0, qt = 0;
	uint32_t i, k, members = 0, processed = 0;
	int rc = 0;

	*out_cp = 0;
	*out_hops = 0;
	*out_n = 0;

	q = calloc(g->n_nodes, sizeof(*q));
	if (q == NULL)
		return -ENOMEM;

	/* Pass 1: initialise per-member DP state and count in-degrees
	 * restricted to in-component edges. */
	for (i = 0; i < g->n_nodes; i++) {
		if (g->nodes[i].comp_root != root)
			continue;
		g->nodes[i].longest_len_ns = g->nodes[i].wcet_ns;
		g->nodes[i].longest_hops = 0;
		g->nodes[i].indeg = 0;
		members++;
	}
	for (k = 0; k < g->n_edges; k++) {
		uint32_t s = g->edges[k].src, d = g->edges[k].dst;
		if (g->nodes[s].comp_root != root)
			continue;
		if (g->nodes[d].comp_root != root)
			continue;
		g->nodes[d].indeg++;
	}

	/* Pass 2: Kahn. Enqueue zero-indegree members, relax outbound
	 * edges. */
	for (i = 0; i < g->n_nodes; i++) {
		if (g->nodes[i].comp_root != root)
			continue;
		if (g->nodes[i].indeg == 0)
			q[qt++] = (int32_t)i;
	}

	while (qh < qt) {
		int32_t cur = q[qh++];
		processed++;

		if (g->nodes[cur].longest_len_ns > *out_cp) {
			*out_cp = g->nodes[cur].longest_len_ns;
			*out_hops = g->nodes[cur].longest_hops;
		}

		for (k = 0; k < g->n_edges; k++) {
			if (g->edges[k].src != (uint32_t)cur)
				continue;
			uint32_t d = g->edges[k].dst;
			if (g->nodes[d].comp_root != root)
				continue;

			uint64_t cand = g->nodes[cur].longest_len_ns +
				g->nodes[d].wcet_ns;
			uint32_t hops = g->nodes[cur].longest_hops + 1;
			if (cand > g->nodes[d].longest_len_ns) {
				g->nodes[d].longest_len_ns = cand;
				g->nodes[d].longest_hops = hops;
			}
			if (g->nodes[d].indeg > 0 &&
					--g->nodes[d].indeg == 0)
				q[qt++] = (int32_t)d;
		}
	}

	if (processed != members)
		rc = -ELOOP;

	*out_n = members;
	free(q);
	return rc;
}

int pw_fusion_graph_evaluate(struct pw_fusion_graph *g,
		const struct pw_fusion_params *params,
		struct pw_fusion_graph_decision *out)
{
	struct pw_fusion_graph_decision *per_comp;
	int rc;
	uint32_t i;

	if (g == NULL || params == NULL || out == NULL)
		return -EINVAL;
	if (g->n_nodes == 0)
		return 0;

	rc = partition_components(g);
	if (rc < 0)
		return rc;

	per_comp = calloc(g->n_nodes, sizeof(*per_comp));
	if (per_comp == NULL)
		return -ENOMEM;

	/* Aggregate per-component sum_wcet / min_samples / min_id. */
	for (i = 0; i < g->n_nodes; i++) {
		uint32_t root = (uint32_t)g->nodes[i].comp_root;
		struct pw_fusion_graph_decision *c = &per_comp[root];

		if (c->component_n_nodes == 0) {
			c->component_min_samples = g->nodes[i].samples;
			c->component_leader_id = g->nodes[i].id;
		} else {
			if (g->nodes[i].samples < c->component_min_samples)
				c->component_min_samples = g->nodes[i].samples;
			if (g->nodes[i].id < c->component_leader_id)
				c->component_leader_id = g->nodes[i].id;
		}
		c->component_n_nodes++;
		c->component_sum_wcet_ns += g->nodes[i].wcet_ns;
	}

	/* Aggregate the previous decision per component for the
	 * hysteresis path. Take the strongest hint any member carries
	 * (FUSE > LINEAR_ONLY > SPLIT): the previous scan applied one
	 * decision per component, so all members agree; if the
	 * component grew this scan and some members joined freshly
	 * (their prev_decision is SPLIT), the max keeps the previous
	 * decision sticky for the established core. */
	for (i = 0; i < g->n_nodes; i++) {
		uint32_t root = (uint32_t)g->nodes[i].comp_root;
		if (g->nodes[i].prev_decision > per_comp[root].prev_decision)
			per_comp[root].prev_decision = g->nodes[i].prev_decision;
	}

	/* Run the CP DP once per component (iteration is over roots). */
	for (i = 0; i < g->n_nodes; i++) {
		uint64_t cp = 0;
		uint32_t hops = 0, n = 0;

		if (per_comp[i].component_n_nodes == 0)
			continue;
		rc = component_cp(g, (int32_t)i, &cp, &hops, &n);
		if (rc < 0) {
			/* Cycle detected -- mark every member as
			 * LINEAR_ONLY and continue with the rest of the
			 * graph. */
			per_comp[i].component_cp_wcet_ns = 0;
			per_comp[i].component_cp_hops = 0;
			per_comp[i].decision = PW_FUSION_DECISION_LINEAR_ONLY;
			continue;
		}
		per_comp[i].component_cp_wcet_ns = cp;
		per_comp[i].component_cp_hops = hops;
	}

	/* Apply Sarkar's criterion per component. */
	for (i = 0; i < g->n_nodes; i++) {
		struct pw_fusion_component c;
		if (per_comp[i].component_n_nodes == 0)
			continue;
		if (per_comp[i].decision == PW_FUSION_DECISION_LINEAR_ONLY)
			continue; /* already decided by cycle path */
		c.n_nodes = per_comp[i].component_n_nodes;
		c.sum_wcet_ns = per_comp[i].component_sum_wcet_ns;
		c.cp_wcet_ns = per_comp[i].component_cp_wcet_ns;
		c.cp_hops = per_comp[i].component_cp_hops;
		c.min_samples_seen = per_comp[i].component_min_samples;
		c.prev_decision = per_comp[i].prev_decision;
		per_comp[i].decision = pw_fusion_decide(&c, params);
	}

	/* Project per-component to per-node. */
	for (i = 0; i < g->n_nodes; i++) {
		uint32_t root = (uint32_t)g->nodes[i].comp_root;
		out[i] = per_comp[root];
	}

	free(per_comp);
	return 0;
}
