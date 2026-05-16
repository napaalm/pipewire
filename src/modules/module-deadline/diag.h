/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_DEADLINE_DIAG_H
#define MODULE_DEADLINE_DIAG_H

/*
 * diag.h
 *
 * Diagnostic snapshots for module-deadline.
 *
 * The diagnostics layer captures, in plain data form, the state of a
 * driver's scheduling pipeline at a single point in time. The first
 * slice is the raw driver-relative PipeWire graph: every follower
 * the daemon sees on the driver's follower_list together with the
 * link topology between those followers. Subsequent slices (the
 * in-period scheduling DAG, fusion decisions, per-task SCHED_DEADLINE
 * parameters) will be added in lockstep with the rest of the
 * observability series.
 *
 * The data plumbing is intentionally decoupled from the
 * pw_impl_node-walking code in module-deadline.c. The types here
 * know nothing about pw_impl_node, pw_context, or pw_loop; tests can
 * drive the renderer with synthetic snapshots and verify format
 * stability without bringing up a daemon. The module side translates
 * pw_impl_node attributes into the bitmasks defined below and feeds
 * them through the *_add_* helpers.
 *
 * The rendered text is consumed by an operator reading the
 * deadline-recalc worker thread's log lines and by unit tests
 * comparing the formatted output against a golden string.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-node analyzability / role flags for the raw-graph slice.
 *
 * The flags mirror the pw_impl_node attributes that drive the
 * eventual scheduling-DAG inclusion decision: the in-period
 * scheduling model excludes async / exported / main-loop nodes and
 * accepts remote nodes only when their processing TID is published.
 * The renderer reports the bits verbatim; the inclusion semantics
 * are carried by module-deadline.c (and by the scheduling-DAG slice
 * once it lands).
 */
enum rt_diag_raw_node_flag {
	RT_DIAG_RAW_NODE_DRIVER        = 1u << 0,
	RT_DIAG_RAW_NODE_DATA_LOOP     = 1u << 1,
	RT_DIAG_RAW_NODE_MAIN_LOOP     = 1u << 2,
	RT_DIAG_RAW_NODE_REMOTE        = 1u << 3,
	RT_DIAG_RAW_NODE_EXPORTED      = 1u << 4,
	RT_DIAG_RAW_NODE_ASYNC         = 1u << 5,
	RT_DIAG_RAW_NODE_DYNAMIC_LOOP  = 1u << 6,
};

/*
 * Per-edge flags. The two recognised forms of within-period
 * exclusion are feedback links (constructed in cycle N to break a
 * cycle, consumer reads cycle N-1 buffers) and async links (both
 * endpoints opt in to the same one-cycle delay via the port async
 * bit). Both forms transport buffers but introduce no in-period
 * precedence constraint, so the eventual scheduling-DAG slice will
 * surface them in the "excluded edges, with reason" list.
 */
enum rt_diag_raw_edge_flag {
	RT_DIAG_RAW_EDGE_FEEDBACK = 1u << 0,
	RT_DIAG_RAW_EDGE_ASYNC    = 1u << 1,
};

/* Fixed-size name buffer keeps the snapshot struct trivially
 * copyable. 64 bytes is enough for every node.name observed in the
 * thesis live-test corpus; longer names are truncated. */
#define RT_DIAG_NODE_NAME_MAX 64

struct rt_diag_raw_node {
	uint32_t id;
	uint32_t driver_id;
	pid_t    tid;
	uint32_t flags;
	char     name[RT_DIAG_NODE_NAME_MAX];
};

struct rt_diag_raw_edge {
	uint32_t src;
	uint32_t dst;
	uint32_t flags;
};

/*
 * Driver-relative raw-graph snapshot. The nodes and edges arrays
 * grow geometrically; cap_* fields track allocation, n_* fields
 * track logical content. Reset rewinds n_* to zero without freeing,
 * so the snapshot can be reused across recalc passes.
 */
struct rt_diag_raw_snapshot {
	uint32_t driver_id;
	uint64_t generation;
	uint64_t period_ns;
	uint64_t deadline_ns;

	struct rt_diag_raw_node *nodes;
	uint32_t n_nodes;
	uint32_t cap_nodes;

	struct rt_diag_raw_edge *edges;
	uint32_t n_edges;
	uint32_t cap_edges;
};

/* Zero-initialise the struct. Safe on a stack-allocated snapshot. */
void rt_diag_raw_snapshot_init(struct rt_diag_raw_snapshot *s);

/* Free the backing arrays. After fini the struct is back to the
 * post-init state; init may be called again safely. NULL-safe. */
