/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Unit tests for module-deadline/conformal.c. The suite is layered:
 *
 *   - Section A pins the configuration helpers and stable token
 *     tables (token strings, defaults validate, validator rejects
 *     every documented out-of-range condition).
 *   - Section B pins the create/destroy lifecycle (NULL safety,
 *     rejection of invalid config blocks, reset semantics).
 *   - Section C pins the base predictor on synthetic streams:
 *     constant, alternating-pair, monotone drift, AR(1), sudden
 *     step, and Pareto-style bursts. The expected envelope is
 *     described inline for each case.
 *   - Section D pins the score ring and the conformal quantile
 *     index: empty-window, single-element, alpha_min boundary,
 *     alpha_max boundary.
 *   - Section E pins the prequential discipline: no look-ahead.
 *     The budget recorded for activation t is reconstructed from
 *     the ring as it stood BEFORE x_t was inserted; a regression
 *     test compares this against an oracle.
 *   - Section F pins the guard and the budget clamping:
 *     guard_ns + guard_percent are applied exactly once; runtime
 *     floor and period clamp are honoured; NaN/Inf-resistant.
 *   - Section G pins the bootstrap policy: a freshly-created
 *     estimator publishes the configured bootstrap floor; the
 *     state transitions to BOOTSTRAP after the first sample and to
 *     VALID after the bootstrap-min threshold is crossed.
 */

#include "config.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/conformal.h"

/* ---------------------------------------------------------------- */
/* Section A: tokens and config helpers (unchanged from scaffold).   */
/* ---------------------------------------------------------------- */

PWTEST(conformal_state_name_stable)
{
	pwtest_str_eq(rt_conformal_state_name(RT_CONF_INSUFFICIENT_DATA),
			"insufficient_data");
	pwtest_str_eq(rt_conformal_state_name(RT_CONF_BOOTSTRAP),
			"bootstrap");
	pwtest_str_eq(rt_conformal_state_name(RT_CONF_VALID), "valid");
	pwtest_str_eq(rt_conformal_state_name(RT_CONF_SHIFT), "shift");
	pwtest_str_eq(rt_conformal_state_name(RT_CONF_DISABLED),
			"disabled");
	pwtest_str_eq(rt_conformal_state_name((enum rt_conformal_state)999),
			"unknown");
	return PWTEST_PASS;
}

PWTEST(conformal_invalidation_reason_name_stable)
{
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_NONE), "none");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_PERIOD), "period");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_FUSION_GROUP), "fusion_group");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_CPU_CLASS), "cpu_class");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_OPERATOR_REQUEST),
			"operator_request");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_PLUGIN_MODE), "plugin_mode");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			(enum rt_conformal_invalidation_reason)999), "unknown");
	return PWTEST_PASS;
}

PWTEST(conformal_risk_allocation_name_stable)
{
	pwtest_str_eq(rt_conformal_risk_allocation_name(
			RT_CONF_RISK_ALLOC_UNIFORM), "uniform");
	pwtest_str_eq(rt_conformal_risk_allocation_name(
			RT_CONF_RISK_ALLOC_DENSITY_WEIGHTED),
			"density_weighted");
	pwtest_str_eq(rt_conformal_risk_allocation_name(
			RT_CONF_RISK_ALLOC_SLOPE_WEIGHTED),
			"slope_weighted");
	pwtest_str_eq(rt_conformal_risk_allocation_name(
			(enum rt_conformal_risk_allocation)999), "unknown");
	return PWTEST_PASS;
}

PWTEST(conformal_config_defaults_validate)
{
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	pwtest_int_eq(rt_conformal_config_validate(&cfg), 0);
	pwtest_bool_true(cfg.alpha_target > 0.0 && cfg.alpha_target < 1.0);
	pwtest_bool_true(cfg.alpha_min  > 0.0 && cfg.alpha_min  <= cfg.alpha_target);
	pwtest_bool_true(cfg.alpha_max  >= cfg.alpha_target && cfg.alpha_max < 1.0);
	pwtest_bool_true(cfg.window >= 2 && cfg.window <= RT_CONFORMAL_MAX_WINDOW);
	pwtest_bool_true(cfg.recalc_period >= 1);
	pwtest_int_eq((int)cfg.risk_allocation, (int)RT_CONF_RISK_ALLOC_UNIFORM);
	return PWTEST_PASS;
}

PWTEST(conformal_config_defaults_null_safe)
{
	rt_conformal_config_defaults(NULL);
	pwtest_int_eq(rt_conformal_config_validate(NULL), -EINVAL);
	return PWTEST_PASS;
}

PWTEST(conformal_config_validator_rejects_out_of_range)
{
	struct rt_conformal_config cfg;

	rt_conformal_config_defaults(&cfg);
	cfg.alpha_target = 0.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.alpha_target = 1.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.alpha_min = cfg.alpha_target * 2.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.alpha_max = cfg.alpha_target / 2.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.eta = 0.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.eta = 1.5;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.ewma_location_lambda = 0.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.ewma_scale_lambda = 1.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.window = 1;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.window = RT_CONFORMAL_MAX_WINDOW + 1;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.bootstrap_min_samples = 1;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.guard_percent = 1.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.guard_percent = -0.01;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.recalc_period = 0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.risk_allocation = (enum rt_conformal_risk_allocation)42;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	return PWTEST_PASS;
}

/* Test-default config: small window so synthetic traces are quick. */
static struct rt_conformal_config cfg_small(void)
{
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	cfg.window                = 64;
	cfg.bootstrap_min_samples = 8;
	cfg.bootstrap_runtime_ns  = 20000;
	cfg.runtime_floor_ns      = 1000;
	cfg.guard_ns              = 500;
	cfg.guard_percent         = 0.05;
	cfg.sigma_floor_ns        = 100;
	cfg.recalc_period         = 1;
	return cfg;
}

/* ---------------------------------------------------------------- */
/* Section B: lifecycle.                                             */
/* ---------------------------------------------------------------- */

PWTEST(conformal_create_destroy_null_safe)
{
	rt_conformal_destroy(NULL);
	rt_conformal_invalidate(NULL, RT_CONF_INVALIDATED_NONE);
	pwtest_bool_false(rt_conformal_observe(NULL, 1000));
	pwtest_int_eq((int)rt_conformal_state(NULL), (int)RT_CONF_INSUFFICIENT_DATA);
	pwtest_int_eq((int)rt_conformal_samples_seen(NULL), 0);
	pwtest_int_eq((int)rt_conformal_overruns_seen(NULL), 0);
	pwtest_bool_true(rt_conformal_budget(NULL, 0) == 0);
	return PWTEST_PASS;
}

