/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/budget_warnings.h"
#include "../src/modules/module-deadline/diag.h"

/* Default-shaped input that triggers no warnings. Individual tests
 * mutate one or two fields to exercise a single condition at a time. */
static struct budget_warning_inputs default_input(void)
{
	struct budget_warning_inputs in = {
		.sel_kind         = RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL,
		.sel_value_ns     = 50000,
		.sel_sample_count = 64,
		.sample_ref       = 50000,
		.runtime          = 49500,
		.conformal_table_present = true,
		.current_used_bootstrap  = false,
		.prev_used_bootstrap     = false,
		.prior_warned_no_class_stats = false,
		.prior_warned_big_bootstrap  = false,
		.prior_warned_predicted_below_cputime = false,
	};
	return in;
}

/* ---------------------------------------------------------------- */
/* HDL-W010: missing per-class statistics.                          */
/* ---------------------------------------------------------------- */

PWTEST(budget_warnings_w010_fires_when_class_stats_empty)
{
	struct budget_warning_inputs in = default_input();
	in.sel_sample_count = 0;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_true(ev.fire_w010);
	pwtest_bool_false(ev.clear_w010);
	pwtest_bool_false(ev.fire_w011);
	pwtest_bool_false(ev.fire_w020);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w010_does_not_refire_when_already_warned)
{
	struct budget_warning_inputs in = default_input();
	in.sel_sample_count = 0;
	in.prior_warned_no_class_stats = true;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w010);
	pwtest_bool_false(ev.clear_w010);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w010_clears_when_samples_arrive)
{
	struct budget_warning_inputs in = default_input();
	in.sel_sample_count = 32;
	in.prior_warned_no_class_stats = true;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w010);
	pwtest_bool_true(ev.clear_w010);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w010_skipped_when_table_absent)
{
	/* A NULL conformal table means the per-class pipeline is not
	 * running for this follower; HDL-W010 must stay silent. */
	struct budget_warning_inputs in = default_input();
	in.sel_sample_count = 0;
	in.conformal_table_present = false;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w010);
	pwtest_bool_false(ev.clear_w010);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w010_skipped_when_sample_ref_zero)
{
	/* A sample with sample_ref == 0 (cycles unavailable, runtime
	 * was zero too) carries no information about the per-class
	 * window state. Treat it as a no-op. */
	struct budget_warning_inputs in = default_input();
	in.sel_sample_count = 0;
	in.sample_ref = 0;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w010);
	pwtest_bool_false(ev.clear_w010);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w010_skipped_when_kind_is_not_conformal)
{
	/* When a deterministic or manual override drives the budget,
	 * the per-class window's readiness is irrelevant. */
	struct budget_warning_inputs in = default_input();
	in.sel_kind = RT_DIAG_BUDGET_DETERMINISTIC_WCET;
	in.sel_sample_count = 0;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w010);
	pwtest_bool_false(ev.clear_w010);
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* HDL-W011: BIG bootstrap from LITTLE statistics.                  */
/* ---------------------------------------------------------------- */

