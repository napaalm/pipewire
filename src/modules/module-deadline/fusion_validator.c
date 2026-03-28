/*
 * fusion_validator.c
 *
 * Implementation of the structural eligibility predicate for
 * fusion candidate groups declared in fusion_validator.h.
 *
 * Pure data: no PipeWire runtime symbols, no syscalls. Tests
 * drive the predicate with synthetic inputs; production wiring in
 * reconcile.c plugs the validator between the cost model's
 * candidate generator and the contracted-DAG builder.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "fusion_validator.h"

bool fusion_validator_accept(const struct fusion_candidate_member *members,
		uint32_t n_members,
		enum fusion_reject_reason *out_reason)
{
	enum fusion_reject_reason ignore = FUSION_REJ_NONE;
	uint32_t i;
	uint32_t driver_id_seen = 0;
	bool driver_id_pinned = false;

	if (out_reason == NULL)
		out_reason = &ignore;
	*out_reason = FUSION_REJ_NONE;

	if (n_members == 0 || members == NULL)
		return true;

	for (i = 0; i < n_members; i++) {
		const struct fusion_candidate_member *m = &members[i];

		/* Same driver DAG: every member must belong to the same
		 * driver. The cost model never proposes a cross-driver
		 * group on the supported code paths, but a future
		 * generator might; rejecting here is defense in depth. */
		if (!driver_id_pinned) {
			driver_id_seen = m->driver_id;
			driver_id_pinned = true;
		} else if (m->driver_id != driver_id_seen) {
			*out_reason = FUSION_REJ_CROSS_DRIVER;
			return false;
		}

		/* Main-loop nodes do their work on the context's main
		 * loop, not on a data loop the daemon can put under
		 * SCHED_DEADLINE; they cannot participate in a fused
		 * data-loop reservation. */
		if (m->flags & FUSION_MEMBER_MAIN_LOOP) {
			*out_reason = FUSION_REJ_MAIN_LOOP;
			return false;
		}

		/* Exported nodes are owned by an out-of-process client
		 * the daemon does not control; the implementation cycle
		 * has no external-reservation model so they are
		 * categorically rejected. */
		if (m->flags & FUSION_MEMBER_EXPORTED) {
			*out_reason = FUSION_REJ_EXPORTED;
			return false;
		}

		/* Remote nodes are allowed iff their processing TID has
		 * been published; otherwise the daemon has no thread to
		 * configure. The TID==-1 sentinel matches what
		 * PW_KEY_NODE_LOOP_TID returns when the property is
		 * absent. */
		if ((m->flags & FUSION_MEMBER_REMOTE) && m->tid <= 0) {
			*out_reason = FUSION_REJ_REMOTE_TID_UNKNOWN;
			return false;
		}
	}

	return true;
}

static bool id_in_set(const uint32_t *set, uint32_t n, uint32_t id)
{
	uint32_t i;
	for (i = 0; i < n; i++) {
		if (set[i] == id)
			return true;
	}
	return false;
}

bool fusion_validator_predecessor_closure_accept(
		const uint32_t *member_ids, uint32_t n_members,
		const struct fusion_edge_input *edges, uint32_t n_edges,
		const uint32_t *source_ids, uint32_t n_sources,
		enum fusion_reject_reason *out_reason)
{
	enum fusion_reject_reason ignore = FUSION_REJ_NONE;
	uint32_t i;

	if (out_reason == NULL)
		out_reason = &ignore;
	*out_reason = FUSION_REJ_NONE;

	if (n_members == 0)
		return true;
	if (member_ids == NULL)
		return false;

	/* For every in-period edge that ends inside F, check whether
	 * the source is also in F or is a graph source. The first
	 * external non-source predecessor we hit is the rejection
	 * cause -- the group would have to wait for that node mid-job. */
	for (i = 0; i < n_edges; i++) {
		const struct fusion_edge_input *e = &edges[i];
		bool dst_in_f = id_in_set(member_ids, n_members, e->dst_id);
		bool src_in_f;

		if (!dst_in_f)
			continue;

		src_in_f = id_in_set(member_ids, n_members, e->src_id);
		if (src_in_f)
			continue; /* internal edge -- no waiting */

		/* External predecessor: must be a DAG source, otherwise
		 * the group would have to wait for it. */
		if (!id_in_set(source_ids, n_sources, e->src_id)) {
			*out_reason = FUSION_REJ_WOULD_SELF_SUSPEND;
			return false;
		}
	}

	return true;
}

/* Collect every successor of `node_id` from the edge list into
 * `out`; returns the count appended. The caller passes a buffer
 * sized to n_edges so the worst case (every edge fans out from
 * node_id) is covered. */
static uint32_t collect_successors(const struct fusion_edge_input *edges,
		uint32_t n_edges, uint32_t node_id, uint32_t *out)
{
	uint32_t i, n = 0;
	for (i = 0; i < n_edges; i++) {
		if (edges[i].src_id == node_id)
			out[n++] = edges[i].dst_id;
	}
	return n;
}

bool fusion_validator_precedence_convex_accept(
		const uint32_t *member_ids, uint32_t n_members,
		const struct fusion_edge_input *edges, uint32_t n_edges,
		enum fusion_reject_reason *out_reason)
{
	enum fusion_reject_reason ignore = FUSION_REJ_NONE;
	uint32_t i;
	uint32_t *stack = NULL;
	uint32_t *succs = NULL;
	uint32_t *visited = NULL;
	uint32_t visited_n;