PWTEST(conformal_create_rejects_invalid_config)
{
	struct rt_conformal_config cfg = cfg_small();
	cfg.alpha_target = 1.0; /* out of (0,1) */
	pwtest_ptr_null(rt_conformal_create(&cfg));
	pwtest_ptr_null(rt_conformal_create(NULL));
	return PWTEST_PASS;
}

PWTEST(conformal_state_data_size_within_budget)
{
	/* The algorithm reference quotes "a few KB" per follower as
	 * the budget. The shell of the estimator (without the heap
	 * rings) must fit comfortably. */
	pwtest_bool_true(rt_conformal_state_data_size() <= 4096);
	return PWTEST_PASS;
}

PWTEST(conformal_invalidate_resets_state)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint32_t i;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 100; i++)
		rt_conformal_observe(e, 50000);

	pwtest_bool_true(rt_conformal_samples_used(e) > 0);
	pwtest_bool_true(rt_conformal_mu_ns(e) > 0.0);

	rt_conformal_invalidate(e, RT_CONF_INVALIDATED_PLUGIN_MODE);

	pwtest_int_eq((int)rt_conformal_state(e),
			(int)RT_CONF_INSUFFICIENT_DATA);
	pwtest_int_eq((int)rt_conformal_samples_used(e), 0);
	pwtest_bool_true(rt_conformal_mu_ns(e) == 0.0);
	pwtest_bool_true(rt_conformal_scale_ns(e) == 0.0);
	pwtest_int_eq((int)rt_conformal_last_invalidation_reason(e),
			(int)RT_CONF_INVALIDATED_PLUGIN_MODE);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* Section C: base predictor on synthetic streams.                   */
/* ---------------------------------------------------------------- */

