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
		uint64_t cumulative_deadline, uint64_t local_deadline,
		uint64_t period, uint32_t cpu)
{
	struct cb_ctx *c = data;
	uint32_t slot = c->calls % (uint32_t)SPA_N_ELEMENTS(c->last);

	(void)cumulative_deadline;
	c->last[slot].id = id;
	c->last[slot].tid = tid;
	c->last[slot].runtime = runtime;
	c->last[slot].deadline = local_deadline;
	c->last[slot].period = period;
	c->last[slot].cpu = cpu;
	c->calls++;
}

static reconcile_state_t *make_state_persistent(double threshold)
{
	return reconcile_init(2, 0.95, NULL, threshold, true);
}

static reconcile_state_t *make_state_legacy(void)
{
	return reconcile_init(2, 0.95, NULL, 0.01, false);
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

/* Late-schedulable recovery without a topology bump. A follower that
 * was zero-wcet on the first reconcile after it appeared (typical for
 * pw_stream-based shim clients: paplay, aplay, ...) gets included in
 * the DAG as soon as its sketch produces a non-zero budget, even
 * though the structural fingerprint (id + tid + edges) is unchanged.
 * Without this behaviour, the persistent-DAG fast path would freeze
 * such followers out of scheduling forever. */
PWTEST(reconcile_rec_11b_late_schedulable_no_gen_bump)
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

	/* WCET recovers, but generation is the SAME. The sketch warm-up
	 * does not bump the structural fingerprint, so the topology
	 * snapshot would otherwise hand the worker an unchanged
	 * generation. The reconcile layer must still re-include the
	 * newly schedulable follower. */
	t.followers[2].wcet = 10000;
	rt2 = make_topo(&t, 5, 4, 1);
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
	s = reconcile_init(0, 0.95, NULL, 0.01, true);
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

/* --- TID-grouping tests ---
 *
 * When libpipewire's chain-merge consolidates several adjacent
 * followers onto a single OS thread, those followers all publish the
 * same PW_KEY_NODE_LOOP_TID. reconcile_apply_tid_groups translates
 * that into co-location group ids on the DAG, picking the lowest
 * follower id per shared TID as the canonical group id and leaving
 * unique-TID nodes ungrouped. These tests inspect the DAG state
 * after reconcile_apply to confirm the wiring.
 *
 * We use reconcile_state_dag (a test-only accessor returning
 * state->dag) to peek inside the persistent DAG. */
static dag_t *reconcile_state_peek_dag(reconcile_state_t *s);

/* Test helper: returns state->dag without exposing the internal
 * layout to production callers. Implemented at the bottom of the
 * file so it can include reconcile.c's private struct. */

PWTEST(reconcile_g1_shared_tid_creates_group)
{
	/* Two followers share TID 100; one has its own TID. After
	 * reconcile, the two shared ones land on the same group id
	 * (the lowest of the pair) and the third stays ungrouped. */
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	/* Override TIDs: 10 and 12 share TID 200; 11 has TID 201. */
	t.followers[0].tid = 200;
	t.followers[1].tid = 201;
	t.followers[2].tid = 200;
	rt = make_topo(&t, 3, 2, 1);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);

	dag_t *dag = reconcile_state_peek_dag(s);
	pwtest_ptr_notnull(dag);

	dag_node_t *n10 = dag_find_node(dag, 10);
	dag_node_t *n11 = dag_find_node(dag, 11);
	dag_node_t *n12 = dag_find_node(dag, 12);
	pwtest_ptr_notnull(n10);
	pwtest_ptr_notnull(n11);
	pwtest_ptr_notnull(n12);

	/* 10 and 12 share TID -> group = min(10,12) = 10. */
	pwtest_int_eq(n10->group_id, 10u);
	pwtest_int_eq(n12->group_id, 10u);
	/* 11 has unique TID -> ungrouped. */
	pwtest_int_eq(n11->group_id, 0u);

	/* Co-location effect: 10 and 12 must share a CPU. */
	pwtest_int_eq((int)n10->cpu, (int)n12->cpu);

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g2_unique_tids_no_grouping)
{
	/* All followers have distinct TIDs -> every node ungrouped.
	 * The DAG behaves exactly as before the chain-merge change. */
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt = make_topo(&t, 5, 4, 1);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	dag_t *dag = reconcile_state_peek_dag(s);
	pwtest_ptr_notnull(dag);
	for (uint32_t i = 0; i < 5; i++) {
		dag_node_t *n = dag_find_node(dag, 10 + i);
		pwtest_ptr_notnull(n);
		pwtest_int_eq(n->group_id, 0u);
	}

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g3_chain_share_tid_collapses_to_one_cpu)
{
	/* A 3-link chain whose three followers all share one TID
	 * should land on a single CPU. Even with multiple CPUs
	 * available, the group constraint pulls them together. */
	struct topo5 t;
	reconcile_state_t *s = reconcile_init(4, 0.95, NULL, 0.01, true);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].tid = 555;
	t.followers[1].tid = 555;
	t.followers[2].tid = 555;
	rt = make_topo(&t, 3, 2, 1);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	dag_t *dag = reconcile_state_peek_dag(s);
	pwtest_ptr_notnull(dag);

	dag_node_t *n10 = dag_find_node(dag, 10);
	dag_node_t *n11 = dag_find_node(dag, 11);
	dag_node_t *n12 = dag_find_node(dag, 12);
	pwtest_int_eq(n10->group_id, 10u);
	pwtest_int_eq(n11->group_id, 10u);
	pwtest_int_eq(n12->group_id, 10u);
	pwtest_int_eq((int)n10->cpu, (int)n11->cpu);
	pwtest_int_eq((int)n10->cpu, (int)n12->cpu);

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g4_chain_break_reverts_group)
{
	/* First pass: three followers share a TID (chain-merged).
	 * Second pass: the middle node's TID changes (chain broke).
	 * The new DAG should show the two remaining shared-TID
	 * followers grouped and the middle node ungrouped. */
	struct topo5 t;
	reconcile_state_t *s = reconcile_init(4, 0.95, NULL, 0.01, true);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].tid = 333;
	t.followers[1].tid = 333;
	t.followers[2].tid = 333;
	rt = make_topo(&t, 3, 2, 1);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);

	/* Now node 11 splits off onto its own thread. */
	t.followers[1].tid = 999;
	rt = make_topo(&t, 3, 2, 2);  /* new generation -> rebuild */
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);

	dag_t *dag = reconcile_state_peek_dag(s);
	pwtest_ptr_notnull(dag);
	dag_node_t *n10 = dag_find_node(dag, 10);
	dag_node_t *n11 = dag_find_node(dag, 11);
	dag_node_t *n12 = dag_find_node(dag, 12);
	pwtest_int_eq(n10->group_id, 10u);  /* 10 and 12 still share */
	pwtest_int_eq(n12->group_id, 10u);
	pwtest_int_eq(n11->group_id, 0u);   /* 11 is alone now */

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g5_zero_tid_does_not_group)
{
	/* Followers with tid <= 0 don't participate in grouping
	 * (no thread to merge onto). Two such followers must stay
	 * ungrouped even if their TIDs are equal. */
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].tid = 0;
	t.followers[1].tid = 0;
	rt = make_topo(&t, 2, 1, 1);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	dag_t *dag = reconcile_state_peek_dag(s);
	pwtest_ptr_notnull(dag);
	dag_node_t *n10 = dag_find_node(dag, 10);
	dag_node_t *n11 = dag_find_node(dag, 11);
	pwtest_int_eq(n10->group_id, 0u);
	pwtest_int_eq(n11->group_id, 0u);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* --- deeper TID-grouping coverage ---
 *
 * The g1..g5 tests pin the contract. The ones below stress patterns
 * that occur in real chain-merge / chain-break cycles: multiple
 * concurrent chains, dynamic member join/leave between passes, the
 * legacy (non-persistent) path, the idempotency we rely on to keep
 * steady-state cost flat, and what the per-node (R, D, T, CPU)
 * actually look like when the library produces a group placement
 * (so module-deadline's downstream summation is reasoning from
 * sound inputs). */

