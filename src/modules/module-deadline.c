/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */
/***
  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
***/

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <stdbool.h>
#include <sched.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <linux/sched.h>
#include <pthread.h>

#include "config.h"

#include "module-deadline/conformal.h"
#include "module-deadline/cpu_topology.h"
#include "module-deadline/dag.h"
#include "module-deadline/diag.h"
#include "module-deadline/reconcile.h"
#include "module-deadline/sched_groups.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/atomic.h>

#include <pipewire/private.h>
#include <pipewire/impl.h>
#include <pipewire/thread.h>
#include <pipewire/thread-loop.h>

/** \page page_module_deadline Deadline
 *
 * The `deadline` module enables the use of the SCHED_DEADLINE scheduling policy
 * on processing threads of nodes in the graph.
 *
 * The module determines dynamic parameters for the SCHED_DEADLINE policy by
 * analyzing the graph of nodes in the context. It calculates the worst-case
 * execution time (WCET) of each node and the period of the processing thread.
 * It then uses some heuristics to determine the runtime and deadline of each
 * thread, based on the WCETs and the graph latency constraints.
 * It also distributes the threads across the available CPUs, using a worst-fit
 * strategy.
 *
 * Note: This module depends on the context.dynamic-data-loops property. If the
 * property is not set on all configurations, the module will refuse to work.
 * Also, to use the SCHED_DEADLINE policy, the pipewire executable must be given
 * the CAP_SYS_NICE capability.
 *
 * ## Module Name
 *
 * `libpipewire-module-deadline`
 *
 * ## Module Options
 *
 * - `cpus.available`: The list of CPUs to which the threads are bound.
 *                       If unset or empty, the default is every CPU
 *                       currently reachable through the pipewire
 *                       process' affinity mask (typically every online
 *                       CPU). Restrict explicitly only when you want a
 *                       subset of cores reserved for audio.
 * - `cpus.utilization`: The maximum CPU utilization (per core) that DEADLINE
 *                       threads are allowed to consume. The default is 0.95.
 * - `cpus.smt-policy`:  How to handle SMT-paired logical CPUs within
 *                       `cpus.available`. `strict` (the default) refuses
 *                       the module load if any two CPUs in the set share
 *                       a physical core: the kernel admission is per
 *                       logical CPU and would silently overcommit the
 *                       physical capacity. `dedupe` keeps the lowest-id
 *                       sibling per core and drops the others. `ignore`
 *                       accepts the set unchanged and logs a warning;
 *                       only useful for diagnostic comparisons.
 * - `cpus.dvfs-policy`: Which cpufreq frequency to use when computing
 *                       per-CPU capacity. `conservative` (the default)
 *                       uses `cpuinfo_min_freq`: any budget that fits
 *                       at analysis time is guaranteed to fit at
 *                       runtime regardless of governor behaviour, since
 *                       real throughput exceeds the min-freq assumption
 *                       whenever the governor parks the CPU higher.
 *                       `assume-max` uses `cpuinfo_max_freq`; admission
 *                       is tighter but a governor that parks below
 *                       max_freq can cause deadline overruns.
 * - `cpus.topology-override`: Hidden test affordance. JSON string
 *                       describing the per-CPU topology (capacities,
 *                       core ids, freqs) that bypasses sysfs probing.
 *                       Used by the heterogeneous live test to fake
 *                       a two-class capacity layout on a homogeneous
 *                       host. See cpu_topology_from_json for the
 *                       schema.
 * - `sched.reclaim`:    Whether to set SCHED_FLAG_RECLAIM (GRUB) on
 *                       every sched_setattr. Default true.
 *                       Setting it to false puts the module into a
 *                       strict-budget mode where any per-job
 *                       overrun surfaces immediately as a deadline
 *                       miss instead of being absorbed by reclaimed
 *                       bandwidth from idle peers. Used by the
 *                       saturation live test to verify the runtime
 *                       budget has not under-bounded the worst
 *                       case; not recommended for production audio
 *                       graphs.
 * - `deadline.budget.source`:
 *                       Which budget source the runtime-selection
 *                       predicate considers. Accepted values:
 *                       `manual`, `deterministic`,
 *                       `adaptive_conformal` (default automatic),
 *                       `bootstrap`. The hierarchy is
 *                       manual -> deterministic -> adaptive_conformal
 *                       -> peak-hold-bootstrap; restricting via this
 *                       knob skips kinds above the chosen layer.
 *                       Strict hard-realtime operation requires
 *                       `manual` or `deterministic` (a configured
 *                       static bound); `adaptive_conformal` is a
 *                       soft / weakly-hard estimate, not a
 *                       deterministic WCET (Bernat, Burns & Llamosi
 *                       2001).
 * - `deadline.conformal.alpha_graph` / `alpha_min` / `alpha_max`:
 *                       Target graph-level overrun frequency and
 *                       the clamps the adaptive alpha update
 *                       respects (Gibbs & Candes 2021). Defaults
 *                       1e-3 / 1e-5 / 5e-2.
 * - `deadline.conformal.eta`:
 *                       Step size of the adaptive alpha update
 *                       per observed activation. Default 5e-3.
 * - `deadline.conformal.window`:
 *                       Score-ring size (number of normalised
 *                       nonconformity scores retained per
 *                       follower). Must be in [2, 4096]. Default
 *                       1024.
 * - `deadline.conformal.recalc_period`:
 *                       Number of observations between rolling
 *                       quantile recomputes. Default 1
 *                       (recompute every sample).
 * - `deadline.conformal.ewma_location_lambda` /
 *   `deadline.conformal.ewma_scale_lambda`:
 *                       EWMA gains for the location and
 *                       absolute-deviation scale predictors.
 *                       Default 0.05 / 0.05.
 * - `deadline.conformal.guard_ns` / `guard_percent`:
 *                       Additive and multiplicative guards
 *                       applied to the raw prediction before
 *                       clamping. Default 1500 ns / 5 %.
 * - `deadline.conformal.sigma_floor_ns`:
 *                       Lower bound on the EWMA scale used in
 *                       score normalisation; prevents division by
 *                       near-zero scale on stable inputs.
 *                       Default 1 ns.
 * - `deadline.conformal.runtime_floor_ns`:
 *                       Lower clamp on the emitted budget.
 *                       Default 1000 ns.
 * - `deadline.conformal.bootstrap_min_samples`:
 *                       Number of observations required before the
 *                       estimator transitions BOOTSTRAP -> VALID.
 *                       Default 64.
 * - `deadline.conformal.bootstrap_runtime_ns`:
 *                       Bootstrap floor published while the state
 *                       machine is in BOOTSTRAP. Default 0 (use
 *                       runtime_floor_ns).
 * - `deadline.conformal.compatible_history`:
 *                       Whether the BOOTSTRAP path may reuse a
 *                       compatible-history budget when a previously
 *                       observed mode-key fingerprint recurs.
 *                       Default true.
 * - `deadline.conformal.trace_export` /
 *   `deadline.conformal.trace_path`:
 *                       When both set, the worker appends one
 *                       JSON line per sample to the configured
 *                       path. Input to the offline calibration
 *                       tool `live-test/conformal_calibrate.py`.
 *                       Default off.
 * - `recalc.sync`:      If true (default false), run the parameter
 *                       recalculation synchronously on the driver's RT
 *                       data-loop thread, as the module did before the
 *                       async refactor. Useful only for benchmarking
 *                       the cost the async path removes from the RT
 *                       path. Production should leave it false.
 * - `recalc.rt-prio`:   SCHED_FIFO priority of the async recalculation
 *                       worker thread. Must be strictly lower than the
 *                       audio processing priority (PipeWire defaults
 *                       client=83, server=88; min configurable=11), so
 *                       the worker never preempts audio. The worker is
 *                       also pinned away from the deadline cores
 *                       (cpus.available) so it never shares CPU time
 *                       with the SCHED_DEADLINE threads it configures.
 *                       Default 5.
 * - `recalc.fake-delay-us`: Debug knob. If non-zero, the worker sleeps
 *                       this many microseconds inside each recalc
 *                       callback to simulate a slow analysis stage and
 *                       exercise the CAS-coalesced "many RT cycles per
 *                       worker wake" path. Default 0.
 * - `recalc.persistent`: If true (default), the worker keeps a
 *                       persistent DAG across wakes and reconciles
 *                       it incrementally. A no-op cycle (no
 *                       topology change, no significant WCET
 *                       drift) does zero analysis work. If false,
 *                       the worker falls back to the legacy
 *                       destroy-and-rebuild path -- a permanent
 *                       kill switch for the persistent-DAG code,
 *                       intended only for diagnostic comparisons.
 * - `wcet.recalc-threshold`: Fractional WCET drift required to
 *                       update the cached DAG node and trigger a
 *                       recalc. With the default 0.01 a follower
 *                       whose reported WCET drifts by less than
 *                       1 % from the cached value is treated as a
 *                       no-op (the cached schedule stays valid).
 *                       Setting to 0 disables the gate (every
 *                       reported change marks dirty). Must be in
 *                       [0, 1).
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-deadline
 *     args = {
 *         # cpus.available defaults to every CPU the process can run
 *         # on; uncomment to restrict explicitly.
 *         # cpus.available = [ 2 3 4 5 6 7 ]
 *         cpus.utilization = 0.95
 *     }
 *     flags = [ ifexists nofail ]
 * }
 * ]
 *\endcode
 */

#define NAME "deadline"

#define MAX_CPUS 128

/* --- */
/* Async worker defaults. */

/* SCHED_FIFO priority of the worker thread. Strictly lower than every
 * audio-processing thread, so the worker cannot preempt audio. The
 * meson options rtprio-server/rtprio-client clamp PipeWire's audio
 * threads to >=11; we pick 5 to leave clear headroom on both sides
 * (>0 means SCHED_FIFO at all; <11 means audio threads always win). */
#define WORKER_DEFAULT_RT_PRIO		5

/* Per-driver SPSC sample ring capacity, in slots of struct sample.
 * Power of two for cheap modulo. Sized for the case where the worker
 * is a few periods behind the RT thread: at 48 kHz with an 8-frame
 * quantum every cycle emits ~30 samples; 4096 slots = ~130 cycles
 * worth, well beyond any plausible worker lag. Overflow policy is
 * drop-oldest (we just stop writing), which the WCET sketch absorbs
 * naturally. */
#define WORKER_RING_CAPACITY		4096u

/* Maximum nodes/edges a topology snapshot can hold. The snapshot
 * lives on the driver sentinel; arrays are realloc()d at snapshot
 * time on the main loop if needed. Initial value is just the starting
 * allocation. */
#define TOPO_INITIAL_NODES		64u
#define TOPO_INITIAL_EDGES		128u

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	"( cpu.available=<list of CPUs> ) "	\
			"( cpu.utilization=<percentage> ) "

/* PipeWire does not assume glibc. Detect the scheduler ABI pieces at
 * configure time and provide only the missing Linux fallbacks here.
 *
 * Before libc headers exposed struct sched_attr, the layout came from the
 * Linux sched_setattr(2) kernel ABI.
 */
#if !HAVE_STRUCT_SCHED_ATTR
struct sched_attr {
	uint32_t size;
	uint32_t sched_policy;
	uint64_t sched_flags;
	int32_t sched_nice;
	uint32_t sched_priority;
	uint64_t sched_runtime;
	uint64_t sched_deadline;
	uint64_t sched_period;
	uint32_t sched_util_min;
	uint32_t sched_util_max;
};
#endif

#if !HAVE_SCHED_SETATTR
static int sched_setattr(pid_t tid, struct sched_attr *attr, unsigned int flags)
{
	return syscall(SYS_sched_setattr, tid, attr, flags);
}
#endif

#if !HAVE_SCHED_GETATTR
static SPA_UNUSED int sched_getattr(pid_t tid, struct sched_attr *attr,
		unsigned int size, unsigned int flags)
{
	return syscall(SYS_sched_getattr, tid, attr, size, flags);
}
#endif

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Antonio Napolitano <antonio.napolitano@santannapisa.it> and Francesco Barcherini <francesco.barcherini@santannapisa.it>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Use SCHED_DEADLINE for processing threads" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

/* A single RT->worker sample. node_id+period_ns are enough for the
 * worker to look up (or lazily create) the node estimator and to
 * detect period changes. cpu carries the CPU id the follower thread
 * ran on when the runtime was measured (its current SCHED_DEADLINE
 * placement, since followers are pinned); the worker uses it to
 * normalise the sample into reference-CPU units before feeding the
 * sketch, so the digest always holds WCETs "as if measured on the
 * fastest CPU". SAMPLE_CPU_UNKNOWN means the follower has not yet
 * been placed (e.g. very first cycle after registration) and the
 * worker treats the sample as already reference-CPU normalised.
 *
 * cycles is the perf_event_open(CPU_CYCLES) delta the executor
 * captured around the node's process() call (impl-node.c writes it
 * into pw_node_activation::prev_run_cycles). Zero means the kernel
 * refused perf_event_open (paranoid > 1 typically) so we fall back
 * on the wall-clock path that conservatively assumes the sample was
 * collected at max_freq.
 *
 * The RT thread writes; the worker reads. */
#define SAMPLE_CPU_UNKNOWN UINT32_MAX

struct sample {
	uint32_t node_id;
	uint32_t cpu;
	uint64_t runtime_ns;
	uint64_t cycles;
	uint64_t period_ns;
};

/* Topology snapshot for one driver. Populated only on the main loop
 * (inside snapshot_topology() dispatched via pw_loop_invoke); read
 * only by the worker, between the moment the main-loop callback
 * returns and the moment we issue the next snapshot request. No
 * other reader exists, so no locking. */
struct topo_node {
	uint32_t id;
	pid_t    tid;
};
struct topo_edge {
	uint32_t src;
	uint32_t dst;
};
struct topo_snap {
	struct topo_node *nodes;
	struct topo_edge *edges;
	uint32_t n_nodes, nodes_cap;
	uint32_t n_edges, edges_cap;
	uint64_t period;
	bool     ok;
	uint32_t pending;  /* atomic, CAS-coalesces async snapshot invokes */

	/* Monotonic generation counter incremented (release store) at
	 * the end of every successful snapshot_topology_main. Workers
	 * read it via acquire load and compare against
	 * drv->topo_gen_applied to decide whether a topology-reconcile
	 * pass is needed this wake. Wraps at UINT64_MAX which is
	 * effectively never in any realistic deployment. */
	uint64_t generation;
};

/* struct sched_group is defined in module-deadline/sched_groups.h
 * (extracted so it can be unit-tested in isolation). */

struct node {
	struct spa_list link;
	struct impl *impl;

	struct pw_impl_node *node;        /* may be NULL on worker-created
					   * follower entries; lookup by id
					   * goes through `node_id` below. */
	uint32_t node_id;                 /* always set; mirrors node->info.id
					   * when `node` is known. */
	struct spa_hook node_rt_listener;

	bool enabled:1;
	bool is_driver:1;

	/* Per-node peak-hold WCET, in reference-CPU units. Only
	 * meaningful for follower nodes (is_driver=false); the driver
	 * sentinel keeps zeros. The conformal estimator below owns the
	 * online prediction; n->wcet retains the running max so the
	 * selection predicate can fall back on a peak-hold floor while
	 * the estimator is in its bootstrap window. */
	uint64_t wcet;
	uint64_t period;

	/* Adaptive-conformal upper-runtime-budget estimator
	 * (Romano, Patterson & Candes 2019; Gibbs & Candes 2021).
	 * Consumes the per-cycle CPU-time samples and publishes a
	 * soft / weakly-hard (Bernat, Burns & Llamosi 2001) one-sided
	 * budget. NULL until the first sample arrives (lazy init). */
	rt_conformal_t *conformal;

	/* Per-follower budget kind chosen by runtime_select_for_node
	 * on the most recent sample. Surfaced in the JSON snapshot so
	 * an operator can audit which source drove this cycle's
	 * kernel runtime. */
	enum rt_diag_budget_kind budget_kind;

	/* Last-applied SCHED_DEADLINE tuple. Used by sched_cb to skip
	 * a sched_setattr+sched_setaffinity pair when the four
	 * components (runtime, deadline, period, cpu) all match the
	 * previously-applied values. Invalidated on apply failure so
	 * the next reconcile retries. last_applied=false means "never
	 * applied; first call must issue the syscalls". */
	uint64_t last_runtime;
	uint64_t last_deadline;            /* kernel-API local deadline */
	uint64_t last_cumulative_deadline; /* graph-relative milestone, for snapshots */
	uint64_t last_period;
	uint32_t last_cpu;
	bool     last_applied;

	/* Last fusion-group leader id stamped by sched_cb. Used to
	 * detect a change in macro-node membership across reconcile
	 * passes so the per-follower MBPTA estimator can be
	 * invalidated (Cucu-Grosjean 2012 §IV: a contracted node is
	 * a fresh probabilistic-timing subject). _seen=false on
	 * first ever sched_cb so the initial leader is just
	 * recorded, not treated as a change. */
	uint32_t last_fusion_group_leader;
	bool     last_fusion_group_seen;

	/* Voluntary-context-switch counter sampled from
	 * /proc/<tid>/status. The recalc worker reads the counter
	 * once per reconcile pass; the delta against
	 * `voluntary_ctxt_switches_last_sample` is the count of
	 * yields the follower thread issued during the recalc
	 * interval. A SCHED_DEADLINE thread issues one voluntary
	 * yield per activation (the deadline wait at cycle end), so
	 * growth above (activations during the interval + tolerance)
	 * is the signal that the thread suspended INSIDE process().
	 * The current cycle's accumulated growth lives in
	 * `voluntary_ctxt_switches_in_process` for snapshot exposure;
	 * a non-zero value across a recalc interval triggers
	 * reconcile_state_report_blocking_observation and the typed
	 * RECONCILE_SOFT_REASON_PROCESS_BLOCKED_INSIDE_RT demotion. */
	uint64_t voluntary_ctxt_switches_last_sample;
	uint64_t voluntary_ctxt_switches_last_sample_time_ns;
	bool     voluntary_ctxt_switches_seen;
	uint64_t voluntary_ctxt_switches_in_process;

	/* Driver topology generation last observed by this
	 * follower. Bumps whenever the snapshot fingerprint changes
	 * (added/removed nodes or edges, period change). MBPTA's
	 * sample-distribution can shift with topology even when the
	 * follower's own fusion-leader stays put -- a new parallel
	 * path can change cache pressure, a removed downstream sink
	 * can change back-pressure. Invalidate on the transition
	 * with MBPTA_INVALIDATED_TOPOLOGY_GENERATION so a stale fit
	 * doesn't carry into the new graph. _seen=false on first
	 * exposure -- the initial generation is recorded but not
	 * treated as a change. */
	uint64_t last_topo_generation;
	bool     last_topo_generation_seen;

	/* Per-driver async state. Only valid when is_driver=true. */
	/* SPSC sample ring: producer is this driver's data-loop thread
	 * (inside the complete/incomplete RT hook), consumer is the
	 * impl->worker thread. */
	struct spa_ringbuffer ring;          /* byte indices into ring_slots */
	struct sample        *ring_slots;
	uint32_t              ring_capacity; /* count of slots */
	uint64_t              ring_dropped;  /* monotonic; written by RT */

	/* Coalescing flag: RT CAS 0->1 before signaling worker; worker
	 * CAS->0 at start of its callback. Atomic. */
	uint32_t              recalc_inflight;

	/* Most recent topology snapshot of the targets scheduled by
	 * this driver. Filled by the main-loop callback; consumed by
	 * the worker. */
	struct topo_snap      topo;

	/* Highest topo.generation the worker has already reconciled
	 * against. Compared by the worker against the snapshot's
	 * current generation before deciding whether the topology
	 * reconcile pass is needed; the persistent-DAG path uses this
	 * to short-circuit no-op cycles. */
	uint64_t              topo_gen_applied;

	/* FNV-1a-ish fingerprint of the latest topology snapshot
	 * (period + sorted follower ids/tids + sorted edge src/dst).
	 * Compared at the end of each snapshot pass against the
	 * previously recorded value; topo.generation only bumps when
	 * the fingerprint changes, so the worker's "freshness" check
	 * fires only on real topology changes -- not on every snapshot
	 * tick. */
	uint64_t              topo_fingerprint;