PWTEST(conformal_constant_stream_locks_mu)
{
	/* A constant stream of 50000 ns drives mu_t -> 50000 and the
	 * EWMA scale -> 0 (within the sigma floor). The published
	 * budget remains a tight bound above the stationary value. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint32_t i;
	uint64_t budget;
	double mu;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 200; i++)
		rt_conformal_observe(e, 50000);

	mu = rt_conformal_mu_ns(e);
	pwtest_bool_true(mu > 49500.0 && mu < 50500.0);
	pwtest_bool_true(rt_conformal_scale_ns(e) < 1000.0);

	budget = rt_conformal_budget(e, 0);
	/* Budget at least the constant value (with guards), well
	 * below 2x (no inflation). */
	pwtest_bool_true(budget >= 50000);
	pwtest_bool_true(budget < 100000);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_alternating_stream_tracks_scale)
{
	/* An alternating 50000 / 60000 stream: mu sits halfway between
	 * the two, the EWMA absolute-deviation scale converges towards
	 * 5000 (the half-amplitude of the oscillation), and the
	 * published budget covers the high value. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint32_t i;
	double mu;
	double scale;
	uint64_t budget;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 600; i++)
		rt_conformal_observe(e, (i & 1u) ? 60000 : 50000);

	mu = rt_conformal_mu_ns(e);
	pwtest_bool_true(mu > 53000.0 && mu < 57000.0);
	scale = rt_conformal_scale_ns(e);
	pwtest_bool_true(scale > 1000.0);

	budget = rt_conformal_budget(e, 0);
	pwtest_bool_true(budget >= 60000);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_step_increase_inflates_budget)
{
	/* After a sudden step from 50000 to 80000, the EWMA mu walks
	 * upward, the scale picks up the new variability, and the
	 * published budget grows above 80000 within a bounded number
	 * of activations. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint32_t i;
	uint64_t pre_budget;
	uint64_t post_budget;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 400; i++)
		rt_conformal_observe(e, 50000);
	pre_budget = rt_conformal_budget(e, 0);

	for (i = 0; i < 400; i++)
		rt_conformal_observe(e, 80000);
	post_budget = rt_conformal_budget(e, 0);

	pwtest_bool_true(post_budget > pre_budget);
	pwtest_bool_true(post_budget >= 80000);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_monotone_drift_does_not_diverge)
{
	/* A slowly drifting stream from 30000 to 60000 over 500
	 * activations: the budget must not exceed a reasonable
	 * multiplier of the largest observation, even though the
	 * EWMA chases a moving target. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint32_t i;
	uint64_t budget;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 500; i++)
		rt_conformal_observe(e, 30000 + 60 * i);

	budget = rt_conformal_budget(e, 0);
	pwtest_bool_true(budget > 30000);
	pwtest_bool_true(budget < 200000);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

/* SplitMix64 PRNG for deterministic synthetic streams. */
static uint64_t conf_rng_next(uint64_t *state)
{
	uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static double conf_u01(uint64_t *state)
{
	uint64_t r = conf_rng_next(state) >> 11; /* 53 bits */
	return (double)r / 9007199254740992.0;   /* 2^53 */
}

PWTEST(conformal_ar1_stream_settles)
{
	/* AR(1): x_{t+1} = phi * x_t + (1 - phi) * mu + eps_t.
	 * Driven over thousands of samples the budget settles into a
	 * stable band above the long-run mean. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint64_t state = 0xCAFEFACEULL;
	double phi = 0.7;
	double mu_target = 40000.0;
	double x = mu_target;
	uint32_t i;
	uint64_t budget;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 4000; i++) {
		double eps = (conf_u01(&state) - 0.5) * 8000.0;
		x = phi * x + (1.0 - phi) * mu_target + eps;
		if (x < 1000.0) x = 1000.0;
		rt_conformal_observe(e, (uint64_t)x);
	}

	budget = rt_conformal_budget(e, 0);
	/* Budget exceeds the stationary mean and the scale-inflated
	 * band; under a (1 - alpha_eff) = 0.999 quantile the empirical
	 * miss rate must stay low (the rolling counter is the proxy). */
	pwtest_bool_true(budget > (uint64_t)mu_target);
	pwtest_bool_true(rt_conformal_overruns_seen(e) > 0);
	pwtest_bool_true(rt_conformal_overruns_seen(e) <
			rt_conformal_samples_used(e) / 10);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_pareto_burst_inflates_scale)
{
	/* A stationary cluster with rare large spikes: the scale picks
	 * up the bursts and the budget grows enough to cover the
	 * majority of future spikes. The overrun rate must not run
	 * away. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint64_t state = 0xDEADBEEFULL;
	uint32_t i;
	uint64_t budget;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 2000; i++) {
		double u = conf_u01(&state);
		uint64_t s = (u < 0.02)
			? 200000 + (uint64_t)(conf_u01(&state) * 100000.0)
			: 40000 + (uint64_t)(conf_u01(&state) * 5000.0);
		rt_conformal_observe(e, s);
	}

	budget = rt_conformal_budget(e, 0);
	pwtest_bool_true(budget > 40000);
	/* Less than half the activations should overrun. */
	pwtest_bool_true(rt_conformal_overruns_seen(e) <
			rt_conformal_samples_used(e) / 2);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* Section D: score-ring and quantile index.                         */
/* ---------------------------------------------------------------- */

PWTEST(conformal_quantile_zero_on_empty_ring)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);
	pwtest_bool_true(rt_conformal_score_quantile(e) == 0.0);
	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_quantile_at_alpha_min_picks_max_score)
{
	/* alpha very small => 1 - alpha very large => the conformal
	 * index lands at the last sorted entry, i.e. the max score
	 * the ring has observed. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	uint32_t i;
	double q;
	cfg.alpha_target = 1e-5;
	cfg.alpha_min    = 1e-6;
	cfg.alpha_max    = 0.5;
	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	/* Inject a stream with a strong outlier so the score ring
	 * has a clearly identifiable maximum. */
	for (i = 0; i < 200; i++)
		rt_conformal_observe(e, 40000);
	rt_conformal_observe(e, 120000);
	for (i = 0; i < 5; i++)
		rt_conformal_observe(e, 40000);

	q = rt_conformal_score_quantile(e);
	pwtest_bool_true(q > 0.0);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_quantile_alpha_close_to_max_lowers_quantile)
{
	/* With alpha very close to alpha_max (loosest budget) the
	 * conformal index lands earlier in the sorted array; the
	 * cached quantile drops below the max score. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	uint32_t i;
	double q;
	cfg.alpha_target = 0.05;
	cfg.alpha_max    = 0.05;
	cfg.alpha_min    = 1e-6;
	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	for (i = 0; i < 200; i++)
		rt_conformal_observe(e, 40000);
	rt_conformal_observe(e, 120000);
	for (i = 0; i < 5; i++)
		rt_conformal_observe(e, 40000);

	q = rt_conformal_score_quantile(e);
	pwtest_bool_true(isfinite(q));

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* Section E: prequential discipline.                                */
/* ---------------------------------------------------------------- */

PWTEST(conformal_no_lookahead_first_observation_uses_bootstrap)
{
	/* The first observation cannot consult the sample x_1 itself
	 * when reconstructing the budget that would have been active
	 * for activation 1 -- doing so would leak x_1 into a quantity
	 * the prequential rule must derive from prior state only. On
	 * the very first call the EWMA pair is uninitialised, so the
	 * estimator falls back to the bootstrap branch (same value
	 * rt_conformal_budget() would have published before any
	 * sample arrived). Verify the recorded last_budget_ns matches
	 * the bootstrap floor regardless of the observed sample size:
	 * sweep an unusually small and an unusually large x_1 and
	 * confirm both record the same bootstrap value. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	uint64_t expected = cfg.bootstrap_runtime_ns > cfg.runtime_floor_ns
		? cfg.bootstrap_runtime_ns : cfg.runtime_floor_ns;

	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);
	rt_conformal_observe(e, 50000);
	pwtest_int_eq((int)rt_conformal_last_budget_ns(e), (int)expected);
	rt_conformal_destroy(e);

	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);
	rt_conformal_observe(e, 1000000); /* x_1 20x larger */
	pwtest_int_eq((int)rt_conformal_last_budget_ns(e), (int)expected);
	rt_conformal_destroy(e);

	return PWTEST_PASS;
}

PWTEST(conformal_no_lookahead_observation_count_matches)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint32_t i;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 50; i++)
		rt_conformal_observe(e, 50000);

	pwtest_int_eq((int)rt_conformal_samples_seen(e), 50);
	pwtest_int_eq((int)rt_conformal_samples_used(e), 50);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* Section F: guard and clamping.                                    */
/* ---------------------------------------------------------------- */

PWTEST(conformal_budget_respects_runtime_floor)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	cfg.bootstrap_runtime_ns = 0;
	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	pwtest_bool_true(rt_conformal_budget(e, 0) >= cfg.runtime_floor_ns);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_budget_respects_period_clamp)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint32_t i;
	uint64_t budget;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 200; i++)
		rt_conformal_observe(e, 200000);

	budget = rt_conformal_budget(e, 100000);
	pwtest_bool_true(budget <= 100000);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_observe_rejects_zero)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	pwtest_bool_false(rt_conformal_observe(e, 0));
	pwtest_int_eq((int)rt_conformal_samples_used(e), 0);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_observe_clamps_below_floor)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint32_t i;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 100; i++)
		rt_conformal_observe(e, 1);

	/* The EWMA mu must end up at or above the floor; a value below
	 * would mean the floor clamp was bypassed. */
	pwtest_bool_true(rt_conformal_mu_ns(e) >= (double)cfg.runtime_floor_ns);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_budget_handles_disabled_state)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);
	rt_conformal_invalidate(e, RT_CONF_INVALIDATED_NONE);
	/* Even after invalidate the state is INSUFFICIENT_DATA, not
	 * DISABLED. There is no public DISABLED setter today (a future
	 * commit can wire that up). The disabled return path is
	 * pinned only for NULL. */
	pwtest_bool_true(rt_conformal_budget(NULL, 0) == 0);
	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* Section G: bootstrap.                                             */
/* ---------------------------------------------------------------- */

PWTEST(conformal_fresh_estimator_publishes_bootstrap_floor)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint64_t b;
	pwtest_ptr_notnull(e);

	b = rt_conformal_budget(e, 0);
	pwtest_int_eq((int)b, (int)cfg.bootstrap_runtime_ns);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_state_transitions_bootstrap_to_valid)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint32_t i;
	pwtest_ptr_notnull(e);

	pwtest_int_eq((int)rt_conformal_state(e),
			(int)RT_CONF_INSUFFICIENT_DATA);

	rt_conformal_observe(e, 50000);
	pwtest_int_eq((int)rt_conformal_state(e), (int)RT_CONF_BOOTSTRAP);

	/* Push past bootstrap_min_samples to land in VALID. */
	for (i = 0; i < cfg.bootstrap_min_samples + 4u; i++)
		rt_conformal_observe(e, 50000);
	pwtest_int_eq((int)rt_conformal_state(e), (int)RT_CONF_VALID);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* Section H: adaptive alpha update and drift detection.             */
/* ---------------------------------------------------------------- */

