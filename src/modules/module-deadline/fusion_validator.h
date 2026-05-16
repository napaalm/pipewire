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
 * Phase 0 of the predicate set (this commit) covers
 * CROSS_DRIVER / MAIN_LOOP / EXPORTED / REMOTE_TID_UNKNOWN; the
 * structural predicates (WOULD_SELF_SUSPEND, NON_CONVEX,
 * INTERNAL_MILESTONE, BLOCKING_RISK) land in their own commits.
 * Until those land, the validator only filters at the analyzability
 * level; structurally unsound but analyzable candidates still pass
 * and the downstream contracted-DAG dispatcher's cycle-detection
 * fallback catches the worst cases.
 */
bool fusion_validator_accept(const struct fusion_candidate_member *members,
		uint32_t n_members,
		enum fusion_reject_reason *out_reason);

/* Stable lower_snake_case token for a given rejection reason.
 * Unknown values render as "unknown". */
const char *fusion_reject_reason_name(enum fusion_reject_reason r);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_FUSION_VALIDATOR_H */
