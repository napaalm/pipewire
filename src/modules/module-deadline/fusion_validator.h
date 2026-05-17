/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_DEADLINE_FUSION_VALIDATOR_H
#define MODULE_DEADLINE_FUSION_VALIDATOR_H

/*
 * fusion_validator.h
 *
 * Structural eligibility predicate for fusion candidate groups.
 *
 * The Sarkar 1989 chapter 5 profitability criterion answers "is
 * fusion likely to win in execution time" once a candidate group
 * is on the table; it says nothing about whether fusing the group
 * is structurally sound. Forming a single SCHED_DEADLINE
 * reservation out of members that span two driver DAGs, that
 * include a main-loop control-plane node, or that contain a node
 * the daemon cannot configure (an exported node or a remote node
 * whose processing TID is unknown) violates the kernel's
 * one-thread-one-reservation contract, and the further structural
 * predicates (precedence convexity, externally-atomic outputs,
 * blocking closure -- Sarkar 1989 §5.3, Chen et al. 2019 on
 * self-suspending tasks) decide whether the macro-node admits a
 * meaningful local deadline at all.
 *
 * The validator is pure data: it consumes a list of candidate
 * members with their analyzability bitmasks plus the candidate's
 * edge list, and returns either accept or one explicit
 * rejection reason from a closed enum. Tests can drive every
 * predicate with synthetic inputs; the production wiring in
 * reconcile.c filters candidate groups through the validator
 * before contraction, so an unsound group never reaches the
 * deadline-splitter input.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Closed enum of structural rejection reasons. NONE is the
 * sentinel for "the candidate is accepted"; every other value
 * names exactly one predicate that failed. The textual tokens
 * (lower_snake_case) double as the JSON snapshot's
 * `reason=<token>` values, so they are stable.
 *
 * Predicates land in the order Phase-style validator commits
 * introduce them:
 *
 *   CROSS_DRIVER         -- members belong to two different driver
 *                           scheduling DAGs.
 *   MAIN_LOOP            -- a member is on the main loop (control-
 *                           plane work, not a data-loop node).
 *   EXPORTED             -- a member is an exported node with no
 *                           controllable reservation.
 *   REMOTE_TID_UNKNOWN   -- a remote node's processing TID is not
 *                           known.
 *   WOULD_SELF_SUSPEND   -- the group would start before all
 *                           in-period external predecessors have
 *                           completed.
 *   NON_CONVEX           -- there is an in-period path from one
 *                           member to another that leaves the
 *                           group and re-enters it.
 *   INTERNAL_MILESTONE   -- a non-terminal internal member has an
 *                           external successor (externally-atomic
 *                           predicate failed).
 *   BLOCKING_RISK        -- a member is not declared non-blocking
 *                           in its capability mask.
 */
enum fusion_reject_reason {
	FUSION_REJ_NONE               = 0,
	FUSION_REJ_CROSS_DRIVER       = 1,
	FUSION_REJ_MAIN_LOOP          = 2,
	FUSION_REJ_EXPORTED           = 3,
	FUSION_REJ_REMOTE_TID_UNKNOWN = 4,
	FUSION_REJ_WOULD_SELF_SUSPEND = 5,
	FUSION_REJ_NON_CONVEX         = 6,
	FUSION_REJ_INTERNAL_MILESTONE = 7,
	FUSION_REJ_BLOCKING_RISK      = 8,
};

/* Per-member analyzability flags. Mirror the pw_impl_node bits the
 * scheduling layer reads at topology-snapshot time. The validator
 * uses them to short-circuit predicates that the simple "is the
 * node analyzable as a SCHED_DEADLINE task" question already
 * decides. */
enum fusion_member_flag {
	FUSION_MEMBER_MAIN_LOOP = 1u << 0,
	FUSION_MEMBER_EXPORTED  = 1u << 1,
	FUSION_MEMBER_REMOTE    = 1u << 2,
};

struct fusion_candidate_member {
	uint32_t id;
	pid_t    tid;          /* -1 if unknown */
	uint32_t driver_id;    /* the driver scheduling DAG the member belongs to */
	uint32_t flags;        /* bitmask of fusion_member_flag */
};

