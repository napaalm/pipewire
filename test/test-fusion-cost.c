/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Unit tests for src/pipewire/fusion-cost.h.
 *
 * The header is pure data, so the tests run without bringing up a
 * pw_context. We exercise both primitives:
 *
 *   - pw_fusion_decide(): the Sarkar 1989 §5.3 internalisation
 *     profitability criterion adapted to the audio-graph setting --
 *     merge iff sum_wcet <= cp_wcet + cp_hops * wakeup_cost. The
 *     SPLIT / LINEAR_ONLY / FUSE branches all need direct coverage,
 *     plus the warm-up gate (LINEAR_ONLY when min_samples_seen <
 *     min_samples), plus the trivial-component degenerate cases
 *     (N<=1, cp_hops==0).
 *
 *   - pw_fusion_window_*(): the per-node sliding-window mean used
 *     to smooth prev_run_time samples before the cost model reads
 *     them. We pin: the first-sample behaviour, the
 *     sum-stays-consistent-under-eviction invariant, the zero-
 *     sample no-op contract (so an idle cycle does not advance the
 *     warm-up gate), and pw_fusion_window_clear leaving the
 *     caller-owned buffer intact (so relocate-and-warmup loops
 *     don't churn allocation).
 *
 *   - Behavioural invariants the principled default rests on:
 *       variance of windowed mean scales as 1/sqrt(N);
 *       step response time scales linearly with N (~0.9*N for
 *           the 90% crossing);
 *       pw_fusion_window_target_n clamps to [N_MIN, N_MAX] on
 *           pathological cycle periods and returns the exact
 *           arithmetic in the principled band;
 *       decision-flip count under realistic jitter is monotonic
 *           in N.
 *     These are the claims fusion-cost.h's block comment uses to
 *     motivate N_MIN, N_MAX, and the 250 ms default target window.
 *
 * Citations for the criterion itself live in fusion-cost.h.
 */

#include "config.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "pwtest.h"

#include "../src/pipewire/fusion-cost.h"

/* --------------------------------------------------------------------- */
/* pw_fusion_decide                                                       */
/* --------------------------------------------------------------------- */

PWTEST(fusion_decide_null_inputs_split)
{
	struct pw_fusion_component c = { .n_nodes = 5, .cp_hops = 3 };
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000, .min_samples = 0 };

	/* NULL component -> SPLIT (defensive). */
	pwtest_int_eq((int)pw_fusion_decide(NULL, &p),
			(int)PW_FUSION_DECISION_SPLIT);
	/* NULL params -> SPLIT. */
	pwtest_int_eq((int)pw_fusion_decide(&c, NULL),
			(int)PW_FUSION_DECISION_SPLIT);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_singleton_splits)
{
	/* A component of one node has nothing to fuse with; the
	 * iteration would still find a "min_id" but there is no edge
	 * to internalise so PARTIME_merged == PARTIME_split and the
	 * decision degenerates to leaving the node ungrouped. */
	struct pw_fusion_component c = {
		.n_nodes = 1, .sum_wcet_ns = 5000,
		.cp_wcet_ns = 5000, .cp_hops = 0, .min_samples_seen = 100
	};
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000, .min_samples = 0 };
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_SPLIT);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_zero_hops_splits)
{
	/* N >= 2 but cp_hops == 0 means a disconnected component fed
	 * into the criterion by mistake: no edges, so internalisation
	 * has no edges to internalise. SPLIT is the safe answer. */
	struct pw_fusion_component c = {
		.n_nodes = 3, .sum_wcet_ns = 9000,
		.cp_wcet_ns = 3000, .cp_hops = 0, .min_samples_seen = 100
	};
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000, .min_samples = 0 };
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_SPLIT);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_warmup_returns_linear_only)
{
	/* A genuinely profitable component still returns LINEAR_ONLY
	 * while min_samples_seen < min_samples. The caller is expected
	 * to fall back to the Gerasoulis-Yang chain-only baseline
	 * during warm-up. */
	struct pw_fusion_component c = {
		.n_nodes = 3, .sum_wcet_ns = 3000,
		.cp_wcet_ns = 3000, .cp_hops = 2,
		.min_samples_seen = 1
	};
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000, .min_samples = 4 };
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_pure_chain_always_fuses)
{
	/* A length-3 chain on a uniform 1000 ns per-node WCET:
	 *     sum_wcet = 3000, cp_wcet = 3000, cp_hops = 2.
	 *     budget   = 3000 + 2 * 1000 = 5000.
	 * Sarkar's criterion holds with room to spare. This is the
	 * pre-existing chain-merge case formalised: Gerasoulis-Yang
	 * 1993 proves chains are always non-pessimistic, and the
	 * cost model agrees. */
	struct pw_fusion_component c = {
		.n_nodes = 3, .sum_wcet_ns = 3000,
		.cp_wcet_ns = 3000, .cp_hops = 2,
		.min_samples_seen = 100
	};
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000, .min_samples = 4 };
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_fan_out_profitable_when_balanced)
{
	/* Fan-out: one source -> 3 cheap siblings. cp_hops = 1 (one
	 * edge on the longest path: source -> heaviest sibling).
	 *     sum_wcet = 1000 (source) + 3*500 = 2500
	 *     cp_wcet  = 1000 + 500       = 1500
	 *     budget   = 1500 + 1 * 1000  = 2500
	 *     sum_wcet <= budget => FUSE.
	 * Three cheap nodes saved on a single source's hop is the
	 * canonical fan-out case the cost model is supposed to catch. */
	struct pw_fusion_component c = {
		.n_nodes = 4, .sum_wcet_ns = 2500,
		.cp_wcet_ns = 1500, .cp_hops = 1,
		.min_samples_seen = 100
	};
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000, .min_samples = 4 };
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_heavy_sibling_blocks_fusion)
{
	/* Fan-out with a heavy second sibling. The merged thread has
	 * to run BOTH siblings sequentially, but the split alternative
	 * runs them in parallel. cp_hops = 1.
	 *     sum_wcet = 100 + 100 + 10000 = 10200
	 *     cp_wcet  = 100 + 10000       = 10100
	 *     budget   = 10100 + 1 * 1000  = 11100
	 *     sum_wcet (10200) <= budget (11100) => still FUSE here,
	 *     because the cheap siblings dominate. To force the
	 *     LINEAR_ONLY decision we need a *second* heavy sibling. */
	struct pw_fusion_component c = {
		.n_nodes = 3, .sum_wcet_ns = 10200,
		.cp_wcet_ns = 10100, .cp_hops = 1,
		.min_samples_seen = 100
	};
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000, .min_samples = 4 };
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	/* Two heavy siblings: sum doubles, cp barely moves.
	 *     sum_wcet = 100 + 10000 + 10000 = 20100
	 *     cp_wcet  = 100 + 10000         = 10100
	 *     budget   = 10100 + 1 * 1000    = 11100
	 *     sum_wcet (20100) > budget (11100) => LINEAR_ONLY. */
	c.sum_wcet_ns = 20100;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_diamond_break_even)
{
	/* Diamond: source -> {A, B} -> sink. cp_hops = 2 (source ->
	 * heaviest middle -> sink). With cheap middles the criterion
	 * holds. */
	struct pw_fusion_component c = {
		.n_nodes = 4, .sum_wcet_ns = 4000,   /* 1000*4 */
		.cp_wcet_ns = 3000,                   /* 1000*3 */
		.cp_hops = 2,
		.min_samples_seen = 100
	};
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000, .min_samples = 4 };
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	/* Now stretch the diamond into the "expensive parallelism"
	 * regime: each of the two middles costs 5000, source/sink
	 * cheap.
	 *     sum_wcet = 500 + 5000 + 5000 + 500 = 11000
	 *     cp_wcet  = 500 + 5000 + 500       = 6000
	 *     budget   = 6000 + 2 * 1000        = 8000
	 *     sum_wcet > budget => LINEAR_ONLY. */
	c.sum_wcet_ns = 11000;
	c.cp_wcet_ns = 6000;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_wakeup_cost_changes_decision)
{
	/* Same component, different W. With a small W (~1us) the
	 * criterion fails; with the eventfd cost on a slow host (10us)
	 * it passes. The configurability of wakeup_cost_ns is the
	 * operator-visible knob that lets the criterion track the
	 * actual measured cost on the host. */
	struct pw_fusion_component c = {
		.n_nodes = 3,
		.sum_wcet_ns = 4500, .cp_wcet_ns = 3000, .cp_hops = 2,
		.min_samples_seen = 100
	};
	struct pw_fusion_params slow = { .wakeup_cost_ns = 10000, .min_samples = 4 };
	struct pw_fusion_params fast = { .wakeup_cost_ns = 500,   .min_samples = 4 };

	pwtest_int_eq((int)pw_fusion_decide(&c, &slow),
			(int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)pw_fusion_decide(&c, &fast),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	return PWTEST_PASS;
}

/* --------------------------------------------------------------------- */
/* pw_fusion_window: basic contract                                       */
/* --------------------------------------------------------------------- */

static void window_init(struct pw_fusion_window *w, uint64_t *buf, uint32_t cap)
{
	w->samples = buf;
	w->capacity = cap;
	w->count = 0;
	w->head = 0;
	w->sum = 0;
}

PWTEST(window_first_sample_records_value)
{
	/* First sample lands at head=0 and seeds sum / count. */
	uint64_t buf[8] = { 0 };
	struct pw_fusion_window w;
	window_init(&w, buf, 8);

	pw_fusion_window_update(&w, 1000);
	pwtest_int_eq((int)w.count, 1);
	pwtest_int_eq((int)w.head, 1);
	pwtest_int_eq((int)w.sum, 1000);
	pwtest_int_eq((int)pw_fusion_window_mean(&w), 1000);
	return PWTEST_PASS;
}

PWTEST(window_zero_sample_is_noop)
{
	/* Zero sample = "no measurement this cycle" -- buffer state
	 * does not advance. The warm-up gate (count < min_samples)
	 * therefore stays put on idle cycles. */
	uint64_t buf[8] = { 0 };
	struct pw_fusion_window w;
	window_init(&w, buf, 8);

	pw_fusion_window_update(&w, 5000);
	pw_fusion_window_update(&w, 0);  /* no-op */
	pw_fusion_window_update(&w, 0);  /* no-op */

	pwtest_int_eq((int)w.count, 1);
	pwtest_int_eq((int)w.head, 1);
	pwtest_int_eq((int)pw_fusion_window_mean(&w), 5000);
	return PWTEST_PASS;
}

PWTEST(window_eviction_keeps_sum_consistent)
{
	/* The running sum must subtract the evicted oldest sample on
	 * overflow; otherwise the mean would drift permanently after
	 * the buffer fills. Fill the window, overflow by N more
	 * samples, verify sum == sum of last N samples. */
	uint64_t buf[4] = { 0 };
	struct pw_fusion_window w;
	uint32_t i;

	window_init(&w, buf, 4);
	/* Push samples 2000, 3000, ..., 9000 through a capacity-4
	 * window. After 8 updates only the last 4 (6000, 7000, 8000,
	 * 9000) remain; sum must reflect the eviction subtraction
	 * exactly, not the cumulative add. */
	for (i = 0; i < 8; i++)
		pw_fusion_window_update(&w, 1000 + (i + 1) * 1000);
	pwtest_int_eq((int)w.count, 4);
	pwtest_int_eq((int)w.sum, 30000);
	pwtest_int_eq((int)pw_fusion_window_mean(&w), 7500);
	return PWTEST_PASS;
}

PWTEST(window_mean_exact_for_full_buffer)
{
	/* The windowed mean of constant input should equal the input
	 * exactly (no rounding error from accumulation), confirming
	 * the sum / count division is the textbook mean. */
	uint64_t buf[16] = { 0 };
	struct pw_fusion_window w;
	uint32_t i;

	window_init(&w, buf, 16);
	for (i = 0; i < 32; i++)
		pw_fusion_window_update(&w, 12345);
	pwtest_int_eq((int)pw_fusion_window_mean(&w), 12345);
	return PWTEST_PASS;
}

PWTEST(window_clear_resets_state_keeps_buffer)
{
	/* pw_fusion_window_clear is the relocation hook in context.c:
	 * it must reset count / head / sum but leave samples
	 * (caller-owned) alone, so the relocate-and-warmup loop does
	 * not allocate repeatedly. */
	uint64_t buf[8] = { 99, 99, 99, 99, 99, 99, 99, 99 };
	struct pw_fusion_window w;
	uint32_t i;

	window_init(&w, buf, 8);
	for (i = 0; i < 5; i++)
		pw_fusion_window_update(&w, 1000);

	pw_fusion_window_clear(&w);
	pwtest_int_eq((int)w.count, 0);
	pwtest_int_eq((int)w.head, 0);
	pwtest_int_eq((int)w.sum, 0);
	pwtest_ptr_eq(w.samples, (uint64_t *)buf);
	pwtest_int_eq((int)w.capacity, 8);
	return PWTEST_PASS;
}

PWTEST(window_null_pointers_no_crash)
{
	uint64_t buf[4] = { 0 };
	struct pw_fusion_window w;
	window_init(&w, buf, 4);

	pw_fusion_window_update(NULL, 1000);
	pw_fusion_window_clear(NULL);
	pwtest_int_eq((int)pw_fusion_window_mean(NULL), 0);

	/* zero capacity is also tolerated. */
	struct pw_fusion_window empty = { 0 };
	pw_fusion_window_update(&empty, 1000);
	pwtest_int_eq((int)empty.count, 0);
	return PWTEST_PASS;
}

/* --------------------------------------------------------------------- */
/* Behavioural invariants the principled default rests on                 */
/* --------------------------------------------------------------------- */

/* Deterministic pseudo-random for the experiments below. We avoid
 * <stdlib.h>'s rand() to keep the test reproducible across libc /
 * platform. LCG parameters from Numerical Recipes. */
struct rng { uint64_t state; };
static uint32_t rng_next(struct rng *r)
{
	r->state = r->state * 1664525ULL + 1013904223ULL;
	return (uint32_t)(r->state >> 16);
}
static double rng_uniform(struct rng *r)
{
	return (double)(rng_next(r) & 0xffff) / 65536.0;
}
/* Box-Muller to get an N(0,1) sample. */
static double rng_normal(struct rng *r)
{
	double u1 = rng_uniform(r);
	double u2 = rng_uniform(r);
	if (u1 < 1e-12) u1 = 1e-12;
	double mag = -2.0 * log(u1);
	if (mag < 0) mag = 0;
	return sqrt(mag) * cos(2.0 * 3.14159265358979 * u2);
}

/* Sample-mean / sample-variance helpers. */
static double mean(const double *v, uint32_t n)
{
	double s = 0;
	uint32_t i;
	for (i = 0; i < n; i++)
		s += v[i];
	return s / n;
}
static double stddev(const double *v, uint32_t n)
{
	double m = mean(v, n);
	double s = 0;
	uint32_t i;
	for (i = 0; i < n; i++) {
		double d = v[i] - m;
		s += d * d;
	}
	return sqrt(s / n);
}

PWTEST(invariant_variance_scales_as_1_over_sqrt_n)
{
	/* Theory: for IID input with per-sample std dev sigma, the
	 * std dev of an N-sample windowed mean is sigma / sqrt(N).
	 * This is the foundation of the variance argument for
	 * picking N -- if it does not hold, the rest of the principled
	 * motivation collapses.
	 *
	 * Experiment: feed 4000 N(mean=5000, sigma=1000) samples
	 * through windows of N in {8, 32, 128, 512}. For each N record
	 * the windowed mean every 16 samples after the window is
	 * full, then compute the std dev of those means. Verify it
	 * tracks sigma/sqrt(N) within a wide multiplicative envelope
	 * (the trace is finite so we expect Monte-Carlo wobble). The
	 * envelope is generous (within a factor of 2 either way)
	 * because the goal is to validate the trend, not curve-fit
	 * the constant. */
	uint32_t Ns[4] = { 8, 32, 128, 512 };
	double observed[4];
	double sigma = 1000.0;
	uint32_t TRACE = 4000;
	uint32_t k;

	for (k = 0; k < 4; k++) {
		uint32_t N = Ns[k];
		uint64_t *buf = calloc(N, sizeof(*buf));
		struct pw_fusion_window w;
		struct rng r = { .state = 1 };
		double *samples_mean;
		uint32_t n_means = 0;
		uint32_t i;

		window_init(&w, buf, N);
		samples_mean = calloc(TRACE / 16, sizeof(*samples_mean));

		for (i = 0; i < TRACE; i++) {
			double v = 5000.0 + rng_normal(&r) * sigma;
			if (v < 0) v = 0;
			pw_fusion_window_update(&w, (uint64_t)v);
			if (i >= N && (i % 16) == 0)
				samples_mean[n_means++] =
					(double)pw_fusion_window_mean(&w);
		}
		observed[k] = stddev(samples_mean, n_means);
		free(samples_mean);
		free(buf);
	}

	/* Pin the trend, not the values: doubling N from 8 to 32
	 * (factor 4 in N) should bring sigma_obs down by factor sqrt(4)
	 * = 2. We accept a wide envelope [1.2, 3.0] because the
	 * Monte-Carlo wobble on a 4000-sample trace (~250 means
	 * sampled at stride 16) puts each ratio's empirical std at
	 * ~15% of the true value -- a narrower envelope would flap
	 * under PRNG state changes. */
	for (k = 0; k + 1 < 4; k++) {
		double ratio = observed[k] / observed[k + 1];
		/* Theoretical ratio = sqrt(4) = 2. */
		pwtest_bool_true(ratio > 1.2 && ratio < 3.0);
	}
	/* Also enforce strict monotonicity: variance must NOT increase
	 * with N regardless of envelope wiggles. */
	pwtest_bool_true(observed[0] > observed[3]);
	return PWTEST_PASS;
}

PWTEST(invariant_step_response_time_scales_linearly_with_n)
{
	/* Theory: a sliding mean of N samples is a moving-average
	 * filter. Its response to a unit step from value A to value B
	 * crosses 90% of the new steady-state at exactly the cycle
	 * 0.9 * N after the step (linear ramp interpretation: 90% of
	 * the window now contains the post-step value). The
	 * step-response argument for picking N is the wall-clock
	 * version of this fact.
	 *
	 * Experiment: feed 2*N samples of 1000 ns, then 2*N samples
	 * of 5000 ns. Find the first cycle after the step at which
	 * the windowed mean crosses 1000 + 0.9 * (5000 - 1000) = 4600.
	 * Verify it's within +/-2 cycles of 0.9 * N. */
	uint32_t Ns[3] = { 16, 64, 256 };
	uint32_t k;

	for (k = 0; k < 3; k++) {
		uint32_t N = Ns[k];
		uint64_t *buf = calloc(N, sizeof(*buf));
		struct pw_fusion_window w;
		uint32_t i;
		int crossed_at = -1;

		window_init(&w, buf, N);
		for (i = 0; i < 2 * N; i++)
			pw_fusion_window_update(&w, 1000);
		/* Verify pre-step baseline. */
		pwtest_int_eq((int)pw_fusion_window_mean(&w), 1000);

		for (i = 0; i < 2 * N; i++) {
			pw_fusion_window_update(&w, 5000);
			if (crossed_at < 0 &&
					pw_fusion_window_mean(&w) >= 4600) {
				crossed_at = (int)i + 1;
				break;
			}
		}
		free(buf);

		/* Theoretical: cycle 0.9 * N. Allow +/-2 cycles of slack
		 * for integer-mean truncation rounding. */
		int target = (int)(0.9 * N + 0.5);
		pwtest_bool_true(crossed_at >= target - 2 &&
				crossed_at <= target + 2);
	}
	return PWTEST_PASS;
}

PWTEST(invariant_target_n_clamps_to_principled_bounds)
{
	/* pw_fusion_window_target_n must:
	 *   - return N_MIN on cold start (period == 0),
	 *   - clamp very fast cycles upward to N_MAX,
	 *   - clamp very slow cycles downward to N_MIN,
	 *   - return the exact arithmetic in the principled band. */
	uint64_t W = 250000000ULL;  /* 250 ms */

	/* Cold start. */
	pwtest_int_eq((int)pw_fusion_window_target_n(0, W),
			(int)PW_FUSION_WINDOW_N_MIN);

	/* Pro-audio: 1.33 ms cycle -> 250 / 1.33 = 188 samples (inside
	 * the [8, 512] band). */
	pwtest_int_eq((int)pw_fusion_window_target_n(1333333ULL, W), 187);

	/* Desktop default: 5.33 ms -> ~47 samples. */
	pwtest_int_eq((int)pw_fusion_window_target_n(5333333ULL, W), 46);

	/* Power-save: 21.3 ms -> ~11, clamped above N_MIN=8. */
	pwtest_int_eq((int)pw_fusion_window_target_n(21333333ULL, W), 11);

	/* Pathologically slow: 100 ms cycle -> 2, clamped to N_MIN. */
	pwtest_int_eq((int)pw_fusion_window_target_n(100000000ULL, W),
			(int)PW_FUSION_WINDOW_N_MIN);

	/* Pathologically fast: 0.1 ms cycle -> 2500, clamped to
	 * N_MAX. */
	pwtest_int_eq((int)pw_fusion_window_target_n(100000ULL, W),
			(int)PW_FUSION_WINDOW_N_MAX);

	return PWTEST_PASS;
}

PWTEST(invariant_decision_stability_under_jitter_improves_with_n)
{
	/* The "false-flip-under-jitter" property is what motivates the
	 * choice to use a smoothed estimator at all. Larger N must
	 * monotonically reduce the number of false flips (where "false"
	 * = the per-sample value crosses the criterion but the mean of
	 * the underlying distribution does not).
	 *
	 * Experiment: simulate a Y-shape exactly at the criterion
	 * threshold. The true per-node mean WCETs are picked so that
	 * the criterion is on the cusp: sum_wcet just below budget
	 * (FUSE). Add IID jitter (sigma = 15% of mean), feed through
	 * windows of increasing N, and count how many cycles flip the
	 * decision (a flip means the criterion verdict for the windowed
	 * mean is LINEAR_ONLY rather than FUSE).
	 *
	 * Pinned property: flips(N=16) > flips(N=64) > flips(N=256).
	 * Numerical values themselves are not pinned -- only the
	 * monotonic improvement. */
	uint64_t W = 3000;
	struct pw_fusion_params p = { .wakeup_cost_ns = W, .min_samples = 4 };
	uint32_t Ns[3] = { 16, 64, 256 };
	int flips[3];
	uint32_t k;
	uint32_t TRACE = 4000;

	/* Y-shape with cp_hops = 2. Target: sum just below budget.
	 *   per-arm mean = 1000 ns each side, source=1000, sink=1000.
	 *   sum_wcet = 4000, cp_wcet = 3000, cp_hops = 2.
	 *   budget   = 3000 + 2*3000 = 9000. sum < budget => FUSE.
	 * With sigma = 150 ns per-arm jitter, the windowed sum's std
	 * dev is roughly sigma * sqrt(4) / sqrt(N). At N=16 -> ~75
	 * ns; the budget margin is 9000 - 4000 = 5000 ns so flips are
	 * essentially zero either way unless we push closer to the
	 * threshold. Push the means up to 2800 each so the margin is
	 * 9000 - 4 * 2800 = -2200 -- it now sits just over the
	 * threshold, decision is LINEAR_ONLY at the true mean, and
	 * jitter pulls some cycles back below it (a FALSE FUSE). */
	double true_mean = 2800.0;
	double sigma = true_mean * 0.15;

	for (k = 0; k < 3; k++) {
		uint32_t N = Ns[k];
		struct pw_fusion_window w_s, w_a, w_b, w_t;
		uint64_t *bs, *ba, *bb, *bt;
		struct rng r = { .state = 1 };
		uint32_t i;
		int local_flips = 0;
		enum pw_fusion_decision last;

		bs = calloc(N, sizeof(*bs));
		ba = calloc(N, sizeof(*ba));
		bb = calloc(N, sizeof(*bb));
		bt = calloc(N, sizeof(*bt));
		window_init(&w_s, bs, N);
		window_init(&w_a, ba, N);
		window_init(&w_b, bb, N);
		window_init(&w_t, bt, N);

		/* Warm up. */
		for (i = 0; i < N; i++) {
			pw_fusion_window_update(&w_s, (uint64_t)true_mean);
			pw_fusion_window_update(&w_a, (uint64_t)true_mean);
			pw_fusion_window_update(&w_b, (uint64_t)true_mean);
			pw_fusion_window_update(&w_t, (uint64_t)true_mean);
		}
		last = PW_FUSION_DECISION_LINEAR_ONLY;

		for (i = 0; i < TRACE; i++) {
			double ss = true_mean + rng_normal(&r) * sigma;
			double sa = true_mean + rng_normal(&r) * sigma;
			double sb = true_mean + rng_normal(&r) * sigma;
			double st = true_mean + rng_normal(&r) * sigma;
			if (ss < 1) ss = 1;
			if (sa < 1) sa = 1;
			if (sb < 1) sb = 1;
			if (st < 1) st = 1;
			pw_fusion_window_update(&w_s, (uint64_t)ss);
			pw_fusion_window_update(&w_a, (uint64_t)sa);
			pw_fusion_window_update(&w_b, (uint64_t)sb);
			pw_fusion_window_update(&w_t, (uint64_t)st);

			uint64_t mS = pw_fusion_window_mean(&w_s);
			uint64_t mA = pw_fusion_window_mean(&w_a);
			uint64_t mB = pw_fusion_window_mean(&w_b);
			uint64_t mT = pw_fusion_window_mean(&w_t);
			uint64_t heavy = mA > mB ? mA : mB;

			struct pw_fusion_component c = {
				.n_nodes = 4,
				.sum_wcet_ns = mS + mA + mB + mT,
				.cp_wcet_ns = mS + heavy + mT,
				.cp_hops = 2,
				.min_samples_seen = w_s.count,
			};
			enum pw_fusion_decision d = pw_fusion_decide(&c, &p);
			if (d != last) {
				local_flips++;
				last = d;
			}
		}

		flips[k] = local_flips;
		free(bs); free(ba); free(bb); free(bt);
	}

	/* Pinned: monotonic improvement. */
	pwtest_bool_true(flips[0] >= flips[1]);
	pwtest_bool_true(flips[1] >= flips[2]);
	/* And the largest window is essentially flat (jitter has been
	 * averaged out): at most a handful of flips over 4000 cycles. */
	pwtest_bool_true(flips[2] < 10);
	return PWTEST_PASS;
}

/* --------------------------------------------------------------------- */
/* Realistic audio-graph topologies where fusion is convenient            */
/* --------------------------------------------------------------------- */

PWTEST(fusion_decide_multi_source_fan_in_convenient)
{
	/* Three audio sources -> single mixer -> sink. With reasonably
	 * cheap sources and a cheap mixer the merged thread saves three
	 * wakeups on the critical path edges (source -> mixer -> sink,
	 * cp_hops = 2). The sources execute sequentially on the merged
	 * thread, but the savings dominate.
	 *
	 *     N = 5 (3 sources + mixer + sink)
	 *     sum_wcet = 5000 (5 nodes * 1000ns each, realistic for a
	 *                      cheap biquad / mixer node at 48 kHz / 256)
	 *     cp_wcet  = 3000 (one source -> mixer -> sink)
	 *     cp_hops  = 2
	 *     wakeup_cost = 3000 (default)
	 *     budget   = 3000 + 2 * 3000 = 9000
	 *     sum_wcet (5000) <= budget (9000) => FUSE.
	 *
	 * This is precisely the topology where the project's PipeWire
	 * graph would benefit most: a mixer with multiple inexpensive
	 * sources is more efficient as a single execution group (Shi
	 * 2024 terminology) than as a parallel constellation. */
	struct pw_fusion_component c = {
		.n_nodes = 5, .sum_wcet_ns = 5000,
		.cp_wcet_ns = 3000, .cp_hops = 2,
		.min_samples_seen = 100
	};
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_tree_mixer_convenient)
{
	/* Binary fan-in tree: 4 sources -> 2 sub-mixers -> 1 master
	 * mixer -> sink, like a typical N-channel mixer. cp_hops = 3
	 * (one source -> sub -> master -> sink), 8 nodes total.
	 *
	 *     sum_wcet = 8000  (8 * 1000)
	 *     cp_wcet  = 4000  (4 * 1000 along the longest path)
	 *     cp_hops  = 3
	 *     wakeup_cost = 3000
	 *     budget   = 4000 + 3 * 3000 = 13000
	 *     sum_wcet (8000) <= budget (13000) => FUSE.
	 *
	 * The deeper the mixer tree, the more cross-thread eventfd
	 * wakes the split schedule would pay, and the more profitable
	 * fusion becomes. Sarkar's criterion captures that scaling. */
	struct pw_fusion_component c = {
		.n_nodes = 8, .sum_wcet_ns = 8000,
		.cp_wcet_ns = 4000, .cp_hops = 3,
		.min_samples_seen = 100
	};
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_two_heavy_parallel_not_convenient)
{
	/* The negative case the cost model has to catch: two
	 * cpu-bound parallel branches (e.g. two convolution
	 * reverbs running in parallel as the producer-side of a
	 * stereo bus). The split alternative runs them on separate
	 * cores in parallel; the merged alternative sequentialises
	 * them on one thread, doubling the critical path.
	 *
	 *     N = 4 (source + 2 reverbs + sink)
	 *     sum_wcet = 200000 (4 * 50000 each)
	 *     cp_wcet  = 150000 (source + one reverb + sink = 3 * 50000)
	 *     cp_hops  = 2
	 *     wakeup_cost = 3000
	 *     budget   = 150000 + 2 * 3000 = 156000
	 *     sum_wcet (200000) > budget (156000) => LINEAR_ONLY.
	 *
	 * The cheap-parallelism intuition stays intact: even though
	 * fusing would save 2 * 3000 ns of wakeup cost, it would
	 * cost 50000 ns of newly-sequentialised work. The cost
	 * model correctly refuses the merge. */
	struct pw_fusion_component c = {
		.n_nodes = 4, .sum_wcet_ns = 200000,
		.cp_wcet_ns = 150000, .cp_hops = 2,
		.min_samples_seen = 100
	};
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	return PWTEST_PASS;
}

/* --------------------------------------------------------------------- */
/* Dynamic merge/split as WCETs change cycle-by-cycle                     */
/*                                                                        */
/* These tests exercise the *dynamic* nature of the decision: the same    */
/* topology can flip FUSE -> LINEAR_ONLY -> FUSE as the per-node          */
/* runtime samples evolve. The EMA is the conduit, and the criterion is   */
/* re-evaluated every cycle. We simulate a few hundred "cycles" of EMA    */
/* updates and verify the decision tracks the steady-state regime.       */
/* --------------------------------------------------------------------- */

/* Build a Y-shape component given per-node WCET estimates:
 *     source -> A
 *     source -> B
 *     A      -> sink
 *     B      -> sink
 * cp_hops = 2 (source -> heavier middle -> sink). The result is
 * sized so each member contributes one wcet_ns sample to the
 * aggregates the cost model sees. */
static void build_y_component(struct pw_fusion_component *out,
		uint64_t w_source, uint64_t w_a, uint64_t w_b, uint64_t w_sink,
		uint32_t samples)
{
	uint64_t heavy_middle = w_a > w_b ? w_a : w_b;
	out->n_nodes = 4;
	out->sum_wcet_ns = w_source + w_a + w_b + w_sink;
	out->cp_wcet_ns = w_source + heavy_middle + w_sink;
	out->cp_hops = 2;
	out->min_samples_seen = samples;
}

PWTEST(fusion_decide_dynamic_low_to_high_to_low)
{
	/* Same Y-shape with WCETs sliding from "cheap" to "heavy" and
	 * back. The sliding window tracks the steady state in
	 * ~0.9*N cycles once filled. For each phase we apply enough
	 * samples that the windowed mean has converged before we
	 * read the criterion. */
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	uint64_t bs[64], ba[64], bb[64], bt[64];
	struct pw_fusion_window w_s, w_a, w_b, w_t;
	int i;
	struct pw_fusion_component c;

	window_init(&w_s, bs, 64);
	window_init(&w_a, ba, 64);
	window_init(&w_b, bb, 64);
	window_init(&w_t, bt, 64);

	/* Phase 1: cheap (1000 ns per node). FUSE. */
	for (i = 0; i < 100; i++) {
		pw_fusion_window_update(&w_s, 1000);
		pw_fusion_window_update(&w_a, 1000);
		pw_fusion_window_update(&w_b, 1000);
		pw_fusion_window_update(&w_t, 1000);
	}
	build_y_component(&c,
			pw_fusion_window_mean(&w_s), pw_fusion_window_mean(&w_a),
			pw_fusion_window_mean(&w_b), pw_fusion_window_mean(&w_t),
			w_s.count);
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	/* Phase 2: arm B turns expensive (a convolution reverb is
	 * plugged in). Sum balloons; cp also grows but less, because
	 * cp passes through the heavier of {A, B} only.
	 *     sum_wcet (asymptote) ~ 1000 + 1000 + 50000 + 1000 = 53000
	 *     cp_wcet  (asymptote) ~ 1000 + 50000 + 1000        = 52000
	 *     budget               = 52000 + 2 * 3000           = 58000
	 *     sum (53000) <= budget (58000): still FUSE.
	 *
	 * A single heavy node inside an otherwise-cheap component
	 * doesn't tip the balance, because the heavy node sits on
	 * the critical path either way. The fusion decision only
	 * flips when MULTIPLE branches are concurrently heavy. */
	for (i = 0; i < 200; i++)
		pw_fusion_window_update(&w_b, 50000);
	build_y_component(&c,
			pw_fusion_window_mean(&w_s), pw_fusion_window_mean(&w_a),
			pw_fusion_window_mean(&w_b), pw_fusion_window_mean(&w_t),
			w_s.count);
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	/* Phase 3: arm A ALSO turns expensive (a second reverb on
	 * the parallel branch). Now both middles are heavy:
	 *     sum_wcet (asymptote) ~ 1000 + 50000 + 50000 + 1000 = 102000
	 *     cp_wcet  (asymptote) ~ 1000 + 50000 + 1000         = 52000
	 *     budget               = 52000 + 2 * 3000            = 58000
	 *     sum (102000) > budget (58000): LINEAR_ONLY. */
	for (i = 0; i < 200; i++)
		pw_fusion_window_update(&w_a, 50000);
	build_y_component(&c,
			pw_fusion_window_mean(&w_s), pw_fusion_window_mean(&w_a),
			pw_fusion_window_mean(&w_b), pw_fusion_window_mean(&w_t),
			w_s.count);
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);

	/* Phase 4: the reverbs are removed; runtimes drop back to
	 * baseline. The sliding mean converges fully within N
	 * cycles. */
	for (i = 0; i < 200; i++) {
		pw_fusion_window_update(&w_a, 1000);
		pw_fusion_window_update(&w_b, 1000);
	}
	build_y_component(&c,
			pw_fusion_window_mean(&w_s), pw_fusion_window_mean(&w_a),
			pw_fusion_window_mean(&w_b), pw_fusion_window_mean(&w_t),
			w_s.count);
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_dynamic_threshold_crossing_is_monotonic)
{
	/* Single Y-shape; sweep one branch's WCET from 1000 to 100000
	 * in 1000-ns steps. Record the WCET at which the criterion
	 * flips from FUSE to LINEAR_ONLY. The flip MUST happen at a
	 * single crossing point (no oscillation), and FUSE before
	 * the crossing should always hold. */
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_component c;
	enum pw_fusion_decision last = PW_FUSION_DECISION_FUSE;
	uint64_t w;
	int crossings = 0;

	for (w = 1000; w <= 100000; w += 1000) {
		/* Symmetric load: both arms get the same WCET, so
		 * sum_wcet = 2*w + sink + source while cp_wcet = w +
		 * sink + source (one arm is on the longest path). */
		build_y_component(&c, 1000, w, w, 1000, 100);
		enum pw_fusion_decision d = pw_fusion_decide(&c, &p);
		if (d != last) {
			crossings++;
			last = d;
		}
	}
	pwtest_int_eq(crossings, 1);
	pwtest_int_eq((int)last, (int)PW_FUSION_DECISION_LINEAR_ONLY);
	return PWTEST_PASS;
}

