/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#ifndef CPU_TOPOLOGY_H
#define CPU_TOPOLOGY_H

/*
 * cpu_topology.h
 *
 * Per-CPU capacity and topology probe for the P-EDF deadline placer.
 * For each CPU id in the caller-provided set, derive a scalar
 * relative_capacity in (0, 1] from sysfs (cpu_capacity, cpufreq min /
 * max frequency, frequency island, SMT siblings), so the DAG library
 * can compare per-task utilisation against per-CPU remaining capacity.
 *
 * Scaling model (linear, single-OPP per CPU):
 *
 *   effective_capacity[i] = raw_capacity[i] * freq_for_policy[i]
 *   C_max                 = max(effective_capacity[i])
 *   relative_capacity[i]  = effective_capacity[i] / C_max
 *
 * where freq_for_policy is min_freq for CPU_DVFS_CONSERVATIVE (the
 * default) or max_freq for CPU_DVFS_ASSUME_MAX. The conservative
 * choice guarantees that any budget admitted at analysis time fits at
 * runtime regardless of governor behaviour: if the CPU runs at any
 * f >= min_freq, real throughput exceeds the analysis assumption.
 *
 * The non-scalable component C_i^{ns} of the EETB scaling in
 * Power-Aware-Cucinotta.pdf (eq. 4) is assumed zero in this revision:
 * all execution time is treated as scaling linearly with capacity.
 * The struct layout reserves space for a future per-task non-scalable
 * fraction without changing the placer's contract.
 *
 * SMT-aware admission is layered on top of the per-CPU capacity: the
 * kernel admits each logical CPU separately, so two SMT siblings of
 * the same physical core get one budget each and the placer would
 * overcommit silently. cpu_topology_apply_smt_policy enforces one of
 *   - CPU_SMT_STRICT  refuse the CPU set on any sibling pair (default)
 *   - CPU_SMT_DEDUPE  drop the higher-numbered sibling per core
 *   - CPU_SMT_IGNORE  accept the set as given, log a warning
 *
 * The probe and the SMT policy are independent: probe always returns
 * the full set; apply_smt_policy mutates it in place.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_TOPOLOGY_MAX_SIBLINGS 8u

struct cpu_info {
	uint32_t cpu_id;
	uint32_t core_id;            /* physical core id (SMT sibling key) */
	uint32_t island_id;          /* canonical (min) CPU of its freq island */
	uint32_t smt_siblings[CPU_TOPOLOGY_MAX_SIBLINGS];
	uint32_t num_siblings;       /* siblings inside this->cpus only */
	uint64_t raw_capacity;       /* sysfs cpu_capacity, default 1024 */
	uint64_t min_freq_khz;
	uint64_t max_freq_khz;
	/* "Target" capacity scalar in (0, 1] -- raw_capacity *
	 * freq_for_policy, normalised by the maximum nominal capacity
	 * in the set. This is what the placer compares per-CPU load
	 * against, and what sched_cb divides the runtime budget by
	 * when shipping it to the kernel. Under cpus.dvfs-policy =
	 * conservative this reflects each CPU's min_freq, so the
	 * resulting kernel budget is guaranteed to be feasible at
	 * any governor-allowed frequency. */
	double   relative_capacity;
	/* "Nominal" capacity scalar in (0, 1] -- raw_capacity *
	 * max_freq, normalised by the maximum nominal capacity in
	 * the set. Always 1.0 for the fastest CPU(s). The sample
	 * normalisation in module-deadline scales every collected
	 * runtime by this value: since we don't actually know which
	 * cpufreq state the CPU was in at the moment of measurement
	 * (sysfs reads are not RT-safe), the conservative choice is
	 * to assume the sample was collected at max_freq, which is
	 * the smallest wall-clock time the same work could possibly
	 * take. That ensures the sketch never under-estimates the
	 * "reference-CPU" WCET regardless of governor behaviour. */
	double   relative_capacity_nominal;
};

struct cpu_topology {
	struct cpu_info *cpus;
	uint32_t         num_cpus;
	uint32_t         reference_cpu_index;
};

enum cpu_smt_policy {
	CPU_SMT_STRICT = 0,
	CPU_SMT_DEDUPE,
	CPU_SMT_IGNORE,
};

enum cpu_dvfs_policy {
	CPU_DVFS_CONSERVATIVE = 0,
	CPU_DVFS_ASSUME_MAX,
};

/* Probe sysfs for `num` CPUs (a contiguous array of cpu ids) and fill
 * `out`. `out->cpus` is heap-allocated; release with
 * cpu_topology_destroy. Missing sysfs files fall back to neutral
 * defaults (raw_capacity = 1024, freqs = 1) so a host without the
 * scheduler-EAS knobs still ends up homogeneous with
 * relative_capacity = 1.0 everywhere. Returns 0 on success, -1 with
 * errno set on allocation failure or invalid input. */
int  cpu_topology_probe(const uint32_t *cpus, uint32_t num,
		enum cpu_dvfs_policy dvfs,
		struct cpu_topology *out);

/* Apply the SMT policy in place. Returns 0 on accept (possibly with
 * fewer CPUs after dedupe), -1/EINVAL on refusal (STRICT with a
 * sibling pair). On refusal the caller can inspect `t` to learn which
 * pair tripped the policy via `offending_a_cpu` / `offending_b_cpu`
 * (set when -1 is returned, else unchanged). */
int  cpu_topology_apply_smt_policy(struct cpu_topology *t,
		enum cpu_smt_policy policy,
		uint32_t *offending_a_cpu,
		uint32_t *offending_b_cpu);

/* Convenience: returns false if any pair of CPUs in `t` share a
 * physical core (i.e. are SMT siblings). */
bool cpu_topology_has_smt_pair(const struct cpu_topology *t,
		uint32_t *out_a_cpu, uint32_t *out_b_cpu);

/* Release `t->cpus`. Safe to call on a zero-initialised cpu_topology. */
void cpu_topology_destroy(struct cpu_topology *t);

/*
 * Test affordance: build a cpu_topology directly from a JSON string,
 * skipping the sysfs probe. JSON shape:
 *
 *   {
 *     "cpus": [
 *       { "cpu_id": 0, "core_id": 0, "island_id": 0,
 *         "raw_capacity": 1024,
 *         "min_freq_khz": 2200000, "max_freq_khz": 3000000 },
 *       ...
 *     ]
 *   }
 *
 * Missing per-CPU fields fall back to the same defaults as the sysfs
 * probe. `relative_capacity` is computed from the chosen dvfs policy.
 */
int cpu_topology_from_json(const char *json,
		enum cpu_dvfs_policy dvfs,
		struct cpu_topology *out);

#ifdef __cplusplus
}
#endif

#endif /* CPU_TOPOLOGY_H */
