/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Unit tests for the same-loop fast path in
 * src/pipewire/private.h::trigger_target_v1.
 *
 * The function under test sits in a header (static inline,
 * referenced through a function pointer) and depends on three
 * external symbols:
 *
 *   - pw_log_*                  -> linked from libpipewire,
 *   - spa_system_eventfd_write  -> needs a real spa_system,
 *   - pw_impl_node_dispatch_inline -> linked from libpipewire normally,
 *                                  but intercepted here through
 *                                  the linker's --wrap so the test
 *                                  can verify whether the inline path
 *                                  was taken without spinning up a
 *                                  real pw_impl_node + spa_node graph.
 *
 * A spa_system is taken from pw_main_loop_new() so the eventfd-write
 * side of the comparison is exercised against the real OS eventfd
 * the production code would use.
 *
 * The wrapper for pw_impl_node_dispatch_inline records call counts and
 * arguments so individual cases can assert the exact path the trigger
 * function took. The wrapper deliberately does *not* delegate to the
 * real implementation -- the goal is to isolate the dispatch decision
 * inside trigger_target_v1 from process_node's machinery, which would
 * need a fully-initialised pw_impl_node and is covered by the live
 * test under live-test/inline-dispatch.sh.
 *
 * Test scenarios:
 *
 *   single_predecessor_inline_when_same_loop
 *   single_predecessor_eventfd_when_cross_loop
 *   single_predecessor_eventfd_when_src_system_null
 *   single_predecessor_eventfd_when_src_system_mismatches
 *   pending_dec_short_circuits_without_trigger
 *   already_triggered_cas_fails_no_dispatch
 *   mixed_predecessors_last_to_fire_drives_inline
 *   mixed_predecessors_last_to_fire_drives_eventfd
 *   external_into_interior_does_not_break_inline_for_same_loop_edge
 *
 * The mixed-predecessor cases stress the scenario where a fused
 * subgraph has an additional cross-loop edge entering one of its
 * interior nodes. Per-edge state in pw_node_activation_state means
 * each predecessor independently decrements the same `pending`
 * counter and only the *last* one to fire transitions the status.
 * Whether that last decrement comes through the same-loop or
 * eventfd path is decided per call and must be consistent with
 * which predecessor happens to win the race in the field.
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "pwtest.h"

#include <pipewire/pipewire.h>
#include <spa/utils/atomic.h>
#include <spa/support/system.h>

#include "pipewire/private.h"

/* -------------------------------------------------------------------------
 * Test wrapper for pw_impl_node_dispatch_inline.
 *
 * Linked in via -Wl,--wrap=pw_impl_node_dispatch_inline. Every call
 * the static-inline trigger_target_v1 compiles into THIS translation
 * unit is redirected here instead of the libpipewire implementation,
 * so we can observe whether the inline path was taken without running
 * a real process_node (which needs a real pw_impl_node).
 * ------------------------------------------------------------------------- */
static int g_dispatch_calls;
static struct pw_node_target *g_dispatch_last_target;
static uint64_t g_dispatch_last_nsec;

int __wrap_pw_impl_node_dispatch_inline(struct pw_node_target *t, uint64_t nsec)
{
	g_dispatch_calls++;
	g_dispatch_last_target = t;
	g_dispatch_last_nsec = nsec;
	return 0;
}

/* -------------------------------------------------------------------------
 * Test fixture: real spa_system from pw_main_loop_new(), a single
 * activation block, and a target the test mutates per case. The fixture
 * provides a single eventfd (real) and a single activation pair so the
 * tests can drain the eventfd and reset the counters between cases.
 * ------------------------------------------------------------------------- */

struct fixture {
	struct pw_main_loop *ml;
	struct pw_loop *loop;
	struct spa_system *system;
	int evfd;
	struct pw_node_activation activation;
};

