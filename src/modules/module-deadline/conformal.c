/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * conformal.c -- adaptive-conformal upper-runtime-budget estimator.
 *
 * The estimator owns:
 *   - a configuration snapshot (rt_conformal_config), validated at
 *     create() and otherwise frozen,
 *   - an EWMA location/scale pair driven by the absolute-deviation
 *     scale estimator (Hampel 1974; see also Brown 1959 for the
 *     exponentially-weighted moving average primitive),
 *   - a fixed-size score ring with capacity = cfg.window holding
 *     per-activation normalised nonconformity scores,
 *   - a fixed-size scratch buffer used to compute the rolling
 *     empirical quantile of the score ring by O(W log W) sort
 *     outside the realtime path,
 *   - the adaptive alpha_eff (initialised to alpha_target; the
 *     online update lands in a subsequent commit),
 *   - overrun / burst counters and per-activation diagnostic
 *     last_* fields.
 *
 * The observation flow follows the prequential discipline: the
 * score s_t for sample x_t is computed against the EWMA state as it
 * existed BEFORE the call (mu_pred, scale_pred). s_t is inserted
 * into the ring. The EWMA pair is updated by x_t. Only after that
 * does the budget for the NEXT activation become computable via
 * rt_conformal_budget(); the empirical quantile is recomputed lazily
 * every recalc_period observations (a unit test pins the
 * no-look-ahead property by inspecting the ring contents around the
 * observe / budget interleaving).
 *
 * Realtime safety: rt_conformal_observe() walks O(1) in the steady
 * state -- one ring insert, one EWMA update, one comparison for the
 * overrun counter. The quantile recompute is gated behind
 * recalc_period and consumes a per-instance scratch buffer
 * allocated once at create() time; the recompute itself is O(W log W)
 * but only fires every recalc_period observations and runs on the
 * audio recalc worker, never on the RT process() thread.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include "conformal.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void rt_conformal_config_defaults(struct rt_conformal_config *cfg)
{
	if (cfg == NULL)
		return;
	memset(cfg, 0, sizeof(*cfg));
	/*
	 * Tuning rationale (recent calibration against polyphonic synth
	 * + filter-chain workloads):
	 *
	 * - alpha_target 1e-3 (was 1e-4): target the 99.9 % one-sided
	 *   quantile. The previous 1e-4 calibration aimed at a tighter
	 *   tail, but the finite-sample conformal index
	 *
	 *       k = ceil((window + 1) * (1 - alpha_eff))
	 *
	 *   clamps to `window` whenever (window + 1) * alpha < 1: with
	 *   the default window 4096 every alpha below ~2.44e-4 reduces
	 *   to "quantile == max score in the ring", so a single
	 *   contaminating sample (a plugin first-touch slipping past the
	 *   warm-up window, a kernel preemption charged to the
	 *   follower's CPU-time counter) dominated the published budget
	 *   for the full lifetime of the ring -- the live trace at
	 *   /tmp/coppwr-deadline-conformal.jsonl shows this directly,
	 *   with one bad sample driving last_prediction_ns up to
	 *   ~period for thousands of cycles. At alpha 1e-3 the rank
	 *   sits about three below the max, so two or three extreme
	 *   samples must coexist in the ring before the quantile
	 *   follows them, which is the actual definition of a
	 *   sustained-spike regime.
	 *
	 * - alpha_min 5e-4 (was 1e-5): kept above 1/(window + 1) for
	 *   the default window so the adaptive descent on overruns can
	 *   still drop alpha_eff toward a tighter quantile without
	 *   ever collapsing back to "quantile == max" behaviour. With
	 *   window 4096 the floor 5e-4 leaves the rank two below the
	 *   max even at the bottom of the alpha range.
	 *
	 * - alpha_max 5e-3 (was 5e-2): adaptive_conformal must not
	 *   relax above the 99.5 % quantile during the quiet intervals
	 *   between bursts. The previous 5e-2 ceiling let alpha_eff
	 *   drift into the 95 % regime, which dropped the published
	 *   budget below the peak score in the ring and made the next
	 *   burst overrun the reservation.
	 *
	 * - window 4096 (was 1024): at the common 42 ms graph period
	 *   the score ring now retains observed peaks for ~170 s
	 *   instead of ~43 s, so a sustained-spike trace that fits in
	 *   a few seconds of activity still influences the budget two
	 *   minutes later. RT_CONFORMAL_MAX_WINDOW remains the ceiling.
	 *
	 * - ewma_scale_lambda 0.005 (was 0.05): the EWMA scale
	 *   multiplies the stored score in the budget formula. The
	 *   previous 0.05 gain decayed the elevated scale within
	 *   ~14 samples (~0.6 s) after a burst, collapsing
	 *   Q * scale -- and therefore the budget -- back to baseline
	 *   even though the peak score was still in the ring. The new
	 *   gain keeps the scale's memory of a burst alive for ~140
	 *   samples (~6 s) so the budget tracks the score ring.
	 *
	 * - ewma_location_lambda 0.01 (was 0.05): paired slowing on
	 *   the location predictor for similar reasons; mu now half-
	 *   decays over ~70 samples (~3 s) and stays above the quiet-
	 *   interval mean long enough to hold the prediction tight to
	 *   recent peak behaviour.
	 *
	 * - bootstrap_runtime_ns 100 us (was 0): a freshly admitted
	 *   follower spends its first bootstrap_min_samples
	 *   activations on the bootstrap floor; publishing 0
	 *   collapsed to runtime_floor_ns (1 us) and the placer
	 *   excluded the node from the DAG until a real sample
	 *   arrived (wcet=0 gate). 100 us is large enough that the
	 *   node enters the DAG immediately with a usable runtime
	 *   estimate, but small enough that several simultaneously
	 *   bootstrapping followers do not collectively push the
	 *   critical path past the global deadline and trip the
	 *   strict feasibility gate.
	 *
	 * - burst_threshold 2, burst_penalty 4.0 (Gibbs & Candes 2021
	 *   §4): after two consecutive overruns the alpha_eff descent
	 *   is multiplied by four. The asymmetric response tightens
	 *   the quantile faster than the symmetric eta * (target -
	 *   event) update, which matters in a polyphonic synth where
	 *   bursts span dozens of cycles.
	 *
	 * - shift_burst_threshold 4 (was 8): the SHIFT marker fires
	 *   sooner so the diagnostic surface flags a sustained-spike
	 *   regime before half a second of overruns have accumulated.
	 */
	cfg->alpha_target          = 1e-3;
	cfg->alpha_min             = 5e-4;
	cfg->alpha_max             = 5e-3;
	cfg->eta                   = 0.005;
	cfg->ewma_location_lambda  = 0.01;
	cfg->ewma_scale_lambda     = 0.005;
	cfg->window                = 4096;
	cfg->bootstrap_min_samples = 64;
	cfg->bootstrap_runtime_ns  = 100000;
	cfg->guard_ns              = 1500;
	cfg->guard_percent         = 0.05;
	cfg->sigma_floor_ns        = 1;
	cfg->runtime_floor_ns      = 1000;
	cfg->recalc_period         = 1;
	cfg->compatible_history    = true;
	cfg->risk_allocation       = RT_CONF_RISK_ALLOC_UNIFORM;
	cfg->max_update_cost_ns    = 5000;
	cfg->trace_export          = false;
	cfg->burst_threshold       = 2;
	cfg->burst_penalty         = 4.0;
	cfg->shift_burst_threshold = 4;
}

