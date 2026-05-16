/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Unit tests for module-deadline/diag.c.
 *
 * The diagnostics layer is plumbing: types describing a snapshot
 * plus a renderer. These tests pin the contract:
 *
 *   - init zeros the struct; fini frees and returns to the post-init
 *     state; reset rewinds the logical counters without freeing.
 *   - add_node / add_edge grow the backing arrays geometrically and
 *     reject NULL inputs with -EINVAL.
 *   - the text renderer produces the documented header / nodes /
 *     edges shape, with stable flag tokens and a "-" placeholder
 *     when the bitmask is empty.
 *
 * The renderer output is verified by writing into a memory stream
 * (open_memstream) and comparing the resulting buffer against a
 * literal expected string. The renderer is the public contract --
 * if a format change is intentional, the expected string must be
 * updated in the same commit so the reason is reviewable.
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/diag.h"

PWTEST(diag_raw_init_zeroes)
{
	struct rt_diag_raw_snapshot s;
	memset(&s, 0xcc, sizeof(s));
	rt_diag_raw_snapshot_init(&s);
	pwtest_ptr_null(s.nodes);
	pwtest_ptr_null(s.edges);
	pwtest_int_eq((int)s.n_nodes, 0);
	pwtest_int_eq((int)s.cap_nodes, 0);
	pwtest_int_eq((int)s.n_edges, 0);
	pwtest_int_eq((int)s.cap_edges, 0);
	pwtest_int_eq((int)s.driver_id, 0);
	return PWTEST_PASS;
}

PWTEST(diag_raw_null_safe)
{
	rt_diag_raw_snapshot_init(NULL);
	rt_diag_raw_snapshot_fini(NULL);
	rt_diag_raw_snapshot_reset(NULL);
	rt_diag_raw_snapshot_render_text(NULL, stderr);
	return PWTEST_PASS;
}

PWTEST(diag_raw_add_node_rejects_null)
{
	struct rt_diag_raw_snapshot s;
	rt_diag_raw_snapshot_init(&s);
	pwtest_int_eq(rt_diag_raw_snapshot_add_node(&s, NULL), -EINVAL);
	pwtest_int_eq(rt_diag_raw_snapshot_add_node(NULL, NULL), -EINVAL);
	pwtest_int_eq((int)s.n_nodes, 0);
	rt_diag_raw_snapshot_fini(&s);
	return PWTEST_PASS;
}

PWTEST(diag_raw_add_edge_rejects_null)
{
	struct rt_diag_raw_snapshot s;
	rt_diag_raw_snapshot_init(&s);
	pwtest_int_eq(rt_diag_raw_snapshot_add_edge(&s, NULL), -EINVAL);
	pwtest_int_eq(rt_diag_raw_snapshot_add_edge(NULL, NULL), -EINVAL);
	pwtest_int_eq((int)s.n_edges, 0);
	rt_diag_raw_snapshot_fini(&s);
	return PWTEST_PASS;
}

PWTEST(diag_raw_add_node_copies_and_truncates_name)
{
	struct rt_diag_raw_snapshot s;
	struct rt_diag_raw_node n = { 0 };
	rt_diag_raw_snapshot_init(&s);

	n.id = 42;
	n.driver_id = 41;
	n.tid = 1234;
	n.flags = RT_DIAG_RAW_NODE_DATA_LOOP | RT_DIAG_RAW_NODE_DYNAMIC_LOOP;
	/* Source string is exactly RT_DIAG_NODE_NAME_MAX-1 'a' chars
	 * followed by NUL; renderer must store it verbatim. */
	memset(n.name, 'a', RT_DIAG_NODE_NAME_MAX - 1);
	n.name[RT_DIAG_NODE_NAME_MAX - 1] = '\0';

	pwtest_int_eq(rt_diag_raw_snapshot_add_node(&s, &n), 0);
	pwtest_int_eq((int)s.n_nodes, 1);
	pwtest_int_eq((int)s.nodes[0].id, 42);
	pwtest_int_eq((int)s.nodes[0].driver_id, 41);
	pwtest_int_eq((int)s.nodes[0].tid, 1234);
	pwtest_int_eq((int)s.nodes[0].flags,
		      (int)(RT_DIAG_RAW_NODE_DATA_LOOP | RT_DIAG_RAW_NODE_DYNAMIC_LOOP));
	pwtest_int_eq((int)strlen(s.nodes[0].name), RT_DIAG_NODE_NAME_MAX - 1);

	rt_diag_raw_snapshot_fini(&s);
	return PWTEST_PASS;
}