static void fixture_init(struct fixture *f)
{
	memset(f, 0, sizeof(*f));
	pw_init(0, NULL);

	f->ml = pw_main_loop_new(NULL);
	pwtest_ptr_notnull(f->ml);

	f->loop = pw_main_loop_get_loop(f->ml);
	pwtest_ptr_notnull(f->loop);

	f->system = f->loop->system;
	pwtest_ptr_notnull(f->system);

	f->evfd = spa_system_eventfd_create(f->system,
			SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	pwtest_int_ne(f->evfd, -1);

	/* Activation starts as NOT_TRIGGERED with no pending/required. The
	 * test cases set state[0].pending / state[0].required as needed. */
	f->activation.state[0].required = 0;
	f->activation.state[0].pending  = 0;
	SPA_ATOMIC_STORE(f->activation.status, PW_NODE_ACTIVATION_NOT_TRIGGERED);

	g_dispatch_calls = 0;
	g_dispatch_last_target = NULL;
	g_dispatch_last_nsec = 0;
}

static void fixture_fini(struct fixture *f)
{
	spa_system_close(f->system, f->evfd);
	pw_main_loop_destroy(f->ml);
}

/* True iff the eventfd has had at least one write committed since the
 * last drain. Non-destructive: drains the counter (consumes the writes)
 * so subsequent calls in the same test only see fresh writes. */
static bool drained_eventfd(struct fixture *f)
{
	uint64_t v = 0;
	int r = spa_system_eventfd_read(f->system, f->evfd, &v);
	if (r == -EAGAIN || v == 0)
		return false;
	return true;
}

/* Convenience: build a same-system target. src_system pre-set so the
 * fast path lights up unless the test wants to mutate it. */
static void make_same_loop_target(struct fixture *f, struct pw_node_target *t)
{
	memset(t, 0, sizeof(*t));
	t->id = 1;
	snprintf(t->name, sizeof(t->name), "%s", "same-loop-target");
	t->activation = &f->activation;
	t->system = f->system;
	t->src_system = f->system;
	t->fd = f->evfd;
	t->trigger = trigger_target_v1;
}

/* Convenience: build a cross-system target. src_system holds a sentinel
 * pointer that is provably != t->system so the same-loop check fails. */
static void make_cross_loop_target(struct fixture *f, struct pw_node_target *t)
{
	make_same_loop_target(f, t);
	/* Use a stack-local address as the sentinel: not equal to any real
	 * spa_system the loop owns. */
	static char other_system_marker;
	t->src_system = (struct spa_system *)&other_system_marker;
	snprintf(t->name, sizeof(t->name), "%s", "cross-loop-target");
}

/* -------------------------------------------------------------------------
 * Single-predecessor cases
 * ------------------------------------------------------------------------- */

PWTEST(single_predecessor_inline_when_same_loop)
{
	struct fixture f;
	struct pw_node_target t;

	fixture_init(&f);
	make_same_loop_target(&f, &t);
	f.activation.state[0].required = 1;
	f.activation.state[0].pending  = 1;

	int res = t.trigger(&t, 12345);

	/* pending hit zero, CAS succeeded, src_system==system so the
	 * inline wrapper was invoked exactly once. The eventfd path
	 * was NOT taken, so the descriptor stays drainless. */
	pwtest_int_eq(res, 0);
	pwtest_int_eq(g_dispatch_calls, 1);
	pwtest_ptr_eq(g_dispatch_last_target, &t);
	pwtest_int_eq(g_dispatch_last_nsec, 12345);
	pwtest_bool_false(drained_eventfd(&f));
	/* Status was advanced to TRIGGERED by trigger_target_v1 before
	 * it handed off to dispatch_inline. process_node would normally
	 * continue the transition; the wrapper short-circuits. */
	pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.status),
			PW_NODE_ACTIVATION_TRIGGERED);
	pwtest_int_eq(f.activation.signal_time, 12345);

	fixture_fini(&f);
	return PWTEST_PASS;
}

PWTEST(single_predecessor_eventfd_when_cross_loop)
{
	struct fixture f;
	struct pw_node_target t;

	fixture_init(&f);
	make_cross_loop_target(&f, &t);
	f.activation.state[0].required = 1;
	f.activation.state[0].pending  = 1;

	/* Eventfd-path success returns 1 ("trigger fired"), matching the
	 * pre-change contract that callers like pw_impl_node_trigger
	 * propagate. The same-loop fast path returns whatever
	 * pw_impl_node_dispatch_inline returns (0 from this TU's wrapper,
	 * whatever process_node returns in production). */
	int res = t.trigger(&t, 999);

	pwtest_int_eq(res, 1);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_true(drained_eventfd(&f));
	pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.status),
			PW_NODE_ACTIVATION_TRIGGERED);
	pwtest_int_eq(f.activation.signal_time, 999);

	fixture_fini(&f);
	return PWTEST_PASS;
}

PWTEST(single_predecessor_eventfd_when_src_system_null)
{
	/* src_system==NULL means the operator (or the remote/exported/
	 * driver exclusion in pw_node_peer_ref) opted out of the fast
	 * path. Even though the test's "same" and "src" pointers would
	 * otherwise match, NULL must keep the path on eventfd. */
	struct fixture f;
	struct pw_node_target t;

	fixture_init(&f);
	make_same_loop_target(&f, &t);
	t.src_system = NULL;
	f.activation.state[0].required = 1;
	f.activation.state[0].pending  = 1;

	int res = t.trigger(&t, 7);

	pwtest_int_eq(res, 1);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_true(drained_eventfd(&f));

	fixture_fini(&f);
	return PWTEST_PASS;
}