int rt_conformal_config_validate(const struct rt_conformal_config *cfg)
{
	if (cfg == NULL)
		return -EINVAL;
	if (!(cfg->alpha_target > 0.0 && cfg->alpha_target < 1.0))
		return -EINVAL;
	if (!(cfg->alpha_min > 0.0 && cfg->alpha_min <= cfg->alpha_target))
		return -EINVAL;
	if (!(cfg->alpha_max >= cfg->alpha_target && cfg->alpha_max < 1.0))
		return -EINVAL;
	if (!(cfg->eta > 0.0 && cfg->eta < 1.0))
		return -EINVAL;
	if (!(cfg->ewma_location_lambda > 0.0 && cfg->ewma_location_lambda < 1.0))
		return -EINVAL;
	if (!(cfg->ewma_scale_lambda > 0.0 && cfg->ewma_scale_lambda < 1.0))
		return -EINVAL;
	if (cfg->window < 2 || cfg->window > RT_CONFORMAL_MAX_WINDOW)
		return -EINVAL;
	if (cfg->bootstrap_min_samples < 2)
		return -EINVAL;
	if (!(cfg->guard_percent >= 0.0 && cfg->guard_percent < 1.0))
		return -EINVAL;
	if (cfg->recalc_period < 1)
		return -EINVAL;
	if (cfg->risk_allocation != RT_CONF_RISK_ALLOC_UNIFORM &&
	    cfg->risk_allocation != RT_CONF_RISK_ALLOC_DENSITY_WEIGHTED &&
	    cfg->risk_allocation != RT_CONF_RISK_ALLOC_SLOPE_WEIGHTED)
		return -EINVAL;
	if (!isfinite(cfg->alpha_target) || !isfinite(cfg->alpha_min) ||
	    !isfinite(cfg->alpha_max)    || !isfinite(cfg->eta) ||
	    !isfinite(cfg->ewma_location_lambda) ||
	    !isfinite(cfg->ewma_scale_lambda) ||
	    !isfinite(cfg->guard_percent))
		return -EINVAL;
	if (!(cfg->burst_penalty >= 1.0 && cfg->burst_penalty <= 1e6))
		return -EINVAL;
	return 0;
}

const char *rt_conformal_state_name(enum rt_conformal_state s)
{
	switch (s) {
	case RT_CONF_INSUFFICIENT_DATA: return "insufficient_data";
	case RT_CONF_BOOTSTRAP:         return "bootstrap";
	case RT_CONF_VALID:             return "valid";
	case RT_CONF_SHIFT:             return "shift";
	case RT_CONF_DISABLED:          return "disabled";
	}
	return "unknown";
}