PWTEST(diag_raw_geometric_growth)
{
	struct rt_diag_raw_snapshot s;
	struct rt_diag_raw_node n = { 0 };
	struct rt_diag_raw_edge e = { 0 };
	uint32_t i;
	rt_diag_raw_snapshot_init(&s);

	strcpy(n.name, "x");
	for (i = 0; i < 32; i++) {
		n.id = i;
		pwtest_int_eq(rt_diag_raw_snapshot_add_node(&s, &n), 0);
	}
	for (i = 0; i < 64; i++) {
		e.src = i;
		e.dst = i + 1;
		pwtest_int_eq(rt_diag_raw_snapshot_add_edge(&s, &e), 0);
	}
	pwtest_int_eq((int)s.n_nodes, 32);
	pwtest_int_eq((int)s.n_edges, 64);
	pwtest_bool_true(s.cap_nodes >= s.n_nodes);
	pwtest_bool_true(s.cap_edges >= s.n_edges);

	rt_diag_raw_snapshot_reset(&s);
	pwtest_int_eq((int)s.n_nodes, 0);
	pwtest_int_eq((int)s.n_edges, 0);
	pwtest_bool_true(s.cap_nodes >= 32);
	pwtest_bool_true(s.cap_edges >= 64);

	rt_diag_raw_snapshot_fini(&s);
	pwtest_ptr_null(s.nodes);
	pwtest_ptr_null(s.edges);
	return PWTEST_PASS;
}

/* Build a synthetic snapshot covering the documented format and
 * compare against a literal expected string. Failure mode here is
 * intentional: a format change must update the expected output
 * in the same commit so the reason is reviewable. */
PWTEST(diag_raw_render_text_golden)
{
	struct rt_diag_raw_snapshot s;
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	const char expected[] =
		"deadline-diag-raw: driver=42 generation=17 period_ns=1000000 deadline_ns=1000000\n"
		"  nodes: 3\n"
		"    node id=100 driver_id=42 tid=1234 flags=driver,data_loop name=alsa-sink\n"
		"    node id=101 driver_id=42 tid=1235 flags=data_loop,remote,dynamic_loop name=client-output\n"
		"    node id=102 driver_id=42 tid=0 flags=- name=-\n"
		"  edges: 2\n"
		"    edge 101->100 flags=-\n"
		"    edge 100->101 flags=feedback\n";

	rt_diag_raw_snapshot_init(&s);
	s.driver_id = 42;
	s.generation = 17;
	s.period_ns = 1000000ull;
	s.deadline_ns = 1000000ull;

	struct rt_diag_raw_node nd = { 0 };

	nd.id = 100;
	nd.driver_id = 42;
	nd.tid = 1234;
	nd.flags = RT_DIAG_RAW_NODE_DRIVER | RT_DIAG_RAW_NODE_DATA_LOOP;
	strcpy(nd.name, "alsa-sink");
	pwtest_int_eq(rt_diag_raw_snapshot_add_node(&s, &nd), 0);

	memset(&nd, 0, sizeof(nd));
	nd.id = 101;
	nd.driver_id = 42;
	nd.tid = 1235;
	nd.flags = RT_DIAG_RAW_NODE_DATA_LOOP | RT_DIAG_RAW_NODE_REMOTE |
		   RT_DIAG_RAW_NODE_DYNAMIC_LOOP;
	strcpy(nd.name, "client-output");
	pwtest_int_eq(rt_diag_raw_snapshot_add_node(&s, &nd), 0);

	memset(&nd, 0, sizeof(nd));
	nd.id = 102;
	nd.driver_id = 42;
	/* Leave tid=0, flags=0, name=empty to cover the "-" rendering. */
	pwtest_int_eq(rt_diag_raw_snapshot_add_node(&s, &nd), 0);

	struct rt_diag_raw_edge ed = { 0 };
	ed.src = 101;
	ed.dst = 100;
	pwtest_int_eq(rt_diag_raw_snapshot_add_edge(&s, &ed), 0);

	ed.src = 100;
	ed.dst = 101;
	ed.flags = RT_DIAG_RAW_EDGE_FEEDBACK;
	pwtest_int_eq(rt_diag_raw_snapshot_add_edge(&s, &ed), 0);

	fp = open_memstream(&buf, &len);
	pwtest_ptr_notnull(fp);
	rt_diag_raw_snapshot_render_text(&s, fp);
	fclose(fp);

	pwtest_str_eq(buf, expected);

	free(buf);
	rt_diag_raw_snapshot_fini(&s);
	return PWTEST_PASS;
}

/* --- scheduling-DAG slice tests --- */

PWTEST(diag_sched_init_zeroes)
{
	struct rt_diag_sched_snapshot s;
	memset(&s, 0xcc, sizeof(s));
	rt_diag_sched_snapshot_init(&s);
	pwtest_ptr_null(s.nodes);
	pwtest_ptr_null(s.edges);
	pwtest_ptr_null(s.excluded_edges);
	pwtest_int_eq((int)s.n_nodes, 0);
	pwtest_int_eq((int)s.n_edges, 0);
	pwtest_int_eq((int)s.n_excluded, 0);
	return PWTEST_PASS;
}

PWTEST(diag_sched_null_safe)
{
	rt_diag_sched_snapshot_init(NULL);
	rt_diag_sched_snapshot_fini(NULL);
	rt_diag_sched_snapshot_reset(NULL);
	rt_diag_sched_snapshot_render_text(NULL, stderr);
	return PWTEST_PASS;
}

