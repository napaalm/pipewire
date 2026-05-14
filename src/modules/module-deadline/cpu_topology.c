/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#include "cpu_topology.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/utils/json.h>

#include <pipewire/log.h>

PW_LOG_TOPIC_STATIC(cpu_topo_topic, "mod.deadline.cpu-topology");
#define PW_LOG_TOPIC_DEFAULT cpu_topo_topic

/* Sysfs default when /sys/.../cpu_capacity is missing. Matches the
 * kernel's "homogeneous, full capacity" sentinel and keeps relative
 * capacities at exactly 1.0 on hosts that don't report cpu_capacity. */
#define CPU_TOPO_DEFAULT_CAPACITY 1024u

/* Neutral frequency default when /sys/.../cpufreq/cpuinfo_{min,max}_freq
 * are missing. The value cancels through C_max so it never affects
 * relative_capacity; using 1 (kHz) keeps the arithmetic in finite-range
 * 64-bit space without overflow risk. */
#define CPU_TOPO_DEFAULT_FREQ_KHZ 1ULL

/* Read a single non-negative integer (decimal) from `path` into *out.
 * Returns 0 on success, -1 if the file is missing, malformed, or
 * empty. The caller is expected to fall back to a default value in
 * the -1 case -- this is intentional: hosts without cpu_capacity, EAS
 * knobs, or cpufreq should still produce a usable homogeneous
 * topology. */
static int read_uint64_file(const char *path, uint64_t *out)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;

	unsigned long long v;
	int n = fscanf(f, "%llu", &v);
	fclose(f);

	if (n != 1)
		return -1;

	*out = (uint64_t)v;
	return 0;
}

/* Read a sysfs cpu-list file (e.g. cpufreq/related_cpus,
 * topology/thread_siblings_list) and decode it into a sorted, deduped
 * array of CPU ids. The kernel emits two formats here interchangeably
 * across versions and files:
 *   - space-separated decimals (related_cpus):       "2 10"
 *   - comma/range list (thread_siblings_list):       "2,10"  or "0-3,8"
 * Both are handled by a single parser. Returns the number of cpus
 * decoded (0 if the file is missing or empty), and writes up to
 * cap entries into `out`. */
static uint32_t read_cpu_list_file(const char *path, uint32_t *out, uint32_t cap)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;

	char buf[256];
	size_t got = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);

	if (got == 0)
		return 0;
	buf[got] = '\0';

	uint32_t n = 0;
	char *p = buf;
	while (*p && n < cap) {
		while (*p && (*p == ' ' || *p == '\t' || *p == '\n' ||
				*p == ',' || *p == '\r'))
			p++;
		if (!*p)
			break;
		if (!isdigit((unsigned char)*p)) {
			p++;
			continue;
		}
		char *end;
		unsigned long lo = strtoul(p, &end, 10);
		p = end;
		unsigned long hi = lo;
		if (*p == '-') {
			p++;
			hi = strtoul(p, &end, 10);
			p = end;
		}
		for (unsigned long c = lo; c <= hi && n < cap; c++)
			out[n++] = (uint32_t)c;
	}

	/* Sort ascending and dedupe. n is small (typically 1 or 2). */
	for (uint32_t i = 1; i < n; i++) {
		for (uint32_t j = i; j > 0 && out[j - 1] > out[j]; j--) {
			uint32_t t = out[j - 1];
			out[j - 1] = out[j];
			out[j] = t;
		}
	}
	uint32_t w = 0;
	for (uint32_t r = 0; r < n; r++) {
		if (r > 0 && out[r] == out[w - 1])
			continue;
		out[w++] = out[r];
	}
	return w;
}

/* Compute relative_capacity[] across all entries in t, picking the
 * frequency per CPU according to `dvfs`. Sets reference_cpu_index to
 * the (deterministic, lowest) index whose relative_capacity == 1.0.
 * Must be invoked after raw_capacity / min_freq_khz / max_freq_khz
 * are filled. */
static void cpu_topology_recompute_relative(struct cpu_topology *t,
		enum cpu_dvfs_policy dvfs)
{
	uint32_t i;
	double max_eff = 0.0;
	double *eff;

	if (!t || t->num_cpus == 0)
		return;

	eff = calloc(t->num_cpus, sizeof(*eff));
	if (!eff)
		return;

	for (i = 0; i < t->num_cpus; i++) {
		uint64_t freq = (dvfs == CPU_DVFS_ASSUME_MAX) ?
			t->cpus[i].max_freq_khz : t->cpus[i].min_freq_khz;
		if (freq == 0)
			freq = CPU_TOPO_DEFAULT_FREQ_KHZ;
		eff[i] = (double)t->cpus[i].raw_capacity * (double)freq;
		if (eff[i] > max_eff)
			max_eff = eff[i];
	}
	if (max_eff <= 0.0)
		max_eff = 1.0;

