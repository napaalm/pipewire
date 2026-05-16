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

/* --- fusion-decision slice tests --- */

PWTEST(diag_fusion_null_safe)
{
	rt_diag_fusion_snapshot_init(NULL);
	rt_diag_fusion_snapshot_fini(NULL);
	rt_diag_fusion_snapshot_reset(NULL);
	rt_diag_fusion_snapshot_render_text(NULL, stderr);
	pwtest_int_eq(rt_diag_fusion_snapshot_begin_group(NULL, 0,
				RT_DIAG_FUSION_FUSE, RT_DIAG_FUSION_REJ_NONE),
		      -EINVAL);
	pwtest_int_eq(rt_diag_fusion_snapshot_add_member(NULL, 0, 1),
		      -EINVAL);
	return PWTEST_PASS;
}

PWTEST(diag_fusion_begin_group_validates_pair)
{
	struct rt_diag_fusion_snapshot s;
	rt_diag_fusion_snapshot_init(&s);

	/* FUSE must come with reason=NONE; any other reason is an
	 * error and the slot is not consumed. */
	pwtest_int_eq(rt_diag_fusion_snapshot_begin_group(&s, 1,
				RT_DIAG_FUSION_FUSE,
				RT_DIAG_FUSION_REJ_BELOW_THRESHOLD),
		      -EINVAL);
	pwtest_int_eq((int)s.n_groups, 0);

	/* SPLIT must come with a non-NONE reason. */
	pwtest_int_eq(rt_diag_fusion_snapshot_begin_group(&s, 2,
				RT_DIAG_FUSION_SPLIT,
				RT_DIAG_FUSION_REJ_NONE),
		      -EINVAL);
	pwtest_int_eq((int)s.n_groups, 0);

	rt_diag_fusion_snapshot_fini(&s);
	return PWTEST_PASS;
}

PWTEST(diag_fusion_add_member_out_of_range)
{
	struct rt_diag_fusion_snapshot s;
	rt_diag_fusion_snapshot_init(&s);
	pwtest_int_eq(rt_diag_fusion_snapshot_add_member(&s, 0, 1), -EINVAL);
	rt_diag_fusion_snapshot_fini(&s);
	return PWTEST_PASS;
}

PWTEST(diag_fusion_render_text_golden)
{
	struct rt_diag_fusion_snapshot s;
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	int g0, g1, g2;
	const char expected[] =
		"deadline-diag-fusion: driver=63 generation=5\n"
		"  groups: 3\n"
		"    group leader=10 verdict=fuse reason=none members=10,11,12\n"
		"    group leader=20 verdict=linear_only reason=below_threshold members=20,21\n"
		"    group leader=30 verdict=split reason=below_threshold members=-\n";

	rt_diag_fusion_snapshot_init(&s);
	s.driver_id = 63;
	s.generation = 5;

	g0 = rt_diag_fusion_snapshot_begin_group(&s, 10,
			RT_DIAG_FUSION_FUSE, RT_DIAG_FUSION_REJ_NONE);
	pwtest_int_eq(g0, 0);
	pwtest_int_eq(rt_diag_fusion_snapshot_add_member(&s, (uint32_t)g0, 10), 0);
	pwtest_int_eq(rt_diag_fusion_snapshot_add_member(&s, (uint32_t)g0, 11), 0);
	pwtest_int_eq(rt_diag_fusion_snapshot_add_member(&s, (uint32_t)g0, 12), 0);

	g1 = rt_diag_fusion_snapshot_begin_group(&s, 20,
			RT_DIAG_FUSION_LINEAR_ONLY,
			RT_DIAG_FUSION_REJ_BELOW_THRESHOLD);
	pwtest_int_eq(g1, 1);
	pwtest_int_eq(rt_diag_fusion_snapshot_add_member(&s, (uint32_t)g1, 20), 0);
	pwtest_int_eq(rt_diag_fusion_snapshot_add_member(&s, (uint32_t)g1, 21), 0);

	g2 = rt_diag_fusion_snapshot_begin_group(&s, 30,
			RT_DIAG_FUSION_SPLIT,
			RT_DIAG_FUSION_REJ_BELOW_THRESHOLD);
	pwtest_int_eq(g2, 2);
	/* Empty member list: renders as "-" so the format remains
	 * parseable even on a singleton split. */

	fp = open_memstream(&buf, &len);
	pwtest_ptr_notnull(fp);
	rt_diag_fusion_snapshot_render_text(&s, fp);
	fclose(fp);

	pwtest_str_eq(buf, expected);

	free(buf);
	rt_diag_fusion_snapshot_fini(&s);
	return PWTEST_PASS;
}

