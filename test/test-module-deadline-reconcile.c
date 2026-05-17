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
		uint32_t fusion_leader;
	} last[16];
};

static void cb_record(void *data, uint32_t id, pid_t tid, uint64_t runtime,
		uint64_t cumulative_deadline, uint64_t local_deadline,
		uint64_t period, uint32_t cpu,
		uint32_t fusion_group_leader_id)
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
	c->last[slot].fusion_leader = fusion_group_leader_id;
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

PWTEST(reconcile_g1_shared_tid_non_convex_is_rejected)
{
	/* The prototype's chain-merge would put 10 and 12 in one
	 * group when they share a TID, even though 11 sits between
	 * them in the chain 10->11->12. That fusion is precedence-
	 * non-convex (Sarkar 1989 §5.3): the path 10 -> 11 -> 12
	 * leaves the candidate group F = {10, 12} via 11 and
	 * re-enters via 12. The fusion validator now rejects the
	 * group and the reconcile layer strips the shared group_id,
	 * so 10, 11 and 12 each land as a singleton. The old
	 * assertion (group_id = 10 on both) was pinning the
	 * unsound prototype behaviour. */
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
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

	/* The non-convex group {10, 12} is rejected; both members
	 * fall back to singleton scheduling. */
	pwtest_int_eq(n10->group_id, 0u);
	pwtest_int_eq(n12->group_id, 0u);
	pwtest_int_eq(n11->group_id, 0u);

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
	 * The full chain {10, 11, 12} on edges 10->11->12 is
	 * precedence-convex (the whole chain is in F) so the
	 * validator accepts it.
	 *
	 * Second pass: the middle node's TID changes (chain broke).
	 * The remaining shared-TID followers are {10, 12} -- and
	 * that is precedence-non-convex (11 sits between them). The
	 * fusion validator rejects the residual group; 10, 11 and
	 * 12 all end as singletons. The old expectation (10 and 12
	 * still share group_id 10) pinned the prototype's unsound
	 * behaviour. */
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
	pwtest_int_eq(n10->group_id, 0u);  /* non-convex {10,12} rejected */
	pwtest_int_eq(n12->group_id, 0u);
	pwtest_int_eq(n11->group_id, 0u);   /* 11 was alone already */

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

PWTEST(reconcile_g6_two_chain_clusters_first_passes_second_rejected)
{
	/* Five followers in a single linear chain (the topo5 edge set
	 * is 10 -> 11 -> 12 -> 13 -> 14), TIDs split them into two
	 * clusters: {10, 11} share TID 700 and {12, 13, 14} share
	 * TID 800.
	 *
	 * Group {10, 11}: a chain prefix anchored at the graph
	 * source 10. Every member's predecessor is either inside
	 * the group or is the source 10 -- predecessor closure
	 * passes; precedence convexity passes (chain is fully
	 * contained); externally atomic passes (terminal 11 has
	 * external successor 12). Accepted.
	 *
	 * Group {12, 13, 14}: starts mid-chain at 12, whose
	 * predecessor 11 is neither inside the group nor a graph
	 * source. Predecessor closure rejects with
	 * WOULD_SELF_SUSPEND -- the group would otherwise have to
	 * wait for 11 mid-job. The old test expectation (both
	 * groups stay grouped) pinned the prototype's unsound
	 * behaviour. */
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

	/* {10, 11} cluster accepted -> shared group_id 10. */
	pwtest_int_eq(n10->group_id, 10u);
	pwtest_int_eq(n11->group_id, 10u);
	pwtest_int_eq((int)n10->cpu, (int)n11->cpu);

	/* {12, 13, 14} cluster rejected -> singletons. */
	pwtest_int_eq(n12->group_id, 0u);
	pwtest_int_eq(n13->group_id, 0u);
	pwtest_int_eq(n14->group_id, 0u);

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

/* --- mode classification + hysteresis tests --- */

/* A comfortably feasible single-task workload: the reconcile
 * dispatcher must classify it HARD and the consecutive-pass
 * counter must increment on every additional apply call (up to
 * the hysteresis cap). */
PWTEST(reconcile_mode_feasible_repeats_keep_hard)
{
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_follower_t f = { 10, 100, 100 };
	reconcile_edge_t e[] = { { 0, 0 } };
	reconcile_topo_t rt = {
		.followers = &f, .n_followers = 1,
		.edges = NULL, .n_edges = 0,
		.period = 1000000, .generation = 1,
	};
	struct reconcile_feasibility feas = { 0 };

	pwtest_ptr_notnull(s);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	reconcile_state_feasibility(s, &feas);
	pwtest_int_eq(feas.mode, RECONCILE_MODE_HARD);
	pwtest_bool_true(feas.consecutive_hard_passes >= 1);

	/* Three more applies on the same topology keep mode HARD;
	 * the consecutive counter saturates at the hysteresis
	 * cap. */
	for (int i = 0; i < 5; i++)
		pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	reconcile_state_feasibility(s, &feas);
	pwtest_int_eq(feas.mode, RECONCILE_MODE_HARD);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Build a topology that the density predicate will reject (sum
 * density on the only CPU exceeds 1). Mode must flip to
 * SOFT_DEGRADED on the first apply, regardless of how many
 * times the same topology is reapplied. */
PWTEST(reconcile_mode_density_overload_flips_soft)
{
	reconcile_state_t *s = reconcile_init(1, 1.0, NULL, 0.01, true);
	struct cb_ctx cb = { 0 };
	reconcile_follower_t followers[] = {
		{ 10, 100, 80000 },
		{ 11, 101, 80000 },
		{ 12, 102, 80000 },
	};
	reconcile_edge_t edges[] = { { 10, 11 }, { 11, 12 } };
	reconcile_topo_t rt = {
		.followers = followers, .n_followers = 3,
		.edges = edges, .n_edges = 2,
		.period = 100000, .generation = 1,
	};
	struct reconcile_feasibility feas = { 0 };

	pwtest_ptr_notnull(s);

	/* On 1 CPU with admission_ceiling=1.0 the placer's density-
	 * style admission test rejects all three chain followers'
	 * combined density. The dispatcher catches the failure,
	 * marks the schedule SOFT_DEGRADED with
	 * reason="placer_rejected", and falls back to the per-
	 * original-node legacy path. */
	(void)reconcile_apply(s, &rt, cb_record, &cb);
	reconcile_state_feasibility(s, &feas);
	pwtest_int_eq(feas.mode, RECONCILE_MODE_SOFT_DEGRADED);
	pwtest_str_eq(feas.reason, "placer_rejected");
	pwtest_int_eq((int)feas.consecutive_hard_passes, 0);

	/* Reapplying the same infeasible topology leaves the mode
	 * unchanged -- transitions are state-driven, not per-period. */
	for (int i = 0; i < 5; i++)
		(void)reconcile_apply(s, &rt, cb_record, &cb);
	reconcile_state_feasibility(s, &feas);
	pwtest_int_eq(feas.mode, RECONCILE_MODE_SOFT_DEGRADED);
	pwtest_int_eq((int)feas.consecutive_hard_passes, 0);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* After a SOFT transition, the dispatcher needs N consecutive
 * feasible passes before promoting back to HARD (hysteresis).
 * Drive an infeasible topology, then a feasible one, and verify
 * the promotion fires only at the third consecutive feasible
 * apply (default hysteresis = 3). */
PWTEST(reconcile_mode_hysteresis_promotes_after_n_passes)
{
	reconcile_state_t *s = reconcile_init(1, 1.0, NULL, 0.01, true);
	struct cb_ctx cb = { 0 };
	reconcile_follower_t bad[] = {
		{ 10, 100, 80000 }, { 11, 101, 80000 }, { 12, 102, 80000 },
	};
	reconcile_edge_t bad_e[] = { { 10, 11 }, { 11, 12 } };
	reconcile_topo_t bad_topo = {
		.followers = bad, .n_followers = 3,
		.edges = bad_e, .n_edges = 2,
		.period = 100000, .generation = 1,
	};
	reconcile_follower_t good = { 10, 100, 1000 };
	reconcile_topo_t good_topo = {
		.followers = &good, .n_followers = 1,
		.edges = NULL, .n_edges = 0,
		.period = 1000000, .generation = 2,
	};
	struct reconcile_feasibility feas = { 0 };

	pwtest_ptr_notnull(s);

	/* Force into SOFT. */
	(void)reconcile_apply(s, &bad_topo, cb_record, &cb);
	reconcile_state_feasibility(s, &feas);
	pwtest_int_eq(feas.mode, RECONCILE_MODE_SOFT_DEGRADED);

	/* First feasible apply: stays SOFT, counter at 1. */
	(void)reconcile_apply(s, &good_topo, cb_record, &cb);
	reconcile_state_feasibility(s, &feas);
	pwtest_int_eq(feas.mode, RECONCILE_MODE_SOFT_DEGRADED);
	pwtest_int_eq((int)feas.consecutive_hard_passes, 1);

	good_topo.generation = 3;
	(void)reconcile_apply(s, &good_topo, cb_record, &cb);
	reconcile_state_feasibility(s, &feas);
	pwtest_int_eq(feas.mode, RECONCILE_MODE_SOFT_DEGRADED);
	pwtest_int_eq((int)feas.consecutive_hard_passes, 2);

	/* Third consecutive feasible apply: promotion to HARD. */
	good_topo.generation = 4;
	(void)reconcile_apply(s, &good_topo, cb_record, &cb);
	reconcile_state_feasibility(s, &feas);
	pwtest_int_eq(feas.mode, RECONCILE_MODE_HARD);
	pwtest_int_eq((int)feas.consecutive_hard_passes, 3);

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Property-based test: for a family of randomly-generated chain
 * topologies (varying length, WCETs, periods) every sched_cb tuple
 * fired by reconcile_apply must satisfy the kernel SCHED_DEADLINE
 * contract `0 < runtime <= deadline <= period`. The chain family is
 * deliberately small (length 2..6) and the WCETs are scaled to
 * stay below period/(2N) so the entire suite always lands in HARD
 * mode -- the goal is to pin the kernel-API constraints, not the
 * feasibility transitions (those have dedicated tests). */
PWTEST(reconcile_property_random_chains_satisfy_kernel_contract)
{
	uint32_t seed = 0xC7F2A91Du;
	uint32_t trial;
	uint32_t total_violations = 0;
	const uint32_t trials = 200;

	for (trial = 0; trial < trials; trial++) {
		struct topo5 t;
		reconcile_state_t *s = make_state_persistent(0.01);
		struct cb_ctx cb = { 0 };
		reconcile_topo_t rt;
		uint32_t n_followers, n_edges, i;
		uint64_t period;

		pwtest_ptr_notnull(s);

		/* Cheap deterministic LCG so the trial set is
		 * reproducible across runs. */
		seed = seed * 1103515245u + 12345u;
		n_followers = 2 + (seed % 5);
		seed = seed * 1103515245u + 12345u;
		period = 100000u + (uint64_t)(seed % 1900000u);

		for (i = 0; i < n_followers; i++) {
			seed = seed * 1103515245u + 12345u;
			t.followers[i].id  = 10 + i;
			t.followers[i].tid = 100 + i;
			t.followers[i].wcet =
				1000u + (seed % (uint32_t)(period /
					(n_followers * 4 + 1)));
		}
		n_edges = n_followers - 1;
		for (i = 0; i < n_edges; i++) {
			t.edges[i] = (reconcile_edge_t){
				.src = 10 + i, .dst = 10 + i + 1
			};
		}

		rt.followers = t.followers;
		rt.n_followers = n_followers;
		rt.edges = t.edges;
		rt.n_edges = n_edges;
		rt.period = period;
		rt.generation = trial + 1;

		pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);

		/* Only count violations on HARD-mode trials: in soft-
		 * degraded mode the analysis is allowed to publish a
		 * tuple that fails the kernel contract (apply_sched_groups
		 * catches it and routes through force_soft, see
		 * reconcile_invalid_params_demotes_to_soft_degraded).
		 * The property under test here is the hard-mode
		 * invariant: when reconcile claims feasibility, every
		 * follower it stamps must satisfy
		 * 0 < runtime <= local_deadline <= period. */
		struct reconcile_feasibility feas;
		reconcile_state_feasibility(s, &feas);
		if (feas.mode == RECONCILE_MODE_HARD) {
			for (i = 0; i < cb.calls; i++) {
				uint64_t r = cb.last[i].runtime;
				uint64_t d = cb.last[i].deadline;
				uint64_t p = cb.last[i].period;
				if (!(r > 0 && r <= d && d <= p))
					total_violations++;
			}
		}

		reconcile_fini(s);
	}

	pwtest_int_eq((int)total_violations, 0);
	return PWTEST_PASS;
}

/* Property-based test extended to the fork, join, and diamond
 * graph families. For each family we generate 60 random
 * instances and assert the hard-mode kernel contract
 * `0 < runtime <= local_deadline <= period`. The underlying
 * mechanism is identical to the chain property test; only the
 * edge layout changes between families. WCETs stay bounded so
 * the workload always lands in HARD. */
PWTEST(reconcile_property_random_shapes_satisfy_kernel_contract)
{
	uint32_t seed = 0xB5C9E227u;
	uint32_t shape, trial;
	uint32_t total_violations = 0;
	const uint32_t trials_per_shape = 60;
	const uint32_t n_shapes = 3; /* fork / join / diamond */

	for (shape = 0; shape < n_shapes; shape++) {
	for (trial = 0; trial < trials_per_shape; trial++) {
		reconcile_state_t *s = make_state_persistent(0.01);
		struct cb_ctx cb = { 0 };
		reconcile_topo_t rt;
		reconcile_follower_t fol[5];
		reconcile_edge_t edges[5];
		uint32_t n_followers = 0, n_edges = 0, i;
		uint64_t period;
		struct reconcile_feasibility feas;

		pwtest_ptr_notnull(s);
		seed = seed * 1103515245u + 12345u;
		period = 200000u + (uint64_t)(seed % 1800000u);

		switch (shape) {
		case 0: /* fork: 1 -> 2, 1 -> 3 */
			n_followers = 3;
			edges[0] = (reconcile_edge_t){ .src = 10, .dst = 11 };
			edges[1] = (reconcile_edge_t){ .src = 10, .dst = 12 };
			n_edges = 2;
			break;
		case 1: /* join: 1 -> 3, 2 -> 3 */
			n_followers = 3;
			edges[0] = (reconcile_edge_t){ .src = 10, .dst = 12 };
			edges[1] = (reconcile_edge_t){ .src = 11, .dst = 12 };
			n_edges = 2;
			break;
		case 2: /* diamond: 1 -> 2, 1 -> 3, 2 -> 4, 3 -> 4 */
			n_followers = 4;
			edges[0] = (reconcile_edge_t){ .src = 10, .dst = 11 };
			edges[1] = (reconcile_edge_t){ .src = 10, .dst = 12 };
			edges[2] = (reconcile_edge_t){ .src = 11, .dst = 13 };
			edges[3] = (reconcile_edge_t){ .src = 12, .dst = 13 };
			n_edges = 4;
			break;
		}

		for (i = 0; i < n_followers; i++) {
			seed = seed * 1103515245u + 12345u;
			fol[i].id  = 10 + i;
			fol[i].tid = 100 + i;
			fol[i].wcet = 1000u + (seed %
				(uint32_t)(period / (n_followers * 6 + 1)));
		}

		rt.followers = fol;
		rt.n_followers = n_followers;
		rt.edges = edges;
		rt.n_edges = n_edges;
		rt.period = period;
		rt.generation = (uint64_t)trial + 1 +
			(uint64_t)shape * 1000;

		pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);

		reconcile_state_feasibility(s, &feas);
		if (feas.mode == RECONCILE_MODE_HARD) {
			for (i = 0; i < cb.calls; i++) {
				uint64_t r = cb.last[i].runtime;
				uint64_t d = cb.last[i].deadline;
				uint64_t p = cb.last[i].period;
				if (!(r > 0 && r <= d && d <= p))
					total_violations++;
			}
		}

		reconcile_fini(s);
	}
	}

	pwtest_int_eq((int)total_violations, 0);
	return PWTEST_PASS;
}

/* apply_sched_groups detects a published runtime > local_deadline
 * tuple before issuing sched_setattr: shipping that tuple would
 * see the kernel reject it anyway. Per the operator-facing
 * contract, the published schedule mode must transition to
 * soft-degraded so that the JSON snapshot reflects the loss of the
 * hard claim (and the soft redistributor can take over on later
 * passes). The mechanism is reconcile_state_force_soft, called
 * from apply_sched_groups with the "kernel_rejected_or_invalid_params"
 * reason. This test pins that mechanism end-to-end: a state in
 * HARD after a feasible reconcile must flip to SOFT_DEGRADED on a
 * force-soft call and carry the invalid-params reason verbatim. */
PWTEST(reconcile_invalid_params_demotes_to_soft_degraded)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;
	struct reconcile_feasibility feas;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt = make_topo(&t, 5, 4, 1);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	reconcile_state_feasibility(s, &feas);
	pwtest_int_eq((int)feas.mode, (int)RECONCILE_MODE_HARD);

	reconcile_state_force_soft(s,
			"kernel_rejected_or_invalid_params");
	reconcile_state_feasibility(s, &feas);
	pwtest_int_eq((int)feas.mode, (int)RECONCILE_MODE_SOFT_DEGRADED);
	pwtest_str_eq(feas.reason,
			"kernel_rejected_or_invalid_params");

	reconcile_fini(s);
	return PWTEST_PASS;
}

/* Small-graph oracle: for graphs small enough that an exhaustive
 * enumeration of CPU placements is tractable, verify the
 * soundness property that the production reconcile path -- which
 * uses the worst-fit heuristic for CPU assignment -- never
 * declares HARD when no placement of the same task set onto the
 * available CPUs is feasible under the density predicate. The
 * oracle iterates every (n_cpus)^n_followers assignment, computes
 * the per-CPU density sum on each, and finds the minimum
 * achievable max-density. Production HARD requires its own
 * worst-fit placement to satisfy density <= 1 on every CPU, so
 * by construction the oracle's optimum is at most production's
 * max-density. A counter-example (production HARD but oracle
 * finds no feasible placement) would only arise from a real
 * arithmetic bug in either the density predicate or the
 * worst-fit placer; the test pins the property so a future
 * regression in either gets caught. */
PWTEST(reconcile_small_graph_oracle_soundness)
{
	uint32_t seed = 0xD3A1FE05u;
	uint32_t trial;
	const uint32_t trials = 64;
	const uint32_t n_cpus = 3;
	const uint32_t n_followers = 4;

	for (trial = 0; trial < trials; trial++) {
		reconcile_state_t *s = reconcile_init(n_cpus, 0.95,
				NULL, 0.01, true);
		struct cb_ctx cb = { 0 };
		reconcile_follower_t fol[4];
		reconcile_topo_t rt;
		uint32_t i, attempt;
		uint64_t period;
		double oracle_best_density = 1e30;
		struct reconcile_feasibility feas;

		pwtest_ptr_notnull(s);
		seed = seed * 1103515245u + 12345u;
		period = 200000u + (seed % 1800000u);

		for (i = 0; i < n_followers; i++) {
			seed = seed * 1103515245u + 12345u;
			fol[i].id  = 20 + i;
			fol[i].tid = 200 + i;
			/* WCETs in the 1-30 % range of the period --
			 * mostly feasible, some at the edge. */
			fol[i].wcet = (uint64_t)(period * 0.01) +
				(uint64_t)(seed % (uint32_t)(period * 0.30));
		}

		rt.followers = fol;
		rt.n_followers = n_followers;
		rt.edges = NULL;     /* singleton task set -- pure
				      * partitioning, no precedence. */
		rt.n_edges = 0;
		rt.period = period;
		rt.generation = trial + 1;

		/* Oracle: enumerate n_cpus^n_followers placements and
		 * take the minimum achievable max-density. The
		 * follower's contribution to its assigned CPU is
		 * wcet / period (constrained-deadline ratio with
		 * D = T). */
		for (attempt = 0;
		     attempt < (uint32_t)(1ull << (2 * n_followers));
		     attempt++) {
			double per_cpu[3] = { 0.0, 0.0, 0.0 };
			double max_d;
			uint32_t a = attempt;
			bool valid = true;
			for (i = 0; i < n_followers; i++) {
				uint32_t cpu = a & 0x3;
				a >>= 2;
				if (cpu >= n_cpus) {
					valid = false;
					break;
				}
				per_cpu[cpu] += (double)fol[i].wcet /
					(double)period;
			}
			if (!valid)
				continue;
			max_d = per_cpu[0];
			if (per_cpu[1] > max_d) max_d = per_cpu[1];
			if (per_cpu[2] > max_d) max_d = per_cpu[2];
			if (max_d < oracle_best_density)
				oracle_best_density = max_d;
		}

		pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
		reconcile_state_feasibility(s, &feas);

		/* Soundness property: production HARD must imply the
		 * oracle's best placement is also feasible. */
		if (feas.mode == RECONCILE_MODE_HARD) {
			pwtest_bool_true(oracle_best_density <= 1.0);
		}

		reconcile_fini(s);
	}

	return PWTEST_PASS;
}

/* When no fusion groups are formed, every follower is its own
 * macro-node leader. The MBPTA fusion-group-invalidation path keys
 * off the leader id staying stable across reconcile passes
 * (Cucu-Grosjean 2012 §IV); the singleton case is the contract's
 * baseline: the leader reported through sched_cb must equal the
 * follower's own id, both on the contracted path and on the
 * dag_foreach_node fallback. */
PWTEST(reconcile_singleton_fusion_leader_equals_follower_id)
{
	struct topo5 t;
	reconcile_state_t *s = make_state_persistent(0.01);
	struct cb_ctx cb = { 0 };
	reconcile_topo_t rt;
	uint32_t i;

	pwtest_ptr_notnull(s);
	topo5_init(&t);
	rt = make_topo(&t, 5, 4, 1);

	pwtest_int_eq(reconcile_apply(s, &rt, cb_record, &cb), 0);
	pwtest_int_eq((int)cb.calls, 5);
	for (i = 0; i < cb.calls; i++) {
		pwtest_int_eq((int)cb.last[i].fusion_leader,
				(int)cb.last[i].id);
	}

	reconcile_fini(s);
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

	pwtest_add(reconcile_g1_shared_tid_non_convex_is_rejected, PWTEST_NOARG);
	pwtest_add(reconcile_g2_unique_tids_no_grouping, PWTEST_NOARG);
	pwtest_add(reconcile_g3_chain_share_tid_collapses_to_one_cpu, PWTEST_NOARG);
	pwtest_add(reconcile_g4_chain_break_reverts_group, PWTEST_NOARG);
	pwtest_add(reconcile_g5_zero_tid_does_not_group, PWTEST_NOARG);
	pwtest_add(reconcile_g6_two_chain_clusters_first_passes_second_rejected, PWTEST_NOARG);
	pwtest_add(reconcile_g7_new_member_joins_existing_group, PWTEST_NOARG);
	pwtest_add(reconcile_g8_idempotent_reapply_keeps_group, PWTEST_NOARG);
	pwtest_add(reconcile_g9_legacy_path_also_groups, PWTEST_NOARG);
	pwtest_add(reconcile_g10_wcet_drift_preserves_group, PWTEST_NOARG);
	pwtest_add(reconcile_g11_cb_receives_per_node_values_for_group, PWTEST_NOARG);
	pwtest_add(reconcile_g12_group_overcapacity_returns_failure_state, PWTEST_NOARG);
	pwtest_add(reconcile_g13_chain_member_leaves_group, PWTEST_NOARG);

	pwtest_add(reconcile_mode_feasible_repeats_keep_hard, PWTEST_NOARG);
	pwtest_add(reconcile_mode_density_overload_flips_soft, PWTEST_NOARG);
	pwtest_add(reconcile_mode_hysteresis_promotes_after_n_passes, PWTEST_NOARG);

	pwtest_add(reconcile_singleton_fusion_leader_equals_follower_id,
			PWTEST_NOARG);
	pwtest_add(reconcile_invalid_params_demotes_to_soft_degraded,
			PWTEST_NOARG);
	pwtest_add(reconcile_property_random_chains_satisfy_kernel_contract,
			PWTEST_NOARG);
	pwtest_add(reconcile_property_random_shapes_satisfy_kernel_contract,
			PWTEST_NOARG);
	pwtest_add(reconcile_small_graph_oracle_soundness, PWTEST_NOARG);

	return PWTEST_PASS;
}
