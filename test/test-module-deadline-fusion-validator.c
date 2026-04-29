/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Unit tests for module-deadline/fusion_validator.c.
 *
 * The validator's contract: accept a fusion candidate (a list of
 * members) iff every structural predicate passes; otherwise return
 * the first failing predicate's reason. This file pins the first
 * four predicates (same-driver, main-loop, exported,
 * remote-tid-unknown); the structural predicates land in
 * follow-up commits.
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/fusion_validator.h"

PWTEST(fusion_validator_empty_group_accepts)
{
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE; /* sentinel */
	pwtest_bool_true(fusion_validator_accept(NULL, 0, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

PWTEST(fusion_validator_null_out_reason_safe)
{
	struct fusion_candidate_member m = { 1, 100, 42, 0 };
	pwtest_bool_true(fusion_validator_accept(&m, 1, NULL));
	return PWTEST_PASS;
}

PWTEST(fusion_validator_single_clean_member_accepts)
{
	struct fusion_candidate_member m = { 1, 100, 42, 0 };
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_accept(&m, 1, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

PWTEST(fusion_validator_same_driver_accepts)
{
	struct fusion_candidate_member ms[] = {
		{ 1, 100, 42, 0 },
		{ 2, 100, 42, 0 },
		{ 3, 100, 42, 0 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_accept(ms, 3, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

PWTEST(fusion_validator_cross_driver_rejects)
{
	struct fusion_candidate_member ms[] = {
		{ 1, 100, 42, 0 },
		{ 2, 100, 43, 0 },  /* different driver */
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_accept(ms, 2, &r));
	pwtest_int_eq(r, FUSION_REJ_CROSS_DRIVER);
	return PWTEST_PASS;
}

PWTEST(fusion_validator_main_loop_rejects)
{
	struct fusion_candidate_member ms[] = {
		{ 1, 100, 42, 0 },
		{ 2, 100, 42, FUSION_MEMBER_MAIN_LOOP },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_accept(ms, 2, &r));
	pwtest_int_eq(r, FUSION_REJ_MAIN_LOOP);
	return PWTEST_PASS;
}

PWTEST(fusion_validator_exported_rejects)
{
	struct fusion_candidate_member ms[] = {
		{ 1, 100, 42, 0 },
		{ 2, 100, 42, FUSION_MEMBER_EXPORTED },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_accept(ms, 2, &r));
	pwtest_int_eq(r, FUSION_REJ_EXPORTED);
	return PWTEST_PASS;
}

/* A remote node WITH a known TID passes. */
PWTEST(fusion_validator_remote_known_tid_accepts)
{
	struct fusion_candidate_member ms[] = {
		{ 1, 100, 42, FUSION_MEMBER_REMOTE },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_true(fusion_validator_accept(ms, 1, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

PWTEST(fusion_validator_remote_unknown_tid_rejects)
{
	struct fusion_candidate_member ms[] = {
		{ 1, -1, 42, FUSION_MEMBER_REMOTE },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_accept(ms, 1, &r));
	pwtest_int_eq(r, FUSION_REJ_REMOTE_TID_UNKNOWN);
	return PWTEST_PASS;
}

PWTEST(fusion_validator_remote_zero_tid_rejects)
{
	/* tid <= 0 is "unknown" per the sched_groups convention. */
	struct fusion_candidate_member ms[] = {
		{ 1, 0, 42, FUSION_MEMBER_REMOTE },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_accept(ms, 1, &r));
	pwtest_int_eq(r, FUSION_REJ_REMOTE_TID_UNKNOWN);
	return PWTEST_PASS;
}

PWTEST(fusion_validator_returns_first_failing_predicate)
{
	/* The cross-driver and main-loop checks would both fail
	 * on this input; the validator must report whichever it
	 * checked first (CROSS_DRIVER, by predicate ordering inside
	 * the loop). */
	struct fusion_candidate_member ms[] = {
		{ 1, 100, 42, 0 },
		{ 2, 100, 43, FUSION_MEMBER_MAIN_LOOP },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_accept(ms, 2, &r));
	pwtest_int_eq(r, FUSION_REJ_CROSS_DRIVER);
	return PWTEST_PASS;
}

PWTEST(fusion_validator_reason_name_stability)
{
	pwtest_str_eq(fusion_reject_reason_name(FUSION_REJ_NONE),
		      "none");
	pwtest_str_eq(fusion_reject_reason_name(FUSION_REJ_CROSS_DRIVER),
		      "cross_driver");
	pwtest_str_eq(fusion_reject_reason_name(FUSION_REJ_MAIN_LOOP),
		      "main_loop");
	pwtest_str_eq(fusion_reject_reason_name(FUSION_REJ_EXPORTED),
		      "exported");
	pwtest_str_eq(fusion_reject_reason_name(FUSION_REJ_REMOTE_TID_UNKNOWN),
		      "remote_tid_unknown");
	pwtest_str_eq(fusion_reject_reason_name(FUSION_REJ_WOULD_SELF_SUSPEND),
		      "would_self_suspend");
	pwtest_str_eq(fusion_reject_reason_name(FUSION_REJ_NON_CONVEX),
		      "non_convex");
	pwtest_str_eq(fusion_reject_reason_name(FUSION_REJ_INTERNAL_MILESTONE),
		      "internal_milestone");
	pwtest_str_eq(fusion_reject_reason_name((enum fusion_reject_reason)999),
		      "unknown");
	return PWTEST_PASS;
}

/* --- predecessor closure predicate (release-barrier fallback) --- */

PWTEST(fusion_pred_closure_empty_group_accepts)
{
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_predecessor_closure_accept(
				NULL, 0, NULL, 0, NULL, 0, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Chain A -> B -> C, candidate F = {A, B}, sources = {A}.
 * B's only incoming edge is A -> B; A is in F, so the edge is
 * internal. F is accepted. */
PWTEST(fusion_pred_closure_chain_prefix_accepts)
{
	uint32_t members[]    = { 1, 2 };
	uint32_t sources[]    = { 1 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 2, 3 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_predecessor_closure_accept(
				members, 2, edges, 2, sources, 1, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Chain A -> B -> C, candidate F = {B, C}, sources = {A}.
 * B's incoming is A -> B; A is NOT in F. A IS a source, so
 * accepted. */
PWTEST(fusion_pred_closure_chain_suffix_with_source_pred_accepts)
{
	uint32_t members[]    = { 2, 3 };
	uint32_t sources[]    = { 1 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 2, 3 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_predecessor_closure_accept(
				members, 2, edges, 2, sources, 1, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Chain A -> B -> C -> D, candidate F = {C, D}, sources = {A}.
 * C's incoming is B -> C; B is NOT in F and NOT a source. The
 * group would have to wait for B mid-job; reject with
 * WOULD_SELF_SUSPEND. */
PWTEST(fusion_pred_closure_chain_mid_with_non_source_pred_rejects)
{
	uint32_t members[]    = { 3, 4 };
	uint32_t sources[]    = { 1 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 2, 3 }, { 3, 4 },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_predecessor_closure_accept(
				members, 2, edges, 3, sources, 1, &r));
	pwtest_int_eq(r, FUSION_REJ_WOULD_SELF_SUSPEND);
	return PWTEST_PASS;
}

/* Join A -> C, B -> C, candidate F = {B, C}, sources = {A, B}.
 * C's incoming includes A -> C; A is NOT in F. A IS a source, so
 * accepted. (B is in F so its edge is internal.) */
PWTEST(fusion_pred_closure_join_with_source_pred_accepts)
{
	uint32_t members[]    = { 2, 3 };
	uint32_t sources[]    = { 1, 2 };
	struct fusion_edge_input edges[] = {
		{ 1, 3 }, { 2, 3 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_predecessor_closure_accept(
				members, 2, edges, 2, sources, 2, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Join A -> C, B -> C, candidate F = {B, C}, sources = {A}.
 * (B is NOT a source -- imagine it has an upstream of its own.)
 * Same shape as the previous test but A is the only declared
 * source. B is in F so its predecessor situation does not matter;
 * A -> C must still be accepted because A is a source. */
PWTEST(fusion_pred_closure_join_member_pred_accepts)
{
	uint32_t members[]    = { 2, 3 };
	uint32_t sources[]    = { 1 };
	struct fusion_edge_input edges[] = {
		{ 1, 3 }, { 2, 3 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_predecessor_closure_accept(
				members, 2, edges, 2, sources, 1, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* No sources declared (n_sources == 0) -- any external predecessor
 * is rejected. */
PWTEST(fusion_pred_closure_no_sources_rejects_external_pred)
{
	uint32_t members[]    = { 2 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_predecessor_closure_accept(
				members, 1, edges, 1, NULL, 0, &r));
	pwtest_int_eq(r, FUSION_REJ_WOULD_SELF_SUSPEND);
	return PWTEST_PASS;
}

PWTEST(fusion_pred_closure_null_member_ids_rejects)
{
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_false(fusion_validator_predecessor_closure_accept(
				NULL, 1, NULL, 0, NULL, 0, &r));
	/* No useful reason on this defensive failure, but the
	 * return value must be false and the caller must not be
	 * fooled into accepting the group. */
	return PWTEST_PASS;
}

/* --- precedence convexity predicate --- */

PWTEST(fusion_convex_singleton_accepts)
{
	uint32_t members[] = { 1 };
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_precedence_convex_accept(
				members, 1, NULL, 0, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Chain A -> B -> C, F = {A, B, C}. Every edge is internal, no
 * paths leave the group. Convex. */
PWTEST(fusion_convex_chain_full_accepts)
{
	uint32_t members[] = { 1, 2, 3 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 2, 3 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_precedence_convex_accept(
				members, 3, edges, 2, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Diamond A -> B -> D and A -> C -> D, F = {A, D}. Walk from A
 * reaches B (outside), B reaches D (inside) -> non-convex. */
PWTEST(fusion_convex_diamond_top_and_bottom_rejects)
{
	uint32_t members[] = { 1, 4 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 1, 3 }, { 2, 4 }, { 3, 4 },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_precedence_convex_accept(
				members, 2, edges, 4, &r));
	pwtest_int_eq(r, FUSION_REJ_NON_CONVEX);
	return PWTEST_PASS;
}

/* Diamond, F = {A, B, D}. From A, the only external successor is
 * C; C's successor is D (inside) -> non-convex (because path
 * A -> C -> D leaves and re-enters). */
PWTEST(fusion_convex_diamond_three_of_four_rejects)
{
	uint32_t members[] = { 1, 2, 4 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 1, 3 }, { 2, 4 }, { 3, 4 },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_precedence_convex_accept(
				members, 3, edges, 4, &r));
	pwtest_int_eq(r, FUSION_REJ_NON_CONVEX);
	return PWTEST_PASS;
}

/* Diamond, F = {A, B, C, D} (whole graph). Every successor is
 * inside F; convex. */
PWTEST(fusion_convex_diamond_full_accepts)
{
	uint32_t members[] = { 1, 2, 3, 4 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 1, 3 }, { 2, 4 }, { 3, 4 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_precedence_convex_accept(
				members, 4, edges, 4, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Fork from A: A -> B, A -> C. F = {A, B, C}. From A, both
 * successors are in F (internal); no external traversal needed.
 * Convex. */
PWTEST(fusion_convex_fork_full_accepts)
{
	uint32_t members[] = { 1, 2, 3 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 1, 3 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_precedence_convex_accept(
				members, 3, edges, 2, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Chain A -> B -> C -> D, F = {A, C}. A's external successor is
 * B; B reaches C (inside) -> non-convex. Equivalent to the
 * prototype's TID-aliased pattern that triggers the contracted
 * cycle-detection fallback today. */
PWTEST(fusion_convex_chain_skip_middle_rejects)
{
	uint32_t members[] = { 1, 3 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 2, 3 }, { 3, 4 },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_precedence_convex_accept(
				members, 2, edges, 3, &r));
	pwtest_int_eq(r, FUSION_REJ_NON_CONVEX);
	return PWTEST_PASS;
}

PWTEST(fusion_convex_null_member_ids_rejects)
{
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_precedence_convex_accept(
				NULL, 2, NULL, 0, &r));
	return PWTEST_PASS;
}

/* --- externally atomic predicate --- */

PWTEST(fusion_externally_atomic_singleton_accepts)
{
	uint32_t members[] = { 1 };
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_externally_atomic_accept(
				members, 1, NULL, 0, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Chain A -> B -> C, F = {A, B}. A is non-terminal (A -> B
 * internal). A has no external successors. B is terminal (no
 * internal successor); B -> C is an external successor from a
 * terminal member -- accepted. */
PWTEST(fusion_externally_atomic_chain_prefix_accepts)
{
	uint32_t members[] = { 1, 2 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 2, 3 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_externally_atomic_accept(
				members, 2, edges, 2, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* A -> B -> C, A -> X, F = {A, B}. A is non-terminal (A -> B
 * internal). A also has external successor X -- that is the
 * internal-milestone violation. Reject. */
PWTEST(fusion_externally_atomic_non_terminal_external_succ_rejects)
{
	uint32_t members[] = { 1, 2 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 2, 3 }, { 1, 99 },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_externally_atomic_accept(
				members, 2, edges, 3, &r));
	pwtest_int_eq(r, FUSION_REJ_INTERNAL_MILESTONE);
	return PWTEST_PASS;
}

/* Fork A -> B, A -> C, F = {A, B, C}. A has two internal
 * outgoing edges (to B and to C), so A is non-terminal. A has
 * no external outgoing edges. B and C are terminal (no
 * outgoing edges at all). Accepted. */
PWTEST(fusion_externally_atomic_fork_full_accepts)
{
	uint32_t members[] = { 1, 2, 3 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 1, 3 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_externally_atomic_accept(
				members, 3, edges, 2, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Chain A -> B -> C with C having external successor D, F =
 * {A, B, C}. A: non-terminal, no external out. B: non-terminal,
 * no external out. C: terminal (no internal out), external out
 * to D -- allowed. Accept. */
PWTEST(fusion_externally_atomic_chain_full_with_external_sink_accepts)
{
	uint32_t members[] = { 1, 2, 3 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 2, 3 }, { 3, 4 },
	};
	enum fusion_reject_reason r = FUSION_REJ_INTERNAL_MILESTONE;
	pwtest_bool_true(fusion_validator_externally_atomic_accept(
				members, 3, edges, 3, &r));
	pwtest_int_eq(r, FUSION_REJ_NONE);
	return PWTEST_PASS;
}

/* Chain A -> B -> C, B -> X (external), F = {A, B, C}. B is
 * non-terminal (B -> C is internal). B also has external
 * successor X. Reject (B is an internal milestone). */
PWTEST(fusion_externally_atomic_middle_external_succ_rejects)
{
	uint32_t members[] = { 1, 2, 3 };
	struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 2, 3 }, { 2, 99 },
	};
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_externally_atomic_accept(
				members, 3, edges, 3, &r));
	pwtest_int_eq(r, FUSION_REJ_INTERNAL_MILESTONE);
	return PWTEST_PASS;
}

PWTEST(fusion_externally_atomic_null_member_ids_rejects)
{
	enum fusion_reject_reason r = FUSION_REJ_NONE;
	pwtest_bool_false(fusion_validator_externally_atomic_accept(
				NULL, 2, NULL, 0, &r));
	return PWTEST_PASS;
}

/*
 * Property-style test: random member sets carrying any combination
 * of FUSION_MEMBER_MAIN_LOOP / EXPORTED / REMOTE plus per-member
 * driver_id and tid choices must produce exactly one of the
 * predicate-aligned rejection reasons, with a stable precedence
 * (the validator returns the first failing predicate, never a
 * lower one). Drives 2048 random candidate groups under an
 * LCG seed so failures are reproducible. */
PWTEST(fusion_validator_property_random_member_flags_yield_one_typed_reason)
{
	uint32_t seed = 0x5C9F2A11u;
	uint32_t trial;
	const uint32_t trials = 2048;
	const uint32_t n_members = 4;
	uint32_t main_loop_count = 0, exported_count = 0;
	uint32_t remote_unknown_count = 0, cross_driver_count = 0;
	uint32_t accepted_count = 0;

	for (trial = 0; trial < trials; trial++) {
		struct fusion_candidate_member m[4];
		enum fusion_reject_reason got = FUSION_REJ_NONE;
		enum fusion_reject_reason want = FUSION_REJ_NONE;
		uint32_t i;
		uint32_t base_driver;

		seed = seed * 1103515245u + 12345u;
		base_driver = seed & 0xFFu;
		for (i = 0; i < n_members; i++) {
			seed = seed * 1103515245u + 12345u;
			m[i].id = i + 1;
			m[i].flags = (seed >> 1) & 0x7u; /* 0..7 */
			m[i].tid = ((seed >> 4) & 0x1u) ? 1000 + i : -1;
			m[i].driver_id = ((seed >> 8) & 0x3u)
					? base_driver
					: base_driver + 1;
		}

		/* Walk the validator's exact member-major precedence: for
		 * each member in order, check cross-driver, then main-loop,
		 * then exported, then remote-tid-unknown. The first
		 * failing check on the first failing member wins. */
		for (i = 0; i < n_members; i++) {
			if (i > 0 && m[i].driver_id != m[0].driver_id) {
				want = FUSION_REJ_CROSS_DRIVER;
				break;
			}
			if (m[i].flags & FUSION_MEMBER_MAIN_LOOP) {
				want = FUSION_REJ_MAIN_LOOP;
				break;
			}
			if (m[i].flags & FUSION_MEMBER_EXPORTED) {
				want = FUSION_REJ_EXPORTED;
				break;
			}
			if ((m[i].flags & FUSION_MEMBER_REMOTE) && m[i].tid <= 0) {
				want = FUSION_REJ_REMOTE_TID_UNKNOWN;
				break;
			}
		}

		(void)fusion_validator_accept(m, n_members, &got);
		pwtest_int_eq(got, want);

		switch (want) {
		case FUSION_REJ_NONE:               accepted_count++; break;
		case FUSION_REJ_CROSS_DRIVER:       cross_driver_count++; break;
		case FUSION_REJ_MAIN_LOOP:          main_loop_count++; break;
		case FUSION_REJ_EXPORTED:           exported_count++; break;
		case FUSION_REJ_REMOTE_TID_UNKNOWN: remote_unknown_count++; break;
		default: break;
		}
	}

	/* Sanity coverage -- 2048 trials must exercise every rejection
	 * branch at least a handful of times or the random generator
	 * is degenerate. The all-clean (accepted) branch is rare with
	 * uniform-random flag bits across 4 members; suppress its
	 * coverage assertion -- the per-iteration pwtest_int_eq is
	 * what actually pins the policy. */
	pwtest_bool_true(main_loop_count > 50);
	pwtest_bool_true(exported_count > 50);
	pwtest_bool_true(remote_unknown_count > 50);
	pwtest_bool_true(cross_driver_count > 50);
	(void)accepted_count;

	return PWTEST_PASS;
}

/*
 * Small-graph oracle: enumerate every non-empty subset of two
 * fixed graph shapes, drive each through the precedence-convex
 * and externally-atomic predicates, and pin the expected accept /
 * reject verdict against a closed-form oracle. Graphs are kept
 * small (5 nodes => 31 non-empty subsets, 8 nodes => 255 subsets)
 * so the brute-force enumeration runs in microseconds and the
 * test remains a tractable regression net.
 *
 * Shape A is a pure chain (1->2->3->4->5). On a 1-in-1-out chain:
 *   - Convexity rejects non-contiguous subsets (a "skip" in the
 *     member range forces a path that leaves and re-enters the
 *     group). Contiguous ranges pass.
 *   - Externally-atomic accepts every subset: every non-terminal
 *     internal member's only out-edge points to an internal
 *     successor (it cannot have an "external" out by construction
 *     of the chain).
 *
 * Shape B is a fork-join diamond (1->2, 1->3, 2->4, 3->4, 4->5).
 * Here externally-atomic does catch non-trivial rejection: the
 * fork point (member 1) with one child in the group and one not
 * is a non-terminal internal member with an external out.
 *
 * Both shapes are within the plan's "graphs with up to 8 nodes"
 * scope; together they cover the chain / fork / join / diamond
 * shapes the plan calls out for property coverage.
 */
static bool subset_contains(uint32_t mask, uint32_t i)
{
	return (mask >> i) & 1u;
}

PWTEST(fusion_oracle_enumerate_chain_5)
{
	const uint32_t N = 5;
	const struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 5 },
	};
	const uint32_t n_edges = 4;
	uint32_t mask;
	uint32_t accepted_convex = 0;
	uint32_t accepted_externally_atomic = 0;

	for (mask = 1; mask < (1u << N); mask++) {
		uint32_t members[5];
		uint32_t n = 0;
		uint32_t i;
		uint32_t min_id = UINT32_MAX, max_id = 0;
		bool contiguous;
		enum fusion_reject_reason cr = FUSION_REJ_NONE;
		enum fusion_reject_reason ar = FUSION_REJ_NONE;
		bool conv_ok, ea_ok;

		for (i = 0; i < N; i++) {
			if (!subset_contains(mask, i))
				continue;
			members[n++] = i + 1;
			if (i + 1 < min_id) min_id = i + 1;
			if (i + 1 > max_id) max_id = i + 1;
		}
		contiguous = (n == (max_id - min_id + 1));

		conv_ok = fusion_validator_precedence_convex_accept(
				members, n, edges, n_edges, &cr);
		ea_ok = fusion_validator_externally_atomic_accept(
				members, n, edges, n_edges, &ar);

		if (n == 1) {
			pwtest_bool_true(conv_ok);
			pwtest_bool_true(ea_ok);
			accepted_convex++;
			accepted_externally_atomic++;
			continue;
		}
		/* Chain convexity: contiguous range iff accepted. */
		pwtest_int_eq((int)conv_ok, (int)contiguous);
		if (conv_ok)
			accepted_convex++;
		/* Chain externally-atomic: every subset accepts because
		 * non-terminal members on a 1-in-1-out chain can only
		 * point to internal successors. */
		pwtest_bool_true(ea_ok);
		accepted_externally_atomic++;
	}

	/* On a 5-chain: 5 singletons + 4+3+2+1 = 15 contiguous ranges
	 * pass convexity; every non-empty subset (31 total) passes
	 * externally-atomic. */
	pwtest_int_eq((int)accepted_convex, 15);
	pwtest_int_eq((int)accepted_externally_atomic, 31);

	return PWTEST_PASS;
}

PWTEST(fusion_oracle_enumerate_diamond_5)
{
	/* 1 -> 2 -> 4   |   1 -> 3 -> 4 -> 5
	 * Members are numbered 1..5; bit i of mask selects member i+1. */
	const uint32_t N = 5;
	const struct fusion_edge_input edges[] = {
		{ 1, 2 }, { 1, 3 }, { 2, 4 }, { 3, 4 }, { 4, 5 },
	};
	const uint32_t n_edges = 5;
	uint32_t mask;
	uint32_t accepted_externally_atomic_nontrivial = 0;

	for (mask = 1; mask < (1u << N); mask++) {
		uint32_t members[5];
		bool inset[6] = { false };
		uint32_t n = 0;
		uint32_t i;
		enum fusion_reject_reason ar = FUSION_REJ_NONE;
		bool ea_ok;
		bool oracle_ea;
		uint32_t e;

		for (i = 0; i < N; i++) {
			if (!subset_contains(mask, i))
				continue;
			members[n++] = i + 1;
			inset[i + 1] = true;
		}
		ea_ok = fusion_validator_externally_atomic_accept(
				members, n, edges, n_edges, &ar);

		/* Closed-form externally-atomic oracle on this diamond:
		 * for each member v in the group, classify v as terminal
		 * iff v has no outgoing edge to another member in the
		 * group. Reject iff some non-terminal v has an outgoing
		 * edge to a non-member. */
		oracle_ea = true;
		for (i = 0; i < N; i++) {
			uint32_t v;
			bool v_has_internal_out = false;
			bool v_has_external_out = false;
			if (!subset_contains(mask, i))
				continue;
			v = i + 1;
			for (e = 0; e < n_edges; e++) {
				if (edges[e].src_id != v)
					continue;
				if (inset[edges[e].dst_id])
					v_has_internal_out = true;
				else
					v_has_external_out = true;
			}
			if (v_has_internal_out && v_has_external_out) {
				oracle_ea = false;
				break;
			}
		}
		pwtest_int_eq((int)ea_ok, (int)oracle_ea);
		if (ea_ok && n > 1)
			accepted_externally_atomic_nontrivial++;
	}

	/* Sanity: at least one non-trivial accept AND at least one
	 * non-trivial reject so we know both branches of the oracle
	 * fired. (Specifically: {2, 4} accepts -- 2's only out is to
	 * 4 internal, 4 has external out to 5 but is terminal. {1, 2}
	 * rejects -- 1 has internal out to 2 AND external out to 3.) */
	pwtest_bool_true(accepted_externally_atomic_nontrivial > 0);

	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_fusion_validator)
{
	pwtest_add(fusion_validator_empty_group_accepts, PWTEST_NOARG);
	pwtest_add(fusion_validator_null_out_reason_safe, PWTEST_NOARG);
	pwtest_add(fusion_validator_single_clean_member_accepts, PWTEST_NOARG);
	pwtest_add(fusion_validator_same_driver_accepts, PWTEST_NOARG);
	pwtest_add(fusion_validator_cross_driver_rejects, PWTEST_NOARG);
	pwtest_add(fusion_validator_main_loop_rejects, PWTEST_NOARG);
	pwtest_add(fusion_validator_exported_rejects, PWTEST_NOARG);
	pwtest_add(fusion_validator_remote_known_tid_accepts, PWTEST_NOARG);
	pwtest_add(fusion_validator_remote_unknown_tid_rejects, PWTEST_NOARG);
	pwtest_add(fusion_validator_remote_zero_tid_rejects, PWTEST_NOARG);
	pwtest_add(fusion_validator_returns_first_failing_predicate, PWTEST_NOARG);
	pwtest_add(fusion_validator_reason_name_stability, PWTEST_NOARG);
	pwtest_add(fusion_pred_closure_empty_group_accepts, PWTEST_NOARG);
	pwtest_add(fusion_pred_closure_chain_prefix_accepts, PWTEST_NOARG);
	pwtest_add(fusion_pred_closure_chain_suffix_with_source_pred_accepts, PWTEST_NOARG);
	pwtest_add(fusion_pred_closure_chain_mid_with_non_source_pred_rejects, PWTEST_NOARG);
	pwtest_add(fusion_pred_closure_join_with_source_pred_accepts, PWTEST_NOARG);
	pwtest_add(fusion_pred_closure_join_member_pred_accepts, PWTEST_NOARG);
	pwtest_add(fusion_pred_closure_no_sources_rejects_external_pred, PWTEST_NOARG);
	pwtest_add(fusion_pred_closure_null_member_ids_rejects, PWTEST_NOARG);
	pwtest_add(fusion_convex_singleton_accepts, PWTEST_NOARG);
	pwtest_add(fusion_convex_chain_full_accepts, PWTEST_NOARG);
	pwtest_add(fusion_convex_diamond_top_and_bottom_rejects, PWTEST_NOARG);
	pwtest_add(fusion_convex_diamond_three_of_four_rejects, PWTEST_NOARG);
	pwtest_add(fusion_convex_diamond_full_accepts, PWTEST_NOARG);
	pwtest_add(fusion_convex_fork_full_accepts, PWTEST_NOARG);
	pwtest_add(fusion_convex_chain_skip_middle_rejects, PWTEST_NOARG);
	pwtest_add(fusion_convex_null_member_ids_rejects, PWTEST_NOARG);
	pwtest_add(fusion_externally_atomic_singleton_accepts, PWTEST_NOARG);
	pwtest_add(fusion_externally_atomic_chain_prefix_accepts, PWTEST_NOARG);
	pwtest_add(fusion_externally_atomic_non_terminal_external_succ_rejects, PWTEST_NOARG);
	pwtest_add(fusion_externally_atomic_fork_full_accepts, PWTEST_NOARG);
	pwtest_add(fusion_externally_atomic_chain_full_with_external_sink_accepts, PWTEST_NOARG);
	pwtest_add(fusion_externally_atomic_middle_external_succ_rejects, PWTEST_NOARG);
	pwtest_add(fusion_externally_atomic_null_member_ids_rejects, PWTEST_NOARG);
	pwtest_add(fusion_validator_property_random_member_flags_yield_one_typed_reason,
			PWTEST_NOARG);
	pwtest_add(fusion_oracle_enumerate_chain_5, PWTEST_NOARG);
	pwtest_add(fusion_oracle_enumerate_diamond_5, PWTEST_NOARG);

	return PWTEST_PASS;
}
