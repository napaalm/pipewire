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

/* ===================================================================
 * Heterogeneous core-class auto-classification: when capacities span
 * the BIG-threshold, the recompute pass tags top-of-range CPUs as
 * RT_CORE_BIG and everything else as RT_CORE_LITTLE. A homogeneous
 * fixture lands every CPU in RT_CORE_BIG so the degenerate case keeps
 * the original semantics.
 * =================================================================== */
PWTEST(cpu_topo_core_class_homogeneous_all_big)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 },"
		"  { cpu_id = 1, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 }"
		"] }";
	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)t.cpus[0].core_class, RT_CORE_BIG);
	pwtest_int_eq((int)t.cpus[1].core_class, RT_CORE_BIG);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

PWTEST(cpu_topo_core_class_split_by_capacity)
{
	/* Two raw-capacity tiers: 1024 (BIG) and 512 (LITTLE). The
	 * threshold is 0.85 and the small CPU sits at 0.5 nominal, so
	 * it lands in RT_CORE_LITTLE. */
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 },"
		"  { cpu_id = 1, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 },"
		"  { cpu_id = 2, raw_capacity = 512,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 },"
		"  { cpu_id = 3, raw_capacity = 512,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 }"
		"] }";
	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)find_cpu(&t, 0)->core_class, RT_CORE_BIG);
	pwtest_int_eq((int)find_cpu(&t, 1)->core_class, RT_CORE_BIG);
	pwtest_int_eq((int)find_cpu(&t, 2)->core_class, RT_CORE_LITTLE);
	pwtest_int_eq((int)find_cpu(&t, 3)->core_class, RT_CORE_LITTLE);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

PWTEST(cpu_topo_core_class_json_override_survives_recompute)
{
	/* Capacity is homogeneous so the recompute pass would auto-stamp
	 * BIG everywhere. Pinning core_class via JSON must survive the
	 * recompute; this is the path the cpus.classes config knob
	 * reuses on homogeneous hardware. */
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000,"
		"    core_class = \"little\" },"
		"  { cpu_id = 1, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000,"
		"    core_class = \"big\" }"
		"] }";
	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)find_cpu(&t, 0)->core_class, RT_CORE_LITTLE);
	pwtest_int_eq((int)find_cpu(&t, 1)->core_class, RT_CORE_BIG);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

PWTEST(cpu_topo_set_core_class_overrides_and_unknown_fails)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 7, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 }"
		"] }";
	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)t.cpus[0].core_class, RT_CORE_BIG);
	pwtest_int_eq(cpu_topology_set_core_class(&t, 7, RT_CORE_LITTLE), 0);
	pwtest_int_eq((int)t.cpus[0].core_class, RT_CORE_LITTLE);

	errno = 0;
	pwtest_int_eq(cpu_topology_set_core_class(&t, 999, RT_CORE_LITTLE), -1);
	pwtest_int_eq(errno, ENOENT);

	errno = 0;
	pwtest_int_eq(cpu_topology_set_core_class(NULL, 7, RT_CORE_LITTLE), -1);
	pwtest_int_eq(errno, EINVAL);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

/* ===================================================================
 * Per-CPU sched_frequency_hz: resolved from kHz * 1000 with the dvfs
 * policy on probe, overridable by cpu_topology_set_freq_override
 * (stamps source = CPU_FREQ_USER) and by cpu_topology_resolve_frequencies
 * (rewrites non-user CPUs to a chosen source).
 * =================================================================== */
PWTEST(cpu_topo_sched_frequency_resolved_from_dvfs_policy)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 2200000, max_freq_khz = 3800000 }"
		"] }";
	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq((int)t.cpus[0].sched_frequency_source, CPU_FREQ_SCALING_MIN);
	pwtest_int_eq((long)t.cpus[0].sched_frequency_hz, 2200000L * 1000L);
	cpu_topology_destroy(&t);

	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_ASSUME_MAX, &t), 0);
	pwtest_int_eq((int)t.cpus[0].sched_frequency_source, CPU_FREQ_SCALING_MAX);
	pwtest_int_eq((long)t.cpus[0].sched_frequency_hz, 3800000L * 1000L);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

PWTEST(cpu_topo_set_freq_override_pins_source_to_user)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 4, raw_capacity = 1024,"
		"    min_freq_khz = 2200000, max_freq_khz = 3800000 }"
		"] }";
	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq(cpu_topology_set_freq_override(&t, 4, 2500000000ULL), 0);
	pwtest_int_eq((int)t.cpus[0].sched_frequency_source, CPU_FREQ_USER);
	pwtest_int_eq((long)t.cpus[0].sched_frequency_hz, 2500000000L);

	errno = 0;
	pwtest_int_eq(cpu_topology_set_freq_override(&t, 4, 0), -1);
	pwtest_int_eq(errno, EINVAL);
	errno = 0;
	pwtest_int_eq(cpu_topology_set_freq_override(&t, 999, 2500000000ULL), -1);
	pwtest_int_eq(errno, ENOENT);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