const char *rt_conformal_invalidation_reason_name(
		enum rt_conformal_invalidation_reason r)
{
	switch (r) {
	case RT_CONF_INVALIDATED_NONE:                return "none";
	case RT_CONF_INVALIDATED_PERIOD:              return "period";
	case RT_CONF_INVALIDATED_FUSION_GROUP:        return "fusion_group";
	case RT_CONF_INVALIDATED_CPU_CLASS:           return "cpu_class";
	case RT_CONF_INVALIDATED_OPERATOR_REQUEST:    return "operator_request";
	case RT_CONF_INVALIDATED_PLUGIN_MODE:         return "plugin_mode";
	}
	return "unknown";
}

const char *rt_conformal_risk_allocation_name(
		enum rt_conformal_risk_allocation a)
{
	switch (a) {
	case RT_CONF_RISK_ALLOC_UNIFORM:          return "uniform";
	case RT_CONF_RISK_ALLOC_DENSITY_WEIGHTED: return "density_weighted";
	case RT_CONF_RISK_ALLOC_SLOPE_WEIGHTED:   return "slope_weighted";
	}
	return "unknown";
}

struct rt_conformal {
	struct rt_conformal_config cfg;
	enum rt_conformal_state    state;
	enum rt_conformal_invalidation_reason last_invalidation_reason;

	/* Counters. */
	uint64_t samples_seen;        /* every call to rt_conformal_observe */
	uint64_t samples_used;        /* observations actually scored */
	uint64_t overruns_seen;       /* cumulative budget exceedances */
	uint64_t recent_overruns;     /* overruns in current window */
	uint64_t current_overrun_burst;
	uint64_t max_overrun_burst;

	/* Adaptive parameters. alpha_eff starts at alpha_target; the
	 * online update lands in a follow-up commit. */
	double   alpha_eff;

	/* EWMA pair. mu is the location estimate; scale tracks
	 * EWMA(|x - mu_pred|), the absolute-deviation scale used as a
	 * robust proxy for the population sigma. */
	double   mu;
	double   scale;
	bool     ewma_initialised;

	/* Last-cycle diagnostics. */
	double   last_prediction_ns;
	double   last_score;
	uint64_t last_budget_ns;
	uint64_t last_runtime_ns;

	/* Score ring. capacity == cfg.window; entries in [0, ring_count). */
	double  *score_ring;
	uint64_t *runtime_ring;
	uint32_t ring_head;
	uint32_t ring_count;

	/* Scratch for the O(W log W) quantile recompute. Pre-allocated
	 * at create() so the steady-state observe path never touches
	 * malloc. */
	double  *sort_scratch;

	/* Cached quantile result and recompute throttle. */
	uint32_t samples_since_recalc;
	double   cached_score_quantile;
	bool     quantile_valid;
};

static int cmp_double_asc(const void *a, const void *b)
{
	double x = *(const double *)a;
	double y = *(const double *)b;
	if (x < y) return -1;
	if (x > y) return 1;
	return 0;
}

static uint64_t clamp_u64(uint64_t v, uint64_t lo, uint64_t hi)
{
	if (v < lo) return lo;
	if (hi != 0 && v > hi) return hi;
	return v;
}

/*
 * Recompute the empirical (1 - alpha_eff) quantile of the score ring
 * using the finite-sample conformal index convention. With n entries
 * and probability level p = 1 - alpha_eff, the sorted-index is
 *
 *     k = ceil((n + 1) * p)   clamped to [1, n]
 *
 * (Romano, Patterson & Candes 2019 §3 cites this convention as the
 * exchangeability-preserving choice; for finite n it is the
 * conservative empirical quantile.) The function sorts a scratch
 * copy of the ring into ascending order and returns score_sorted[k-1].
 *
 * The empty-ring case returns 0.0; the caller treats that as "no
 * score adjustment" and falls back on the bootstrap branch.
 */
static double compute_score_quantile(rt_conformal_t *e)
{
	uint32_t n = e->ring_count;
	uint32_t i;
	double   p;
	double   q_idx;
	uint32_t k;

	if (n == 0)
		return 0.0;

	for (i = 0; i < n; i++)
		e->sort_scratch[i] = e->score_ring[i];
	qsort(e->sort_scratch, n, sizeof(double), cmp_double_asc);

	p = 1.0 - e->alpha_eff;
	if (!(p > 0.0))
		return e->sort_scratch[0];
	if (p >= 1.0)
		return e->sort_scratch[n - 1];

	q_idx = ceil((double)(n + 1u) * p);
	if (q_idx < 1.0)
		k = 1;
	else if (q_idx > (double)n)
		k = n;
	else
		k = (uint32_t)q_idx;
	return e->sort_scratch[k - 1u];
}

static void ring_insert(rt_conformal_t *e, double score, uint64_t runtime)
{
	uint32_t cap = e->cfg.window;
	e->score_ring[e->ring_head] = score;
	e->runtime_ring[e->ring_head] = runtime;
	e->ring_head = (e->ring_head + 1u) % cap;
	if (e->ring_count < cap)
		e->ring_count++;
}

