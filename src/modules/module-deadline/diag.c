/*
 * diag.c
 *
 * Implementation of the diagnostic snapshot types and renderer
 * declared in diag.h. The translation unit is intentionally small
 * and self-contained: no PipeWire runtime symbols, no logging
 * topics, no syscalls. The module side feeds plain tuples in and
 * gets a deterministic text rendering out.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <errno.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"

/* JSON requires '.' as the decimal separator (RFC 8259 §6); the
 * text snapshot is consumed by tooling that expects the same.
 * snprintf honours LC_NUMERIC, so a daemon launched under e.g.
 * it_IT would otherwise emit "0,041" and break every downstream
 * parser. Format every floating-point value through this helper:
 * it rounds with the active locale and then rewrites the decimal
 * marker, which avoids the (non-portable, non-thread-friendly)
 * uselocale dance. */
static void diag_fprintf_double(FILE *out, int precision, double v)
{
	char buf[64];
	int n = snprintf(buf, sizeof(buf), "%.*f", precision, v);
	if (n < 0 || (size_t)n >= sizeof(buf)) {
		fputs("0", out);
		return;
	}
	for (char *p = buf; *p != '\0'; p++) {
		if (*p == ',')
			*p = '.';
	}
	fputs(buf, out);
}

/* Initial allocation when the first node/edge lands. Both arrays
 * grow geometrically (doubling) so amortised cost is O(1) per add.
 * Sized to comfortably fit the small (< 16-node) graphs that
 * dominate the live-test corpus without an early realloc. */
#define RT_DIAG_INITIAL_NODES 8u
#define RT_DIAG_INITIAL_EDGES 16u

void rt_diag_raw_snapshot_init(struct rt_diag_raw_snapshot *s)
{
	if (s == NULL)
		return;
	memset(s, 0, sizeof(*s));
}

void rt_diag_raw_snapshot_fini(struct rt_diag_raw_snapshot *s)
{
	if (s == NULL)
		return;
	free(s->nodes);
	free(s->edges);
	memset(s, 0, sizeof(*s));
}

void rt_diag_raw_snapshot_reset(struct rt_diag_raw_snapshot *s)
{
	if (s == NULL)
		return;
	s->n_nodes = 0;
	s->n_edges = 0;
	s->driver_id = 0;
	s->generation = 0;
	s->period_ns = 0;
	s->deadline_ns = 0;
}

/* Grow `*arr` (current capacity `*cap`) to at least `*cap + 1`
 * slots of `elem_sz` bytes, using geometric doubling from
 * `initial_cap`. Returns 0 on success, -ENOMEM on realloc failure
 * (leaving the caller's array untouched). */
static int grow_array(void **arr, uint32_t *cap, uint32_t elem_sz,
		      uint32_t initial_cap)
{
	uint32_t new_cap;
	void *resized;

	new_cap = *cap ? *cap * 2u : initial_cap;
	resized = realloc(*arr, (size_t)new_cap * (size_t)elem_sz);
	if (resized == NULL) {
		errno = ENOMEM;
		return -ENOMEM;
	}
	*arr = resized;
	*cap = new_cap;
	return 0;
}

int rt_diag_raw_snapshot_add_node(struct rt_diag_raw_snapshot *s,
				  const struct rt_diag_raw_node *node)
{
	struct rt_diag_raw_node *slot;

	if (s == NULL || node == NULL)
		return -EINVAL;

	if (s->n_nodes == s->cap_nodes) {
		int r = grow_array((void **)&s->nodes, &s->cap_nodes,
				   (uint32_t)sizeof(*s->nodes),
				   RT_DIAG_INITIAL_NODES);
		if (r < 0)
			return r;
	}

	slot = &s->nodes[s->n_nodes++];
	slot->id = node->id;
	slot->driver_id = node->driver_id;
	slot->tid = node->tid;
	slot->flags = node->flags;
	/* Defensive copy: caller may pass an unterminated buffer if
	 * the source string was exactly the maximum length, so always
	 * cap with an explicit NUL. */
	memset(slot->name, 0, sizeof(slot->name));
	if (node->name[0] != '\0') {
		size_t i;
		for (i = 0; i + 1 < sizeof(slot->name); i++) {
			char c = node->name[i];
			if (c == '\0')
				break;
			slot->name[i] = c;
		}
		slot->name[i] = '\0';
	}
	return 0;
}