/*
 * Decide whether the candidate group described by members[] is
 * structurally eligible for fusion.
 *
 * Returns true (FUSION_REJ_NONE in *out_reason) iff every predicate
 * passes. Otherwise sets *out_reason to the first failing
 * predicate's reason and returns false. n_members == 0 trivially
 * accepts.
 *
 * The analyzability predicates (CROSS_DRIVER / MAIN_LOOP /
 * EXPORTED / REMOTE_TID_UNKNOWN) plus the structural ones
 * (WOULD_SELF_SUSPEND, NON_CONVEX, INTERNAL_MILESTONE,
 * BLOCKING_RISK) are all live; together they enforce the
 * fusion-soundness predicate stack the contracted-DAG
 * dispatcher then trusts.
 */
bool fusion_validator_accept(const struct fusion_candidate_member *members,
		uint32_t n_members,
		enum fusion_reject_reason *out_reason);

/*
 * Strict predecessor closure -- the conservative structural
 * fallback for the runtime release-barrier predicate.
 *
 * A fused group F must not begin executing one internal member
 * and then block waiting for another internal member's external
 * predecessor; doing so turns the macro-node into a self-suspending
 * task and breaks the EDF feasibility analysis (Chen et al.
 * 2019 §III on suspension-aware analysis). The sound long-term fix
 * is a macro-node release barrier that gates the group's wake-up
 * on every external predecessor completing -- a runtime mechanism
 * that lands later. Until then, strict predecessor closure is the
 * safe structural shortcut documented by the plan's fallback:
 *
 *   Accept F iff every in-period predecessor of every member
 *   either belongs to F itself or is an original graph source
 *   (a DAG node with no incoming in-period edges; released
 *   together with the driver activation).
 *
 * The check is more conservative than a release barrier -- it
 * rejects some fusions a barrier would accept -- but it
 * guarantees that nothing inside F ever has to wait for external
 * work mid-job, so the macro-node is non-self-suspending by
 * construction.
 *
 * Inputs:
 *   - member_ids[]: the ids of every member of the candidate
 *     group F (length n_members).
 *   - edges[]: every in-period edge of the surrounding scheduling
 *     DAG (length n_edges). The validator scans for incoming
 *     edges to every member of F.
 *   - source_ids[]: the ids of every node that is a source in
 *     the surrounding scheduling DAG (i.e. has no incoming
 *     in-period edges). length n_sources. A NULL source list
 *     with n_sources == 0 means "no sources known"; the
 *     predicate then rejects any external predecessor.
 *
 * Returns true with *out_reason = NONE on accept, false with
 * *out_reason = WOULD_SELF_SUSPEND on reject.
 *
 * n_members == 0 trivially accepts (an empty group has no
 * incoming edges to check).
 */
struct fusion_edge_input {
	uint32_t src_id;
	uint32_t dst_id;
};

bool fusion_validator_predecessor_closure_accept(
		const uint32_t *member_ids, uint32_t n_members,
		const struct fusion_edge_input *edges, uint32_t n_edges,
		const uint32_t *source_ids, uint32_t n_sources,
		enum fusion_reject_reason *out_reason);

/*
 * Precedence convexity predicate.
 *
 * A fusion group F is precedence-convex iff for every pair of
 * members u, v in F, every in-period path from u to v is fully
 * contained in F. Equivalently, no path from one member to
 * another leaves F (passes through a non-member) and re-enters
 * F. Sarkar 1989 §5.3 introduces the notion as a soundness
 * requirement for macro-actor formation: contracting a non-
 * convex group creates a cycle in the contracted DAG (the
 * non-member round trip becomes macro -> outside -> macro), and
 * the deadline-splitter cannot operate on a cyclic graph.
 *
 * Implementation: for every member u in F, walk the in-period
 * successors of u that are NOT in F, then continue walking those
 * out-of-group descendants. If the walk ever re-enters F, the
 * candidate is rejected with FUSION_REJ_NON_CONVEX. The cost is
 * O(|F| * (V + E)) which is well within the scheduling-DAG
 * sizes the audio path produces.
 *
 * Returns true with *out_reason = NONE on accept, false with
 * *out_reason = NON_CONVEX on reject. n_members <= 1 trivially
 * accepts.
 */