PWTEST(budget_warnings_w011_fires_on_bootstrap_engaged)
{
	struct budget_warning_inputs in = default_input();
	in.current_used_bootstrap = true;
	in.prev_used_bootstrap = false;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_true(ev.fire_w011);
	pwtest_bool_false(ev.clear_w011);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w011_does_not_refire_while_bootstrap_stays_engaged)
{
	struct budget_warning_inputs in = default_input();
	in.current_used_bootstrap = true;
	in.prev_used_bootstrap = true;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w011);
	pwtest_bool_false(ev.clear_w011);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w011_clears_when_big_window_ready)
{
	struct budget_warning_inputs in = default_input();
	in.current_used_bootstrap = false;
	in.prev_used_bootstrap = true;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w011);
	pwtest_bool_true(ev.clear_w011);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w011_independent_of_w010_state)
{
	/* W011 and W010 can fire on the same call. */
	struct budget_warning_inputs in = default_input();
	in.current_used_bootstrap = true;
	in.prev_used_bootstrap = false;
	in.sel_sample_count = 0;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_true(ev.fire_w011);
	pwtest_bool_true(ev.fire_w010);
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* HDL-W020: predicted below last measured cputime.                  */
/* ---------------------------------------------------------------- */

PWTEST(budget_warnings_w020_fires_on_first_under_estimation)
{
	struct budget_warning_inputs in = default_input();
	in.sel_value_ns = 1000;
	in.runtime = 5000;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_true(ev.fire_w020);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w020_one_shot_per_follower)
{
	struct budget_warning_inputs in = default_input();
	in.sel_value_ns = 1000;
	in.runtime = 5000;
	in.prior_warned_predicted_below_cputime = true;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w020);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w020_silent_when_predicted_meets_runtime)
{
	struct budget_warning_inputs in = default_input();
	in.sel_value_ns = 50000;
	in.runtime = 50000; /* equal, not below */
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w020);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w020_silent_when_predicted_above_runtime)
{
	struct budget_warning_inputs in = default_input();
	in.sel_value_ns = 100000;
	in.runtime = 50000;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w020);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w020_silent_when_runtime_zero)
{
	/* A degenerate sample (runtime == 0) carries no comparison
	 * point. The published budget cannot be "below" zero. */
	struct budget_warning_inputs in = default_input();
	in.sel_value_ns = 100;
	in.runtime = 0;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w020);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w020_silent_when_predicted_zero)
{
	/* A zero published budget is the "bootstrap fallback failed"
	 * sentinel; the W020 comparison is meaningless then. */
	struct budget_warning_inputs in = default_input();
	in.sel_value_ns = 0;
	in.runtime = 5000;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w020);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_w020_does_not_clear)
{
	/* W020 has no clear edge; even when the condition clears, the
	 * struct's flag stays set and no clear event fires. */
	struct budget_warning_inputs in = default_input();
	in.sel_value_ns = 100000;
	in.runtime = 50000; /* predicted above runtime */
	in.prior_warned_predicted_below_cputime = true;
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w020);
	/* No `clear_w020` field exists by design; the struct cannot
	 * emit a clear edge for this warning. */
	return PWTEST_PASS;
}

/* ---------------------------------------------------------------- */
/* Safety / NULL handling.                                          */
/* ---------------------------------------------------------------- */

PWTEST(budget_warnings_null_input_returns_no_events)
{
	struct budget_warning_events ev = compute_budget_warning_events(NULL);
	pwtest_bool_false(ev.fire_w010);
	pwtest_bool_false(ev.clear_w010);
	pwtest_bool_false(ev.fire_w011);
	pwtest_bool_false(ev.clear_w011);
	pwtest_bool_false(ev.fire_w020);
	return PWTEST_PASS;
}

PWTEST(budget_warnings_quiet_steady_state_emits_nothing)
{
	/* Every follower's typical sample: per-class stats present,
	 * predicted comfortably above runtime, no bootstrap, no
	 * prior warnings outstanding. The function must produce a
	 * zero events struct. */
	struct budget_warning_inputs in = default_input();
	struct budget_warning_events ev = compute_budget_warning_events(&in);
	pwtest_bool_false(ev.fire_w010);
	pwtest_bool_false(ev.clear_w010);
	pwtest_bool_false(ev.fire_w011);
	pwtest_bool_false(ev.clear_w011);
	pwtest_bool_false(ev.fire_w020);
	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_budget_warnings)
{
	pwtest_add(budget_warnings_w010_fires_when_class_stats_empty,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w010_does_not_refire_when_already_warned,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w010_clears_when_samples_arrive,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w010_skipped_when_table_absent,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w010_skipped_when_sample_ref_zero,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w010_skipped_when_kind_is_not_conformal,
			PWTEST_NOARG);

	pwtest_add(budget_warnings_w011_fires_on_bootstrap_engaged,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w011_does_not_refire_while_bootstrap_stays_engaged,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w011_clears_when_big_window_ready,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w011_independent_of_w010_state,
			PWTEST_NOARG);

	pwtest_add(budget_warnings_w020_fires_on_first_under_estimation,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w020_one_shot_per_follower, PWTEST_NOARG);
	pwtest_add(budget_warnings_w020_silent_when_predicted_meets_runtime,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w020_silent_when_predicted_above_runtime,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w020_silent_when_runtime_zero,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w020_silent_when_predicted_zero,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_w020_does_not_clear, PWTEST_NOARG);

	pwtest_add(budget_warnings_null_input_returns_no_events,
			PWTEST_NOARG);
	pwtest_add(budget_warnings_quiet_steady_state_emits_nothing,
			PWTEST_NOARG);

	return PWTEST_PASS;
}