	/* Per-driver reconcile state. Owned by this struct node; freed
	 * at driver_removed / module_destroy via reconcile_fini. */
	reconcile_state_t    *reconcile;

	/* Per-driver RT-hook timing histogram. Log2-bucket of CPU-time
	 * nanoseconds spent in the RT hook body. Worker reads at
	 * destroy; RT writer otherwise has exclusive access. */
	uint64_t              hist_buckets[32];
	uint64_t              hist_count;
	uint64_t              hist_sum_ns;
	uint64_t              hist_min_ns;
	uint64_t              hist_max_ns;

	/* Per-driver worker per-wake timing histogram. Same log2 shape
	 * as the RT-hook histogram so a live A/B summary can interleave
	 * the two tables. Sampled around the body of worker_recalc_one;
	 * the producer is the deadline-recalc worker thread, which is
	 * also the only reader except at driver_removed / module_destroy.
	 * Untouched in sync mode (where the worker is not used). */
	uint64_t              wk_hist_buckets[32];
	uint64_t              wk_hist_count;
	uint64_t              wk_hist_sum_ns;
	uint64_t              wk_hist_min_ns;
	uint64_t              wk_hist_max_ns;
};

struct impl {
	struct pw_context *context;
	struct pw_properties *props;

	struct pw_impl_module *module;

	struct spa_hook module_listener;
	struct spa_hook context_listener;

	int n_cpus;
	int cpus[MAX_CPUS];
	float cpu_utilization;
	/* Per-CPU relative_capacity vector aligned with cpus[],
	 * derived from cpu_topology at module init. The "target"
	 * vector reflects the dvfs policy (min_freq under
	 * conservative, max_freq under assume-max) and is what
	 * reconcile_init forwards to dag_create -- it is both the
	 * admission ceiling the placer compares per-CPU load against
	 * and the divisor sched_cb applies before sched_setattr.
	 * NULL is the homogeneous (all-1.0) identity. The "nominal"
	 * vector is always max-freq based; the sketch-insert path
	 * uses it to normalise samples as if they had been collected
	 * at max_freq, which is the smallest wall-clock time the
	 * same work could possibly take and therefore the
	 * conservative upper bound on the sample's true cycle
	 * count. The two vectors agree under the assume-max policy
	 * and diverge under conservative, where target < nominal. */
	double *relative_capacity;
	double *relative_capacity_nominal;
	/* The probed (or JSON-overridden) topology. Kept alive for the
	 * lifetime of the module so the per-CPU diagnostic fields are
	 * available for logging at any point. */
	struct cpu_topology topology;
	enum cpu_smt_policy  smt_policy;
	enum cpu_dvfs_policy dvfs_policy;
	/* When true (the default), set SCHED_FLAG_RECLAIM on every
	 * sched_setattr so unused bandwidth flows to peers via GRUB.
	 * Disabled by sched.reclaim=false for the saturation live
	 * test: a strict-budget mode where deadline misses surface
	 * instead of being absorbed by reclaim. Fixed for the
	 * lifetime of the module. */
	bool sched_reclaim;

	/* WCET estimator configuration; see module-options doc above. */


	/*
	 * Runtime budget-source preference. The runtime selection
	 * predicate walks manual-override -> deterministic-WCET ->
	 * adaptive-conformal -> bootstrap-fallback in declining order
	 * of provenance strength; this knob picks the automatic source
	 * the operator wants the predicate to consider. Bernat, Burns
	 * & Llamosi 2001 §III: strict hard-realtime operation requires
	 * manual / static / hybrid WCETs -- the adaptive-conformal
	 * estimator is soft / weakly-hard only.
	 */
	enum rt_budget_source {
		BUDGET_SOURCE_AUTO              = 0,
		BUDGET_SOURCE_MANUAL            = 1,
		BUDGET_SOURCE_DETERMINISTIC     = 2,
		BUDGET_SOURCE_ADAPTIVE_CONFORMAL = 3,
		BUDGET_SOURCE_BOOTSTRAP          = 6,
	}        budget_source;

	/*
	 * Adaptive-conformal estimator configuration. Defaults are the
	 * calibration starting points from rt_conformal_config_defaults;
	 * the per-knob deadline.conformal.* options override them. The
	 * estimator's own state lives per-follower; this block carries
	 * the module-wide configuration that initialises every new
	 * estimator instance.
	 */
	struct rt_conformal_config conformal_cfg;

	/* Persistent-DAG path on the worker (default). When false the
	 * worker still runs but reconcile_apply takes the legacy
	 * destroy-and-rebuild path -- a permanent kill switch for the
	 * persistent-DAG code. */
	bool                  recalc_persistent;

	/* WCET drift threshold: a follower's reported WCET must change
	 * by more than this fraction of the current value to trigger
	 * dag_set_node_wcet (and therefore a recalc). Default 0.01;
	 * 0.0 = always recalc. Must stay strictly below 1.0. */
	double                wcet_recalc_threshold;

	/* Async worker. Created at init when deadline policy is available;
	 * NULL when sync_mode is true. */
	bool                  sync_mode;
	int                   worker_rt_prio;
	uint32_t              worker_fake_delay_us;
	struct pw_thread_loop *worker_tloop;
	struct pw_loop       *worker_loop;
	struct pw_loop       *main_loop;
	/* Worker wake-up is an eventfd source signaled by the RT hook
	 * with a single eventfd_write (sub-microsecond cross-CPU). The
	 * source must be added before pw_thread_loop_start (see init):
	 * sources added from inside a worker invoke proved unreliable
	 * on this host -- the worker's epoll set did not observe the
	 * newly-registered fd. */
	struct spa_source    *worker_wake;
	cpu_set_t             worker_affinity; /* online CPUs - cpus.available */

	struct spa_list node_list;

	/* O(log N) id -> struct node * index. Sorted by node_id
	 * ascending; maintained by node_register / node_unregister so
	 * every alloc/free site stays in sync. Geometric realloc
	 * (start 16, double on overflow). */
	struct node **nodes_by_id;
	uint32_t      nodes_by_id_count;
	uint32_t      nodes_by_id_cap;

	/* sched_setattr skip cache telemetry. Bumped by sched_cb on
	 * every callback; dumped at module_destroy alongside the
	 * histograms. */
	uint64_t      sched_calls_total;
	uint64_t      sched_calls_skipped;

	/* Per-TID accumulator used by the chain-merge path. Each
	 * dag_foreach_node pass calls sched_cb once per node; when
	 * libpipewire has consolidated nodes onto a shared thread,
	 * multiple followers report the same tid. sched_cb sums
	 * (runtime, deadline) into the entry for that tid; once the
	 * foreach returns, apply_sched_groups walks the table and
	 * issues exactly one sched_setattr per distinct tid. Reset at
	 * the start of each apply pass via sched_groups_reset. */
	struct sched_groups sched_groups;

	/* Observability switch: when true, the snapshot path emits a
	 * driver-relative raw-graph dump on every topology-fingerprint
	 * change. Default false so the production hot path is
	 * unaffected; flipped on via the `debug.dump-raw-graph` module
	 * argument when an operator (or a test runner) needs to see
	 * every follower the daemon reports under the driver,
	 * regardless of whether it would survive the scheduling-DAG
	 * inclusion filters. */
	bool                  debug_dump_raw_graph;

	/* Companion switch for the scheduling-DAG slice. When true,
	 * the snapshot path emits an in-period view: the followers
	 * the daemon will hand to the analysis layer plus every link
	 * the inclusion filters dropped, each tagged with a single
	 * exclusion reason (feedback, async, exported, ...). Gated
	 * identically to debug_dump_raw_graph; the two views can be
	 * enabled independently because operators occasionally want
	 * the curated view without the raw-graph noise. */
	bool                  debug_dump_sched_graph;

	/* Companion switch for the fusion-decision slice. When true,
	 * the snapshot path emits one group entry per applied fusion
	 * verdict (fuse / linear_only / split) read off each
	 * follower's pw_impl_node fusion_prev_decision plus the
	 * PW_KEY_NODE_LOOP_GROUP property. */
	bool                  debug_dump_fusion;

	/* Machine-readable combined snapshot. When set to a writable
	 * filesystem path, the snapshot path emits a single JSON
	 * document covering raw graph / scheduling DAG / fusion /
	 * parameters / mode / feasibility, per topology-fingerprint
	 * change. The write is atomic (temp file + rename) so a
	 * concurrent reader either sees the previous snapshot or the
	 * fresh one but never a torn document. NULL or empty string
	 * disables emission. */
	char                 *debug_snapshot_json_path;

	/*
	 * Adaptive-conformal calibration trace export. When
	 * impl->conformal_cfg.trace_export is true and trace_path is
	 * non-NULL/non-empty, the worker appends one JSON line per
	 * accepted sample to the file. The line carries the fields a
	 * prequential replay tool needs to reconstruct the
	 * estimator's progression off-line: timestamp, follower id,
	 * mode-key fingerprint, period, observed runtime in
	 * reference-CPU units, the conformal-published budget that
	 * was active for this activation, and the selected
	 * budget_kind token. Open lazily on the first export; closed
	 * at module_destroy. The RT path never touches this file --
	 * writes happen on the audio recalc worker, same lifecycle as
	 * the JSON snapshot emission.
	 */
	char                 *conformal_trace_path;
	FILE                 *conformal_trace_fp;
};

static void hist_dump(const char *who, struct node *drv);
static void node_unregister(struct impl *impl, struct node *n);
static struct node *find_node_by_id(struct impl *impl, uint32_t id);
static struct node *find_node_any_by_id(struct impl *impl, uint32_t id);

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct node *n, *tmp;

	/* Stop emitting RT signals first: drop the context listener so no
	 * more drivers are added, then disable existing drivers so no
	 * new samples are pushed into the rings. */
	spa_hook_remove(&impl->context_listener);
	spa_list_for_each(n, &impl->node_list, link) {
		if (n->is_driver && n->enabled) {
			SPA_FLAG_CLEAR(n->node->rt.target.activation->flags,
				       PW_NODE_ACTIVATION_FLAG_PROFILER);
			pw_impl_node_remove_rt_listener(n->node, &n->node_rt_listener);
			n->enabled = false;
		}
	}

	/* Stop the worker before tearing down per-driver buffers it
	 * might still touch. Sources are owned by the worker loop and
	 * are destroyed when the loop is destroyed -- but we destroy
	 * them explicitly here so the order is unambiguous: no more
	 * wakeups can be processed after pw_thread_loop_stop returns. */
	if (impl->worker_tloop) {
		pw_thread_loop_stop(impl->worker_tloop);
		if (impl->worker_wake && impl->worker_loop)
			pw_loop_destroy_source(impl->worker_loop, impl->worker_wake);
		impl->worker_wake = NULL;
		pw_thread_loop_destroy(impl->worker_tloop);
		impl->worker_tloop = NULL;
		impl->worker_loop = NULL;
	}

	/* Drain the main-loop invoke queue before freeing per-node state.
	 *
	 * The worker may have queued one or more snapshot_topology_main
	 * invokes via pw_loop_invoke(block=false) between its last
	 * recalc and the pw_thread_loop_stop call above. The invokes
	 * are sitting on the main loop's event queue; they capture the
	 * struct node * by value (snapshot_arg.drv) and dereference it
	 * when they fire (drv->node, drv->topo, ...). If we free the
	 * struct node here and the queued invokes fire later (in the
	 * next main-loop iteration, after module_destroy returns), the
	 * stale pointer turns into a use-after-free and the daemon
	 * SEGVs in snapshot_topology_main at line 2937
	 * (dnode->target_rate.denom) -- the bug observed during shutdown
	 * of the live-verification and mbpta-calibration runs.
	 *
	 * pw_loop_invoke called from the main loop's own thread (which
	 * module_destroy always is) calls flush_all_queues() inline
	 * before running the supplied function, processing every
	 * already-queued invoke first. A no-op invoke with block=true
	 * is therefore a synchronous barrier: when it returns, no
	 * snapshot_topology_main invoke is pending and freeing the
	 * struct node entries is safe.
	 *
	 * The worker is already stopped, so no new invokes can land
	 * between this barrier and the freeing loop below. */
	if (impl->main_loop != NULL) {
		pw_loop_invoke(impl->main_loop, NULL, 0, NULL, 0, true, NULL);
	}

	/* Dump per-driver timing histograms for the live A/B test, then
	 * release driver/follower bookkeeping. */
	spa_list_for_each_safe(n, tmp, &impl->node_list, link) {
		hist_dump("destroy", n);
		if (n->reconcile) {
			reconcile_fini(n->reconcile);
			n->reconcile = NULL;
		}
		if (n->conformal != NULL) {
			rt_conformal_destroy(n->conformal);
			n->conformal = NULL;
		}
		free(n->ring_slots);
		free(n->topo.nodes);
		free(n->topo.edges);
		node_unregister(impl, n);
		spa_list_remove(&n->link);
		free(n);
	}

	spa_hook_remove(&impl->module_listener);

	free(impl->nodes_by_id);
	free(impl->relative_capacity);
	free(impl->relative_capacity_nominal);
	free(impl->debug_snapshot_json_path);
	if (impl->conformal_trace_fp != NULL) {
		fclose(impl->conformal_trace_fp);
		impl->conformal_trace_fp = NULL;
	}
	free(impl->conformal_trace_path);
	cpu_topology_destroy(&impl->topology);
	sched_groups_fini(&impl->sched_groups);
	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static bool can_use_deadline_policy(void)
{
	struct __user_cap_header_struct hdr;
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
	unsigned int index, mask;

	spa_zero(hdr);
	spa_zero(data);
	hdr.version = _LINUX_CAPABILITY_VERSION_3;
	hdr.pid = 0;

	if (syscall(SYS_capget, &hdr, data) != 0) {
		switch (errno) {
		case EPERM:
		case EACCES:
			pw_log_info("deadline scheduling unavailable: capget() permission denied");
			break;
		case ENOSYS:
		case EOPNOTSUPP:
			pw_log_info("deadline scheduling unavailable: capget() not supported");
			break;
		default:
			pw_log_info("deadline scheduling unavailable: capget() failed: %m");
			break;
		}
		return false;
	}

	index = CAP_SYS_NICE / 32;
	mask = 1U << (CAP_SYS_NICE % 32);

	if ((data[index].effective & mask) != 0) {
		pw_log_debug("CAP_SYS_NICE available for current thread");
		return true;
	}

	pw_log_info("deadline scheduling unavailable: CAP_SYS_NICE not in effective set");
	return false;
}

static int set_deadline_sched(pid_t tid, uint64_t runtime, uint64_t deadline,
		uint64_t period, bool use_reclaim)
{
	int ret = 0;

	if (runtime == 0 || deadline > period) {
		pw_log_warn("invalid DEADLINE attributes for tid %d: r:%lu d:%lu p:%lu", tid, runtime, deadline, period);
		errno = EINVAL;
		return -1;
	}

	struct sched_attr attr = {0};

	attr.sched_policy = SCHED_DEADLINE;
	attr.sched_runtime = runtime;
	attr.sched_deadline = deadline;
	attr.sched_period = period;

	if (use_reclaim) {
		/* GRUB (Greedy Reclamation of Unused Bandwidth) reclaim:
		 * tasks that finish before their budget hand the slack
		 * back to peers transparently, so the occasional WCET
		 * overshoot the t-digest sketch admits is absorbed
		 * silently. Disabled by sched.reclaim=false for the
		 * bandwidth-saturation live test, where deadline misses
		 * caused by a too-small budget must surface instead of
		 * being absorbed. */
		attr.sched_flags |= SCHED_FLAG_RECLAIM;
	}

	ret = sched_setattr(tid, &attr, 0);

	if (ret) {
		/* sched_setattr failure means the kernel rejected the
		 * tuple we computed -- and per Linux's
		 * sched-deadline.rst, the kernel's admission test is
		 * the final authority on whether the schedule is
		 * realisable. Any hard-real-time claim derived from
		 * the in-process feasibility predicates is therefore
		 * suspect for this period until the next reconcile
		 * either re-validates the tuple or drops the
		 * follower's last_applied cache. The caller in
		 * apply_sched_groups clears last_applied on rc != 0;
		 * here we log the errno + the offending tuple so the
		 * downstream investigation has the full data. */
		if (errno == EINVAL)
			pw_log_warn("sched_setattr rejected DEADLINE tuple"
				" for tid %d (errno=EINVAL, r=%lu d=%lu p=%lu);"
				" hard guarantees from in-process predicates"
				" no longer apply for this period",
				tid, runtime, deadline, period);
		else
			pw_log_error("sched_setattr failed for tid %d"
				" (errno=%d %s, r=%lu d=%lu p=%lu);"
				" hard guarantees dropped for this period",
				tid, errno, strerror(errno),
				runtime, deadline, period);
	}
	else
		pw_log_debug("set DEADLINE scheduling for tid %d: r:%lu d:%lu p:%lu", tid, runtime, deadline, period);

	return ret;
}

static int set_cpu_affinity(pid_t tid, int cpu)
{
	int ret = 0;

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);

	ret = sched_setaffinity(tid, sizeof(cpuset), &cpuset);
	if (ret < 0) {
		if (errno == EINVAL)
			pw_log_warn("invalid affinity for tid %d: cpu %d", tid, cpu);
		else
			pw_log_error("failed to set affinity for tid %d: %s", tid, strerror(errno));
	}
	return ret;
}

/* DAG callback (collection pass).
 *
 * dag_foreach_node fires this once per real node with the per-node
 * (runtime, deadline, period, cpu) computed by the analysis. We
 * accumulate those values into per-TID slots via sched_groups_add:
 * when libpipewire has consolidated several adjacent followers onto
 * a single thread, multiple callbacks land on the same TID slot and
 * the runtime / deadline pair is the summed envelope that the
 * merged thread is supposed to fit into.
 *
 * The actual sched_setattr / sched_setaffinity happens in
 * apply_sched_groups, called by the worker once this collection pass
 * finishes. Skipping the sched syscalls inside the callback also
 * keeps dag_foreach_node free of syscall jitter, which matters in
 * sync mode where the foreach runs on the RT data-loop thread. */
static void sched_cb(void *data, uint32_t id, pid_t tid, uint64_t runtime,
		uint64_t cumulative_deadline, uint64_t local_deadline,
		uint64_t period, uint32_t cpu,
		uint32_t fusion_group_leader_id)
{
	struct impl *impl = data;

	/* Denormalise runtime from reference-CPU units into kernel
	 * units for the placement CPU. The sketch holds WCETs as if the
	 * follower ran on the fastest CPU; on a slower placement the
	 * thread needs proportionally more wall-clock time, so the
	 * budget shipped to sched_setattr divides by
	 * relative_capacity[cpu]. Identity when relative_capacity is
	 * unset (NULL vector) or the placement CPU is the reference. */
	uint64_t runtime_kernel = runtime;
	if (impl->relative_capacity != NULL &&
			cpu < (uint32_t)impl->n_cpus &&
			impl->relative_capacity[cpu] > 0.0) {
		double scaled = (double)runtime / impl->relative_capacity[cpu];
		if (scaled < 0.0)
			scaled = 0.0;
		if (scaled > (double)UINT64_MAX)
			scaled = (double)UINT64_MAX;
		runtime_kernel = (uint64_t)scaled;
	}

	int res = sched_groups_add(&impl->sched_groups, id, tid,
			runtime_kernel, cumulative_deadline, local_deadline,
			period, cpu);
	if (res == -ENOMEM)
		pw_log_warn("sched: out of memory accumulating tid=%d", (int)tid);
	/* -EINVAL (tid <= 0) is silently ignored: a follower with no
	 * published thread can't be scheduled, same as before the
	 * extraction. */

	/* Stamp the per-follower graph-relative milestone on the
	 * caller's struct node so populate_params_snapshot can surface
	 * cumulative and local deadlines separately. apply_sched_groups
	 * later overwrites the kernel-side fields (runtime, local
	 * deadline, period, cpu) on the group's leader follower; the
	 * cumulative deadline is per-follower and lands here. */
	struct node *mn = find_node_by_id(impl, id);
	if (mn != NULL) {
		mn->last_cumulative_deadline = cumulative_deadline;
		/* Fusion-group invalidation. Per Cucu-Grosjean 2012
		 * §IV "Path Coverage" a contracted node is a fresh
		 * MBPTA subject -- its execution-time distribution
		 * reflects samples taken *after* the contraction is in
		 * effect. When the macro leader changes between two
		 * reconcile passes the old fit no longer corresponds
		 * to the new workload, so drop it. The first pass
		 * (last_fusion_group_seen = false) records the leader
		 * without invalidating; subsequent passes compare. */
		if (mn->last_fusion_group_seen &&
		    mn->last_fusion_group_leader != fusion_group_leader_id) {
			if (mn->conformal != NULL)
				rt_conformal_invalidate(mn->conformal,
						RT_CONF_INVALIDATED_FUSION_GROUP);
		}
		mn->last_fusion_group_leader = fusion_group_leader_id;
		mn->last_fusion_group_seen = true;
	}
}