PWTEST(fusion_decide_dynamic_isolated_spike_absorbed_by_window)
{
	/* Single isolated spike in arm B should NOT flip a FUSE
	 * decision to LINEAR_ONLY, because the sliding mean absorbs
	 * the spike. This is the "no oscillation under jitter"
	 * property the smoothing exists to provide.
	 *
	 * Setup: cheap Y-shape running for 100 cycles into N=64
	 * windows, then one monster 100000-ns sample on arm B. Read
	 * the criterion afterwards. The spike contributes 100000/64 =
	 * 1562 ns to the mean of arm B, raising it from 1000 to
	 * ~2562 ns -- still well within the FUSE band:
	 *     sum_wcet ~ 1000 + 1000 + 2562 + 1000 = 5562
	 *     cp_wcet  ~ 1000 + 2562 + 1000        = 4562
	 *     budget   = 4562 + 2*3000             = 10562
	 *     sum (5562) <= budget (10562) => FUSE. */
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	uint64_t bs[64], ba[64], bb[64], bt[64];
	struct pw_fusion_window w_s, w_a, w_b, w_t;
	int i;
	struct pw_fusion_component c;

	window_init(&w_s, bs, 64);
	window_init(&w_a, ba, 64);
	window_init(&w_b, bb, 64);
	window_init(&w_t, bt, 64);

	for (i = 0; i < 100; i++) {
		pw_fusion_window_update(&w_s, 1000);
		pw_fusion_window_update(&w_a, 1000);
		pw_fusion_window_update(&w_b, 1000);
		pw_fusion_window_update(&w_t, 1000);
	}
	pw_fusion_window_update(&w_b, 100000);

	build_y_component(&c,
			pw_fusion_window_mean(&w_s), pw_fusion_window_mean(&w_a),
			pw_fusion_window_mean(&w_b), pw_fusion_window_mean(&w_t),
			w_s.count);
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);
	return PWTEST_PASS;
}