rt_conformal_t *rt_conformal_create(const struct rt_conformal_config *cfg)
{
	rt_conformal_t *e;

	if (rt_conformal_config_validate(cfg) != 0)
		return NULL;

	e = calloc(1, sizeof(*e));
	if (e == NULL)
		return NULL;

	e->cfg = *cfg;
	e->state = RT_CONF_INSUFFICIENT_DATA;
	e->last_invalidation_reason = RT_CONF_INVALIDATED_NONE;
	e->alpha_eff = cfg->alpha_target;

	e->score_ring   = calloc(cfg->window, sizeof(double));
	e->runtime_ring = calloc(cfg->window, sizeof(uint64_t));
	e->sort_scratch = calloc(cfg->window, sizeof(double));
	if (e->score_ring == NULL || e->runtime_ring == NULL ||
	    e->sort_scratch == NULL) {
		free(e->score_ring);
		free(e->runtime_ring);
		free(e->sort_scratch);
		free(e);
		return NULL;
	}

	return e;
}

void rt_conformal_destroy(rt_conformal_t *e)
{
	if (e == NULL)
		return;
	free(e->score_ring);
	free(e->runtime_ring);
	free(e->sort_scratch);
	free(e);
}

void rt_conformal_invalidate(rt_conformal_t *e,
		enum rt_conformal_invalidation_reason reason)
{
	if (e == NULL)
		return;
	e->state = RT_CONF_INSUFFICIENT_DATA;
	e->last_invalidation_reason = reason;
	e->samples_used = 0;
	e->overruns_seen = 0;
	e->recent_overruns = 0;
	e->current_overrun_burst = 0;
	e->max_overrun_burst = 0;
	e->mu = 0.0;
	e->scale = 0.0;
	e->ewma_initialised = false;
	e->last_prediction_ns = 0.0;
	e->last_score = 0.0;
	e->last_budget_ns = 0;
	e->last_runtime_ns = 0;
	e->ring_head = 0;
	e->ring_count = 0;
	e->samples_since_recalc = 0;
	e->cached_score_quantile = 0.0;
	e->quantile_valid = false;
	memset(e->score_ring, 0, sizeof(double) * e->cfg.window);
	memset(e->runtime_ring, 0, sizeof(uint64_t) * e->cfg.window);
	e->alpha_eff = e->cfg.alpha_target;
}

/*
 * Internal: classify the state after an observation has updated the
 * counters. The state machine:
 *
 *   DISABLED is sticky -- only rt_conformal_enable can clear it.
 *   samples_used == 0                                -> INSUFFICIENT_DATA
 *   samples_used < bootstrap_min_samples / 2         -> BOOTSTRAP
 *   ring_count   < bootstrap_min_samples / 2         -> BOOTSTRAP
 *   current_overrun_burst >= shift_burst_threshold   -> SHIFT
 *   otherwise                                        -> VALID
 *
 * The SHIFT classification is a diagnostic surface: the alpha_eff
 * update continues progressing while in SHIFT (it does not freeze).
 * An operator can use the state as a signal to investigate
 * sustained overruns; the typed reason vocabulary lets a future
 * commit cross-reference SHIFT with the worker's
 * "report-blocking-observation" path.
 */
static void recompute_state(rt_conformal_t *e)
{
	uint32_t half_boot;
	if (e->state == RT_CONF_DISABLED)
		return;
	if (e->samples_used == 0) {
		e->state = RT_CONF_INSUFFICIENT_DATA;
		return;
	}
	half_boot = e->cfg.bootstrap_min_samples / 2u;
	if (half_boot < 1)
		half_boot = 1;
	if (e->samples_used < e->cfg.bootstrap_min_samples ||
	    e->ring_count   < half_boot) {
		e->state = RT_CONF_BOOTSTRAP;
		return;
	}
	if (e->cfg.shift_burst_threshold > 0 &&
	    e->current_overrun_burst >= e->cfg.shift_burst_threshold) {
		e->state = RT_CONF_SHIFT;
		return;
	}
	e->state = RT_CONF_VALID;
}

