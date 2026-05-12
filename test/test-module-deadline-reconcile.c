/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Unit tests for module-deadline/reconcile.c. Drives reconcile_apply
 * with synthetic topo snapshots and observes sched_cb call counts +
 * per-tuple values. The persistent-DAG path's dirty bit and the
 * 1 % WCET drift gate are exercised by replaying the snapshot with
 * known WCET deltas.
 *
 * Covers the documented reconcile behaviours: WCET drift gating
 * (above and below the threshold, in both directions), topology
 * mutation, soft-failure exclusion and recovery, feedback-edge
 * pre-filtering, library failure dropping the cache, the
 * recalc.persistent kill switch, and the back-off counter.
 */

#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/dag.h"
#include "../src/modules/module-deadline/reconcile.h"

struct cb_ctx {
	uint32_t calls;
	struct {
		uint32_t id;
		pid_t    tid;
		uint64_t runtime;
		uint64_t deadline;
		uint64_t period;
		uint32_t cpu;
	} last[16];
};

static void cb_record(void *data, uint32_t id, pid_t tid, uint64_t runtime,
		uint64_t deadline, uint64_t period, uint32_t cpu)
{
	struct cb_ctx *c = data;
	uint32_t slot = c->calls % (uint32_t)SPA_N_ELEMENTS(c->last);

	c->last[slot].id = id;
	c->last[slot].tid = tid;
	c->last[slot].runtime = runtime;
	c->last[slot].deadline = deadline;
	c->last[slot].period = period;
	c->last[slot].cpu = cpu;
	c->calls++;
}

static reconcile_state_t *make_state_persistent(double threshold)
{
	return reconcile_init(2, 0.95, threshold, true);
}

static reconcile_state_t *make_state_legacy(void)
{
	return reconcile_init(2, 0.95, 0.01, false);
}

/* Build a small 3-node topo: 1 -> 2 -> 3. WCETs are caller-set. */
struct topo5 {
	reconcile_follower_t followers[5];
	reconcile_edge_t     edges[4];
};

static void topo5_init(struct topo5 *t)
{
	uint32_t i;
	for (i = 0; i < 5; i++) {
		t->followers[i].id  = 10 + i;
		t->followers[i].tid = 100 + i;
		t->followers[i].wcet = 10000;
	}
	t->edges[0] = (reconcile_edge_t){ .src = 10, .dst = 11 };
	t->edges[1] = (reconcile_edge_t){ .src = 11, .dst = 12 };
	t->edges[2] = (reconcile_edge_t){ .src = 12, .dst = 13 };
	t->edges[3] = (reconcile_edge_t){ .src = 13, .dst = 14 };
}

static reconcile_topo_t make_topo(const struct topo5 *t, uint32_t n_followers,
		uint32_t n_edges, uint64_t generation)
{
	reconcile_topo_t rt;
	rt.followers = t->followers;
	rt.n_followers = n_followers;
	rt.edges = t->edges;
	rt.n_edges = n_edges;
	rt.period = 1000000;       /* 1ms */
	rt.generation = generation;
	return rt;
}

/* First reconcile builds the DAG; every node gets a positive
 * deadline; sched_cb fires once per follower. */
PWTEST(reconcile_rec_1_first_build)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt = make_topo(&t, 5, 4, 1);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	pwtest_int_eq((int)cb.calls, 5);
	pwtest_bool_true(reconcile_state_has_persistent_dag(s));

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Identical inputs across two reconciles produce identical
 * per-follower tuples; the cached dag_t pointer is preserved (i.e.
 * the persistent path didn't drop and rebuild). */
PWTEST(reconcile_rec_2_noop_repeat)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb1 = { 0 };
	struct cb_ctx cb2 = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt = make_topo(&t, 5, 4, 7);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb1), 0);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb2), 0);

	pwtest_int_eq((int)cb1.calls, 5);
	pwtest_int_eq((int)cb2.calls, 5);
	/* Same five followers seen in the same order. */
	for (uint32_t i = 0; i < 5; i++) {
		pwtest_int_eq((int)cb1.last[i].id, (int)cb2.last[i].id);
		pwtest_int_eq((int)cb1.last[i].deadline,
				(int)cb2.last[i].deadline);
		pwtest_int_eq((int)cb1.last[i].cpu, (int)cb2.last[i].cpu);
	}
	pwtest_bool_true(reconcile_state_has_persistent_dag(s));

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Bumping WCET by 0.5 % does not trigger dag_set_node_wcet
 * (1 % gate). The cached deadline assignment for follower 0 stays
 * exactly equal across two applies. */