/* Per-group apply pass.
 *
 * Iterates the accumulator and issues at most one sched_setattr +
 * one sched_setaffinity per distinct TID. Reuses the leader follower
 * node's last_applied cache so a stable graph re-applies nothing:
 * the cache lives on the lowest-id member of each group and is
 * invalidated automatically when the group composition changes
 * (leader becomes a different follower, sums change, period or CPU
 * change).
 *
 * Singleton TIDs (n_members == 1) take exactly the same code path
 * as multi-member groups -- the original one-node-one-thread case
 * is just the degenerate single-member group. */
/*
 * Read voluntary_ctxt_switches for a given pid from /proc/<tid>/status.
 * Returns 0 on success with *out populated, -errno on failure. The
 * field name in the file is "voluntary_ctxt_switches:"; the value is
 * a decimal counter that increments every time the thread voluntarily
 * yields the CPU (sleeps, futex_wait, blocking I/O, deadline wait at
 * cycle end on SCHED_DEADLINE).
 */
static int read_voluntary_ctxt_switches(pid_t tid, uint64_t *out)
{
	char path[64];
	char line[256];
	FILE *fp;
	int r = -ENOENT;

	if (tid <= 0 || out == NULL)
		return -EINVAL;
	snprintf(path, sizeof(path), "/proc/%d/status", (int)tid);
	fp = fopen(path, "r");
	if (fp == NULL)
		return -errno;
	while (fgets(line, sizeof(line), fp) != NULL) {
		unsigned long long v;
		if (sscanf(line, "voluntary_ctxt_switches: %llu", &v) == 1) {
			*out = (uint64_t)v;
			r = 0;
			break;
		}
	}
	fclose(fp);
	return r;
}

/*
 * Per-recalc voluntary-context-switch sampling for blocking
 * detection. Called from the worker after reconcile_apply has
 * returned. For each follower with a known tid and period, samples
 * voluntary_ctxt_switches from /proc, computes the delta since the
 * previous sample, and compares against the expected count (one
 * deadline wait per activation across the recalc interval). The
 * EXCESS is the count of yields the thread issued INSIDE process();
 * a non-zero excess violates the static blocking-closure predicate's
 * accept and is reported through reconcile_state_report_blocking_observation
 * for the soft-degraded demotion.
 *
 * The cost is O(followers) file opens per recalc; the recalc rate is
 * far below the cycle rate so the open-per-recalc cost is amortised.
 * The check runs off the RT path.
 */
/*
 * Iterate the worker-owned topology snapshot (drv->topo.nodes[]) --
 * the same view worker_apply_dag uses to build the reconcile_topo_t.
 * The snapshot is rebuilt on the main loop in snapshot_topology_main
 * and consumed lock-free by the worker after spa_loop_invoke
 * completes; iterating dnode->follower_list directly from worker
 * context would race with main-loop topology mutations and is what
 * an earlier draft of this function did (the resulting SIGSEGV was
 * observed at daemon shutdown when the follower list was being torn
 * down concurrently).
 *
 * The tolerance over expected voluntary yields is generous: in
 * production a filter-chain follower can legitimately yield a
 * handful of times per second beyond the one-per-deadline-wait
 * baseline (lazy-init epoll path, occasional reservation handshake,
 * the first cycles after a topology flip). Reporting on a single
 * extra yield generates noise; require excess > a margin that is
 * proportional to the number of cycles in the interval before
 * demoting to soft-degraded.
 */
static void sample_voluntary_ctxt_switches_main(struct impl *impl,
		struct node *drv)
{
	struct topo_snap *t;
	struct timespec ts;
	uint64_t now_ns;
	uint32_t i;

	if (impl == NULL || drv == NULL || drv->reconcile == NULL)
		return;
	t = &drv->topo;
	if (!t->ok || t->n_nodes == 0)
		return;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return;
	now_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;

	for (i = 0; i < t->n_nodes; i++) {
		pid_t tid = t->nodes[i].tid;
		uint32_t id = t->nodes[i].id;
		struct node *mn;
		uint64_t cur, delta, expected, excess, interval_ns, period_ns;
		uint64_t margin;

		if (tid <= 0)
			continue;
		if (read_voluntary_ctxt_switches(tid, &cur) < 0)
			continue;
		mn = find_node_by_id(impl, id);
		if (mn == NULL)
			continue;
		if (!mn->voluntary_ctxt_switches_seen) {
			mn->voluntary_ctxt_switches_last_sample = cur;
			mn->voluntary_ctxt_switches_last_sample_time_ns = now_ns;
			mn->voluntary_ctxt_switches_seen = true;
			mn->voluntary_ctxt_switches_in_process = 0;
			continue;
		}
		delta = cur - mn->voluntary_ctxt_switches_last_sample;
		interval_ns = now_ns -
			mn->voluntary_ctxt_switches_last_sample_time_ns;
		period_ns = mn->last_period;
		if (period_ns == 0)
			period_ns = mn->period;
		if (period_ns == 0) {
			expected = 0;
			margin = 64;  /* arbitrary noise floor */
		} else {
			uint64_t cycles = interval_ns / period_ns + 1;
			/* One yield per activation (the deadline wait) plus
			 * a generous margin: a 10% slack on top of the cycle
			 * count, with a floor of 16 yields so very short
			 * intervals do not produce a 0-margin window that
			 * fires on every scheduler hiccup. */
			expected = cycles;
			margin = cycles / 10;
			if (margin < 16)
				margin = 16;
		}
		excess = delta > expected + margin ? delta - expected - margin : 0;
		mn->voluntary_ctxt_switches_in_process = excess;
		mn->voluntary_ctxt_switches_last_sample = cur;
		mn->voluntary_ctxt_switches_last_sample_time_ns = now_ns;
		if (excess > 0 && excess < UINT32_MAX) {
			(void)reconcile_state_report_blocking_observation(
					drv->reconcile, tid, (uint32_t)excess);
		}
	}
}

static void apply_sched_groups(struct impl *impl, struct node *drv)
{
	uint32_t i;
	bool any_failure = false;
	for (i = 0; i < impl->sched_groups.count; i++) {
		struct sched_group *g = &impl->sched_groups.entries[i];
		struct node *anchor;
		uint64_t kernel_deadline;
		int rc_sched, rc_aff;

		impl->sched_calls_total++;

		/* The kernel-API deadline is the leader's local_deadline.
		 * Every member of a fused thread now reports the same
		 * macro-node local_deadline (the contracted-DAG analysis
		 * computes one deadline per macro-node and the per-follower
		 * sched_cb invocations all carry that value), so the
		 * accumulator's leader_local_deadline field carries the
		 * correct kernel-relative quantity verbatim. The period
		 * clamp is defensive: the contracted analysis already
		 * enforces local_deadline <= period before reaching this
		 * point, and the splitter never produces a value above the
		 * period, but leaving the check here preserves the kernel
		 * SCHED_DEADLINE contract locally. max_cumulative_deadline
		 * is retained on the accumulator for diagnostic use only. */
		kernel_deadline = g->leader_local_deadline;
		if (kernel_deadline > g->period)
			kernel_deadline = g->period;

		/* Pre-syscall validation. SCHED_DEADLINE requires
		 *   0 < runtime <= deadline <= period.
		 * If the analysis published a tuple that violates the
		 * contract (typically runtime > local_deadline because a
		 * follower's measured WCET exceeds its split slice),
		 * skipping the syscall is necessary but not sufficient:
		 * the active schedule no longer meets the contracted
		 * bound, so the driver's mode must transition to
		 * soft-degraded so the snapshot reflects reality and
		 * the soft redistributor takes over on subsequent
		 * passes. Without the transition, an operator inspecting
		 * the snapshot would still see mode=hard while the
		 * graph runs without the kernel admission test it
		 * promises. */
		if (g->sum_runtime == 0 || kernel_deadline == 0 ||
		    kernel_deadline > g->period) {
			pw_log_warn("sched: invalid params for tid=%d "
					"runtime=%" PRIu64 " deadline=%" PRIu64
					" period=%" PRIu64
					" (n_members=%u, leader=%u); "
					"skipping sched_setattr",
					(int)g->tid, g->sum_runtime,
					kernel_deadline, g->period,
					g->n_members, g->leader_id);
			anchor = find_node_by_id(impl, g->leader_id);
			if (anchor != NULL)
				anchor->last_applied = false;
			any_failure = true;
			continue;
		}

		/* Soft-degraded throttling cap. When the analysis
		 * publishes a tuple with runtime > deadline (typically a
		 * follower whose measured WCET overshot its splitter
		 * slice), the kernel would reject the syscall. Rather
		 * than silently skipping -- which leaves the previous
		 * params in place and provides no soft redistribution
		 * floor -- ship the smallest valid value: a runtime of
		 * 95 % of the local deadline. The follower then runs on
		 * CBS but at a budget below its observed demand, so it
		 * may be throttled and miss deadlines. This is exactly
		 * the operator-visible "throttling risk is expected"
		 * behaviour the soft objective calls for. The
		 * any_failure flag below routes the mode through
		 * reconcile_state_force_soft so the snapshot reflects
		 * that the hard claim is no longer valid. */
		if (g->sum_runtime > kernel_deadline) {
			uint64_t capped = (uint64_t)((double)kernel_deadline
					* 0.95);
			if (capped == 0)
				capped = 1;
			pw_log_warn("sched: throttling tid=%d "
				"runtime %" PRIu64 " -> %" PRIu64
				" to fit deadline %" PRIu64 " (soft cap)",
				(int)g->tid, g->sum_runtime, capped,
				kernel_deadline);
			g->sum_runtime = capped;
			any_failure = true;
		}

		anchor = find_node_by_id(impl, g->leader_id);
		if (anchor != NULL && anchor->last_applied &&
				anchor->last_runtime == g->sum_runtime &&
				anchor->last_deadline == kernel_deadline &&
				anchor->last_period == g->period &&
				anchor->last_cpu == g->cpu) {
			impl->sched_calls_skipped++;
			continue;
		}

		rc_sched = set_deadline_sched(g->tid, g->sum_runtime,
				kernel_deadline, g->period,
				impl->sched_reclaim);
		rc_aff = set_cpu_affinity(g->tid, impl->cpus[g->cpu]);

		if (anchor == NULL)
			continue;

		if (rc_sched == 0 && rc_aff == 0) {
			anchor->last_runtime  = g->sum_runtime;
			anchor->last_deadline = kernel_deadline;
			anchor->last_period   = g->period;
			anchor->last_cpu      = g->cpu;
			anchor->last_applied  = true;
		} else {
			anchor->last_applied = false;
			any_failure = true;
		}
	}
	/* Any kernel-side rejection of the in-process feasibility
	 * verdict proves the schedule does not meet every deadline
	 * on every activation -- the kernel's admission test is the
	 * final authority. Force the driver's published mode to
	 * SOFT_DEGRADED so the JSON snapshot reflects reality;
	 * the next reconcile pass re-evaluates and may promote
	 * back to HARD via the standard hysteresis path. */
	if (any_failure && drv != NULL && drv->reconcile != NULL)
		reconcile_state_force_soft(drv->reconcile,
				"kernel_rejected_or_invalid_params");
}

/* Bsearch over impl->nodes_by_id for `id`; returns the array slot
 * where `id` lives or would be inserted to keep order. */
#define MODULE_NODES_BY_ID_INITIAL_CAP 16u