bool rt_conformal_observe(rt_conformal_t *e, uint64_t runtime_ns)
{
	double sample;
	double sigma_pred;
	double mu_pred;
	double score;
	uint64_t budget_for_this;
	bool triggered_recalc = false;

	if (e == NULL)
		return false;
	e->samples_seen++;
	if (runtime_ns == 0)
		return false;
	if (e->state == RT_CONF_DISABLED)
		return false;

	/*
	 * Clamp the sample to the configured runtime floor before
	 * scoring. A zero-or-tiny sample (sub-clock-resolution) would
	 * otherwise depress the EWMA scale and inflate later scores.
	 */
	if (runtime_ns < e->cfg.runtime_floor_ns)
		runtime_ns = e->cfg.runtime_floor_ns;

	sample = (double)runtime_ns;
	if (!isfinite(sample))
		return false;

	/* The score is computed against the EWMA state as it stood
	 * BEFORE this observation. Snapshot now so the post-update
	 * mu/scale do not leak into the score. */
	if (!e->ewma_initialised) {
		mu_pred = sample;
		sigma_pred = 0.0;
	} else {
		mu_pred    = e->mu;
		sigma_pred = e->scale;
	}

	{
		double denom = sigma_pred + (double)e->cfg.sigma_floor_ns;
		if (denom <= 0.0)
			denom = 1.0;
		score = (sample - mu_pred) / denom;
		if (!isfinite(score))
			score = 0.0;
	}

	/* Reconstruct the budget that would have been emitted for the
	 * activation we are now observing -- the prequential
	 * "predicted_t" -- without consulting x_t itself. The cached
	 * quantile reflects the ring as it stood before this insert.
	 *
	 * On the first observation the EWMA pair is uninitialised, so
	 * mu_pred / sigma_pred have no prior state to draw on. The
	 * estimator behaves as if rt_conformal_budget had been called
	 * before any sample arrived: bootstrap_runtime_ns (or the
	 * runtime floor) is what would have been active. Computing
	 * mu_pred from x_t itself in that case would leak x_t into the
	 * reconstructed budget -- exactly the look-ahead the
	 * prequential rule forbids. */
	if (!e->ewma_initialised) {
		uint64_t boot = e->cfg.bootstrap_runtime_ns > e->cfg.runtime_floor_ns
			? e->cfg.bootstrap_runtime_ns : e->cfg.runtime_floor_ns;
		budget_for_this = clamp_u64(boot, e->cfg.runtime_floor_ns, 0);
	} else {
		double q = e->quantile_valid ? e->cached_score_quantile : 0.0;
		double pred = mu_pred + (q > 0.0 ? q : 0.0) *
				(sigma_pred + (double)e->cfg.sigma_floor_ns);
		double guarded = pred * (1.0 + e->cfg.guard_percent) +
				(double)e->cfg.guard_ns;
		double rounded = ceil(guarded);
		uint64_t b;
		if (!isfinite(rounded) || rounded < 0.0)
			rounded = 0.0;
		if (rounded > (double)UINT64_MAX)
			b = UINT64_MAX;
		else
			b = (uint64_t)rounded;
		budget_for_this = clamp_u64(b, e->cfg.runtime_floor_ns, 0);
	}

	/* Overrun on the budget that was active for activation t.
	 * overrun_t in {0, 1} drives the adaptive alpha update below. */
	{
		bool overrun = runtime_ns > budget_for_this && budget_for_this > 0;
		double overrun_t = overrun ? 1.0 : 0.0;
		double step;
		double new_alpha;

		if (overrun) {
			e->overruns_seen++;
			e->recent_overruns++;
			e->current_overrun_burst++;
			if (e->current_overrun_burst > e->max_overrun_burst)
				e->max_overrun_burst = e->current_overrun_burst;
		} else {
			e->current_overrun_burst = 0;
		}

		/*
		 * Adaptive conformal inference update under distribution
		 * shift (Gibbs & Candes 2021):
		 *
		 *   alpha_eff_{t+1} = alpha_eff_t
		 *                    + eta * (alpha_target - overrun_t)
		 *                    clamped to [alpha_min, alpha_max]
		 *
		 * Interpretation: a sample that exceeded budget_t pushes
		 * alpha_eff downward (tighter next quantile, more
		 * conservative); a non-overrun pushes it upward slowly.
		 *
		 * Optional burst-penalty extension: when the current
		 * consecutive-overrun streak reaches the configured
		 * threshold, the negative update is multiplied by
		 * burst_penalty so the next quantile tightens faster.
		 */
		step = e->cfg.eta * (e->cfg.alpha_target - overrun_t);
		if (overrun && e->cfg.burst_threshold > 0 &&
		    e->current_overrun_burst >= e->cfg.burst_threshold &&
		    e->cfg.burst_penalty > 1.0) {
			step *= e->cfg.burst_penalty;
		}
		new_alpha = e->alpha_eff + step;
		if (new_alpha < e->cfg.alpha_min)
			new_alpha = e->cfg.alpha_min;
		if (new_alpha > e->cfg.alpha_max)
			new_alpha = e->cfg.alpha_max;
		e->alpha_eff = new_alpha;
	}

	/* Insert (score, runtime) into the ring, then update the
	 * EWMA pair using x_t. */
	ring_insert(e, score, runtime_ns);

	if (!e->ewma_initialised) {
		e->mu = sample;
		e->scale = 0.0;
		e->ewma_initialised = true;
	} else {
		double lam_mu = e->cfg.ewma_location_lambda;
		double lam_sc = e->cfg.ewma_scale_lambda;
		double abs_dev = fabs(sample - mu_pred);
		e->mu    = (1.0 - lam_mu) * e->mu    + lam_mu * sample;
		e->scale = (1.0 - lam_sc) * e->scale + lam_sc * abs_dev;
	}

	e->samples_used++;
	e->last_runtime_ns = runtime_ns;
	e->last_score = score;
	e->last_budget_ns = budget_for_this;
	e->last_prediction_ns =
		(double)mu_pred + (e->quantile_valid ? e->cached_score_quantile : 0.0)
		* (sigma_pred + (double)e->cfg.sigma_floor_ns);
	if (!isfinite(e->last_prediction_ns))
		e->last_prediction_ns = 0.0;

	e->samples_since_recalc++;
	if (e->samples_since_recalc >= e->cfg.recalc_period) {
		e->cached_score_quantile = compute_score_quantile(e);
		e->samples_since_recalc = 0;
		e->quantile_valid = true;
		triggered_recalc = true;
	}

	recompute_state(e);

	return triggered_recalc;
}

