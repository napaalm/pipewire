/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_DEADLINE_CONFORMAL_H
#define MODULE_DEADLINE_CONFORMAL_H

/*
 * conformal.h
 *
 * Online one-sided upper-runtime budget estimator built on an EWMA
 * location/scale base predictor wrapped in adaptive conformal
 * inference for distribution shift. The estimator consumes one
 * per-activation CPU-time sample (CLOCK_THREAD_CPUTIME_ID-derived,
 * already normalised to a reference CPU upstream) and publishes a
 * one-sided runtime bound calibrated to a target overrun frequency.
 *
 * Mathematical references:
 *
 *   - Romano, Patterson & Candes, "Conformalized Quantile Regression",
 *     NeurIPS 2019: the one-sided conformal score machinery.
 *   - Gibbs & Candes, "Adaptive Conformal Inference Under
 *     Distribution Shift", NeurIPS 2021: the online alpha_eff update
 *     that recalibrates the score quantile when the empirical overrun
 *     rate drifts from the target.
 *   - Bernat, Burns & Llamosi, "Weakly Hard Real-Time Systems", IEEE
 *     TC 50(4), 2001: the vocabulary the published bound inhabits.
 *     The output is NOT a deterministic WCET; it is a soft /
 *     weakly-hard upper budget. Strict hard-realtime operation still
 *     requires manual / static / hybrid budgets.
 *
 * Realtime safety: the estimator is intentionally pure data. No
 * allocations after rt_conformal_init; fixed-size ring buffers; the
 * exact rolling-quantile recompute uses a per-instance scratch buffer
 * sized at init time. The expensive quantile recompute is gated
 * behind a recalc_period throttle so an audio recalc worker can run
 * it outside the realtime path. The score-ring insert and budget
 * emission are O(1).
 *
 * Threading: the estimator is single-threaded; the audio recalc
 * worker serialises all entry points. Snapshots that surface state
 * to a diagnostic reader copy fields under the same single-threaded
 * discipline as the existing MBPTA estimator.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum permitted score-/runtime-ring window. The plan calls for a
 * compile-time bound so sizeof(struct rt_conformal_state_data) is
 * predictable and reportable in a unit test. 4096 is the upper end of
 * the calibration sweep; bigger windows must be rejected at config
 * parse time.
 */
#define RT_CONFORMAL_MAX_WINDOW 4096u

/*
 * Estimator state machine. The bootstrap state covers the warm
 * startup period when the score ring is too sparse for a meaningful
 * empirical quantile -- the estimator publishes a configured
 * bootstrap budget while it accumulates samples. Newly created
 * nodes start IN bootstrap, not in a quarantine state; they always
 * publish a valid budget.
 *
 *   BOOTSTRAP         -- score ring below bootstrap_min_samples / 2
 *                        and / or fewer than bootstrap_min_samples
 *                        total samples observed.
 *   VALID             -- ring populated; the rolling empirical
 *                        quantile drives the published budget.
 *   SHIFT             -- sustained overrun burst above the
 *                        weakly-hard threshold; reserved for the
 *                        adaptive-update path.
 *   INSUFFICIENT_DATA -- post-reset transient before the first new
 *                        sample arrives.
 *   DISABLED          -- estimator explicitly turned off.
 */
enum rt_conformal_state {
	RT_CONF_INSUFFICIENT_DATA = 0,
	RT_CONF_BOOTSTRAP         = 1,
	RT_CONF_VALID             = 2,
	RT_CONF_SHIFT             = 3,
	RT_CONF_DISABLED          = 4,
};

const char *rt_conformal_state_name(enum rt_conformal_state s);

/*
 * Why the mode key was rebuilt. Surfaced via
 * rt_conformal_last_invalidation_reason() and rendered as a stable
 * lowercase token for operator diagnostics. The set is intentionally
 * the same dimensions the existing MBPTA estimator key fingerprint
 * folds: period, fusion-leader, topology generation, CPU placement,
 * plus operator request and plugin-mode change. Sharing the
 * vocabulary lets a single typed reason describe both estimators.
 */