PWTEST(diag_fusion_verdict_and_reason_names)
{
	pwtest_str_eq(rt_diag_fusion_verdict_name(RT_DIAG_FUSION_FUSE), "fuse");
	pwtest_str_eq(rt_diag_fusion_verdict_name(RT_DIAG_FUSION_LINEAR_ONLY),
		      "linear_only");
	pwtest_str_eq(rt_diag_fusion_verdict_name(RT_DIAG_FUSION_SPLIT), "split");
	pwtest_str_eq(rt_diag_fusion_verdict_name((enum rt_diag_fusion_verdict)999),
		      "unknown");
	pwtest_str_eq(rt_diag_fusion_reject_reason_name(RT_DIAG_FUSION_REJ_NONE),
		      "none");
	pwtest_str_eq(rt_diag_fusion_reject_reason_name(RT_DIAG_FUSION_REJ_BELOW_THRESHOLD),
		      "below_threshold");
	pwtest_str_eq(rt_diag_fusion_reject_reason_name(
		      (enum rt_diag_fusion_reject_reason)999), "unknown");
	return PWTEST_PASS;
}

/* --- scheduling-parameters slice tests --- */

PWTEST(diag_params_init_zeroes)
{
	struct rt_diag_params_snapshot s;
	memset(&s, 0xcc, sizeof(s));
	rt_diag_params_snapshot_init(&s);
	pwtest_ptr_null(s.nodes);
	pwtest_int_eq((int)s.n_nodes, 0);
	pwtest_int_eq((int)s.cap_nodes, 0);
	return PWTEST_PASS;
}

PWTEST(diag_params_null_safe)
{
	rt_diag_params_snapshot_init(NULL);
	rt_diag_params_snapshot_fini(NULL);
	rt_diag_params_snapshot_reset(NULL);
	rt_diag_params_snapshot_render_text(NULL, "x", stderr);
	pwtest_int_eq(rt_diag_params_snapshot_add_node(NULL, NULL), -EINVAL);
	return PWTEST_PASS;
}

PWTEST(diag_params_render_text_golden)
{
	struct rt_diag_params_snapshot s;
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	const char expected[] =
		"deadline-diag-params: driver=63 generation=4 mode=prototype\n"
		"  nodes: 2\n"
		"    node id=37 tid=302370 runtime=85494ns local_deadline=21333333ns"
		" cumulative_deadline=21333333ns period=21333333ns cpu=4 applied=true"
		" budget_kind=empirical_quantile budget_samples=128"
		" mbpta_state=insufficient_data mbpta_pwcet=0ns mbpta_blocks=0"
		" mbpta_mu=0 mbpta_sigma=0"
		" mbpta_ks=0.000000 mbpta_runs_z=0.000000 mbpta_crps=0.000000"
		" mbpta_conv=0 mbpta_iid_reject=0\n"
		"    node id=38 tid=302371 runtime=42620ns local_deadline=21333333ns"
		" cumulative_deadline=21333333ns period=21333333ns cpu=5 applied=false"
		" budget_kind=bootstrap_fallback budget_samples=0"
		" mbpta_state=insufficient_data mbpta_pwcet=0ns mbpta_blocks=0"
		" mbpta_mu=0 mbpta_sigma=0"
		" mbpta_ks=0.000000 mbpta_runs_z=0.000000 mbpta_crps=0.000000"
		" mbpta_conv=0 mbpta_iid_reject=0\n";

	rt_diag_params_snapshot_init(&s);
	s.driver_id = 63;
	s.generation = 4;

	struct rt_diag_param_node n = { 0 };
	n.id = 37; n.tid = 302370;
	n.runtime_budget_ns = 85494; n.local_deadline_ns = 21333333;
	n.cumulative_deadline_ns = 21333333; n.period_ns = 21333333;
	n.cpu = 4; n.applied = true;
	n.budget_kind = RT_DIAG_BUDGET_EMPIRICAL_QUANTILE;
	n.budget_sample_count = 128;
	pwtest_int_eq(rt_diag_params_snapshot_add_node(&s, &n), 0);
	n.id = 38; n.tid = 302371;
	n.runtime_budget_ns = 42620; n.local_deadline_ns = 21333333;
	n.cumulative_deadline_ns = 21333333; n.period_ns = 21333333;
	n.cpu = 5; n.applied = false;
	n.budget_kind = RT_DIAG_BUDGET_BOOTSTRAP_FALLBACK;
	n.budget_sample_count = 0;
	pwtest_int_eq(rt_diag_params_snapshot_add_node(&s, &n), 0);

	fp = open_memstream(&buf, &len);
	pwtest_ptr_notnull(fp);
	rt_diag_params_snapshot_render_text(&s, "prototype", fp);
	fclose(fp);
	pwtest_str_eq(buf, expected);

	free(buf);
	rt_diag_params_snapshot_fini(&s);
	return PWTEST_PASS;
}