	t->reference_cpu_index = 0;
	for (i = 0; i < t->num_cpus; i++) {
		double rel = eff[i] / max_eff;
		if (rel <= 0.0)
			rel = 1.0 / max_eff;     /* defensive: keep >0 */
		if (rel > 1.0)
			rel = 1.0;
		t->cpus[i].relative_capacity = rel;
	}
	for (i = 0; i < t->num_cpus; i++) {
		if (t->cpus[i].relative_capacity >= 1.0) {
			t->reference_cpu_index = i;
			break;
		}
	}
	free(eff);
}

/* Pass over `t` filling each cpu_info::smt_siblings/num_siblings with
 * the indices of the same core_id, restricted to the CPUs that are
 * actually present in this topology (the caller-provided set may be a
 * subset of online CPUs). Self is excluded from the list. */
static void cpu_topology_fill_siblings(struct cpu_topology *t)
{
	uint32_t i, j;

	for (i = 0; i < t->num_cpus; i++) {
		t->cpus[i].num_siblings = 0;
		for (j = 0; j < t->num_cpus; j++) {
			if (j == i)
				continue;
			if (t->cpus[j].core_id != t->cpus[i].core_id)
				continue;
			if (t->cpus[i].num_siblings >= CPU_TOPOLOGY_MAX_SIBLINGS)
				break;
			t->cpus[i].smt_siblings[t->cpus[i].num_siblings++] =
				t->cpus[j].cpu_id;
		}
	}
}

int cpu_topology_probe(const uint32_t *cpus, uint32_t num,
		enum cpu_dvfs_policy dvfs,
		struct cpu_topology *out)
{
	uint32_t i;
	char path[256];

	if (!cpus || num == 0 || !out) {
		errno = EINVAL;
		return -1;
	}

	memset(out, 0, sizeof(*out));
	out->cpus = calloc(num, sizeof(*out->cpus));
	if (!out->cpus) {
		errno = ENOMEM;
		return -1;
	}
	out->num_cpus = num;

	for (i = 0; i < num; i++) {
		uint32_t cpu = cpus[i];
		struct cpu_info *ci = &out->cpus[i];
		uint64_t v;
		uint32_t related[CPU_TOPOLOGY_MAX_SIBLINGS];
		uint32_t n_related;

		ci->cpu_id = cpu;
		ci->core_id = cpu;        /* fallback: every CPU is its own core */
		ci->island_id = cpu;      /* fallback: every CPU is its own island */
		ci->raw_capacity = CPU_TOPO_DEFAULT_CAPACITY;
		ci->min_freq_khz = CPU_TOPO_DEFAULT_FREQ_KHZ;
		ci->max_freq_khz = CPU_TOPO_DEFAULT_FREQ_KHZ;

		snprintf(path, sizeof(path),
				"/sys/devices/system/cpu/cpu%u/cpu_capacity", cpu);
		if (read_uint64_file(path, &v) == 0 && v > 0)
			ci->raw_capacity = v;

		snprintf(path, sizeof(path),
				"/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_min_freq", cpu);
		if (read_uint64_file(path, &v) == 0 && v > 0)
			ci->min_freq_khz = v;

		snprintf(path, sizeof(path),
				"/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", cpu);
		if (read_uint64_file(path, &v) == 0 && v > 0)
			ci->max_freq_khz = v;

		snprintf(path, sizeof(path),
				"/sys/devices/system/cpu/cpu%u/topology/core_id", cpu);
		if (read_uint64_file(path, &v) == 0)
			ci->core_id = (uint32_t)v;

		snprintf(path, sizeof(path),
				"/sys/devices/system/cpu/cpu%u/cpufreq/related_cpus", cpu);
		n_related = read_cpu_list_file(path, related,
				CPU_TOPOLOGY_MAX_SIBLINGS);
		if (n_related > 0)
			ci->island_id = related[0];  /* sorted ascending */
	}

	cpu_topology_fill_siblings(out);
	cpu_topology_recompute_relative(out, dvfs);

	return 0;
}

/* Pop entry at index `idx`, shifting tail forward. */
static void cpu_topology_drop_at(struct cpu_topology *t, uint32_t idx)
{
	if (idx >= t->num_cpus)
		return;
	if (idx < t->num_cpus - 1) {
		memmove(&t->cpus[idx], &t->cpus[idx + 1],
				(t->num_cpus - idx - 1) * sizeof(*t->cpus));
	}
	t->num_cpus--;
}

