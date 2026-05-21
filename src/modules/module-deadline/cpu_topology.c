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

const char *rt_core_class_name(enum rt_core_class c)
{
	switch (c) {
	case RT_CORE_LITTLE:    return "little";
	case RT_CORE_BIG:       return "big";
	case RT_CORE_CLASS_N:   break;
	}
	return "unknown";
}

const char *cpu_freq_source_name(enum cpu_freq_source s)
{
	switch (s) {
	case CPU_FREQ_SCALING_MIN: return "scaling_min";
	case CPU_FREQ_SCALING_MAX: return "scaling_max";
	case CPU_FREQ_USER:        return "user";
	}
	return "unknown";
}

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

/* Compute the relative_capacity / relative_capacity_nominal pair
 * across all entries in t. Both vectors share the same denominator
 * (the maximum *nominal* effective capacity in the set), so the
 * nominal vector saturates at 1.0 on the fastest CPU(s) and the
 * target vector falls below that on hosts where freq_for_policy <
 * max_freq. Sets reference_cpu_index to the (deterministic, lowest)
 * index whose nominal capacity == 1.0. Must be invoked after
 * raw_capacity / min_freq_khz / max_freq_khz are filled. */
static void cpu_topology_recompute_relative(struct cpu_topology *t,
		enum cpu_dvfs_policy dvfs)
{
	uint32_t i;
	double max_nom = 0.0;
	double *nom, *tgt;

	if (!t || t->num_cpus == 0)
		return;

	nom = calloc(t->num_cpus, sizeof(*nom));
	tgt = calloc(t->num_cpus, sizeof(*tgt));
	if (!nom || !tgt) {
		free(nom);
		free(tgt);
		return;
	}

	for (i = 0; i < t->num_cpus; i++) {
		uint64_t target_freq = (dvfs == CPU_DVFS_ASSUME_MAX) ?
			t->cpus[i].max_freq_khz : t->cpus[i].min_freq_khz;
		uint64_t nominal_freq = t->cpus[i].max_freq_khz;
		if (target_freq == 0)
			target_freq = CPU_TOPO_DEFAULT_FREQ_KHZ;
		if (nominal_freq == 0)
			nominal_freq = CPU_TOPO_DEFAULT_FREQ_KHZ;
		nom[i] = (double)t->cpus[i].raw_capacity * (double)nominal_freq;
		tgt[i] = (double)t->cpus[i].raw_capacity * (double)target_freq;
		if (nom[i] > max_nom)
			max_nom = nom[i];
	}
	if (max_nom <= 0.0)
		max_nom = 1.0;

	for (i = 0; i < t->num_cpus; i++) {
		double rel_nom = nom[i] / max_nom;
		double rel_tgt = tgt[i] / max_nom;
		/* Defensive clamps: keep both strictly in (0, 1]; values
		 * outside that range break the DAG-library admission and
		 * the runtime denormalisation arithmetic. */
		if (rel_nom <= 0.0)
			rel_nom = 1.0 / max_nom;
		if (rel_nom > 1.0)
			rel_nom = 1.0;
		if (rel_tgt <= 0.0)
			rel_tgt = 1.0 / max_nom;
		if (rel_tgt > 1.0)
			rel_tgt = 1.0;
		t->cpus[i].relative_capacity_nominal = rel_nom;
		t->cpus[i].relative_capacity = rel_tgt;
	}

	t->reference_cpu_index = 0;
	for (i = 0; i < t->num_cpus; i++) {
		if (t->cpus[i].relative_capacity_nominal >= 1.0) {
			t->reference_cpu_index = i;
			break;
		}
	}

	/* Auto-classify into RT_CORE_LITTLE / RT_CORE_BIG by nominal
	 * capacity. A homogeneous host clusters every CPU at 1.0 and
	 * lands in RT_CORE_BIG -- the heterogeneous machinery degrades
	 * to homogeneous behaviour. Resolve sched_frequency_hz from the
	 * dvfs policy: conservative -> min_freq, assume_max -> max_freq.
	 * Both are converted from kHz to Hz so downstream code can
	 * uniformly use Hz. Probe-time auto-classification only stamps
	 * CPUs whose source has not already been set to USER; explicit
	 * user-frequency overrides survive a recompute. */
	for (i = 0; i < t->num_cpus; i++) {
		double rel_nom = t->cpus[i].relative_capacity_nominal;
		t->cpus[i].core_class = (rel_nom >= CPU_TOPOLOGY_BIG_THRESHOLD)
			? RT_CORE_BIG : RT_CORE_LITTLE;
		if (t->cpus[i].sched_frequency_source != CPU_FREQ_USER) {
			uint64_t freq_khz = (dvfs == CPU_DVFS_ASSUME_MAX) ?
				t->cpus[i].max_freq_khz :
				t->cpus[i].min_freq_khz;
			if (freq_khz == 0)
				freq_khz = CPU_TOPO_DEFAULT_FREQ_KHZ;
			t->cpus[i].sched_frequency_hz = freq_khz * 1000ULL;
			t->cpus[i].sched_frequency_source =
				(dvfs == CPU_DVFS_ASSUME_MAX) ?
				CPU_FREQ_SCALING_MAX : CPU_FREQ_SCALING_MIN;
		}
	}