/* --- combined JSON renderer tests --- */

PWTEST(diag_json_null_inputs)
{
	rt_diag_render_json(NULL, stderr);
	struct rt_diag_combined c = { 0 };
	rt_diag_render_json(&c, NULL);
	/* No crash, no output. */
	return PWTEST_PASS;
}

PWTEST(diag_json_empty_combined)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	const char expected[] =
		"{\"module\":\"module-deadline\","
		"\"driver_id\":0,\"generation\":0,\"period_ns\":0,"
		"\"deadline_ns\":0,\"mode\":\"prototype\","
		"\"feasibility\":{\"method\":\"none\",\"status\":\"n/a\"},"
		"\"raw_graph\":{\"nodes\":[],\"edges\":[]},"
		"\"scheduling_dag\":{\"nodes\":[],\"edges\":[],\"excluded_edges\":[]},"
		"\"fusion\":{\"groups\":[]},"
		"\"parameters\":{\"nodes\":[]},"
		"\"peer_dispatch\":{\"inline_armed\":0,\"eventfd_path\":0}}\n";

	struct rt_diag_combined c = { 0 };
	fp = open_memstream(&buf, &len);
	pwtest_ptr_notnull(fp);
	rt_diag_render_json(&c, fp);
	fclose(fp);
	pwtest_str_eq(buf, expected);

	free(buf);
	return PWTEST_PASS;
}