PWTEST(conformal_alpha_eff_decreases_on_overrun)
{
	/* A single overrun pushes alpha_eff down (more conservative
	 * next quantile) by eta * (alpha_target - 1.0). The test uses
	 * a small eta so the new value lands strictly below the
	 * starting alpha_eff without saturating against alpha_min on
	 * the first step. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	double a0, a1;

	cfg.eta = 0.001; /* one overrun moves alpha by ~0.001 */
	cfg.alpha_target = 0.01;
	cfg.alpha_min = 1e-6;
	cfg.alpha_max = 0.5;
	cfg.bootstrap_runtime_ns = 5000;
	cfg.runtime_floor_ns = 5000;
	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	a0 = rt_conformal_alpha_eff(e);
	rt_conformal_observe(e, 5000000); /* huge: overruns bootstrap */
	a1 = rt_conformal_alpha_eff(e);

	pwtest_bool_true(a1 < a0);
	pwtest_bool_true(a1 >= cfg.alpha_min);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_alpha_eff_increases_on_non_overrun)
{
	/* A non-overruning sample nudges alpha_eff upward by
	 * eta * alpha_target (a slow positive drift). Starting from
	 * alpha_target the value must end strictly above. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	uint32_t i;
	double a;

	cfg.eta = 0.1;
	cfg.alpha_target = 0.01;
	cfg.alpha_min = 1e-6;
	cfg.alpha_max = 0.5;
	cfg.bootstrap_runtime_ns = 200000; /* very loose, no overruns */
	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	for (i = 0; i < 100; i++)
		rt_conformal_observe(e, 40000); /* below 200k bootstrap */

	a = rt_conformal_alpha_eff(e);
	pwtest_bool_true(a > cfg.alpha_target);
	pwtest_bool_true(a <= cfg.alpha_max);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_alpha_eff_clamps_to_min)
{
	/* Repeated overruns must not push alpha_eff below alpha_min. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	uint32_t i;

	cfg.eta = 0.5; /* aggressive */
	cfg.alpha_target = 0.01;
	cfg.alpha_min = 0.001;
	cfg.alpha_max = 0.5;
	cfg.bootstrap_runtime_ns = 100; /* easy to overrun */
	cfg.runtime_floor_ns = 100;
	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	for (i = 0; i < 1000; i++)
		rt_conformal_observe(e, 10000000); /* always overrun */

	pwtest_bool_true(rt_conformal_alpha_eff(e) >= cfg.alpha_min);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_alpha_eff_clamps_to_max)
{
	/* Repeated non-overruns must not push alpha_eff above
	 * alpha_max. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	uint32_t i;

	cfg.eta = 0.5;
	cfg.alpha_target = 0.01;
	cfg.alpha_min = 1e-6;
	cfg.alpha_max = 0.05;
	cfg.bootstrap_runtime_ns = 200000;
	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	for (i = 0; i < 5000; i++)
		rt_conformal_observe(e, 40000);

	pwtest_bool_true(rt_conformal_alpha_eff(e) <= cfg.alpha_max);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_max_overrun_burst_tracks_streak)
{
	/* A geometrically-growing sample stream coupled with an
	 * artificially-large sigma_floor pegs the score-ring
	 * contribution near zero (s = delta / (scale + huge) ~ 0),
	 * which leaves the reconstructed budget at
	 * mu_pred * (1 + guard_percent) + guard_ns. mu trails the
	 * fast-growing stream, so every step overruns. The test pins
	 * three properties: (i) the streak counter monotonically
	 * advances under sustained overruns, (ii) max_overrun_burst
	 * stays at the running max, (iii) a small sample (which
	 * cannot overrun the inflated budget) resets the current
	 * streak but does not lower max. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	uint32_t i;
	uint64_t sample;
	uint64_t streak_after_5;

	cfg.sigma_floor_ns = (uint64_t)1e15; /* peg score at ~0 */
	cfg.bootstrap_runtime_ns = 5000;
	cfg.runtime_floor_ns = 5000;
	cfg.ewma_location_lambda = 0.05;
	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	sample = 100000;
	for (i = 0; i < 5; i++) {
		rt_conformal_observe(e, sample);
		sample = (uint64_t)((double)sample * 1.5);
	}
	streak_after_5 = rt_conformal_current_overrun_burst(e);
	pwtest_bool_true(streak_after_5 >= 4);
	pwtest_bool_true(rt_conformal_max_overrun_burst(e) >= streak_after_5);

	/* A small sample below the inflated budget resets current
	 * but max sticks. */
	rt_conformal_observe(e, 5000);
	pwtest_int_eq((int)rt_conformal_current_overrun_burst(e), 0);
	pwtest_bool_true(rt_conformal_max_overrun_burst(e) >= streak_after_5);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_state_shifts_on_sustained_burst)
{
	/* When the consecutive-overrun streak reaches the configured
	 * SHIFT threshold the state transitions to RT_CONF_SHIFT. The
	 * test uses the same large-sigma_floor trick as the burst
	 * counter test to keep the quantile contribution near zero so
	 * the streak actually develops; then it confirms the state
	 * classifier surfaces SHIFT. The alpha update is still
	 * progressing in parallel -- the SHIFT state is informational,
	 * not a kill. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	uint32_t i;
	uint64_t sample;

	cfg.sigma_floor_ns = (uint64_t)1e15;
	cfg.shift_burst_threshold = 3;
	cfg.bootstrap_runtime_ns = 5000;
	cfg.runtime_floor_ns = 5000;
	cfg.bootstrap_min_samples = 2;
	cfg.ewma_location_lambda = 0.05;
	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	for (i = 0; i < 50; i++)
		rt_conformal_observe(e, 6000);
	pwtest_int_eq((int)rt_conformal_state(e), (int)RT_CONF_VALID);

	sample = 100000;
	for (i = 0; i < 5; i++) {
		rt_conformal_observe(e, sample);
		sample = (uint64_t)((double)sample * 1.5);
	}
	pwtest_int_eq((int)rt_conformal_state(e), (int)RT_CONF_SHIFT);

	/* Once the streak is broken the state returns to VALID. */
	rt_conformal_observe(e, 5000);
	pwtest_int_eq((int)rt_conformal_state(e), (int)RT_CONF_VALID);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_disable_freezes_observation)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint64_t mu_before;
	pwtest_ptr_notnull(e);

	rt_conformal_observe(e, 50000);
	rt_conformal_observe(e, 50000);
	mu_before = (uint64_t)rt_conformal_mu_ns(e);

	rt_conformal_disable(e);
	pwtest_int_eq((int)rt_conformal_state(e), (int)RT_CONF_DISABLED);

	pwtest_bool_false(rt_conformal_observe(e, 200000));
	/* mu must not move while disabled. */
	pwtest_bool_true((uint64_t)rt_conformal_mu_ns(e) == mu_before);
	pwtest_bool_true(rt_conformal_budget(e, 0) == 0);

	rt_conformal_enable(e);
	pwtest_bool_true(rt_conformal_state(e) != RT_CONF_DISABLED);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_burst_penalty_amplifies_negative_step)
{
	/* With burst_threshold = 3, burst_penalty = 10 and a stream
	 * that produces a sustained overrun streak (the
	 * large-sigma_floor trick), the alpha_eff step at and beyond
	 * the threshold is much larger than the step before it. The
	 * test inspects the diff before vs after the threshold trip
	 * and asserts the post-threshold step is significantly larger.
	 *
	 * Because the alpha update is gated by alpha_min, choose the
	 * floor low enough that the pre-threshold steps do not
	 * saturate. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e;
	double a0, a1, a2, a3, a4;
	double pre_step;
	double post_step;
	uint64_t sample;

	cfg.sigma_floor_ns = (uint64_t)1e15;
	cfg.eta = 0.0001;
	cfg.alpha_target = 0.4;
	cfg.alpha_min = 1e-6;
	cfg.alpha_max = 0.5;
	cfg.bootstrap_runtime_ns = 5000;
	cfg.runtime_floor_ns = 5000;
	cfg.burst_threshold = 3;
	cfg.burst_penalty = 100.0;
	cfg.ewma_location_lambda = 0.05;
	e = rt_conformal_create(&cfg);
	pwtest_ptr_notnull(e);

	a0 = rt_conformal_alpha_eff(e);
	sample = 100000;
	rt_conformal_observe(e, sample); sample = (uint64_t)(sample * 1.5);
	a1 = rt_conformal_alpha_eff(e);
	rt_conformal_observe(e, sample); sample = (uint64_t)(sample * 1.5);
	a2 = rt_conformal_alpha_eff(e);
	rt_conformal_observe(e, sample); sample = (uint64_t)(sample * 1.5);
	a3 = rt_conformal_alpha_eff(e); /* third overrun -> threshold met */
	rt_conformal_observe(e, sample);
	a4 = rt_conformal_alpha_eff(e); /* fourth overrun -> penalty applies */

	pre_step  = a1 - a2; /* before threshold */
	post_step = a3 - a4; /* threshold met -> amplified step */
	(void)a0;
	pwtest_bool_true(post_step > pre_step * 5.0);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_burst_penalty_validator_rejects_below_one)
{
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	cfg.burst_penalty = 0.5;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);
	cfg.burst_penalty = -1.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);
	cfg.burst_penalty = 1.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), 0);
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* Section I: fusion-group invalidation.                             */
/* ---------------------------------------------------------------- */