bool cpu_topology_has_smt_pair(const struct cpu_topology *t,
		uint32_t *out_a_cpu, uint32_t *out_b_cpu)
{
	uint32_t i, j;

	if (!t)
		return false;

	for (i = 0; i < t->num_cpus; i++) {
		for (j = i + 1; j < t->num_cpus; j++) {
			if (t->cpus[i].core_id != t->cpus[j].core_id)
				continue;
			if (out_a_cpu)
				*out_a_cpu = t->cpus[i].cpu_id;
			if (out_b_cpu)
				*out_b_cpu = t->cpus[j].cpu_id;
			return true;
		}
	}
	return false;
}

int cpu_topology_apply_smt_policy(struct cpu_topology *t,
		enum cpu_smt_policy policy,
		uint32_t *offending_a_cpu,
		uint32_t *offending_b_cpu)
{
	uint32_t a = 0, b = 0;

	if (!t) {
		errno = EINVAL;
		return -1;
	}

	switch (policy) {
	case CPU_SMT_IGNORE:
		if (cpu_topology_has_smt_pair(t, &a, &b)) {
			pw_log_warn("cpu-topology: SMT pair (cpu%u, cpu%u, core %u) "
					"accepted under smt-policy=ignore; per-CPU "
					"admission will overcount physical capacity",
					a, b, t->cpus[0].core_id);
		}
		return 0;
	case CPU_SMT_STRICT:
		if (cpu_topology_has_smt_pair(t, &a, &b)) {
			if (offending_a_cpu)
				*offending_a_cpu = a;
			if (offending_b_cpu)
				*offending_b_cpu = b;
			errno = EINVAL;
			return -1;
		}
		return 0;
	case CPU_SMT_DEDUPE:
	{
		/* Restart from scratch every time we drop an entry: indices
		 * shift, and the cleaner alternative is a quadratic pass
		 * whose cost is irrelevant for num_cpus << 100. */
		bool changed;
		do {
			changed = false;
			for (uint32_t i = 0; i < t->num_cpus; i++) {
				for (uint32_t j = i + 1; j < t->num_cpus; j++) {
					if (t->cpus[i].core_id != t->cpus[j].core_id)
						continue;
					pw_log_info("cpu-topology: drop SMT sibling cpu%u "
							"(kept cpu%u on core %u)",
							t->cpus[j].cpu_id,
							t->cpus[i].cpu_id,
							t->cpus[i].core_id);
					cpu_topology_drop_at(t, j);
					changed = true;
					break;
				}
				if (changed)
					break;
			}
		} while (changed);
		cpu_topology_fill_siblings(t);
		/* relative_capacity is independent of which siblings remain
		 * (it is a per-CPU scalar), but reference_cpu_index may need
		 * to move if a sibling was dropped. Recompute by reusing the
		 * existing min/max freq pair, derived from the surviving
		 * entries' relative_capacity values. */
		t->reference_cpu_index = 0;
		for (uint32_t i = 0; i < t->num_cpus; i++) {
			if (t->cpus[i].relative_capacity >= 1.0) {
				t->reference_cpu_index = i;
				break;
			}
		}
		return 0;
	}
	}

	errno = EINVAL;
	return -1;
}

void cpu_topology_destroy(struct cpu_topology *t)
{
	if (!t)
		return;
	free(t->cpus);
	t->cpus = NULL;
	t->num_cpus = 0;
	t->reference_cpu_index = 0;
}

/* ---------------------------------------------------------------
 * JSON test affordance
 * ---------------------------------------------------------------
 *
 * Parser for the schema documented in cpu_topology.h. Implemented in
 * terms of spa_json so we accept the relaxed PipeWire-style JSON the
 * rest of the daemon uses (unquoted keys, line comments).
 */

static int json_field_uint64(struct spa_json *o, const char *want_key,
		const char *key, int key_len, uint64_t *out)
{
	const char *val;
	int val_len;
	char buf[64];

	if ((size_t)key_len != strlen(want_key) ||
			strncmp(key, want_key, key_len) != 0)
		return 0;

	val_len = spa_json_next(o, &val);
	if (val_len <= 0)
		return -1;
	if (val_len >= (int)sizeof(buf))
		return -1;
	memcpy(buf, val, val_len);
	buf[val_len] = '\0';
	char *end;
	unsigned long long v = strtoull(buf, &end, 0);
	if (end == buf)
		return -1;
	*out = (uint64_t)v;
	return 1;
}

/* Parse one { cpu_id=..., ... } object into ci. Returns 0 on success,
 * -1 on malformed input. Per-field presence flags let the caller fall
 * back on cpu_id-derived defaults only for fields that were actually
 * omitted (rather than explicitly set to 0). */