PWTEST(single_predecessor_eventfd_when_src_system_mismatches)
{
	struct fixture f;
	struct pw_node_target t;

	fixture_init(&f);
	make_cross_loop_target(&f, &t);
	f.activation.state[0].required = 1;
	f.activation.state[0].pending  = 1;

	int res = t.trigger(&t, 1);

	pwtest_int_eq(res, 1);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_true(drained_eventfd(&f));

	fixture_fini(&f);
	return PWTEST_PASS;
}

/* -------------------------------------------------------------------------
 * pending counter and status CAS
 * ------------------------------------------------------------------------- */

PWTEST(pending_dec_short_circuits_without_trigger)
{
	/* Three predecessors, only one has fired (pending dec from 3 to
	 * 2). trigger_target_v1 must NOT advance the status, NOT write
	 * the eventfd, and NOT inline-dispatch. */
	struct fixture f;
	struct pw_node_target t;

	fixture_init(&f);
	make_same_loop_target(&f, &t);
	f.activation.state[0].required = 3;
	f.activation.state[0].pending  = 3;

	int res = t.trigger(&t, 11);

	pwtest_int_eq(res, 0);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_false(drained_eventfd(&f));
	pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.status),
			PW_NODE_ACTIVATION_NOT_TRIGGERED);
	pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.state[0].pending), 2);

	fixture_fini(&f);
	return PWTEST_PASS;
}

PWTEST(already_triggered_cas_fails_no_dispatch)
{
	/* If a concurrent producer beat us to the CAS (status already
	 * TRIGGERED or further along the state machine), trigger_target_v1
	 * must return -EIO and NOT dispatch -- this is the production
	 * code's defence against double-process of the same cycle. */
	struct fixture f;
	struct pw_node_target t;

	fixture_init(&f);
	make_same_loop_target(&f, &t);
	f.activation.state[0].required = 1;
	f.activation.state[0].pending  = 1;
	SPA_ATOMIC_STORE(f.activation.status, PW_NODE_ACTIVATION_TRIGGERED);

	int res = t.trigger(&t, 1);

	pwtest_int_eq(res, -EIO);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_false(drained_eventfd(&f));

	fixture_fini(&f);
	return PWTEST_PASS;
}

/* -------------------------------------------------------------------------
 * Mixed (same-loop + cross-loop) predecessors on the same consumer.
 *
 * This is the case the user flagged: a fused subgraph has at least one
 * consumer whose `pending` counter also decrements from an external,
 * different-loop predecessor. The pre-existing per-edge ordering of
 * pending decrements decides which path (inline or eventfd) actually
 * drives the consumer in any given cycle.
 *
 * The tests simulate both orderings by reusing the same activation
 * (with required=pending=2) across the two predecessors' triggers.
 * ------------------------------------------------------------------------- */

PWTEST(mixed_predecessors_last_to_fire_drives_inline)
{
	struct fixture f;
	struct pw_node_target same, cross;

	fixture_init(&f);
	make_same_loop_target(&f, &same);
	make_cross_loop_target(&f, &cross);
	/* Both targets point at the same activation -- they represent the
	 * same downstream consumer C as seen by two distinct producers. */
	cross.activation = &f.activation;
	f.activation.state[0].required = 2;
	f.activation.state[0].pending  = 2;

	/* Cross-loop predecessor fires first: pending 2 -> 1, no trigger. */
	int res = cross.trigger(&cross, 100);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_false(drained_eventfd(&f));
	pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.status),
			PW_NODE_ACTIVATION_NOT_TRIGGERED);

	/* Same-loop predecessor fires second: pending 1 -> 0, CAS to
	 * TRIGGERED succeeds, inline path taken. */
	res = same.trigger(&same, 200);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(g_dispatch_calls, 1);
	pwtest_bool_false(drained_eventfd(&f));
	pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.status),
			PW_NODE_ACTIVATION_TRIGGERED);
	pwtest_int_eq(f.activation.signal_time, 200);

	fixture_fini(&f);
	return PWTEST_PASS;
}

PWTEST(mixed_predecessors_last_to_fire_drives_eventfd)
{
	struct fixture f;
	struct pw_node_target same, cross;

	fixture_init(&f);
	make_same_loop_target(&f, &same);
	make_cross_loop_target(&f, &cross);
	cross.activation = &f.activation;
	f.activation.state[0].required = 2;
	f.activation.state[0].pending  = 2;

	/* Same-loop predecessor fires first. */
	int res = same.trigger(&same, 50);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_false(drained_eventfd(&f));
	pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.status),
			PW_NODE_ACTIVATION_NOT_TRIGGERED);

	/* Cross-loop predecessor fires second: eventfd write, no inline.
	 * Eventfd-path success returns 1. */
	res = cross.trigger(&cross, 70);
	pwtest_int_eq(res, 1);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_true(drained_eventfd(&f));
	pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.status),
			PW_NODE_ACTIVATION_TRIGGERED);
	pwtest_int_eq(f.activation.signal_time, 70);

	fixture_fini(&f);
	return PWTEST_PASS;
}