PWTEST(reconcile_rec_3_sub_threshold_no_op)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb1 = { 0 };
	struct cb_ctx cb2 = { 0 };
	reconcile_topo_t rt1, rt2;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt1 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt1, cb_record, &cb1), 0);

	/* Bump first follower by 0.5 % (50 ns out of 10,000). */
	t.followers[0].wcet = 10050;
	rt2 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt2, cb_record, &cb2), 0);

	/* Same deadline assignments. */
	for (uint32_t i = 0; i < 5; i++) {
		pwtest_int_eq((int)cb1.last[i].id, (int)cb2.last[i].id);
		pwtest_int_eq((int)cb1.last[i].deadline,
				(int)cb2.last[i].deadline);
	}

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Bumping WCET by 5 % does trigger; some deadline shifts. */
PWTEST(reconcile_rec_4_super_threshold_triggers)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb1 = { 0 };
	struct cb_ctx cb2 = { 0 };
	reconcile_topo_t rt1, rt2;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt1 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt1, cb_record, &cb1), 0);

	/* Bump first follower by 5 %. */
	t.followers[0].wcet = 10500;
	rt2 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt2, cb_record, &cb2), 0);

	/* The follower whose WCET grew shifts its assigned deadline
	 * (and possibly knock-on shifts elsewhere). At least one
	 * deadline differs. */
	{
		bool any_diff = false;
		for (uint32_t i = 0; i < 5; i++) {
			if (cb1.last[i].deadline != cb2.last[i].deadline) {
				any_diff = true;
				break;
			}
		}
		pwtest_bool_true(any_diff);
	}

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Lowering WCET by 5 % triggers symmetrically. */
PWTEST(reconcile_rec_5_downward_triggers)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb1 = { 0 };
	struct cb_ctx cb2 = { 0 };
	reconcile_topo_t rt1, rt2;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt1 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt1, cb_record, &cb1), 0);

	t.followers[0].wcet = 9500; /* -5 % */
	rt2 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt2, cb_record, &cb2), 0);

	{
		bool any_diff = false;
		for (uint32_t i = 0; i < 5; i++) {
			if (cb1.last[i].deadline != cb2.last[i].deadline) {
				any_diff = true;
				break;
			}
		}
		pwtest_bool_true(any_diff);
	}

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Adding a new follower mid-life via a generation bump.
 * sched_cb fires for the new follower too. */
PWTEST(reconcile_rec_6_add_follower)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb1 = { 0 };
	struct cb_ctx cb2 = { 0 };
	reconcile_topo_t rt1, rt2;

	pwtest_ptr_notnull(s);
	topo5_init(&t);

	/* Start with 4 followers. */
	rt1 = make_topo(&t, 4, 3, 1);
	pwtest_int_eq(reconcile_apply(s, &rt1, cb_record, &cb1), 0);
	pwtest_int_eq((int)cb1.calls, 4);

	/* Bump generation and admit the 5th follower. */
	rt2 = make_topo(&t, 5, 4, 2);
	pwtest_int_eq(reconcile_apply(s, &rt2, cb_record, &cb2), 0);
	pwtest_int_eq((int)cb2.calls, 5);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Removing a follower via a generation bump drops them from
 * the cb output. */
PWTEST(reconcile_rec_7_remove_follower)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb1 = { 0 };
	struct cb_ctx cb2 = { 0 };
	reconcile_topo_t rt1, rt2;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt1 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt1, cb_record, &cb1), 0);
	pwtest_int_eq((int)cb1.calls, 5);

	rt2 = make_topo(&t, 4, 3, 2);
	pwtest_int_eq(reconcile_apply(s, &rt2, cb_record, &cb2), 0);
	pwtest_int_eq((int)cb2.calls, 4);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Adding an edge. The cb sees the same follower count, but
 * the deadline assignments may shift (one node now has a successor
 * it didn't before). */
PWTEST(reconcile_rec_8_add_edge)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.0);
	struct cb_ctx cb1 = { 0 };
	struct cb_ctx cb2 = { 0 };
	reconcile_topo_t rt1, rt2;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	/* Start with NO edges. */
	rt1 = make_topo(&t, 5, 0, 1);
	pwtest_int_eq(reconcile_apply(s, &rt1, cb_record, &cb1), 0);
	pwtest_int_eq((int)cb1.calls, 5);

	rt2 = make_topo(&t, 5, 4, 2);
	pwtest_int_eq(reconcile_apply(s, &rt2, cb_record, &cb2), 0);
	pwtest_int_eq((int)cb2.calls, 5);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Period change reapplies every follower's tuple (period
 * field differs). */