void rt_diag_raw_snapshot_fini(struct rt_diag_raw_snapshot *s);

/* Reset n_nodes / n_edges to zero without freeing. NULL-safe. Used
 * to reuse the snapshot across recalc passes. */
void rt_diag_raw_snapshot_reset(struct rt_diag_raw_snapshot *s);

/* Append a node entry. Copies the caller's struct into the snapshot;
 * the name buffer is copied with explicit truncation to fit the
 * fixed-size field. Returns 0 on success, -EINVAL if s or node is
 * NULL, -ENOMEM on allocation failure. */
int rt_diag_raw_snapshot_add_node(struct rt_diag_raw_snapshot *s,
				  const struct rt_diag_raw_node *node);

/* Append an edge entry. Returns 0 on success, -EINVAL if s or edge
 * is NULL, -ENOMEM on allocation failure. */
int rt_diag_raw_snapshot_add_edge(struct rt_diag_raw_snapshot *s,
				  const struct rt_diag_raw_edge *edge);

/*
 * Render the snapshot to `out` as a stable human-readable text
 * block. The format is one header line followed by one line per
 * node and one line per edge, indented for readability:
 *
 *     deadline-diag-raw: driver=<id> generation=<g> period_ns=<p> deadline_ns=<d>
 *       nodes: <n>
 *         node id=<i> driver_id=<d> tid=<t> flags=<flag,flag,...> name=<n>
 *         ...
 *       edges: <n>
 *         edge <src>-><dst> flags=<flag,flag,...>
 *         ...
 *
 * flags=- when the bitmask is zero. The renderer is deterministic
 * over the snapshot's contents so unit tests can pin the exact
 * output.
 */
void rt_diag_raw_snapshot_render_text(const struct rt_diag_raw_snapshot *s,
				      FILE *out);

/*
 * Scheduling-DAG slice.
 *
 * Where the raw-graph slice records every follower and every link
 * verbatim, the scheduling-DAG slice records the subset that
 * contributes an in-period precedence constraint -- the actual
 * input to the deadline analysis. The dump surfaces (a) the
 * included nodes, (b) the included edges, and (c) every edge that
 * was rejected from the included set, tagged with a single
 * exclusion-reason code so the operator can correlate the
 * scheduling decision against the raw graph.
 *
 * The reason set is closed: every excluded edge must carry exactly
 * one reason from the enum below. NONE is the sentinel for "not
 * excluded"; it must never appear in the excluded-edges list. New
 * reasons may be appended; the textual token is stable.
 */
enum rt_diag_sched_exclude_reason {
	RT_DIAG_SCHED_EXC_NONE         = 0,
	RT_DIAG_SCHED_EXC_FEEDBACK     = 1,
	RT_DIAG_SCHED_EXC_ASYNC        = 2,
	RT_DIAG_SCHED_EXC_CROSS_DRIVER = 3,
	RT_DIAG_SCHED_EXC_EXPORTED     = 4,
	RT_DIAG_SCHED_EXC_NON_RT       = 5,
	RT_DIAG_SCHED_EXC_UNSUPPORTED  = 6,
};

struct rt_diag_sched_node {
	uint32_t id;
	pid_t    tid;
};

struct rt_diag_sched_edge {
	uint32_t src;
	uint32_t dst;
};

struct rt_diag_sched_excluded_edge {
	uint32_t src;
	uint32_t dst;
	enum rt_diag_sched_exclude_reason reason;
};

struct rt_diag_sched_snapshot {
	uint32_t driver_id;
	uint64_t generation;
	uint64_t period_ns;
	uint64_t deadline_ns;

	struct rt_diag_sched_node *nodes;
	uint32_t n_nodes;
	uint32_t cap_nodes;

	struct rt_diag_sched_edge *edges;
	uint32_t n_edges;
	uint32_t cap_edges;

	struct rt_diag_sched_excluded_edge *excluded_edges;
	uint32_t n_excluded;
	uint32_t cap_excluded;
};

void rt_diag_sched_snapshot_init(struct rt_diag_sched_snapshot *s);
void rt_diag_sched_snapshot_fini(struct rt_diag_sched_snapshot *s);
void rt_diag_sched_snapshot_reset(struct rt_diag_sched_snapshot *s);

int rt_diag_sched_snapshot_add_node(struct rt_diag_sched_snapshot *s,
				    const struct rt_diag_sched_node *node);
int rt_diag_sched_snapshot_add_edge(struct rt_diag_sched_snapshot *s,
				    const struct rt_diag_sched_edge *edge);
