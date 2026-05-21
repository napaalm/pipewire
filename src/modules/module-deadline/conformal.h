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
 * IMPORTANT: the estimator publishes an online upper runtime budget
 * calibrated to a target overrun frequency for stable mode keys and
 * adapted under distribution shift. It is intended for soft /
 * weakly-hard real-time operation with SCHED_DEADLINE reservations.
 * It is NOT a deterministic WCET proof. Strict hard-real-time
 * guarantees require manual / static / hybrid WCETs and admission
 * control that rejects unschedulable graph changes.
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

	/*
	 * Optional burst-penalty extension. burst_threshold is the
	 * consecutive-overrun count above which the alpha_eff
	 * negative update is multiplied by burst_penalty. Defaults
	 * keep the extension off (burst_penalty = 1.0); calibration
	 * may enable it.
	 */
	uint32_t burst_threshold;
	double   burst_penalty;

	/*
	 * SHIFT-state detector. When the most recent consecutive
	 * overrun burst reaches shift_burst_threshold the estimator
	 * transitions from VALID to SHIFT and the typed
	 * invalidation reason RT_CONF_INVALIDATED_PLUGIN_MODE may
	 * be stamped by the caller. The SHIFT branch is informational
	 * for diagnostics; the alpha update still progresses (the
	 * threshold is a hint, not a kill).
	 */
	uint32_t shift_burst_threshold;
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

/*
 * Optional burst-penalty extension: when the current consecutive
 * overrun burst reaches the configured threshold, the alpha_eff
 * update is multiplied by burst_penalty so it tightens faster
 * (Gibbs & Candes 2021 §4 motivates the asymmetric response). The
 * extension is OFF by default (burst_penalty = 1.0); a calibration
 * pass enables it only if it improves weakly-hard metrics.
 */
double   rt_conformal_burst_penalty(const rt_conformal_t *e);
uint32_t rt_conformal_burst_threshold(const rt_conformal_t *e);

/*
 * Force the estimator into RT_CONF_DISABLED. Subsequent calls to
 * rt_conformal_observe accept samples but do not contribute to the
 * EWMA / ring; rt_conformal_budget returns 0 to signal "no
 * conformal opinion". An operator opt-out hook.
 */
void rt_conformal_disable(rt_conformal_t *e);
void rt_conformal_enable(rt_conformal_t *e);
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

/*
 * Mode-keyed estimator table.
 *
 * The single-estimator API above operates on a flat per-follower
 * instance: switching sample rate, quantum, or CPU class invalidates
 * the cached score ring and forces a fresh bootstrap. The mode-keyed
 * table partitions samples by the triple
 *
 *   (sample_rate_hz, quantum_frames, core_class)
 *
 * so each combination owns its own rt_conformal_t. The entity id
 * (typically a follower's node_id) is the table's owner; the mode key
 * is the per-sample partition. Switching modes routes new samples into
 * a different estimator without disturbing the others, and switching
 * back picks up the previously calibrated state.
 *
 * Fusion-group ownership: when several PipeWire nodes have been
 * collapsed onto a single data-loop thread, the schedulable entity is
 * the fusion-group leader, not the individual member nodes. The
 * table is therefore owned by the leader's id; member-only churn
 * (a new follower joining a chain that already has a stable thread)
 * keeps the per-mode calibration warm. When fusion membership itself
 * changes the workload's per-period cost distribution, the caller
 * should clear every per-mode entry via
 * rt_conformal_table_invalidate_all(t, RT_CONF_INVALIDATED_FUSION_GROUP)
 * -- the same invalidation reason the single-estimator path uses --
 * so the table reverts to bootstrap pending fresh samples under the
 * new membership.
 *
 * The table is small: each follower keeps at most max_modes entries
 * (default 8). When the cap is reached the least-recently-used entry
 * is recycled to make room for the new mode key and a one-shot warning
 * surfaces the eviction so the operator can widen the cap if the
 * workload genuinely needs more concurrent modes.
 *
 * Threading mirrors the single-estimator contract: the table is
 * single-threaded; the audio recalc worker serialises every entry.
 */

