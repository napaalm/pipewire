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

/* Sliding-window mean for per-node WCET smoothing.
 *
 * The fusion scan reads `prev_run_time` (CLOCK_THREAD_CPUTIME_ID of
 * the previous cycle, set by impl-node.c) every recalc and folds it
 * into the per-node sample buffer that backs this struct. Smoothing
 * matters because a single cycle's runtime can spike on cold-cache
 * wake-ups; basing the fusion decision on the raw last sample would
 * alternately merge and split the same component.
 *
 * Why a sliding mean (and not a t-digest, EMA, or quantile sketch)
 * -----------------------------------------------------------------
 *
 * The cost model in pw_fusion_decide() compares two *expected*
 * parallel completion times:
 *
 *     PARTIME_merged  = E[sum_wcet]
 *     PARTIME_split   = E[cp_wcet] + cp_hops * wakeup_cost.
 *
 * The textbook estimator for an expected value is the sample mean.
 * The project's existing per-node WCET sketch (t-digest, in
 * src/modules/module-deadline/wcet_sketch.[ch]) targets a *high
 * quantile* (p95 by default) because the SCHED_DEADLINE budget
 * needs an upper bound that the task will fit into with high
 * probability. Borrowing that statistic for the fusion decision
 * would systematically over-state sum_wcet and under-trigger
 * fusion -- the opposite of what we want for a central-tendency
 * comparison. The sliding mean here is therefore intentionally a
 * *different* estimator from the SCHED_DEADLINE budget; the two
 * answer two different questions.
 *
 * The previous implementation of this primitive (an exponential
 * moving average with alpha = 1/2^3 = 1/8) had the same
 * expected-value character but is not a paper-grounded estimator.
 * The sliding window with explicit length N maps cleanly onto the
 * MA[n] estimator from Palopoli, Cucinotta, Marzario,
 * "AQuoSA -- adaptive quality of service architecture",
 * Software: Practice and Experience 39(1):1-31, 2009 (Italian
 * pre-print papers/Aquosa-medie-mobili.pdf), where MA[n] is the
 * arithmetic mean of the last n execution-time samples. (The
 * paper's MMA[n,S] variant adds outlier rejection; we defer that
 * extension because the audio domain rarely produces the kind of
 * pathological tails MMA targets, and the residual jitter is well
 * absorbed by a moderately large window -- see N tuning below.)
 *
 * Default window size: time-based, derived from graph variables
 * --------------------------------------------------------------
 *
 * Two opposing arguments set the bounds on the right N:
 *
 *   - Variance reduction. The standard error of an N-sample mean
 *     is sigma / sqrt(N), purely a function of the sample count.
 *     Larger N is always better for variance, capped only by
 *     memory and the rate at which the estimate becomes useless
 *     for tracking genuine workload changes.
 *
 *   - Step-response time. A sliding mean reaches 90% of the new
 *     steady-state in ~0.9 * N cycles, i.e. 0.9 * N * period_ns
 *     wall-clock seconds. Audio-system responsiveness studies put
 *     the user-perceptible-lag threshold for interactive graph
 *     edits around 100-500 ms; we want the windowed mean to track
 *     a genuine regime change within that envelope so a freshly
 *     enabled convolution reverb does not take "long" to register
 *     in the fusion verdict.
 *
 *   - IRQ-jitter averaging. The window must be substantially
 *     longer than the host's IRQ-jitter timescale (a few ms on a
 *     PREEMPT_RT host with a tuned profile, often more on a
 *     vanilla kernel) so the windowed mean is dominated by the
 *     workload, not interrupt noise.
 *
 * The variance argument is sample-count-based and configuration-
 * agnostic; the response and jitter-averaging arguments are
 * wall-clock-based. Picking a fixed sample-count default would
 * give a wall-clock window that varies by ~16x across the
 * PipeWire cycle periods we see (1.3 ms pro-audio, ~5 ms desktop,
 * ~21 ms power-save / Bluetooth), which is enough to drag the
 * extremes outside the principled band.
 *
 * The right default is therefore **a target wall-clock window**,
 * with N derived from the live graph's cycle period at scan
 * time. We pick 250 ms as the target, motivated by:
 *
 *   - longer than typical IRQ-jitter timescale on Linux RT hosts
 *     (a few ms),
 *   - shorter than the 500 ms user-perceptible-lag threshold for
 *     interactive audio controls,
 *   - matches the time-constant rule of thumb for RT control
 *     loops on 1-10 ms sample rates,
 *   - lands in the middle of the principled band on every common
 *     PipeWire cycle period (pro-audio gets a 188-sample window,
 *     desktop gets ~47, power-save gets ~12).
 *
 * To keep that band sane on pathological configurations (a 0.1 ms
 * cycle would suggest N=2500; a 100 ms cycle would suggest N=3)
 * we clamp the derived N to [N_MIN, N_MAX]:
 *
 *   N_MIN = 8:    below this, standard error of the mean is
 *                 > sigma/sqrt(8) = sigma/2.8, > 35% of per-
 *                 sample noise. The windowed mean tracks noise
 *                 more than signal; the criterion would flap.
 *
 *   N_MAX = 512:  beyond this, memory grows linearly (4 KB / node)
 *                 with diminishing variance return (sigma/sqrt(512)
 *                 = sigma/22.6 already, an extra 4x sample count
 *                 only halves the noise again). On slowest cycle
 *                 periods (100 ms) N_MAX gives a 51 s window,
 *                 which is the upper bound of "still responding to
 *                 graph changes".
 *
 * Both bounds are also exposed (as PW_FUSION_WINDOW_N_MIN /
 * PW_FUSION_WINDOW_N_MAX below) so operators with extreme needs
 * can override via the config knobs in context.c. The auto
 * derivation removes the need to think about cycle period for
 * the typical operator -- the default just works on every
 * configuration PipeWire is likely to see.
 *
 * Implementation note: the per-cycle period is per-driver, not
 * per-context. context.c picks a representative period (the first
 * driver with an eligible follower) for the derivation. In a
 * mixed-driver graph the small-N driver dominates (more samples,
 * longer wall-clock history is conservative); this matches the
 * cost-benefit asymmetry -- we are happier with a slightly
 * over-conservative window than with a flappy one.
 *
 * Unit tests verify behavioural invariants (variance scales as
 * 1/N, step response scales linearly with N, eviction keeps the
 * running sum consistent) but do *not* curve-fit a numeric
 * default. That decision lives in this comment, not in a test
 * fixture, so it stays valid across hosts.
 */