int rt_diag_sched_snapshot_add_excluded(struct rt_diag_sched_snapshot *s,
					const struct rt_diag_sched_excluded_edge *edge);

/* Stable token for a given exclusion reason. The strings are kept
 * narrow (lower_snake_case) so the rendered text is grep-friendly.
 * Unknown reasons render as "unknown". */
const char *rt_diag_sched_exclude_reason_name(enum rt_diag_sched_exclude_reason r);

/*
 * Render to `out` as a stable text block:
 *
 *   deadline-diag-sched: driver=<id> generation=<g> period_ns=<p> deadline_ns=<d>
 *     nodes: <n>
 *       node id=<i> tid=<t>
 *       ...
 *     edges: <n>
 *       edge <src>-><dst>
 *       ...
 *     excluded: <n>
 *       excluded <src>-><dst> reason=<reason>
 *       ...
 */
void rt_diag_sched_snapshot_render_text(const struct rt_diag_sched_snapshot *s,
					FILE *out);

/*
 * Fusion-decision slice.
 *
 * The Sarkar 1989 profitability criterion drives a per-component
 * fusion decision in the core (see src/pipewire/fusion-cost.h). The
 * decision values are FUSE (members co-located on one data loop),
 * LINEAR_ONLY (chain fallback for single-input / single-output
 * pipelines per Gerasoulis & Yang 1993), and SPLIT (no fusion).
 *
 * For Phase 0 observability, the dump groups followers by the
 * applied decision and records each group's component leader, the
 * applied verdict, the rejection reason for non-FUSE verdicts (today
 * the only reason is "below_threshold" -- the Sarkar inequality did
 * not hold for the component; Phase 3's soundness validator will
 * extend this enum with structural rejection reasons such as
 * non_convex, internal_milestone, blocking_risk, ...), and the list
 * of member node ids in the group.
 */
enum rt_diag_fusion_verdict {
	RT_DIAG_FUSION_FUSE        = 0,
	RT_DIAG_FUSION_LINEAR_ONLY = 1,
	RT_DIAG_FUSION_SPLIT       = 2,
};

enum rt_diag_fusion_reject_reason {
	RT_DIAG_FUSION_REJ_NONE            = 0,
	RT_DIAG_FUSION_REJ_BELOW_THRESHOLD = 1,
};

struct rt_diag_fusion_group {
	uint32_t leader_id;
	enum rt_diag_fusion_verdict verdict;
	enum rt_diag_fusion_reject_reason reject_reason;
	uint32_t *members;
	uint32_t  n_members;
	uint32_t  cap_members;
};

struct rt_diag_fusion_snapshot {
	uint32_t driver_id;
	uint64_t generation;

	struct rt_diag_fusion_group *groups;
	uint32_t n_groups;
	uint32_t cap_groups;
};

void rt_diag_fusion_snapshot_init(struct rt_diag_fusion_snapshot *s);
void rt_diag_fusion_snapshot_fini(struct rt_diag_fusion_snapshot *s);
void rt_diag_fusion_snapshot_reset(struct rt_diag_fusion_snapshot *s);

/* Begin a new group with the supplied leader / verdict / reason.
 * Returns the group's index in s->groups on success, or -EINVAL /
 * -ENOMEM. The reason is required to be NONE when verdict is FUSE
 * and required to be non-NONE otherwise; the call returns -EINVAL
 * on a mismatch. */
int rt_diag_fusion_snapshot_begin_group(struct rt_diag_fusion_snapshot *s,
					uint32_t leader_id,
					enum rt_diag_fusion_verdict verdict,
					enum rt_diag_fusion_reject_reason reason);

/* Append `member_id` to the group at `group_idx`. Returns 0 on
 * success, -EINVAL on out-of-range index, -ENOMEM on allocation
 * failure. */
int rt_diag_fusion_snapshot_add_member(struct rt_diag_fusion_snapshot *s,
				       uint32_t group_idx,
				       uint32_t member_id);

const char *rt_diag_fusion_verdict_name(enum rt_diag_fusion_verdict v);
const char *rt_diag_fusion_reject_reason_name(enum rt_diag_fusion_reject_reason r);

/*
 * Render to `out` as a stable text block:
 *
 *   deadline-diag-fusion: driver=<id> generation=<g>
 *     groups: <n>
 *       group leader=<id> verdict=<verdict> reason=<reason> members=<id,id,...>
 *       ...
 */
void rt_diag_fusion_snapshot_render_text(const struct rt_diag_fusion_snapshot *s,
					 FILE *out);

