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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"

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
