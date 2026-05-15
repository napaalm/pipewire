/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_FUSION_COST_H
#define PIPEWIRE_FUSION_COST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cost-model primitives for subgraph fusion in pw_context_recalc_graph.
 *
 * Background
 * ----------
 * Two strands of real-time DAG-scheduling literature directly underpin the
 * decisions made here:
 *
 *  - Sarkar, V., "Partitioning and Scheduling Parallel Programs for
 *    Multiprocessors", MIT Press / Pitman, 1989, chapter 5.3
 *    "Internalisation Pre-pass". Sarkar's algorithm walks the candidate
 *    edges in priority order and merges the two clusters terminating an
 *    edge iff the parallel completion time does not increase. Step 6.e in
 *    procedure PartitionGraph (figure 5-12) reads "the internalisation
 *    does not cause an increase in the parallel execution time if and
 *    only if PARTIME_new <= PARTIME, which is the condition in 6.e". We
 *    apply the same criterion here, but specialised to the audio-graph
 *    setting:
 *        * the "parallel" alternative is the graph still split across N
 *          threads, with one eventfd-wake hop per critical-path edge --
 *          cp_wcet + cp_hops * W_wakeup;
 *        * the "merged" alternative is the same nodes running
 *          sequentially on one thread -- sum_wcet, with zero internal
 *          wakeups.
 *    Merging is profitable when sum_wcet <= cp_wcet + cp_hops * W_wakeup.
 *
 *  - Gerasoulis, A. and Yang, T., "On the Granularity and Clustering of
 *    Directed Acyclic Task Graphs", IEEE TPDS 4(6):686-701, June 1993,
 *    DOI 10.1109/71.242154. The paper proves that for a coarse-grain DAG
 *    every nonlinear clustering admits an equivalent linear clustering
 *    of equal-or-smaller parallel time. A degree-1 chain in our graph is
 *    exactly a linear cluster: cp_hops = N-1, sum_wcet = cp_wcet, so
 *    Sarkar's criterion above collapses to 0 <= (N-1) * W_wakeup which
 *    is unconditionally true. The corollary -- "chain merging is
 *    always non-worsening" -- is exactly the design assumption that the
 *    pre-existing context.merge-adjacent-chains feature relied on
 *    without justification. We now have one.
 *
 *  - Shi, J., Guenzel, M., Ueter, N., von der Brueggen, G., Chen, J.-J.,
 *    "DAG Scheduling with Execution Groups", RTAS 2024, citation
 *    RTAS2024.05. The EG-DAG model formalises sets of subtasks
 *    constrained to execute on the same processor (Definition 1,
 *    "execution group") and analyses their response time. In our
 *    setting a fused subgraph IS an execution group: nodes share a TID
 *    so they share a kernel scheduling entity. The terminology used
 *    here ("group" / "execution group") follows that paper; the
 *    response-time analysis lives in module-deadline (worst-fit
 *    placement, sum_runtime / sum_deadline accumulation), not here.
 *
 * Inputs
 * ------
 * The caller fills a `struct pw_fusion_component` with:
 *   - `n_nodes`           number of eligible nodes in the component;
 *   - `sum_wcet_ns`       sum of per-node smoothed WCETs;
 *   - `cp_wcet_ns`        longest weighted path within the component;
 *   - `cp_hops`           number of edges on the longest path;
 *   - `min_samples_seen`  the smallest sample count among members of the
 *                         component (used by the caller to gate the
 *                         decision on a sufficiently warm WCET estimate);
 *
 * and `struct pw_fusion_params`:
 *   - `wakeup_cost_ns`    cost of one cross-thread eventfd+epoll wake
 *                         (default 3000 ns -- a conservative upper bound
 *                         for x86_64 with a hot cache); the live test
 *                         scripts can pin this empirically.
 *   - `min_samples`       per-node WCET sample threshold below which we
 *                         refuse to apply the Sarkar criterion (and fall
 *                         back to the Gerasoulis-Yang linear-cluster
 *                         guarantee, i.e. chain-only fusion).
 *
 * Output
 * ------
 *   PW_FUSION_DECISION_FUSE        -- merge the whole component into
 *                                     one execution group (Sarkar's
 *                                     criterion holds).
 *   PW_FUSION_DECISION_LINEAR_ONLY -- not enough WCET samples, OR the
 *                                     component fails Sarkar's
 *                                     criterion; restrict fusion to
 *                                     the linear (chain) sub-clusters
 *                                     inside the component, which the
 *                                     coarse-grain theorem permits
 *                                     unconditionally.
 *   PW_FUSION_DECISION_SPLIT       -- N <= 1: nothing to fuse;
 *                                     caller should leave the component
 *                                     ungrouped.
 *
 * Pure data: this header is intentionally self-contained so the cost
 * model can be unit-tested without bringing up a pw_context.
 */