uint64_t rt_conformal_budget(rt_conformal_t *e, uint64_t period_ns)
{
	double pred;
	double guarded;
	double rounded;
	uint64_t budget;
	double q;
	uint64_t floor_ns;
	uint64_t bootstrap;

	if (e == NULL)
		return 0;
	if (e->state == RT_CONF_DISABLED)
		return 0;

	floor_ns = e->cfg.runtime_floor_ns;
	bootstrap = e->cfg.bootstrap_runtime_ns;

	if (e->state == RT_CONF_INSUFFICIENT_DATA ||
	    e->state == RT_CONF_BOOTSTRAP) {
		uint64_t b = bootstrap > floor_ns ? bootstrap : floor_ns;
		return clamp_u64(b, floor_ns, period_ns);
	}

	q = e->quantile_valid ? e->cached_score_quantile : 0.0;
	pred = e->mu + (q > 0.0 ? q : 0.0) *
			(e->scale + (double)e->cfg.sigma_floor_ns);
	guarded = pred * (1.0 + e->cfg.guard_percent) +
			(double)e->cfg.guard_ns;
	rounded = ceil(guarded);
	if (!isfinite(rounded) || rounded < 0.0)
		rounded = 0.0;
	if (rounded > (double)UINT64_MAX)
		budget = UINT64_MAX;
	else
		budget = (uint64_t)rounded;

	return clamp_u64(budget, floor_ns, period_ns);
}

enum rt_conformal_state rt_conformal_state(const rt_conformal_t *e)
{
	return e ? e->state : RT_CONF_INSUFFICIENT_DATA;
}

uint64_t rt_conformal_samples_seen(const rt_conformal_t *e)
{
	return e ? e->samples_seen : 0;
}

uint64_t rt_conformal_samples_used(const rt_conformal_t *e)
{
	return e ? e->samples_used : 0;
}

uint64_t rt_conformal_overruns_seen(const rt_conformal_t *e)
{
	return e ? e->overruns_seen : 0;
}

uint64_t rt_conformal_recent_overruns(const rt_conformal_t *e)
{
	return e ? e->recent_overruns : 0;
}

uint64_t rt_conformal_max_overrun_burst(const rt_conformal_t *e)
{
	return e ? e->max_overrun_burst : 0;
}

uint64_t rt_conformal_current_overrun_burst(const rt_conformal_t *e)
{
	return e ? e->current_overrun_burst : 0;
}

double rt_conformal_alpha_eff(const rt_conformal_t *e)
{
	return e ? e->alpha_eff : 0.0;
}

double rt_conformal_mu_ns(const rt_conformal_t *e)
{
	return e ? e->mu : 0.0;
}

double rt_conformal_scale_ns(const rt_conformal_t *e)
{
	return e ? e->scale : 0.0;
}

double rt_conformal_last_prediction_ns(const rt_conformal_t *e)
{
	return e ? e->last_prediction_ns : 0.0;
}

double rt_conformal_last_score(const rt_conformal_t *e)
{
	return e ? e->last_score : 0.0;
}

uint64_t rt_conformal_last_budget_ns(const rt_conformal_t *e)
{
	return e ? e->last_budget_ns : 0;
}

uint64_t rt_conformal_last_runtime_ns(const rt_conformal_t *e)
{
	return e ? e->last_runtime_ns : 0;
}

double rt_conformal_score_quantile(const rt_conformal_t *e)
{
	if (e == NULL)
		return 0.0;
	return e->quantile_valid ? e->cached_score_quantile : 0.0;
}

uint64_t rt_conformal_guard_ns_effective(const rt_conformal_t *e)
{
	return e ? e->cfg.guard_ns : 0;
}

enum rt_conformal_invalidation_reason
rt_conformal_last_invalidation_reason(const rt_conformal_t *e)
{
	return e ? e->last_invalidation_reason : RT_CONF_INVALIDATED_NONE;
}

size_t rt_conformal_state_data_size(void)
{
	return sizeof(struct rt_conformal);
}

double rt_conformal_burst_penalty(const rt_conformal_t *e)
{
	return e ? e->cfg.burst_penalty : 1.0;
}

uint32_t rt_conformal_burst_threshold(const rt_conformal_t *e)
{
	return e ? e->cfg.burst_threshold : 0;
}

void rt_conformal_disable(rt_conformal_t *e)
{
	if (e == NULL)
		return;
	e->state = RT_CONF_DISABLED;
}