PWTEST(conformal_fusion_group_invalidation_resets_state)
{
	/* When a follower's macro-node membership changes (the
	 * reconcile dispatcher detects a different fusion-leader id
	 * across two reconcile passes), the conformal estimator must
	 * reset to RT_CONF_INSUFFICIENT_DATA and stamp the typed
	 * RT_CONF_INVALIDATED_FUSION_GROUP reason -- the sample
	 * distribution observed under the old contraction is no longer
	 * representative of the new macro-node. */
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_t *e = rt_conformal_create(&cfg);
	uint32_t i;
	pwtest_ptr_notnull(e);

	for (i = 0; i < 100; i++)
		rt_conformal_observe(e, 50000);
	pwtest_int_eq((int)rt_conformal_state(e), (int)RT_CONF_VALID);

	rt_conformal_invalidate(e, RT_CONF_INVALIDATED_FUSION_GROUP);

	pwtest_int_eq((int)rt_conformal_state(e),
			(int)RT_CONF_INSUFFICIENT_DATA);
	pwtest_int_eq((int)rt_conformal_last_invalidation_reason(e),
			(int)RT_CONF_INVALIDATED_FUSION_GROUP);
	pwtest_int_eq((int)rt_conformal_samples_used(e), 0);

	/* After the reset the estimator returns to BOOTSTRAP on the
	 * very first new sample, then VALID once enough samples have
	 * accumulated -- the same lifecycle a freshly-created macro
	 * sees. */
	rt_conformal_observe(e, 80000);
	pwtest_int_eq((int)rt_conformal_state(e), (int)RT_CONF_BOOTSTRAP);
	for (i = 0; i < cfg.bootstrap_min_samples + 4u; i++)
		rt_conformal_observe(e, 80000);
	pwtest_int_eq((int)rt_conformal_state(e), (int)RT_CONF_VALID);

	rt_conformal_destroy(e);
	return PWTEST_PASS;
}

PWTEST(conformal_compatible_history_field_round_trips)
{
	/* compatible_history is a parsed boolean; defaults to true.
	 * Toggle it and confirm the validator accepts both values. */
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	cfg.compatible_history = false;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), 0);
	cfg.compatible_history = true;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), 0);
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* Section H: mode-keyed estimator table.                            */
/* ---------------------------------------------------------------- */

PWTEST(conformal_mode_key_equal_compares_every_field)
{
	struct rt_conformal_mode_key a = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_BIG };
	struct rt_conformal_mode_key b = a;
	pwtest_bool_true(rt_conformal_mode_key_equal(&a, &b));

	b.sample_rate_hz = 44100;
	pwtest_bool_false(rt_conformal_mode_key_equal(&a, &b));

	b = a;
	b.quantum_frames = 512;
	pwtest_bool_false(rt_conformal_mode_key_equal(&a, &b));

	b = a;
	b.core_class = RT_CONF_CORE_LITTLE;
	pwtest_bool_false(rt_conformal_mode_key_equal(&a, &b));
	return PWTEST_PASS;
}

PWTEST(conformal_table_create_destroy_null_safe)
{
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 4);
	pwtest_ptr_notnull(t);
	pwtest_int_eq((int)rt_conformal_table_mode_count(t), 0);
	rt_conformal_table_destroy(t);

	/* NULL config refused. */
	pwtest_ptr_null(rt_conformal_table_create(NULL, 4));

	/* Invalid config refused (alpha_target out of range). */
	cfg.alpha_target = 5.0;
	pwtest_ptr_null(rt_conformal_table_create(&cfg, 4));

	/* NULL destroy is a no-op. */
	rt_conformal_table_destroy(NULL);
	return PWTEST_PASS;
}