PWTEST(diag_sched_excluded_rejects_none)
{
	struct rt_diag_sched_snapshot s;
	struct rt_diag_sched_excluded_edge e = { 0 };
	rt_diag_sched_snapshot_init(&s);
	e.src = 1; e.dst = 2; e.reason = RT_DIAG_SCHED_EXC_NONE;
	/* NONE must not appear in the excluded list -- the reason
	 * sentinel is just for "this edge is not excluded". */
	pwtest_int_eq(rt_diag_sched_snapshot_add_excluded(&s, &e), -EINVAL);
	pwtest_int_eq((int)s.n_excluded, 0);
	rt_diag_sched_snapshot_fini(&s);
	return PWTEST_PASS;
}

PWTEST(diag_sched_reason_names_stable)
{
	pwtest_str_eq(rt_diag_sched_exclude_reason_name(RT_DIAG_SCHED_EXC_NONE),
		      "none");
	pwtest_str_eq(rt_diag_sched_exclude_reason_name(RT_DIAG_SCHED_EXC_FEEDBACK),
		      "feedback");
	pwtest_str_eq(rt_diag_sched_exclude_reason_name(RT_DIAG_SCHED_EXC_ASYNC),
		      "async");
	pwtest_str_eq(rt_diag_sched_exclude_reason_name(RT_DIAG_SCHED_EXC_CROSS_DRIVER),
		      "cross_driver");
	pwtest_str_eq(rt_diag_sched_exclude_reason_name(RT_DIAG_SCHED_EXC_EXPORTED),
		      "exported");
	pwtest_str_eq(rt_diag_sched_exclude_reason_name(RT_DIAG_SCHED_EXC_NON_RT),
		      "non_rt");
	pwtest_str_eq(rt_diag_sched_exclude_reason_name(RT_DIAG_SCHED_EXC_UNSUPPORTED),
		      "unsupported");
	pwtest_str_eq(rt_diag_sched_exclude_reason_name((enum rt_diag_sched_exclude_reason)999),
		      "unknown");
	return PWTEST_PASS;
}

PWTEST(diag_sched_render_text_golden)
{
	struct rt_diag_sched_snapshot s;
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	const char expected[] =
		"deadline-diag-sched: driver=63 generation=4 period_ns=1000000 deadline_ns=1000000\n"
		"  nodes: 2\n"
		"    node id=37 tid=302370\n"
		"    node id=38 tid=302371\n"
		"  edges: 1\n"
		"    edge 38->63\n"
		"  excluded: 2\n"
		"    excluded 100->101 reason=feedback\n"
		"    excluded 76->37 reason=async\n";

	rt_diag_sched_snapshot_init(&s);
	s.driver_id = 63;
	s.generation = 4;
	s.period_ns = 1000000ull;
	s.deadline_ns = 1000000ull;

	struct rt_diag_sched_node n = { 0 };
	n.id = 37; n.tid = 302370;
	pwtest_int_eq(rt_diag_sched_snapshot_add_node(&s, &n), 0);
	n.id = 38; n.tid = 302371;
	pwtest_int_eq(rt_diag_sched_snapshot_add_node(&s, &n), 0);

	struct rt_diag_sched_edge e = { 0 };
	e.src = 38; e.dst = 63;
	pwtest_int_eq(rt_diag_sched_snapshot_add_edge(&s, &e), 0);

	struct rt_diag_sched_excluded_edge x = { 0 };
	x.src = 100; x.dst = 101; x.reason = RT_DIAG_SCHED_EXC_FEEDBACK;
	pwtest_int_eq(rt_diag_sched_snapshot_add_excluded(&s, &x), 0);
	x.src = 76;  x.dst = 37;  x.reason = RT_DIAG_SCHED_EXC_ASYNC;
	pwtest_int_eq(rt_diag_sched_snapshot_add_excluded(&s, &x), 0);

	fp = open_memstream(&buf, &len);
	pwtest_ptr_notnull(fp);
	rt_diag_sched_snapshot_render_text(&s, fp);
	fclose(fp);

	pwtest_str_eq(buf, expected);

	free(buf);
	rt_diag_sched_snapshot_fini(&s);
	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_diag)
{
	pwtest_add(diag_raw_init_zeroes, PWTEST_NOARG);
	pwtest_add(diag_raw_null_safe, PWTEST_NOARG);
	pwtest_add(diag_raw_add_node_rejects_null, PWTEST_NOARG);
	pwtest_add(diag_raw_add_edge_rejects_null, PWTEST_NOARG);
	pwtest_add(diag_raw_add_node_copies_and_truncates_name, PWTEST_NOARG);
	pwtest_add(diag_raw_geometric_growth, PWTEST_NOARG);
	pwtest_add(diag_raw_render_text_golden, PWTEST_NOARG);
	pwtest_add(diag_sched_init_zeroes, PWTEST_NOARG);
	pwtest_add(diag_sched_null_safe, PWTEST_NOARG);
	pwtest_add(diag_sched_excluded_rejects_none, PWTEST_NOARG);
	pwtest_add(diag_sched_reason_names_stable, PWTEST_NOARG);
	pwtest_add(diag_sched_render_text_golden, PWTEST_NOARG);

	return PWTEST_PASS;
}