int rt_diag_raw_snapshot_add_edge(struct rt_diag_raw_snapshot *s,
				  const struct rt_diag_raw_edge *edge)
{
	struct rt_diag_raw_edge *slot;

	if (s == NULL || edge == NULL)
		return -EINVAL;

	if (s->n_edges == s->cap_edges) {
		int r = grow_array((void **)&s->edges, &s->cap_edges,
				   (uint32_t)sizeof(*s->edges),
				   RT_DIAG_INITIAL_EDGES);
		if (r < 0)
			return r;
	}

	slot = &s->edges[s->n_edges++];
	slot->src = edge->src;
	slot->dst = edge->dst;
	slot->flags = edge->flags;
	return 0;
}

struct flag_name {
	uint32_t bit;
	const char *name;
};

static const struct flag_name raw_node_flags[] = {
	{ RT_DIAG_RAW_NODE_DRIVER,       "driver" },
	{ RT_DIAG_RAW_NODE_DATA_LOOP,    "data_loop" },
	{ RT_DIAG_RAW_NODE_MAIN_LOOP,    "main_loop" },
	{ RT_DIAG_RAW_NODE_REMOTE,       "remote" },
	{ RT_DIAG_RAW_NODE_EXPORTED,     "exported" },
	{ RT_DIAG_RAW_NODE_ASYNC,        "async" },
	{ RT_DIAG_RAW_NODE_DYNAMIC_LOOP, "dynamic_loop" },
};

static const struct flag_name raw_edge_flags[] = {
	{ RT_DIAG_RAW_EDGE_FEEDBACK, "feedback" },
	{ RT_DIAG_RAW_EDGE_ASYNC,    "async" },
};

static void render_flags(FILE *out, uint32_t flags,
			 const struct flag_name *table, size_t n)
{
	bool first = true;
	size_t i;

	if (flags == 0u) {
		fputc('-', out);
		return;
	}
	for (i = 0; i < n; i++) {
		if ((flags & table[i].bit) == 0u)
			continue;
		if (!first)
			fputc(',', out);
		fputs(table[i].name, out);
		first = false;
	}
}

void rt_diag_raw_snapshot_render_text(const struct rt_diag_raw_snapshot *s,
				      FILE *out)
{
	uint32_t i;

	if (s == NULL || out == NULL)
		return;

	fprintf(out,
		"deadline-diag-raw: driver=%u generation=%llu period_ns=%llu deadline_ns=%llu\n",
		s->driver_id,
		(unsigned long long)s->generation,
		(unsigned long long)s->period_ns,
		(unsigned long long)s->deadline_ns);

	fprintf(out, "  nodes: %u\n", s->n_nodes);
	for (i = 0; i < s->n_nodes; i++) {
		const struct rt_diag_raw_node *n = &s->nodes[i];
		fprintf(out,
			"    node id=%u driver_id=%u tid=%d flags=",
			n->id, n->driver_id, (int)n->tid);
		render_flags(out, n->flags, raw_node_flags,
			     sizeof(raw_node_flags) / sizeof(raw_node_flags[0]));
		fprintf(out, " name=%s\n", n->name[0] ? n->name : "-");
	}

	fprintf(out, "  edges: %u\n", s->n_edges);
	for (i = 0; i < s->n_edges; i++) {
		const struct rt_diag_raw_edge *e = &s->edges[i];
		fprintf(out, "    edge %u->%u flags=", e->src, e->dst);
		render_flags(out, e->flags, raw_edge_flags,
			     sizeof(raw_edge_flags) / sizeof(raw_edge_flags[0]));
		fputc('\n', out);
	}
}

/* --- scheduling-DAG slice --- */

void rt_diag_sched_snapshot_init(struct rt_diag_sched_snapshot *s)
{
	if (s == NULL)
		return;
	memset(s, 0, sizeof(*s));
}

void rt_diag_sched_snapshot_fini(struct rt_diag_sched_snapshot *s)
{
	if (s == NULL)
		return;
	free(s->nodes);
	free(s->edges);
	free(s->excluded_edges);
	memset(s, 0, sizeof(*s));
}

void rt_diag_sched_snapshot_reset(struct rt_diag_sched_snapshot *s)
{
	if (s == NULL)
		return;
	s->n_nodes = 0;
	s->n_edges = 0;
	s->n_excluded = 0;
	s->driver_id = 0;
	s->generation = 0;
	s->period_ns = 0;
	s->deadline_ns = 0;
}

int rt_diag_sched_snapshot_add_node(struct rt_diag_sched_snapshot *s,
				    const struct rt_diag_sched_node *node)
{
	if (s == NULL || node == NULL)
		return -EINVAL;
	if (s->n_nodes == s->cap_nodes) {
		int r = grow_array((void **)&s->nodes, &s->cap_nodes,
				   (uint32_t)sizeof(*s->nodes),
				   RT_DIAG_INITIAL_NODES);
		if (r < 0)
			return r;
	}
	s->nodes[s->n_nodes++] = *node;
	return 0;
}

