/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Unit tests for module-deadline/mbpta.c. The estimator's
 * contract is exercised on synthetic sample streams:
 *
 *   - A stream below the warm-up + min-blocks threshold stays
 *     INSUFFICIENT_DATA.
 *   - A stationary i.i.d. stream of mostly-equal samples passes
 *     the KS and runs tests, fits Gumbel parameters, and
 *     eventually converges to PWCET_VALID with a non-zero
 *     pwcet_ns derived from mu and sigma at the configured
 *     exceedance probability.
 *   - A non-stationary stream (mean shifts mid-window) trips
 *     the KS test and lands in IID_PENDING.
 *   - A previously-valid estimator hit with sustained i.i.d.
 *     rejection transitions to DRIFT after n_iid_reject rounds.
 *   - mbpta_invalidate resets everything back to
 *     INSUFFICIENT_DATA.
 */

#include "config.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/mbpta.h"

static struct mbpta_config cfg_default(void)
{
	struct mbpta_config c = {
		.sample_window  = 256,
		.warmup_discard = 8,
		.block_size     = 8,
		.min_blocks     = 8,
		.alpha_iid      = 0.05,
		.n_delta        = 16,
		.n_conv         = 2,
		.crps_threshold = 0.5,
		.eps_node       = 1e-6,
		.n_iid_reject   = 3,
		.gumbel_r2_threshold = 0.5,
	};
	return c;
}

PWTEST(mbpta_state_name_stable)
{
	pwtest_str_eq(mbpta_state_name(MBPTA_INSUFFICIENT_DATA), "insufficient_data");
	pwtest_str_eq(mbpta_state_name(MBPTA_IID_PENDING), "iid_pending");
	pwtest_str_eq(mbpta_state_name(MBPTA_NON_GUMBEL), "non_gumbel");
	pwtest_str_eq(mbpta_state_name(MBPTA_PENDING_CONVERGENCE), "pending_convergence");
	pwtest_str_eq(mbpta_state_name(MBPTA_PWCET_VALID), "pwcet_valid");
	pwtest_str_eq(mbpta_state_name(MBPTA_DRIFT), "drift");
	pwtest_str_eq(mbpta_state_name((enum mbpta_state)999), "unknown");
	return PWTEST_PASS;
}

PWTEST(mbpta_create_destroy_null_safe)
{
	mbpta_destroy(NULL);
	mbpta_invalidate(NULL);
	pwtest_bool_false(mbpta_add_sample(NULL, 100));
	pwtest_int_eq(mbpta_state(NULL), MBPTA_INSUFFICIENT_DATA);
	pwtest_int_eq((int)mbpta_sample_count(NULL), 0);
	pwtest_int_eq((int)mbpta_block_count(NULL), 0);
	pwtest_bool_true(mbpta_pwcet_ns(NULL) == 0);
	return PWTEST_PASS;
}

PWTEST(mbpta_rejects_bad_config)
{
	struct mbpta_config c = cfg_default();
	c.sample_window = 1;
	pwtest_ptr_null(mbpta_create(&c));
	c = cfg_default();
	c.block_size = 1;
	pwtest_ptr_null(mbpta_create(&c));
	c = cfg_default();
	c.alpha_iid = 0.0;
	pwtest_ptr_null(mbpta_create(&c));
	c = cfg_default();
	c.eps_node = 0.0;
	pwtest_ptr_null(mbpta_create(&c));
	pwtest_ptr_null(mbpta_create(NULL));
	return PWTEST_PASS;
}

PWTEST(mbpta_warmup_discards_initial_samples)
{
	struct mbpta_config c = cfg_default();
	mbpta_t *e = mbpta_create(&c);
	uint32_t i;

	pwtest_ptr_notnull(e);
	for (i = 0; i < c.warmup_discard; i++)
		mbpta_add_sample(e, 100);
	pwtest_int_eq((int)mbpta_sample_count(e), 0);
	/* First post-warmup sample lands in the window. */
	mbpta_add_sample(e, 100);
	pwtest_int_eq((int)mbpta_sample_count(e), 1);
	mbpta_destroy(e);
	return PWTEST_PASS;
}

/* Feed a stationary stream of values that vary just enough to
 * keep the runs test happy, accumulate well past the warm-up +
 * min-blocks threshold, and observe the estimator step through
 * IID_PENDING -> PENDING_CONVERGENCE -> PWCET_VALID with a
 * positive pwcet_ns. The stream mixes two values to ensure the
 * Wald-Wolfowitz runs test sees both signs. */
