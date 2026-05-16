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

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_DIAG_H */
