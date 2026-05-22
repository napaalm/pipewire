#ifndef MODULE_DEADLINE_BUDGET_WARNINGS_H
#define MODULE_DEADLINE_BUDGET_WARNINGS_H

/*
 * Pure decision helpers that drive the HDL-W010 / HDL-W011 / HDL-W020
 * warning emissions from the per-sample budget selection in
 * apply_sample. The helpers live in a separate translation unit so a
 * unit test can exercise every fire / clear transition without the
 * weight of a full module-deadline.c instance.
 *
 * No PipeWire state, no logging, no global mutation. Inputs describe
 * the snapshot at the end of one runtime_select_for_node pass; output
 * is a struct of edge-triggered booleans the caller emits as it
 * pleases.
 */

#include <stdbool.h>
#include <stdint.h>

#include "diag.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-call result describing every warning state transition that the
 * latest budget-selection invocation produced for one follower.
 *
 * Convention:
 *   - `fire_*`  is true on the rising edge from "not yet warned" to
 *               "condition observed". Set the follower's persistent
 *               flag to true alongside the log line.
 *   - `clear_*` is true on the falling edge from "warned" to
 *               "condition cleared". Set the follower's persistent
 *               flag to false alongside the (info-level) log line.
 *   - Both false: no log line for this warning on this call.
 *
 * HDL-W020 is one-shot per follower lifetime: it has no clear edge.
 * `fire_w020` is true on the very first call that observes the
 * condition; every subsequent call returns false even when the
 * condition holds, because the follower's
 * `warned_predicted_below_cputime` is already set.
 */
struct budget_warning_events {
	bool fire_w010;
	bool clear_w010;
	bool fire_w011;
	bool clear_w011;
	bool fire_w020;
};

/*
 * Inputs to compute_budget_warning_events.
 *
 * `sel_kind`, `sel_value_ns`, `sel_sample_count` come from the
 * runtime_select_result the predicate returned. `sample_ref` is the
 * reference-CPU-normalised most-recent sample (zero on the very first
 * activation or when the cycle-derived value is unavailable).
 * `runtime` is the raw runtime measured for the same sample, before
 * normalisation; HDL-W020 compares the published budget against this
 * value.
 *
 * `conformal_table_present` is true iff the follower owns a non-NULL
 * rt_conformal_table_t at call time (the lazy-init in apply_sample
 * may have failed). HDL-W010 only fires when the table is present:
 * a NULL table means the conformal pipeline is not running for this
 * follower for unrelated reasons and the missing-class-stats
 * condition is irrelevant.
 *
 * `current_used_bootstrap` is what runtime_select_for_node stamped
 * on n->budget_used_bootstrap for the current call;
 * `prev_used_bootstrap` is what the field held going in. The pair
 * drives HDL-W011's edge-triggered set / clear.
 *
 * `prior_warned_*` are the follower's persistent dedup flags. Each
 * fire_* output requires the corresponding prior_warned_* to be
 * false; each clear_* requires it to be true.
 */
struct budget_warning_inputs {
	enum rt_diag_budget_kind sel_kind;
	uint64_t sel_value_ns;
	uint64_t sel_sample_count;
	uint64_t sample_ref;
	uint64_t runtime;
	bool conformal_table_present;
	bool current_used_bootstrap;
	bool prev_used_bootstrap;
	bool prior_warned_no_class_stats;
	bool prior_warned_big_bootstrap;
	bool prior_warned_predicted_below_cputime;
};

static inline struct budget_warning_events
compute_budget_warning_events(const struct budget_warning_inputs *in)
{
	struct budget_warning_events ev = { 0 };
	if (in == NULL)
		return ev;

	/* HDL-W011 edge-triggered: the bootstrap flag flipped state
	 * between the previous and current selection. */
	if (in->current_used_bootstrap && !in->prev_used_bootstrap)
		ev.fire_w011 = true;
	else if (!in->current_used_bootstrap && in->prev_used_bootstrap)
		ev.clear_w011 = true;

	/* HDL-W010 edge-triggered: the per-class window produced
	 * neither a target-class budget nor a LITTLE bootstrap, so the
	 * predicate fell through to the peak-hold floor. The signal:
	 * the predicate reported an adaptive-conformal kind (its
	 * default) but with samples_used == 0, meaning no estimator
	 * produced a publishable value. A NULL table means the
	 * conformal pipeline is not running for this follower at all
	 * and the warning is irrelevant. */
	bool class_stats_missing =
		(in->sel_kind == RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL) &&
		in->sel_sample_count == 0 &&
		in->sample_ref > 0 &&
		in->conformal_table_present;
	if (class_stats_missing && !in->prior_warned_no_class_stats)
		ev.fire_w010 = true;
	else if (!class_stats_missing && in->prior_warned_no_class_stats)
		ev.clear_w010 = true;

	/* HDL-W020 one-shot per follower: predicted budget below the
	 * last measured runtime on at least one sample. The condition
	 * is sample-volatile (a single noisy cycle can flap it);
	 * one-shot dedup prevents log flooding. No clear edge by
	 * design -- the follower struct must be torn down and rebuilt
	 * (via node_unregister / node_register) to re-arm the warning. */
	bool predicted_below_cputime =
		in->runtime > 0 && in->sel_value_ns > 0 &&
		in->sel_value_ns < in->runtime;
	if (predicted_below_cputime &&
			!in->prior_warned_predicted_below_cputime)
		ev.fire_w020 = true;

	return ev;
}

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_BUDGET_WARNINGS_H */