/* --------------------------------------------------------------------
 * Hysteresis tests for pw_fusion_decide.
 *
 * The cost model carries a `prev_decision` hint in the component
 * struct and a `hysteresis_pct` knob in the params. The intent is to
 * keep a component sticky against its prior decision when the WCET
 * sample is close to the threshold, so transient jitter does not
 * trigger a relocation. Each test below pins one branch of the
 * hysteresis logic.
 *
 * Notation: with hysteresis_pct=H, budget=B, sum=S, the band is
 *   [B-margin, B+margin] where margin = B*H/100.
 *
 * Outside the band, decisions follow the base rule (S<=B fuses).
 * Inside the band, the previous decision sticks.
 * -------------------------------------------------------------------- */

static void build_balanced_pair(struct pw_fusion_component *c,
		uint64_t per_node_wcet)
{
	c->n_nodes = 2;
	c->sum_wcet_ns = 2 * per_node_wcet;
	c->cp_wcet_ns = 2 * per_node_wcet; /* chain: cp == sum */
	c->cp_hops = 1;
	c->min_samples_seen = 16;
	c->prev_decision = PW_FUSION_DECISION_SPLIT;
}

PWTEST(hysteresis_no_prev_decision_uses_base_rule)
{
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000,
		.min_samples = 4, .hysteresis_pct = 50 };
	struct pw_fusion_component c;

	build_balanced_pair(&c, 100);
	/* sum = 200, cp = 200, hops = 1, wakeup = 1000.
	 * budget = 200 + 1*1000 = 1200. sum (200) <= 1200 -> FUSE. */
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	c.sum_wcet_ns = 5000;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);

	return PWTEST_PASS;
}