static int parse_json_cpu(struct spa_json *o, struct cpu_info *ci,
		bool *have_core_id, bool *have_island_id)
{
	const char *key;
	int key_len;
	uint64_t v;
	int rc;
	bool got_cpu_id = false;

	*have_core_id = false;
	*have_island_id = false;

	while ((key_len = spa_json_next(o, &key)) > 0) {
		rc = json_field_uint64(o, "cpu_id", key, key_len, &v);
		if (rc < 0) return -1;
		if (rc > 0) { ci->cpu_id = (uint32_t)v; got_cpu_id = true; continue; }

		rc = json_field_uint64(o, "core_id", key, key_len, &v);
		if (rc < 0) return -1;
		if (rc > 0) { ci->core_id = (uint32_t)v; *have_core_id = true; continue; }

		rc = json_field_uint64(o, "island_id", key, key_len, &v);
		if (rc < 0) return -1;
		if (rc > 0) { ci->island_id = (uint32_t)v; *have_island_id = true; continue; }

		rc = json_field_uint64(o, "raw_capacity", key, key_len, &v);
		if (rc < 0) return -1;
		if (rc > 0) { ci->raw_capacity = v; continue; }

		rc = json_field_uint64(o, "min_freq_khz", key, key_len, &v);
		if (rc < 0) return -1;
		if (rc > 0) { ci->min_freq_khz = v; continue; }

		rc = json_field_uint64(o, "max_freq_khz", key, key_len, &v);
		if (rc < 0) return -1;
		if (rc > 0) { ci->max_freq_khz = v; continue; }

		/* Unknown field: skip its value. */
		const char *skip;
		if (spa_json_next(o, &skip) <= 0)
			return -1;
	}

	if (!got_cpu_id)
		return -1;
	return 0;
}

int cpu_topology_from_json(const char *json,
		enum cpu_dvfs_policy dvfs,
		struct cpu_topology *out)
{
	struct spa_json top, root, arr, obj;
	const char *key;
	int key_len;

	if (!json || !out) {
		errno = EINVAL;
		return -1;
	}

	memset(out, 0, sizeof(*out));

	spa_json_init(&top, json, strlen(json));
	if (spa_json_enter_object(&top, &root) <= 0) {
		/* Allow a bare top-level object form (the typical
		 * unquoted-key shape used elsewhere in PipeWire confs). */
		spa_json_init(&top, json, strlen(json));
		root = top;
	}

	while ((key_len = spa_json_next(&root, &key)) > 0) {
		if (key_len != (int)strlen("cpus") ||
				strncmp(key, "cpus", key_len) != 0) {
			const char *skip;
			if (spa_json_next(&root, &skip) <= 0)
				goto bad;
			continue;
		}

		if (spa_json_enter_array(&root, &arr) <= 0)
			goto bad;

		uint32_t cap = 8;
		struct cpu_info *cpus = calloc(cap, sizeof(*cpus));
		if (!cpus) {
			errno = ENOMEM;
			return -1;
		}
		uint32_t n = 0;

		while (spa_json_enter_object(&arr, &obj) > 0) {
			if (n == cap) {
				cap *= 2;
				struct cpu_info *r = realloc(cpus, cap * sizeof(*cpus));
				if (!r) {
					free(cpus);
					errno = ENOMEM;
					return -1;
				}
				cpus = r;
			}
			memset(&cpus[n], 0, sizeof(cpus[n]));
			cpus[n].raw_capacity = CPU_TOPO_DEFAULT_CAPACITY;
			cpus[n].min_freq_khz = CPU_TOPO_DEFAULT_FREQ_KHZ;
			cpus[n].max_freq_khz = CPU_TOPO_DEFAULT_FREQ_KHZ;
			bool have_core_id, have_island_id;
			if (parse_json_cpu(&obj, &cpus[n],
					&have_core_id, &have_island_id) < 0) {
				free(cpus);
				goto bad;
			}
			/* Missing-field fallback: every CPU is its own core
			 * and its own island. Explicit zero in JSON is
			 * preserved. */
			if (!have_core_id)
				cpus[n].core_id = cpus[n].cpu_id;
			if (!have_island_id)
				cpus[n].island_id = cpus[n].cpu_id;
			n++;
		}

		out->cpus = cpus;
		out->num_cpus = n;
		cpu_topology_fill_siblings(out);
		cpu_topology_recompute_relative(out, dvfs);
		return 0;
	}

bad:
	free(out->cpus);
	memset(out, 0, sizeof(*out));
	errno = EINVAL;
	return -1;
}