enum rt_conformal_invalidation_reason {
	RT_CONF_INVALIDATED_NONE                = 0,
	RT_CONF_INVALIDATED_PERIOD              = 1,
	RT_CONF_INVALIDATED_FUSION_GROUP        = 2,
	RT_CONF_INVALIDATED_TOPOLOGY_GENERATION = 3,
	RT_CONF_INVALIDATED_CPU_CLASS           = 4,
	RT_CONF_INVALIDATED_OPERATOR_REQUEST    = 5,
	RT_CONF_INVALIDATED_PLUGIN_MODE         = 6,
};

const char *rt_conformal_invalidation_reason_name(
		enum rt_conformal_invalidation_reason r);

/*
 * Risk-allocation policy used by the graph-level alpha splitter.
 * UNIFORM gives every schedulable entity alpha_graph / N; the two
 * weighted variants are placeholders for future allocators -- they
 * compile-time exist but are not yet wired into the graph-level
 * splitter and fall back to uniform until the weighted heuristics
 * land.
 */
enum rt_conformal_risk_allocation {
	RT_CONF_RISK_ALLOC_UNIFORM          = 0,
	RT_CONF_RISK_ALLOC_DENSITY_WEIGHTED = 1,
	RT_CONF_RISK_ALLOC_SLOPE_WEIGHTED   = 2,
};

const char *rt_conformal_risk_allocation_name(
		enum rt_conformal_risk_allocation a);

/*
 * Configuration block. The defaults the algorithm reference
 * recommends as starting points are:
 *
 *   alpha_target           = 1e-3 / N_schedulable (graph-uniform)
 *   alpha_min              = 1e-5
 *   alpha_max              = 5e-2
 *   eta                    = 0.005
 *   window                 = 1024
 *   recalc_period          = 1
 *   ewma_location_lambda   = 0.05
 *   ewma_scale_lambda      = 0.05
 *   guard_ns               = 1500
 *   guard_percent          = 0.05
 *   sigma_floor_ns         = 1 (calibrated)
 *   runtime_floor_ns       = 1000
 *   bootstrap_min_samples  = 64
 *   bootstrap_runtime_ns   = 0 (caller fills in a sound floor)
 *
 * Every field has a meaningful default; rt_conformal_config_defaults
 * is the canonical initialiser.
 */
struct rt_conformal_config {
	double   alpha_target;
	double   alpha_min;
	double   alpha_max;
	double   eta;
	double   ewma_location_lambda;
	double   ewma_scale_lambda;
	uint32_t window;
	uint32_t bootstrap_min_samples;
	uint64_t bootstrap_runtime_ns;
	uint64_t guard_ns;
	double   guard_percent;
	uint64_t sigma_floor_ns;
	uint64_t runtime_floor_ns;
	uint32_t recalc_period;
	bool     compatible_history;
	enum rt_conformal_risk_allocation risk_allocation;
	uint64_t max_update_cost_ns;
	bool     trace_export;
};

/*
 * Populate cfg with the recommended starting-point defaults. Safe on
 * a stack-allocated struct. NULL-safe (no-op).
 */
void rt_conformal_config_defaults(struct rt_conformal_config *cfg);

/*
 * Validate cfg. Returns 0 if every field is within range, -EINVAL
 * otherwise. The estimator factory uses this; the module's parser
 * uses it to refuse a configuration block that would land the
 * estimator in an unsound state. NULL is treated as -EINVAL.
 *
 * Validity envelopes:
 *   alpha_target in (0, 1)
 *   alpha_min    in (0, alpha_target]
 *   alpha_max    in [alpha_target, 1)
 *   eta          in (0, 1)
 *   ewma_*_lambda in (0, 1)
 *   window       in [2, RT_CONFORMAL_MAX_WINDOW]
 *   bootstrap_min_samples >= 2
 *   guard_percent in [0, 1)
 *   recalc_period >= 1
 */
int rt_conformal_config_validate(const struct rt_conformal_config *cfg);

/*
 * Opaque estimator handle. Owns its own configuration snapshot, its
 * score and runtime ring buffers, the EWMA location/scale pair, the
 * adaptive alpha state and the cached sorted-score quantile. The
 * sizeof() is reported via rt_conformal_state_data_size() so a unit
 * test can pin the per-follower memory cost.
 */
typedef struct rt_conformal rt_conformal_t;

/*
 * Build a fresh estimator. Returns NULL when cfg is invalid (the
 * validator is run before any allocation), or on out-of-memory. The
 * estimator owns a private copy of cfg; the caller may free its own
 * after the call returns.
 */