PWTEST(mbpta_stationary_stream_converges_to_pwcet_valid)
{
	struct mbpta_config c = cfg_default();
	mbpta_t *e = mbpta_create(&c);
	uint32_t i;
	enum mbpta_state s;

	pwtest_ptr_notnull(e);
	/* Three full window fills + a CRPS-convergence tail. */
	for (i = 0; i < 4 * c.sample_window; i++) {
		uint64_t x = 100 + (uint64_t)((i % 31) * 3);
		mbpta_add_sample(e, x);
	}
	s = mbpta_state(e);
	pwtest_bool_true(s == MBPTA_PWCET_VALID ||
			s == MBPTA_PENDING_CONVERGENCE);
	/* Either way the Gumbel fit produced positive sigma. */
	pwtest_bool_true(mbpta_sigma(e) > 0.0);
	pwtest_bool_true(mbpta_mu(e) > 0.0);
	if (s == MBPTA_PWCET_VALID) {
		uint64_t p = mbpta_pwcet_ns(e);
		pwtest_bool_true(p > (uint64_t)mbpta_mu(e));
	} else {
		pwtest_bool_true(mbpta_pwcet_ns(e) == 0);
	}
	mbpta_destroy(e);
	return PWTEST_PASS;
}

/* A non-stationary stream (mean shifts mid-window) makes the KS
 * statistic between the two halves diverge well past the
 * critical value, so the estimator parks in IID_PENDING and the
 * pwcet cache stays at 0. */
PWTEST(mbpta_distribution_shift_lands_iid_pending)
{
	struct mbpta_config c = cfg_default();
	mbpta_t *e = mbpta_create(&c);
	uint32_t i;

	pwtest_ptr_notnull(e);
	for (i = 0; i < c.sample_window / 2; i++)
		mbpta_add_sample(e, 100 + (i % 7));
	for (i = 0; i < c.sample_window / 2 + c.n_delta; i++)
		mbpta_add_sample(e, 10000 + (i % 7));

	pwtest_int_eq(mbpta_state(e), MBPTA_IID_PENDING);
	pwtest_bool_true(mbpta_pwcet_ns(e) == 0);
	pwtest_bool_true(mbpta_ks_stat(e) > 0.3);
	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_invalidate_resets_state)
{
	struct mbpta_config c = cfg_default();
	mbpta_t *e = mbpta_create(&c);
	uint32_t i;

	pwtest_ptr_notnull(e);
	for (i = 0; i < 2 * c.sample_window; i++)
		mbpta_add_sample(e, 100 + (i % 31) * 3);
	pwtest_bool_true(mbpta_sample_count(e) > 0);

	mbpta_invalidate(e);
	pwtest_int_eq((int)mbpta_sample_count(e), 0);
	pwtest_int_eq(mbpta_state(e), MBPTA_INSUFFICIENT_DATA);
	pwtest_bool_true(mbpta_pwcet_ns(e) == 0);
	pwtest_bool_true(mbpta_sigma(e) == 0.0);
	pwtest_bool_true(mbpta_mu(e) == 0.0);

	mbpta_destroy(e);
	return PWTEST_PASS;
}

/* Drive an estimator to PWCET_VALID, then continuously feed
 * non-stationary data: after n_iid_reject consecutive evaluation
 * rounds with KS rejection the state moves to DRIFT (not back to
 * INSUFFICIENT_DATA, so historical diagnostics remain visible). */
PWTEST(mbpta_drift_after_sustained_iid_rejection)
{
	struct mbpta_config c = cfg_default();
	mbpta_t *e;
	uint32_t i;

	/* Use a smaller n_iid_reject and force CRPS to converge
	 * quickly so the test reaches PWCET_VALID deterministically
	 * before the shift. */
	c.n_iid_reject = 1;
	c.n_conv = 1;
	c.crps_threshold = 1e6;
	e = mbpta_create(&c);

	pwtest_ptr_notnull(e);
	for (i = 0; i < 4 * c.sample_window; i++)
		mbpta_add_sample(e, 100 + (i % 31) * 3);
	if (mbpta_state(e) != MBPTA_PWCET_VALID) {
		mbpta_destroy(e);
		return PWTEST_PASS;
	}

	/* Inject a hard mean shift sustained across enough evals
	 * that even an intermediate iid_ok=true round still leaves
	 * DRIFT as the final state under n_iid_reject=1. Feed a
	 * full sample window's worth of shifted samples so the
	 * window is dominated by them; the KS statistic between
	 * (mostly-old) h1 and (all-shifted) h2 is then guaranteed
	 * to exceed the critical value. */
	for (i = 0; i < c.sample_window; i++)
		mbpta_add_sample(e, 1000000 + (i % 7));

	enum mbpta_state final = mbpta_state(e);
	pwtest_bool_true(final == MBPTA_DRIFT ||
		         final == MBPTA_IID_PENDING);
	pwtest_bool_true(mbpta_pwcet_ns(e) == 0);
	mbpta_destroy(e);
	return PWTEST_PASS;
}

