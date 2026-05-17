/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_DEADLINE_MBPTA_H
#define MODULE_DEADLINE_MBPTA_H

/*
 * mbpta.h
 *
 * Online measurement-based probabilistic timing analysis (MBPTA)
 * estimator for per-task execution-time samples. The estimator
 * implements the extreme-value-theory pipeline described in
 * Cucu-Grosjean, Santinelli, Houston, Lo, Vardanega, Kosmidis,
 * Abella, Mezzetti, Quinones, Cazorla, "Measurement-Based
 * Probabilistic Timing Analysis for Multi-path Programs", ECRTS
 * 2012:
 *
 *   1. Maintain a rolling window of raw per-cycle samples. Discard
 *      the first `warmup` samples to skip the warm-up transient.
 *   2. Periodically re-evaluate the fit (every `n_delta` new
 *      samples). The pipeline:
 *      a. Two-sample Kolmogorov-Smirnov on contiguous halves of
 *         the window -- identical-distribution check (§III-B).
 *      b. Wald-Wolfowitz runs test on sign(x_{i+1}-x_i) --
 *         independence check (§V-C).
 *      c. Block-maxima sampling into disjoint blocks of size
 *         `block_size` (§II-B), require >= `min_blocks` blocks.
 *      d. Gumbel parameter estimation via QQ-plot linear
 *         regression against -ln(-ln(p_i)) (§II-A).
 *      e. Continuous-rank-probability-score convergence check
 *         against the previous fit (§III-C, §III-D).
 *      f. Once `n_conv` consecutive CRPS values stay below
 *         `crps_threshold`, declare state PWCET_VALID and
 *         publish pWCET(eps_node) = mu - sigma*ln(-ln(1-eps))
 *         (§III-D step 6).
 *
 * The ET test that the paper places between (c) and (d) checks
 * that the block-maxima distribution falls in the Gumbel
 * sub-family of GEV. This implementation assumes Gumbel directly:
 * the audio-scheduling workload is the textbook stationary,
 * stable-architecture case the paper's main example targets, and
 * the residual standard error of the QQ-plot regression
 * (recorded for diagnostics) is the conservative proxy. A future
 * ET-test landing is straightforward; the state machine reserves
 * NON_GUMBEL for it.
 *
 * The estimator is single-threaded: callers serialise calls to
 * mbpta_add_sample and mbpta_step against each other. The audio
 * recalc worker satisfies this naturally.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Six states the estimator moves through. Only PWCET_VALID is
 * permitted to drive the kernel runtime budget from
 * pWCET(eps_node); every other state falls back to whatever
 * empirical / fallback budget the caller chooses.
 *
 *   INSUFFICIENT_DATA  -- not enough samples for a block-maxima
 *                         fit yet.
 *   IID_PENDING        -- KS or runs test rejected; not safe to
 *                         fit until the rejection clears.
 *   NON_GUMBEL         -- ET test rejected the Gumbel sub-family
 *                         (reserved; current implementation
 *                         does not run the ET test).
 *   PENDING_CONVERGENCE -- Gumbel fit obtained but CRPS has not
 *                          stabilised across enough rounds.
 *   PWCET_VALID        -- fit converged; pWCET(eps_node) usable.
 *   DRIFT              -- previously valid fit has been
 *                         invalidated by sustained i.i.d.
 *                         rejection; runtime falls back to the
 *                         soft path while a new fit is built.
 */
enum mbpta_state {
	MBPTA_INSUFFICIENT_DATA   = 0,
	MBPTA_IID_PENDING         = 1,
	MBPTA_NON_GUMBEL          = 2,
	MBPTA_PENDING_CONVERGENCE = 3,
	MBPTA_PWCET_VALID         = 4,
	MBPTA_DRIFT               = 5,
};

const char *mbpta_state_name(enum mbpta_state s);

/* Configuration block. The defaults the plan recommends are:
 *   sample_window     = 2048
 *   warmup_discard    = 64
 *   block_size        = 32
 *   min_blocks        = 50
 *   alpha_iid         = 0.05  (KS)
 *   n_delta           = 50
 *   n_conv            = 5
 *   crps_threshold    = 0.1
 *   eps_node          = 1e-9
 *   n_iid_reject      = 3
 *
 * The caller picks values appropriate for the workload; the
 * defaults here are starting points (the plan calls them
 * placeholders pending a calibration step). */
