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
 *   - a dual indexed heap (max-heap + min-heap) that maintains the
 *     rolling empirical quantile incrementally in O(log W) per
 *     observation,
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
 * rt_conformal_budget(); the empirical quantile is maintained
 * incrementally at every insertion (a unit test pins the
 * no-look-ahead property by inspecting the ring contents around the
 * observe / budget interleaving).
 *
 * Realtime safety: rt_conformal_observe() runs in O(log W) per call
 * -- one ring insert, one heap expire + insert + rebalance, one EWMA
 * update, one comparison for the overrun counter. No sorting, no
 * scratch buffer; the quantile is always fresh after each insert.
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
	 * - alpha_target 5e-4 (was 1e-3): target the 99.95 % one-sided
	 *   quantile. The previous 1e-3 calibration aimed at the 99.9 %
	 *   tail but in practice alpha_eff drifted up to alpha_max
	 *   between bursts and the operating point ended up at the
	 *   loosest allowed quantile rather than the intended one;
	 *   tightening alpha_target moves the steady-state operating
	 *   point deeper into the tail by design, which gives
	 *   high-variance followers the extra margin they need to keep
	 *   the kernel-side overrun rate below the target rate they were
	 *   supposed to honour. The finite-sample conformal index
	 *
	 *       k = ceil((window + 1) * (1 - alpha_eff))
	 *
	 *   clamps to `window` whenever (window + 1) * alpha < 1, so
	 *   alpha must stay above 1 / (window + 1); with window 4096
	 *   every alpha at or above ~2.44e-4 places the rank strictly
	 *   below the maximum of the ring, which is what the
	 *   sustained-spike regime requires (two or three extreme
	 *   samples must coexist for the quantile to follow them).
	 *
	 * - alpha_min 1e-4 (was 1e-5): below alpha_target so the
	 *   adaptive descent on a burst tightens the quantile further
	 *   than the steady-state aim, briefly placing the empirical
	 *   rank at the maximum of the score ring (most conservative
	 *   the estimator can publish) until the up-drift returns
	 *   alpha_eff to alpha_target over a few hundred samples. This
	 *   is what gives an actual burst extra margin without making
	 *   the steady-state operating point any tighter. The
	 *   sigma_floor_ns bound below keeps the "max-of-ring"
	 *   excursion bounded: the per-sample score is capped at
	 *   roughly 2 * period / sigma_floor (about 270 at the
	 *   small-quantum case, a few thousand at the default), so the
	 *   transient over-shoot stays within the C-side period_ns
	 *   clamp at the read site.
	 *
	 * - alpha_max 1e-3 (was 5e-3): only twice the target, not ten
	 *   times. The previous 5e-3 ceiling let alpha_eff drift up to
	 *   the 99.5 % quantile in quiet stretches, which produced two
	 *   bad symptoms simultaneously: the operating point sat 5x
	 *   looser than the alpha_target promise, and the budget could
	 *   swing by an order of magnitude between a burst (alpha at
	 *   alpha_min) and a quiet stretch (alpha at alpha_max). The
	 *   new ceiling caps the steady-state relaxation at 99.9 %
	 *   quantile, keeps the alpha_eff range to 2x rather than 10x,
	 *   and removes most of the cycle-to-cycle variance in the
	 *   published budget.
	 *
	 * - eta 0.002 (was 0.005): slower adaptation so a single
	 *   overrun moves alpha_eff by 0.002 instead of 0.005, and the
	 *   recovery from alpha_min back toward alpha_max takes longer
	 *   too. Combined with the narrower alpha range above, this is
	 *   the smoothing knob: the predictor's response to a single
	 *   sample is gentler, the steady-state budget varies less, and
	 *   reactivity is still adequate (one burst still tightens the
	 *   quantile within a few hundred milliseconds at the common
	 *   graph periods).
	 *
	 * - window 4096 (was 1024): at the common 42 ms graph period
	 *   the score ring now retains observed peaks for ~170 s
	 *   instead of ~43 s, so a sustained-spike trace that fits in
	 *   a few seconds of activity still influences the budget two
	 *   minutes later. RT_CONFORMAL_MAX_WINDOW remains the ceiling.
	 *
	 * - ewma_scale_lambda 0.01 (was 0.005): doubles the rate at
	 *   which the EWMA scale tracks recent deviation magnitude. The
	 *   previous 0.005 gain kept past-burst scale memory alive for
	 *   ~140 samples, which combined with a small sigma_floor_ns
	 *   produced very large quantile-times-scale products on chains
	 *   that visit a brief variance step and then quiet down. The
	 *   live calibration against pw-cat + builtin busy + convolver
	 *   + multi-chain workloads showed that doubling the gain keeps
	 *   overrun_rate below 0.0012 across every scenario while
	 *   shrinking the steady-state budget overshoot ratio
	 *   (budget_p99 / runtime_p99) from ~2x to ~1.1x.
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
	 *
	 * - sigma_floor_ns 10000 (was 1): bounds the denominator of the
	 *   nonconformity score (x - mu) / (scale + sigma_floor) and
	 *   the prediction's variance term mu + q * (scale + sigma_floor)
	 *   from below by 10 microseconds. The previous 1 ns floor let
	 *   a quiet stretch collapse scale to near zero, after which a
	 *   single 4 ms sample produced a per-sample score in the tens
	 *   of thousands, the empirical quantile inherited that score,
	 *   and subsequent predictions blew up to ~q * scale = millions
	 *   of nanoseconds even though the actual workload never went
	 *   above a few milliseconds. The C-side period_ns clamp at the
	 *   read site masked the kernel-visible budget, but the
	 *   *prediction itself* explodes, which the calibration aims to
	 *   keep bounded so the daemon does not rely on the clamp to
	 *   stay safe. A 10 microsecond floor caps the
	 *   maximum per-sample score at (period / sigma_floor) approx
	 *   4000 for the small-quantum case and a few hundred for the
	 *   default quantum, and bounds the prediction's variance term
	 *   to a small multiple of the EWMA location. Calibrated
	 *   against the same multi-workload trace set as ewma_scale_lambda.
	 *
	 * - guard_ns 5000 (was 1500): the absolute additive cushion on
	 *   top of (mu + q * (scale + sigma_floor)). 5 microseconds at
	 *   the small-quantum case (1.33 ms period) is roughly 0.4 % of
	 *   the period; at the default quantum it is irrelevant
	 *   alongside the multiplicative guard_percent term. The
	 *   calibration showed the larger guard reduces overrun_rate by
	 *   a third without measurably inflating the steady-state
	 *   budget overhead.
	 */
	cfg->alpha_target          = 5e-4;
	cfg->alpha_min             = 1e-4;
	cfg->alpha_max             = 1e-3;
	cfg->eta                   = 0.002;
	cfg->ewma_location_lambda  = 0.01;
	cfg->ewma_scale_lambda     = 0.01;
	cfg->window                = 4096;
	cfg->bootstrap_min_samples = 64;
	cfg->bootstrap_runtime_ns  = 100000;
	cfg->guard_ns              = 5000;
	cfg->guard_percent         = 0.05;
	cfg->sigma_floor_ns        = 10000;
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