	if (out_reason == NULL)
		out_reason = &ignore;
	*out_reason = FUSION_REJ_NONE;

	if (n_members <= 1)
		return true;
	if (member_ids == NULL)
		return false;

	/* Worst-case sizes: stack holds at most n_edges entries (every
	 * edge's dst could be pushed once); succs is sized for the
	 * fan-out of a single node. visited tracks already-explored
	 * external nodes to keep the walk linear. */
	if (n_edges == 0)
		return true;

	/* Buffers sized to (n_edges + n_members): the worst-case
	 * number of distinct destinations across the DAG is bounded
	 * by the number of edges plus the number of members (every
	 * edge endpoint plus the group's own ids). succs is sized
	 * the same way (single-node fan-out cannot exceed n_edges).
	 * The cost is negligible for the small graphs the audio
	 * scheduler produces. */
	size_t cap = (size_t)n_edges + (size_t)n_members;
	stack = malloc(cap * sizeof(*stack));
	succs = malloc(cap * sizeof(*succs));
	visited = malloc(cap * sizeof(*visited));
	if (stack == NULL || succs == NULL || visited == NULL) {
		free(stack); free(succs); free(visited);
		return false;
	}

	bool accept = true;
	for (i = 0; i < n_members && accept; i++) {
		uint32_t u = member_ids[i];
		uint32_t stack_n = 0;
		uint32_t s_n, s;

		visited_n = 0;
		/* Seed the walk with u's direct successors that are NOT
		 * in F. Edges to F members are internal -- they cannot
		 * "leave" the group, so they do not start a non-convex
		 * round trip. */
		s_n = collect_successors(edges, n_edges, u, succs);
		for (s = 0; s < s_n; s++) {
			if (id_in_set(member_ids, n_members, succs[s]))
				continue;
			if (id_in_set(visited, visited_n, succs[s]))
				continue;
			visited[visited_n++] = succs[s];
			stack[stack_n++] = succs[s];
		}

		while (stack_n > 0) {
			uint32_t node = stack[--stack_n];
			uint32_t cs_n, cs;

			/* Re-entering F from outside is the rejection
			 * signature. */
			if (id_in_set(member_ids, n_members, node)) {
				accept = false;
				*out_reason = FUSION_REJ_NON_CONVEX;
				break;
			}

			cs_n = collect_successors(edges, n_edges, node, succs);
			for (cs = 0; cs < cs_n; cs++) {
				if (id_in_set(visited, visited_n, succs[cs]))
					continue;
				visited[visited_n++] = succs[cs];
				stack[stack_n++] = succs[cs];
			}
		}
	}

	free(stack);
	free(succs);
	free(visited);
	return accept;
}

bool fusion_validator_externally_atomic_accept(
		const uint32_t *member_ids, uint32_t n_members,
		const struct fusion_edge_input *edges, uint32_t n_edges,
		enum fusion_reject_reason *out_reason)
{
	enum fusion_reject_reason ignore = FUSION_REJ_NONE;
	uint32_t i, e;

	if (out_reason == NULL)
		out_reason = &ignore;
	*out_reason = FUSION_REJ_NONE;

	if (n_members <= 1)
		return true;
	if (member_ids == NULL)
		return false;

	for (i = 0; i < n_members; i++) {
		uint32_t m = member_ids[i];
		bool is_terminal = true;
		bool has_external_out = false;

		for (e = 0; e < n_edges; e++) {
			if (edges[e].src_id != m)
				continue;
			if (id_in_set(member_ids, n_members,
					edges[e].dst_id))
				is_terminal = false;
			else
				has_external_out = true;
		}

		if (!is_terminal && has_external_out) {
			*out_reason = FUSION_REJ_INTERNAL_MILESTONE;
			return false;
		}
	}

	return true;
}

bool fusion_validator_blocking_closure_accept(
		const uint32_t *member_caps, uint32_t n_members,
		enum fusion_reject_reason *out_reason)
{
	enum fusion_reject_reason ignore = FUSION_REJ_NONE;
	uint32_t i;

	if (out_reason == NULL)
		out_reason = &ignore;
	*out_reason = FUSION_REJ_NONE;

	if (n_members == 0)
		return true;
	if (member_caps == NULL) {
		*out_reason = FUSION_REJ_BLOCKING_RISK;
		return false;
	}

	for (i = 0; i < n_members; i++) {
		if ((member_caps[i] & FUSION_CAP_NONBLOCKING_PROCESS) == 0) {
			*out_reason = FUSION_REJ_BLOCKING_RISK;
			return false;
		}
	}
	return true;
}

const char *fusion_reject_reason_name(enum fusion_reject_reason r)
{
	switch (r) {
	case FUSION_REJ_NONE:               return "none";
	case FUSION_REJ_CROSS_DRIVER:       return "cross_driver";
	case FUSION_REJ_MAIN_LOOP:          return "main_loop";
	case FUSION_REJ_EXPORTED:           return "exported";
	case FUSION_REJ_REMOTE_TID_UNKNOWN: return "remote_tid_unknown";
	case FUSION_REJ_WOULD_SELF_SUSPEND: return "would_self_suspend";
	case FUSION_REJ_NON_CONVEX:         return "non_convex";
	case FUSION_REJ_INTERNAL_MILESTONE: return "internal_milestone";
	case FUSION_REJ_BLOCKING_RISK:      return "blocking_risk";
	}
	return "unknown";
}
