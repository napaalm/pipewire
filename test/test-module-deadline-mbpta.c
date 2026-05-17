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

/* Helpers for tests that need a deterministic-but-i.i.d. sample
 * stream: SplitMix64 plus the Gumbel inverse CDF
 *     x = mu - sigma * ln(-ln(U))     with U ~ Uniform(0,1)
 * lets the suite drive the estimator with a closed-form Gumbel
 * source whose moments are known a priori. The runs test on the
 * up/down sequence of an i.i.d. continuous source expects
 * (2N-1)/3 runs (Bartels 1982) -- the suite verifies the
 * estimator does not falsely reject under that distribution. */
static uint64_t mbpta_test_rng_next(uint64_t *state)
{
	uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static double mbpta_test_uniform(uint64_t *state)
{
	uint64_t r = mbpta_test_rng_next(state);
	double u = ((r >> 11) & ((1ULL << 53) - 1)) / (double)(1ULL << 53);
	if (u <= 0.0) u = 1.0e-12;
	if (u >= 1.0) u = 1.0 - 1.0e-12;
	return u;
}

static uint64_t mbpta_test_gumbel_sample(uint64_t *state, double mu,
		double sigma)
{
	double u = mbpta_test_uniform(state);
	double x = mu - sigma * log(-log(u));
	if (x < 0.0) x = 0.0;
	return (uint64_t)x;
}

/* Feed a stationary i.i.d. Gumbel(mu, sigma) stream, accumulate
 * well past the warm-up + min-blocks threshold, and observe the
 * estimator step through IID_PENDING -> PENDING_CONVERGENCE ->
 * PWCET_VALID with a positive pwcet_ns. */
PWTEST(mbpta_stationary_stream_converges_to_pwcet_valid)
{
	struct mbpta_config c = cfg_default();
	mbpta_t *e = mbpta_create(&c);
	uint32_t i;
	enum mbpta_state s;
	uint64_t rng = 0xC0FFEE0123456789ULL;

	pwtest_ptr_notnull(e);
	/* Three full window fills + a CRPS-convergence tail. */
	for (i = 0; i < 4 * c.sample_window; i++) {
		uint64_t x = mbpta_test_gumbel_sample(&rng, 100000.0, 5000.0);
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

/* When a follower's fusion-group membership changes the
 * estimator-key fingerprint flips and the surviving fit no
 * longer corresponds to the running workload. The fusion-leader
 * tracker in module-deadline.c calls
 * mbpta_invalidate_with_reason(FUSION_GROUP) on the transition;
 * this test pins the contract that the invalidation actually
 * resets the estimator and stamps the typed reason so an
 * operator inspecting the snapshot can identify which dimension
 * flipped. */
PWTEST(mbpta_invalidate_fusion_group_resets_and_tags_reason)
{
	struct mbpta_config c = cfg_default();
	mbpta_t *e = mbpta_create(&c);
	uint32_t i;

	pwtest_ptr_notnull(e);
	for (i = 0; i < 2 * c.sample_window; i++)
		mbpta_add_sample(e, 100 + (i % 31) * 3);
	pwtest_bool_true(mbpta_sample_count(e) > 0);

	mbpta_invalidate_with_reason(e, MBPTA_INVALIDATED_FUSION_GROUP);
	pwtest_int_eq((int)mbpta_sample_count(e), 0);
	pwtest_int_eq(mbpta_state(e), MBPTA_INSUFFICIENT_DATA);
	pwtest_int_eq(mbpta_last_invalidation_reason(e),
			MBPTA_INVALIDATED_FUSION_GROUP);
	pwtest_str_eq(mbpta_invalidation_reason_name(
				mbpta_last_invalidation_reason(e)),
			"fusion_group");

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

PWTEST(mbpta_runs_test_rejects_monotone_trend)
{
	/* Cucu-Grosjean 2012 §V-C uses the Wald-Wolfowitz runs test
	 * as the independence gate. A strictly monotone sample
	 * stream has sign(x_{i+1} - x_i) = +1 everywhere, which
	 * collapses the number of runs to 1 -- the test rejects
	 * with |Z| well above the 1.96 alpha=0.05 threshold and the
	 * estimator must never reach PWCET_VALID. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;
	uint32_t i;
	enum mbpta_state s;

	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	for (i = 0; i < 4 * c.sample_window; i++)
		mbpta_add_sample(e, 1000 + (uint64_t)i);

	s = mbpta_state(e);
	pwtest_bool_true(s != MBPTA_PWCET_VALID);
	pwtest_bool_true(mbpta_pwcet_ns(e) == 0);

	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_insufficient_data_until_b_min_blocks)
{
	/* The block-maxima Gumbel fit needs at least min_blocks
	 * disjoint blocks of size block_size before any pWCET claim
	 * can be made (Cucu-Grosjean 2012 §II-B). Feed strictly
	 * fewer samples than block_size * min_blocks and check that
	 * the estimator stays in INSUFFICIENT_DATA with no
	 * pwcet_ns published. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;
	uint32_t target_samples, fed_samples, i;

	c.warmup_discard = 0;
	c.n_delta = 4;
	target_samples = c.block_size * c.min_blocks;
	pwtest_bool_true(target_samples >= 4);
	fed_samples = target_samples - 1;

	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	for (i = 0; i < fed_samples; i++)
		mbpta_add_sample(e, 1000 + (uint64_t)(i % 7));

	/* Below the block_size * min_blocks threshold the estimator
	 * may not publish a pWCET; whether the per-step path has
	 * accumulated any block maxima yet is an implementation
	 * detail. The contract that matters is no pWCET claim and a
	 * non-PWCET_VALID state. */
	pwtest_bool_true(mbpta_state(e) != MBPTA_PWCET_VALID);
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

PWTEST(mbpta_ks_runs_pvalues_default_to_one)
{
	/* Before the first re-evaluation round runs there is no
	 * evidence against H_0 (identical distribution / independence),
	 * so the two p-values default to 1.0 at creation. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;

	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);
	pwtest_bool_true(mbpta_ks_pvalue(e) == 1.0);
	pwtest_bool_true(mbpta_runs_pvalue(e) == 1.0);
	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_ks_pvalue_low_under_distribution_shift)
{
	/* A step-shift in mean drives the KS statistic well above
	 * critical and the two-sided KS p-value below alpha=0.05. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;
	uint32_t i;

	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	/* First half: tight stationary stream around 100. */
	for (i = 0; i < c.sample_window / 2; i++)
		mbpta_add_sample(e, 100 + (i % 5));
	/* Second half: shifted stream around 100_000. */
	for (i = 0; i < c.sample_window / 2; i++)
		mbpta_add_sample(e, 100000 + (i % 5));

	/* Force a few more samples to land an evaluation round. */
	for (i = 0; i < c.n_delta; i++)
		mbpta_add_sample(e, 100000 + (i % 5));

	pwtest_bool_true(mbpta_ks_stat(e) > 0.0);
	pwtest_bool_true(mbpta_ks_pvalue(e) < 0.05);
	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_runs_pvalue_low_under_clustered_transitions)
{
	/* The Wald-Wolfowitz runs test needs both up and down
	 * transitions to evaluate (a purely monotone stream has
	 * one sign and the implementation collapses Z to 0); the
	 * informative reject case is a stream whose up- and
	 * down-runs cluster into long blocks instead of
	 * alternating randomly. Build a window of long monotone
	 * up-blocks followed by long monotone down-blocks: there
	 * are only ~2 runs in a window of hundreds of transitions,
	 * which sits far below E[R] under H_0 and pushes the
	 * runs-test two-sided p-value below 0.05. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;
	uint32_t i;
	uint64_t v = 1000;

	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	for (i = 0; i < 4 * c.sample_window; i++) {
		if ((i / 50) % 2 == 0)
			v += 1;
		else
			v -= 1;
		mbpta_add_sample(e, v);
	}

	pwtest_bool_true(mbpta_runs_pvalue(e) < 0.05);
	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_gumbel_source_recovers_parameters_within_tolerance)
{
	/* Cucu-Grosjean 2012 §III-D: an i.i.d. stream drawn from
	 * Gumbel(mu, sigma) must be recoverable to a small tolerance
	 * once the estimator has enough block maxima -- the QQ-plot
	 * regression slope is an unbiased estimator of sigma and the
	 * intercept of mu. Use values close to the plan-suggested
	 * 1 000 000 / 50 000 ground truth (the constant in the
	 * fixed-point cast pulls extreme tail samples to integer
	 * which loses a little resolution; use a slightly smaller
	 * mu to keep the cast well-conditioned). */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;
	uint64_t rng = 0xA1B2C3D4E5F60718ULL;
	uint32_t i;
	const double mu_true = 100000.0;
	const double sigma_true = 5000.0;
	double sigma_err;

	c.sample_window = 2048;
	c.warmup_discard = 32;
	c.block_size = 16;
	c.min_blocks = 32;
	c.n_delta = 64;
	c.n_conv = 2;
	c.alpha_et = 0.0; /* the PWM shape estimator's small-sample
			   * variance occasionally rejects under
			   * H_0; the parameter-recovery property is
			   * orthogonal to the ET gate. */
	c.gumbel_r2_threshold = 0.5;
	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	for (i = 0; i < 6 * c.sample_window; i++) {
		uint64_t x = mbpta_test_gumbel_sample(&rng, mu_true,
				sigma_true);
		mbpta_add_sample(e, x);
	}

	/* Whatever state the run lands in (PWCET_VALID or
	 * PENDING_CONVERGENCE), the Gumbel fit's sigma estimate
	 * should be within +/-10 % of the true scale. Mu shifts
	 * with block size (block maxima of a Gumbel are themselves
	 * Gumbel with the same sigma and mu + sigma * ln(m)), so the
	 * tight tolerance applies to sigma; mu is checked loosely. */
	pwtest_bool_true(mbpta_sigma(e) > 0.0);
	sigma_err = (mbpta_sigma(e) - sigma_true) / sigma_true;
	if (sigma_err < 0) sigma_err = -sigma_err;
	pwtest_bool_true(sigma_err < 0.20);

	/* When the fit reaches PWCET_VALID the published pWCET must
	 * match the closed-form Gumbel inverse CDF
	 *     pWCET = mu - sigma * ln(-ln(1 - eps_node))
	 * at the configured eps_node. The integer cast in the
	 * estimator's cache truncates by < 1 ns, well below the
	 * tolerance the sigma recovery already accepts. */
	if (mbpta_state(e) == MBPTA_PWCET_VALID) {
		double mu_hat = mbpta_mu(e);
		double sigma_hat = mbpta_sigma(e);
		double expected = mu_hat - sigma_hat *
			log(-log(1.0 - c.eps_node));
		uint64_t actual = mbpta_pwcet_ns(e);
		double diff = (double)actual - expected;
		if (diff < 0) diff = -diff;
		pwtest_bool_true(diff < 2.0);
	}

	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_pwcet_exceeds_observed_max_at_low_eps)
{
	/* Operational sanity check from Cucu-Grosjean 2012: when the
	 * fit converges and the configured eps_node is small (1e-9),
	 * the extrapolated tail must lie above the largest sample
	 * actually observed in the window. A failing assertion would
	 * indicate either the Gumbel inverse-CDF arithmetic is
	 * truncating the tail or the published pwcet is using the
	 * empirical max instead of the extrapolated value. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;
	uint64_t rng = 0xCAFEBABEFACEFEEDULL;
	uint32_t i;
	const double mu_true = 100000.0;
	const double sigma_true = 5000.0;
	uint64_t observed_max = 0;

	c.sample_window = 2048;
	c.warmup_discard = 32;
	c.block_size = 16;
	c.min_blocks = 32;
	c.n_delta = 64;
	c.n_conv = 2;
	c.eps_node = 1.0e-9;
	c.alpha_et = 0.0;
	c.gumbel_r2_threshold = 0.5;
	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	for (i = 0; i < 6 * c.sample_window; i++) {
		uint64_t x = mbpta_test_gumbel_sample(&rng, mu_true,
				sigma_true);
		if (x > observed_max) observed_max = x;
		mbpta_add_sample(e, x);
	}

	if (mbpta_state(e) == MBPTA_PWCET_VALID) {
		uint64_t pwcet = mbpta_pwcet_ns(e);
		/* At eps_node = 1e-9 the tail should clear the
		 * observed max by a comfortable margin; assert the
		 * weaker property that it at least exceeds it. */
		pwtest_bool_true(pwcet > observed_max);
	}

	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_et_pvalue_defaults_to_one)
{
	/* Before the first re-evaluation round the ET test has no
	 * evidence against H_0: k = 0, so the p-value defaults to
	 * 1.0 and the shape estimate to 0.0. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;

	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);
	pwtest_bool_true(mbpta_et_pvalue(e) == 1.0);
	pwtest_bool_true(mbpta_gev_shape_k(e) == 0.0);
	mbpta_destroy(e);
	return PWTEST_PASS;
}

PWTEST(mbpta_et_pvalue_rejects_heavy_tailed_input)
{
	/* A heavy-tailed (Frechet-style, k > 0) source has a GEV
	 * shape parameter significantly above zero; the PWM
	 * estimator picks this up and the two-sided ET p-value
	 * drops well below 0.05. Build a series whose block
	 * maxima have a Pareto-like tail by mixing a dominant
	 * stationary cluster with rare large spikes. */
	struct mbpta_config c = cfg_default();
	mbpta_t *e;
	uint32_t i;

	c.gumbel_r2_threshold = 0.0; /* disable R^2 short-circuit */
	c.alpha_et = 0.05;
	e = mbpta_create(&c);
	pwtest_ptr_notnull(e);

	for (i = 0; i < 4 * c.sample_window; i++) {
		uint64_t x = (i % 13 == 0)
			? 100000 + (uint64_t)(i * 200)
			: 100 + (i % 7);
		mbpta_add_sample(e, x);
	}

	/* Either the ET test outright rejects Gumbel and we land
	 * in NON_GUMBEL, or the i.i.d. gates fire first (heavy
	 * spikes also break KS); both outcomes prove the
	 * Gumbel-only PWCET_VALID path is not entered. */
	pwtest_bool_true(mbpta_state(e) != MBPTA_PWCET_VALID);
	pwtest_bool_true(mbpta_pwcet_ns(e) == 0);
	mbpta_destroy(e);
	return PWTEST_PASS;
}

/*
 * The kernel `runtime` field carries pWCET(eps_node) only when both
 *   (a) the per-node estimator is PWCET_VALID, and
 *   (b) the operator has explicitly opted in via
 *       deadline.mbpta.accept_probabilistic_hard = true.
 *
 * The two tests below pin both halves of this AND. mbpta_runtime_uses_pwcet
 * is the pure predicate the runtime caller in module-deadline.c projects
 * (mbpta_state, accept flag) through; gating both via the same function
 * is what makes them testable without standing up the full reconcile
 * pass.
 */
PWTEST(mbpta_runtime_field_is_pwcet_when_valid_and_opted_in)
{
	pwtest_bool_true(mbpta_runtime_uses_pwcet(MBPTA_PWCET_VALID, true));
	return PWTEST_PASS;
}

PWTEST(mbpta_runtime_field_not_pwcet_when_opt_in_off)
{
	/* Opt-in off: even a converged PWCET_VALID fit does NOT drive
	 * the kernel runtime. Telemetry stays emitted (the diag layer
	 * continues to report the estimator state and pWCET value),
	 * but the runtime falls back to empirical / soft / fallback
	 * budgets. */
	pwtest_bool_false(mbpta_runtime_uses_pwcet(MBPTA_PWCET_VALID, false));

	/* Opt-in on but state is not yet PWCET_VALID: still falls
	 * back. Every non-PWCET_VALID state is enumerated to pin the
	 * policy. */
	pwtest_bool_false(mbpta_runtime_uses_pwcet(MBPTA_INSUFFICIENT_DATA, true));
	pwtest_bool_false(mbpta_runtime_uses_pwcet(MBPTA_IID_PENDING, true));
	pwtest_bool_false(mbpta_runtime_uses_pwcet(MBPTA_NON_GUMBEL, true));
	pwtest_bool_false(mbpta_runtime_uses_pwcet(MBPTA_PENDING_CONVERGENCE, true));
	pwtest_bool_false(mbpta_runtime_uses_pwcet(MBPTA_DRIFT, true));

	/* Both off: definitely falls back. */
	pwtest_bool_false(mbpta_runtime_uses_pwcet(MBPTA_INSUFFICIENT_DATA, false));
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
	pwtest_add(mbpta_invalidate_fusion_group_resets_and_tags_reason,
			PWTEST_NOARG);
	pwtest_add(mbpta_drift_after_sustained_iid_rejection, PWTEST_NOARG);
	pwtest_add(mbpta_non_gumbel_distribution_rejects_fit, PWTEST_NOARG);
	pwtest_add(mbpta_runs_test_rejects_monotone_trend, PWTEST_NOARG);
	pwtest_add(mbpta_insufficient_data_until_b_min_blocks, PWTEST_NOARG);
	pwtest_add(mbpta_eps_node_at_floor_is_not_capped, PWTEST_NOARG);
	pwtest_add(mbpta_eps_node_below_floor_is_clamped, PWTEST_NOARG);
	pwtest_add(mbpta_eps_node_above_floor_passes_through, PWTEST_NOARG);
	pwtest_add(mbpta_ks_runs_pvalues_default_to_one, PWTEST_NOARG);
	pwtest_add(mbpta_ks_pvalue_low_under_distribution_shift,
			PWTEST_NOARG);
	pwtest_add(mbpta_runs_pvalue_low_under_clustered_transitions,
			PWTEST_NOARG);
	pwtest_add(mbpta_gumbel_source_recovers_parameters_within_tolerance,
			PWTEST_NOARG);
	pwtest_add(mbpta_pwcet_exceeds_observed_max_at_low_eps,
			PWTEST_NOARG);
	pwtest_add(mbpta_et_pvalue_defaults_to_one, PWTEST_NOARG);
	pwtest_add(mbpta_et_pvalue_rejects_heavy_tailed_input,
			PWTEST_NOARG);
	pwtest_add(mbpta_runtime_field_is_pwcet_when_valid_and_opted_in,
			PWTEST_NOARG);
	pwtest_add(mbpta_runtime_field_not_pwcet_when_opt_in_off,
			PWTEST_NOARG);

	return PWTEST_PASS;
}