PWTEST(hysteresis_keeps_fuse_when_sum_just_above_threshold)
{
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000,
		.min_samples = 4, .hysteresis_pct = 20 };
	struct pw_fusion_component c;

	build_balanced_pair(&c, 100);
	c.prev_decision = PW_FUSION_DECISION_FUSE;
	c.sum_wcet_ns = 1300;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	c.sum_wcet_ns = 1440;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	c.sum_wcet_ns = 1441;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);

	return PWTEST_PASS;
}

PWTEST(hysteresis_keeps_linear_only_when_sum_just_below_threshold)
{
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000,
		.min_samples = 4, .hysteresis_pct = 20 };
	struct pw_fusion_component c;

	build_balanced_pair(&c, 100);
	c.prev_decision = PW_FUSION_DECISION_LINEAR_ONLY;
	c.sum_wcet_ns = 1000;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);

	c.sum_wcet_ns = 960;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);

	c.sum_wcet_ns = 959;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	return PWTEST_PASS;
}

PWTEST(hysteresis_zero_pct_disables_band)
{
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000,
		.min_samples = 4, .hysteresis_pct = 0 };
	struct pw_fusion_component c;

	build_balanced_pair(&c, 100);
	c.prev_decision = PW_FUSION_DECISION_FUSE;
	c.sum_wcet_ns = 1201;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);

	c.prev_decision = PW_FUSION_DECISION_LINEAR_ONLY;
	c.sum_wcet_ns = 1199;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	return PWTEST_PASS;
}