/* Bounds on the auto-derived window size. See block comment above
 * for the principled motivation. */
#define PW_FUSION_WINDOW_N_MIN	8u
#define PW_FUSION_WINDOW_N_MAX	512u

/* Default target wall-clock window in nanoseconds. See block
 * comment above. Exposed so context.c and the test suite share one
 * source of truth. */
#define PW_FUSION_WINDOW_DEFAULT_TIME_NS	(250ULL * 1000000ULL)

/* Compute the target sample count from a graph cycle period
 * (period_ns) and a target wall-clock window (window_time_ns),
 * clamped into [PW_FUSION_WINDOW_N_MIN, PW_FUSION_WINDOW_N_MAX].
 * Returns N_MIN if period_ns is 0 (a defensive cold-start case
 * where the driver has not yet negotiated a quantum). */
static inline uint32_t
pw_fusion_window_target_n(uint64_t period_ns, uint64_t window_time_ns)
{
	uint64_t n;
	if (period_ns == 0)
		return PW_FUSION_WINDOW_N_MIN;
	n = window_time_ns / period_ns;
	if (n < PW_FUSION_WINDOW_N_MIN)
		return PW_FUSION_WINDOW_N_MIN;
	if (n > PW_FUSION_WINDOW_N_MAX)
		return PW_FUSION_WINDOW_N_MAX;
	return (uint32_t)n;
}

/*
 * pw_fusion_window: caller-owned sliding-window mean primitive.
 *
 * `samples` is a circular buffer of length `capacity` allocated by
 * the caller (pw_impl_node allocates it lazily from the auto-
 * derived window size at the first WCET update). `count` is the
 * current fill level (saturates at capacity once the window is
 * full); `head` is the next write position; `sum` is the running
 * sum so the mean is O(1) to query.
 *
 * The helpers below are inline so the test suite can exercise the
 * primitive without bringing up a pw_context. Memory management
 * stays with the caller because allocation policy varies (lazy
 * on first sample, pre-allocated at node creation, etc.).
 */
struct pw_fusion_window {
	uint64_t *samples;
	uint32_t  capacity;
	uint32_t  count;
	uint32_t  head;
	uint64_t  sum;
};

/* Fold one runtime sample into the window.
 *
 * sample == 0 is treated as "no measurement this cycle" (the data
 * loop did not run the node, e.g. an idle source) and leaves the
 * window untouched. This is load-bearing: if a zero sample advanced
 * the window the fill-level gate would fire prematurely on a
 * partially-quiet graph.
 *
 * On overflow (count == capacity) the oldest sample is evicted from
 * the running sum before the new one is written; the head pointer
 * wraps and count saturates. The arithmetic is fully O(1).
 */
static inline void
pw_fusion_window_update(struct pw_fusion_window *w, uint64_t sample)
{
	if (w == NULL || w->samples == NULL || w->capacity == 0 || sample == 0)
		return;

	if (w->count == w->capacity)
		w->sum -= w->samples[w->head];
	else
		w->count++;

	w->samples[w->head] = sample;
	w->sum += sample;
	w->head++;
	if (w->head == w->capacity)
		w->head = 0;
}

/* Read the current windowed mean. Returns 0 when the window is empty
 * (the caller uses count to gate that case separately via
 * pw_fusion_decide's min_samples threshold, so a zero from here only
 * means "no samples yet" -- never confuse it with a measured zero). */
static inline uint64_t
pw_fusion_window_mean(const struct pw_fusion_window *w)
{
	if (w == NULL || w->count == 0)
		return 0;
	return w->sum / w->count;
}

/* Reset to empty without freeing the backing buffer. Used by
 * context.c on data-loop relocation: the previous owning thread's
 * prev_run_time is no longer representative of the new thread, so
 * the window must restart its warm-up from zero samples. The buffer
 * itself stays so a relocate-and-warmup loop does not allocate
 * repeatedly. */
static inline void
pw_fusion_window_clear(struct pw_fusion_window *w)
{
	if (w == NULL)
		return;
	w->count = 0;
	w->head = 0;
	w->sum = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_FUSION_COST_H */