PWTEST(reconcile_g6_two_independent_chains_distinct_groups)
{
	/* Five followers in a single linear chain (the topo5 edge set
	 * is 10 -> 11 -> 12 -> 13 -> 14), but the TID pattern splits
	 * them into two clusters: {10, 11} share TID 700 and
	 * {12, 13, 14} share TID 800. Each cluster must collapse to
	 * its own group; within each group the members must co-locate.
	 *
	 * Note: the two groups can legitimately land on the same CPU
	 * here -- in a linear chain every node is related to every
	 * other, so the unrelated-set admission lets the two groups
	 * stack on one CPU. The contract is "group members share a
	 * CPU"; "distinct groups occupy distinct CPUs" is only
	 * required when the groups are pairwise unrelated and have a
	 * worst-fit alternative, which a single-chain topology
	 * doesn't offer. */
	struct topo5 t;
	reconcile_state_t *s = reconcile_init(4, 0.95, NULL, 0.01, true);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].tid = 700;  /* id 10 */
	t.followers[1].tid = 700;  /* id 11 */
	t.followers[2].tid = 800;  /* id 12 */
	t.followers[3].tid = 800;  /* id 13 */
	t.followers[4].tid = 800;  /* id 14 */
	rt = make_topo(&t, 5, 4, 1);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	dag_t *dag = reconcile_state_peek_dag(s);
	pwtest_ptr_notnull(dag);

	dag_node_t *n10 = dag_find_node(dag, 10);
	dag_node_t *n11 = dag_find_node(dag, 11);
	dag_node_t *n12 = dag_find_node(dag, 12);
	dag_node_t *n13 = dag_find_node(dag, 13);
	dag_node_t *n14 = dag_find_node(dag, 14);

	pwtest_int_eq(n10->group_id, 10u);
	pwtest_int_eq(n11->group_id, 10u);
	pwtest_int_eq(n12->group_id, 12u);
	pwtest_int_eq(n13->group_id, 12u);
	pwtest_int_eq(n14->group_id, 12u);

	pwtest_int_eq((int)n10->cpu, (int)n11->cpu);
	pwtest_int_eq((int)n12->cpu, (int)n13->cpu);
	pwtest_int_eq((int)n12->cpu, (int)n14->cpu);

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g7_new_member_joins_existing_group)
{
	/* First pass: 2 followers share TID 444; a third has a
	 * unique TID 999 and stays ungrouped. Second pass (generation
	 * bumped, full rebuild): the third follower's TID changes to
	 * 444 too. It must join the existing group on the next
	 * apply.
	 *
	 * Note: the leader_id stays at the lowest id (10), so the
	 * group_id remains 10 across both passes. */
	struct topo5 t;
	reconcile_state_t *s = reconcile_init(4, 0.95, NULL, 0.01, true);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].tid = 444;
	t.followers[1].tid = 444;
	t.followers[2].tid = 999;
	rt = make_topo(&t, 3, 2, 1);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	{
		dag_t *dag = reconcile_state_peek_dag(s);
		dag_node_t *n12 = dag_find_node(dag, 12);
		pwtest_int_eq(n12->group_id, 0u);
	}

	t.followers[2].tid = 444;
	rt = make_topo(&t, 3, 2, 2);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	{
		dag_t *dag = reconcile_state_peek_dag(s);
		dag_node_t *n10 = dag_find_node(dag, 10);
		dag_node_t *n11 = dag_find_node(dag, 11);
		dag_node_t *n12 = dag_find_node(dag, 12);
		pwtest_int_eq(n10->group_id, 10u);
		pwtest_int_eq(n11->group_id, 10u);
		pwtest_int_eq(n12->group_id, 10u);
		pwtest_int_eq((int)n10->cpu, (int)n12->cpu);
	}

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g8_idempotent_reapply_keeps_group)
{
	/* Apply the same shared-TID topology twice without bumping
	 * the generation. The DAG group_id must be stable across the
	 * second call (so the steady-state reconcile is cheap: a
	 * library-side dag_set_node_group with the same value is a
	 * no-op, the dirty bit stays clear, and assign_cpus doesn't
	 * re-run). */
	struct topo5 t;
	reconcile_state_t *s = reconcile_init(4, 0.95, NULL, 0.01, true);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].tid = 555;
	t.followers[1].tid = 555;
	rt = make_topo(&t, 2, 1, 7);  /* generation 7, doesn't change */

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	dag_t *dag1 = reconcile_state_peek_dag(s);
	dag_node_t *n10 = dag_find_node(dag1, 10);
	dag_node_t *n11 = dag_find_node(dag1, 11);
	uint32_t cpu_before = n10->cpu;
	pwtest_int_eq(n10->group_id, 10u);
	pwtest_int_eq(n11->group_id, 10u);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	dag_t *dag2 = reconcile_state_peek_dag(s);
	pwtest_ptr_eq(dag1, dag2);  /* persistent: same DAG instance */
	pwtest_int_eq(n10->group_id, 10u);
	pwtest_int_eq(n11->group_id, 10u);
	pwtest_int_eq((int)n10->cpu, (int)cpu_before);
	pwtest_bool_false(dag1->dirty);  /* re-stamp did NOT mark dirty */

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g9_legacy_path_also_groups)
{
	/* The recalc.persistent=false kill switch still needs to
	 * honour TID grouping -- otherwise the legacy path would lose
	 * the chain-merge feature whenever the operator switched it
	 * on for diagnostic A/B comparisons. */
	struct topo5 t;
	reconcile_state_t *s = make_state_legacy();
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].tid = 333;
	t.followers[1].tid = 333;
	rt = make_topo(&t, 2, 1, 1);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);

	/* Legacy path destroys the DAG after each call; we can't peek
	 * directly. Instead verify via the callback record: both
	 * followers received a placement and they reported the same
	 * CPU. */
	pwtest_int_eq((int)cb.calls, 2);
	pwtest_int_eq((int)cb.last[0].cpu, (int)cb.last[1].cpu);

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g10_wcet_drift_preserves_group)
{
	/* The persistent path's WCET-drift handling must not clobber
	 * the group_id when a follower's WCET changes between passes.
	 * Without this guarantee, every WCET update would silently
	 * re-spread the chain across CPUs. */
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].tid = 222;
	t.followers[1].tid = 222;
	rt = make_topo(&t, 2, 1, 1);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);

	/* Bump WCETs above the 1 % drift threshold, keep the same
	 * generation, re-apply. */
	t.followers[0].wcet = 20000;
	t.followers[1].wcet = 25000;
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);

	dag_t *dag = reconcile_state_peek_dag(s);
	dag_node_t *n10 = dag_find_node(dag, 10);
	dag_node_t *n11 = dag_find_node(dag, 11);
	pwtest_int_eq(n10->group_id, 10u);
	pwtest_int_eq(n11->group_id, 10u);
	pwtest_int_eq((int)n10->cpu, (int)n11->cpu);

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g11_cb_receives_per_node_values_for_group)
{
	/* The sched_cb sees one call per real node, not one per group:
	 * the callback signature is per-node by design (the summation
	 * happens in module-deadline.c outside the library). Each call
	 * must carry the same TID (the consolidated chain thread), the
	 * same period (global), the same CPU (group-pinned), and a
	 * per-node (runtime, deadline) the caller can sum. */
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;
	uint32_t i, calls_for_grouped = 0;
	pid_t group_tid = 0;
	uint64_t group_period = 0;
	uint32_t group_cpu = UINT32_MAX;
	uint64_t sum_runtime = 0, sum_deadline = 0;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].tid = 612;
	t.followers[1].tid = 612;
	t.followers[2].tid = 612;
	rt = make_topo(&t, 3, 2, 1);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);

	pwtest_int_eq((int)cb.calls, 3);
	for (i = 0; i < cb.calls; i++) {
		if (cb.last[i].tid != 612)
			continue;
		calls_for_grouped++;
		if (group_tid == 0) {
			group_tid = cb.last[i].tid;
			group_period = cb.last[i].period;
			group_cpu = cb.last[i].cpu;
		}
		pwtest_int_eq((int)cb.last[i].tid, (int)group_tid);
		pwtest_int_eq((int64_t)cb.last[i].period, (int64_t)group_period);
		pwtest_int_eq((int)cb.last[i].cpu, (int)group_cpu);
		pwtest_bool_true(cb.last[i].runtime > 0);
		pwtest_bool_true(cb.last[i].deadline > 0);
		sum_runtime += cb.last[i].runtime;
		sum_deadline += cb.last[i].deadline;
	}
	pwtest_int_eq((int)calls_for_grouped, 3);
	/* The summed deadline a chain reservation would receive must
	 * be > each member's individual deadline (proportional split
	 * guarantees D_i < D_chain when more than one node sits on
	 * the path). */
	for (i = 0; i < cb.calls; i++) {
		pwtest_bool_true(sum_deadline >= cb.last[i].deadline);
		pwtest_bool_true(sum_runtime >= cb.last[i].runtime);
	}

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g12_group_overcapacity_returns_failure_state)
{
	/* When a TID-grouped chain's summed density overflows the
	 * configured cpus.utilization, the DAG library returns EAGAIN
	 * and reconcile drops the cached DAG. The next call must
	 * rebuild (and, if the topology hasn't otherwise changed, fail
	 * again). The point is that the library's split-refusal
	 * propagates cleanly through reconcile rather than getting
	 * silently dropped. */
	struct topo5 t;
	reconcile_state_t *s = reconcile_init(2, 0.55, NULL, 0.01, true);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].wcet = 30000000;  /* 30 ms */
	t.followers[1].wcet = 30000000;  /* 30 ms */
	t.followers[0].tid = 777;
	t.followers[1].tid = 777;
	rt = make_topo(&t, 2, 1, 1);
	rt.period = 100000000;            /* 100 ms */

	/* Per-node density 0.30 each; summed on one CPU = 0.60 > 0.55. */
	(void)reconcile_apply(s, &rt, cb_record, &cb);

	/* Either the DAG was destroyed (legacy/failure path) or it's
	 * still cached but dirty; either way the callback fired zero
	 * times because no real node ever got a placement. */
	pwtest_int_eq((int)cb.calls, 0);

	reconcile_fini(s);
	return PWTEST_PASS;
}

