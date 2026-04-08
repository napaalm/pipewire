/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Tests for the per-CPU capacity / topology probe consumed by the
 * P-EDF placer in module-deadline. The sysfs-probe path is exercised
 * on the dev host (homogeneous case D1.7.a, sysfs fallback D1.7.f);
 * synthetic heterogeneous / DVFS / island / SMT cases use the JSON
 * affordance to bypass sysfs and inject deterministic capacity
 * vectors.
 */

#include "config.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/cpu_topology.h"

static const struct cpu_info *
find_cpu(const struct cpu_topology *t, uint32_t cpu_id)
{
	for (uint32_t i = 0; i < t->num_cpus; i++) {
		if (t->cpus[i].cpu_id == cpu_id)
			return &t->cpus[i];
	}
	return NULL;
}

/* ===================================================================
 * D1.7.a -- homogeneous host: every CPU has the same effective capacity
 * (raw_capacity * min_freq), so relative_capacity == 1.0 everywhere and
 * each CPU is its own island.
 * =================================================================== */
PWTEST(cpu_topo_homogeneous_json_all_ones)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, core_id = 0, island_id = 0,"
		"    raw_capacity = 1024, min_freq_khz = 3000000, max_freq_khz = 3000000 },"
		"  { cpu_id = 1, core_id = 1, island_id = 1,"
		"    raw_capacity = 1024, min_freq_khz = 3000000, max_freq_khz = 3000000 },"
		"  { cpu_id = 2, core_id = 2, island_id = 2,"
		"    raw_capacity = 1024, min_freq_khz = 3000000, max_freq_khz = 3000000 }"
		"] }";

	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)t.num_cpus, 3);
	for (uint32_t i = 0; i < t.num_cpus; i++)
		pwtest_double_eq(t.cpus[i].relative_capacity, 1.0);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

/* ===================================================================
 * D1.7.b -- two capacity classes via raw_capacity. Half the CPUs come
 * back at relative_capacity ~ 0.5; the rest at 1.0.
 * =================================================================== */
PWTEST(cpu_topo_two_capacity_classes)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, core_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 },"
		"  { cpu_id = 1, core_id = 1, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 },"
		"  { cpu_id = 2, core_id = 2, raw_capacity = 512,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 },"
		"  { cpu_id = 3, core_id = 3, raw_capacity = 512,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 }"
		"] }";

	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)t.num_cpus, 4);
	pwtest_double_eq(find_cpu(&t, 0)->relative_capacity, 1.0);
	pwtest_double_eq(find_cpu(&t, 1)->relative_capacity, 1.0);
	pwtest_double_eq(find_cpu(&t, 2)->relative_capacity, 0.5);
	pwtest_double_eq(find_cpu(&t, 3)->relative_capacity, 0.5);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

/* ===================================================================
 * D1.7.c -- DVFS-conservative: identical raw_capacity but a CPU pinned
 * to half the min-freq comes back at relative_capacity = 0.5. The same
 * topology with policy ASSUME_MAX uses max_freq instead, which here
 * gives relative_capacity = 1.0 across the board (max_freq is uniform).
 * =================================================================== */
PWTEST(cpu_topo_dvfs_min_vs_max)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 },"
		"  { cpu_id = 1, raw_capacity = 1024,"
		"    min_freq_khz = 1500000, max_freq_khz = 3000000 }"
		"] }";

	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_double_eq(find_cpu(&t, 0)->relative_capacity, 1.0);
	pwtest_double_eq(find_cpu(&t, 1)->relative_capacity, 0.5);
	cpu_topology_destroy(&t);

	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_ASSUME_MAX, &t), 0);
	pwtest_double_eq(find_cpu(&t, 0)->relative_capacity, 1.0);
	pwtest_double_eq(find_cpu(&t, 1)->relative_capacity, 1.0);
	cpu_topology_destroy(&t);

	return PWTEST_PASS;
}

/* ===================================================================
 * D1.7.d -- island detection. Two CPUs that share the same canonical
 * island id end up stamped with that id, distinct from a third CPU's.
 * =================================================================== */
PWTEST(cpu_topo_island_detection)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, core_id = 0, island_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 },"
		"  { cpu_id = 1, core_id = 1, island_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 },"
		"  { cpu_id = 2, core_id = 2, island_id = 2, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 }"
		"] }";

	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)find_cpu(&t, 0)->island_id, 0);
	pwtest_int_eq((int)find_cpu(&t, 1)->island_id, 0);
	pwtest_int_eq((int)find_cpu(&t, 2)->island_id, 2);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

/* ===================================================================
 * D1.7.e -- SMT-pair detection. Two CPUs that share a physical core
 * (same core_id) are detected as siblings; cpu_topology_has_smt_pair
 * fires, and STRICT policy refuses while DEDUPE drops the higher one.
 * =================================================================== */
PWTEST(cpu_topo_smt_pair_strict_refuses)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 2, core_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 },"
		"  { cpu_id = 10, core_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 },"
		"  { cpu_id = 3, core_id = 1, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 }"
		"] }";

	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);

	uint32_t a = 0, b = 0;
	pwtest_bool_true(cpu_topology_has_smt_pair(&t, &a, &b));
	pwtest_int_eq((int)a, 2);
	pwtest_int_eq((int)b, 10);

	pwtest_int_eq((int)find_cpu(&t, 2)->num_siblings, 1);
	pwtest_int_eq((int)find_cpu(&t, 2)->smt_siblings[0], 10);

	uint32_t oa = 0, ob = 0;
	pwtest_int_eq(cpu_topology_apply_smt_policy(&t, CPU_SMT_STRICT, &oa, &ob), -1);
	pwtest_int_eq((int)oa, 2);
	pwtest_int_eq((int)ob, 10);

	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