static uint32_t nodes_by_id_bsearch(struct impl *impl, uint32_t id)
{
	uint32_t lo = 0, hi = impl->nodes_by_id_count;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (impl->nodes_by_id[mid]->node_id < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static int node_register(struct impl *impl, struct node *n)
{
	uint32_t pos;

	if (impl->nodes_by_id_count == impl->nodes_by_id_cap) {
		uint32_t new_cap = impl->nodes_by_id_cap ?
				impl->nodes_by_id_cap * 2 :
				MODULE_NODES_BY_ID_INITIAL_CAP;
		struct node **resized = realloc(impl->nodes_by_id,
				(size_t)new_cap * sizeof(*resized));

		if (!resized)
			return -ENOMEM;
		impl->nodes_by_id = resized;
		impl->nodes_by_id_cap = new_cap;
	}

	pos = nodes_by_id_bsearch(impl, n->node_id);
	if (pos < impl->nodes_by_id_count) {
		memmove(&impl->nodes_by_id[pos + 1], &impl->nodes_by_id[pos],
				(impl->nodes_by_id_count - pos) *
				sizeof(*impl->nodes_by_id));
	}
	impl->nodes_by_id[pos] = n;
	impl->nodes_by_id_count++;
	return 0;
}

static void node_unregister(struct impl *impl, struct node *n)
{
	uint32_t pos;

	if (impl->nodes_by_id_count == 0)
		return;
	pos = nodes_by_id_bsearch(impl, n->node_id);
	if (pos >= impl->nodes_by_id_count || impl->nodes_by_id[pos] != n)
		return;
	if (pos < impl->nodes_by_id_count - 1) {
		memmove(&impl->nodes_by_id[pos], &impl->nodes_by_id[pos + 1],
				(impl->nodes_by_id_count - pos - 1) *
				sizeof(*impl->nodes_by_id));
	}
	impl->nodes_by_id_count--;
}

static struct node *find_node(struct impl *impl, struct pw_impl_node *node)
{
	struct node *n;
	spa_list_for_each(n, &impl->node_list, link) {
		if (n->node == node)
			return n;
	}
	return NULL;
}

/* O(log N) follower lookup. The shared index holds both drivers
 * and followers (the PipeWire id space is unique per context, so
 * two distinct struct nodes never share node_id); we filter out
 * drivers here so behaviour matches the previous linear-scan
 * version exactly. */
static struct node *find_node_by_id(struct impl *impl, uint32_t id)
{
	uint32_t pos;
	struct node *n;

	if (impl->nodes_by_id_count == 0)
		return NULL;
	pos = nodes_by_id_bsearch(impl, id);
	if (pos >= impl->nodes_by_id_count)
		return NULL;
	n = impl->nodes_by_id[pos];
	if (n->node_id != id || n->is_driver)
		return NULL;
	return n;
}

/*
 * Driver-aware lookup: same index walk as find_node_by_id but
 * accepts both follower and driver entries. Used by code paths
 * that may hold a driver id (snapshot_topology_main captures the
 * driver's id by value at queue time; the post-drain lookup needs
 * to resolve it whether or not it is a driver). Returns NULL on a
 * missing id or empty index.
 */
static struct node *find_node_any_by_id(struct impl *impl, uint32_t id)
{
	uint32_t pos;
	struct node *n;

	if (impl == NULL || impl->nodes_by_id_count == 0)
		return NULL;
	pos = nodes_by_id_bsearch(impl, id);
	if (pos >= impl->nodes_by_id_count)
		return NULL;
	n = impl->nodes_by_id[pos];
	if (n->node_id != id)
		return NULL;
	return n;
}

/* Log2 bucket of x. Used to bin RT-hook timings cheaply (no log call,
 * no division). bucket(0) returns 0; bucket(1) returns 0;
 * bucket(2..3) returns 1; bucket(4..7) returns 2; ...; bucket(>=2^31)
 * is clamped to 31. */
static inline uint32_t log2_bucket(uint64_t x)
{
	if (x <= 1)
		return 0;
	uint32_t b = 63 - __builtin_clzll(x);
	return b > 31 ? 31 : b;
}

static inline uint64_t now_thread_cputime_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static void hist_record(struct node *drv, uint64_t dur_ns)
{
	if (dur_ns == 0)
		return;
	drv->hist_buckets[log2_bucket(dur_ns)]++;
	drv->hist_count++;
	drv->hist_sum_ns += dur_ns;
	if (drv->hist_min_ns == 0 || dur_ns < drv->hist_min_ns)
		drv->hist_min_ns = dur_ns;
	if (dur_ns > drv->hist_max_ns)
		drv->hist_max_ns = dur_ns;
}

static void worker_hist_record(struct node *drv, uint64_t dur_ns)
{
	if (dur_ns == 0)
		return;
	drv->wk_hist_buckets[log2_bucket(dur_ns)]++;
	drv->wk_hist_count++;
	drv->wk_hist_sum_ns += dur_ns;
	if (drv->wk_hist_min_ns == 0 || dur_ns < drv->wk_hist_min_ns)
		drv->wk_hist_min_ns = dur_ns;
	if (dur_ns > drv->wk_hist_max_ns)
		drv->wk_hist_max_ns = dur_ns;
}

static void hist_dump(const char *who, struct node *drv)
{
	if (drv->hist_count == 0 && drv->wk_hist_count == 0)
		return;
	if (drv->hist_count > 0) {
		pw_log_info("deadline RT-hook timing [%s drv-node=%d mode=%s]:",
			    who,
			    drv->node ? drv->node->info.id : (uint32_t)-1,
			    drv->impl->sync_mode ? "sync" : "async");
		pw_log_info("  cycles=%lu min=%luns max=%luns avg=%luns",
			    drv->hist_count, drv->hist_min_ns, drv->hist_max_ns,
			    drv->hist_sum_ns / drv->hist_count);
		pw_log_info("  bucket counts (lo bound ns -> count):");
		for (uint32_t i = 0; i < 32; i++) {
			if (drv->hist_buckets[i] == 0)
				continue;
			pw_log_info("    >=%luns -> %lu", (uint64_t)1 << i,
				    drv->hist_buckets[i]);
		}
		drv->ring_dropped = SPA_ATOMIC_LOAD(drv->ring_dropped);
		if (drv->ring_dropped)
			pw_log_info("  ring-dropped samples (cumulative): %lu",
				    drv->ring_dropped);
	}
	if (drv->wk_hist_count > 0) {
		pw_log_info("deadline worker per-wake timing [%s drv-node=%d mode=%s]:",
			    who,
			    drv->node ? drv->node->info.id : (uint32_t)-1,
			    drv->impl->sync_mode ? "sync" : "async");
		pw_log_info("  wakes=%lu min=%luns max=%luns avg=%luns",
			    drv->wk_hist_count, drv->wk_hist_min_ns, drv->wk_hist_max_ns,
			    drv->wk_hist_sum_ns / drv->wk_hist_count);
		pw_log_info("  bucket counts (lo bound ns -> count):");
		for (uint32_t i = 0; i < 32; i++) {
			if (drv->wk_hist_buckets[i] == 0)
				continue;
			pw_log_info("    >=%luns -> %lu", (uint64_t)1 << i,
				    drv->wk_hist_buckets[i]);
		}
	}
	/* sched_setattr cache telemetry. Reported on every dump (the
	 * counter is shared across drivers, so re-printing it on each
	 * driver's dump is fine -- the value monotonically increases). */
	if (drv->impl->sched_calls_total > 0) {
		uint64_t total = drv->impl->sched_calls_total;
		uint64_t skipped = drv->impl->sched_calls_skipped;
		pw_log_info("deadline sched-cb cache [%s drv-node=%d]:",
			    who,
			    drv->node ? drv->node->info.id : (uint32_t)-1);
		pw_log_info("  total=%lu issued=%lu skipped=%lu (skip rate=%.1f%%)",
			    total, total - skipped, skipped,
			    100.0 * (double)skipped / (double)total);
	}
}

static SPA_UNUSED bool is_audio_source_media_class(const char *media_class)
{
	return media_class != NULL &&
		(spa_strstartswith(media_class, "Audio/Source") ||
		 spa_strstartswith(media_class, "Stream/Output/Audio"));
}

static SPA_UNUSED bool is_audio_sink_media_class(const char *media_class)
{
	return media_class != NULL &&
		(spa_strstartswith(media_class, "Audio/Sink") ||
		 spa_strstartswith(media_class, "Stream/Input/Audio"));
}

static inline uint64_t get_runtime_ns(struct pw_impl_node *node, struct pw_node_activation *na)
{
	uint64_t runtime = SPA_ATOMIC_LOAD(na->prev_run_time);
	
	if (runtime == 0 || runtime > UINT64_MAX / 2) {
		/* Normal for a freshly activated follower: prev_run_time
		 * is published on the cycle the node first ran. The
		 * reconciler tolerates a 0 sample by skipping the node
		 * for one round. */
		pw_log_debug("invalid runtime %lu for node %d, using 0 instead",
				runtime, node->info.id);
		return 0;
	}

	return runtime;
}

/* Convert a CPU-cycle delta collected on `sample_cpu` into a
 * frequency-invariant reference-CPU runtime in nanoseconds. The work
 * done by `cycles` cycles on CPU `sample_cpu` is
 *
 *     work = cycles * raw_capacity[sample_cpu]
 *
 * (raw_capacity captures the CPU's relative IPC per cycle; the same
 * count of cycles on an E-core does less work than on a P-core). The
 * reference CPU at its peak nominal throughput would execute that
 * work in
 *
 *     ref_ns = work * 1e9 / (raw_capacity[ref] * max_freq_hz[ref])
 *
 * On a uniform host that collapses to cycles / max_freq[ref]. Returns
 * a positive double on success, 0.0 when cycles is 0 or any required
 * topology field is missing (the caller then falls back on the
 * wall-clock path). */
static inline double wcet_cycles_to_reference_ns(struct impl *impl,
		uint64_t cycles, uint32_t sample_cpu)
{
	if (cycles == 0 || impl == NULL || impl->topology.num_cpus == 0)
		return 0.0;
	if (sample_cpu == SAMPLE_CPU_UNKNOWN ||
			sample_cpu >= impl->topology.num_cpus)
		return 0.0;

	uint32_t ref_idx = impl->topology.reference_cpu_index;
	if (ref_idx >= impl->topology.num_cpus)
		return 0.0;

	const struct cpu_info *src = &impl->topology.cpus[sample_cpu];
	const struct cpu_info *ref = &impl->topology.cpus[ref_idx];
	if (ref->raw_capacity == 0 || ref->max_freq_khz == 0)
		return 0.0;

	double work = (double)cycles * (double)src->raw_capacity;
	double ref_throughput =
		(double)ref->raw_capacity * (double)ref->max_freq_khz * 1000.0;
	if (!(ref_throughput > 0.0))
		return 0.0;
	return work * 1.0e9 / ref_throughput;
}

/* Wall-clock fallback when cycles are unavailable. We do not know the
 * cpufreq state at the moment of measurement (a per-sample sysfs read
 * is not RT-safe), so the conservative assumption is that the sample
 * was collected at the CPU's max_freq -- i.e. the smallest wall-clock
 * time the same workload could possibly take. The denormalisation in
 * sched_cb then divides by relative_capacity[placement_cpu], which is
 * the *target* capacity (min_freq under conservative policy). The
 * resulting kernel budget is inflated by approximately
 * max_freq / freq_for_policy: on a host where the governor parks
 * CPUs anywhere down to min_freq, the budget remains feasible even
 * without per-cycle frequency information.
 *
 * Identity (no scaling) on homogeneous hardware *only* when
 * max_freq == freq_for_policy, e.g. under cpus.dvfs-policy =
 * assume-max or on a host where cpuinfo_max_freq == cpuinfo_min_freq.
 *
 * Falls back to the raw runtime when relative_capacity_nominal is
 * absent or sample_cpu is out of range / not yet known. */
static inline double wcet_sample_to_reference(struct impl *impl,
		uint64_t runtime, uint32_t sample_cpu)
{
	if (impl->relative_capacity_nominal == NULL)
		return (double)runtime;
	if (sample_cpu == SAMPLE_CPU_UNKNOWN ||
			sample_cpu >= (uint32_t)impl->n_cpus)
		return (double)runtime;
	double rc = impl->relative_capacity_nominal[sample_cpu];
	if (!(rc > 0.0))
		return (double)runtime;
	return (double)runtime * rc;
}

/*
 * Pure predicate that walks the runtime budget-source hierarchy and
 * returns the kind that should drive the kernel runtime for this
 * follower this cycle, along with the value (in reference-CPU ns)
 * and the sample count that backs it. Exactly three kinds exist:
 *
 *   1. RT_DIAG_BUDGET_MANUAL_OVERRIDE -- per-node operator override
 *      (no plumbing yet; reserved for a future per-node property).
 *      Always wins when set.
 *   2. RT_DIAG_BUDGET_DETERMINISTIC_WCET -- a hard static bound
 *      supplied by the plugin (no plumbing yet; reserved for a
 *      future plugin attribute). Used when the plugin exports it.
 *   3. RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL -- the adaptive-conformal
 *      estimator's published budget, whether the estimator is in
 *      BOOTSTRAP (using the configured bootstrap floor) or in
 *      VALID / SHIFT (using the score-ring quantile + EWMA). When
 *      the conformal estimator has not yet been instantiated for
 *      this follower (the very first sample has not arrived) the
 *      predicate reports the same kind with a peak-hold value as
 *      a degenerate floor; the operator can read samples_used = 0
 *      in the conformal sub-object to identify this state.
 *
 * The operator can constrain the hierarchy via deadline.budget.source:
 * BUDGET_SOURCE_BOOTSTRAP returns the peak-hold floor under the
 * conformal kind; the manual / deterministic / adaptive_conformal
 * values skip the kinds above the requested one. BUDGET_SOURCE_AUTO
 * (the default) walks the full hierarchy.
 *
 * Pure function: no PipeWire side effects, no global state mutation.
 * sample_ref is the reference-CPU-normalised most-recent sample;
 * peak_hold is the running maximum observed so far in the same
 * normalisation. Both are uint64 ns. period_ns is forwarded so a
 * future deterministic-bound source can refuse a budget > period.
 */
struct runtime_select_result {
	enum rt_diag_budget_kind kind;
	uint64_t                 value_ns;
	uint64_t                 sample_count;
};

static struct runtime_select_result runtime_select_for_node(
		const struct impl *impl,
		struct node *n,
		uint64_t sample_ref,
		uint64_t peak_hold,
		uint64_t period_ns SPA_UNUSED)
{
	struct runtime_select_result r = {
		.kind = RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL,
		.value_ns = SPA_MAX(peak_hold, sample_ref),
		.sample_count = 0,
	};
	enum rt_budget_source pref = impl->budget_source;

	/*
	 * Per-node manual override hook. No plugin property is wired
	 * to it yet; once a per-node deadline.manual_override.runtime_ns
	 * property exists this branch picks it up. Manual override
	 * always wins.
	 */
	if (false /* placeholder until per-node property lands */) {
		r.kind = RT_DIAG_BUDGET_MANUAL_OVERRIDE;
		return r;
	}
	if (pref == BUDGET_SOURCE_MANUAL)
		return r;

	/*
	 * Per-node deterministic WCET hook. No plugin attribute is
	 * wired to it yet; reserved for a future PW_KEY_NODE_WCET_NS
	 * or equivalent.
	 */
	if (false /* placeholder until plugin attribute lands */) {
		r.kind = RT_DIAG_BUDGET_DETERMINISTIC_WCET;
		return r;
	}
	if (pref == BUDGET_SOURCE_DETERMINISTIC)
		return r;

	/*
	 * Adaptive-conformal upper budget. Used when the estimator
	 * has cleared bootstrap (state == VALID, SHIFT). SHIFT is
	 * still publishable: the value remains a valid one-sided
	 * bound; the drift flag rides in the diagnostic surface.
	 */
	if (n->conformal != NULL &&
	    pref != BUDGET_SOURCE_BOOTSTRAP) {
		enum rt_conformal_state cs = rt_conformal_state(n->conformal);
		if (cs == RT_CONF_VALID || cs == RT_CONF_SHIFT) {
			uint64_t c = rt_conformal_budget(n->conformal, 0);
			if (c > 0) {
				r.kind = RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL;
				r.value_ns = SPA_MAX(c, sample_ref);
				r.sample_count = rt_conformal_samples_used(n->conformal);
				return r;
			}
		}
	}
	/* If the operator requested adaptive_conformal explicitly and
	 * the estimator is not yet ready, the predicate falls through
	 * to the peak-hold floor rather than holding the budget back. */

	/* Bootstrap fallback: peak-hold value (default of r). The
	 * conformal estimator owns its own bootstrap-with-immediate-
	 * start path, so a freshly-created follower lands here only
	 * for the very first activation; subsequent activations either
	 * stay on this path until the conformal estimator clears
	 * bootstrap, or switch to RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL. */
	return r;
}

/* Apply one sample to a follower's estimator. Worker-thread or RT-
 * thread (in sync mode); never both for a given node. sample_cpu is
 * the placement CPU the follower ran on; cycles is the
 * PERF_COUNT_HW_CPU_CYCLES delta the executor (impl-node.c) captured
 * around the node's process() call. When cycles > 0 the sketch holds
 * a frequency-invariant "ns at reference CPU peak throughput" value
 * derived directly from the cycle count, which removes the
 * assume-max wall-clock conservatism. When cycles == 0 (perf
 * unavailable: paranoid > 1, kernel too old, non-Linux) the worker
 * falls back to the wall-clock path that assumes the sample was
 * collected at max_freq.
 *
 * In both paths the sketch is reference-CPU-normalised, so the emit
 * step in sched_cb divides uniformly by
 * relative_capacity[placement_cpu] without caring how the sample
 * got there. */
static void apply_sample(struct impl *impl, struct node *n,
		uint64_t runtime, uint64_t cycles,
		uint32_t sample_cpu, uint64_t period)
{
	if (n->period != period) {
		if (n->conformal != NULL)
			rt_conformal_invalidate(n->conformal,
					RT_CONF_INVALIDATED_PERIOD);
		n->wcet = 0;
	}

	/* Lazy-init the adaptive-conformal estimator with the module's
	 * configured knobs. The estimator runs unconditionally so the
	 * conformal-source path is available the moment its state
	 * machine clears bootstrap; runtime_select_for_node gates
	 * whether its output reaches the kernel runtime field. */
	if (n->conformal == NULL) {
		n->conformal = rt_conformal_create(&impl->conformal_cfg);
		if (n->conformal == NULL)
			pw_log_warn("node %d: conformal estimator init failed",
				n->node ? n->node->info.id : (uint32_t)-1);
	}

	/* Prefer cycles when available: they are frequency-invariant
	 * by construction and yield a precise reference-CPU WCET
	 * without the assume-max inflation. */
	double sample_ref = wcet_cycles_to_reference_ns(impl, cycles, sample_cpu);
	if (sample_ref <= 0.0)
		sample_ref = wcet_sample_to_reference(impl, runtime, sample_cpu);

	/* Feed the conformal estimator the same sample. The observation
	 * flow obeys the prequential discipline internally (score
	 * computed against pre-observation EWMA state). */
	if (runtime > 0 && n->conformal != NULL && sample_ref > 0.0)
		(void)rt_conformal_observe(n->conformal, (uint64_t)sample_ref);

	/* Peak-hold floor: an outlier the estimators have not yet
	 * incorporated still raises the budget for the next cycle.
	 * Track the running max in n->wcet so the selection predicate
	 * can fall back on it. */
	{
		uint64_t sample_ref_u64 = sample_ref > 0.0
			? (uint64_t)sample_ref : 0;
		uint64_t peak_hold = SPA_MAX(n->wcet, sample_ref_u64);
		struct runtime_select_result sel = runtime_select_for_node(
				impl, n, sample_ref_u64, peak_hold, period);
		n->wcet = sel.value_ns;
		n->budget_kind = sel.kind;
	}

	/*
	 * Optional calibration trace export. When enabled the worker
	 * appends one JSON line per sample to the configured path; a
	 * Python replay tool (live-test/conformal_calibrate.py)
	 * consumes the file to reproduce the estimator's progression
	 * under alternative parameter grids without touching the
	 * production daemon. The RT path is unaffected -- this code
	 * runs on the audio recalc worker. The hook only emits when
	 * the conformal estimator actually accepted the sample
	 * (sample_ref > 0). */
	if (impl->conformal_cfg.trace_export &&
	    impl->conformal_trace_path != NULL &&
	    sample_ref > 0.0 && n->conformal != NULL) {
		if (impl->conformal_trace_fp == NULL) {
			impl->conformal_trace_fp = fopen(
					impl->conformal_trace_path, "a");
			if (impl->conformal_trace_fp == NULL)
				pw_log_warn("conformal trace fopen %s failed: %m",
					impl->conformal_trace_path);
		}
		if (impl->conformal_trace_fp != NULL) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			fprintf(impl->conformal_trace_fp,
				"{\"timestamp_ns\":%llu,"
				"\"entity_id\":%u,"
				"\"period_ns\":%llu,"
				"\"runtime_ns\":%llu,"
				"\"budget_ns\":%llu,"
				"\"budget_kind\":\"%s\","
				"\"conformal_state\":\"%s\","
				"\"conformal_alpha_eff\":%g,"
				"\"conformal_samples_used\":%llu}\n",
				(unsigned long long)
					((uint64_t)ts.tv_sec * 1000000000ULL +
					 (uint64_t)ts.tv_nsec),
				n->node ? n->node->info.id : (uint32_t)-1,
				(unsigned long long)period,
				(unsigned long long)(sample_ref > 0.0 ?
					(uint64_t)sample_ref : 0),
				(unsigned long long)n->wcet,
				rt_diag_budget_kind_name(n->budget_kind),
				rt_conformal_state_name(
					rt_conformal_state(n->conformal)),
				rt_conformal_alpha_eff(n->conformal),
				(unsigned long long)
					rt_conformal_samples_used(n->conformal));
		}
	}

	n->period = period;
}

/* ------------------------------------------------------------------
 * Sync path: original behaviour, now running on the same reconcile
 * orchestrator the async worker uses. Builds the topology view from
 * the driver's rt.target_list and the live pw_impl_node graph (not
 * from a topo snapshot, because the sync path runs inside the RT
 * hook and has direct access), then hands it to reconcile_apply.
 * Selected via recalc.sync=true; recalc.persistent and
 * wcet.recalc-threshold apply just like in the async case.
 * ------------------------------------------------------------------ */
static void recalc_params_sync(struct node *drv)
{
	struct pw_impl_node *node = drv->node;
	struct impl *impl = drv->impl;
	struct pw_node_target *t;
	uint64_t period;
	reconcile_follower_t *followers;
	reconcile_edge_t *edges;
	uint32_t followers_cap;
	uint32_t edges_cap;
	uint32_t n_followers = 0;
	uint32_t n_edges = 0;
	reconcile_topo_t rtopo = { 0 };

	if (node->target_rate.denom == 0 || node->target_quantum == 0)
		return;

	period = SPA_NSEC_PER_SEC * node->target_quantum / node->target_rate.denom;

	if (drv->reconcile == NULL) {
		drv->reconcile = reconcile_init((uint32_t)impl->n_cpus,
				impl->cpu_utilization,
				impl->relative_capacity,
				impl->wcet_recalc_threshold,
				impl->recalc_persistent);
		if (drv->reconcile == NULL) {
			pw_log_warn("reconcile_init failed: %m");
			return;
		}
	}

	/* Pre-size the follower/edge arrays generously; realloc on
	 * overflow. */
	followers_cap = 32;
	edges_cap = 64;
	followers = calloc(followers_cap, sizeof(*followers));
	edges = calloc(edges_cap, sizeof(*edges));
	if (!followers || !edges) {
		free(followers);
		free(edges);
		return;
	}

	spa_list_for_each(t, &node->rt.target_list, link) {
		struct pw_impl_node *tnode = t->node;
		struct pw_node_activation *na;
		uint64_t runtime;
		pid_t tid;
		struct node *n;

		if (!pw_properties_get_bool(tnode->properties,
				PW_KEY_NODE_LOOP_DYNAMIC, false))
			continue;
		tid = pw_properties_get_int32(tnode->properties,
				PW_KEY_NODE_LOOP_TID, -1);
		if (tid == -1)
			continue;

		n = find_node(impl, tnode);
		if (n == NULL) {
			n = calloc(1, sizeof(*n));
			if (!n)
				continue;
			n->impl = impl;
			n->node = tnode;
			n->node_id = tnode->info.id;
			n->enabled = true;
			spa_list_insert(&impl->node_list, &n->link);
			if (node_register(impl, n) < 0) {
				spa_list_remove(&n->link);
				free(n);
				continue;
			}
		}

		na = t->activation;
		runtime = get_runtime_ns(tnode, na);
		if (runtime > period)
			pw_log_warn("node %d runtime %lu exceeds period %lu",
				    tnode->info.id, runtime, period);

		apply_sample(impl, n, runtime,
				SPA_ATOMIC_LOAD(na->prev_run_cycles),
				n->last_applied ? n->last_cpu : SAMPLE_CPU_UNKNOWN,
				period);

		if (n_followers >= followers_cap) {
			uint32_t new_cap = followers_cap * 2;
			reconcile_follower_t *r = realloc(followers,
					new_cap * sizeof(*followers));
			if (!r)
				continue;
			followers = r;
			followers_cap = new_cap;
		}
		followers[n_followers].id = tnode->info.id;
		followers[n_followers].tid = tid;
		followers[n_followers].wcet = n->wcet;
		n_followers++;
	}

	if (n_followers == 0) {
		free(followers);
		free(edges);
		return;
	}

	/* Edges: same feedback/async filter as the snapshot path. */
	spa_list_for_each(t, &node->rt.target_list, link) {
		struct pw_impl_node *tnode = t->node;
		struct pw_impl_port *p;
		struct pw_impl_link *l;

		spa_list_for_each(p, &tnode->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				if (!l->input || !l->input->node)
					continue;
				if (l->feedback)
					continue;
				if (l->output->node && l->input->node &&
						(l->output->node->async ||
						 l->input->node->async))
					continue;
				if (n_edges >= edges_cap) {
					uint32_t new_cap = edges_cap * 2;
					reconcile_edge_t *r = realloc(edges,
							new_cap * sizeof(*edges));
					if (!r)
						continue;
					edges = r;
					edges_cap = new_cap;
				}
				edges[n_edges].src = tnode->info.id;
				edges[n_edges].dst = l->input->node->info.id;
				n_edges++;
			}
		}
	}

	/* Compute a cheap topology fingerprint (FNV-1a-ish hash over
	 * the follower-id list and edge-src/dst pairs) so the
	 * persistent reconcile path can short-circuit when nothing
	 * structural changed. The sync path runs every audio cycle;
	 * bumping the generation every call would defeat the
	 * persistent-DAG optimisation. */
	{
		uint64_t h = 0xcbf29ce484222325ULL;
		uint32_t i;
		for (i = 0; i < n_followers; i++) {
			h ^= followers[i].id;
			h *= 0x100000001b3ULL;
			h ^= (uint64_t)followers[i].tid;
			h *= 0x100000001b3ULL;
		}
		for (i = 0; i < n_edges; i++) {
			h ^= edges[i].src;
			h *= 0x100000001b3ULL;
			h ^= edges[i].dst;
			h *= 0x100000001b3ULL;
		}
		if (h != drv->topo.generation) {
			drv->topo.generation = h;
		}
	}

	rtopo.followers = followers;
	rtopo.n_followers = n_followers;
	rtopo.edges = edges;
	rtopo.n_edges = n_edges;
	rtopo.period = period;
	rtopo.generation = drv->topo.generation;

	sched_groups_reset(&impl->sched_groups);
	(void)reconcile_apply(drv->reconcile, &rtopo, sched_cb, impl);
	apply_sched_groups(impl, drv);
	sample_voluntary_ctxt_switches_main(impl, drv);

	free(followers);
	free(edges);
}