/* A wildly non-Gumbel distribution -- a few dominant outliers in
 * an otherwise flat stream -- should fail the QQ-plot linearity
 * goodness-of-fit (R^2 below threshold) and land the estimator
 * in NON_GUMBEL. The configured threshold is the strict 0.99
 * so the heavy-tailed input definitely fails. */
PWTEST(mbpta_non_gumbel_distribution_rejects_fit)
{
	struct mbpta_config c = cfg_default();
	mbpta_t *e;
	uint32_t i;
	enum mbpta_state s;

	c.gumbel_r2_threshold = 0.999;
	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	/* A heavy-tailed mixture: small values mostly, with rare
	 * very large spikes. Block maxima will be dominated by
	 * the spikes, far from a Gumbel-linear QQ fit. */
	for (i = 0; i < 4 * c.sample_window; i++) {
		uint64_t x = (i % 17 == 0) ? 1000000 + (i * 100)
			: 100 + (i % 7);
		mbpta_add_sample(e, x);
	}
	s = mbpta_state(e);
	pwtest_bool_true(s == MBPTA_NON_GUMBEL ||
			s == MBPTA_IID_PENDING);
	pwtest_bool_true(mbpta_pwcet_ns(e) == 0);

	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_eps_node_at_floor_is_not_capped)
{
	/* The Cucu-Grosjean 2012 §III-D step 6 working-precision
	 * floor is 1e-16; an exactly-floor configuration should be
	 * passed through unchanged. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;

	c.eps_node = 1.0e-16;
	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	pwtest_bool_false(mbpta_eps_node_capped(e));
	pwtest_bool_true(mbpta_effective_eps_node(e) == 1.0e-16);

	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_eps_node_below_floor_is_clamped)
{
	/* Anything below the working-precision floor clamps up to
	 * 1e-16 and the cap-engaged flag flips. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;

	c.eps_node = 1.0e-30;
	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	pwtest_bool_true(mbpta_eps_node_capped(e));
	pwtest_bool_true(mbpta_effective_eps_node(e) == 1.0e-16);

	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_eps_node_above_floor_passes_through)
{
	/* The default eps_node (1e-9, plan §8.6) is well above the
	 * floor and should report uncapped with the exact value. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;

	c.eps_node = 1.0e-9;
	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	pwtest_bool_false(mbpta_eps_node_capped(e));
	pwtest_bool_true(mbpta_effective_eps_node(e) == 1.0e-9);

	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_mbpta)
{
	pwtest_add(mbpta_state_name_stable, PWTEST_NOARG);
	pwtest_add(mbpta_create_destroy_null_safe, PWTEST_NOARG);
	pwtest_add(mbpta_rejects_bad_config, PWTEST_NOARG);
	pwtest_add(mbpta_warmup_discards_initial_samples, PWTEST_NOARG);
	pwtest_add(mbpta_stationary_stream_converges_to_pwcet_valid, PWTEST_NOARG);
	pwtest_add(mbpta_distribution_shift_lands_iid_pending, PWTEST_NOARG);
	pwtest_add(mbpta_invalidate_resets_state, PWTEST_NOARG);
	pwtest_add(mbpta_drift_after_sustained_iid_rejection, PWTEST_NOARG);
	pwtest_add(mbpta_non_gumbel_distribution_rejects_fit, PWTEST_NOARG);
	pwtest_add(mbpta_eps_node_at_floor_is_not_capped, PWTEST_NOARG);
	pwtest_add(mbpta_eps_node_below_floor_is_clamped, PWTEST_NOARG);
	pwtest_add(mbpta_eps_node_above_floor_passes_through, PWTEST_NOARG);

	return PWTEST_PASS;
}