int rt_diag_sched_snapshot_add_edge(struct rt_diag_sched_snapshot *s,
				    const struct rt_diag_sched_edge *edge)
{
	if (s == NULL || edge == NULL)
		return -EINVAL;
	if (s->n_edges == s->cap_edges) {
		int r = grow_array((void **)&s->edges, &s->cap_edges,
				   (uint32_t)sizeof(*s->edges),
				   RT_DIAG_INITIAL_EDGES);
		if (r < 0)
			return r;
	}
	s->edges[s->n_edges++] = *edge;
	return 0;
}

int rt_diag_sched_snapshot_add_excluded(struct rt_diag_sched_snapshot *s,
					const struct rt_diag_sched_excluded_edge *edge)
{
	if (s == NULL || edge == NULL)
		return -EINVAL;
	if (edge->reason == RT_DIAG_SCHED_EXC_NONE)
		return -EINVAL;
	if (s->n_excluded == s->cap_excluded) {
		int r = grow_array((void **)&s->excluded_edges,
				   &s->cap_excluded,
				   (uint32_t)sizeof(*s->excluded_edges),
				   RT_DIAG_INITIAL_EDGES);
		if (r < 0)
			return r;
	}
	s->excluded_edges[s->n_excluded++] = *edge;
	return 0;
}

const char *rt_diag_sched_exclude_reason_name(enum rt_diag_sched_exclude_reason r)
{
	switch (r) {
	case RT_DIAG_SCHED_EXC_NONE:         return "none";
	case RT_DIAG_SCHED_EXC_FEEDBACK:     return "feedback";
	case RT_DIAG_SCHED_EXC_ASYNC:        return "async";
	case RT_DIAG_SCHED_EXC_CROSS_DRIVER: return "cross_driver";
	case RT_DIAG_SCHED_EXC_EXPORTED:     return "exported";
	case RT_DIAG_SCHED_EXC_NON_RT:       return "non_rt";
	case RT_DIAG_SCHED_EXC_UNSUPPORTED:  return "unsupported";
	}
	return "unknown";
}

enum rt_diag_sched_exclude_reason rt_diag_sched_classify_edge(
		bool feedback,
		bool src_async, bool dst_async,
		bool src_exported, bool dst_exported,
		bool src_in_set, bool dst_in_set)
{
	if (feedback)
		return RT_DIAG_SCHED_EXC_FEEDBACK;
	if (src_async || dst_async)
		return RT_DIAG_SCHED_EXC_ASYNC;
	if (src_exported || dst_exported)
		return RT_DIAG_SCHED_EXC_EXPORTED;
	if (!src_in_set || !dst_in_set)
		return RT_DIAG_SCHED_EXC_UNSUPPORTED;
	return RT_DIAG_SCHED_EXC_NONE;
}

void rt_diag_sched_snapshot_render_text(const struct rt_diag_sched_snapshot *s,
					FILE *out)
{
	uint32_t i;

	if (s == NULL || out == NULL)
		return;

	fprintf(out,
		"deadline-diag-sched: driver=%u generation=%llu period_ns=%llu deadline_ns=%llu\n",
		s->driver_id,
		(unsigned long long)s->generation,
		(unsigned long long)s->period_ns,
		(unsigned long long)s->deadline_ns);

	fprintf(out, "  nodes: %u\n", s->n_nodes);
	for (i = 0; i < s->n_nodes; i++) {
		const struct rt_diag_sched_node *n = &s->nodes[i];
		fprintf(out, "    node id=%u tid=%d\n",
			n->id, (int)n->tid);
	}

	fprintf(out, "  edges: %u\n", s->n_edges);
	for (i = 0; i < s->n_edges; i++) {
		const struct rt_diag_sched_edge *e = &s->edges[i];
		fprintf(out, "    edge %u->%u\n", e->src, e->dst);
	}

	fprintf(out, "  excluded: %u\n", s->n_excluded);
	for (i = 0; i < s->n_excluded; i++) {
		const struct rt_diag_sched_excluded_edge *e = &s->excluded_edges[i];
		fprintf(out, "    excluded %u->%u reason=%s\n",
			e->src, e->dst,
			rt_diag_sched_exclude_reason_name(e->reason));
	}
}

/* --- fusion-decision slice --- */