/* ------------------------------------------------------------------
 * Async path: the RT hook just pushes per-target samples to a per-
 * driver SPSC ring and signals the worker via an eventfd. All the
 * heavy work (sketch updates, DAG build, P-EDF analysis, syscalls)
 * happens on the worker thread, which runs at a low SCHED_FIFO prio
 * and is CPU-pinned away from the deadline cores so it never shares
 * a CPU with the threads it configures.
 * ------------------------------------------------------------------ */

/* RT-context: push samples into drv's ring. Drops if full (worker is
 * lagging); the WCET sketch tail stats absorb the loss naturally. */
static void rt_push_samples(struct node *drv, uint64_t period)
{
	struct pw_impl_node *node = drv->node;
	struct pw_node_target *t;
	uint32_t buf_size_bytes = drv->ring_capacity * sizeof(struct sample);

	spa_list_for_each(t, &node->rt.target_list, link) {
		struct pw_impl_node *tnode = t->node;
		uint64_t runtime = get_runtime_ns(tnode, t->activation);

		uint32_t widx;
		int32_t filled = spa_ringbuffer_get_write_index(&drv->ring, &widx);
		if (filled < 0 || (uint32_t)filled >= buf_size_bytes) {
			drv->ring_dropped++;
			continue;
		}
		uint32_t offset = (widx % buf_size_bytes);
		/* Slot-aligned writes only; capacity is power of two of slot
		 * size so offset always lands on a slot boundary. */
		struct sample *s = (struct sample *)((uint8_t *)drv->ring_slots + offset);
		s->node_id = tnode->info.id;
		/* Stamp the follower's current placement CPU. The
		 * follower has been pinned by sched_setaffinity since
		 * the previous reconcile, so this is the CPU its thread
		 * actually ran on for this cycle. Falls back to
		 * SAMPLE_CPU_UNKNOWN before the first placement; the
		 * worker then skips normalisation and treats the sample
		 * as already in reference-CPU units. */
		struct node *n_lookup = find_node_by_id(drv->impl,
				tnode->info.id);
		s->cpu = (n_lookup != NULL && n_lookup->last_applied) ?
				n_lookup->last_cpu : SAMPLE_CPU_UNKNOWN;
		s->runtime_ns = runtime;
		s->cycles = SPA_ATOMIC_LOAD(t->activation->prev_run_cycles);
		s->period_ns = period;
		spa_ringbuffer_write_update(&drv->ring, widx + sizeof(struct sample));
	}
}

/* Worker-context: drain everything the producer wrote since last time. */
static void worker_drain_samples(struct impl *impl, struct node *drv)
{
	uint32_t buf_size_bytes = drv->ring_capacity * sizeof(struct sample);
	uint32_t ridx;
	int32_t avail = spa_ringbuffer_get_read_index(&drv->ring, &ridx);
	if (avail <= 0)
		return;

	/* Process slot-by-slot. */
	uint32_t processed = 0;
	while ((uint32_t)avail - processed >= sizeof(struct sample)) {
		uint32_t offset = ((ridx + processed) % buf_size_bytes);
		const struct sample *s = (const struct sample *)
			((uint8_t *)drv->ring_slots + offset);

		struct node *n = find_node_by_id(impl, s->node_id);
		if (n == NULL) {
			n = calloc(1, sizeof(*n));
			if (n) {
				n->impl = impl;
				n->node_id = s->node_id;
				n->enabled = true;
				/* n->node stays NULL: the worker never
				 * dereferences pw_impl_node *; the
				 * topology snapshot carries the id+tid
				 * we need to apply DEADLINE. */
				spa_list_insert(&impl->node_list, &n->link);
				if (node_register(impl, n) < 0) {
					spa_list_remove(&n->link);
					free(n);
					n = NULL;
				}
			}
		}
		if (n)
			apply_sample(impl, n, s->runtime_ns, s->cycles,
					s->cpu, s->period_ns);

		processed += sizeof(struct sample);
	}
	spa_ringbuffer_read_update(&drv->ring, ridx + processed);
	pw_log_trace("worker drained %u samples (drv-node=%d)",
		     processed / (uint32_t)sizeof(struct sample),
		     drv->node ? drv->node->info.id : (uint32_t)-1);
}

/* Build a raw-graph diagnostic snapshot of the driver as seen on the
 * main loop and emit it via pw_log_info. The dump is the unfiltered
 * view: every follower the driver reports, every link the daemon
 * exposes, with each follower's analyzability bits and each edge's
 * within-period flags recorded as bitmasks the renderer translates
 * into stable tokens. The eventual scheduling-DAG slice will
 * subtract from this view -- async / feedback / exported / main-loop
 * / unknown-tid entries that do not contribute an in-period
 * precedence constraint -- but observability happens first, before
 * inclusion semantics change.
 *
 * The function is called from the snapshot path, which already
 * walks the same follower / port / link lists for the worker's
 * topology view, so the work is bounded by the same set the
 * scheduler already pays for once per topology-fingerprint change.
 */

/* Emit a multi-line text buffer one logical line per pw_log_info
 * call so the PipeWire logger renders it the same way hist_dump
 * does. The buffer is mutated in place (the newline is stomped with
 * '\0' to terminate the line) -- callers must own a mutable copy. */
static void log_info_lines(char *buf)
{
	char *cursor = buf;
	while (*cursor != '\0') {
		char *nl = strchr(cursor, '\n');
		if (nl != NULL)
			*nl = '\0';
		pw_log_info("%s", cursor);
		if (nl == NULL)
			break;
		cursor = nl + 1;
	}
}

static void dump_raw_graph_main(struct impl *impl, struct node *drv)
{
	struct pw_impl_node *dnode = drv->node;
	struct pw_impl_node *n_iter;
	struct rt_diag_raw_snapshot snap;
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	int r;

	rt_diag_raw_snapshot_init(&snap);
	snap.driver_id = dnode->info.id;
	snap.generation = SPA_ATOMIC_LOAD(drv->topo.generation);
	snap.period_ns = drv->topo.period;
	/* End-to-end deadline reporting is the scheduling-DAG slice's
	 * responsibility; until that lands, mirror the period so the
	 * field is still meaningful for a same-period driver. */
	snap.deadline_ns = drv->topo.period;

	/* Walk every follower attached to the driver (the driver
	 * itself is in its own follower_list, surfaced with the
	 * driver bit). */
	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		struct rt_diag_raw_node rn;
		memset(&rn, 0, sizeof(rn));
		rn.id = n_iter->info.id;
		rn.driver_id = dnode->info.id;
		rn.tid = pw_properties_get_int32(n_iter->properties,
				PW_KEY_NODE_LOOP_TID, -1);
		rn.flags = RT_DIAG_RAW_NODE_DATA_LOOP;
		if (n_iter == dnode)
			rn.flags |= RT_DIAG_RAW_NODE_DRIVER;
		if (n_iter->async)
			rn.flags |= RT_DIAG_RAW_NODE_ASYNC;
		if (n_iter->remote)
			rn.flags |= RT_DIAG_RAW_NODE_REMOTE;
		if (n_iter->exported)
			rn.flags |= RT_DIAG_RAW_NODE_EXPORTED;
		if (pw_properties_get_bool(n_iter->properties,
				PW_KEY_NODE_LOOP_DYNAMIC, false))
			rn.flags |= RT_DIAG_RAW_NODE_DYNAMIC_LOOP;
		if (n_iter->name != NULL)
			snprintf(rn.name, sizeof(rn.name), "%s", n_iter->name);
		r = rt_diag_raw_snapshot_add_node(&snap, &rn);
		if (r < 0)
			goto cleanup;
	}

	/* Edges: walk every follower's output ports and the link list
	 * on each port. Feedback / async links are surfaced via flags
	 * so the renderer's reader can correlate a raw edge against
	 * the eventual scheduling-DAG exclusion list. */
	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		struct pw_impl_port *p;
		struct pw_impl_link *l;
		spa_list_for_each(p, &n_iter->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				struct rt_diag_raw_edge re = { 0 };
				if (l->input == NULL || l->input->node == NULL)
					continue;
				re.src = n_iter->info.id;
				re.dst = l->input->node->info.id;
				if (l->feedback)
					re.flags |= RT_DIAG_RAW_EDGE_FEEDBACK;
				if (l->output != NULL && l->output->node != NULL &&
				    (l->output->node->async ||
				     l->input->node->async))
					re.flags |= RT_DIAG_RAW_EDGE_ASYNC;
				r = rt_diag_raw_snapshot_add_edge(&snap, &re);
				if (r < 0)
					goto cleanup;
			}
		}
	}

	fp = open_memstream(&buf, &len);
	if (fp == NULL)
		goto cleanup;
	rt_diag_raw_snapshot_render_text(&snap, fp);
	fclose(fp);
	if (buf != NULL)
		log_info_lines(buf);

cleanup:
	free(buf);
	rt_diag_raw_snapshot_fini(&snap);
}

/* Is `follower` a candidate for the scheduling-DAG inclusion set?
 * The rule mirrors the existing topology-snapshot filter: a follower
 * that publishes PW_KEY_NODE_LOOP_DYNAMIC plus a numeric
 * PW_KEY_NODE_LOOP_TID is on a dedicated data-loop thread the
 * daemon can configure with SCHED_DEADLINE. Any other follower is
 * excluded from the scheduling DAG with an UNSUPPORTED reason on
 * its edges. */
static bool sched_dag_follower_in_set(struct pw_impl_node *follower)
{
	if (!pw_properties_get_bool(follower->properties,
			PW_KEY_NODE_LOOP_DYNAMIC, false))
		return false;
	return pw_properties_get_int32(follower->properties,
			PW_KEY_NODE_LOOP_TID, -1) >= 0;
}

/* Per-edge exclusion reason, derived from the pw_impl_link state
 * the topology pass already consults. Returns RT_DIAG_SCHED_EXC_NONE
 * for an edge that should land in the included-edges list. */
static enum rt_diag_sched_exclude_reason sched_dag_edge_reason(
		struct pw_impl_link *l, struct pw_impl_node *src,
		struct pw_impl_node *dst)
{
	/* Endpoint-missing degenerates to UNSUPPORTED before we touch
	 * the per-endpoint flags: a NULL endpoint has neither async nor
	 * exported booleans to query. The pure classifier below has no
	 * way to express "missing endpoint" other than via the in-set
	 * booleans, so handle it explicitly here. */
	return rt_diag_sched_classify_edge(
			l->feedback,
			src != NULL && src->async,
			dst != NULL && dst->async,
			src != NULL && src->exported,
			dst != NULL && dst->exported,
			src != NULL && sched_dag_follower_in_set(src),
			dst != NULL && sched_dag_follower_in_set(dst));
}

/* Build a scheduling-DAG diagnostic snapshot and emit it via
 * pw_log_info. The included set is the same one the existing
 * topology pass passes to the worker (dynamic-loop followers with a
 * known TID, plus their feedback-/async-free links). The excluded
 * list records every link that did not make it into that set,
 * tagged with the single reason that drove the exclusion. */
static void dump_sched_graph_main(struct impl *impl, struct node *drv)
{
	struct pw_impl_node *dnode = drv->node;
	struct pw_impl_node *n_iter;
	struct rt_diag_sched_snapshot snap;
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;

	rt_diag_sched_snapshot_init(&snap);
	snap.driver_id = dnode->info.id;
	snap.generation = SPA_ATOMIC_LOAD(drv->topo.generation);
	snap.period_ns = drv->topo.period;
	snap.deadline_ns = drv->topo.period;

	/* The driver itself is implicitly part of the schedulable
	 * set: it is the activation root of the in-period DAG.
	 * Surface it so the included-nodes list is reader-friendly. */
	{
		struct rt_diag_sched_node sn = { 0 };
		sn.id = dnode->info.id;
		sn.tid = pw_properties_get_int32(dnode->properties,
				PW_KEY_NODE_LOOP_TID, -1);
		if (rt_diag_sched_snapshot_add_node(&snap, &sn) < 0)
			goto cleanup;
	}
	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		if (n_iter == dnode)
			continue;
		if (!sched_dag_follower_in_set(n_iter))
			continue;
		struct rt_diag_sched_node sn = { 0 };
		sn.id = n_iter->info.id;
		sn.tid = pw_properties_get_int32(n_iter->properties,
				PW_KEY_NODE_LOOP_TID, -1);
		if (rt_diag_sched_snapshot_add_node(&snap, &sn) < 0)
			goto cleanup;
	}

	/* Walk every output-port link once; each link is classified
	 * by sched_dag_edge_reason. RT_DIAG_SCHED_EXC_NONE lands in
	 * the included list; anything else lands in the excluded
	 * list with its reason. */
	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		struct pw_impl_port *p;
		struct pw_impl_link *l;
		spa_list_for_each(p, &n_iter->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				struct pw_impl_node *src, *dst;
				enum rt_diag_sched_exclude_reason r;

				if (l->input == NULL || l->input->node == NULL)
					continue;
				src = n_iter;
				dst = l->input->node;
				r = sched_dag_edge_reason(l, src, dst);
				if (r == RT_DIAG_SCHED_EXC_NONE) {
					struct rt_diag_sched_edge se = { 0 };
					se.src = src->info.id;
					se.dst = dst->info.id;
					if (rt_diag_sched_snapshot_add_edge(&snap, &se) < 0)
						goto cleanup;
				} else {
					struct rt_diag_sched_excluded_edge xe = { 0 };
					xe.src = src->info.id;
					xe.dst = dst->info.id;
					xe.reason = r;
					if (rt_diag_sched_snapshot_add_excluded(&snap, &xe) < 0)
						goto cleanup;
				}
			}
		}
	}

	fp = open_memstream(&buf, &len);
	if (fp == NULL)
		goto cleanup;
	rt_diag_sched_snapshot_render_text(&snap, fp);
	fclose(fp);
	if (buf != NULL)
		log_info_lines(buf);

cleanup:
	free(buf);
	rt_diag_sched_snapshot_fini(&snap);
}

/* Translate a pw_impl_node fusion verdict into the diag enum. The
 * core's enum (see fusion-cost.h) is intentionally kept separate
 * from the diag enum so a future verdict addition does not silently
 * change the dump format. */
static enum rt_diag_fusion_verdict diag_verdict_from_core(
		enum pw_fusion_decision d)
{
	switch (d) {
	case PW_FUSION_DECISION_FUSE:        return RT_DIAG_FUSION_FUSE;
	case PW_FUSION_DECISION_LINEAR_ONLY: return RT_DIAG_FUSION_LINEAR_ONLY;
	case PW_FUSION_DECISION_SPLIT:       return RT_DIAG_FUSION_SPLIT;
	}
	return RT_DIAG_FUSION_SPLIT;
}

/* Build a fusion-decision diagnostic snapshot and emit it via
 * pw_log_info. The dump aggregates followers into groups keyed by
 * the PW_KEY_NODE_LOOP_GROUP property (singleton when absent) and
 * stamps each group with the applied verdict from
 * pw_impl_node::fusion_prev_decision. Today, the core fusion-cost
 * model emits a binary FUSE / LINEAR_ONLY / SPLIT verdict driven by
 * the Sarkar 1989 inequality; a non-FUSE verdict is rendered with
 * reason=below_threshold. When the fusion soundness validator
 * lands, it will extend the reject-reason enum with structural
 * codes (non_convex, internal_milestone, blocking_risk, ...) and
 * stamp them on this slice without changing the rendered shape. */
static void dump_fusion_main(struct impl *impl, struct node *drv)
{
	struct pw_impl_node *dnode = drv->node;
	struct pw_impl_node *n_iter;
	struct rt_diag_fusion_snapshot snap;
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;

	rt_diag_fusion_snapshot_init(&snap);
	snap.driver_id = dnode->info.id;
	snap.generation = SPA_ATOMIC_LOAD(drv->topo.generation);

	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		const char *group_name;
		enum rt_diag_fusion_verdict verdict;
		enum rt_diag_fusion_reject_reason reason;
		int slot;
		uint32_t g;
		bool found;

		group_name = pw_properties_get(n_iter->properties,
				PW_KEY_NODE_LOOP_GROUP);
		verdict = diag_verdict_from_core(n_iter->fusion_prev_decision);
		reason = (verdict == RT_DIAG_FUSION_FUSE)
				? RT_DIAG_FUSION_REJ_NONE
				: RT_DIAG_FUSION_REJ_BELOW_THRESHOLD;

		/* Look for an existing group that matches by leader_id
		 * (when group_name is "fusion.<leader>"); fall back to a
		 * scan when the group name shape changes. Followers
		 * without PW_KEY_NODE_LOOP_GROUP get their own singleton
		 * group keyed by node id. */
		found = false;
		for (g = 0; g < snap.n_groups && !found; g++) {
			struct rt_diag_fusion_group *grp = &snap.groups[g];
			if (group_name != NULL) {
				char buf2[64];
				snprintf(buf2, sizeof(buf2), "fusion.%u",
					 grp->leader_id);
				if (strcmp(buf2, group_name) == 0) {
					(void)rt_diag_fusion_snapshot_add_member(
						&snap, g, n_iter->info.id);
					found = true;
				}
			}
		}
		if (!found) {
			uint32_t leader = n_iter->info.id;
			if (group_name != NULL &&
			    strncmp(group_name, "fusion.", 7) == 0) {
				unsigned long parsed = strtoul(group_name + 7,
						NULL, 10);
				if (parsed != 0 && parsed <= UINT32_MAX)
					leader = (uint32_t)parsed;
			}
			slot = rt_diag_fusion_snapshot_begin_group(&snap,
					leader, verdict, reason);
			if (slot < 0)
				goto cleanup;
			(void)rt_diag_fusion_snapshot_add_member(&snap,
					(uint32_t)slot, n_iter->info.id);
		}
	}

	fp = open_memstream(&buf, &len);
	if (fp == NULL)
		goto cleanup;
	rt_diag_fusion_snapshot_render_text(&snap, fp);
	fclose(fp);
	if (buf != NULL)
		log_info_lines(buf);

cleanup:
	free(buf);
	rt_diag_fusion_snapshot_fini(&snap);
}

/* Populate `snap` with the raw-graph view of `drv`. Mirrors the
 * walk done by dump_raw_graph_main but stops short of rendering;
 * the JSON emitter feeds the rt_diag_combined struct from the
 * resulting snapshots. Returns 0 on success, -ENOMEM on
 * allocation failure (the caller is expected to abandon the
 * combined dump rather than emit a partial document). */
static int populate_raw_snapshot(struct impl *impl SPA_UNUSED,
		struct node *drv, struct rt_diag_raw_snapshot *snap)
{
	struct pw_impl_node *dnode = drv->node;
	struct pw_impl_node *n_iter;
	int r;

	snap->driver_id = dnode->info.id;
	snap->generation = SPA_ATOMIC_LOAD(drv->topo.generation);
	snap->period_ns = drv->topo.period;
	snap->deadline_ns = drv->topo.period;

	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		struct rt_diag_raw_node rn;
		memset(&rn, 0, sizeof(rn));
		rn.id = n_iter->info.id;
		rn.driver_id = dnode->info.id;
		rn.tid = pw_properties_get_int32(n_iter->properties,
				PW_KEY_NODE_LOOP_TID, -1);
		rn.flags = RT_DIAG_RAW_NODE_DATA_LOOP;
		if (n_iter == dnode)
			rn.flags |= RT_DIAG_RAW_NODE_DRIVER;
		if (n_iter->async)
			rn.flags |= RT_DIAG_RAW_NODE_ASYNC;
		if (n_iter->remote)
			rn.flags |= RT_DIAG_RAW_NODE_REMOTE;
		if (n_iter->exported)
			rn.flags |= RT_DIAG_RAW_NODE_EXPORTED;
		if (pw_properties_get_bool(n_iter->properties,
				PW_KEY_NODE_LOOP_DYNAMIC, false))
			rn.flags |= RT_DIAG_RAW_NODE_DYNAMIC_LOOP;
		if (n_iter->name != NULL)
			snprintf(rn.name, sizeof(rn.name), "%s", n_iter->name);
		r = rt_diag_raw_snapshot_add_node(snap, &rn);
		if (r < 0)
			return r;
	}

	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		struct pw_impl_port *p;
		struct pw_impl_link *l;
		spa_list_for_each(p, &n_iter->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				struct rt_diag_raw_edge re = { 0 };
				if (l->input == NULL || l->input->node == NULL)
					continue;
				re.src = n_iter->info.id;
				re.dst = l->input->node->info.id;
				if (l->feedback)
					re.flags |= RT_DIAG_RAW_EDGE_FEEDBACK;
				if (l->output != NULL && l->output->node != NULL &&
				    (l->output->node->async ||
				     l->input->node->async))
					re.flags |= RT_DIAG_RAW_EDGE_ASYNC;
				r = rt_diag_raw_snapshot_add_edge(snap, &re);
				if (r < 0)
					return r;
			}
		}
	}
	return 0;
}