/*
 * Scheduling-parameters slice.
 *
 * One entry per schedulable follower carrying the kernel-facing
 * SCHED_DEADLINE tuple (runtime, deadline, period, cpu) plus a
 * "cumulative" deadline kept distinct in the API even though, at
 * Phase 0, it equals the local deadline. Phase 1's deadline-semantics
 * refactor populates the two fields independently, at which point
 * the JSON snapshot already carries the distinction without a schema
 * change.
 *
 * The `applied` field reflects whether sched_setattr has issued at
 * least once for the follower; an entry with applied=false means the
 * tuple has been computed by the analysis layer but the syscall has
 * not yet run (typically a brand-new follower).
 */
/*
 * Per-node runtime-budget provenance.
 *
 * A SCHED_DEADLINE `runtime` value is only as strong a guarantee
 * as the estimator that produced it. A deterministic WCET from
 * static analysis or a manufacturer's datasheet supports a hard
 * timing claim; a measurement-based pWCET with stated exceedance
 * probability supports a probabilistic claim; an empirical p-
 * quantile from a streaming sketch supports a soft-real-time
 * claim only; a bootstrap fallback (used while a new node has not
 * yet accumulated enough samples for a real estimate) supports
 * neither. Calling any of those a "WCET" without qualification --
 * which the prototype's t-digest p95 currently does -- is the
 * provenance bug Cucu-Grosjean et al. 2012 warns about: a raw
 * percentile is not a WCET and pretending it is silently
 * weakens every downstream feasibility claim.
 *
 * The kinds below mirror Cucu-Grosjean 2012 (pWCET), Dunning &
 * Ertl 2019 (empirical quantile via t-digest), and the
 * deterministic / manual-override / bootstrap cases that
 * complete the taxonomy. Tokens are lower_snake_case.
 */
enum rt_diag_budget_kind {
	RT_DIAG_BUDGET_DETERMINISTIC_WCET = 0,
	RT_DIAG_BUDGET_PWCET              = 1,
	RT_DIAG_BUDGET_EMPIRICAL_QUANTILE = 2,
	RT_DIAG_BUDGET_BOOTSTRAP_FALLBACK = 3,
	RT_DIAG_BUDGET_MANUAL_OVERRIDE    = 4,
};

const char *rt_diag_budget_kind_name(enum rt_diag_budget_kind k);

/*
 * MBPTA estimator state mirror. The enum is intentionally a
 * caller-visible copy of mbpta.h's state machine so the diag
 * layer can render the tokens without depending on the
 * estimator TU. The accompanying pwcet_ns is the most recent
 * tail-extrapolation result; zero unless state == PWCET_VALID.
 * Tokens are the same lower_snake_case strings the estimator's
 * mbpta_state_name returns.
 */
enum rt_diag_mbpta_state {
	RT_DIAG_MBPTA_INSUFFICIENT_DATA   = 0,
	RT_DIAG_MBPTA_IID_PENDING         = 1,
	RT_DIAG_MBPTA_NON_GUMBEL          = 2,
	RT_DIAG_MBPTA_PENDING_CONVERGENCE = 3,
	RT_DIAG_MBPTA_PWCET_VALID         = 4,
	RT_DIAG_MBPTA_DRIFT               = 5,
};

const char *rt_diag_mbpta_state_name(enum rt_diag_mbpta_state s);

struct rt_diag_param_node {
	uint32_t id;
	pid_t    tid;
	uint64_t runtime_budget_ns;
	uint64_t local_deadline_ns;
	uint64_t cumulative_deadline_ns;
	uint64_t period_ns;
	uint32_t cpu;
	bool     applied;
	enum rt_diag_budget_kind budget_kind;
	uint64_t budget_sample_count;
	enum rt_diag_mbpta_state mbpta_state;
	uint64_t mbpta_pwcet_ns;
	uint32_t mbpta_block_count;
	/* MBPTA fit diagnostics. The first three are the most
	 * recent statistical-test outcomes (Kolmogorov-Smirnov
	 * statistic on the two-sample identical-distribution test;
	 * Wald-Wolfowitz Z on the independence test;
	 * continuous-rank-probability-score between successive
	 * Gumbel fits); the last two are the per-fit Gumbel
	 * location and scale parameters
	 * (Cucu-Grosjean 2012 §II-A). The values are read
	 * verbatim from mbpta_ks_stat / mbpta_runs_z / mbpta_crps /
	 * mbpta_mu / mbpta_sigma. All zero until the first re-eval
	 * round runs. */
	double   mbpta_ks_stat;
	double   mbpta_runs_z;
	double   mbpta_crps;
	double   mbpta_mu;
	double   mbpta_sigma;
	uint32_t mbpta_convergence_streak;
	uint32_t mbpta_iid_reject_streak;

