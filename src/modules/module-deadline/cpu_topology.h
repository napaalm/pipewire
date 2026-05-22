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

/* Heterogeneous-CPU class. The placer derives runtime estimates per
 * class instead of per CPU so a node moving between two CPUs of the
 * same class reuses statistics; a node moving between classes routes
 * into a different statistical state (the same key that partitions
 * the conformal estimator). RT_CORE_BIG is the default on a
 * homogeneous host: every CPU lands in the single high-capacity class,
 * which makes the heterogeneous machinery degrade gracefully to the
 * original homogeneous behaviour. */
enum rt_core_class {
	RT_CORE_LITTLE   = 0,
	RT_CORE_BIG      = 1,
	RT_CORE_CLASS_N  = 2,
};

const char *rt_core_class_name(enum rt_core_class c);

/* Source from which sched_frequency_hz was derived. The placer uses
 * the resolved value (in Hz) to convert cycle estimates to a kernel
 * runtime; the source is preserved for diagnostics. */
enum cpu_freq_source {
	CPU_FREQ_SCALING_MIN = 0,   /* /sys/.../scaling_min_freq * 1000 */
	CPU_FREQ_SCALING_MAX = 1,   /* /sys/.../scaling_max_freq * 1000 */
	CPU_FREQ_USER        = 2,   /* explicit user-frequency-hz */
};

const char *cpu_freq_source_name(enum cpu_freq_source s);

struct cpu_info {
	uint32_t cpu_id;
	uint32_t core_id;            /* physical core id (SMT sibling key) */
	uint32_t island_id;          /* canonical (min) CPU of its freq island */
	uint32_t smt_siblings[CPU_TOPOLOGY_MAX_SIBLINGS];
	uint32_t num_siblings;       /* siblings inside this->cpus only */
	uint64_t raw_capacity;       /* sysfs cpu_capacity, default 1024 */
	uint64_t min_freq_khz;
	uint64_t max_freq_khz;
	/* Heterogeneous-CPU class. Auto-assigned by
	 * cpu_topology_recompute_relative from relative_capacity_nominal
	 * (CPUs at the top of the capacity range -- within
	 * CPU_TOPOLOGY_BIG_THRESHOLD of the maximum -- land in
	 * RT_CORE_BIG, every other CPU in RT_CORE_LITTLE). A homogeneous
	 * host gets every CPU in RT_CORE_BIG. The module-level config
	 * cpus.classes may override the auto-assignment per-CPU at the
	 * cost of bypassing the capacity check; the JSON probe accepts
	 * core_class for unit-test scenarios. */
	enum rt_core_class core_class;
	/* Scheduling frequency in Hz used by the placer to convert a
	 * per-CPU-type cycle estimate into a kernel runtime budget:
	 *
	 *   runtime_ns = ceil(cycles_est * 1e9 / sched_frequency_hz)
	 *
	 * Distinct from the live cpufreq policy: the placer needs a
	 * deterministic value so admission and utilisation arithmetic
	 * are stable even when the governor varies the actual frequency.
	 * Resolved at probe time from cpu_topology_recompute_relative
	 * (default: freq_for_policy * 1000) or from an explicit
	 * user-frequency-hz override via cpu_topology_set_freq_override
	 * / cpus.freq.<id>.user-hz. */
	uint64_t sched_frequency_hz;
	enum cpu_freq_source sched_frequency_source;
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

/* Auto-classification threshold on relative_capacity_nominal. A CPU
 * whose nominal capacity is at or above the threshold lands in
 * RT_CORE_BIG; everything else in RT_CORE_LITTLE. Exposed so the unit
 * suite can pin the boundary value. */
#define CPU_TOPOLOGY_BIG_THRESHOLD 0.85

/* Override a single CPU's core class. Returns 0 on success, -1 with
 * errno set on ENOENT (cpu_id not present) or EINVAL (null inputs).
 * Useful for the fake big.LITTLE config knob (cpus.classes) on
 * homogeneous hardware: the operator declares which physical CPUs
 * should be treated as LITTLE for scheduling purposes even when their
 * underlying capacity is identical. */
int cpu_topology_set_core_class(struct cpu_topology *t, uint32_t cpu_id,
		enum rt_core_class core_class);

/* Override a single CPU's sched_frequency_hz with an explicit value
 * (in Hz, the same unit cpu_info::sched_frequency_hz uses) and stamp
 * sched_frequency_source = CPU_FREQ_USER. A zero or otherwise invalid
 * frequency is rejected with -1/EINVAL. Returns 0 on success,
 * -1/ENOENT on unknown cpu_id, -1/EINVAL on null inputs. */
int cpu_topology_set_freq_override(struct cpu_topology *t, uint32_t cpu_id,
		uint64_t sched_frequency_hz);

/* Resolve every CPU's sched_frequency_hz from the chosen freq-source
 * (scaling_min / scaling_max). User-overridden CPUs (source ==
 * CPU_FREQ_USER) keep their existing value. Returns 0 on success. */
int cpu_topology_resolve_frequencies(struct cpu_topology *t,
		enum cpu_freq_source default_source);

/* Recompute relative_capacity / relative_capacity_nominal from the
 * already-resolved sched_frequency_hz on every CPU. The default
 * probe-time recompute uses min_freq_khz / max_freq_khz from cpufreq
 * sysfs, which collapses to a uniform 1.0 vector on a host whose
 * cpufreq sysfs reports the same frequency for every CPU even when
 * `cpus.classes` and `cpus.freq.<id>.user-hz` have declared a fake
 * big.LITTLE split for the scheduler. After applying user
 * frequency overrides the caller should invoke this function so the
 * dag's capacity vector reflects the heterogeneity the operator
 * declared: every per-CPU entry becomes
 *
 *     raw_capacity[i] * sched_frequency_hz[i] /
 *         max_j(raw_capacity[j] * sched_frequency_hz[j])
 *
 * which mirrors the probe-time formula but substitutes the resolved
 * scheduling frequency for the cpufreq sysfs value. On a uniform
 * sched_frequency_hz vector the result is identical to the probe-time
 * value (every entry 1.0), so calling this function is safe even when
 * no user overrides are in effect. Returns 0 on success, -1/EINVAL on
 * a null topology. */
int cpu_topology_refresh_relative_from_sched_freq(struct cpu_topology *t);

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