static int populate_sched_snapshot(struct impl *impl SPA_UNUSED,
		struct node *drv, struct rt_diag_sched_snapshot *snap)
{
	struct pw_impl_node *dnode = drv->node;
	struct pw_impl_node *n_iter;
	int r;

	snap->driver_id = dnode->info.id;
	snap->generation = SPA_ATOMIC_LOAD(drv->topo.generation);
	snap->period_ns = drv->topo.period;
	snap->deadline_ns = drv->topo.period;

	{
		struct rt_diag_sched_node sn = { 0 };
		sn.id = dnode->info.id;
		sn.tid = pw_properties_get_int32(dnode->properties,
				PW_KEY_NODE_LOOP_TID, -1);
		r = rt_diag_sched_snapshot_add_node(snap, &sn);
		if (r < 0)
			return r;
	}
	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		if (n_iter == dnode)
			continue;
		if (!sched_dag_follower_in_set(n_iter))
			continue;
		struct rt_diag_sched_node sn = { 0 };
		sn.id = n_iter->info.id;
		sn.tid = pw_properties_get_int32(n_iter->properties,
				PW_KEY_NODE_LOOP_TID, -1);
		r = rt_diag_sched_snapshot_add_node(snap, &sn);
		if (r < 0)
			return r;
	}

	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		struct pw_impl_port *p;
		struct pw_impl_link *l;
		spa_list_for_each(p, &n_iter->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				struct pw_impl_node *src, *dst;
				enum rt_diag_sched_exclude_reason reason;

				if (l->input == NULL || l->input->node == NULL)
					continue;
				src = n_iter;
				dst = l->input->node;
				reason = sched_dag_edge_reason(l, src, dst);
				if (reason == RT_DIAG_SCHED_EXC_NONE) {
					struct rt_diag_sched_edge se = { 0 };
					se.src = src->info.id;
					se.dst = dst->info.id;
					r = rt_diag_sched_snapshot_add_edge(snap, &se);
				} else {
					struct rt_diag_sched_excluded_edge xe = { 0 };
					xe.src = src->info.id;
					xe.dst = dst->info.id;
					xe.reason = reason;
					r = rt_diag_sched_snapshot_add_excluded(snap, &xe);
				}
				if (r < 0)
					return r;
			}
		}
	}
	return 0;
}

static int populate_fusion_snapshot(struct impl *impl SPA_UNUSED,
		struct node *drv, struct rt_diag_fusion_snapshot *snap)
{
	struct pw_impl_node *dnode = drv->node;
	struct pw_impl_node *n_iter;

	snap->driver_id = dnode->info.id;
	snap->generation = SPA_ATOMIC_LOAD(drv->topo.generation);

	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		const char *group_name;
		enum rt_diag_fusion_verdict verdict;
		enum rt_diag_fusion_reject_reason reason;
		int slot;
		uint32_t g;
		bool found = false;

		group_name = pw_properties_get(n_iter->properties,
				PW_KEY_NODE_LOOP_GROUP);
		verdict = diag_verdict_from_core(n_iter->fusion_prev_decision);
		reason = (verdict == RT_DIAG_FUSION_FUSE)
				? RT_DIAG_FUSION_REJ_NONE
				: RT_DIAG_FUSION_REJ_BELOW_THRESHOLD;

		for (g = 0; g < snap->n_groups && !found; g++) {
			struct rt_diag_fusion_group *grp = &snap->groups[g];
			if (group_name != NULL) {
				char buf2[64];
				snprintf(buf2, sizeof(buf2), "fusion.%u",
					 grp->leader_id);
				if (strcmp(buf2, group_name) == 0) {
					int r = rt_diag_fusion_snapshot_add_member(
						snap, g, n_iter->info.id);
					if (r < 0)
						return r;
					found = true;
				}
			}
		}
		if (!found) {
			uint32_t leader = n_iter->info.id;
			if (group_name != NULL &&
			    strncmp(group_name, "fusion.", 7) == 0) {
				unsigned long parsed = strtoul(group_name + 7,
						NULL, 10);
				if (parsed != 0 && parsed <= UINT32_MAX)
					leader = (uint32_t)parsed;
			}
			slot = rt_diag_fusion_snapshot_begin_group(snap,
					leader, verdict, reason);
			if (slot < 0)
				return slot;
			int r = rt_diag_fusion_snapshot_add_member(snap,
					(uint32_t)slot, n_iter->info.id);
			if (r < 0)
				return r;
		}
	}
	return 0;
}

/* The per-follower (runtime, deadline, period, cpu) tuple lives on
 * struct node's last_applied cache (set by sched_cb on every
 * successful sched_setattr). Followers that have not yet been
 * touched by the scheduler report applied=false; their numeric
 * fields are zero. The cache is written by the worker thread and
 * read here on the main loop -- the values are aligned uint64_t /
 * uint32_t fields and the read may see a value one cycle stale, but
 * never torn on this target. */
static int populate_params_snapshot(struct impl *impl,
		struct node *drv, struct rt_diag_params_snapshot *snap)
{
	struct pw_impl_node *dnode = drv->node;
	struct pw_impl_node *n_iter;
	int r;

	snap->driver_id = dnode->info.id;
	snap->generation = SPA_ATOMIC_LOAD(drv->topo.generation);

	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		if (n_iter == dnode)
			continue;
		if (!sched_dag_follower_in_set(n_iter))
			continue;

		struct node *mn = find_node_by_id(impl, n_iter->info.id);
		struct rt_diag_param_node pn = { 0 };
		pn.id = n_iter->info.id;
		pn.tid = pw_properties_get_int32(n_iter->properties,
				PW_KEY_NODE_LOOP_TID, -1);
		if (mn != NULL && mn->last_applied) {
			pn.runtime_budget_ns = mn->last_runtime;
			pn.local_deadline_ns = mn->last_deadline;
			/* The cumulative deadline is graph-relative and
			 * stamped per-follower by sched_cb; it may be
			 * zero on a follower whose first sched_cb has
			 * not yet fired (a transient that the next
			 * recalc round clears). */
			pn.cumulative_deadline_ns = mn->last_cumulative_deadline
				!= 0 ? mn->last_cumulative_deadline
				     : mn->last_deadline;
			pn.period_ns = mn->last_period;
			pn.cpu = mn->last_cpu;
			pn.applied = true;
		} else {
			pn.applied = false;
		}
		/*
		 * Budget provenance. The adaptive-conformal estimator
		 * (Romano, Patterson & Candes 2019; Gibbs & Candes
		 * 2021) is the active source for the kernel runtime
		 * budget today; the predicate runtime_select_for_node
		 * stamped the per-follower budget_kind at sample time.
		 */
		/*
		 * The budget-source predicate stamped the per-follower
		 * fields when the last sample arrived; surface them
		 * verbatim so the JSON snapshot reflects the kind that
		 * actually drove the kernel runtime for the most recent
		 * apply pass, not a re-derived classification.
		 */
		if (mn != NULL) {
			pn.budget_kind = mn->budget_kind;
		} else {
			pn.budget_kind = RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL;
		}
		/* Release-barrier and blocking-observation surfacing.
		 * required_external_inputs comes from the contracted DAG
		 * (the post-contraction predecessor count for this
		 * follower's macro-node); voluntary_ctxt_switches_in_process
		 * carries the most recent excess yields per recalc
		 * interval as computed by the per-recalc
		 * /proc/<tid>/status sampler. A non-zero excess has
		 * already triggered a soft-degraded demotion at sample
		 * time; surfacing it here lets an operator correlate the
		 * mode transition with the offending follower. */
		pn.required_external_inputs =
			reconcile_state_node_required_external_inputs(
				drv->reconcile, n_iter->info.id);
		pn.voluntary_ctxt_switches_in_process =
			(mn != NULL) ? mn->voluntary_ctxt_switches_in_process : 0;

		/*
		 * Adaptive-conformal diagnostics. Pull the state every
		 * snapshot pass so the JSON reader sees the current
		 * EWMA pair, the current alpha_eff, and the overrun
		 * counters that the estimator updated since the last
		 * snapshot. When the conformal estimator has not yet
		 * been instantiated for this follower (e.g. a driver
		 * sentinel) the fields stay at zero -- the JSON token
		 * still reports "insufficient_data" so a parser sees a
		 * stable shape regardless.
		 */
		if (mn != NULL && mn->conformal != NULL) {
			rt_conformal_t *c = mn->conformal;
			pn.conformal_state = (uint8_t)rt_conformal_state(c);
			pn.conformal_samples_seen =
				rt_conformal_samples_seen(c);
			pn.conformal_samples_used =
				rt_conformal_samples_used(c);
			pn.conformal_overruns_seen =
				rt_conformal_overruns_seen(c);
			pn.conformal_recent_overruns =
				rt_conformal_recent_overruns(c);
			pn.conformal_max_overrun_burst =
				rt_conformal_max_overrun_burst(c);
			pn.conformal_current_overrun_burst =
				rt_conformal_current_overrun_burst(c);
			pn.conformal_alpha_target =
				impl->conformal_cfg.alpha_target;
			pn.conformal_alpha_eff = rt_conformal_alpha_eff(c);
			pn.conformal_window = impl->conformal_cfg.window;
			pn.conformal_ewma_location_ns =
				rt_conformal_mu_ns(c);
			pn.conformal_ewma_scale_ns =
				rt_conformal_scale_ns(c);
			pn.conformal_score_quantile =
				rt_conformal_score_quantile(c);
			pn.conformal_guard_ns =
				rt_conformal_guard_ns_effective(c);
			pn.conformal_guard_percent =
				impl->conformal_cfg.guard_percent;
			pn.conformal_runtime_floor_ns =
				impl->conformal_cfg.runtime_floor_ns;
			pn.conformal_last_runtime_ns =
				rt_conformal_last_runtime_ns(c);
			pn.conformal_last_prediction_ns =
				rt_conformal_last_prediction_ns(c);
			pn.conformal_last_score = rt_conformal_last_score(c);
			pn.conformal_last_budget_ns =
				rt_conformal_last_budget_ns(c);
			pn.conformal_last_invalidation_reason = (uint8_t)
				rt_conformal_last_invalidation_reason(c);
		}

		/*
		 * Soft-mode budget_clipped / risk_objective_value
		 * surfacing. The flag lives on the dag_node after
		 * dag_soft_redistribute_deadlines; queried by id via
		 * the reconcile state so we do not need to thread the
		 * pointer through the snapshot path. The graph-level
		 * objective is mirrored on every follower for parser
		 * convenience -- the same number on every node in a
		 * given snapshot.
		 */
		pn.budget_clipped = reconcile_state_node_budget_clipped(
				drv->reconcile, n_iter->info.id);
		pn.risk_objective_value =
			reconcile_state_risk_objective(drv->reconcile);

		r = rt_diag_params_snapshot_add_node(snap, &pn);
		if (r < 0)
			return r;
	}
	return 0;
}

/* Walk every follower's pw_node_peer list and count peer-edges
 * whose inline-dispatch fast path is armed (src_system populated
 * to the consumer's system at peer_ref time) versus those that
 * remained on the eventfd_write path. The fast path is the
 * runtime realisation of a single-job macro-actor: when both
 * endpoints share a data loop, trigger_target_v1 calls
 * process_node on the producer's stack instead of crossing the
 * kernel via eventfd_write/epoll_wait. Counting both arms here
 * makes the post-fusion utilisation observable from the JSON
 * snapshot. */
static void populate_peer_dispatch(struct impl *impl SPA_UNUSED,
		struct node *drv, struct rt_diag_peer_dispatch *snap)
{
	struct pw_impl_node *dnode = drv->node;
	struct pw_impl_node *n_iter;
	struct pw_node_peer *peer;

	snap->driver_id = dnode->info.id;
	snap->generation = SPA_ATOMIC_LOAD(drv->topo.generation);

	spa_list_for_each(n_iter, &dnode->follower_list, follower_link) {
		spa_list_for_each(peer, &n_iter->peer_list, link) {
			if (peer->target.src_system != NULL &&
			    peer->target.src_system == peer->target.system)
				snap->inline_armed++;
			else
				snap->eventfd_path++;
		}
	}
}

/* Atomic JSON snapshot: render the diagnostic slices into a temp
 * file, then rename(2) over the final path so concurrent readers
 * see either the previous full document or the new one, never a
 * torn write. Emits nothing when debug.snapshot-json-path is
 * unset or empty. */
static void dump_combined_json_main(struct impl *impl, struct node *drv)
{
	struct rt_diag_raw_snapshot    raw;
	struct rt_diag_sched_snapshot  sched;
	struct rt_diag_fusion_snapshot fusion;
	struct rt_diag_params_snapshot params;
	struct rt_diag_peer_dispatch   peer_dispatch;
	struct rt_diag_combined c = { 0 };
	struct pw_impl_node *dnode = drv->node;
	char tmp_path[PATH_MAX];
	FILE *fp;
	int n;

	if (impl->debug_snapshot_json_path == NULL ||
	    impl->debug_snapshot_json_path[0] == '\0')
		return;

	rt_diag_raw_snapshot_init(&raw);
	rt_diag_sched_snapshot_init(&sched);
	rt_diag_fusion_snapshot_init(&fusion);
	rt_diag_params_snapshot_init(&params);
	rt_diag_peer_dispatch_init(&peer_dispatch);

	if (populate_raw_snapshot(impl, drv, &raw) < 0)
		goto cleanup;
	if (populate_sched_snapshot(impl, drv, &sched) < 0)
		goto cleanup;
	if (populate_fusion_snapshot(impl, drv, &fusion) < 0)
		goto cleanup;
	if (populate_params_snapshot(impl, drv, &params) < 0)
		goto cleanup;
	populate_peer_dispatch(impl, drv, &peer_dispatch);

	c.driver_id = dnode->info.id;
	c.generation = SPA_ATOMIC_LOAD(drv->topo.generation);
	c.period_ns = drv->topo.period;
	c.deadline_ns = drv->topo.period;
	{
		struct reconcile_feasibility feas;
		reconcile_state_feasibility(drv->reconcile, &feas);
		c.mode = (feas.mode == RECONCILE_MODE_HARD)
			? "hard" : "soft_degraded";
		if (feas.density_passed)
			c.feasibility_method = "density";
		else if (feas.dbf_passed)
			c.feasibility_method = "dbf";
		else
			c.feasibility_method = "none";
		c.feasibility_status = (feas.mode == RECONCILE_MODE_HARD)
			? "feasible" : "infeasible";
	}
	c.raw = &raw;
	c.sched = &sched;
	c.fusion = &fusion;
	c.params = &params;
	c.peer_dispatch = &peer_dispatch;

	n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp",
			impl->debug_snapshot_json_path);
	if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
		pw_log_warn("snapshot json path too long: %s",
				impl->debug_snapshot_json_path);
		goto cleanup;
	}
	fp = fopen(tmp_path, "w");
	if (fp == NULL) {
		pw_log_warn("cannot open %s for write: %m", tmp_path);
		goto cleanup;
	}
	rt_diag_render_json(&c, fp);
	if (fclose(fp) != 0) {
		pw_log_warn("error finishing %s: %m", tmp_path);
		(void)unlink(tmp_path);
		goto cleanup;
	}
	if (rename(tmp_path, impl->debug_snapshot_json_path) != 0) {
		pw_log_warn("cannot rename %s -> %s: %m",
				tmp_path, impl->debug_snapshot_json_path);
		(void)unlink(tmp_path);
	}

cleanup:
	rt_diag_raw_snapshot_fini(&raw);
	rt_diag_sched_snapshot_fini(&sched);
	rt_diag_fusion_snapshot_fini(&fusion);
	rt_diag_params_snapshot_fini(&params);
}

/* Main-loop context: walk the driver's follower list and the
 * follower ports/links to capture a self-contained topology snapshot
 * that the worker can consume without further main-loop access. */
/*
 * Snapshot invokes are queued asynchronously from the worker via
 * pw_loop_invoke(block=false). The data block is captured by value
 * and dereferenced when the main loop drains its queue, which can
 * be after the targeted driver has been torn down --
 * context_driver_removed() may have freed the struct node, or the
 * PipeWire core may have destroyed the underlying pw_impl_node as
 * part of the daemon shutdown sequence before module_destroy gets
 * a chance to run.
 *
 * Capturing the struct node pointer directly turns the second case
 * into a use-after-free (SIGSEGV in snapshot_topology_main at the
 * first dnode->... access). To stay safe across the entire
 * tear-down window, capture (impl, driver_id) instead and look up
 * the live struct node from impl->node_list on every invocation.
 *
 * `impl` itself is kept alive across all pending invokes by the
 * synchronous drain barrier at the start of module_destroy
 * (pw_loop_invoke from the main-loop thread calls flush_all_queues
 * inline before running the supplied function, so issuing a no-op
 * synchronous invoke drains every previously-queued snapshot
 * invoke before impl is freed). The lookup pattern below covers
 * the same race for context_driver_removed() (driver tear-down
 * happens before module_destroy in the normal shutdown sequence).
 */
struct snapshot_arg {
	struct impl *impl;
	uint32_t     driver_id;
};