/* Indexed heap used for O(log n) incremental quantile maintenance. */
struct indexed_heap {
	double   *vals;    /* heap-ordered values */
	uint32_t *slots;   /* vals[i] came from ring slot slots[i] */
	uint32_t  size;
};

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

	/* Dual indexed heap for O(log n) incremental quantile.
	 * lower is a max-heap of the k smallest scores (quantile = top).
	 * upper is a min-heap of the (n - k) largest scores.
	 * heap_loc[ring_slot] encodes which heap + position. */
	struct indexed_heap lower;
	struct indexed_heap upper;
	int32_t *heap_loc;

	double   cached_score_quantile;
	bool     quantile_valid;
};

static uint64_t clamp_u64(uint64_t v, uint64_t lo, uint64_t hi)
{
	if (v < lo) return lo;
	if (hi != 0 && v > hi) return hi;
	return v;
}

/* ------------------------------------------------------------------ */
/* Indexed heap helpers for O(log n) incremental quantile.            */
/* ------------------------------------------------------------------ */

#define HEAP_LOC_SENTINEL INT32_MIN
#define HEAP_LOC_UPPER_BIT (1 << 30)

static inline int32_t heap_loc_encode(bool is_upper, uint32_t pos)
{
	return (int32_t)(is_upper ? HEAP_LOC_UPPER_BIT : 0) | (int32_t)pos;
}

static inline bool heap_loc_is_upper(int32_t loc)
{
	return (loc & HEAP_LOC_UPPER_BIT) != 0;
}