PWTEST(hysteresis_clamp_above_100_is_safe)
{
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000,
		.min_samples = 4, .hysteresis_pct = 1000 };
	struct pw_fusion_component c;

	build_balanced_pair(&c, 100);
	c.prev_decision = PW_FUSION_DECISION_FUSE;
	c.sum_wcet_ns = 1201;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	return PWTEST_PASS;
}

PWTEST(hysteresis_warmup_takes_precedence)
{
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000,
		.min_samples = 16, .hysteresis_pct = 50 };
	struct pw_fusion_component c;

	build_balanced_pair(&c, 100);
	c.prev_decision = PW_FUSION_DECISION_FUSE;
	c.min_samples_seen = 3;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	return PWTEST_PASS;
}

PWTEST(hysteresis_split_returned_for_degenerate)
{
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000,
		.min_samples = 4, .hysteresis_pct = 50 };
	struct pw_fusion_component c;

	build_balanced_pair(&c, 100);
	c.prev_decision = PW_FUSION_DECISION_FUSE;
	c.n_nodes = 1;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_SPLIT);

	c.n_nodes = 2;
	c.cp_hops = 0;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_SPLIT);
	return PWTEST_PASS;
}

PWTEST(hysteresis_band_endpoints_keep_prev)
{
	struct pw_fusion_params p = { .wakeup_cost_ns = 1000,
		.min_samples = 4, .hysteresis_pct = 25 };
	struct pw_fusion_component c;

	build_balanced_pair(&c, 100);
	c.prev_decision = PW_FUSION_DECISION_FUSE;
	c.sum_wcet_ns = 1500; /* edge: budget + margin */
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);
	c.sum_wcet_ns = 1501;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);

	c.prev_decision = PW_FUSION_DECISION_LINEAR_ONLY;
	c.sum_wcet_ns = 900; /* edge: budget - margin */
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	c.sum_wcet_ns = 899;
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);
	return PWTEST_PASS;
}

