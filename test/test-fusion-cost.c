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
 *   - pw_fusion_ema_update(): the per-node EMA used to smooth
 *     prev_run_time samples before the cost model reads them. We pin
 *     the seed behaviour (first sample initialises the EMA),
 *     convergence under repeated samples, and the "sample == 0
 *     leaves the EMA untouched" no-op contract that the warm-up
 *     gate depends on (so a cycle with no measurement does not
 *     prematurely cross the min_samples threshold).
 *
 * Citations for the criterion itself live in fusion-cost.h.
 */

#include "config.h"

#include <stdint.h>

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
/* pw_fusion_ema_update                                                   */
/* --------------------------------------------------------------------- */

PWTEST(ema_first_sample_seeds_value)
{
	/* The first non-zero sample seeds the EMA directly. Otherwise
	 * the "EMA value 0 + sample" arithmetic would converge from
	 * zero, biasing the warm-up estimate downward. */
	uint64_t ema = 0;
	uint32_t samples = 0;

	pw_fusion_ema_update(&ema, &samples, 1000, 3);
	pwtest_int_eq((int)ema, 1000);
	pwtest_int_eq((int)samples, 1);
	return PWTEST_PASS;
}

PWTEST(ema_zero_sample_is_noop)
{
	/* A zero sample is the "no measurement this cycle" signal
	 * (e.g. a node that did not produce because its peer was
	 * idle). The EMA and the sample count must NOT advance --
	 * otherwise the warm-up gate would prematurely transition to
	 * the Sarkar criterion on an unmoving estimate. */
	uint64_t ema = 5000;
	uint32_t samples = 3;

	pw_fusion_ema_update(&ema, &samples, 0, 3);
	pwtest_int_eq((int)ema, 5000);
	pwtest_int_eq((int)samples, 3);
	return PWTEST_PASS;
}

PWTEST(ema_converges_to_constant_input)
{
	/* Feed a steady 10000 ns sample for 50 cycles. After enough
	 * cycles the EMA should be within a small epsilon of 10000.
	 * With alpha = 1/8, convergence is exponential: after 50 steps
	 * the residual is (7/8)^50 ~ 0.001, i.e. << 1% of the target. */
	uint64_t ema = 0;
	uint32_t samples = 0;
	int i;

	for (i = 0; i < 50; i++)
		pw_fusion_ema_update(&ema, &samples, 10000, 3);

	pwtest_int_eq((int)samples, 50);
	pwtest_int_gt((int)ema, 9950);
	pwtest_int_lt((int)ema, 10050);
	return PWTEST_PASS;
}

PWTEST(ema_handles_high_to_low_step)
{
	/* The EMA should respond symmetrically to up-steps and
	 * down-steps (the implementation branches on sample > prev to
	 * avoid signed-overflow undefined behaviour; this confirms the
	 * negative branch works). */
	uint64_t ema = 10000;
	uint32_t samples = 10;
	int i;

	for (i = 0; i < 50; i++)
		pw_fusion_ema_update(&ema, &samples, 100, 3);

	pwtest_int_gt((int)ema, 0);
	pwtest_int_lt((int)ema, 150);
	return PWTEST_PASS;
}

PWTEST(ema_saturates_samples_at_uint32_max)
{
	/* A long-running node that never gets relocated would
	 * eventually wrap the sample counter; the EMA logic saturates
	 * to UINT32_MAX instead. This keeps "min_samples_seen <
	 * min_samples" stable forever once warm. */
	uint64_t ema = 1000;
	uint32_t samples = UINT32_MAX;

	pw_fusion_ema_update(&ema, &samples, 1000, 3);
	pwtest_int_eq((int)samples, (int)UINT32_MAX);
	return PWTEST_PASS;
}