PWTEST(diag_json_full_golden)
{
	struct rt_diag_raw_snapshot raw;
	struct rt_diag_sched_snapshot sched;
	struct rt_diag_fusion_snapshot fusion;
	struct rt_diag_params_snapshot params;
	struct rt_diag_combined c = { 0 };
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	int g0;
	const char expected[] =
		"{\"module\":\"module-deadline\","
		"\"driver_id\":63,\"generation\":3,\"period_ns\":21333333,"
		"\"deadline_ns\":21333333,\"mode\":\"prototype\","
		"\"feasibility\":{\"method\":\"none\",\"status\":\"n/a\"},"
		"\"raw_graph\":{\"nodes\":["
		"{\"id\":63,\"driver_id\":63,\"tid\":302388,\"flags\":[\"driver\",\"data_loop\"],\"name\":\"alsa-sink\"}"
		"],\"edges\":[{\"src\":38,\"dst\":63,\"flags\":[]}]},"
		"\"scheduling_dag\":{\"nodes\":[{\"id\":63,\"tid\":302388}],"
		"\"edges\":[{\"src\":38,\"dst\":63}],"
		"\"excluded_edges\":[{\"src\":76,\"dst\":37,\"reason\":\"async\"}]},"
		"\"fusion\":{\"groups\":[{\"leader\":63,\"verdict\":\"split\","
		"\"reason\":\"below_threshold\",\"members\":[63]}]},"
		"\"parameters\":{\"nodes\":[{\"id\":63,\"tid\":302388,"
		"\"runtime_ns\":85494,\"local_deadline_ns\":21333333,"
		"\"cumulative_deadline_ns\":21333333,\"period_ns\":21333333,"
		"\"cpu\":4,\"applied\":true,"
		"\"budget_kind\":\"empirical_quantile\","
		"\"budget_samples\":512,"
		"\"mbpta\":{\"state\":\"pwcet_valid\",\"pwcet_ns\":91234,\"blocks\":50,"
		"\"mu\":120000,\"sigma\":3500,"
		"\"ks_stat\":0.041000,\"runs_z\":-0.230000,"
		"\"crps\":0.072000,"
		"\"convergence_streak\":4,"
		"\"iid_reject_streak\":0}}]},"
		"\"peer_dispatch\":{\"inline_armed\":5,\"eventfd_path\":2}}\n";

	rt_diag_raw_snapshot_init(&raw);
	rt_diag_sched_snapshot_init(&sched);
	rt_diag_fusion_snapshot_init(&fusion);
	rt_diag_params_snapshot_init(&params);

	struct rt_diag_raw_node rn = { 0 };
	rn.id = 63; rn.driver_id = 63; rn.tid = 302388;
	rn.flags = RT_DIAG_RAW_NODE_DRIVER | RT_DIAG_RAW_NODE_DATA_LOOP;
	strcpy(rn.name, "alsa-sink");
	pwtest_int_eq(rt_diag_raw_snapshot_add_node(&raw, &rn), 0);

	struct rt_diag_raw_edge re = { 0 };
	re.src = 38; re.dst = 63;
	pwtest_int_eq(rt_diag_raw_snapshot_add_edge(&raw, &re), 0);

	struct rt_diag_sched_node sn = { 0 };
	sn.id = 63; sn.tid = 302388;
	pwtest_int_eq(rt_diag_sched_snapshot_add_node(&sched, &sn), 0);

	struct rt_diag_sched_edge se = { 0 };
	se.src = 38; se.dst = 63;
	pwtest_int_eq(rt_diag_sched_snapshot_add_edge(&sched, &se), 0);

	struct rt_diag_sched_excluded_edge xe = { 0 };
	xe.src = 76; xe.dst = 37; xe.reason = RT_DIAG_SCHED_EXC_ASYNC;
	pwtest_int_eq(rt_diag_sched_snapshot_add_excluded(&sched, &xe), 0);

	g0 = rt_diag_fusion_snapshot_begin_group(&fusion, 63,
			RT_DIAG_FUSION_SPLIT,
			RT_DIAG_FUSION_REJ_BELOW_THRESHOLD);
	pwtest_int_eq(g0, 0);
	pwtest_int_eq(rt_diag_fusion_snapshot_add_member(&fusion,
				(uint32_t)g0, 63), 0);

	struct rt_diag_param_node pn = { 0 };
	pn.id = 63; pn.tid = 302388;
	pn.runtime_budget_ns = 85494; pn.local_deadline_ns = 21333333;
	pn.cumulative_deadline_ns = 21333333; pn.period_ns = 21333333;
	pn.cpu = 4; pn.applied = true;
	pn.budget_kind = RT_DIAG_BUDGET_EMPIRICAL_QUANTILE;
	pn.budget_sample_count = 512;
	pn.mbpta_state = RT_DIAG_MBPTA_PWCET_VALID;
	pn.mbpta_pwcet_ns = 91234;
	pn.mbpta_block_count = 50;
	pn.mbpta_mu = 120000.0;
	pn.mbpta_sigma = 3500.0;
	pn.mbpta_ks_stat = 0.041;
	pn.mbpta_runs_z = -0.23;
	pn.mbpta_crps = 0.072;
	pn.mbpta_convergence_streak = 4;
	pn.mbpta_iid_reject_streak = 0;
	pwtest_int_eq(rt_diag_params_snapshot_add_node(&params, &pn), 0);

	c.driver_id = 63;
	c.generation = 3;
	c.period_ns = 21333333;
	c.deadline_ns = 21333333;
	c.mode = "prototype";
	c.feasibility_method = "none";
	c.feasibility_status = "n/a";
	struct rt_diag_peer_dispatch pd;
	rt_diag_peer_dispatch_init(&pd);
	pd.driver_id = 63;
	pd.generation = 3;
	pd.inline_armed = 5;
	pd.eventfd_path = 2;

	c.raw = &raw;
	c.sched = &sched;
	c.fusion = &fusion;
	c.params = &params;
	c.peer_dispatch = &pd;

	fp = open_memstream(&buf, &len);
	pwtest_ptr_notnull(fp);
	rt_diag_render_json(&c, fp);
	fclose(fp);
	pwtest_str_eq(buf, expected);

	free(buf);
	rt_diag_raw_snapshot_fini(&raw);
	rt_diag_sched_snapshot_fini(&sched);
	rt_diag_fusion_snapshot_fini(&fusion);
	rt_diag_params_snapshot_fini(&params);
	return PWTEST_PASS;
}

PWTEST(diag_peer_dispatch_render_text)
{
	struct rt_diag_peer_dispatch pd;
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	const char expected[] =
		"deadline-diag-peer-dispatch: driver=42 generation=7 "
		"inline_armed=4 eventfd_path=1\n";

	rt_diag_peer_dispatch_init(&pd);
	pd.driver_id = 42;
	pd.generation = 7;
	pd.inline_armed = 4;
	pd.eventfd_path = 1;

	fp = open_memstream(&buf, &len);
	pwtest_ptr_notnull(fp);
	rt_diag_peer_dispatch_render_text(&pd, fp);
	fclose(fp);
	pwtest_str_eq(buf, expected);
	free(buf);
	return PWTEST_PASS;
}

PWTEST(diag_peer_dispatch_null_safe)
{
	rt_diag_peer_dispatch_init(NULL);
	rt_diag_peer_dispatch_reset(NULL);
	rt_diag_peer_dispatch_render_text(NULL, stderr);
	return PWTEST_PASS;
}