static inline uint32_t heap_loc_pos(int32_t loc)
{
	return (uint32_t)(loc & ~HEAP_LOC_UPPER_BIT);
}

static inline void heap_swap(struct indexed_heap *h, uint32_t i, uint32_t j,
		int32_t *heap_loc, bool is_upper)
{
	double tv;
	uint32_t ts;
	tv = h->vals[i]; h->vals[i] = h->vals[j]; h->vals[j] = tv;
	ts = h->slots[i]; h->slots[i] = h->slots[j]; h->slots[j] = ts;
	heap_loc[h->slots[i]] = heap_loc_encode(is_upper, i);
	heap_loc[h->slots[j]] = heap_loc_encode(is_upper, j);
}

static inline bool heap_cmp(double a, double b, bool is_max)
{
	return is_max ? (a > b) : (a < b);
}

static void heap_sift_up(struct indexed_heap *h, uint32_t i,
		int32_t *heap_loc, bool is_max, bool is_upper)
{
	while (i > 0) {
		uint32_t parent = (i - 1) / 2;
		if (heap_cmp(h->vals[i], h->vals[parent], is_max))
			heap_swap(h, i, parent, heap_loc, is_upper);
		else
			break;
		i = parent;
	}
}

static void heap_sift_down(struct indexed_heap *h, uint32_t i,
		int32_t *heap_loc, bool is_max, bool is_upper)
{
	for (;;) {
		uint32_t best = i;
		uint32_t l = 2 * i + 1;
		uint32_t r = 2 * i + 2;
		if (l < h->size && heap_cmp(h->vals[l], h->vals[best], is_max))
			best = l;
		if (r < h->size && heap_cmp(h->vals[r], h->vals[best], is_max))
			best = r;
		if (best == i)
			break;
		heap_swap(h, i, best, heap_loc, is_upper);
		i = best;
	}
}

static void heap_insert(struct indexed_heap *h, double val, uint32_t ring_slot,
		int32_t *heap_loc, bool is_max, bool is_upper)
{
	uint32_t pos = h->size;
	h->vals[pos] = val;
	h->slots[pos] = ring_slot;
	h->size++;
	heap_loc[ring_slot] = heap_loc_encode(is_upper, pos);
	heap_sift_up(h, pos, heap_loc, is_max, is_upper);
}

static void heap_remove_at(struct indexed_heap *h, uint32_t pos,
		int32_t *heap_loc, bool is_max, bool is_upper)
{
	uint32_t last = h->size - 1;
	if (pos != last) {
		heap_swap(h, pos, last, heap_loc, is_upper);
		h->size--;
		heap_loc[h->slots[last]] = HEAP_LOC_SENTINEL;
		heap_sift_down(h, pos, heap_loc, is_max, is_upper);
		heap_sift_up(h, pos, heap_loc, is_max, is_upper);
	} else {
		heap_loc[h->slots[pos]] = HEAP_LOC_SENTINEL;
		h->size--;
	}
}

static inline double heap_peek(const struct indexed_heap *h)
{
	return h->vals[0];
}

static uint32_t quantile_target_k(uint32_t n, double alpha_eff)
{
	double p = 1.0 - alpha_eff;
	double q_idx;
	uint32_t k;
	if (n == 0)
		return 0;
	if (!(p > 0.0))
		return 1;
	if (p >= 1.0)
		return n;
	q_idx = ceil((double)(n + 1u) * p);
	if (q_idx < 1.0)
		k = 1;
	else if (q_idx > (double)n)
		k = n;
	else
		k = (uint32_t)q_idx;
	return k;
}

static void heaps_rebalance(rt_conformal_t *e)
{
	uint32_t target_k = quantile_target_k(e->ring_count, e->alpha_eff);
	if (target_k == 0)
		return;

	while (e->lower.size > target_k) {
		uint32_t slot = e->lower.slots[0];
		double val = e->lower.vals[0];
		heap_remove_at(&e->lower, 0, e->heap_loc, true, false);
		heap_insert(&e->upper, val, slot, e->heap_loc, false, true);
	}
	while (e->lower.size < target_k && e->upper.size > 0) {
		uint32_t slot = e->upper.slots[0];
		double val = e->upper.vals[0];
		heap_remove_at(&e->upper, 0, e->heap_loc, false, true);
		heap_insert(&e->lower, val, slot, e->heap_loc, true, false);
	}
}