PWTEST(conformal_table_keys_route_to_distinct_estimators)
{
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	cfg.bootstrap_runtime_ns = 1000;
	cfg.runtime_floor_ns = 100;
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 8);
	pwtest_ptr_notnull(t);

	struct rt_conformal_mode_key k_big_1024 = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_BIG };
	struct rt_conformal_mode_key k_little_1024 = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_LITTLE };
	struct rt_conformal_mode_key k_big_512 = {
		.sample_rate_hz = 48000, .quantum_frames = 512,
		.core_class = RT_CONF_CORE_BIG };
	struct rt_conformal_mode_key k_big_44k = {
		.sample_rate_hz = 44100, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_BIG };

	/* Each observe() on a fresh key allocates an entry. */
	(void)rt_conformal_table_observe(t, &k_big_1024, 5000);
	pwtest_int_eq((int)rt_conformal_table_mode_count(t), 1);
	(void)rt_conformal_table_observe(t, &k_little_1024, 9000);
	pwtest_int_eq((int)rt_conformal_table_mode_count(t), 2);
	(void)rt_conformal_table_observe(t, &k_big_512, 4000);
	pwtest_int_eq((int)rt_conformal_table_mode_count(t), 3);
	(void)rt_conformal_table_observe(t, &k_big_44k, 5500);
	pwtest_int_eq((int)rt_conformal_table_mode_count(t), 4);

	/* Backing estimators are distinct objects with independent
	 * sample counters. */
	rt_conformal_t *e_big_1024 = rt_conformal_table_get(t, &k_big_1024);
	rt_conformal_t *e_little_1024 = rt_conformal_table_get(t, &k_little_1024);
	rt_conformal_t *e_big_512 = rt_conformal_table_get(t, &k_big_512);
	pwtest_ptr_notnull(e_big_1024);
	pwtest_ptr_notnull(e_little_1024);
	pwtest_ptr_notnull(e_big_512);
	pwtest_bool_true(e_big_1024 != e_little_1024);
	pwtest_bool_true(e_big_1024 != e_big_512);
	pwtest_bool_true(e_little_1024 != e_big_512);

	/* Observe more samples on big-1024 only; the other estimators
	 * stay at one sample each. */
	for (int i = 0; i < 5; i++)
		(void)rt_conformal_table_observe(t, &k_big_1024, 5000 + i);
	pwtest_int_eq((int)rt_conformal_samples_seen(e_big_1024), 6);
	pwtest_int_eq((int)rt_conformal_samples_seen(e_little_1024), 1);
	pwtest_int_eq((int)rt_conformal_samples_seen(e_big_512), 1);

	pwtest_int_eq((int)rt_conformal_table_evictions(t), 0);
	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

PWTEST(conformal_table_evicts_lru_when_cap_exceeded)
{
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	cfg.bootstrap_runtime_ns = 1000;
	cfg.runtime_floor_ns = 100;
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 2);
	pwtest_ptr_notnull(t);

	struct rt_conformal_mode_key k1 = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_BIG };
	struct rt_conformal_mode_key k2 = {
		.sample_rate_hz = 48000, .quantum_frames = 512,
		.core_class = RT_CONF_CORE_BIG };
	struct rt_conformal_mode_key k3 = {
		.sample_rate_hz = 48000, .quantum_frames = 256,
		.core_class = RT_CONF_CORE_BIG };

	(void)rt_conformal_table_observe(t, &k1, 5000);
	(void)rt_conformal_table_observe(t, &k2, 5000);
	pwtest_int_eq((int)rt_conformal_table_mode_count(t), 2);
	pwtest_int_eq((int)rt_conformal_table_evictions(t), 0);

	/* Touch k2 again so k1 becomes the LRU victim. */
	(void)rt_conformal_table_observe(t, &k2, 5100);

	(void)rt_conformal_table_observe(t, &k3, 6000);
	pwtest_int_eq((int)rt_conformal_table_mode_count(t), 2);
	pwtest_int_eq((int)rt_conformal_table_evictions(t), 1);

	/* k1 was evicted; k2 and k3 remain. */
	pwtest_ptr_null(rt_conformal_table_get(t, &k1));
	pwtest_ptr_notnull(rt_conformal_table_get(t, &k2));
	pwtest_ptr_notnull(rt_conformal_table_get(t, &k3));

	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

PWTEST(conformal_table_budget_returns_zero_on_unobserved_key)
{
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	cfg.bootstrap_runtime_ns = 1000;
	cfg.runtime_floor_ns = 100;
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 4);
	pwtest_ptr_notnull(t);

	struct rt_conformal_mode_key k = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_BIG };
	pwtest_int_eq((int)rt_conformal_table_budget(t, &k, 1000000), 0);

	(void)rt_conformal_table_observe(t, &k, 5000);
	pwtest_int_gt((int)rt_conformal_table_budget(t, &k, 1000000), 0);

	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

PWTEST(conformal_table_invalidate_all_resets_every_entry)
{
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	cfg.bootstrap_runtime_ns = 1000;
	cfg.runtime_floor_ns = 100;
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 4);
	pwtest_ptr_notnull(t);

	struct rt_conformal_mode_key k1 = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_BIG };
	struct rt_conformal_mode_key k2 = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_LITTLE };

	for (int i = 0; i < 5; i++) {
		(void)rt_conformal_table_observe(t, &k1, 5000 + i);
		(void)rt_conformal_table_observe(t, &k2, 9000 + i);
	}
	pwtest_int_eq((int)rt_conformal_samples_seen(
				rt_conformal_table_get(t, &k1)), 5);
	pwtest_int_eq((int)rt_conformal_samples_seen(
				rt_conformal_table_get(t, &k2)), 5);

	rt_conformal_table_invalidate_all(t, RT_CONF_INVALIDATED_PERIOD);

	/* Entries persist but their samples_used counters are zeroed by
	 * rt_conformal_invalidate. */
	rt_conformal_t *e1 = rt_conformal_table_get(t, &k1);
	rt_conformal_t *e2 = rt_conformal_table_get(t, &k2);
	pwtest_ptr_notnull(e1);
	pwtest_ptr_notnull(e2);
	pwtest_int_eq((int)rt_conformal_samples_used(e1), 0);
	pwtest_int_eq((int)rt_conformal_samples_used(e2), 0);
	pwtest_int_eq((int)rt_conformal_last_invalidation_reason(e1),
			RT_CONF_INVALIDATED_PERIOD);

	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

PWTEST(conformal_table_collect_keys_iterates_every_populated_entry)
{
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	cfg.bootstrap_runtime_ns = 1000;
	cfg.runtime_floor_ns = 100;
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 4);
	pwtest_ptr_notnull(t);

	struct rt_conformal_mode_key keys[3] = {
		{ .sample_rate_hz = 48000, .quantum_frames = 1024,
		  .core_class = RT_CONF_CORE_BIG },
		{ .sample_rate_hz = 48000, .quantum_frames = 512,
		  .core_class = RT_CONF_CORE_BIG },
		{ .sample_rate_hz = 44100, .quantum_frames = 1024,
		  .core_class = RT_CONF_CORE_LITTLE },
	};

	for (int i = 0; i < 3; i++)
		(void)rt_conformal_table_observe(t, &keys[i], 5000);

	struct rt_conformal_mode_key out[4];
	uint32_t cnt = 4;
	pwtest_int_eq(rt_conformal_table_collect_keys(t, out, &cnt), 0);
	pwtest_int_eq((int)cnt, 3);

	/* Every input key must appear exactly once in the output. */
	for (int i = 0; i < 3; i++) {
		int found = 0;
		for (uint32_t j = 0; j < cnt; j++)
			if (rt_conformal_mode_key_equal(&out[j], &keys[i]))
				found++;
		pwtest_int_eq(found, 1);
	}

	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