#define RT_DIAG_INITIAL_GROUPS  8u
#define RT_DIAG_INITIAL_MEMBERS 4u

void rt_diag_fusion_snapshot_init(struct rt_diag_fusion_snapshot *s)
{
	if (s == NULL)
		return;
	memset(s, 0, sizeof(*s));
}

void rt_diag_fusion_snapshot_fini(struct rt_diag_fusion_snapshot *s)
{
	uint32_t i;
	if (s == NULL)
		return;
	for (i = 0; i < s->n_groups; i++)
		free(s->groups[i].members);
	free(s->groups);
	memset(s, 0, sizeof(*s));
}

void rt_diag_fusion_snapshot_reset(struct rt_diag_fusion_snapshot *s)
{
	uint32_t i;
	if (s == NULL)
		return;
	for (i = 0; i < s->n_groups; i++)
		s->groups[i].n_members = 0;
	s->n_groups = 0;
	s->driver_id = 0;
	s->generation = 0;
}

int rt_diag_fusion_snapshot_begin_group(struct rt_diag_fusion_snapshot *s,
					uint32_t leader_id,
					enum rt_diag_fusion_verdict verdict,
					enum rt_diag_fusion_reject_reason reason)
{
	struct rt_diag_fusion_group *g;

	if (s == NULL)
		return -EINVAL;
	if (verdict == RT_DIAG_FUSION_FUSE && reason != RT_DIAG_FUSION_REJ_NONE)
		return -EINVAL;
	if (verdict != RT_DIAG_FUSION_FUSE && reason == RT_DIAG_FUSION_REJ_NONE)
		return -EINVAL;

	if (s->n_groups == s->cap_groups) {
		int r = grow_array((void **)&s->groups, &s->cap_groups,
				   (uint32_t)sizeof(*s->groups),
				   RT_DIAG_INITIAL_GROUPS);
		if (r < 0)
			return r;
	}

	g = &s->groups[s->n_groups];
	g->leader_id = leader_id;
	g->verdict = verdict;
	g->reject_reason = reason;
	g->members = NULL;
	g->n_members = 0;
	g->cap_members = 0;
	return (int)s->n_groups++;
}

int rt_diag_fusion_snapshot_add_member(struct rt_diag_fusion_snapshot *s,
				       uint32_t group_idx,
				       uint32_t member_id)
{
	struct rt_diag_fusion_group *g;

	if (s == NULL || group_idx >= s->n_groups)
		return -EINVAL;
	g = &s->groups[group_idx];
	if (g->n_members == g->cap_members) {
		int r = grow_array((void **)&g->members, &g->cap_members,
				   (uint32_t)sizeof(*g->members),
				   RT_DIAG_INITIAL_MEMBERS);
		if (r < 0)
			return r;
	}
	g->members[g->n_members++] = member_id;
	return 0;
}

const char *rt_diag_fusion_verdict_name(enum rt_diag_fusion_verdict v)
{
	switch (v) {
	case RT_DIAG_FUSION_FUSE:        return "fuse";
	case RT_DIAG_FUSION_LINEAR_ONLY: return "linear_only";
	case RT_DIAG_FUSION_SPLIT:       return "split";
	}
	return "unknown";
}

const char *rt_diag_fusion_reject_reason_name(enum rt_diag_fusion_reject_reason r)
{
	switch (r) {
	case RT_DIAG_FUSION_REJ_NONE:            return "none";
	case RT_DIAG_FUSION_REJ_BELOW_THRESHOLD: return "below_threshold";
	}
	return "unknown";
}

void rt_diag_fusion_snapshot_render_text(const struct rt_diag_fusion_snapshot *s,
					 FILE *out)
{
	uint32_t i, j;

	if (s == NULL || out == NULL)
		return;

	fprintf(out, "deadline-diag-fusion: driver=%u generation=%llu\n",
		s->driver_id, (unsigned long long)s->generation);
	fprintf(out, "  groups: %u\n", s->n_groups);
	for (i = 0; i < s->n_groups; i++) {
		const struct rt_diag_fusion_group *g = &s->groups[i];
		fprintf(out,
			"    group leader=%u verdict=%s reason=%s members=",
			g->leader_id,
			rt_diag_fusion_verdict_name(g->verdict),
			rt_diag_fusion_reject_reason_name(g->reject_reason));
		if (g->n_members == 0) {
			fputc('-', out);
		} else {
			for (j = 0; j < g->n_members; j++) {
				if (j > 0)
					fputc(',', out);
				fprintf(out, "%u", g->members[j]);
			}
		}
		fputc('\n', out);
	}
}