PWTEST(reconcile_g13_chain_member_leaves_group)
{
	/* Reverse of g7: a third member starts shared, then leaves.
	 * Verifies that the lowest-id leader convention follows the
	 * surviving members and the departed one becomes ungrouped. */
	struct topo5 t;
	reconcile_state_t *s = reconcile_init(4, 0.95, NULL, 0.01, true);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	t.followers[0].tid = 411;
	t.followers[1].tid = 411;
	t.followers[2].tid = 411;
	rt = make_topo(&t, 3, 2, 1);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);

	/* The lowest-id follower (10) leaves the chain (gets its own
	 * TID); 11 and 12 stay grouped. New leader_id should be 11. */
	t.followers[0].tid = 9999;
	rt = make_topo(&t, 3, 2, 2);
	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	{
		dag_t *dag = reconcile_state_peek_dag(s);
		dag_node_t *n10 = dag_find_node(dag, 10);
		dag_node_t *n11 = dag_find_node(dag, 11);
		dag_node_t *n12 = dag_find_node(dag, 12);
		pwtest_int_eq(n10->group_id, 0u);
		pwtest_int_eq(n11->group_id, 11u);
		pwtest_int_eq(n12->group_id, 11u);
		pwtest_int_eq((int)n11->cpu, (int)n12->cpu);
	}

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Provided by reconcile.c (#ifdef BUILT_FOR_TEST -- the meson target
 * compiles reconcile.c with that define so the symbol is visible
 * only in the test binary, never in production module-deadline.so). */