PWTEST(ema_null_pointers_no_crash)
{
	uint64_t ema = 1000;
	uint32_t samples = 5;
	pw_fusion_ema_update(NULL, &samples, 100, 3);
	pw_fusion_ema_update(&ema, NULL, 100, 3);
	pwtest_int_eq((int)ema, 1000);
	pwtest_int_eq((int)samples, 5);
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
	 * back. The EMA tracks the steady state with shift=3 (alpha =
	 * 1/8). For each phase we apply enough samples (50) that the
	 * EMA converges well into the new regime, then evaluate the
	 * criterion against the smoothed value. */
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	uint64_t ema_source = 0, ema_a = 0, ema_b = 0, ema_sink = 0;
	uint32_t s_source = 0, s_a = 0, s_b = 0, s_sink = 0;
	int i;
	struct pw_fusion_component c;

	/* Phase 1: cheap (1000 ns per node). FUSE. */
	for (i = 0; i < 50; i++) {
		pw_fusion_ema_update(&ema_source, &s_source, 1000, 3);
		pw_fusion_ema_update(&ema_a, &s_a, 1000, 3);
		pw_fusion_ema_update(&ema_b, &s_b, 1000, 3);
		pw_fusion_ema_update(&ema_sink, &s_sink, 1000, 3);
	}
	build_y_component(&c, ema_source, ema_a, ema_b, ema_sink,
			s_source < s_a ? s_source : s_a);
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	/* Phase 2: arm B turns expensive (a convolution reverb is
	 * plugged in). The EMA folds 50000 ns samples until ema_b
	 * approaches 50000. Sum balloons; cp also grows but less,
	 * because cp passes through the heavier of {A, B} only.
	 *     sum_wcet (asymptote) ~ 1000 + 1000 + 50000 + 1000 = 53000
	 *     cp_wcet  (asymptote) ~ 1000 + 50000 + 1000        = 52000
	 *     budget               = 52000 + 2 * 3000           = 58000
	 *     sum (53000) <= budget (58000): still FUSE.
	 *
	 * The cost model agrees with intuition: a single heavy node
	 * inside an otherwise-cheap component doesn't tip the balance,
	 * because the heavy node sits on the critical path either way.
	 * The fusion decision only flips when MULTIPLE branches are
	 * concurrently heavy. */
	for (i = 0; i < 200; i++)
		pw_fusion_ema_update(&ema_b, &s_b, 50000, 3);
	build_y_component(&c, ema_source, ema_a, ema_b, ema_sink, 100);
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_FUSE);

	/* Phase 3: arm A ALSO turns expensive (a second reverb on
	 * the parallel branch). Now both middles are heavy:
	 *     sum_wcet (asymptote) ~ 1000 + 50000 + 50000 + 1000 = 102000
	 *     cp_wcet  (asymptote) ~ 1000 + 50000 + 1000         = 52000
	 *     budget               = 52000 + 2 * 3000            = 58000
	 *     sum (102000) > budget (58000): LINEAR_ONLY.
	 *
	 * Two heavy parallel branches is the canonical "do not merge"
	 * case. The cost model fires correctly. */
	for (i = 0; i < 200; i++)
		pw_fusion_ema_update(&ema_a, &s_a, 50000, 3);
	build_y_component(&c, ema_source, ema_a, ema_b, ema_sink, 100);
	pwtest_int_eq((int)pw_fusion_decide(&c, &p),
			(int)PW_FUSION_DECISION_LINEAR_ONLY);

	/* Phase 4: the reverbs are removed; runtimes drop back to
	 * baseline. The EMA folds in 1000 ns samples for both arms
	 * and the criterion flips back to FUSE once the smoothed
	 * values cross the threshold.
	 *
	 * Convergence is exponential with alpha = 1/8: a 100x drop
	 * in the sample value takes ~log_{8/7}(100) ≈ 35 cycles to
	 * cross most of the gap. With 200 cycles we are well past
	 * the threshold. */
	for (i = 0; i < 200; i++) {
		pw_fusion_ema_update(&ema_a, &s_a, 1000, 3);
		pw_fusion_ema_update(&ema_b, &s_b, 1000, 3);
	}
	build_y_component(&c, ema_source, ema_a, ema_b, ema_sink, 100);
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

PWTEST(fusion_decide_dynamic_hysteresis_from_ema_smoothing)
{
	/* Single isolated spike in arm B should NOT flip a FUSE
	 * decision to LINEAR_ONLY, because the EMA absorbs the spike.
	 * This is the "no oscillation under jitter" property the
	 * smoothing exists to provide.
	 *
	 * Setup: cheap Y-shape running for 100 cycles, then one
	 * monster 100000-ns sample on arm B, then resume baseline.
	 * The criterion is read once after the spike has been folded
	 * in but before the EMA has reverted. We expect FUSE. */
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	uint64_t ema_source = 0, ema_a = 0, ema_b = 0, ema_sink = 0;
	uint32_t s_source = 0, s_a = 0, s_b = 0, s_sink = 0;
	int i;
	struct pw_fusion_component c;

	for (i = 0; i < 100; i++) {
		pw_fusion_ema_update(&ema_source, &s_source, 1000, 3);
		pw_fusion_ema_update(&ema_a, &s_a, 1000, 3);
		pw_fusion_ema_update(&ema_b, &s_b, 1000, 3);
		pw_fusion_ema_update(&ema_sink, &s_sink, 1000, 3);
	}
	/* One outlier sample. */
	pw_fusion_ema_update(&ema_b, &s_b, 100000, 3);

	build_y_component(&c, ema_source, ema_a, ema_b, ema_sink, 100);
	/* The spike folded with alpha=1/8 raises ema_b by (100000 -
	 * 1000) / 8 ~= 12375 above the 1000 baseline. With a single
	 * spiked arm the criterion still holds because cp also moves
	 * up. */
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
	pwtest_add(ema_first_sample_seeds_value, PWTEST_NOARG);
	pwtest_add(ema_zero_sample_is_noop, PWTEST_NOARG);
	pwtest_add(ema_converges_to_constant_input, PWTEST_NOARG);
	pwtest_add(ema_handles_high_to_low_step, PWTEST_NOARG);
	pwtest_add(ema_saturates_samples_at_uint32_max, PWTEST_NOARG);
	pwtest_add(ema_null_pointers_no_crash, PWTEST_NOARG);
	pwtest_add(fusion_decide_multi_source_fan_in_convenient, PWTEST_NOARG);
	pwtest_add(fusion_decide_tree_mixer_convenient, PWTEST_NOARG);
	pwtest_add(fusion_decide_two_heavy_parallel_not_convenient, PWTEST_NOARG);
	pwtest_add(fusion_decide_dynamic_low_to_high_to_low, PWTEST_NOARG);
	pwtest_add(fusion_decide_dynamic_threshold_crossing_is_monotonic, PWTEST_NOARG);
	pwtest_add(fusion_decide_dynamic_hysteresis_from_ema_smoothing, PWTEST_NOARG);
	return PWTEST_PASS;
}
