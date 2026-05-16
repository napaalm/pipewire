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
	enum fusion_reject_reason r = FUSION_REJ_BLOCKING_RISK; /* sentinel */
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
	enum fusion_reject_reason r = FUSION_REJ_BLOCKING_RISK;
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
	enum fusion_reject_reason r = FUSION_REJ_BLOCKING_RISK;
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
	pwtest_str_eq(fusion_reject_reason_name(FUSION_REJ_BLOCKING_RISK),
		      "blocking_risk");
	pwtest_str_eq(fusion_reject_reason_name((enum fusion_reject_reason)999),
		      "unknown");
	return PWTEST_PASS;
}

/* --- predecessor closure predicate (Phase 3.2 fallback) --- */

PWTEST(fusion_pred_closure_empty_group_accepts)
{
	enum fusion_reject_reason r = FUSION_REJ_BLOCKING_RISK;
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
	enum fusion_reject_reason r = FUSION_REJ_BLOCKING_RISK;
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
	enum fusion_reject_reason r = FUSION_REJ_BLOCKING_RISK;
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
	enum fusion_reject_reason r = FUSION_REJ_BLOCKING_RISK;
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
	enum fusion_reject_reason r = FUSION_REJ_BLOCKING_RISK;
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
	enum fusion_reject_reason r = FUSION_REJ_BLOCKING_RISK;
	pwtest_bool_false(fusion_validator_predecessor_closure_accept(
				NULL, 1, NULL, 0, NULL, 0, &r));
	/* No useful reason on this defensive failure, but the
	 * return value must be false and the caller must not be
	 * fooled into accepting the group. */
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

	return PWTEST_PASS;
}