static int snapshot_topology_main(struct spa_loop *loop SPA_UNUSED,
				   bool async SPA_UNUSED, uint32_t seq SPA_UNUSED,
				   const void *data, size_t size SPA_UNUSED,
				   void *user_data SPA_UNUSED)
{
	const struct snapshot_arg *a = data;
	struct node *drv;
	struct pw_impl_node *dnode;
	struct topo_snap *t;

	drv = find_node_any_by_id(a->impl, a->driver_id);
	if (drv == NULL || drv->node == NULL)
		return 0;  /* driver gone since the invoke was queued */
	dnode = drv->node;
	t = &drv->topo;

	t->ok = false;
	t->n_nodes = 0;
	t->n_edges = 0;

	if (dnode->target_rate.denom == 0 || dnode->target_quantum == 0) {
		SPA_ATOMIC_STORE(t->pending, 0);
		return 0;
	}
	t->period = SPA_NSEC_PER_SEC * dnode->target_quantum / dnode->target_rate.denom;

	struct pw_impl_node *follower;
	spa_list_for_each(follower, &dnode->follower_list, follower_link) {
		if (follower == dnode)
			continue;
		if (!pw_properties_get_bool(follower->properties,
					    PW_KEY_NODE_LOOP_DYNAMIC, false))
			continue;
		pid_t tid = pw_properties_get_int32(follower->properties,
						    PW_KEY_NODE_LOOP_TID, -1);
		if (tid == -1)
			continue;

		if (t->n_nodes >= t->nodes_cap) {
			uint32_t newcap = t->nodes_cap ? t->nodes_cap * 2 : TOPO_INITIAL_NODES;
			struct topo_node *nn = realloc(t->nodes, newcap * sizeof(*nn));
			if (!nn)
				return -ENOMEM;
			t->nodes = nn;
			t->nodes_cap = newcap;
		}
		t->nodes[t->n_nodes].id = follower->info.id;
		t->nodes[t->n_nodes].tid = tid;
		t->n_nodes++;
	}

	/* Edges: for each follower, walk output ports -> links -> input
	 * node, skipping links that don't introduce an in-period
	 * precedence constraint.
	 *
	 * A PipeWire link is *feedback* when constructing it would close
	 * a cycle in the graph (pw_impl_node_can_reach hits on the dst);
	 * the link records PW_KEY_LINK_FEEDBACK in its properties and
	 * the data plane uses spa_io_async_buffers instead of
	 * spa_io_buffers so the consumer in cycle N reads the producer's
	 * data from cycle N-1. There is no within-period dependency
	 * between the two endpoints, so the scheduling DAG must omit the
	 * edge entirely.
	 *
	 * An *async* link is one whose endpoints opt in to the same
	 * one-cycle-delay semantics via pw_impl_node.async (set when
	 * both endpoints' ports declare PW_IMPL_PORT_FLAG_ASYNC). The
	 * delay rationale is identical, so we filter both kinds with
	 * the same one-liner. */
	spa_list_for_each(follower, &dnode->follower_list, follower_link) {
		if (follower == dnode)
			continue;
		struct pw_impl_port *p;
		struct pw_impl_link *l;
		spa_list_for_each(p, &follower->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				if (!l->input || !l->input->node)
					continue;
				if (l->feedback)
					continue;
				if (l->output->node && l->input->node &&
						(l->output->node->async ||
						 l->input->node->async))
					continue;
				if (t->n_edges >= t->edges_cap) {
					uint32_t newcap = t->edges_cap ? t->edges_cap * 2 : TOPO_INITIAL_EDGES;
					struct topo_edge *ne = realloc(t->edges, newcap * sizeof(*ne));
					if (!ne)
						return -ENOMEM;
					t->edges = ne;
					t->edges_cap = newcap;
				}
				t->edges[t->n_edges].src = follower->info.id;
				t->edges[t->n_edges].dst = l->input->node->info.id;
				t->n_edges++;
			}
		}
	}

	t->ok = true;

	/* Compute a fingerprint of the freshly-captured topology. Only
	 * bump the generation if the fingerprint differs from the
	 * previous one -- otherwise the worker sees a "topology change"
	 * every snapshot tick and re-runs dag_build_analysis even when
	 * nothing structural changed, defeating the persistent-DAG
	 * optimisation. The fingerprint covers period, follower
	 * id+tid tuples and edge src/dst pairs (the same shape
	 * reconcile_apply reads). */
	{
		uint64_t h = 0xcbf29ce484222325ULL;
		uint32_t i;
		h ^= t->period;
		h *= 0x100000001b3ULL;
		for (i = 0; i < t->n_nodes; i++) {
			h ^= t->nodes[i].id;
			h *= 0x100000001b3ULL;
			h ^= (uint64_t)t->nodes[i].tid;
			h *= 0x100000001b3ULL;
		}
		for (i = 0; i < t->n_edges; i++) {
			h ^= t->edges[i].src;
			h *= 0x100000001b3ULL;
			h ^= t->edges[i].dst;
			h *= 0x100000001b3ULL;
		}
		if (h != drv->topo_fingerprint) {
			drv->topo_fingerprint = h;
			/* Release-store the generation bump so the worker
			 * (which acquires t->generation before reading the
			 * nodes/edges arrays) observes the freshly-written
			 * topology atomically. */
			SPA_ATOMIC_STORE(t->generation, t->generation + 1);
			if (drv->impl != NULL && drv->impl->debug_dump_raw_graph)
				dump_raw_graph_main(drv->impl, drv);
			if (drv->impl != NULL && drv->impl->debug_dump_sched_graph)
				dump_sched_graph_main(drv->impl, drv);
			if (drv->impl != NULL && drv->impl->debug_dump_fusion)
				dump_fusion_main(drv->impl, drv);
		}
	}
	/* Combined JSON snapshot fires every snapshot tick, not just
	 * on topology fingerprint change. The per-node parameters
	 * (last_runtime / last_deadline / last_period / last_cpu)
	 * advance whenever the worker's apply_sched_groups runs, even
	 * when topology is stable -- a quantum change, a WCET drift,
	 * a fusion-group leader flip all update follower state
	 * without bumping the fingerprint. Gating the snapshot on
	 * fingerprint change left the JSON pinned to whichever
	 * per-node values were live at the last structural change,
	 * which trivially diverges from /proc/<tid>/sched within a
	 * cycle and makes the snapshot useless for cross-checks. */
	if (drv->impl != NULL && t->ok)
		dump_combined_json_main(drv->impl, drv);
	SPA_ATOMIC_STORE(t->pending, 0);
	return 0;
}

/* Worker-context: rebuild and reapply the DAG using the freshest
 * topology snapshot + current per-node sketch budgets.
 *
 * Soft-failure: a single follower that never produces a non-zero
 * runtime (e.g. an idle source: a synth client whose plugin failed
 * to start, a paused stream, a node currently disconnected from any
 * input) used to abort the whole driver's recalc, leaving every
 * other follower on the default SCHED_FIFO scheduling. Instead,
 * skip the broken node from the DAG and apply SCHED_DEADLINE to
 * everyone else. Drop any edge that referenced the skipped node;
 * the DAG library treats the resulting sub-graph as the workload
 * to schedule. */
/* Build a reconcile_topo_t snapshot from the driver's per-tick
 * topology view, then hand off to the reconcile layer. The
 * follower array carries each follower's current WCET (looked up
 * via the module-side id-index in O(log N)). */
static void worker_apply_dag(struct impl *impl, struct node *drv)
{
	struct topo_snap *t = &drv->topo;
	reconcile_follower_t *followers;
	reconcile_edge_t *edges;
	reconcile_topo_t rtopo = { 0 };
	uint32_t i;

	if (!t->ok || t->n_nodes == 0)
		return;

	if (drv->reconcile == NULL) {
		drv->reconcile = reconcile_init((uint32_t)impl->n_cpus,
				impl->cpu_utilization,
				impl->relative_capacity,
				impl->wcet_recalc_threshold,
				impl->recalc_persistent);
		if (drv->reconcile == NULL) {
			pw_log_warn("reconcile_init failed: %m");
			return;
		}
	}

	followers = calloc(t->n_nodes, sizeof(*followers));
	edges = t->n_edges ? calloc(t->n_edges, sizeof(*edges)) : NULL;
	if (!followers || (t->n_edges && !edges)) {
		free(followers);
		free(edges);
		return;
	}

	{
		uint64_t this_gen = SPA_ATOMIC_LOAD(t->generation);
		for (i = 0; i < t->n_nodes; i++) {
			struct node *n = find_node_by_id(impl,
					t->nodes[i].id);

			followers[i].id = t->nodes[i].id;
			followers[i].tid = t->nodes[i].tid;
			followers[i].wcet = n ? n->wcet : 0;

			if (n == NULL)
				continue;
			if (n->last_topo_generation_seen &&
			    n->last_topo_generation != this_gen) {
				if (n->conformal != NULL)
					rt_conformal_invalidate(n->conformal,
						RT_CONF_INVALIDATED_TOPOLOGY_GENERATION);
			}
			n->last_topo_generation = this_gen;
			n->last_topo_generation_seen = true;
		}
	}
	for (i = 0; i < t->n_edges; i++) {
		edges[i].src = t->edges[i].src;
		edges[i].dst = t->edges[i].dst;
	}

	rtopo.followers = followers;
	rtopo.n_followers = t->n_nodes;
	rtopo.edges = edges;
	rtopo.n_edges = t->n_edges;
	rtopo.period = t->period;
	rtopo.generation = SPA_ATOMIC_LOAD(t->generation);

	sched_groups_reset(&impl->sched_groups);
	(void)reconcile_apply(drv->reconcile, &rtopo, sched_cb, impl);
	apply_sched_groups(impl, drv);
	sample_voluntary_ctxt_switches_main(impl, drv);

	free(followers);
	free(edges);
}

/* Worker-context: full recalc cycle for one driver. Drains samples,
 * applies the DAG using the most recent topology snapshot, then
 * kicks off a *non-blocking* refresh on the main loop for the next
 * cycle. Non-blocking is essential: module_destroy runs on the main
 * loop and would otherwise deadlock against a worker still parked in
 * a synchronous main-loop invoke. The first wake uses topo.ok=false
 * (skips apply); subsequent wakes use the topology refreshed by the
 * previous wake's async invoke. Graph mutations are rare so trailing
 * by one cycle is acceptable. */
static void worker_recalc_one(struct impl *impl, struct node *drv)
{
	uint64_t t0;

	if (impl->worker_fake_delay_us > 0) {
		struct timespec ts = {
			.tv_sec  = impl->worker_fake_delay_us / 1000000u,
			.tv_nsec = (impl->worker_fake_delay_us % 1000000u) * 1000u,
		};
		nanosleep(&ts, NULL);
	}

	/* Per-wake CPU-time measurement, log2-bucketed alongside the
	 * RT-hook histogram. fake-delay-us is excluded (nanosleep above
	 * does not advance CLOCK_THREAD_CPUTIME_ID). */
	t0 = now_thread_cputime_ns();

	worker_drain_samples(impl, drv);

	worker_apply_dag(impl, drv);

	{
		uint64_t t1 = now_thread_cputime_ns();
		if (t1 > t0)
			worker_hist_record(drv, t1 - t0);
	}

	/* Coalesce snapshot invokes: only queue a new one if no previous
	 * one is still pending. The main-loop callback clears the flag
	 * after writing. */
	if (SPA_ATOMIC_CAS(drv->topo.pending, 0, 1)) {
		struct snapshot_arg a = {
			.impl = impl,
			.driver_id = drv->node_id,
		};
		pw_loop_invoke(impl->main_loop, snapshot_topology_main, 0,
			       &a, sizeof(a), false, NULL);
	}
}

/* Worker-thread self-setup: SCHED_FIFO + CPU affinity. The poll
 * timer is registered separately from the main thread (before the
 * worker starts running its loop) -- registering it from inside a
 * worker invoke proved unreliable in earlier iterations of this
 * file. Sched failures are warnings; the worker still runs, just
 * at SCHED_OTHER. */
struct worker_setup_arg {
	cpu_set_t aff;
	int       rt_prio;
};

static int worker_setup(struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
			uint32_t seq SPA_UNUSED, const void *data, size_t size SPA_UNUSED,
			void *user_data SPA_UNUSED)
{
	const struct worker_setup_arg *p = data;
	struct sched_param sp = { .sched_priority = p->rt_prio };
	int rc;

	if (sched_setaffinity(0, sizeof(p->aff), &p->aff) != 0)
		pw_log_warn("deadline-recalc: sched_setaffinity failed: %m");

	rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	if (rc != 0)
		pw_log_warn("deadline-recalc: pthread_setschedparam(FIFO,%d) failed: %s",
			    p->rt_prio, strerror(rc));

	int actual_policy = -1;
	struct sched_param actual_sp = {0};
	pthread_getschedparam(pthread_self(), &actual_policy, &actual_sp);
	pid_t tid = (pid_t)syscall(SYS_gettid);

	pw_log_info("deadline-recalc worker tid=%d: SCHED_FIFO prio %d (actual policy=%d, "
		    "FIFO=%d), affinity=%d cpus",
		    tid, p->rt_prio, actual_policy, SCHED_FIFO,
		    CPU_COUNT(&p->aff));
	return 0;
}

/* Worker-thread context: drain every driver whose recalc_inflight
 * flag was raised on the RT side since the previous wakeup. The
 * worker is woken by pw_loop_signal_event on the impl->worker_wake
 * eventfd source -- the RT hook calls signal_event once on every
 * 0->1 CAS of recalc_inflight, so a single wakeup covers all of the
 * RT cycles that piled up while the worker was busy. */
static void worker_wake_func(void *data, uint64_t count SPA_UNUSED)
{
	struct impl *impl = data;
	struct node *drv;

	spa_list_for_each(drv, &impl->node_list, link) {
		if (!drv->is_driver)
			continue;
		uint32_t inflight = SPA_ATOMIC_XCHG(drv->recalc_inflight, 0);
		if (inflight == 0)
			continue;
		worker_recalc_one(impl, drv);
	}
}

/* RT-context dispatcher: measures CPU time and routes to sync or
 * async body. Kept short so async-mode hook never grows beyond a
 * ring write + an eventfd signal. */
static void rt_hook(void *data)
{
	struct node *drv = data;
	struct pw_impl_node *node = drv->node;
	struct impl *impl = drv->impl;

	if (node->target_rate.denom == 0 || node->target_quantum == 0)
		return;

	uint64_t t0 = now_thread_cputime_ns();

	if (impl->sync_mode) {
		recalc_params_sync(drv);
	} else {
		uint64_t period = SPA_NSEC_PER_SEC * node->target_quantum
				  / node->target_rate.denom;
		rt_push_samples(drv, period);
		/* CAS 0->1 to coalesce many RT cycles into one worker
		 * pass; only signal the eventfd on the 0->1 transition.
		 * If the worker is already scheduled (CAS fails because
		 * inflight=1), the existing wakeup will pick up our
		 * just-written sample on its next pass. The worker
		 * XCHG-resets the flag at the start of its callback, so
		 * the very next RT cycle after the worker starts
		 * re-arms the wakeup. */
		if (SPA_ATOMIC_CAS(drv->recalc_inflight, 0, 1))
			pw_loop_signal_event(impl->worker_loop, impl->worker_wake);
	}

	uint64_t t1 = now_thread_cputime_ns();
	if (t1 > t0)
		hist_record(drv, t1 - t0);
}

static const struct pw_impl_node_rt_events node_rt_events = {
	PW_VERSION_IMPL_NODE_RT_EVENTS,
	.complete = rt_hook,
	.incomplete = rt_hook,
};

static void set_driver_hook_state(struct node *n, bool enabled)
{
	if (enabled && !n->enabled) {
		SPA_FLAG_SET(n->node->rt.target.activation->flags, PW_NODE_ACTIVATION_FLAG_PROFILER);
		pw_impl_node_add_rt_listener(n->node, &n->node_rt_listener, &node_rt_events, n);
	} else if (!enabled && n->enabled) {
		SPA_FLAG_CLEAR(n->node->rt.target.activation->flags, PW_NODE_ACTIVATION_FLAG_PROFILER);
		pw_impl_node_remove_rt_listener(n->node, &n->node_rt_listener);
	}
	n->enabled = enabled;
}

static void context_driver_added(void *data, struct pw_impl_node *node)
{
	struct impl *impl = data;
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		return;

	n->impl = impl;
	n->node = node;
	n->node_id = node->info.id;
	n->is_driver = true;

	/* Allocate the per-driver SPSC sample ring. Worker reads, RT
	 * thread writes. Drop-oldest on overflow. */
	n->ring_capacity = WORKER_RING_CAPACITY;
	n->ring_slots = calloc(n->ring_capacity, sizeof(struct sample));
	if (n->ring_slots == NULL) {
		pw_log_warn("driver %d: failed to allocate sample ring; module disabled for this driver",
			    node->info.id);
		free(n);
		return;
	}
	spa_ringbuffer_init(&n->ring);

	spa_list_append(&impl->node_list, &n->link);
	if (node_register(impl, n) < 0) {
		pw_log_warn("driver %d: failed to register in id index; module disabled for this driver",
			    node->info.id);
		spa_list_remove(&n->link);
		free(n->ring_slots);
		free(n);
		return;
	}
	set_driver_hook_state(n, true);

	/* Async mode: prime the first topology snapshot so the very
	 * first worker wake has a populated topo to work with. The
	 * driver may not have a period yet (target_rate/quantum=0 until
	 * negotiated), in which case snapshot_topology_main sets
	 * topo.ok=false and the worker will retry next wake. */
	if (!impl->sync_mode && impl->main_loop != NULL) {
		struct snapshot_arg a = {
			.impl = impl,
			.driver_id = n->node_id,
		};
		SPA_ATOMIC_STORE(n->topo.pending, 1);
		snapshot_topology_main(NULL, false, 0, &a, sizeof(a), NULL);
	}
}

static void context_driver_removed(void *data, struct pw_impl_node *node)
{
	struct impl *impl = data;
	struct node *n;

	n = find_node(impl, node);
	if (n == NULL)
		return;

	set_driver_hook_state(n, false);

	/* After remove_rt_listener returns, no more RT callbacks for
	 * this driver are in flight. Tear down its async state. */
	hist_dump("driver-removed", n);
	if (n->reconcile) {
		reconcile_fini(n->reconcile);
		n->reconcile = NULL;
	}
	free(n->ring_slots);
	free(n->topo.nodes);
	free(n->topo.edges);
	node_unregister(impl, n);
	spa_list_remove(&n->link);
	free(n);
}

static const struct pw_context_events context_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.driver_added = context_driver_added,
	.driver_removed = context_driver_removed,
};

/* Fill impl->cpus[] with every CPU reachable through the pipewire
 * process' current affinity mask. This is the fallback when
 * cpus.available is unset or empty: it gives the operator "use every
 * CPU the kernel lets us touch" without forcing them to enumerate the
 * topology by hand. Returns the number of CPUs written, or 0 if the
 * affinity query itself fails (the caller will then refuse to load). */
static int default_cpus_from_affinity(struct impl *impl)
{
	cpu_set_t set;
	int i, n = 0;

	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof(set), &set) < 0) {
		pw_log_warn("sched_getaffinity failed: %m");
		return 0;
	}
	for (i = 0; i < CPU_SETSIZE && n < MAX_CPUS; i++) {
		if (CPU_ISSET(i, &set))
			impl->cpus[n++] = i;
	}
	return n;
}

static void parse_cpus(struct impl *impl, const char *cpus_str)
{
	struct spa_json it[3];
	int i = 0, v;

	if (cpus_str == NULL || cpus_str[0] == '\0') {
		impl->n_cpus = default_cpus_from_affinity(impl);
		return;
	}

	spa_json_init(&it[0], cpus_str, strlen(cpus_str));
	if (spa_json_enter_array(&it[0], &it[1]) <= 0)
		spa_json_init(&it[1], cpus_str, strlen(cpus_str));

	while (spa_json_get_int(&it[1], &v) > 0) {
		if (i >= MAX_CPUS)
			break;
		if (v >= 0) {
			impl->cpus[i] = v;
			i++;
		}
	}
	if (i == 0)
		i = default_cpus_from_affinity(impl);
	impl->n_cpus = i;
}

static enum cpu_smt_policy parse_smt_policy(const char *s)
{
	if (s == NULL)
		return CPU_SMT_STRICT;
	if (strcmp(s, "strict") == 0)
		return CPU_SMT_STRICT;
	if (strcmp(s, "dedupe") == 0)
		return CPU_SMT_DEDUPE;
	if (strcmp(s, "ignore") == 0)
		return CPU_SMT_IGNORE;
	pw_log_warn("cpus.smt-policy '%s' not recognised; using 'strict'", s);
	return CPU_SMT_STRICT;
}

static enum cpu_dvfs_policy parse_dvfs_policy(const char *s)
{
	if (s == NULL)
		return CPU_DVFS_CONSERVATIVE;
	if (strcmp(s, "conservative") == 0)
		return CPU_DVFS_CONSERVATIVE;
	if (strcmp(s, "assume-max") == 0)
		return CPU_DVFS_ASSUME_MAX;
	pw_log_warn("cpus.dvfs-policy '%s' not recognised; using 'conservative'", s);
	return CPU_DVFS_CONSERVATIVE;
}

/* Probe (or JSON-override) the per-CPU topology, apply the SMT policy
 * (strict refusal aborts module init), and shrink impl->cpus[] /
 * impl->n_cpus to match if DEDUPE dropped siblings. On success
 * impl->relative_capacity is populated, aligned with impl->cpus[],
 * and the per-CPU diagnostic block has been logged. Returns 0 on
 * success, -1 on refusal or sysfs failure. */