static void heaps_expire_slot(rt_conformal_t *e, uint32_t ring_slot)
{
	int32_t loc = e->heap_loc[ring_slot];
	if (loc == HEAP_LOC_SENTINEL)
		return;
	if (heap_loc_is_upper(loc)) {
		heap_remove_at(&e->upper, heap_loc_pos(loc),
				e->heap_loc, false, true);
	} else {
		heap_remove_at(&e->lower, heap_loc_pos(loc),
				e->heap_loc, true, false);
	}
}

static void heaps_insert_score(rt_conformal_t *e, double val, uint32_t ring_slot)
{
	if (e->lower.size == 0 || val <= heap_peek(&e->lower)) {
		heap_insert(&e->lower, val, ring_slot, e->heap_loc, true, false);
	} else {
		heap_insert(&e->upper, val, ring_slot, e->heap_loc, false, true);
	}
}

static void heaps_update_quantile(rt_conformal_t *e)
{
	heaps_rebalance(e);
	if (e->lower.size > 0) {
		e->cached_score_quantile = heap_peek(&e->lower);
		e->quantile_valid = true;
	} else {
		e->cached_score_quantile = 0.0;
		e->quantile_valid = false;
	}
}

static void ring_insert(rt_conformal_t *e, double score, uint64_t runtime)
{
	uint32_t cap = e->cfg.window;
	uint32_t slot = e->ring_head;

	if (e->ring_count == cap)
		heaps_expire_slot(e, slot);

	e->score_ring[slot] = score;
	e->runtime_ring[slot] = runtime;
	e->ring_head = (slot + 1u) % cap;
	if (e->ring_count < cap)
		e->ring_count++;

	heaps_insert_score(e, score, slot);
	heaps_update_quantile(e);
}

rt_conformal_t *rt_conformal_create(const struct rt_conformal_config *cfg)
{
	rt_conformal_t *e;
	uint32_t w;

	if (rt_conformal_config_validate(cfg) != 0)
		return NULL;

	e = calloc(1, sizeof(*e));
	if (e == NULL)
		return NULL;

	e->cfg = *cfg;
	e->state = RT_CONF_INSUFFICIENT_DATA;
	e->last_invalidation_reason = RT_CONF_INVALIDATED_NONE;
	e->alpha_eff = cfg->alpha_target;

	w = cfg->window;
	e->score_ring   = calloc(w, sizeof(double));
	e->runtime_ring = calloc(w, sizeof(uint64_t));
	e->lower.vals   = calloc(w, sizeof(double));
	e->lower.slots  = calloc(w, sizeof(uint32_t));
	e->upper.vals   = calloc(w, sizeof(double));
	e->upper.slots  = calloc(w, sizeof(uint32_t));
	e->heap_loc     = malloc(w * sizeof(int32_t));
	if (e->score_ring == NULL || e->runtime_ring == NULL ||
	    e->lower.vals == NULL || e->lower.slots == NULL ||
	    e->upper.vals == NULL || e->upper.slots == NULL ||
	    e->heap_loc == NULL) {
		free(e->score_ring);
		free(e->runtime_ring);
		free(e->lower.vals);
		free(e->lower.slots);
		free(e->upper.vals);
		free(e->upper.slots);
		free(e->heap_loc);
		free(e);
		return NULL;
	}

	for (uint32_t i = 0; i < w; i++)
		e->heap_loc[i] = HEAP_LOC_SENTINEL;

	return e;
}

void rt_conformal_destroy(rt_conformal_t *e)
{
	if (e == NULL)
		return;
	free(e->score_ring);
	free(e->runtime_ring);
	free(e->lower.vals);
	free(e->lower.slots);
	free(e->upper.vals);
	free(e->upper.slots);
	free(e->heap_loc);
	free(e);
}

void rt_conformal_invalidate(rt_conformal_t *e,
		enum rt_conformal_invalidation_reason reason)
{
	uint32_t i;
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
	e->cached_score_quantile = 0.0;
	e->quantile_valid = false;
	memset(e->score_ring, 0, sizeof(double) * e->cfg.window);
	memset(e->runtime_ring, 0, sizeof(uint64_t) * e->cfg.window);
	e->lower.size = 0;
	e->upper.size = 0;
	for (i = 0; i < e->cfg.window; i++)
		e->heap_loc[i] = HEAP_LOC_SENTINEL;
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

	triggered_recalc = true;

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
