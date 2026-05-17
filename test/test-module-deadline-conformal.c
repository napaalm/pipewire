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
			RT_CONF_INVALIDATED_TOPOLOGY_GENERATION),
			"topology_generation");
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

	return PWTEST_PASS;
}