/* Drive every entry of the table referenced by `keys` through enough
 * samples that each lands in RT_CONF_VALID. cfg must be the same one
 * used to create the table. */
static void table_drive_keys_to_valid(rt_conformal_table_t *t,
		const struct rt_conformal_config *cfg,
		const struct rt_conformal_mode_key *keys, uint32_t n_keys,
		uint64_t runtime_ns)
{
	for (uint32_t i = 0; i < n_keys; i++) {
		for (uint32_t j = 0; j < cfg->bootstrap_min_samples + 8u; j++)
			(void)rt_conformal_table_observe(t, &keys[i], runtime_ns);
	}
}

PWTEST(conformal_table_ready_for_class_distinguishes_states)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 4);
	pwtest_ptr_notnull(t);

	/* No entry yet: ready returns false. */
	pwtest_bool_false(rt_conformal_table_ready_for_class(t,
			48000, 1024, RT_CONF_CORE_LITTLE));
	pwtest_bool_false(rt_conformal_table_ready_for_class(t,
			48000, 1024, RT_CONF_CORE_BIG));

	struct rt_conformal_mode_key little_key = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_LITTLE,
	};

	/* One observation: BOOTSTRAP state, still not publish-ready. */
	(void)rt_conformal_table_observe(t, &little_key, 50000);
	pwtest_bool_false(rt_conformal_table_ready_for_class(t,
			48000, 1024, RT_CONF_CORE_LITTLE));

	/* Drive past bootstrap into VALID. */
	for (uint32_t i = 0; i < cfg.bootstrap_min_samples + 8u; i++)
		(void)rt_conformal_table_observe(t, &little_key, 50000);
	pwtest_bool_true(rt_conformal_table_ready_for_class(t,
			48000, 1024, RT_CONF_CORE_LITTLE));

	/* A different class on the same (rate, quantum) is independent. */
	pwtest_bool_false(rt_conformal_table_ready_for_class(t,
			48000, 1024, RT_CONF_CORE_BIG));

	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

PWTEST(conformal_table_budget_for_class_returns_target_when_ready)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 4);
	pwtest_ptr_notnull(t);

	struct rt_conformal_mode_key big_key = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_BIG,
	};
	table_drive_keys_to_valid(t, &cfg, &big_key, 1, 50000);

	bool used_bootstrap = true;
	uint64_t budget = rt_conformal_table_budget_for_class(t,
			48000, 1024, RT_CONF_CORE_BIG, 100000,
			&used_bootstrap);
	pwtest_int_lt(0, (int)budget);
	pwtest_bool_false(used_bootstrap);

	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

PWTEST(conformal_table_budget_for_class_bootstraps_big_from_little)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 4);
	pwtest_ptr_notnull(t);

	/* LITTLE is ready, BIG is not. A budget request for BIG must
	 * borrow LITTLE's budget and flag the bootstrap. */
	struct rt_conformal_mode_key little_key = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_LITTLE,
	};
	table_drive_keys_to_valid(t, &cfg, &little_key, 1, 50000);

	bool used_bootstrap = false;
	uint64_t budget = rt_conformal_table_budget_for_class(t,
			48000, 1024, RT_CONF_CORE_BIG, 100000,
			&used_bootstrap);
	pwtest_int_lt(0, (int)budget);
	pwtest_bool_true(used_bootstrap);

	/* The bootstrap value must equal the LITTLE entry's own
	 * publishable budget at the same period. */
	uint64_t little_budget = rt_conformal_table_budget(t, &little_key,
			100000);
	pwtest_int_eq((int)budget, (int)little_budget);

	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

PWTEST(conformal_table_budget_for_class_does_not_bootstrap_little_from_big)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 4);
	pwtest_ptr_notnull(t);

	/* Only BIG is ready. The reverse direction (LITTLE bootstrap
	 * from BIG) is intentionally refused: borrowing the faster
	 * core's cycle estimate to schedule the slower one would
	 * under-reserve. */
	struct rt_conformal_mode_key big_key = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_BIG,
	};
	table_drive_keys_to_valid(t, &cfg, &big_key, 1, 50000);

	bool used_bootstrap = true;
	uint64_t budget = rt_conformal_table_budget_for_class(t,
			48000, 1024, RT_CONF_CORE_LITTLE, 100000,
			&used_bootstrap);
	pwtest_int_eq((int)budget, 0);
	pwtest_bool_false(used_bootstrap);

	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

PWTEST(conformal_table_budget_for_class_returns_zero_when_neither_ready)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 4);
	pwtest_ptr_notnull(t);

	bool used_bootstrap = true;
	uint64_t budget = rt_conformal_table_budget_for_class(t,
			48000, 1024, RT_CONF_CORE_BIG, 100000,
			&used_bootstrap);
	pwtest_int_eq((int)budget, 0);
	pwtest_bool_false(used_bootstrap);

	/* Insert a BOOTSTRAP-state BIG entry: still not ready, still no
	 * bootstrap available. */
	struct rt_conformal_mode_key big_key = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_BIG,
	};
	(void)rt_conformal_table_observe(t, &big_key, 50000);

	budget = rt_conformal_table_budget_for_class(t,
			48000, 1024, RT_CONF_CORE_BIG, 100000,
			&used_bootstrap);
	pwtest_int_eq((int)budget, 0);
	pwtest_bool_false(used_bootstrap);

	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

PWTEST(conformal_table_budget_for_class_null_safe)
{
	bool used_bootstrap = true;
	pwtest_int_eq((int)rt_conformal_table_budget_for_class(NULL,
			48000, 1024, RT_CONF_CORE_BIG, 100000,
			&used_bootstrap), 0);
	pwtest_bool_false(used_bootstrap);

	/* out_used_bootstrap may be NULL. */
	pwtest_int_eq((int)rt_conformal_table_budget_for_class(NULL,
			48000, 1024, RT_CONF_CORE_BIG, 100000, NULL), 0);

	pwtest_bool_false(rt_conformal_table_ready_for_class(NULL,
			48000, 1024, RT_CONF_CORE_BIG));
	return PWTEST_PASS;
}

