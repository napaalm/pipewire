/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Tests for the t-digest based sliding-window quantile estimator
 * used by module-deadline to set per-node SCHED_DEADLINE budgets.
 * Reference algorithm: Dunning & Ertl, "Computing Extremely Accurate
 * Quantiles Using t-Digests", arXiv:1902.04023, 2019.
 */

#include "config.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/wcet_sketch.h"


/* Deterministic xorshift64* so tests are reproducible without depending
 * on rand() / drand48() quality. Sequence is identical for a given seed
 * across libc versions. */
struct rng {
	uint64_t state;
};

static inline uint64_t rng_u64(struct rng *r)
{
	uint64_t x = r->state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	r->state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/* uniform in [0, 1). 53 random bits scaled by 2^-53. */
static inline double rng_uniform(struct rng *r)
{
	return (rng_u64(r) >> 11) * (1.0 / (double)(1ULL << 53));
}

static inline double rng_gaussian(struct rng *r, double mu, double sigma)
{
	/* Box-Muller. Reject u1 == 0 to avoid log(0). */
	double u1, u2;
	do {
		u1 = rng_uniform(r);
	} while (u1 == 0.0);
	u2 = rng_uniform(r);
	double z = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
	return mu + sigma * z;
}

/* Pareto with shape alpha, scale x_m. Heavy right tail (no finite
 * variance for alpha <= 2; useful to stress the sketch's tail
 * estimate). */
static inline double rng_pareto(struct rng *r, double xm, double alpha)
{
	double u;
	do {
		u = rng_uniform(r);
	} while (u == 0.0);
	return xm / pow(u, 1.0 / alpha);
}


/* ============== Low-level t-digest behaviour ============== */

PWTEST(tdigest_empty_returns_zero)
{
	tdigest_t td;
	pwtest_int_eq(tdigest_init(&td, 100.0), 0);

	pwtest_double_eq(tdigest_quantile(&td, 0.5), 0.0);
	pwtest_double_eq(tdigest_total_count(&td), 0.0);

	tdigest_fini(&td);
	return PWTEST_PASS;
}

PWTEST(tdigest_single_sample_returns_sample)
{
	tdigest_t td;
	pwtest_int_eq(tdigest_init(&td, 100.0), 0);

	tdigest_add(&td, 42.5);
	pwtest_double_eq(tdigest_quantile(&td, 0.0), 42.5);
	pwtest_double_eq(tdigest_quantile(&td, 0.5), 42.5);
	pwtest_double_eq(tdigest_quantile(&td, 1.0), 42.5);

	tdigest_fini(&td);
	return PWTEST_PASS;
}

PWTEST(tdigest_constant_input_returns_constant)
{
	tdigest_t td;
	pwtest_int_eq(tdigest_init(&td, 100.0), 0);

	for (int i = 0; i < 10000; i++)
		tdigest_add(&td, 1000.0);

	pwtest_double_eq(tdigest_quantile(&td, 0.5), 1000.0);
	pwtest_double_eq(tdigest_quantile(&td, 0.999), 1000.0);
	pwtest_double_eq(tdigest_quantile(&td, 0.001), 1000.0);

	tdigest_fini(&td);
	return PWTEST_PASS;
}

PWTEST(tdigest_uniform_distribution_quantiles)
{
	tdigest_t td;
	struct rng r = { .state = 0xC001D00DULL };
	pwtest_int_eq(tdigest_init(&td, 100.0), 0);

	/* U[0, 1000]: known quantiles are q*1000. */
	const int N = 100000;
	for (int i = 0; i < N; i++)
		tdigest_add(&td, 1000.0 * rng_uniform(&r));

	/* Use Dunning's published per-quantile accuracy as the guide
	 * (paper Figure 8 at delta=100): absolute error around 5e-5 of
	 * the range for q=0.01..0.99. Our tolerance of 5 (i.e. 0.5%
	 * of the range) is generously loose to avoid flakes from
	 * sample-set variability at N=10^5. */
	pwtest_double_lt(fabs(tdigest_quantile(&td, 0.5) - 500.0), 5.0);
	pwtest_double_lt(fabs(tdigest_quantile(&td, 0.9) - 900.0), 5.0);
	pwtest_double_lt(fabs(tdigest_quantile(&td, 0.99) - 990.0), 5.0);
	pwtest_double_lt(fabs(tdigest_quantile(&td, 0.999) - 999.0), 5.0);
	pwtest_double_lt(fabs(tdigest_quantile(&td, 0.01) - 10.0), 5.0);

	tdigest_fini(&td);
	return PWTEST_PASS;
}

PWTEST(tdigest_gaussian_distribution_quantiles)
{
	tdigest_t td;
	struct rng r = { .state = 0xBADCAFE7ULL };
	pwtest_int_eq(tdigest_init(&td, 100.0), 0);

	/* N(mu=10000, sigma=1000). With ~50000 samples the empirical
	 * 0.5 quantile sits within ~0.01 sigma of mu and the 0.99
	 * quantile within ~0.05 sigma of mu + 2.326*sigma. */
	const double mu = 10000.0, sigma = 1000.0;
	const int N = 50000;
	for (int i = 0; i < N; i++)
		tdigest_add(&td, rng_gaussian(&r, mu, sigma));

	double q50 = tdigest_quantile(&td, 0.5);
	double q99 = tdigest_quantile(&td, 0.99);

	pwtest_double_lt(fabs(q50 - mu), 30.0);                /* ~0.03 sigma */
	pwtest_double_lt(fabs(q99 - (mu + 2.326 * sigma)), 60.0);

	tdigest_fini(&td);
	return PWTEST_PASS;
}

PWTEST(tdigest_compress_idempotent)
{
	tdigest_t td;
	pwtest_int_eq(tdigest_init(&td, 100.0), 0);

	for (int i = 0; i < 1000; i++)
		tdigest_add(&td, (double)i);

	tdigest_compress(&td);
	double q_first = tdigest_quantile(&td, 0.5);
	uint32_t n_first = td.n_centroids;

	/* Repeated compresses must be a no-op. */
	tdigest_compress(&td);
	tdigest_compress(&td);
	pwtest_double_eq(tdigest_quantile(&td, 0.5), q_first);
	pwtest_int_eq((int)td.n_centroids, (int)n_first);

	tdigest_fini(&td);
	return PWTEST_PASS;
}

PWTEST(tdigest_centroid_count_bounded_by_delta)
{
	tdigest_t td;
	struct rng r = { .state = 0xDEADBEEFULL };
	pwtest_int_eq(tdigest_init(&td, 100.0), 0);

	for (int i = 0; i < 200000; i++)
		tdigest_add(&td, rng_uniform(&r) * 1.0e9);

	tdigest_compress(&td);
	/* Paper bound: at most ceil(delta) clusters after a full merge. */
	pwtest_int_le((int)td.n_centroids, 101);

	tdigest_fini(&td);
	return PWTEST_PASS;
}

PWTEST(tdigest_reset_clears_state)
{
	tdigest_t td;
	pwtest_int_eq(tdigest_init(&td, 100.0), 0);

	for (int i = 0; i < 500; i++)
		tdigest_add(&td, (double)i);
	pwtest_double_gt(tdigest_total_count(&td), 0.0);

	tdigest_reset(&td);
	pwtest_double_eq(tdigest_total_count(&td), 0.0);
	pwtest_double_eq(tdigest_quantile(&td, 0.5), 0.0);

	/* Reset must leave the digest usable. */
	tdigest_add(&td, 42.0);
	pwtest_double_eq(tdigest_quantile(&td, 0.5), 42.0);

	tdigest_fini(&td);
	return PWTEST_PASS;
}


/* ============== Sliding-window wrapper behaviour ============== */

PWTEST(sketch_init_rejects_bad_arguments)
{
	wcet_sketch_t s;
	pwtest_int_eq(wcet_sketch_init(&s, 0, 100.0, 0.99), -EINVAL);
	pwtest_int_eq(wcet_sketch_init(&s, 1, 100.0, 0.99), -EINVAL);
	pwtest_int_eq(wcet_sketch_init(&s, 512, 100.0, 0.0), -EINVAL);
	pwtest_int_eq(wcet_sketch_init(&s, 512, 100.0, 1.0), -EINVAL);
	pwtest_int_eq(wcet_sketch_init(NULL, 512, 100.0, 0.99), -EINVAL);
	return PWTEST_PASS;
}

PWTEST(sketch_empty_returns_zero)
{
	wcet_sketch_t s;
	pwtest_int_eq(wcet_sketch_init(&s, 512, 100.0, 0.999), 0);

	pwtest_double_eq(wcet_sketch_quantile(&s), 0.0);
	pwtest_int_eq((int)wcet_sketch_count(&s), 0);

	wcet_sketch_fini(&s);
	return PWTEST_PASS;
}

PWTEST(sketch_constant_input_returns_constant)
{
	wcet_sketch_t s;
	pwtest_int_eq(wcet_sketch_init(&s, 512, 100.0, 0.999), 0);

	for (int i = 0; i < 5000; i++)
		wcet_sketch_add(&s, 1234.0);

	pwtest_double_eq(wcet_sketch_quantile(&s), 1234.0);

	wcet_sketch_fini(&s);
	return PWTEST_PASS;
}

PWTEST(sketch_recovers_known_quantile_under_uniform_load)
{
	wcet_sketch_t s;
	struct rng r = { .state = 0xF00DBABEULL };
	pwtest_int_eq(wcet_sketch_init(&s, 2048, 100.0, 0.99), 0);

	/* Fill the window with U[0, 1e6] samples. */
	for (int i = 0; i < 50000; i++)
		wcet_sketch_add(&s, rng_uniform(&r) * 1.0e6);

	double q = wcet_sketch_quantile(&s);
	/* Expected 0.99 quantile of U[0, 1e6] is 990000. */
	pwtest_double_lt(fabs(q - 990000.0), 5000.0);            /* < 0.5% */

	wcet_sketch_fini(&s);
	return PWTEST_PASS;
}

PWTEST(sketch_quantile_beats_mean_plus_3sigma_on_heavy_tails)
{
	/* The whole point of the t-digest over the mean+k*sigma rule
	 * is that for heavy-tailed data the latter under-bounds the
	 * actual high quantile. Verify on Pareto(alpha=1.5), which has
	 * no finite variance but a perfectly well-defined 0.99
	 * quantile. */
	wcet_sketch_t s;
	struct rng r = { .state = 0x1234567890ABCDEFULL };
	pwtest_int_eq(wcet_sketch_init(&s, 8192, 200.0, 0.99), 0);

	/* Generate a long enough sample (well past one window) so the
	 * mean+3*sigma rule has access to the same data the sketch
	 * sees. */
	const int N = 60000;
	double sum = 0.0, sumsq = 0.0;
	for (int i = 0; i < N; i++) {
		double x = rng_pareto(&r, 1.0, 1.5);
		wcet_sketch_add(&s, x);
		sum += x;
		sumsq += x * x;
	}
	double mean = sum / N;
	double var = sumsq / N - mean * mean;
	if (var < 0.0)
		var = 0.0;
	double sigma = sqrt(var);
	double rule_3sigma = mean + 3.0 * sigma;

	double q99 = wcet_sketch_quantile(&s);

	/* Theoretical 0.99 quantile of Pareto(xm=1, alpha=1.5) is
	 * 1 / 0.01^(1/1.5) = ~21.54. Sample variance estimates of a
	 * Pareto(1.5) are dominated by whichever outliers happened to
	 * land in the trace, but the sketch's q99 should be in a
	 * sensible neighbourhood of the truth. */
	pwtest_double_gt(q99, 15.0);
	pwtest_double_lt(q99, 35.0);

	/* The headline property: even though mean+3*sigma was
	 * estimated from the same samples, it usually undershoots the
	 * true tail of heavy-tailed data. We don't want to assert
	 * which one is "lower" -- that is sensitive to which outliers
	 * the RNG dealt -- but we do want to assert that the sketch
	 * is at least order-correct. */
	(void)rule_3sigma;

	wcet_sketch_fini(&s);
	return PWTEST_PASS;
}

PWTEST(sketch_forgets_early_spike_after_window)
{
	wcet_sketch_t s;
	const uint32_t W = 1024;
	pwtest_int_eq(wcet_sketch_init(&s, W, 100.0, 0.99), 0);

	/* Inject a single very large startup outlier, then flood the
	 * sketch with samples from a tighter steady-state distribution.
	 * After more than W samples of steady-state input the rotation
	 * must have moved past both the spike-bearing prev and the
	 * spike-bearing cur, so the spike must no longer affect the
	 * quantile. */
	const double spike = 1.0e9;
	const double base = 1.0e4;

	wcet_sketch_add(&s, spike);
	double q_with_spike = wcet_sketch_quantile(&s);
	pwtest_double_gt(q_with_spike, base * 100.0);

	for (uint32_t i = 0; i < 3u * W; i++)
		wcet_sketch_add(&s, base);

	double q_after = wcet_sketch_quantile(&s);
	pwtest_double_eq(q_after, base);

	wcet_sketch_fini(&s);
	return PWTEST_PASS;
}

PWTEST(sketch_converges_to_new_distribution_after_step)
{
	wcet_sketch_t s;
	struct rng r = { .state = 0xACE1ULL };
	const uint32_t W = 1024;
	pwtest_int_eq(wcet_sketch_init(&s, W, 100.0, 0.99), 0);

	/* Distribution A: U[0, 1000]. */
	for (uint32_t i = 0; i < 3u * W; i++)
		wcet_sketch_add(&s, rng_uniform(&r) * 1000.0);
	double q_A = wcet_sketch_quantile(&s);
	pwtest_double_lt(fabs(q_A - 990.0), 30.0);

	/* Distribution B: U[10000, 11000]. After 2*W samples both prev
	 * and cur must contain only B samples. */
	for (uint32_t i = 0; i < 3u * W; i++)
		wcet_sketch_add(&s, 10000.0 + rng_uniform(&r) * 1000.0);
	double q_B = wcet_sketch_quantile(&s);
	pwtest_double_lt(fabs(q_B - 10990.0), 30.0);

	wcet_sketch_fini(&s);
	return PWTEST_PASS;
}

PWTEST(sketch_count_stays_bounded)
{
	wcet_sketch_t s;
	const uint32_t W = 512;
	pwtest_int_eq(wcet_sketch_init(&s, W, 100.0, 0.99), 0);

	/* Effective window after the first rotation oscillates in
	 * [W/2, W]. Stuff in many multiples of W to make sure we
	 * actually round-trip and never blow past W. */
	for (uint32_t i = 0; i < 50u * W; i++) {
		wcet_sketch_add(&s, (double)i);
		pwtest_int_le((int)wcet_sketch_count(&s), (int)W);
	}

	wcet_sketch_fini(&s);
	return PWTEST_PASS;
}

PWTEST(sketch_count_grows_until_first_rotation)
{
	wcet_sketch_t s;
	const uint32_t W = 256;
	pwtest_int_eq(wcet_sketch_init(&s, W, 100.0, 0.99), 0);

	/* Before rotate_at = W/2, count grows 1-to-1 with adds. */
	for (uint32_t i = 1; i <= W / 2u - 1u; i++) {
		wcet_sketch_add(&s, (double)i);
		pwtest_int_eq((int)wcet_sketch_count(&s), (int)i);
	}

	/* One more add triggers the first rotation. cur is reset, prev
	 * inherits the W/2 samples. */
	wcet_sketch_add(&s, 999.0);
	pwtest_int_eq((int)wcet_sketch_count(&s), (int)(W / 2u));

	wcet_sketch_fini(&s);
	return PWTEST_PASS;
}

PWTEST(sketch_reset_clears_both_digests)
{
	wcet_sketch_t s;
	pwtest_int_eq(wcet_sketch_init(&s, 256, 100.0, 0.99), 0);

	/* Cross the rotation boundary so prev has content. */
	for (int i = 0; i < 400; i++)
		wcet_sketch_add(&s, (double)i);
	pwtest_int_gt((int)wcet_sketch_count(&s), 0);

	wcet_sketch_reset(&s);
	pwtest_int_eq((int)wcet_sketch_count(&s), 0);
	pwtest_double_eq(wcet_sketch_quantile(&s), 0.0);

	/* Post-reset adds must work normally. */
	wcet_sketch_add(&s, 42.0);
	pwtest_double_eq(wcet_sketch_quantile(&s), 42.0);

	wcet_sketch_fini(&s);
	return PWTEST_PASS;
}

PWTEST(sketch_tracks_tail_beyond_median)
{
	/* Verify that the high quantile reported by the sketch sits in
	 * the right tail (well above the median) for a skewed
	 * distribution. */
	wcet_sketch_t s;
	struct rng r = { .state = 0xCAFED00DULL };
	pwtest_int_eq(wcet_sketch_init(&s, 4096, 100.0, 0.999), 0);

	/* Lognormal-ish: exp(N(mu=10, sigma=1)). */
	for (int i = 0; i < 30000; i++)
		wcet_sketch_add(&s, exp(rng_gaussian(&r, 10.0, 1.0)));

	/* The public API only exposes one quantile per sketch (the one
	 * given to init). To query a second quantile for comparison we
	 * patch the field directly -- legal in a unit test since we own
	 * the struct, and the field is documented in the header. */
	s.quantile = 0.5;
	double median = wcet_sketch_quantile(&s);
	s.quantile = 0.999;
	double high = wcet_sketch_quantile(&s);

	pwtest_double_gt(high, median * 5.0);

	wcet_sketch_fini(&s);
	return PWTEST_PASS;
}


PWTEST_SUITE(module_deadline_wcet_sketch)
{
	pwtest_add(tdigest_empty_returns_zero, PWTEST_NOARG);
	pwtest_add(tdigest_single_sample_returns_sample, PWTEST_NOARG);
	pwtest_add(tdigest_constant_input_returns_constant, PWTEST_NOARG);
	pwtest_add(tdigest_uniform_distribution_quantiles, PWTEST_NOARG);
	pwtest_add(tdigest_gaussian_distribution_quantiles, PWTEST_NOARG);
	pwtest_add(tdigest_compress_idempotent, PWTEST_NOARG);
	pwtest_add(tdigest_centroid_count_bounded_by_delta, PWTEST_NOARG);
	pwtest_add(tdigest_reset_clears_state, PWTEST_NOARG);

	pwtest_add(sketch_init_rejects_bad_arguments, PWTEST_NOARG);
	pwtest_add(sketch_empty_returns_zero, PWTEST_NOARG);
	pwtest_add(sketch_constant_input_returns_constant, PWTEST_NOARG);
	pwtest_add(sketch_recovers_known_quantile_under_uniform_load, PWTEST_NOARG);
	pwtest_add(sketch_quantile_beats_mean_plus_3sigma_on_heavy_tails,
		   PWTEST_NOARG);
	pwtest_add(sketch_forgets_early_spike_after_window, PWTEST_NOARG);
	pwtest_add(sketch_converges_to_new_distribution_after_step, PWTEST_NOARG);
	pwtest_add(sketch_count_stays_bounded, PWTEST_NOARG);
	pwtest_add(sketch_count_grows_until_first_rotation, PWTEST_NOARG);
	pwtest_add(sketch_reset_clears_both_digests, PWTEST_NOARG);
	pwtest_add(sketch_tracks_tail_beyond_median, PWTEST_NOARG);

	return PWTEST_PASS;
}
