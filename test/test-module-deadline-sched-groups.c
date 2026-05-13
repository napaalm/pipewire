/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Unit tests for module-deadline/sched_groups.c. Exercises the
 * per-TID accumulator that module-deadline uses to collapse a
 * dag_foreach_node pass (one callback per real node) into one
 * SCHED_DEADLINE update per OS thread.
 *
 * The contract under test:
 *
 *   - find_or_insert returns an existing slot when the TID matches,
 *     allocates a fresh one (with leader_id = UINT32_MAX, every
 *     numeric counter zeroed) when it doesn't.
 *
 *   - add folds a per-node observation into the right slot:
 *       leader_id = min(existing, id)
 *       sum_runtime / sum_deadline += this call's values
 *       period / cpu := latest call's values
 *       n_members ++
 *
 *   - tid <= 0 returns -EINVAL without touching the accumulator
 *     (a follower with no published thread can't be scheduled).
 *
 *   - reset rewinds count to 0 without freeing -- geometric reuse
 *     across passes.
 *
 *   - fini frees the backing array; calling init / reset / add
 *     after fini behaves as if the struct were freshly init'd.
 *
 *   - geometric growth: the backing array doubles on overflow; the
 *     initial allocation is 8 entries, so insertion 9 triggers
 *     realloc. The first 32 inserts fit in 8 -> 16 -> 32 capacity
 *     bumps, which is what we observe.
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/sched_groups.h"

/* --- init / fini --- */

PWTEST(sg_init_zeroes_struct)
{
	struct sched_groups sg = { (void *)0xdeadbeef, 42, 99 };
	sched_groups_init(&sg);
	pwtest_ptr_null(sg.entries);
	pwtest_int_eq((int)sg.count, 0);
	pwtest_int_eq((int)sg.cap, 0);
	return PWTEST_PASS;
}

PWTEST(sg_fini_null_safe)
{
	sched_groups_fini(NULL);
	sched_groups_init(NULL);
	sched_groups_reset(NULL);
	return PWTEST_PASS;
}

PWTEST(sg_fini_after_inserts_clears)
{
	struct sched_groups sg;
	sched_groups_init(&sg);
	pwtest_ptr_notnull(sched_groups_find_or_insert(&sg, 100));
	pwtest_ptr_notnull(sched_groups_find_or_insert(&sg, 200));
	pwtest_int_eq((int)sg.count, 2);
	pwtest_bool_true(sg.cap >= 2);

	sched_groups_fini(&sg);
	pwtest_ptr_null(sg.entries);
	pwtest_int_eq((int)sg.count, 0);
	pwtest_int_eq((int)sg.cap, 0);

	/* Re-init after fini is allowed and produces a fresh state. */
	sched_groups_init(&sg);
	pwtest_ptr_notnull(sched_groups_find_or_insert(&sg, 300));
	pwtest_int_eq((int)sg.count, 1);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

/* --- find_or_insert --- */

PWTEST(sg_find_or_insert_creates_fresh_entry)
{
	struct sched_groups sg;
	struct sched_group *g;
	sched_groups_init(&sg);

	g = sched_groups_find_or_insert(&sg, 42);
	pwtest_ptr_notnull(g);
	pwtest_int_eq((int)g->tid, 42);
	pwtest_int_eq((int)g->leader_id, (int)UINT32_MAX);
	pwtest_int_eq((int)g->sum_runtime, 0);
	pwtest_int_eq((int)g->sum_deadline, 0);
	pwtest_int_eq((int)g->period, 0);
	pwtest_int_eq((int)g->cpu, 0);
	pwtest_int_eq((int)g->n_members, 0);
	pwtest_int_eq((int)sg.count, 1);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

PWTEST(sg_find_or_insert_same_tid_returns_same_slot)
{
	struct sched_groups sg;
	struct sched_group *g1, *g2;
	sched_groups_init(&sg);

	g1 = sched_groups_find_or_insert(&sg, 42);
	g2 = sched_groups_find_or_insert(&sg, 42);
	pwtest_ptr_eq(g1, g2);
	pwtest_int_eq((int)sg.count, 1);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

PWTEST(sg_find_or_insert_distinct_tids_give_distinct_slots)
{
	struct sched_groups sg;
	struct sched_group *g1, *g2, *g3;
	sched_groups_init(&sg);

	g1 = sched_groups_find_or_insert(&sg, 10);
	g2 = sched_groups_find_or_insert(&sg, 20);
	g3 = sched_groups_find_or_insert(&sg, 30);
	pwtest_ptr_notnull(g1);
	pwtest_ptr_notnull(g2);
	pwtest_ptr_notnull(g3);
	pwtest_bool_true(g1 != g2);
	pwtest_bool_true(g1 != g3);
	pwtest_bool_true(g2 != g3);
	pwtest_int_eq((int)sg.count, 3);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

PWTEST(sg_find_or_insert_null_sg_returns_null)
{
	pwtest_ptr_null(sched_groups_find_or_insert(NULL, 1));
	return PWTEST_PASS;
}

/* --- add --- */

PWTEST(sg_add_records_first_observation)
{
	struct sched_groups sg;
	sched_groups_init(&sg);

	pwtest_int_eq(sched_groups_add(&sg, 7, 100, 1000, 2000, 5000, 2), 0);
	pwtest_int_eq((int)sg.count, 1);
	pwtest_int_eq((int)sg.entries[0].tid, 100);
	pwtest_int_eq((int)sg.entries[0].leader_id, 7);
	pwtest_int_eq((int)sg.entries[0].sum_runtime, 1000);
	pwtest_int_eq((int)sg.entries[0].sum_deadline, 2000);
	pwtest_int_eq((int)sg.entries[0].period, 5000);
	pwtest_int_eq((int)sg.entries[0].cpu, 2);
	pwtest_int_eq((int)sg.entries[0].n_members, 1);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

PWTEST(sg_add_sums_runtime_and_deadline)
{
	struct sched_groups sg;
	sched_groups_init(&sg);

	pwtest_int_eq(sched_groups_add(&sg, 7, 100, 1000, 2000, 5000, 1), 0);
	pwtest_int_eq(sched_groups_add(&sg, 8, 100,  500, 1500, 5000, 1), 0);
	pwtest_int_eq(sched_groups_add(&sg, 9, 100,  300,  800, 5000, 1), 0);

	pwtest_int_eq((int)sg.count, 1);
	pwtest_int_eq((int)sg.entries[0].sum_runtime, 1800);
	pwtest_int_eq((int)sg.entries[0].sum_deadline, 4300);
	pwtest_int_eq((int)sg.entries[0].n_members, 3);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

PWTEST(sg_add_leader_id_is_minimum_observed)
{
	/* Whatever order we present the observations in, leader_id
	 * must end up at the smallest follower id. This is what
	 * module-deadline uses as the cache anchor; if the order
	 * changed the leader, the last_applied cache would
	 * spuriously invalidate every pass. */
	struct sched_groups sg;
	sched_groups_init(&sg);

	pwtest_int_eq(sched_groups_add(&sg, 50, 100, 1, 1, 1, 0), 0);
	pwtest_int_eq((int)sg.entries[0].leader_id, 50);

	pwtest_int_eq(sched_groups_add(&sg, 30, 100, 1, 1, 1, 0), 0);
	pwtest_int_eq((int)sg.entries[0].leader_id, 30);

	pwtest_int_eq(sched_groups_add(&sg, 70, 100, 1, 1, 1, 0), 0);
	pwtest_int_eq((int)sg.entries[0].leader_id, 30);

	pwtest_int_eq(sched_groups_add(&sg, 10, 100, 1, 1, 1, 0), 0);
	pwtest_int_eq((int)sg.entries[0].leader_id, 10);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

PWTEST(sg_add_period_and_cpu_track_latest)
{
	/* period and cpu are invariant per group by construction
	 * (the global period and the DAG's group-pinned CPU). The
	 * accumulator overwrites them with the latest observation
	 * since there's no "first observation" bookkeeping; the
	 * behaviour is documented and matches what
	 * apply_sched_groups expects. */
	struct sched_groups sg;
	sched_groups_init(&sg);

	pwtest_int_eq(sched_groups_add(&sg, 7, 100, 1, 1, 5000, 0), 0);
	pwtest_int_eq((int)sg.entries[0].period, 5000);
	pwtest_int_eq((int)sg.entries[0].cpu, 0);

	pwtest_int_eq(sched_groups_add(&sg, 8, 100, 1, 1, 5001, 3), 0);
	pwtest_int_eq((int)sg.entries[0].period, 5001);
	pwtest_int_eq((int)sg.entries[0].cpu, 3);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

PWTEST(sg_add_invalid_tid_returns_einval)
{
	struct sched_groups sg;
	sched_groups_init(&sg);

	pwtest_int_eq(sched_groups_add(&sg, 7, 0, 1, 1, 1, 0), -EINVAL);
	pwtest_int_eq(sched_groups_add(&sg, 7, -1, 1, 1, 1, 0), -EINVAL);
	pwtest_int_eq((int)sg.count, 0);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

PWTEST(sg_add_null_sg_returns_einval)
{
	pwtest_int_eq(sched_groups_add(NULL, 7, 100, 1, 1, 1, 0), -EINVAL);
	return PWTEST_PASS;
}

PWTEST(sg_add_two_tids_yields_two_slots)
{
	struct sched_groups sg;
	sched_groups_init(&sg);

	pwtest_int_eq(sched_groups_add(&sg, 1, 100, 10, 20, 1000, 0), 0);
	pwtest_int_eq(sched_groups_add(&sg, 2, 200, 30, 40, 1000, 1), 0);
	pwtest_int_eq((int)sg.count, 2);

	struct sched_group *a = sched_groups_find_or_insert(&sg, 100);
	struct sched_group *b = sched_groups_find_or_insert(&sg, 200);
	pwtest_int_eq((int)a->sum_runtime, 10);
	pwtest_int_eq((int)a->sum_deadline, 20);
	pwtest_int_eq((int)a->leader_id, 1);
	pwtest_int_eq((int)b->sum_runtime, 30);
	pwtest_int_eq((int)b->sum_deadline, 40);
	pwtest_int_eq((int)b->leader_id, 2);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

/* --- reset --- */

PWTEST(sg_reset_clears_count_keeps_capacity)
{
	struct sched_groups sg;
	uint32_t cap_before;
	sched_groups_init(&sg);

	pwtest_int_eq(sched_groups_add(&sg, 1, 100, 1, 1, 1, 0), 0);
	pwtest_int_eq(sched_groups_add(&sg, 2, 200, 1, 1, 1, 0), 0);
	cap_before = sg.cap;
	pwtest_bool_true(cap_before > 0);

	sched_groups_reset(&sg);
	pwtest_int_eq((int)sg.count, 0);
	pwtest_int_eq((int)sg.cap, (int)cap_before);

	/* After reset, find_or_insert allocates from the existing
	 * backing array (no realloc). */
	pwtest_int_eq(sched_groups_add(&sg, 7, 300, 1, 1, 1, 0), 0);
	pwtest_int_eq((int)sg.count, 1);
	pwtest_int_eq((int)sg.cap, (int)cap_before);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

/* --- geometric growth --- */

PWTEST(sg_grows_geometrically_to_32)
{
	/* Initial cap is 8; doubling rule means we see 8 -> 16 -> 32
	 * as we cross capacity boundaries. Verify the cap progression
	 * and that the first 32 distinct TIDs all fit. */
	struct sched_groups sg;
	sched_groups_init(&sg);

	for (pid_t t = 1; t <= 32; t++) {
		struct sched_group *g = sched_groups_find_or_insert(&sg, t);
		pwtest_ptr_notnull(g);
		pwtest_int_eq((int)g->tid, (int)t);
	}
	pwtest_int_eq((int)sg.count, 32);
	pwtest_int_eq((int)sg.cap, 32);

	/* Lookup must still find earlier inserts (they didn't move
	 * relative to the index, but realloc could have shifted the
	 * underlying buffer; the API only promises stable pointers
	 * within a single insert). */
	for (pid_t t = 1; t <= 32; t++) {
		struct sched_group *g = sched_groups_find_or_insert(&sg, t);
		pwtest_ptr_notnull(g);
		pwtest_int_eq((int)g->tid, (int)t);
	}

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

/* --- realistic chain-merge scenario --- */

PWTEST(sg_realistic_chain_pattern)
{
	/* Three-node chain {10, 11, 12} sharing TID 555 plus one
	 * singleton {13} on TID 666. After the foreach pass:
	 *   - tid 555: sum_runtime = sum of the three nodes',
	 *     leader_id = 10, n_members = 3.
	 *   - tid 666: single node, sum_runtime = the singleton's,
	 *     leader_id = 13, n_members = 1.
	 * This is what module-deadline's apply_sched_groups receives. */
	struct sched_groups sg;
	sched_groups_init(&sg);

	pwtest_int_eq(sched_groups_add(&sg, 12, 555, 4000, 9000, 100000, 2), 0);
	pwtest_int_eq(sched_groups_add(&sg, 11, 555, 5000, 9500, 100000, 2), 0);
	pwtest_int_eq(sched_groups_add(&sg, 10, 555, 6000, 9300, 100000, 2), 0);
	pwtest_int_eq(sched_groups_add(&sg, 13, 666, 2000, 8000, 100000, 1), 0);

	pwtest_int_eq((int)sg.count, 2);

	struct sched_group *chain = sched_groups_find_or_insert(&sg, 555);
	pwtest_int_eq((int)chain->leader_id, 10);
	pwtest_int_eq((int)chain->sum_runtime, 15000);
	pwtest_int_eq((int)chain->sum_deadline, 27800);
	pwtest_int_eq((int)chain->n_members, 3);
	pwtest_int_eq((int)chain->cpu, 2);
	pwtest_int_eq((int)chain->period, 100000);

	struct sched_group *alone = sched_groups_find_or_insert(&sg, 666);
	pwtest_int_eq((int)alone->leader_id, 13);
	pwtest_int_eq((int)alone->sum_runtime, 2000);
	pwtest_int_eq((int)alone->sum_deadline, 8000);
	pwtest_int_eq((int)alone->n_members, 1);
	pwtest_int_eq((int)alone->cpu, 1);

	/* Next pass starts fresh: reset clears the count, the same
	 * TIDs land in the same logical slots (though pointers may
	 * differ across a realloc), and the running sums restart
	 * from zero. */
	sched_groups_reset(&sg);
	pwtest_int_eq((int)sg.count, 0);
	pwtest_int_eq(sched_groups_add(&sg, 11, 555, 7000, 9500, 100000, 2), 0);
	chain = sched_groups_find_or_insert(&sg, 555);
	pwtest_int_eq((int)chain->sum_runtime, 7000);
	pwtest_int_eq((int)chain->leader_id, 11);
	pwtest_int_eq((int)chain->n_members, 1);

	sched_groups_fini(&sg);
	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_sched_groups)
{
	pwtest_add(sg_init_zeroes_struct, PWTEST_NOARG);
	pwtest_add(sg_fini_null_safe, PWTEST_NOARG);
	pwtest_add(sg_fini_after_inserts_clears, PWTEST_NOARG);
	pwtest_add(sg_find_or_insert_creates_fresh_entry, PWTEST_NOARG);
	pwtest_add(sg_find_or_insert_same_tid_returns_same_slot, PWTEST_NOARG);
	pwtest_add(sg_find_or_insert_distinct_tids_give_distinct_slots, PWTEST_NOARG);
	pwtest_add(sg_find_or_insert_null_sg_returns_null, PWTEST_NOARG);
	pwtest_add(sg_add_records_first_observation, PWTEST_NOARG);
	pwtest_add(sg_add_sums_runtime_and_deadline, PWTEST_NOARG);
	pwtest_add(sg_add_leader_id_is_minimum_observed, PWTEST_NOARG);
	pwtest_add(sg_add_period_and_cpu_track_latest, PWTEST_NOARG);
	pwtest_add(sg_add_invalid_tid_returns_einval, PWTEST_NOARG);
	pwtest_add(sg_add_null_sg_returns_einval, PWTEST_NOARG);
	pwtest_add(sg_add_two_tids_yields_two_slots, PWTEST_NOARG);
	pwtest_add(sg_reset_clears_count_keeps_capacity, PWTEST_NOARG);
	pwtest_add(sg_grows_geometrically_to_32, PWTEST_NOARG);
	pwtest_add(sg_realistic_chain_pattern, PWTEST_NOARG);

	return PWTEST_PASS;
}