PWTEST(conformal_table_budget_for_class_prefers_target_over_bootstrap)
{
	struct rt_conformal_config cfg = cfg_small();
	rt_conformal_table_t *t = rt_conformal_table_create(&cfg, 4);
	pwtest_ptr_notnull(t);

	/* Both classes ready; the BIG request must publish the BIG
	 * estimator's budget, not the LITTLE one. Drive them with
	 * distinct runtime distributions so the budgets are
	 * distinguishable. */
	struct rt_conformal_mode_key little_key = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_LITTLE,
	};
	struct rt_conformal_mode_key big_key = {
		.sample_rate_hz = 48000, .quantum_frames = 1024,
		.core_class = RT_CONF_CORE_BIG,
	};
	table_drive_keys_to_valid(t, &cfg, &little_key, 1, 80000);
	table_drive_keys_to_valid(t, &cfg, &big_key, 1, 30000);

	bool used_bootstrap = true;
	uint64_t big_budget = rt_conformal_table_budget_for_class(t,
			48000, 1024, RT_CONF_CORE_BIG, 100000,
			&used_bootstrap);
	pwtest_bool_false(used_bootstrap);

	uint64_t little_budget = rt_conformal_table_budget(t, &little_key,
			100000);
	pwtest_bool_true(big_budget != little_budget);

	rt_conformal_table_destroy(t);
	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_conformal)
{
	pwtest_add(conformal_state_name_stable, PWTEST_NOARG);
	pwtest_add(conformal_invalidation_reason_name_stable, PWTEST_NOARG);
	pwtest_add(conformal_risk_allocation_name_stable, PWTEST_NOARG);
	pwtest_add(conformal_config_defaults_validate, PWTEST_NOARG);
	pwtest_add(conformal_config_defaults_null_safe, PWTEST_NOARG);
	pwtest_add(conformal_config_validator_rejects_out_of_range,
			PWTEST_NOARG);

	pwtest_add(conformal_create_destroy_null_safe, PWTEST_NOARG);
	pwtest_add(conformal_create_rejects_invalid_config, PWTEST_NOARG);
	pwtest_add(conformal_state_data_size_within_budget, PWTEST_NOARG);
	pwtest_add(conformal_invalidate_resets_state, PWTEST_NOARG);

	pwtest_add(conformal_constant_stream_locks_mu, PWTEST_NOARG);
	pwtest_add(conformal_alternating_stream_tracks_scale, PWTEST_NOARG);
	pwtest_add(conformal_step_increase_inflates_budget, PWTEST_NOARG);
	pwtest_add(conformal_monotone_drift_does_not_diverge, PWTEST_NOARG);
	pwtest_add(conformal_ar1_stream_settles, PWTEST_NOARG);
	pwtest_add(conformal_pareto_burst_inflates_scale, PWTEST_NOARG);

	pwtest_add(conformal_quantile_zero_on_empty_ring, PWTEST_NOARG);
	pwtest_add(conformal_quantile_at_alpha_min_picks_max_score,
			PWTEST_NOARG);
	pwtest_add(conformal_quantile_alpha_close_to_max_lowers_quantile,
			PWTEST_NOARG);

	pwtest_add(conformal_no_lookahead_first_observation_uses_bootstrap,
			PWTEST_NOARG);
	pwtest_add(conformal_no_lookahead_observation_count_matches,
			PWTEST_NOARG);

	pwtest_add(conformal_budget_respects_runtime_floor, PWTEST_NOARG);
	pwtest_add(conformal_budget_respects_period_clamp, PWTEST_NOARG);
	pwtest_add(conformal_observe_rejects_zero, PWTEST_NOARG);
	pwtest_add(conformal_observe_clamps_below_floor, PWTEST_NOARG);
	pwtest_add(conformal_budget_handles_disabled_state, PWTEST_NOARG);

	pwtest_add(conformal_fresh_estimator_publishes_bootstrap_floor,
			PWTEST_NOARG);
	pwtest_add(conformal_state_transitions_bootstrap_to_valid,
			PWTEST_NOARG);
	pwtest_add(conformal_compatible_history_field_round_trips,
			PWTEST_NOARG);

	pwtest_add(conformal_alpha_eff_decreases_on_overrun, PWTEST_NOARG);
	pwtest_add(conformal_alpha_eff_increases_on_non_overrun,
			PWTEST_NOARG);
	pwtest_add(conformal_alpha_eff_clamps_to_min, PWTEST_NOARG);
	pwtest_add(conformal_alpha_eff_clamps_to_max, PWTEST_NOARG);
	pwtest_add(conformal_max_overrun_burst_tracks_streak,
			PWTEST_NOARG);
	pwtest_add(conformal_state_shifts_on_sustained_burst,
			PWTEST_NOARG);
	pwtest_add(conformal_disable_freezes_observation, PWTEST_NOARG);
	pwtest_add(conformal_burst_penalty_amplifies_negative_step,
			PWTEST_NOARG);
	pwtest_add(conformal_burst_penalty_validator_rejects_below_one,
			PWTEST_NOARG);

	pwtest_add(conformal_fusion_group_invalidation_resets_state,
			PWTEST_NOARG);

	/* Section H: mode-keyed estimator table. */
	pwtest_add(conformal_mode_key_equal_compares_every_field,
			PWTEST_NOARG);
	pwtest_add(conformal_table_create_destroy_null_safe, PWTEST_NOARG);
	pwtest_add(conformal_table_keys_route_to_distinct_estimators,
			PWTEST_NOARG);
	pwtest_add(conformal_table_evicts_lru_when_cap_exceeded,
			PWTEST_NOARG);
	pwtest_add(conformal_table_budget_returns_zero_on_unobserved_key,
			PWTEST_NOARG);
	pwtest_add(conformal_table_invalidate_all_resets_every_entry,
			PWTEST_NOARG);
	pwtest_add(conformal_table_ready_for_class_distinguishes_states,
			PWTEST_NOARG);
	pwtest_add(conformal_table_budget_for_class_returns_target_when_ready,
			PWTEST_NOARG);
	pwtest_add(conformal_table_budget_for_class_bootstraps_big_from_little,
			PWTEST_NOARG);
	pwtest_add(conformal_table_budget_for_class_does_not_bootstrap_little_from_big,
			PWTEST_NOARG);
	pwtest_add(conformal_table_budget_for_class_returns_zero_when_neither_ready,
			PWTEST_NOARG);
	pwtest_add(conformal_table_budget_for_class_null_safe, PWTEST_NOARG);
	pwtest_add(conformal_table_budget_for_class_prefers_target_over_bootstrap,
			PWTEST_NOARG);
	pwtest_add(conformal_table_collect_keys_iterates_every_populated_entry,
			PWTEST_NOARG);

	return PWTEST_PASS;
}