PWTEST(cpu_topo_smt_pair_dedupe_drops_higher)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 2, core_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 },"
		"  { cpu_id = 10, core_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 },"
		"  { cpu_id = 3, core_id = 1, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 }"
		"] }";

	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)t.num_cpus, 3);

	pwtest_int_eq(cpu_topology_apply_smt_policy(&t, CPU_SMT_DEDUPE,
				NULL, NULL), 0);

	pwtest_int_eq((int)t.num_cpus, 2);
	pwtest_ptr_notnull(find_cpu(&t, 2));
	pwtest_ptr_notnull(find_cpu(&t, 3));
	pwtest_ptr_null(find_cpu(&t, 10));

	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

/* IGNORE accepts the input unchanged; the warning is observable only in
 * the log, so this case just checks num_cpus stays the same. */
PWTEST(cpu_topo_smt_pair_ignore_keeps_set)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 2, core_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 },"
		"  { cpu_id = 10, core_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 1000000, max_freq_khz = 1000000 }"
		"] }";
	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq(cpu_topology_apply_smt_policy(&t, CPU_SMT_IGNORE, NULL, NULL), 0);
	pwtest_int_eq((int)t.num_cpus, 2);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

/* ===================================================================
 * D1.7.f -- sysfs-missing fallback. cpu_topology_probe must succeed
 * even on (impossible-on-Linux) CPU ids that have no sysfs files,
 * landing on the documented defaults (raw_capacity = 1024,
 * relative_capacity = 1.0 since freqs cancel through C_max).
 * =================================================================== */
PWTEST(cpu_topo_sysfs_missing_fallback)
{
	uint32_t cpus[] = { 1000000, 1000001 };
	struct cpu_topology t = { 0 };

	pwtest_int_eq(cpu_topology_probe(cpus, 2, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)t.num_cpus, 2);

	pwtest_double_eq(t.cpus[0].relative_capacity, 1.0);
	pwtest_double_eq(t.cpus[1].relative_capacity, 1.0);
	pwtest_int_eq((int)t.cpus[0].raw_capacity, 1024);
	pwtest_int_eq((int)t.cpus[1].raw_capacity, 1024);

	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

/* ===================================================================
 * D1.7.a (sysfs path) -- the live dev host is homogeneous, so probing
 * the actual /sys must yield relative_capacity_nominal = 1.0 across
 * every CPU. The target relative_capacity is bounded above by the
 * nominal one and equals it under assume-max policy (and on a host
 * whose min == max == single OPP). Probes cpu 0 (always online,
 * never isolated), which is enough to exercise the read paths. */
PWTEST(cpu_topo_sysfs_homogeneous_cpu0)
{
	uint32_t cpus[] = { 0 };
	struct cpu_topology t = { 0 };

	pwtest_int_eq(cpu_topology_probe(cpus, 1, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)t.num_cpus, 1);
	pwtest_double_eq(t.cpus[0].relative_capacity_nominal, 1.0);
	pwtest_bool_true(t.cpus[0].relative_capacity > 0.0 &&
			t.cpus[0].relative_capacity <= 1.0);
	pwtest_bool_true(t.cpus[0].relative_capacity <=
			t.cpus[0].relative_capacity_nominal);

	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

/* ===================================================================
 * Conservatism gap: when min_freq < max_freq the nominal vector
 * stays at 1.0 (we assume samples were collected at max_freq, the
 * smallest possible wall-clock time) while the target vector drops
 * to min_freq / max_freq under conservative policy. Under
 * assume-max the two vectors coincide. The asymmetry is what makes
 * sched_cb produce an inflated kernel budget large enough to stay
 * feasible at any governor-selected frequency.
 * =================================================================== */
PWTEST(cpu_topo_nominal_vs_target_under_conservative)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 2200000, max_freq_khz = 3800000 }"
		"] }";

	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_double_eq(t.cpus[0].relative_capacity_nominal, 1.0);
	pwtest_double_eq(t.cpus[0].relative_capacity, 2200000.0 / 3800000.0);
	cpu_topology_destroy(&t);

	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_ASSUME_MAX, &t), 0);
	pwtest_double_eq(t.cpus[0].relative_capacity_nominal, 1.0);
	pwtest_double_eq(t.cpus[0].relative_capacity, 1.0);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

PWTEST_SUITE(cpu_topology)
{
	pwtest_add(cpu_topo_homogeneous_json_all_ones, PWTEST_NOARG);
	pwtest_add(cpu_topo_two_capacity_classes, PWTEST_NOARG);
	pwtest_add(cpu_topo_dvfs_min_vs_max, PWTEST_NOARG);
	pwtest_add(cpu_topo_island_detection, PWTEST_NOARG);
	pwtest_add(cpu_topo_smt_pair_strict_refuses, PWTEST_NOARG);
	pwtest_add(cpu_topo_smt_pair_dedupe_drops_higher, PWTEST_NOARG);
	pwtest_add(cpu_topo_smt_pair_ignore_keeps_set, PWTEST_NOARG);
	pwtest_add(cpu_topo_sysfs_missing_fallback, PWTEST_NOARG);
	pwtest_add(cpu_topo_sysfs_homogeneous_cpu0, PWTEST_NOARG);
	pwtest_add(cpu_topo_nominal_vs_target_under_conservative, PWTEST_NOARG);

	return PWTEST_PASS;
}