bool fusion_validator_precedence_convex_accept(
		const uint32_t *member_ids, uint32_t n_members,
		const struct fusion_edge_input *edges, uint32_t n_edges,
		enum fusion_reject_reason *out_reason);

/*
 * Externally atomic predicate.
 *
 * A single SCHED_DEADLINE reservation has one local deadline. If
 * a non-terminal internal member has an external successor, that
 * successor's release depends on a milestone INSIDE the group
 * that the macro-node deadline cannot represent: the external
 * peer observes the internal member's output before the macro-
 * node completes, and there is no kernel-visible deadline that
 * pins when. Without an internal milestone-aware dispatcher
 * (deferred future work), such groups are unsound.
 *
 * "Terminal internal member" = a member with no outgoing edges
 * to other F members. Terminal members may have external
 * outgoing edges (those represent the macro-node's externally
 * observable output, which the macro deadline does pin).
 *
 * The first implementation policy is the conservative one the
 * plan calls for: reject any group containing a non-terminal
 * internal member with an external outgoing edge.
 *
 * Returns true with *out_reason = NONE on accept, false with
 * *out_reason = INTERNAL_MILESTONE on reject. n_members <= 1
 * trivially accepts.
 */
bool fusion_validator_externally_atomic_accept(
		const uint32_t *member_ids, uint32_t n_members,
		const struct fusion_edge_input *edges, uint32_t n_edges,
		enum fusion_reject_reason *out_reason);

/*
 * Per-member RT capability bitmask.
 *
 * Chen et al. 2019 establishes that suspension-aware EDF
 * feasibility is subtle and error-prone, and that the safe
 * default for an unanalysed task is to refuse the analysis. The
 * blocking-closure predicate enforces that default: every fusion
 * member must declare that its process() function does not
 * suspend the calling thread. Unknown plugin nodes carry no
 * capability bits and are therefore non-fusible at hard mode by
 * construction.
 *
 * The capability bits below match the plan's pw_rt_capability
 * vocabulary; today only NONBLOCKING_PROCESS is consumed by the
 * predicate. The other bits are reserved for future, more
 * granular fall-through checks (a memory-allocation guard, a
 * main-loop-wait guard, an unbounded-lock guard) that the
 * runtime instrumentation will populate.
 */
enum fusion_member_capability {
	FUSION_CAP_NONBLOCKING_PROCESS    = 1u << 0,
	FUSION_CAP_NO_DYNAMIC_ALLOCATION  = 1u << 1,
	FUSION_CAP_NO_MAINLOOP_WAIT       = 1u << 2,
	FUSION_CAP_NO_UNBOUNDED_LOCKS     = 1u << 3,
};

/*
 * Blocking-closure predicate.
 *
 * A fused group fails hard-mode admission unless every member
 * declares FUSION_CAP_NONBLOCKING_PROCESS. The reasoning: a
 * member that may suspend inside process() turns the entire
 * macro-node into a self-suspending task, and the EDF
 * feasibility proof on which the contracted-DAG schedule relies
 * does not apply to self-suspending tasks without explicit
 * suspension-aware analysis (Chen et al. 2019 §III).
 *
 * Inputs:
 *   - member_caps[]: per-member capability bitmask, length
 *     n_members. A NULL pointer with n_members > 0 rejects
 *     defensively (treated as "all members are unknown").
 *
 * Returns true with *out_reason = NONE on accept, false with
 * *out_reason = BLOCKING_RISK on reject. n_members == 0
 * trivially accepts.
 */
bool fusion_validator_blocking_closure_accept(
		const uint32_t *member_caps, uint32_t n_members,
		enum fusion_reject_reason *out_reason);

/* Stable lower_snake_case token for a given rejection reason.
 * Unknown values render as "unknown". */
const char *fusion_reject_reason_name(enum fusion_reject_reason r);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_FUSION_VALIDATOR_H */