struct mbpta_config {
	uint32_t sample_window;
	uint32_t warmup_discard;
	uint32_t block_size;
	uint32_t min_blocks;
	double   alpha_iid;
	uint32_t n_delta;
	uint32_t n_conv;
	double   crps_threshold;
	double   eps_node;
	uint32_t n_iid_reject;
	/* Two complementary gates decide whether the block-maxima
	 * series is admissible as Gumbel (Cucu-Grosjean 2012 §II-A):
	 *
	 *   * `alpha_et`            -- significance level for the
	 *     formal ET test on the GEV shape parameter k. The test
	 *     uses Hosking-Wallis (1985) probability-weighted
	 *     moments to estimate k from the block-maxima series and
	 *     rejects H_0: k = 0 (Gumbel sub-family of GEV) when the
	 *     two-sided p-value drops below alpha_et.
	 *     Default 0.05.
	 *
	 *   * `gumbel_r2_threshold` -- a weaker, complementary check
	 *     on the linearity of the QQ regression: a Gumbel series
	 *     hugs a straight line on the QQ plot, so a low R^2 is
	 *     evidence the distribution is not Gumbel. Kept alongside
	 *     the ET test as a fast sanity gate -- a high-skew tail
	 *     can fail QQ linearity even before the PWM-based shape
	 *     estimator stabilises. R^2 below the threshold lands
	 *     the estimator in NON_GUMBEL just as an ET rejection
	 *     would. Default 0.90.
	 */
	double   alpha_et;
	double   gumbel_r2_threshold;
};

typedef struct mbpta mbpta_t;

mbpta_t *mbpta_create(const struct mbpta_config *cfg);
void     mbpta_destroy(mbpta_t *e);

/* Reasons the estimator key fingerprint can change. Surfaced
 * via mbpta_last_invalidation_reason for operator diagnostics --
 * a follower whose estimator keeps rebuilding gives the
 * operator a clear cause to investigate.
 *
 * The PLUGIN_MODE reason covers any change to the plugin's
 * internal configuration that materially changes the distribution
 * of per-cycle execution times: a convolver impulse-response
 * swap, a filter-chain bypass toggle, a synth polyphony cap, a
 * resampler-quality switch. The runtime layer that detects such a
 * change calls mbpta_invalidate_with_reason(e, ..._PLUGIN_MODE) so
 * the next fit window starts after the old behaviour stops
 * contributing samples. */
enum mbpta_invalidation_reason {
	MBPTA_INVALIDATED_NONE              = 0,
	MBPTA_INVALIDATED_PERIOD            = 1,
	MBPTA_INVALIDATED_FUSION_GROUP      = 2,
	MBPTA_INVALIDATED_TOPOLOGY_GENERATION = 3,
	MBPTA_INVALIDATED_CPU_CLASS         = 4,
	MBPTA_INVALIDATED_OPERATOR_REQUEST  = 5,
	MBPTA_INVALIDATED_PLUGIN_MODE       = 6,
};

const char *mbpta_invalidation_reason_name(enum mbpta_invalidation_reason r);

/* Drop every cached sample and reset to INSUFFICIENT_DATA. Used
 * on estimator-key change (period, sample rate, fusion-group
 * membership, CPU class assignment, topology generation). The
 * reason is stored and exposed via mbpta_last_invalidation_reason. */
void mbpta_invalidate(mbpta_t *e);

void mbpta_invalidate_with_reason(mbpta_t *e,
		enum mbpta_invalidation_reason reason);

enum mbpta_invalidation_reason mbpta_last_invalidation_reason(const mbpta_t *e);

/* Add a raw per-cycle execution-time sample in nanoseconds.
 * Returns true if the call triggered a re-evaluation (every
 * n_delta-th sample after the warm-up discard). */
bool mbpta_add_sample(mbpta_t *e, uint64_t sample_ns);

enum mbpta_state mbpta_state(const mbpta_t *e);
uint32_t mbpta_sample_count(const mbpta_t *e);
uint32_t mbpta_block_count(const mbpta_t *e);

/* Gumbel parameters of the last fit. 0.0 / 0.0 when no fit has
 * been computed yet. */