PWTEST(cpu_topo_resolve_frequencies_preserves_user_override)
{
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 2200000, max_freq_khz = 3800000 },"
		"  { cpu_id = 1, raw_capacity = 1024,"
		"    min_freq_khz = 2200000, max_freq_khz = 3800000 }"
		"] }";
	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	pwtest_int_eq(cpu_topology_set_freq_override(&t, 1, 2500000000ULL), 0);
	pwtest_int_eq(cpu_topology_resolve_frequencies(&t, CPU_FREQ_SCALING_MAX), 0);
	/* cpu_id=0 picked up the scaling_max value. */
	pwtest_int_eq((long)find_cpu(&t, 0)->sched_frequency_hz, 3800000L * 1000L);
	pwtest_int_eq((int)find_cpu(&t, 0)->sched_frequency_source,
			CPU_FREQ_SCALING_MAX);
	/* cpu_id=1 kept its USER value. */
	pwtest_int_eq((long)find_cpu(&t, 1)->sched_frequency_hz, 2500000000L);
	pwtest_int_eq((int)find_cpu(&t, 1)->sched_frequency_source, CPU_FREQ_USER);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

PWTEST(cpu_topo_refresh_relative_from_sched_freq_yields_ratio)
{
	/* Two CPUs with identical raw_capacity but user-set
	 * sched_frequency_hz at a 2:1 ratio. After the refresh, the
	 * relative_capacity vector must reflect that ratio: the faster
	 * CPU lands at 1.0, the slower at 0.5. */
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 },"
		"  { cpu_id = 1, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 }"
		"] }";
	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_CONSERVATIVE, &t), 0);
	/* Probe-time pass produces a uniform vector. */
	pwtest_double_eq(find_cpu(&t, 0)->relative_capacity,
			find_cpu(&t, 1)->relative_capacity);

	/* User-declare a fake big.LITTLE: cpu0 at 3 GHz, cpu1 at 1.5 GHz. */
	pwtest_int_eq(cpu_topology_set_freq_override(&t, 0, 3000000000ULL), 0);
	pwtest_int_eq(cpu_topology_set_freq_override(&t, 1, 1500000000ULL), 0);

	pwtest_int_eq(cpu_topology_refresh_relative_from_sched_freq(&t), 0);
	pwtest_double_eq(find_cpu(&t, 0)->relative_capacity, 1.0);
	pwtest_double_eq(find_cpu(&t, 1)->relative_capacity, 0.5);
	pwtest_double_eq(find_cpu(&t, 0)->relative_capacity_nominal, 1.0);
	pwtest_double_eq(find_cpu(&t, 1)->relative_capacity_nominal, 0.5);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

PWTEST(cpu_topo_refresh_relative_from_sched_freq_uniform_is_noop)
{
	/* On a uniform sched_frequency vector the refresh must leave
	 * every entry at 1.0 -- the function must not introduce
	 * spurious heterogeneity. */
	const char *json =
		"{ cpus = ["
		"  { cpu_id = 0, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 },"
		"  { cpu_id = 1, raw_capacity = 1024,"
		"    min_freq_khz = 3000000, max_freq_khz = 3000000 }"
		"] }";
	struct cpu_topology t = { 0 };
	pwtest_int_eq(cpu_topology_from_json(json, CPU_DVFS_ASSUME_MAX, &t), 0);
	pwtest_int_eq(cpu_topology_resolve_frequencies(&t, CPU_FREQ_SCALING_MAX), 0);
	pwtest_int_eq(cpu_topology_refresh_relative_from_sched_freq(&t), 0);
	pwtest_double_eq(find_cpu(&t, 0)->relative_capacity, 1.0);
	pwtest_double_eq(find_cpu(&t, 1)->relative_capacity, 1.0);
	cpu_topology_destroy(&t);
	return PWTEST_PASS;
}

PWTEST(cpu_topo_refresh_relative_null_safe)
{
	pwtest_int_eq(cpu_topology_refresh_relative_from_sched_freq(NULL), -1);
	pwtest_int_eq(errno, EINVAL);
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

	pwtest_add(cpu_topo_core_class_homogeneous_all_big, PWTEST_NOARG);
	pwtest_add(cpu_topo_core_class_split_by_capacity, PWTEST_NOARG);
	pwtest_add(cpu_topo_core_class_json_override_survives_recompute,
			PWTEST_NOARG);
	pwtest_add(cpu_topo_set_core_class_overrides_and_unknown_fails,
			PWTEST_NOARG);
	pwtest_add(cpu_topo_sched_frequency_resolved_from_dvfs_policy,
			PWTEST_NOARG);
	pwtest_add(cpu_topo_set_freq_override_pins_source_to_user,
			PWTEST_NOARG);
	pwtest_add(cpu_topo_resolve_frequencies_preserves_user_override,
			PWTEST_NOARG);
	pwtest_add(cpu_topo_refresh_relative_from_sched_freq_yields_ratio,
			PWTEST_NOARG);
	pwtest_add(cpu_topo_refresh_relative_from_sched_freq_uniform_is_noop,
			PWTEST_NOARG);
	pwtest_add(cpu_topo_refresh_relative_null_safe, PWTEST_NOARG);

	return PWTEST_PASS;
}