/* --- scheduling-parameters slice --- */

void rt_diag_params_snapshot_init(struct rt_diag_params_snapshot *s)
{
	if (s == NULL)
		return;
	memset(s, 0, sizeof(*s));
}

void rt_diag_params_snapshot_fini(struct rt_diag_params_snapshot *s)
{
	if (s == NULL)
		return;
	free(s->nodes);
	memset(s, 0, sizeof(*s));
}

void rt_diag_params_snapshot_reset(struct rt_diag_params_snapshot *s)
{
	if (s == NULL)
		return;
	s->n_nodes = 0;
	s->driver_id = 0;
	s->generation = 0;
}

int rt_diag_params_snapshot_add_node(struct rt_diag_params_snapshot *s,
				     const struct rt_diag_param_node *node)
{
	if (s == NULL || node == NULL)
		return -EINVAL;
	if (s->n_nodes == s->cap_nodes) {
		int r = grow_array((void **)&s->nodes, &s->cap_nodes,
				   (uint32_t)sizeof(*s->nodes),
				   RT_DIAG_INITIAL_NODES);
		if (r < 0)
			return r;
	}
	s->nodes[s->n_nodes++] = *node;
	return 0;
}

void rt_diag_params_snapshot_render_text(const struct rt_diag_params_snapshot *s,
					 const char *mode, FILE *out)
{
	uint32_t i;

	if (s == NULL || out == NULL)
		return;
	if (mode == NULL)
		mode = "prototype";

	fprintf(out,
		"deadline-diag-params: driver=%u generation=%llu mode=%s\n",
		s->driver_id, (unsigned long long)s->generation, mode);
	fprintf(out, "  nodes: %u\n", s->n_nodes);
	for (i = 0; i < s->n_nodes; i++) {
		const struct rt_diag_param_node *n = &s->nodes[i];
		fprintf(out,
			"    node id=%u tid=%d runtime=%lluns local_deadline=%lluns"
			" cumulative_deadline=%lluns period=%lluns cpu=%u applied=%s"
			" budget_kind=%s budget_samples=%llu"
			" mbpta_state=%s mbpta_pwcet=%lluns mbpta_blocks=%u"
			" mbpta_mu=",
			n->id, (int)n->tid,
			(unsigned long long)n->runtime_budget_ns,
			(unsigned long long)n->local_deadline_ns,
			(unsigned long long)n->cumulative_deadline_ns,
			(unsigned long long)n->period_ns,
			n->cpu,
			n->applied ? "true" : "false",
			rt_diag_budget_kind_name(n->budget_kind),
			(unsigned long long)n->budget_sample_count,
			rt_diag_mbpta_state_name(n->mbpta_state),
			(unsigned long long)n->mbpta_pwcet_ns,
			n->mbpta_block_count);
		diag_fprintf_double(out, 0, n->mbpta_mu);
		fputs(" mbpta_sigma=", out);
		diag_fprintf_double(out, 0, n->mbpta_sigma);
		fputs(" mbpta_ks=", out);
		diag_fprintf_double(out, 6, n->mbpta_ks_stat);
		fputs(" mbpta_ks_p=", out);
		diag_fprintf_double(out, 6, n->mbpta_ks_pvalue);
		fputs(" mbpta_runs_z=", out);
		diag_fprintf_double(out, 6, n->mbpta_runs_z);
		fputs(" mbpta_runs_p=", out);
		diag_fprintf_double(out, 6, n->mbpta_runs_pvalue);
		fputs(" mbpta_et_p=", out);
		diag_fprintf_double(out, 6, n->mbpta_et_pvalue);
		fputs(" mbpta_k=", out);
		diag_fprintf_double(out, 6, n->mbpta_gev_shape_k);
		fputs(" mbpta_r2=", out);
		diag_fprintf_double(out, 6, n->mbpta_gumbel_r2);
		fputs(" mbpta_rse=", out);
		diag_fprintf_double(out, 6, n->mbpta_gumbel_rse);
		fputs(" mbpta_crps=", out);
		diag_fprintf_double(out, 6, n->mbpta_crps);
		fprintf(out,
			" mbpta_conv=%u mbpta_iid_reject=%u"
			" mbpta_eps_eff=",
			n->mbpta_convergence_streak,
			n->mbpta_iid_reject_streak);
		diag_fprintf_double(out, 16, n->mbpta_effective_eps_node);
		fprintf(out, " mbpta_eps_capped=%s mbpta_last_invalidation=%s"
			" mbpta_estimator_key=0x%016llx\n",
			n->mbpta_eps_node_capped ? "true" : "false",
			n->mbpta_last_invalidation_reason[0] != '\0'
				? n->mbpta_last_invalidation_reason : "none",
			(unsigned long long)n->mbpta_estimator_key);
	}
}