rt_conformal_t *rt_conformal_create(const struct rt_conformal_config *cfg);

/* NULL-safe. */
void rt_conformal_destroy(rt_conformal_t *e);

/* Drop every cached sample, reset the score ring, return the
 * estimator to RT_CONF_INSUFFICIENT_DATA and stamp the typed
 * invalidation reason. The estimator-config snapshot is preserved.
 * NULL-safe. */
void rt_conformal_invalidate(rt_conformal_t *e,
		enum rt_conformal_invalidation_reason reason);

/*
 * Record one observed per-activation runtime in nanoseconds. The
 * observation flow follows the prequential discipline (no
 * look-ahead): the score s_t for this sample is computed from the
 * EWMA state as it existed BEFORE this call (mu_pred, scale_pred),
 * then s_t is inserted into the score ring; only after that does the
 * EWMA pair get updated by x_t. This keeps rt_conformal_budget()
 * usable as a one-step-ahead prediction (it consumes the ring as it
 * stood when the prior budget was emitted, not as it stands after
 * the matching observation arrived). Returns true if the call
 * triggered a deferred-quantile recompute (every recalc_period
 * samples).
 *
 * Samples below the configured runtime_floor_ns are clamped to the
 * floor; the score is computed on the clamped value so a degenerate
 * zero sample (a follower that ran for less than the clock
 * resolution) does not skew the EWMA scale.
 *
 * NULL-safe (returns false); zero / negative / non-finite samples
 * are rejected silently.
 */
bool rt_conformal_observe(rt_conformal_t *e, uint64_t runtime_ns);

/*
 * Publish the one-sided runtime budget for the next activation, in
 * nanoseconds:
 *
 *   prediction = mu + Q * (scale + sigma_floor)
 *   guarded    = prediction * (1 + guard_percent) + guard_ns
 *   budget     = clamp(ceil(guarded), runtime_floor_ns, period_ns)
 *
 * where Q is the empirical quantile at level (1 - alpha_eff) of the
 * current score ring, using the finite-sample conformal index
 * convention ceil((n + 1) * (1 - alpha_eff)) clamped to [1, n]. When
 * the estimator is in RT_CONF_BOOTSTRAP the configured
 * bootstrap_runtime_ns is published (still clamped to
 * [runtime_floor_ns, period_ns]); when in RT_CONF_DISABLED the
 * function returns 0.
 *
 * period_ns is the kernel-side period the budget will be clamped
 * against; pass 0 to disable the upper clamp (the caller will then
 * impose its own).
 */
uint64_t rt_conformal_budget(rt_conformal_t *e, uint64_t period_ns);

/*
 * Diagnostic accessors. All are NULL-safe (return 0 / default).
 */
enum rt_conformal_state rt_conformal_state(const rt_conformal_t *e);
uint64_t rt_conformal_samples_seen(const rt_conformal_t *e);
uint64_t rt_conformal_samples_used(const rt_conformal_t *e);
uint64_t rt_conformal_overruns_seen(const rt_conformal_t *e);
uint64_t rt_conformal_recent_overruns(const rt_conformal_t *e);
uint64_t rt_conformal_max_overrun_burst(const rt_conformal_t *e);
uint64_t rt_conformal_current_overrun_burst(const rt_conformal_t *e);
double   rt_conformal_alpha_eff(const rt_conformal_t *e);
double   rt_conformal_mu_ns(const rt_conformal_t *e);
double   rt_conformal_scale_ns(const rt_conformal_t *e);
double   rt_conformal_last_prediction_ns(const rt_conformal_t *e);
double   rt_conformal_last_score(const rt_conformal_t *e);
uint64_t rt_conformal_last_budget_ns(const rt_conformal_t *e);
uint64_t rt_conformal_last_runtime_ns(const rt_conformal_t *e);
double   rt_conformal_score_quantile(const rt_conformal_t *e);
uint64_t rt_conformal_guard_ns_effective(const rt_conformal_t *e);
enum rt_conformal_invalidation_reason
	rt_conformal_last_invalidation_reason(const rt_conformal_t *e);

/*
 * Returns the byte size of one estimator instance. Used by the unit
 * suite to pin the per-follower memory cost; the budget the algorithm
 * reference quotes is "a few KB".
 */
size_t rt_conformal_state_data_size(void);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_CONFORMAL_H */