static int build_cpu_topology(struct impl *impl, struct pw_properties *props)
{
	uint32_t cpus[MAX_CPUS];
	uint32_t i;
	const char *override;
	int rc;

	if (impl->n_cpus <= 0) {
		pw_log_warn("cpus.available is empty; "
				"deadline scheduling will not be applied");
		return -1;
	}

	impl->smt_policy  = parse_smt_policy(
			pw_properties_get(props, "cpus.smt-policy"));
	impl->dvfs_policy = parse_dvfs_policy(
			pw_properties_get(props, "cpus.dvfs-policy"));

	for (i = 0; i < (uint32_t)impl->n_cpus; i++)
		cpus[i] = (uint32_t)impl->cpus[i];

	override = pw_properties_get(props, "cpus.topology-override");
	if (override != NULL && override[0] != '\0') {
		rc = cpu_topology_from_json(override, impl->dvfs_policy,
				&impl->topology);
	} else {
		rc = cpu_topology_probe(cpus, (uint32_t)impl->n_cpus,
				impl->dvfs_policy, &impl->topology);
	}
	if (rc < 0) {
		pw_log_error("cpu-topology: probe/override failed: %m");
		return -1;
	}

	uint32_t off_a = 0, off_b = 0;
	if (cpu_topology_apply_smt_policy(&impl->topology, impl->smt_policy,
				&off_a, &off_b) < 0) {
		/* STRICT refusal: name the offending pair and the physical
		 * core they share so the operator can edit cpus.available
		 * deliberately. */
		uint32_t core = 0;
		for (i = 0; i < impl->topology.num_cpus; i++) {
			if (impl->topology.cpus[i].cpu_id == off_a) {
				core = impl->topology.cpus[i].core_id;
				break;
			}
		}
		pw_log_error("cpus.smt-policy=strict refuses cpus.available: "
				"cpu%u and cpu%u are SMT siblings on physical core %u",
				off_a, off_b, core);
		cpu_topology_destroy(&impl->topology);
		return -1;
	}

	/* Re-derive impl->cpus[] from the (possibly shrunken) topology
	 * so DEDUPE actually removes siblings from the deadline-CPU set
	 * the rest of the module sees. The topology preserves the order
	 * in which CPUs were originally listed in cpus.available, so the
	 * resulting impl->cpus[] is "the original list minus the dropped
	 * siblings", which is what the operator would have written had
	 * they known about the duplicate. */
	for (i = 0; i < impl->topology.num_cpus; i++)
		impl->cpus[i] = (int)impl->topology.cpus[i].cpu_id;
	impl->n_cpus = (int)impl->topology.num_cpus;

	free(impl->relative_capacity);
	free(impl->relative_capacity_nominal);
	impl->relative_capacity = calloc(impl->topology.num_cpus,
			sizeof(*impl->relative_capacity));
	impl->relative_capacity_nominal = calloc(impl->topology.num_cpus,
			sizeof(*impl->relative_capacity_nominal));
	if (!impl->relative_capacity || !impl->relative_capacity_nominal) {
		free(impl->relative_capacity);
		free(impl->relative_capacity_nominal);
		impl->relative_capacity = NULL;
		impl->relative_capacity_nominal = NULL;
		cpu_topology_destroy(&impl->topology);
		return -1;
	}
	for (i = 0; i < impl->topology.num_cpus; i++) {
		impl->relative_capacity[i] =
			impl->topology.cpus[i].relative_capacity;
		impl->relative_capacity_nominal[i] =
			impl->topology.cpus[i].relative_capacity_nominal;
	}

	pw_log_info("cpu-topology: smt-policy=%s dvfs-policy=%s num_cpus=%u",
			impl->smt_policy == CPU_SMT_STRICT ? "strict" :
			impl->smt_policy == CPU_SMT_DEDUPE ? "dedupe" : "ignore",
			impl->dvfs_policy == CPU_DVFS_CONSERVATIVE ?
				"conservative" : "assume-max",
			impl->topology.num_cpus);
	for (i = 0; i < impl->topology.num_cpus; i++) {
		const struct cpu_info *ci = &impl->topology.cpus[i];
		pw_log_info("cpu-topology: cpu%u core=%u island=%u "
				"raw_cap=%" PRIu64 " min_freq_khz=%" PRIu64
				" max_freq_khz=%" PRIu64
				" relative_capacity=%.3f nominal=%.3f",
				ci->cpu_id, ci->core_id, ci->island_id,
				ci->raw_capacity, ci->min_freq_khz,
				ci->max_freq_khz, ci->relative_capacity,
				ci->relative_capacity_nominal);
	}
	return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_properties *props;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	spa_list_init(&impl->node_list);

	pw_log_debug("module %p: new", impl);

	props = args ? pw_properties_new_string(args) : pw_properties_new(NULL, NULL);
	if (!props) {
		res = -errno;
		goto error;
	}
	impl->context = context;
	impl->props = props;

	parse_cpus(impl, pw_properties_get(props, "cpus.available"));

	impl->cpu_utilization = 0.95f;
	const char *cpu_utilization_str = pw_properties_get(props, "cpus.utilization");
	if (cpu_utilization_str != NULL && cpu_utilization_str[0] != '\0')
		spa_json_parse_float(cpu_utilization_str,
				strlen(cpu_utilization_str),
				&impl->cpu_utilization);

	if (build_cpu_topology(impl, props) < 0) {
		/* Strict refusal or sysfs failure: disable deadline policy
		 * but keep the module loaded so PipeWire can still run. */
		pw_log_warn("deadline scheduling disabled (cpu topology probe failed)");
		goto done;
	}

	impl->sched_reclaim = pw_properties_get_bool(props,
			"sched.reclaim", true);
	/* Per Linux sched-deadline.rst, SCHED_FLAG_RECLAIM (GRUB)
	 * is a runtime efficiency feature that redistributes
	 * unused per-task bandwidth to peers; it does not relax
	 * the kernel's admission test or the per-task budget.
	 * The in-process feasibility predicates (Baruah 1990
	 * density + DBF) make NO assumption about reclaim, so a
	 * schedule that passes them passes equally with
	 * sched.reclaim=false. Reclaim is therefore an
	 * optimisation, not part of the guarantee. */
	pw_log_info("sched.reclaim = %s (optimisation; the kernel"
			" admission test and the in-process feasibility"
			" check both ignore the GRUB reclaim flag)",
			impl->sched_reclaim ? "true" : "false");

	const char *s;

	/*
	 * Adaptive-conformal estimator configuration. Defaults match
	 * the rt_conformal_config_defaults starting points; the
	 * deadline.conformal.* keys override each knob and the parsed
	 * block is run through rt_conformal_config_validate so an
	 * unsound combination is refused at parse time rather than
	 * arming a broken estimator later.
	 */
	rt_conformal_config_defaults(&impl->conformal_cfg);
	if ((s = pw_properties_get(props, "deadline.conformal.alpha_graph")) != NULL) {
		char *end; double v = strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->conformal_cfg.alpha_target = v;
		else
			pw_log_warn("deadline.conformal.alpha_graph %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.alpha_min")) != NULL) {
		char *end; double v = strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->conformal_cfg.alpha_min = v;
		else
			pw_log_warn("deadline.conformal.alpha_min %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.alpha_max")) != NULL) {
		char *end; double v = strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->conformal_cfg.alpha_max = v;
		else
			pw_log_warn("deadline.conformal.alpha_max %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.eta")) != NULL) {
		char *end; double v = strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->conformal_cfg.eta = v;
		else
			pw_log_warn("deadline.conformal.eta %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.window")) != NULL) {
		char *end; unsigned long v = strtoul(s, &end, 10);
		if (end != s && v >= 2 && v <= RT_CONFORMAL_MAX_WINDOW)
			impl->conformal_cfg.window = (uint32_t)v;
		else
			pw_log_warn("deadline.conformal.window %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.recalc_period")) != NULL) {
		char *end; unsigned long v = strtoul(s, &end, 10);
		if (end != s && v >= 1)
			impl->conformal_cfg.recalc_period = (uint32_t)v;
		else
			pw_log_warn("deadline.conformal.recalc_period %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.ewma_location_lambda")) != NULL) {
		char *end; double v = strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->conformal_cfg.ewma_location_lambda = v;
		else
			pw_log_warn("deadline.conformal.ewma_location_lambda %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.ewma_scale_lambda")) != NULL) {
		char *end; double v = strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->conformal_cfg.ewma_scale_lambda = v;
		else
			pw_log_warn("deadline.conformal.ewma_scale_lambda %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.guard_ns")) != NULL) {
		char *end; unsigned long long v = strtoull(s, &end, 10);
		if (end != s)
			impl->conformal_cfg.guard_ns = (uint64_t)v;
		else
			pw_log_warn("deadline.conformal.guard_ns %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.guard_percent")) != NULL) {
		char *end; double v = strtod(s, &end);
		if (end != s && v >= 0.0 && v < 1.0)
			impl->conformal_cfg.guard_percent = v;
		else
			pw_log_warn("deadline.conformal.guard_percent %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.sigma_floor_ns")) != NULL) {
		char *end; unsigned long long v = strtoull(s, &end, 10);
		if (end != s && v >= 1)
			impl->conformal_cfg.sigma_floor_ns = (uint64_t)v;
		else
			pw_log_warn("deadline.conformal.sigma_floor_ns %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.runtime_floor_ns")) != NULL) {
		char *end; unsigned long long v = strtoull(s, &end, 10);
		if (end != s)
			impl->conformal_cfg.runtime_floor_ns = (uint64_t)v;
		else
			pw_log_warn("deadline.conformal.runtime_floor_ns %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.bootstrap_min_samples")) != NULL) {
		char *end; unsigned long v = strtoul(s, &end, 10);
		if (end != s && v >= 2)
			impl->conformal_cfg.bootstrap_min_samples = (uint32_t)v;
		else
			pw_log_warn("deadline.conformal.bootstrap_min_samples %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.bootstrap_runtime_ns")) != NULL) {
		char *end; unsigned long long v = strtoull(s, &end, 10);
		if (end != s)
			impl->conformal_cfg.bootstrap_runtime_ns = (uint64_t)v;
		else
			pw_log_warn("deadline.conformal.bootstrap_runtime_ns %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.max_update_cost_ns")) != NULL) {
		char *end; unsigned long long v = strtoull(s, &end, 10);
		if (end != s)
			impl->conformal_cfg.max_update_cost_ns = (uint64_t)v;
		else
			pw_log_warn("deadline.conformal.max_update_cost_ns %s ignored", s);
	}
	impl->conformal_cfg.compatible_history = pw_properties_get_bool(props,
			"deadline.conformal.compatible_history",
			impl->conformal_cfg.compatible_history);
	impl->conformal_cfg.trace_export = pw_properties_get_bool(props,
			"deadline.conformal.trace_export",
			impl->conformal_cfg.trace_export);
	if (impl->conformal_cfg.trace_export) {
		const char *tp = pw_properties_get(props,
				"deadline.conformal.trace_path");
		if (tp != NULL && tp[0] != '\0') {
			impl->conformal_trace_path = strdup(tp);
			if (impl->conformal_trace_path == NULL)
				pw_log_warn("strdup of conformal trace path failed: %m");
			else
				pw_log_info("deadline.conformal.trace_path = %s",
					impl->conformal_trace_path);
		} else {
			pw_log_warn("deadline.conformal.trace_export=true but"
					" deadline.conformal.trace_path is empty;"
					" trace export disabled");
			impl->conformal_cfg.trace_export = false;
		}
	}
	if ((s = pw_properties_get(props, "deadline.conformal.risk_allocation")) != NULL) {
		if (spa_streq(s, "uniform"))
			impl->conformal_cfg.risk_allocation = RT_CONF_RISK_ALLOC_UNIFORM;
		else if (spa_streq(s, "density_weighted"))
			impl->conformal_cfg.risk_allocation = RT_CONF_RISK_ALLOC_DENSITY_WEIGHTED;
		else if (spa_streq(s, "slope_weighted"))
			impl->conformal_cfg.risk_allocation = RT_CONF_RISK_ALLOC_SLOPE_WEIGHTED;
		else
			pw_log_warn("deadline.conformal.risk_allocation %s ignored", s);
	}
	if (rt_conformal_config_validate(&impl->conformal_cfg) != 0) {
		pw_log_warn("deadline.conformal.* parsed values do not pass"
				" validation; reverting to defaults");
		rt_conformal_config_defaults(&impl->conformal_cfg);
	}

	/*
	 * Runtime budget-source preference. Accepts the same token set
	 * the diag layer surfaces in the JSON snapshot plus the
	 * automatic-default sentinel. The mbpta and empirical_quantile
	 * tokens, plus every deadline.mbpta.* / wcet.* key, are
	 * handled by the legacy-key deprecation pass in the next
	 * commit; right now they log a warning and fall through to
	 * the automatic default. Strict hard-realtime operation needs
	 * manual / static / hybrid budgets (Bernat, Burns & Llamosi
	 * 2001 §III).
	 */
	impl->budget_source = BUDGET_SOURCE_AUTO;
	if ((s = pw_properties_get(props, "deadline.budget.source")) != NULL) {
		if (spa_streq(s, "manual"))
			impl->budget_source = BUDGET_SOURCE_MANUAL;
		else if (spa_streq(s, "deterministic"))
			impl->budget_source = BUDGET_SOURCE_DETERMINISTIC;
		else if (spa_streq(s, "adaptive_conformal"))
			impl->budget_source = BUDGET_SOURCE_ADAPTIVE_CONFORMAL;
		else if (spa_streq(s, "bootstrap"))
			impl->budget_source = BUDGET_SOURCE_BOOTSTRAP;
		else {
			pw_log_warn("deadline.budget.source %s ignored", s);
		}
	}

	impl->debug_dump_raw_graph = pw_properties_get_bool(props,
			"debug.dump-raw-graph", false);
	if (impl->debug_dump_raw_graph)
		pw_log_info("debug.dump-raw-graph = true (raw-graph diagnostic"
				" dumps will be logged on topology change)");
	impl->debug_dump_sched_graph = pw_properties_get_bool(props,
			"debug.dump-sched-graph", false);
	if (impl->debug_dump_sched_graph)
		pw_log_info("debug.dump-sched-graph = true (scheduling-DAG"
				" diagnostic dumps will be logged on topology change)");
	impl->debug_dump_fusion = pw_properties_get_bool(props,
			"debug.dump-fusion", false);
	if (impl->debug_dump_fusion)
		pw_log_info("debug.dump-fusion = true (fusion-decision"
				" diagnostic dumps will be logged on topology change)");
	{
		const char *jp = pw_properties_get(props, "debug.snapshot-json-path");
		if (jp != NULL && jp[0] != '\0') {
			impl->debug_snapshot_json_path = strdup(jp);
			if (impl->debug_snapshot_json_path == NULL)
				pw_log_warn("strdup of snapshot json path failed: %m");
			else
				pw_log_info("debug.snapshot-json-path = %s",
					impl->debug_snapshot_json_path);
		}
	}

	impl->recalc_persistent = pw_properties_get_bool(props, "recalc.persistent", true);
	impl->wcet_recalc_threshold = 0.01;
	if ((s = pw_properties_get(props, "wcet.recalc-threshold")) != NULL) {
		char *end;
		double v = strtod(s, &end);
		if (end != s && v >= 0.0 && v < 1.0)
			impl->wcet_recalc_threshold = v;
		else
			pw_log_warn("wcet.recalc-threshold %s ignored", s);
	}

	impl->sync_mode = pw_properties_get_bool(props, "recalc.sync", false);
	impl->worker_rt_prio = WORKER_DEFAULT_RT_PRIO;
	if ((s = pw_properties_get(props, "recalc.rt-prio")) != NULL) {
		char *end;
		long v = strtol(s, &end, 10);
		if (end != s && v >= 1 && v <= 99)
			impl->worker_rt_prio = (int)v;
		else
			pw_log_warn("recalc.rt-prio %s ignored", s);
	}
	impl->worker_fake_delay_us = 0;
	if ((s = pw_properties_get(props, "recalc.fake-delay-us")) != NULL) {
		char *end;
		unsigned long v = strtoul(s, &end, 10);
		if (end != s)
			impl->worker_fake_delay_us = (uint32_t)v;
		else
			pw_log_warn("recalc.fake-delay-us %s ignored", s);
	}

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);
	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));
	pw_impl_module_update_properties(module, &props->dict);

	const char *disable_deadline = getenv("PIPEWIRE_DISABLE_MODULE_DEADLINE");
	if (disable_deadline != NULL && disable_deadline[0] != '\0' && strcmp(disable_deadline, "0") != 0) {
		pw_log_info("deadline scheduling disabled by PIPEWIRE_DISABLE_MODULE_DEADLINE");
		goto done;
	}

	if (!can_use_deadline_policy()) {
		pw_log_warn("deadline scheduling disabled");
		goto done;
	}

	impl->main_loop = pw_context_get_main_loop(impl->context);

	/* Async worker setup. Computed in two pieces:
	 * - CPU affinity mask: daemon's current online set MINUS the
	 *   deadline cores (cpus.available). Worker must never share a
	 *   CPU with a thread it has just promoted to SCHED_DEADLINE.
	 * - SCHED_FIFO priority: strictly below PipeWire's audio
	 *   threads (default 88; min configurable 11) so the worker
	 *   never preempts audio. */
	if (!impl->sync_mode) {
		CPU_ZERO(&impl->worker_affinity);
		cpu_set_t base;
		CPU_ZERO(&base);
		if (sched_getaffinity(0, sizeof(base), &base) == 0) {
			for (int c = 0; c < CPU_SETSIZE; c++)
				if (CPU_ISSET(c, &base))
					CPU_SET(c, &impl->worker_affinity);
		} else {
			pw_log_warn("sched_getaffinity failed: %m; worker will run on all CPUs");
			for (int c = 0; c < CPU_SETSIZE; c++)
				CPU_SET(c, &impl->worker_affinity);
		}
		for (int i = 0; i < impl->n_cpus; i++) {
			if (impl->cpus[i] >= 0 && impl->cpus[i] < CPU_SETSIZE)
				CPU_CLR(impl->cpus[i], &impl->worker_affinity);
		}
		if (CPU_COUNT(&impl->worker_affinity) == 0) {
			pw_log_warn("no housekeeping CPUs left for worker; falling back to all");
			for (int c = 0; c < CPU_SETSIZE; c++)
				CPU_SET(c, &impl->worker_affinity);
		}

		impl->worker_tloop = pw_thread_loop_new("deadline-recalc", NULL);
		if (impl->worker_tloop == NULL) {
			pw_log_error("failed to create deadline-recalc thread loop: %m");
			goto worker_failed;
		}
		impl->worker_loop = pw_thread_loop_get_loop(impl->worker_tloop);

		/* Register the wake source BEFORE starting the worker
		 * thread. Sources added mid-iteration from within a
		 * worker_setup invoke proved unreliable on this host:
		 * the worker's epoll_wait would not observe the newly
		 * added fd. Adding from main, pre-start, guarantees the
		 * very first pw_loop_iterate already polls it. */
		impl->worker_wake = pw_loop_add_event(impl->worker_loop,
						      worker_wake_func, impl);
		if (impl->worker_wake == NULL) {
			pw_log_error("failed to add deadline-recalc wake source: %m");
			pw_thread_loop_destroy(impl->worker_tloop);
			impl->worker_tloop = NULL;
			goto worker_failed;
		}

		if (pw_thread_loop_start(impl->worker_tloop) < 0) {
			pw_log_error("failed to start deadline-recalc thread: %m");
			pw_loop_destroy_source(impl->worker_loop, impl->worker_wake);
			impl->worker_wake = NULL;
			pw_thread_loop_destroy(impl->worker_tloop);
			impl->worker_tloop = NULL;
			goto worker_failed;
		}

		/* Apply scheduling+affinity to the worker thread itself
		 * via a synchronous invoke. Failure is non-fatal: the
		 * worker still works, just at SCHED_OTHER. */
		struct worker_setup_arg setup = {
			.aff = impl->worker_affinity,
			.rt_prio = impl->worker_rt_prio,
		};
		(void)pw_loop_invoke(impl->worker_loop, worker_setup, 0,
				     &setup, sizeof(setup), true, NULL);
	}

worker_failed:
	pw_context_add_listener(impl->context, &impl->context_listener, &context_events, impl);

	goto done;

error:
	free(impl);
done:
	pw_properties_free(props);

	return res;
}