double mbpta_mu(const mbpta_t *e);
double mbpta_sigma(const mbpta_t *e);

/* The most recent KS statistic and runs-test Z, plus the last
 * CRPS value. Useful for diagnostics. The associated two-sided
 * p-values are derived from the same evaluation round: ks_pvalue
 * uses the Smirnov asymptotic series (Stephens 1970 small-sample
 * correction); runs_pvalue uses the normal-approximation
 * erfc(|z|/sqrt(2)). Both default to 1.0 (no evidence against
 * H_0) before the first evaluation round runs. */
double mbpta_ks_stat(const mbpta_t *e);
double mbpta_ks_pvalue(const mbpta_t *e);
double mbpta_runs_z(const mbpta_t *e);
double mbpta_runs_pvalue(const mbpta_t *e);
/* Exponential-tail test on the GEV shape parameter k. The PWM
 * estimator k_hat (Hosking & Wallis 1985) is reported via
 * mbpta_gev_shape_k; the two-sided p-value for H_0: k = 0 (Gumbel
 * sub-family) is reported via mbpta_et_pvalue. Both default to
 * (1.0, 0.0) on a fresh estimator. The estimator routes to
 * NON_GUMBEL when the p-value falls below the configured
 * alpha_et. */
double mbpta_et_pvalue(const mbpta_t *e);
double mbpta_gev_shape_k(const mbpta_t *e);
/* Goodness-of-fit diagnostics for the QQ-plot regression:
 * coefficient of determination R^2 and residual standard error
 * RSE = sqrt(SS_res / (n - 2)). R^2 close to 1 and RSE small
 * relative to the fitted scale are evidence the block-maxima
 * series is well-modelled by a Gumbel. R^2 is the gating
 * predicate today; RSE rides along as a diagnostic
 * (Cucu-Grosjean 2012 §II-A names it). Both default to 0.0
 * before the first fit. */
double mbpta_gumbel_r2(const mbpta_t *e);
double mbpta_gumbel_rse(const mbpta_t *e);
double mbpta_crps(const mbpta_t *e);
uint32_t mbpta_convergence_streak(const mbpta_t *e);
uint32_t mbpta_iid_reject_streak(const mbpta_t *e);

/* pWCET at the configured exceedance probability eps_node:
 *
 *     pWCET = mu - sigma * ln(-ln(1 - eps_node))
 *
 * Returns 0 when state != PWCET_VALID. */
uint64_t mbpta_pwcet_ns(const mbpta_t *e);

/* Effective per-node exceedance probability actually used to
 * compute pWCET. Equal to cfg.eps_node when that value is at or
 * above the working-precision floor (1e-16, the value Cucu-Grosjean
 * 2012 §III-D step 6 quotes), otherwise clamped to the floor so the
 * Gumbel inverse-CDF evaluation stays in the representable range.
 * mbpta_eps_node_capped() is true exactly when the clamp was
 * applied; surface both in diagnostics so an operator-set value
 * below the floor is observable. */
double mbpta_effective_eps_node(const mbpta_t *e);
bool   mbpta_eps_node_capped(const mbpta_t *e);

/*
 * Pure policy predicate: should the kernel `runtime` field carry
 * pWCET(eps_node) for a contracted node whose estimator is in the
 * given state?
 *
 * Returns true iff state == MBPTA_PWCET_VALID AND
 * accept_probabilistic_hard is true. This is the only combination
 * the plan permits to drive the kernel runtime from a probabilistic
 * tail extrapolation: a converged fit AND explicit operator opt-in.
 * Every other combination falls back to empirical / fallback
 * budgets (Cucu-Grosjean 2012 §III-D; see also Phase 8.9 of the
 * project's scheduling-model reference, which surfaces the opt-in
 * knob as deadline.mbpta.accept_probabilistic_hard).
 *
 * Pure data: no estimator handle required. The runtime caller in
 * module-deadline.c projects (mbpta_state(estimator),
 * impl->mbpta_accept_probabilistic_hard) through this predicate to
 * decide between the pWCET path and the soft / fallback path.
 */
bool mbpta_runtime_uses_pwcet(enum mbpta_state state,
		bool accept_probabilistic_hard);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_MBPTA_H */