enum rt_core_class_compat {
	RT_CONF_CORE_LITTLE = 0,
	RT_CONF_CORE_BIG    = 1,
	RT_CONF_CORE_N      = 2,
};

struct rt_conformal_mode_key {
	uint32_t sample_rate_hz;
	uint32_t quantum_frames;
	uint8_t  core_class;     /* enum rt_core_class_compat */
	uint8_t  _pad[3];
};

static inline bool
rt_conformal_mode_key_equal(const struct rt_conformal_mode_key *a,
		const struct rt_conformal_mode_key *b)
{
	return a->sample_rate_hz == b->sample_rate_hz &&
		a->quantum_frames == b->quantum_frames &&
		a->core_class == b->core_class;
}

typedef struct rt_conformal_table rt_conformal_table_t;

/* Create a mode-keyed table backed by per-mode rt_conformal_t
 * instances. Every entry will be created from a private copy of cfg.
 * max_modes is clamped to [1, RT_CONFORMAL_TABLE_MAX_MODES]. Returns
 * NULL on EINVAL or on out-of-memory; emits no log on its own. */
#define RT_CONFORMAL_TABLE_MAX_MODES 16u
rt_conformal_table_t *rt_conformal_table_create(
		const struct rt_conformal_config *cfg,
		uint32_t max_modes);

void rt_conformal_table_destroy(rt_conformal_table_t *t);

/* Discard every cached mode state. The configuration snapshot is
 * preserved; subsequent observe() calls re-populate entries lazily.
 * Useful when a graph-level event (topology change, fusion-group
 * recomposition) invalidates every per-mode calibration at once. */
void rt_conformal_table_invalidate_all(rt_conformal_table_t *t,
		enum rt_conformal_invalidation_reason reason);

/* Record one sample against the estimator that owns `key`. The entry
 * is created on first touch; on cap overflow the least-recently-used
 * existing entry is evicted (a single-line warning is emitted at most
 * once per table life). Returns true if the observe triggered a
 * deferred-quantile recompute on the active entry, false otherwise.
 * NULL-safe (returns false). */
bool rt_conformal_table_observe(rt_conformal_table_t *t,
		const struct rt_conformal_mode_key *key,
		uint64_t runtime_ns);

/* Publish the budget for the estimator owning `key`. When the key has
 * not been observed yet, return 0 so the caller can apply its own
 * bootstrap policy. NULL-safe (returns 0). */
uint64_t rt_conformal_table_budget(rt_conformal_table_t *t,
		const struct rt_conformal_mode_key *key,
		uint64_t period_ns);

/* Borrow the rt_conformal_t backing `key`, or NULL if none exists yet.
 * The pointer is owned by the table and stays valid until the next
 * observe() that triggers an eviction, or until destroy. Diagnostic
 * accessors (rt_conformal_state(), rt_conformal_alpha_eff(), ...) can
 * be called on the returned pointer. */
rt_conformal_t *rt_conformal_table_get(rt_conformal_table_t *t,
		const struct rt_conformal_mode_key *key);

/* Number of modes currently held in the table (0..max_modes). */
uint32_t rt_conformal_table_mode_count(const rt_conformal_table_t *t);

/* Iterate every populated entry's key. Returns 0 on success and copies
 * up to *count keys into out_keys; on entry *count is the buffer size,
 * on exit it is the number of entries actually copied. */
int rt_conformal_table_collect_keys(const rt_conformal_table_t *t,
		struct rt_conformal_mode_key *out_keys,
		uint32_t *count);

/* Cumulative eviction counter (modes recycled because max_modes was
 * exceeded). */
uint64_t rt_conformal_table_evictions(const rt_conformal_table_t *t);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_CONFORMAL_H */