PWTEST(external_into_interior_does_not_break_inline_for_same_loop_edge)
{
	/* Five predecessors of an interior consumer C: four same-loop
	 * (the fused subgraph that surrounds C) and one cross-loop (the
	 * external edge the user asked about). pending=5 reaches zero
	 * regardless of fire order. We verify the invariant: exactly
	 * one of the five triggers transitions to TRIGGERED, exactly one
	 * of {inline, eventfd} fires, never both, never neither. We run
	 * the scenario once with the cross-loop predecessor fired last
	 * and once with it fired in the middle to keep both orderings
	 * covered. */
	struct fixture f;
	struct pw_node_target same[4], cross;
	int i;

	fixture_init(&f);
	for (i = 0; i < 4; i++) {
		make_same_loop_target(&f, &same[i]);
		same[i].id = 100 + i;
	}
	make_cross_loop_target(&f, &cross);
	cross.activation = &f.activation;
	f.activation.state[0].required = 5;
	f.activation.state[0].pending  = 5;

	/* Cross fires last; the four same-loop predecessors all fire
	 * first, decrementing pending 5->1 without triggering, then
	 * cross fires and takes the eventfd path. */
	for (i = 0; i < 4; i++) {
		int res = same[i].trigger(&same[i], 1000 + i);
		pwtest_int_eq(res, 0);
		pwtest_int_eq(g_dispatch_calls, 0);
		pwtest_bool_false(drained_eventfd(&f));
		pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.status),
				PW_NODE_ACTIVATION_NOT_TRIGGERED);
	}
	int res = cross.trigger(&cross, 1500);
	pwtest_int_eq(res, 1);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_true(drained_eventfd(&f));
	pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.status),
			PW_NODE_ACTIVATION_TRIGGERED);

	/* Second scenario on the same fixture: cross fires in the middle,
	 * one of the same-loop predecessors fires last and inline-dispatches. */
	/* Reset the activation and counters. */
	SPA_ATOMIC_STORE(f.activation.status, PW_NODE_ACTIVATION_NOT_TRIGGERED);
	f.activation.state[0].pending = 5;
	g_dispatch_calls = 0;
	(void)drained_eventfd(&f);

	/* same[0..1], then cross, then same[2..3]. The last fire is
	 * same[3] -> inline. */
	res = same[0].trigger(&same[0], 1);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_false(drained_eventfd(&f));

	res = same[1].trigger(&same[1], 2);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_false(drained_eventfd(&f));

	res = cross.trigger(&cross, 3);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_false(drained_eventfd(&f));

	res = same[2].trigger(&same[2], 4);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(g_dispatch_calls, 0);
	pwtest_bool_false(drained_eventfd(&f));

	res = same[3].trigger(&same[3], 5);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(g_dispatch_calls, 1);
	pwtest_bool_false(drained_eventfd(&f));
	pwtest_int_eq(SPA_ATOMIC_LOAD(f.activation.status),
			PW_NODE_ACTIVATION_TRIGGERED);
	pwtest_int_eq(f.activation.signal_time, 5);

	fixture_fini(&f);
	return PWTEST_PASS;
}

PWTEST_SUITE(trigger_target)
{
	pwtest_add(single_predecessor_inline_when_same_loop,           PWTEST_NOARG);
	pwtest_add(single_predecessor_eventfd_when_cross_loop,         PWTEST_NOARG);
	pwtest_add(single_predecessor_eventfd_when_src_system_null,    PWTEST_NOARG);
	pwtest_add(single_predecessor_eventfd_when_src_system_mismatches, PWTEST_NOARG);
	pwtest_add(pending_dec_short_circuits_without_trigger,         PWTEST_NOARG);
	pwtest_add(already_triggered_cas_fails_no_dispatch,            PWTEST_NOARG);
	pwtest_add(mixed_predecessors_last_to_fire_drives_inline,      PWTEST_NOARG);
	pwtest_add(mixed_predecessors_last_to_fire_drives_eventfd,     PWTEST_NOARG);
	pwtest_add(external_into_interior_does_not_break_inline_for_same_loop_edge, PWTEST_NOARG);
	return PWTEST_PASS;
}