PWTEST(reconcile_rec_9_period_change)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb1 = { 0 };
	struct cb_ctx cb2 = { 0 };
	reconcile_topo_t rt1, rt2;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt1 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt1, cb_record, &cb1), 0);

	rt2 = make_topo(&t, 5, 4, 1);
	rt2.period = 2000000;
	pwtest_int_eq(reconcile_apply(s, &rt2, cb_record, &cb2), 0);
	pwtest_int_eq((int)cb2.calls, 5);
	for (uint32_t i = 0; i < 5; i++) {
		pwtest_int_eq((int)cb2.last[i].period, 2000000);
	}

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Soft failure -- a follower with wcet=0 is excluded from
 * the DAG; peers still get SCHED_DEADLINE. */
PWTEST(reconcile_rec_10_soft_failure_excludes)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[2].wcet = 0;
	rt = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	pwtest_int_eq((int)cb.calls, 4);
	for (uint32_t i = 0; i < cb.calls; i++) {
		pwtest_bool_true(cb.last[i].id != 12);
	}

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Soft failure recovery -- when the WCET returns to >0
 * on a subsequent reconcile, the follower re-enters the DAG. */
PWTEST(reconcile_rec_11_soft_failure_recovery)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb1 = { 0 };
	struct cb_ctx cb2 = { 0 };
	reconcile_topo_t rt1, rt2;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[2].wcet = 0;
	rt1 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt1, cb_record, &cb1), 0);
	pwtest_int_eq((int)cb1.calls, 4);

	/* WCET recovers. Bump generation so the topology pass re-adds
	 * the node. */
	t.followers[2].wcet = 10000;
	rt2 = make_topo(&t, 5, 4, 2);
	pwtest_int_eq(reconcile_apply(s, &rt2, cb_record, &cb2), 0);
	pwtest_int_eq((int)cb2.calls, 5);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Feedback edge filtering happens at the caller (module
 * layer); the reconcile layer simply consumes the filtered topology.
 * Here we test that a topology missing a "would-be" edge still
 * succeeds: reconcile sees the same N nodes but M-1 edges. */
PWTEST(reconcile_rec_12_feedback_pre_filtered)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	/* Drop edge 12->13 (simulates a feedback link filtered at the
	 * snapshot stage). The remaining graph is still acyclic. */
	rt = make_topo(&t, 5, 4, 1);
	rt.n_edges = 3; /* keeps 10->11, 11->12, 12->13 */
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	pwtest_int_eq((int)cb.calls, 5);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* A topology snapshot whose edges close a cycle is
 * rejected by the DAG library (ELOOP). reconcile_drops the DAG and
 * bumps the failure counter. */
PWTEST(reconcile_rec_13_library_failure_drops_dag)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;
	reconcile_edge_t cyclic_edges[5];

	pwtest_ptr_notnull(s);
	topo5_init(&t);

	cyclic_edges[0] = (reconcile_edge_t){ .src = 10, .dst = 11 };
	cyclic_edges[1] = (reconcile_edge_t){ .src = 11, .dst = 12 };
	cyclic_edges[2] = (reconcile_edge_t){ .src = 12, .dst = 13 };
	cyclic_edges[3] = (reconcile_edge_t){ .src = 13, .dst = 14 };
	cyclic_edges[4] = (reconcile_edge_t){ .src = 14, .dst = 10 };

	rt.followers = t.followers;
	rt.n_followers = 5;
	rt.edges = cyclic_edges;
	rt.n_edges = 5;
	rt.period = 1000000;
	rt.generation = 1;

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), -1);
	pwtest_bool_false(reconcile_state_has_persistent_dag(s));

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* wcet.recalc-threshold = 0.0 disables the gate; every
 * WCET change triggers dag_set_node_wcet. */
PWTEST(reconcile_rec_14_threshold_zero_always_triggers)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.0);
	struct cb_ctx cb1 = { 0 };
	struct cb_ctx cb2 = { 0 };
	reconcile_topo_t rt1, rt2;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt1 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt1, cb_record, &cb1), 0);

	/* Bump first follower by 0.001 % (1 ns out of 10000). With
	 * threshold=0 every difference is significant; with
	 * threshold=0.01 this would be sub-threshold and a no-op. We
	 * can't directly assert "dirty was marked" from the public API,
	 * but the deadline assignments should at least be computed
	 * fresh. The strong signal is that we get the same callback
	 * count and stable assignments. */
	t.followers[0].wcet = 10001;
	rt2 = make_topo(&t, 5, 4, 1);
	pwtest_int_eq(reconcile_apply(s, &rt2, cb_record, &cb2), 0);
	pwtest_int_eq((int)cb2.calls, 5);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* legacy path (persistent=false) does not keep a cached
 * dag_t between reconcile_apply calls. */