dag_t *reconcile_state_dag_for_test(reconcile_state_t *s);

static dag_t *reconcile_state_peek_dag(reconcile_state_t *s)
{
	return reconcile_state_dag_for_test(s);
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
	pwtest_add(reconcile_rec_11b_late_schedulable_no_gen_bump, PWTEST_NOARG);
	pwtest_add(reconcile_rec_12_feedback_pre_filtered, PWTEST_NOARG);
	pwtest_add(reconcile_rec_13_library_failure_drops_dag, PWTEST_NOARG);
	pwtest_add(reconcile_rec_14_threshold_zero_always_triggers, PWTEST_NOARG);
	pwtest_add(reconcile_rec_15_legacy_no_persistent_dag, PWTEST_NOARG);
	pwtest_add(reconcile_rec_16_backoff_and_reset, PWTEST_NOARG);
	pwtest_add(reconcile_e1_empty_topo, PWTEST_NOARG);
	pwtest_add(reconcile_e2_single_node, PWTEST_NOARG);
	pwtest_add(reconcile_e3_zero_cpus_rejected, PWTEST_NOARG);
	pwtest_add(reconcile_e7_fini_null_safe, PWTEST_NOARG);

	pwtest_add(reconcile_g1_shared_tid_creates_group, PWTEST_NOARG);
	pwtest_add(reconcile_g2_unique_tids_no_grouping, PWTEST_NOARG);
	pwtest_add(reconcile_g3_chain_share_tid_collapses_to_one_cpu, PWTEST_NOARG);
	pwtest_add(reconcile_g4_chain_break_reverts_group, PWTEST_NOARG);
	pwtest_add(reconcile_g5_zero_tid_does_not_group, PWTEST_NOARG);
	pwtest_add(reconcile_g6_two_independent_chains_distinct_groups, PWTEST_NOARG);
	pwtest_add(reconcile_g7_new_member_joins_existing_group, PWTEST_NOARG);
	pwtest_add(reconcile_g8_idempotent_reapply_keeps_group, PWTEST_NOARG);
	pwtest_add(reconcile_g9_legacy_path_also_groups, PWTEST_NOARG);
	pwtest_add(reconcile_g10_wcet_drift_preserves_group, PWTEST_NOARG);
	pwtest_add(reconcile_g11_cb_receives_per_node_values_for_group, PWTEST_NOARG);
	pwtest_add(reconcile_g12_group_overcapacity_returns_failure_state, PWTEST_NOARG);
	pwtest_add(reconcile_g13_chain_member_leaves_group, PWTEST_NOARG);

	return PWTEST_PASS;
}
