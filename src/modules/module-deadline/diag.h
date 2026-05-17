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
 * Pure classifier for one in-period edge. Returns the single
 * exclusion reason that drove the edge out of the scheduling DAG
 * (or RT_DIAG_SCHED_EXC_NONE when the edge belongs to the included
 * set).
 *
 * The decision order matches the runtime path -- feedback edges
 * dominate every other reason because they intentionally use
 * previous-period data; async edges drop next because they break
 * the in-period chain; exported edges drop because the daemon
 * cannot place SCHED_DEADLINE on the remote thread; and finally
 * any edge whose endpoint is missing or not part of the schedulable
 * follower set is classified UNSUPPORTED. The decision is total:
 * exactly one reason is returned per call.
 *
 * Pure data: no PipeWire runtime symbols; safe to call from unit
 * tests with arbitrary flag combinations. The runtime callers in
 * module-deadline.c project the live pw_impl_link / pw_impl_node
 * flags into these booleans.
 */
enum rt_diag_sched_exclude_reason rt_diag_sched_classify_edge(
		bool feedback,
		bool src_async, bool dst_async,
		bool src_exported, bool dst_exported,
		bool src_in_set, bool dst_in_set);

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
 * The dump groups followers by the applied decision and records
 * each group's component leader, the applied verdict, the
 * rejection reason for non-FUSE verdicts (today the only reason
 * is "below_threshold" -- the Sarkar inequality did not hold for
 * the component; the soundness validator below extends this enum
 * with structural rejection reasons such as non_convex,
 * internal_milestone, blocking_risk, ...), and the list of member
 * node ids in the group.
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
 * "cumulative" deadline kept distinct in the API. The two
 * fields are populated independently by the deadline-semantics
 * refactor that splits cumulative graph milestones from the
 * local kernel-relative deadlines passed to sched_setattr; the
 * JSON snapshot already carries the distinction without a
 * schema change.
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
	/*
	 * Per-node operator override (no per-node property is wired
	 * to this branch yet; reserved for a future
	 * deadline.manual_override.<id>.runtime_ns key). Always wins
	 * when set.
	 */
	RT_DIAG_BUDGET_MANUAL_OVERRIDE    = 0,
	/*
	 * Static deterministic WCET supplied by the plugin (no
	 * plugin attribute is wired yet; reserved for a future
	 * PW_KEY_NODE_WCET_NS or equivalent). Hard real-time
	 * provenance.
	 */
	RT_DIAG_BUDGET_DETERMINISTIC_WCET = 1,
	/*
	 * Adaptive online upper-runtime budget calibrated to a target
	 * overrun frequency via one-sided conformal prediction with an
	 * EWMA location/scale base predictor (Romano, Patterson &
	 * Candes 2019; Gibbs & Candes 2021). The published value is a
	 * soft / weakly-hard bound (Bernat, Burns & Llamosi 2001), not
	 * a deterministic WCET: strict hard-realtime operation still
	 * requires manual / static / hybrid WCETs.
	 */
	RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL = 2,
};

const char *rt_diag_budget_kind_name(enum rt_diag_budget_kind k);

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

	/* Macro-node release-barrier count: the number of distinct
	 * in-period predecessors the contracted scheduling-DAG node
	 * for this follower must observe completing before its data
	 * loop may begin its job. Sources of the contracted DAG
	 * report 0; a chain mid-section that has been fused with its
	 * predecessor also reports 0 because the contracting collapsed
	 * the internal edge. The runtime hook (when wired) arms the
	 * per-cycle pending counter from this value at cycle start
	 * and decrements it on each external-predecessor completion;
	 * today's snapshot exposes it for diagnostics so an operator
	 * can confirm the contracting layer's predecessor accounting. */
	uint32_t required_external_inputs;

	/* Runtime-detected voluntary context switches inside
	 * process(). Surfaced from the blocking-observation hook
	 * (reconcile_state_report_blocking_observation): a non-zero
	 * value means the follower's thread voluntarily yielded
	 * inside an activation window, which violates the
	 * blocking-closure predicate's static accept. Zero is the
	 * expected value on a healthy schedule. The runtime samples
	 * /proc/<tid>/status's voluntary_ctxt_switches around the
	 * process() call; growth lands in this counter and triggers
	 * a soft-degraded demotion via the typed
	 * RECONCILE_SOFT_REASON_PROCESS_BLOCKED_INSIDE_RT reason. */
	uint64_t voluntary_ctxt_switches_in_process;

	/*
	 * Adaptive-conformal estimator diagnostics. The estimator's
	 * state surfaces as the stable lowercase token in
	 * rt_conformal_state_name(); samples_seen / samples_used /
	 * the overrun counters / alpha_eff / mu_ns / scale_ns /
	 * score_quantile mirror the per-instance state. Zero values
	 * are the expected steady-state when the conformal estimator
	 * has not yet been instantiated for this follower (e.g. a
	 * driver node that does not run process()).
	 */
	uint8_t  conformal_state; /* enum rt_conformal_state */
	uint64_t conformal_samples_seen;
	uint64_t conformal_samples_used;
	uint64_t conformal_overruns_seen;
	uint64_t conformal_recent_overruns;
	uint64_t conformal_max_overrun_burst;
	uint64_t conformal_current_overrun_burst;
	double   conformal_alpha_target;
	double   conformal_alpha_eff;
	uint32_t conformal_window;
	double   conformal_ewma_location_ns;
	double   conformal_ewma_scale_ns;
	double   conformal_score_quantile;
	uint64_t conformal_guard_ns;
	double   conformal_guard_percent;
	uint64_t conformal_runtime_floor_ns;
	uint64_t conformal_last_runtime_ns;
	double   conformal_last_prediction_ns;
	double   conformal_last_score;
	uint64_t conformal_last_budget_ns;
	uint8_t  conformal_last_invalidation_reason;

	/*
	 * Soft-mode risk-aware redistribution surface. The
	 * `budget_clipped` flag is set by the soft heuristic when this
	 * follower's wcet exceeded its assigned local deadline after
	 * the cumulative-deadline redistribution; the apply path then
	 * caps the kernel runtime to fit. The graph-level
	 * `risk_objective_value` aggregate is mirrored on every
	 * follower for parser convenience -- it is the same number for
	 * every follower in a given snapshot.
	 */
	bool     budget_clipped;
	double   risk_objective_value;
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
 * cannot yet justify; when the caller has no admission verdict in
 * hand it passes mode="prototype" and feasibility_method="none".
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