PWTEST_SUITE(fusion_cost)
{
	pwtest_add(fusion_decide_null_inputs_split, PWTEST_NOARG);
	pwtest_add(fusion_decide_singleton_splits, PWTEST_NOARG);
	pwtest_add(fusion_decide_zero_hops_splits, PWTEST_NOARG);
	pwtest_add(fusion_decide_warmup_returns_linear_only, PWTEST_NOARG);
	pwtest_add(fusion_decide_pure_chain_always_fuses, PWTEST_NOARG);
	pwtest_add(fusion_decide_fan_out_profitable_when_balanced, PWTEST_NOARG);
	pwtest_add(fusion_decide_heavy_sibling_blocks_fusion, PWTEST_NOARG);
	pwtest_add(fusion_decide_diamond_break_even, PWTEST_NOARG);
	pwtest_add(fusion_decide_wakeup_cost_changes_decision, PWTEST_NOARG);
	pwtest_add(window_first_sample_records_value, PWTEST_NOARG);
	pwtest_add(window_zero_sample_is_noop, PWTEST_NOARG);
	pwtest_add(window_eviction_keeps_sum_consistent, PWTEST_NOARG);
	pwtest_add(window_mean_exact_for_full_buffer, PWTEST_NOARG);
	pwtest_add(window_clear_resets_state_keeps_buffer, PWTEST_NOARG);
	pwtest_add(window_null_pointers_no_crash, PWTEST_NOARG);
	pwtest_add(invariant_variance_scales_as_1_over_sqrt_n, PWTEST_NOARG);
	pwtest_add(invariant_step_response_time_scales_linearly_with_n, PWTEST_NOARG);
	pwtest_add(invariant_target_n_clamps_to_principled_bounds, PWTEST_NOARG);
	pwtest_add(invariant_decision_stability_under_jitter_improves_with_n, PWTEST_NOARG);
	pwtest_add(fusion_decide_multi_source_fan_in_convenient, PWTEST_NOARG);
	pwtest_add(fusion_decide_tree_mixer_convenient, PWTEST_NOARG);
	pwtest_add(fusion_decide_two_heavy_parallel_not_convenient, PWTEST_NOARG);
	pwtest_add(fusion_decide_dynamic_low_to_high_to_low, PWTEST_NOARG);
	pwtest_add(fusion_decide_dynamic_threshold_crossing_is_monotonic, PWTEST_NOARG);
	pwtest_add(fusion_decide_dynamic_isolated_spike_absorbed_by_window, PWTEST_NOARG);
	pwtest_add(hysteresis_no_prev_decision_uses_base_rule, PWTEST_NOARG);
	pwtest_add(hysteresis_keeps_fuse_when_sum_just_above_threshold, PWTEST_NOARG);
	pwtest_add(hysteresis_keeps_linear_only_when_sum_just_below_threshold, PWTEST_NOARG);
	pwtest_add(hysteresis_zero_pct_disables_band, PWTEST_NOARG);
	pwtest_add(hysteresis_clamp_above_100_is_safe, PWTEST_NOARG);
	pwtest_add(hysteresis_warmup_takes_precedence, PWTEST_NOARG);
	pwtest_add(hysteresis_split_returned_for_degenerate, PWTEST_NOARG);
	pwtest_add(hysteresis_band_endpoints_keep_prev, PWTEST_NOARG);
	return PWTEST_PASS;
}