/* --- combined JSON renderer --- */

static void json_write_escaped(FILE *out, const char *s)
{
	if (s == NULL) {
		fputs("\"\"", out);
		return;
	}
	fputc('"', out);
	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '"':  fputs("\\\"", out); break;
		case '\\': fputs("\\\\", out); break;
		case '\b': fputs("\\b", out);  break;
		case '\f': fputs("\\f", out);  break;
		case '\n': fputs("\\n", out);  break;
		case '\r': fputs("\\r", out);  break;
		case '\t': fputs("\\t", out);  break;
		default:
			if (c < 0x20)
				fprintf(out, "\\u%04x", c);
			else
				fputc((int)c, out);
		}
	}
	fputc('"', out);
}

static void json_write_flags(FILE *out, uint32_t flags,
			     const struct flag_name *table, size_t n)
{
	bool first = true;
	size_t i;

	fputc('[', out);
	for (i = 0; i < n; i++) {
		if ((flags & table[i].bit) == 0u)
			continue;
		if (!first)
			fputc(',', out);
		fputc('"', out);
		fputs(table[i].name, out);
		fputc('"', out);
		first = false;
	}
	fputc(']', out);
}

static void json_write_raw_section(FILE *out, const struct rt_diag_raw_snapshot *s)
{
	uint32_t i;

	fputs("{\"nodes\":[", out);
	if (s != NULL) {
		for (i = 0; i < s->n_nodes; i++) {
			const struct rt_diag_raw_node *n = &s->nodes[i];
			if (i > 0)
				fputc(',', out);
			fprintf(out,
				"{\"id\":%u,\"driver_id\":%u,\"tid\":%d,",
				n->id, n->driver_id, (int)n->tid);
			fputs("\"flags\":", out);
			json_write_flags(out, n->flags, raw_node_flags,
					 sizeof(raw_node_flags) /
					 sizeof(raw_node_flags[0]));
			fputs(",\"name\":", out);
			json_write_escaped(out, n->name);
			fputc('}', out);
		}
	}
	fputs("],\"edges\":[", out);
	if (s != NULL) {
		for (i = 0; i < s->n_edges; i++) {
			const struct rt_diag_raw_edge *e = &s->edges[i];
			if (i > 0)
				fputc(',', out);
			fprintf(out,
				"{\"src\":%u,\"dst\":%u,\"flags\":",
				e->src, e->dst);
			json_write_flags(out, e->flags, raw_edge_flags,
					 sizeof(raw_edge_flags) /
					 sizeof(raw_edge_flags[0]));
			fputc('}', out);
		}
	}
	fputs("]}", out);
}

static void json_write_sched_section(FILE *out, const struct rt_diag_sched_snapshot *s)
{
	uint32_t i;

	fputs("{\"nodes\":[", out);
	if (s != NULL) {
		for (i = 0; i < s->n_nodes; i++) {
			const struct rt_diag_sched_node *n = &s->nodes[i];
			if (i > 0)
				fputc(',', out);
			fprintf(out, "{\"id\":%u,\"tid\":%d}",
				n->id, (int)n->tid);
		}
	}
	fputs("],\"edges\":[", out);
	if (s != NULL) {
		for (i = 0; i < s->n_edges; i++) {
			const struct rt_diag_sched_edge *e = &s->edges[i];
			if (i > 0)
				fputc(',', out);
			fprintf(out, "{\"src\":%u,\"dst\":%u}", e->src, e->dst);
		}
	}
	fputs("],\"excluded_edges\":[", out);
	if (s != NULL) {
		for (i = 0; i < s->n_excluded; i++) {
			const struct rt_diag_sched_excluded_edge *e = &s->excluded_edges[i];
			if (i > 0)
				fputc(',', out);
			fprintf(out, "{\"src\":%u,\"dst\":%u,\"reason\":\"%s\"}",
				e->src, e->dst,
				rt_diag_sched_exclude_reason_name(e->reason));
		}
	}
	fputs("]}", out);
}