	free(nom);
	free(tgt);
}

int cpu_topology_set_core_class(struct cpu_topology *t, uint32_t cpu_id,
		enum rt_core_class core_class)
{
	uint32_t i;
	if (!t || (unsigned)core_class >= RT_CORE_CLASS_N) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < t->num_cpus; i++) {
		if (t->cpus[i].cpu_id == cpu_id) {
			t->cpus[i].core_class = core_class;
			return 0;
		}
	}
	errno = ENOENT;
	return -1;
}

int cpu_topology_set_freq_override(struct cpu_topology *t, uint32_t cpu_id,
		uint64_t sched_frequency_hz)
{
	uint32_t i;
	if (!t || sched_frequency_hz == 0) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < t->num_cpus; i++) {
		if (t->cpus[i].cpu_id == cpu_id) {
			t->cpus[i].sched_frequency_hz = sched_frequency_hz;
			t->cpus[i].sched_frequency_source = CPU_FREQ_USER;
			return 0;
		}
	}
	errno = ENOENT;
	return -1;
}

int cpu_topology_resolve_frequencies(struct cpu_topology *t,
		enum cpu_freq_source default_source)
{
	uint32_t i;
	if (!t) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < t->num_cpus; i++) {
		if (t->cpus[i].sched_frequency_source == CPU_FREQ_USER)
			continue;
		uint64_t freq_khz;
		switch (default_source) {
		case CPU_FREQ_SCALING_MIN:
			freq_khz = t->cpus[i].min_freq_khz;
			break;
		case CPU_FREQ_SCALING_MAX:
			freq_khz = t->cpus[i].max_freq_khz;
			break;
		case CPU_FREQ_USER:
			/* Treat as no-op for default; refuse to invent a
			 * user frequency the operator did not supply. */
			continue;
		default:
			errno = EINVAL;
			return -1;
		}
		if (freq_khz == 0)
			freq_khz = CPU_TOPO_DEFAULT_FREQ_KHZ;
		t->cpus[i].sched_frequency_hz = freq_khz * 1000ULL;
		t->cpus[i].sched_frequency_source = default_source;
	}
	return 0;
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
		/* Both relative_capacity vectors are per-CPU scalars and
		 * independent of which siblings remain, so dedupe doesn't
		 * touch them. reference_cpu_index, however, indexes into the
		 * (now possibly shorter) cpus[] array; recompute it against
		 * the nominal vector, matching cpu_topology_recompute_relative. */
		t->reference_cpu_index = 0;
		for (uint32_t i = 0; i < t->num_cpus; i++) {
			if (t->cpus[i].relative_capacity_nominal >= 1.0) {
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

/* Match a JSON object's value against the literal core-class tokens
 * "little" / "big". Returns 1 if the value was consumed and decoded
 * into *out, 0 if the key doesn't match the literal "core_class",
 * -1 on malformed input. */
static int json_field_core_class(struct spa_json *o, const char *want,
		const char *key, int key_len, enum rt_core_class *out)
{
	const char *val;
	int val_len;
	char buf[16];
	if ((int)strlen(want) != key_len || strncmp(key, want, key_len) != 0)
		return 0;
	val_len = spa_json_next(o, &val);
	if (val_len <= 0)
		return -1;
	if (val_len >= (int)sizeof(buf))
		return -1;
	memcpy(buf, val, val_len);
	buf[val_len] = '\0';
	/* JSON strings come back wrapped in their delimiter; strip the
	 * leading quote if present so "big" and big both work. */
	const char *s = buf;
	if (*s == '"')
		s++;
	if (strncmp(s, "little", 6) == 0) {
		*out = RT_CORE_LITTLE;
		return 1;
	}
	if (strncmp(s, "big", 3) == 0) {
		*out = RT_CORE_BIG;
		return 1;
	}
	return -1;
}

/* Parse one { cpu_id=..., ... } object into ci. Returns 0 on success,
 * -1 on malformed input. Per-field presence flags let the caller fall
 * back on cpu_id-derived defaults only for fields that were actually
 * omitted (rather than explicitly set to 0). */
static int parse_json_cpu(struct spa_json *o, struct cpu_info *ci,
		bool *have_core_id, bool *have_island_id,
		bool *have_core_class, bool *have_user_freq_hz)
{
	const char *key;
	int key_len;
	uint64_t v;
	enum rt_core_class cc;
	int rc;
	bool got_cpu_id = false;

	*have_core_id = false;
	*have_island_id = false;
	*have_core_class = false;
	*have_user_freq_hz = false;

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

		rc = json_field_core_class(o, "core_class", key, key_len, &cc);
		if (rc < 0) return -1;
		if (rc > 0) { ci->core_class = cc; *have_core_class = true; continue; }

		rc = json_field_uint64(o, "user_frequency_hz", key, key_len, &v);
		if (rc < 0) return -1;
		if (rc > 0) {
			ci->sched_frequency_hz = v;
			ci->sched_frequency_source = CPU_FREQ_USER;
			*have_user_freq_hz = true;
			continue;
		}

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
		/* Side arrays remembering which fields the JSON pinned per
		 * CPU. We need them because cpu_topology_recompute_relative
		 * unconditionally derives core_class from
		 * relative_capacity_nominal; any explicit JSON override is
		 * re-applied after the recompute so the test affordance
		 * survives. */
		bool *json_core_class = calloc(cap, sizeof(*json_core_class));
		enum rt_core_class *json_cc = calloc(cap, sizeof(*json_cc));
		if (!cpus || !json_core_class || !json_cc) {
			free(cpus);
			free(json_core_class);
			free(json_cc);
			errno = ENOMEM;
			return -1;
		}
		uint32_t n = 0;

		while (spa_json_enter_object(&arr, &obj) > 0) {
			if (n == cap) {
				uint32_t new_cap = cap * 2;
				struct cpu_info *r = realloc(cpus, new_cap * sizeof(*cpus));
				bool *r_json_cc = realloc(json_core_class,
						new_cap * sizeof(*json_core_class));
				enum rt_core_class *r_cc = realloc(json_cc,
						new_cap * sizeof(*json_cc));
				if (!r || !r_json_cc || !r_cc) {
					free(r ? r : cpus);
					free(r_json_cc ? r_json_cc : json_core_class);
					free(r_cc ? r_cc : json_cc);
					errno = ENOMEM;
					return -1;
				}
				cpus = r;
				json_core_class = r_json_cc;
				json_cc = r_cc;
				cap = new_cap;
			}
			memset(&cpus[n], 0, sizeof(cpus[n]));
			cpus[n].raw_capacity = CPU_TOPO_DEFAULT_CAPACITY;
			cpus[n].min_freq_khz = CPU_TOPO_DEFAULT_FREQ_KHZ;
			cpus[n].max_freq_khz = CPU_TOPO_DEFAULT_FREQ_KHZ;
			bool have_core_id, have_island_id;
			bool have_core_class, have_user_freq_hz;
			if (parse_json_cpu(&obj, &cpus[n],
					&have_core_id, &have_island_id,
					&have_core_class, &have_user_freq_hz) < 0) {
				free(cpus);
				free(json_core_class);
				free(json_cc);
				goto bad;
			}
			/* Missing-field fallback: every CPU is its own core
			 * and its own island. Explicit zero in JSON is
			 * preserved. */
			if (!have_core_id)
				cpus[n].core_id = cpus[n].cpu_id;
			if (!have_island_id)
				cpus[n].island_id = cpus[n].cpu_id;
			json_core_class[n] = have_core_class;
			json_cc[n] = cpus[n].core_class;
			(void)have_user_freq_hz; /* survives through recompute */
			n++;
		}

		out->cpus = cpus;
		out->num_cpus = n;
		cpu_topology_fill_siblings(out);
		cpu_topology_recompute_relative(out, dvfs);
		/* Re-apply explicit JSON core_class overrides; the recompute
		 * pass stamps every CPU from capacity, so a homogeneous JSON
		 * fixture that asked for an artificial little/big split
		 * would otherwise lose the distinction. */
		for (uint32_t k = 0; k < n; k++) {
			if (json_core_class[k])
				out->cpus[k].core_class = json_cc[k];
		}
		free(json_core_class);
		free(json_cc);
		return 0;
	}

bad:
	free(out->cpus);
	memset(out, 0, sizeof(*out));
	errno = EINVAL;
	return -1;
}