void rt_conformal_enable(rt_conformal_t *e)
{
	if (e == NULL)
		return;
	if (e->state == RT_CONF_DISABLED) {
		e->state = e->samples_used > 0
			? RT_CONF_BOOTSTRAP : RT_CONF_INSUFFICIENT_DATA;
		recompute_state(e);
	}
}

/* ------------------------------------------------------------------
 * Mode-keyed estimator table
 *
 * A small fixed-cap open-addressed cache: each entry pairs a mode key
 * with a per-mode rt_conformal_t. Lookup is linear scan since
 * max_modes is bounded at RT_CONFORMAL_TABLE_MAX_MODES (16). LRU
 * tracking uses a monotonically increasing tick stamped on every
 * touch; eviction picks the entry with the smallest tick.
 * ------------------------------------------------------------------ */

struct rt_conformal_table_entry {
	bool occupied;
	struct rt_conformal_mode_key key;
	rt_conformal_t *estimator;
	uint64_t last_touch_tick;
};

struct rt_conformal_table {
	struct rt_conformal_config cfg;
	uint32_t max_modes;
	uint64_t tick;
	uint64_t evictions;
	struct rt_conformal_table_entry entries[RT_CONFORMAL_TABLE_MAX_MODES];
};

rt_conformal_table_t *rt_conformal_table_create(
		const struct rt_conformal_config *cfg,
		uint32_t max_modes)
{
	if (cfg == NULL)
		return NULL;
	if (rt_conformal_config_validate(cfg) != 0)
		return NULL;

	rt_conformal_table_t *t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;
	t->cfg = *cfg;
	if (max_modes == 0)
		max_modes = 1;
	if (max_modes > RT_CONFORMAL_TABLE_MAX_MODES)
		max_modes = RT_CONFORMAL_TABLE_MAX_MODES;
	t->max_modes = max_modes;
	return t;
}

void rt_conformal_table_destroy(rt_conformal_table_t *t)
{
	if (t == NULL)
		return;
	for (uint32_t i = 0; i < t->max_modes; i++) {
		if (t->entries[i].occupied && t->entries[i].estimator != NULL)
			rt_conformal_destroy(t->entries[i].estimator);
	}
	free(t);
}

void rt_conformal_table_invalidate_all(rt_conformal_table_t *t,
		enum rt_conformal_invalidation_reason reason)
{
	if (t == NULL)
		return;
	for (uint32_t i = 0; i < t->max_modes; i++) {
		if (t->entries[i].occupied && t->entries[i].estimator != NULL)
			rt_conformal_invalidate(t->entries[i].estimator, reason);
	}
}

static int rt_conformal_table_find(rt_conformal_table_t *t,
		const struct rt_conformal_mode_key *k)
{
	for (uint32_t i = 0; i < t->max_modes; i++) {
		if (t->entries[i].occupied &&
				rt_conformal_mode_key_equal(&t->entries[i].key, k))
			return (int)i;
	}
	return -1;
}

static int rt_conformal_table_find_or_create(rt_conformal_table_t *t,
		const struct rt_conformal_mode_key *k)
{
	int idx = rt_conformal_table_find(t, k);
	if (idx >= 0)
		return idx;

	/* Find a free slot. */
	for (uint32_t i = 0; i < t->max_modes; i++) {
		if (!t->entries[i].occupied) {
			t->entries[i].estimator = rt_conformal_create(&t->cfg);
			if (t->entries[i].estimator == NULL)
				return -1;
			t->entries[i].key = *k;
			t->entries[i].occupied = true;
			t->entries[i].last_touch_tick = ++t->tick;
			return (int)i;
		}
	}

	/* No free slot: evict the least-recently-touched entry. The
	 * caller-facing eviction counter lets the operator notice this
	 * pressure; the table stays bounded by design. */
	uint32_t victim = 0;
	uint64_t oldest = t->entries[0].last_touch_tick;
	for (uint32_t i = 1; i < t->max_modes; i++) {
		if (t->entries[i].last_touch_tick < oldest) {
			oldest = t->entries[i].last_touch_tick;
			victim = i;
		}
	}
	if (t->entries[victim].estimator != NULL) {
		rt_conformal_destroy(t->entries[victim].estimator);
		t->entries[victim].estimator = NULL;
	}
	t->entries[victim].estimator = rt_conformal_create(&t->cfg);
	if (t->entries[victim].estimator == NULL) {
		t->entries[victim].occupied = false;
		return -1;
	}
	t->entries[victim].key = *k;
	t->entries[victim].occupied = true;
	t->entries[victim].last_touch_tick = ++t->tick;
	t->evictions++;
	return (int)victim;
}

bool rt_conformal_table_observe(rt_conformal_table_t *t,
		const struct rt_conformal_mode_key *k,
		uint64_t runtime_ns)
{
	if (t == NULL || k == NULL)
		return false;
	int idx = rt_conformal_table_find_or_create(t, k);
	if (idx < 0)
		return false;
	t->entries[idx].last_touch_tick = ++t->tick;
	return rt_conformal_observe(t->entries[idx].estimator, runtime_ns);
}