static void json_write_fusion_section(FILE *out, const struct rt_diag_fusion_snapshot *s)
{
	uint32_t i, j;

	fputs("{\"groups\":[", out);
	if (s != NULL) {
		for (i = 0; i < s->n_groups; i++) {
			const struct rt_diag_fusion_group *g = &s->groups[i];
			if (i > 0)
				fputc(',', out);
			fprintf(out,
				"{\"leader\":%u,\"verdict\":\"%s\",\"reason\":\"%s\",\"members\":[",
				g->leader_id,
				rt_diag_fusion_verdict_name(g->verdict),
				rt_diag_fusion_reject_reason_name(g->reject_reason));
			for (j = 0; j < g->n_members; j++) {
				if (j > 0)
					fputc(',', out);
				fprintf(out, "%u", g->members[j]);
			}
			fputs("]}", out);
		}
	}
	fputs("]}", out);
}

const char *rt_diag_mbpta_state_name(enum rt_diag_mbpta_state s)
{
	switch (s) {
	case RT_DIAG_MBPTA_INSUFFICIENT_DATA:   return "insufficient_data";
	case RT_DIAG_MBPTA_IID_PENDING:         return "iid_pending";
	case RT_DIAG_MBPTA_NON_GUMBEL:          return "non_gumbel";
	case RT_DIAG_MBPTA_PENDING_CONVERGENCE: return "pending_convergence";
	case RT_DIAG_MBPTA_PWCET_VALID:         return "pwcet_valid";
	case RT_DIAG_MBPTA_DRIFT:               return "drift";
	}
	return "unknown";
}

const char *rt_diag_budget_kind_name(enum rt_diag_budget_kind k)
{
	switch (k) {
	case RT_DIAG_BUDGET_DETERMINISTIC_WCET: return "deterministic_wcet";
	case RT_DIAG_BUDGET_PWCET:              return "pwcet";
	case RT_DIAG_BUDGET_EMPIRICAL_QUANTILE: return "empirical_quantile";
	case RT_DIAG_BUDGET_BOOTSTRAP_FALLBACK: return "bootstrap_fallback";
	case RT_DIAG_BUDGET_MANUAL_OVERRIDE:    return "manual_override";
	}
	return "unknown";
}

void rt_diag_peer_dispatch_init(struct rt_diag_peer_dispatch *s)
{
	if (s == NULL)
		return;
	memset(s, 0, sizeof(*s));
}

void rt_diag_peer_dispatch_reset(struct rt_diag_peer_dispatch *s)
{
	if (s == NULL)
		return;
	memset(s, 0, sizeof(*s));
}

void rt_diag_peer_dispatch_render_text(const struct rt_diag_peer_dispatch *s,
				       FILE *out)
{
	if (s == NULL || out == NULL)
		return;
	fprintf(out,
		"deadline-diag-peer-dispatch: driver=%u generation=%llu "
		"inline_armed=%u eventfd_path=%u\n",
		s->driver_id, (unsigned long long)s->generation,
		s->inline_armed, s->eventfd_path);
}

static void json_write_peer_dispatch_section(FILE *out,
		const struct rt_diag_peer_dispatch *s)
{
	if (s == NULL) {
		fputs("{\"inline_armed\":0,\"eventfd_path\":0}", out);
		return;
	}
	fprintf(out,
		"{\"inline_armed\":%u,\"eventfd_path\":%u}",
		s->inline_armed, s->eventfd_path);
}