PWTEST(diag_mbpta_state_names_stable)
{
	pwtest_str_eq(rt_diag_mbpta_state_name(RT_DIAG_MBPTA_INSUFFICIENT_DATA),
		      "insufficient_data");
	pwtest_str_eq(rt_diag_mbpta_state_name(RT_DIAG_MBPTA_IID_PENDING),
		      "iid_pending");
	pwtest_str_eq(rt_diag_mbpta_state_name(RT_DIAG_MBPTA_NON_GUMBEL),
		      "non_gumbel");
	pwtest_str_eq(rt_diag_mbpta_state_name(RT_DIAG_MBPTA_PENDING_CONVERGENCE),
		      "pending_convergence");
	pwtest_str_eq(rt_diag_mbpta_state_name(RT_DIAG_MBPTA_PWCET_VALID),
		      "pwcet_valid");
	pwtest_str_eq(rt_diag_mbpta_state_name(RT_DIAG_MBPTA_DRIFT), "drift");
	pwtest_str_eq(rt_diag_mbpta_state_name((enum rt_diag_mbpta_state)999),
		      "unknown");
	return PWTEST_PASS;
}

PWTEST(diag_budget_kind_names_stable)
{
	pwtest_str_eq(rt_diag_budget_kind_name(RT_DIAG_BUDGET_DETERMINISTIC_WCET),
		      "deterministic_wcet");
	pwtest_str_eq(rt_diag_budget_kind_name(RT_DIAG_BUDGET_PWCET),
		      "pwcet");
	pwtest_str_eq(rt_diag_budget_kind_name(RT_DIAG_BUDGET_EMPIRICAL_QUANTILE),
		      "empirical_quantile");
	pwtest_str_eq(rt_diag_budget_kind_name(RT_DIAG_BUDGET_BOOTSTRAP_FALLBACK),
		      "bootstrap_fallback");
	pwtest_str_eq(rt_diag_budget_kind_name(RT_DIAG_BUDGET_MANUAL_OVERRIDE),
		      "manual_override");
	pwtest_str_eq(rt_diag_budget_kind_name((enum rt_diag_budget_kind)999),
		      "unknown");
	return PWTEST_PASS;
}

PWTEST(diag_json_escapes_strings)
{
	struct rt_diag_raw_snapshot raw;
	struct rt_diag_combined c = { 0 };
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;

	/* Cover every escape branch: quote, backslash, control char,
	 * embedded newline. */
	rt_diag_raw_snapshot_init(&raw);
	struct rt_diag_raw_node rn = { 0 };
	rn.id = 1;
	/* "weird\name\twith\"quotes" */
	strcpy(rn.name, "a\"b\\c\td\ne");
	pwtest_int_eq(rt_diag_raw_snapshot_add_node(&raw, &rn), 0);

	c.raw = &raw;

	fp = open_memstream(&buf, &len);
	pwtest_ptr_notnull(fp);
	rt_diag_render_json(&c, fp);
	fclose(fp);

	/* Spot-check the escaped name field. */
	pwtest_bool_true(strstr(buf, "\"name\":\"a\\\"b\\\\c\\td\\ne\"") != NULL);

	free(buf);
	rt_diag_raw_snapshot_fini(&raw);
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
	pwtest_add(diag_fusion_null_safe, PWTEST_NOARG);
	pwtest_add(diag_fusion_begin_group_validates_pair, PWTEST_NOARG);
	pwtest_add(diag_fusion_add_member_out_of_range, PWTEST_NOARG);
	pwtest_add(diag_fusion_verdict_and_reason_names, PWTEST_NOARG);
	pwtest_add(diag_fusion_render_text_golden, PWTEST_NOARG);
	pwtest_add(diag_params_init_zeroes, PWTEST_NOARG);
	pwtest_add(diag_params_null_safe, PWTEST_NOARG);
	pwtest_add(diag_params_render_text_golden, PWTEST_NOARG);
	pwtest_add(diag_json_null_inputs, PWTEST_NOARG);
	pwtest_add(diag_json_empty_combined, PWTEST_NOARG);
	pwtest_add(diag_json_full_golden, PWTEST_NOARG);
	pwtest_add(diag_json_escapes_strings, PWTEST_NOARG);
	pwtest_add(diag_peer_dispatch_render_text, PWTEST_NOARG);
	pwtest_add(diag_peer_dispatch_null_safe, PWTEST_NOARG);
	pwtest_add(diag_budget_kind_names_stable, PWTEST_NOARG);
	pwtest_add(diag_mbpta_state_names_stable, PWTEST_NOARG);

	return PWTEST_PASS;
}