uint64_t rt_conformal_table_budget(rt_conformal_table_t *t,
		const struct rt_conformal_mode_key *k,
		uint64_t period_ns)
{
	if (t == NULL || k == NULL)
		return 0;
	int idx = rt_conformal_table_find(t, k);
	if (idx < 0)
		return 0;
	t->entries[idx].last_touch_tick = ++t->tick;
	return rt_conformal_budget(t->entries[idx].estimator, period_ns);
}

rt_conformal_t *rt_conformal_table_get(rt_conformal_table_t *t,
		const struct rt_conformal_mode_key *k)
{
	if (t == NULL || k == NULL)
		return NULL;
	int idx = rt_conformal_table_find(t, k);
	return idx < 0 ? NULL : t->entries[idx].estimator;
}

/* "Is the (sample_rate, quantum, class) estimator in a publish-ready
 * state?" Looks the entry up without touching the LRU tick so a
 * pure-readiness probe cannot influence eviction. The publish-ready
 * states are RT_CONF_VALID and RT_CONF_SHIFT -- the same two the
 * single-estimator path's runtime_select_for_node treats as
 * publishable in module-deadline.c. */
bool rt_conformal_table_ready_for_class(const rt_conformal_table_t *t,
		uint32_t sample_rate_hz, uint32_t quantum_frames,
		uint8_t core_class)
{
	if (t == NULL)
		return false;
	struct rt_conformal_mode_key key = {
		.sample_rate_hz = sample_rate_hz,
		.quantum_frames = quantum_frames,
		.core_class     = core_class,
	};
	for (uint32_t i = 0; i < t->max_modes; i++) {
		if (!t->entries[i].occupied)
			continue;
		if (!rt_conformal_mode_key_equal(&t->entries[i].key, &key))
			continue;
		enum rt_conformal_state s =
				rt_conformal_state(t->entries[i].estimator);
		return s == RT_CONF_VALID || s == RT_CONF_SHIFT;
	}
	return false;
}

uint64_t rt_conformal_table_budget_for_class(rt_conformal_table_t *t,
		uint32_t sample_rate_hz, uint32_t quantum_frames,
		uint8_t target_class, uint64_t period_ns,
		bool *out_used_bootstrap)
{
	if (out_used_bootstrap != NULL)
		*out_used_bootstrap = false;
	if (t == NULL)
		return 0;

	/* Preferred path: the target class has a ready entry. The
	 * lookup goes through the standard table_budget so the LRU
	 * tick advances and the entry counts as touched. */
	if (rt_conformal_table_ready_for_class(t, sample_rate_hz,
			quantum_frames, target_class)) {
		struct rt_conformal_mode_key key = {
			.sample_rate_hz = sample_rate_hz,
			.quantum_frames = quantum_frames,
			.core_class     = target_class,
		};
		return rt_conformal_table_budget(t, &key, period_ns);
	}

	/* BIG bootstrap fallback: when the target is BIG but no BIG
	 * entry is ready yet, borrow the LITTLE entry's budget. The
	 * directionality is intentional -- borrowing in the reverse
	 * direction (BIG -> LITTLE) would under-reserve at the slower
	 * core and is therefore not supported here. */
	if (target_class == RT_CONF_CORE_BIG &&
			rt_conformal_table_ready_for_class(t, sample_rate_hz,
				quantum_frames, RT_CONF_CORE_LITTLE)) {
		struct rt_conformal_mode_key little_key = {
			.sample_rate_hz = sample_rate_hz,
			.quantum_frames = quantum_frames,
			.core_class     = RT_CONF_CORE_LITTLE,
		};
		uint64_t bootstrap = rt_conformal_table_budget(t, &little_key,
				period_ns);
		if (bootstrap > 0 && out_used_bootstrap != NULL)
			*out_used_bootstrap = true;
		return bootstrap;
	}

	/* Neither the target class nor (if applicable) the LITTLE
	 * bootstrap is ready. Caller falls back to its warm-up policy
	 * -- typically pinning the node to a LITTLE core under
	 * module-rt until statistics accumulate. */
	return 0;
}

uint32_t rt_conformal_table_mode_count(const rt_conformal_table_t *t)
{
	if (t == NULL)
		return 0;
	uint32_t n = 0;
	for (uint32_t i = 0; i < t->max_modes; i++)
		if (t->entries[i].occupied)
			n++;
	return n;
}

int rt_conformal_table_collect_keys(const rt_conformal_table_t *t,
		struct rt_conformal_mode_key *out_keys, uint32_t *count)
{
	if (t == NULL || out_keys == NULL || count == NULL)
		return -1;
	uint32_t cap = *count;
	uint32_t n = 0;
	for (uint32_t i = 0; i < t->max_modes; i++) {
		if (!t->entries[i].occupied)
			continue;
		if (n < cap)
			out_keys[n] = t->entries[i].key;
		n++;
	}
	*count = n < cap ? n : cap;
	return 0;
}

uint64_t rt_conformal_table_evictions(const rt_conformal_table_t *t)
{
	return t ? t->evictions : 0;
}
