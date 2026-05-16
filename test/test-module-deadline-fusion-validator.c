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

	return PWTEST_PASS;
}