struct pw_fusion_component {
	uint32_t n_nodes;
	uint64_t sum_wcet_ns;
	uint64_t cp_wcet_ns;
	uint32_t cp_hops;
	uint32_t min_samples_seen;
};

struct pw_fusion_params {
	uint64_t wakeup_cost_ns;
	uint32_t min_samples;
};

enum pw_fusion_decision {
	PW_FUSION_DECISION_SPLIT = 0,
	PW_FUSION_DECISION_LINEAR_ONLY,
	PW_FUSION_DECISION_FUSE,
};

/* Apply Sarkar's internalisation criterion to the component.
 *
 * Returns PW_FUSION_DECISION_SPLIT when fusion does not apply (n_nodes
 * <= 1, or cp_hops == 0 which means a disconnected singleton).
 *
 * Returns PW_FUSION_DECISION_LINEAR_ONLY when the WCET samples have not
 * accumulated to `params->min_samples`, OR the parallel-time criterion
 * fails. The caller is then expected to retry with the linear (chain)
 * sub-components only, which Gerasoulis-Yang prove are
 * non-pessimistic.
 *
 * Returns PW_FUSION_DECISION_FUSE when Sarkar's criterion holds:
 *     sum_wcet <= cp_wcet + cp_hops * wakeup_cost.
 *
 * cp_hops is the number of edges along the longest weighted path, which
 * is the number of cross-thread eventfd wakes that the split schedule
 * would pay along the critical path. For a chain of N nodes cp_hops is
 * always N-1 and cp_wcet equals sum_wcet, so the inequality reduces to
 * 0 <= (N-1) * W_wakeup -- always profitable, matching the
 * pre-existing chain-merge contract.
 */
static inline enum pw_fusion_decision
pw_fusion_decide(const struct pw_fusion_component *c,
		const struct pw_fusion_params *p)
{
	uint64_t budget;

	if (c == NULL || p == NULL)
		return PW_FUSION_DECISION_SPLIT;
	if (c->n_nodes < 2 || c->cp_hops == 0)
		return PW_FUSION_DECISION_SPLIT;

	/* Warm-up gate: until every member has accumulated at least
	 * `min_samples` measurements, we don't trust the sum/cp values
	 * enough to extend fusion past the linear-cluster baseline. The
	 * caller falls back to chain-only fusion, which Gerasoulis-Yang
	 * 1993 guarantees is non-pessimistic for any coarse-grain DAG. */
	if (c->min_samples_seen < p->min_samples)
		return PW_FUSION_DECISION_LINEAR_ONLY;

	/* Sarkar 1989 §5.3 step 6.e:
	 *
	 *     PARTIME_split  = cp_wcet + cp_hops * W_wakeup
	 *     PARTIME_merged = sum_wcet
	 *
	 *     fuse iff PARTIME_merged <= PARTIME_split.
	 *
	 * The arithmetic is in unsigned ns; the budget add cannot
	 * overflow under any realistic graph (cp_hops * W_wakeup is at
	 * most a handful of microseconds for cp_hops in the hundreds,
	 * far below UINT64 limits). */
	budget = c->cp_wcet_ns + (uint64_t)c->cp_hops * p->wakeup_cost_ns;
	if (c->sum_wcet_ns <= budget)
		return PW_FUSION_DECISION_FUSE;

	return PW_FUSION_DECISION_LINEAR_ONLY;
}

/* Exponential moving average for per-node WCET smoothing. The fusion
 * scan reads `prev_run_time` (CLOCK_THREAD_CPUTIME_ID of the previous
 * cycle, set by impl-node.c) every recalc and folds it into the per-
 * node EMA stored on the pw_impl_node. Smoothing matters because a
 * single cycle's runtime can spike on cold-cache wake-ups; basing the
 * fusion decision on the raw last sample would alternately merge and
 * split the same component.
 *
 * The shift parameter is the EMA strength (alpha = 1 / 2^shift); shift
 * == 3 gives alpha = 1/8, which converges within ~24 cycles -- about
 * 4 ms at 6 kHz audio rate, much faster than the human ear can detect a
 * topology change.
 *
 * sample == 0 is treated as "no measurement this cycle" and leaves the
 * EMA untouched. samples_capped is the running count of folded samples,
 * saturating at UINT32_MAX.
 */
static inline void
pw_fusion_ema_update(uint64_t *ema, uint32_t *samples,
		uint64_t sample, unsigned shift)
{
	if (ema == NULL || samples == NULL || sample == 0)
		return;

	if (*samples == 0) {
		*ema = sample;
	} else {
		uint64_t prev = *ema;
		/* new = prev + (sample - prev) / 2^shift, with both signs
		 * handled in unsigned arithmetic to avoid signed-overflow
		 * undefined behaviour for samples close to UINT64_MAX. */
		if (sample > prev)
			*ema = prev + ((sample - prev) >> shift);
		else
			*ema = prev - ((prev - sample) >> shift);
	}

	if (*samples != UINT32_MAX)
		(*samples)++;
}

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_FUSION_COST_H */