static void json_write_params_section(FILE *out, const struct rt_diag_params_snapshot *s)
{
	uint32_t i;

	fputs("{\"nodes\":[", out);
	if (s != NULL) {
		for (i = 0; i < s->n_nodes; i++) {
			const struct rt_diag_param_node *n = &s->nodes[i];
			if (i > 0)
				fputc(',', out);
			fprintf(out,
				"{\"id\":%u,\"tid\":%d,\"runtime_ns\":%llu,"
				"\"local_deadline_ns\":%llu,"
				"\"cumulative_deadline_ns\":%llu,"
				"\"period_ns\":%llu,\"cpu\":%u,"
				"\"applied\":%s,\"budget_kind\":\"%s\","
				"\"budget_samples\":%llu,"
				"\"mbpta\":{\"state\":\"%s\","
				"\"pwcet_ns\":%llu,\"blocks\":%u,"
				"\"mu\":",
				n->id, (int)n->tid,
				(unsigned long long)n->runtime_budget_ns,
				(unsigned long long)n->local_deadline_ns,
				(unsigned long long)n->cumulative_deadline_ns,
				(unsigned long long)n->period_ns,
				n->cpu,
				n->applied ? "true" : "false",
				rt_diag_budget_kind_name(n->budget_kind),
				(unsigned long long)n->budget_sample_count,
				rt_diag_mbpta_state_name(n->mbpta_state),
				(unsigned long long)n->mbpta_pwcet_ns,
				n->mbpta_block_count);
			diag_fprintf_double(out, 0, n->mbpta_mu);
			fputs(",\"sigma\":", out);
			diag_fprintf_double(out, 0, n->mbpta_sigma);
			fputs(",\"ks_stat\":", out);
			diag_fprintf_double(out, 6, n->mbpta_ks_stat);
			fputs(",\"ks_pvalue\":", out);
			diag_fprintf_double(out, 6, n->mbpta_ks_pvalue);
			fputs(",\"runs_z\":", out);
			diag_fprintf_double(out, 6, n->mbpta_runs_z);
			fputs(",\"runs_pvalue\":", out);
			diag_fprintf_double(out, 6, n->mbpta_runs_pvalue);
			fputs(",\"et_pvalue\":", out);
			diag_fprintf_double(out, 6, n->mbpta_et_pvalue);
			fputs(",\"gev_shape_k\":", out);
			diag_fprintf_double(out, 6, n->mbpta_gev_shape_k);
			fputs(",\"gumbel_r2\":", out);
			diag_fprintf_double(out, 6, n->mbpta_gumbel_r2);
			fputs(",\"gumbel_rse\":", out);
			diag_fprintf_double(out, 6, n->mbpta_gumbel_rse);
			fputs(",\"crps\":", out);
			diag_fprintf_double(out, 6, n->mbpta_crps);
			fprintf(out,
				",\"convergence_streak\":%u,"
				"\"iid_reject_streak\":%u,"
				"\"effective_eps_node\":",
				n->mbpta_convergence_streak,
				n->mbpta_iid_reject_streak);
			diag_fprintf_double(out, 16,
					n->mbpta_effective_eps_node);
			fprintf(out,
				",\"eps_node_capped\":%s,"
				"\"last_invalidation_reason\":",
				n->mbpta_eps_node_capped ? "true" : "false");
			json_write_escaped(out,
				n->mbpta_last_invalidation_reason[0] != '\0'
				? n->mbpta_last_invalidation_reason : "none");
			{
				char keybuf[32];
				snprintf(keybuf, sizeof(keybuf),
					"0x%016llx",
					(unsigned long long)n->mbpta_estimator_key);
				fputs(",\"estimator_key\":", out);
				json_write_escaped(out, keybuf);
			}
			fputs("}}", out);
		}
	}
	/* Cucu-Grosjean 2012 §IV "Path Coverage": an MBPTA pWCET is
	 * extrapolated from samples drawn from paths actually
	 * executed in the sample window. Branches that never fired
	 * in-window are not covered by the fit; an operator reading
	 * the snapshot must know the claim's scope. Surface the
	 * disclaimer alongside the per-node pWCET values so it
	 * travels with the data. */
	fputs("],\"pwcet_path_coverage_note\":\""
	      "pWCET claims do not extend to untriggered branches; "
	      "the result is only valid for paths actually observed "
	      "in the sample window (Cucu-Grosjean 2012 SIV).\"}", out);
}

void rt_diag_render_json(const struct rt_diag_combined *c, FILE *out)
{
	if (c == NULL || out == NULL)
		return;

	const char *mode = c->mode != NULL ? c->mode : "prototype";
	const char *feas_method = c->feasibility_method != NULL
		? c->feasibility_method : "none";
	const char *feas_status = c->feasibility_status != NULL
		? c->feasibility_status : "n/a";

	fputs("{\"module\":\"module-deadline\",", out);
	fprintf(out, "\"driver_id\":%u,", c->driver_id);
	fprintf(out, "\"generation\":%llu,",
		(unsigned long long)c->generation);
	fprintf(out, "\"period_ns\":%llu,",
		(unsigned long long)c->period_ns);
	fprintf(out, "\"deadline_ns\":%llu,",
		(unsigned long long)c->deadline_ns);
	fputs("\"mode\":", out);
	json_write_escaped(out, mode);
	fputs(",\"feasibility\":{\"method\":", out);
	json_write_escaped(out, feas_method);
	fputs(",\"status\":", out);
	json_write_escaped(out, feas_status);
	fputc('}', out);
	fputs(",\"raw_graph\":", out);
	json_write_raw_section(out, c->raw);
	fputs(",\"scheduling_dag\":", out);
	json_write_sched_section(out, c->sched);
	fputs(",\"fusion\":", out);
	json_write_fusion_section(out, c->fusion);
	fputs(",\"parameters\":", out);
	json_write_params_section(out, c->params);
	fputs(",\"peer_dispatch\":", out);
	json_write_peer_dispatch_section(out, c->peer_dispatch);
	fputc('}', out);
	fputc('\n', out);
}