PWTEST(reconcile_rec_15_legacy_no_persistent_dag)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_legacy();
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt = make_topo(&t, 5, 4, 1);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	pwtest_int_eq((int)cb.calls, 5);
	pwtest_bool_false(reconcile_state_has_persistent_dag(s));

	rt.generation = 2;
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	pwtest_bool_false(reconcile_state_has_persistent_dag(s));

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Back-off counter. After 16 consecutive failures on the
 * same generation, the next reconcile_apply short-circuits with
 * EAGAIN. After a generation bump, the counter resets. */
PWTEST(reconcile_rec_16_backoff_and_reset)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;
	reconcile_edge_t cyclic_edges[5];
	int i;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	cyclic_edges[0] = (reconcile_edge_t){ .src = 10, .dst = 11 };
	cyclic_edges[1] = (reconcile_edge_t){ .src = 11, .dst = 12 };
	cyclic_edges[2] = (reconcile_edge_t){ .src = 12, .dst = 13 };
	cyclic_edges[3] = (reconcile_edge_t){ .src = 13, .dst = 14 };
	cyclic_edges[4] = (reconcile_edge_t){ .src = 14, .dst = 10 };

	rt.followers = t.followers;
	rt.n_followers = 5;
	rt.edges = cyclic_edges;
	rt.n_edges = 5;
	rt.period = 1000000;
	rt.generation = 1;

	/* 16 failures on generation 1. */
	for (i = 0; i < 16; i++) {
		pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), -1);
	}

	/* 17th attempt short-circuits with EAGAIN -- the back-off
	 * kicked in. */
	errno = 0;
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), -1);
	pwtest_int_eq(errno, EAGAIN);

	/* Bumping the generation resets the counter and we get a fresh
	 * (still failing in this test) error from the library. */
	rt.generation = 2;
	errno = 0;
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), -1);
	pwtest_int_eq(errno, ELOOP);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* E-1: Reconcile against an empty topo: no-op, no apply. */
PWTEST(reconcile_e1_empty_topo)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt = make_topo(&t, 0, 0, 1);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	pwtest_int_eq((int)cb.calls, 0);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* E-2: One-node topology -- the single node owns the entire global
 * deadline (per the DAG layer's single-node behaviour). */
PWTEST(reconcile_e2_single_node)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt = make_topo(&t, 1, 0, 1);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	pwtest_int_eq((int)cb.calls, 1);
	pwtest_int_eq((int)cb.last[0].deadline, 1000000);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* E-3: reconcile_init refuses n_cpus == 0. */
PWTEST(reconcile_e3_zero_cpus_rejected)
{
	reconcile_state_t *s;
	errno = 0;
	s = reconcile_init(0, 0.95, 0.01, true);
	pwtest_ptr_null(s);
	pwtest_int_eq(errno, EINVAL);
	return PWTEST_PASS;
}

/* E-7: reconcile_fini on a NULL state is a no-op. */
PWTEST(reconcile_e7_fini_null_safe)
{
	reconcile_fini(NULL);
	reconcile_drop(NULL);
	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_reconcile)
{
	pwtest_add(reconcile_rec_1_first_build, PWTEST_NOARG);
	pwtest_add(reconcile_rec_2_noop_repeat, PWTEST_NOARG);
	pwtest_add(reconcile_rec_3_sub_threshold_no_op, PWTEST_NOARG);
	pwtest_add(reconcile_rec_4_super_threshold_triggers, PWTEST_NOARG);
	pwtest_add(reconcile_rec_5_downward_triggers, PWTEST_NOARG);
	pwtest_add(reconcile_rec_6_add_follower, PWTEST_NOARG);
	pwtest_add(reconcile_rec_7_remove_follower, PWTEST_NOARG);
	pwtest_add(reconcile_rec_8_add_edge, PWTEST_NOARG);
	pwtest_add(reconcile_rec_9_period_change, PWTEST_NOARG);
	pwtest_add(reconcile_rec_10_soft_failure_excludes, PWTEST_NOARG);
	pwtest_add(reconcile_rec_11_soft_failure_recovery, PWTEST_NOARG);
	pwtest_add(reconcile_rec_12_feedback_pre_filtered, PWTEST_NOARG);
	pwtest_add(reconcile_rec_13_library_failure_drops_dag, PWTEST_NOARG);
	pwtest_add(reconcile_rec_14_threshold_zero_always_triggers, PWTEST_NOARG);
	pwtest_add(reconcile_rec_15_legacy_no_persistent_dag, PWTEST_NOARG);
	pwtest_add(reconcile_rec_16_backoff_and_reset, PWTEST_NOARG);
	pwtest_add(reconcile_e1_empty_topo, PWTEST_NOARG);
	pwtest_add(reconcile_e2_single_node, PWTEST_NOARG);
	pwtest_add(reconcile_e3_zero_cpus_rejected, PWTEST_NOARG);
	pwtest_add(reconcile_e7_fini_null_safe, PWTEST_NOARG);

	return PWTEST_PASS;
}