	/* Effective per-node exceedance probability used in the
	 * Gumbel inverse-CDF evaluation, after the
	 * working-precision floor (Cucu-Grosjean 2012 §III-D step 6,
	 * 1e-16) is applied. mbpta_eps_node_capped is true iff the
	 * floor clipped the configured value -- surface both so the
	 * runtime probabilistic guarantee in effect is visible. */
	double   mbpta_effective_eps_node;
	bool     mbpta_eps_node_capped;
};

struct rt_diag_params_snapshot {
	uint32_t driver_id;
	uint64_t generation;

	struct rt_diag_param_node *nodes;
	uint32_t n_nodes;
	uint32_t cap_nodes;
};

void rt_diag_params_snapshot_init(struct rt_diag_params_snapshot *s);
void rt_diag_params_snapshot_fini(struct rt_diag_params_snapshot *s);
void rt_diag_params_snapshot_reset(struct rt_diag_params_snapshot *s);

int rt_diag_params_snapshot_add_node(struct rt_diag_params_snapshot *s,
				     const struct rt_diag_param_node *node);

/*
 * Render to `out` as a stable text block:
 *
 *   deadline-diag-params: driver=<id> generation=<g> mode=<mode>
 *     nodes: <n>
 *       node id=<i> tid=<t> runtime=<r>ns local_deadline=<ld>ns
 *           cumulative_deadline=<cd>ns period=<p>ns cpu=<c> applied=<bool>
 *       ...
 */
void rt_diag_params_snapshot_render_text(const struct rt_diag_params_snapshot *s,
					 const char *mode,
					 FILE *out);

/*
 * Peer-dispatch slice.
 *
 * The contracted-DAG analysis assumes that, once a macro-node's
 * leader is woken, every internal edge inside the macro-node
 * dispatches without leaving the data-loop thread: the in-tree
 * trigger_target_v1 same-loop fast path (private.h) skips the
 * eventfd round-trip when the producer and consumer share a
 * spa_system and calls process_node on the same stack. That fast
 * path is the runtime realisation of Sarkar 1989 §5.3's macro-
 * actor: a single non-self-suspending compound execution.
 *
 * To verify the assumption holds in practice, the diag layer
 * carries a per-driver count of how many peer-edges have armed
 * inline dispatch (`inline_armed`) versus how many remained on
 * the eventfd path (`eventfd_path`). Module-deadline populates
 * these by walking each follower's peer list at snapshot time;
 * the JSON snapshot surfaces them so an operator can confirm,
 * post-fusion, that the internal edges of every accepted macro-
 * node use the fast path.
 */
struct rt_diag_peer_dispatch {
	uint32_t driver_id;
	uint64_t generation;
	uint32_t inline_armed;
	uint32_t eventfd_path;
};

void rt_diag_peer_dispatch_init(struct rt_diag_peer_dispatch *s);
void rt_diag_peer_dispatch_reset(struct rt_diag_peer_dispatch *s);
void rt_diag_peer_dispatch_render_text(const struct rt_diag_peer_dispatch *s,
				       FILE *out);

/*
 * Combined JSON document.
 *
 * Carries the four diagnostic slices plus the driver-level metadata
 * (mode, feasibility) in one stable structure. The mode and
 * feasibility strings are caller-provided so the rendered text does
 * not pretend to a hard/soft classification that the implementation
 * cannot yet justify; at Phase 0 the caller passes mode="prototype"
 * and feasibility_method="none".
 *
 * Any sub-snapshot pointer may be NULL; the corresponding section is
 * still emitted as an empty object so the JSON schema is stable
 * across configurations.
 */
struct rt_diag_combined {
	uint32_t driver_id;
	uint64_t generation;
	uint64_t period_ns;
	uint64_t deadline_ns;
	const char *mode;                 /* "prototype" today */
	const char *feasibility_method;   /* "none" today */
	const char *feasibility_status;   /* "n/a" today */

	const struct rt_diag_raw_snapshot    *raw;
	const struct rt_diag_sched_snapshot  *sched;
	const struct rt_diag_fusion_snapshot *fusion;
	const struct rt_diag_params_snapshot *params;
	const struct rt_diag_peer_dispatch   *peer_dispatch;
};

void rt_diag_render_json(const struct rt_diag_combined *c, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_DIAG_H */
