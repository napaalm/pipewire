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

#include "module-deadline/budget_warnings.h"
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
#include <pipewire/cycle-counter.h>

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
 *                       If unset or empty, the default is `[ 0 1 2 3 ]`.
 *                       Override explicitly to reserve a different
 *                       subset of cores for audio.
 * - `cpus.utilization`: The maximum CPU utilization (per core) that DEADLINE
 *                       threads are allowed to consume. The default is 0.95.
 * - `cpus.smt-policy`:  How to handle SMT-paired logical CPUs within
 *                       `cpus.available`. `dedupe` (the default) keeps
 *                       the lowest-id sibling per core and drops the
 *                       others, so the kernel admission test never
 *                       overcommits a physical core. `strict` refuses
 *                       the module load if any two CPUs in the set
 *                       share a physical core. `ignore` accepts the
 *                       set unchanged and logs a warning; only useful
 *                       for diagnostic comparisons.
 * - `cpus.dvfs-policy`: Which cpufreq frequency to use when computing
 *                       per-CPU capacity. `conservative` (the default)
 *                       uses `scaling_min_freq`: any budget that fits
 *                       at analysis time is guaranteed to fit at
 *                       runtime regardless of governor behaviour, since
 *                       real throughput exceeds the min-freq assumption
 *                       whenever the governor parks the CPU higher.
 *                       `assume-max` uses `scaling_max_freq`; admission
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
 *                       manual -> deterministic -> adaptive_conformal;
 *                       restricting via this knob skips kinds above
 *                       the chosen layer. `bootstrap` is now an
 *                       "opt out of admission" knob: the follower
 *                       is never promoted to SCHED_DEADLINE and
 *                       always stays at whatever module-rt gave it
 *                       (SCHED_FIFO at the audio priority). Strict
 *                       hard-realtime operation requires `manual` or
 *                       `deterministic` (a configured static bound);
 *                       `adaptive_conformal` is a soft / weakly-hard
 *                       estimate, not a deterministic WCET (Bernat,
 *                       Burns & Llamosi 2001).
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
 * - `driver.schedule`:  If true (default false), include the driver
 *                       node's own data-loop thread in the scheduling
 *                       DAG: its WCET is sampled from its self-entry
 *                       in target_list, it becomes a node in the
 *                       analysis (sink for playback graphs, source
 *                       for capture), and it receives a
 *                       SCHED_DEADLINE tuple alongside its followers.
 *                       Default false: the driver keeps whatever
 *                       policy the daemon (or rtkit) already gave it.
 *                       Requires the driver to run on a dedicated
 *                       dynamic data-loop (`context.dynamic-data-loops
 *                       = true`); a driver still on the shared main
 *                       loop has no `PW_KEY_NODE_LOOP_TID` published
 *                       and is silently excluded, same fallback as
 *                       followers without a TID.
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
 *         # cpus.available defaults to [ 0 1 2 3 ]; uncomment to
 *         # override explicitly.
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

/* Per-follower warm-up window armed when the driver's topology
 * fingerprint changes (a follower joined / left, a link was
 * re-routed, the period changed). For the next N apply_sample
 * invocations on each follower the sample is dropped before it
 * reaches the conformal estimator, the mode-keyed table or the
 * peak-hold floor: the first cycles after a topology mutation can
 * carry plugin first-touch costs (scratch buffer allocations,
 * convolution kernel priming, JIT warm-up) that the follower will
 * never repeat in steady state, and admitting them inflates the
 * empirical quantile (a single such sample dominates the ring
 * until it ages out, ~5 s at the common 1.3 ms period) which then
 * starves the soft-redistribute peers with sub-microsecond
 * reservations. Eight cycles is a few graph periods at the small
 * quanta the live tests use and a fraction of the bootstrap window
 * so a freshly added follower still enters the DAG quickly. */
#define TOPO_CHANGE_WARMUP_SAMPLES	8u

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
	uint8_t  xrun;
	uint8_t  _pad[7];
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

	/* Per-node admission WCET, in reference-CPU units. 0 means
	 * "not admitted yet": the follower's conformal estimator has
	 * not cleared bootstrap, so the worker filters this node out
	 * of the DAG and apply_sched_groups will not issue a
	 * sched_setattr -- the thread runs under whatever module-rt
	 * gave it (SCHED_FIFO at the audio priority by default).
	 * A non-zero value is the conformal estimator's published
	 * upper-budget for the next cycle, in reference-CPU units;
	 * sched_cb denormalises it for the placement CPU before
	 * passing it to sched_setattr. apply_sample re-reads this
	 * field on every sample, so a state transition
	 * BOOTSTRAP -> VALID/SHIFT flips it positive and the next
	 * reconcile pass admits the follower. */
	uint64_t wcet;
	uint64_t period;

	/* Decremented in apply_sample; while > 0 the incoming sample is
	 * dropped before it reaches the conformal estimator or the
	 * mode-keyed table. Set by the main thread to
	 * TOPO_CHANGE_WARMUP_SAMPLES when the driver's topology
	 * fingerprint changes (a follower joined / left / a link was
	 * re-routed), so the first few cycles after the change cannot
	 * pollute the statistical model with set-up-time spikes the
	 * follower would not see in steady state. uint32_t access is
	 * naturally aligned and the producer / consumer threads only
	 * read or write atomically through SPA_ATOMIC_*; no other
	 * ordering is required because the counter is purely advisory --
	 * a transient mis-observation just lets one extra sample
	 * through. */
	uint32_t warmup_samples_remaining;

	uint32_t last_xrun_count;

	/* Adaptive-conformal upper-runtime-budget estimator
	 * (Romano, Patterson & Candes 2019; Gibbs & Candes 2021).
	 * Consumes the per-cycle CPU-time samples and publishes a
	 * soft / weakly-hard (Bernat, Burns & Llamosi 2001) one-sided
	 * budget. NULL until the first sample arrives (lazy init). */
	rt_conformal_t *conformal;

	/* Mode-keyed adaptive-conformal table. Partitions the same
	 * sample stream by `(sample_rate, quantum, core_class)` so each
	 * combination owns its own estimator: switching driver rate or
	 * quantum no longer resets the conformal state, and samples
	 * collected on LITTLE vs. BIG cores accumulate in separate
	 * windows that the class-aware budget query can read
	 * independently.
	 *
	 * Sized the same as the legacy `conformal` field's lifecycle:
	 * lazy-created on first apply_sample, destroyed alongside
	 * conformal, invalidated alongside conformal. apply_sample
	 * dual-writes -- the legacy single estimator continues to feed
	 * the existing read sites (runtime_select_for_node, diagnostic
	 * accessors) so the kernel-apply behaviour is bit-for-bit
	 * unchanged; the table accumulates per-class statistics that a
	 * later commit will consume through
	 * rt_conformal_table_budget_for_class.
	 *
	 * Default cap (RT_CONFORMAL_TABLE_MAX_MODES) is generous for a
	 * realistic audio workload (every (rate, quantum) the driver
	 * ever runs at, times two classes); on overflow the LRU entry
	 * is evicted. */
	rt_conformal_table_t *conformal_table;

	/* Per-follower budget kind chosen by runtime_select_for_node
	 * on the most recent sample. Surfaced in the JSON snapshot so
	 * an operator can audit which source drove this cycle's
	 * kernel runtime. */
	enum rt_diag_budget_kind budget_kind;

	/* True iff the last runtime_select_for_node call on this
	 * follower satisfied the budget query via the LITTLE-bootstrap
	 * fallback in rt_conformal_table_budget_for_class (the BIG
	 * entry was not yet ready). Reset to false on every selection
	 * that returns from the target-class entry. Used for
	 * rate-limited HDL-W011 emission and surfaced through the diag
	 * snapshot. */
	bool budget_used_bootstrap;

	/* Edge-triggered guards for the HDL-W warning catalogue so a
	 * sustained condition emits one line per transition instead of
	 * flooding the log on every sample. Each flag is set when the
	 * corresponding condition first holds and cleared when it
	 * clears. The follower's own id keys the rate-limit; topology
	 * churn that recreates the node resets the flags via the
	 * usual node_unregister path. */
	bool warned_no_class_stats;       /* HDL-W010 */
	bool warned_big_bootstrap_from_little; /* HDL-W011 */
	bool warned_predicted_below_cputime;   /* HDL-W020 */

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

	/* Heterogeneous-host iterative recalc.
	 *
	 * `heterogeneous` is the master gate: when false (the default),
	 * the reconcile layer calls dag_recalculate exactly once per
	 * dispatch and the system behaves bit-for-bit as before.
	 *
	 * When true, the reconcile layer calls dag_recalculate_heterogeneous
	 * with `heterogeneous_iterations` as the upper bound on rounds.
	 * The iteration short-circuits to the single-shot pass on a
	 * single-CPU host or when relative_capacity is uniform, so even
	 * a heterogeneous=true config on a uniform host pays no extra
	 * cost in steady state.
	 *
	 * `heterogeneous_iterations` must be in the closed range
	 * [1, DAG_HETEROGENEOUS_MAX_ITERATIONS]; out-of-range values
	 * are clamped at parse time with a log warning. The default
	 * of 2 is the smallest bound that lets a placement-stretched
	 * split actually take effect (round 0 produces the seed,
	 * round 1 refines it), and in practice converges on every
	 * heterogeneous topology we have measured. */
	bool                  heterogeneous;
	uint32_t              heterogeneous_iterations;

	/* Class assignment for a follower that has not yet been
	 * placed (n->last_applied == false) and for a sample whose
	 * placement CPU is unknown (SAMPLE_CPU_UNKNOWN). The value is
	 * RT_CONF_CORE_LITTLE on a topology that contains at least
	 * one LITTLE-class CPU (the usual case) and RT_CONF_CORE_BIG
	 * on an all-BIG topology. The fallback is silent: a topology
	 * with no LITTLE CPU is not a misconfiguration, it just means
	 * the warm-up phase accumulates directly into the BIG class
	 * entry and the LITTLE-to-BIG bootstrap fallback in
	 * rt_conformal_table_budget_for_class never has cause to fire.
	 * Migration from LITTLE to BIG is the only direction that
	 * benefits from a bootstrap, so its absence on a homogeneous-
	 * BIG host has no operational impact. */
	uint8_t               default_warmup_class;

	/* Operator policy when the recalc reports a workload that
	 * cannot be admitted under the strict feasibility gate. The
	 * default ("keep-previous") leaves the last successfully
	 * applied schedule in place and emits HDL-W030; "rt-fallback"
	 * drops every node back to module-rt; "apply-degraded" applies
	 * the soft fallback's clipped/rescaled schedule; "reject-new-graph"
	 * refuses to admit the new follower set without affecting
	 * already-admitted ones. The default is what the reconcile
	 * layer's soft fallback already does; the other values are
	 * placeholders for callers that want a stricter contract. */
	enum {
		DEADLINE_ON_INFEASIBLE_KEEP_PREVIOUS = 0,
		DEADLINE_ON_INFEASIBLE_RT_FALLBACK,
		DEADLINE_ON_INFEASIBLE_APPLY_DEGRADED,
		DEADLINE_ON_INFEASIBLE_REJECT_NEW_GRAPH,
	}                     on_infeasible;

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

	/* When true, the driver's own data-loop thread is part of the
	 * scheduling DAG: its self-entry in target_list is sampled, it
	 * is added to topo.nodes[], and it receives a SCHED_DEADLINE
	 * tuple alongside its followers. Default false: the driver
	 * keeps whatever policy the daemon (or rtkit) already gave it,
	 * and only followers transition to SCHED_DEADLINE. The flag is
	 * gating because the driver thread is the timing root of the
	 * graph -- a bad tuple here stalls every follower at once, so
	 * the safer default is to leave it alone until live
	 * verification confirms a workload is healthy under the
	 * extended policy. */
	bool                  driver_schedule;
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
		if (n->conformal_table != NULL) {
			rt_conformal_table_destroy(n->conformal_table);
			n->conformal_table = NULL;
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
		 * downstream investigation has the full data.
		 *
		 * ESRCH is special: the target thread no longer exists
		 * (a client like pw-play just exited and its follower
		 * has not yet been removed from the topology snapshot).
		 * This is graceful-removal latency, not a scheduling
		 * decision the kernel disagrees with, so don't escalate
		 * the log level and return a distinct code so the caller
		 * can avoid forcing the driver into SOFT_DEGRADED. */
		if (errno == ESRCH) {
			pw_log_debug("sched_setattr: tid %d gone (ESRCH);"
				" follower will be dropped on next snapshot",
				tid);
			return -ESRCH;
		}
		if (errno == EINVAL)
			pw_log_warn("HDL-W070-SCHEDULE-APPLY-FAILED: "
				"sched_setattr rejected DEADLINE tuple"
				" for tid %d (errno=EINVAL, r=%lu d=%lu p=%lu);"
				" hard guarantees from in-process predicates"
				" no longer apply for this period",
				tid, runtime, deadline, period);
		else
			pw_log_error("HDL-W070-SCHEDULE-APPLY-FAILED: "
				"sched_setattr failed for tid %d"
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
		/* ESRCH: thread already gone (client disconnected between
		 * the snapshot capture and this syscall). Treat the same
		 * way as set_deadline_sched: log at debug, return a
		 * distinct code so the caller does not escalate to
		 * SOFT_DEGRADED for a transient removal race. */
		if (errno == ESRCH) {
			pw_log_debug("sched_setaffinity: tid %d gone (ESRCH);"
				" follower will be dropped on next snapshot",
				tid);
			return -ESRCH;
		}
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
			if (mn->conformal_table != NULL)
				rt_conformal_table_invalidate_all(
						mn->conformal_table,
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

		/* Driver-aware lookup: a sched group whose leader_id resolves
		 * to the driver (post-94b69ad24 the driver is a regular node
		 * in the scheduling DAG) needs the driver entry as anchor so
		 * the last_* bookkeeping below mirrors what sched_setattr
		 * actually applied. find_node_by_id intentionally filters
		 * drivers (its callers want follower-only iteration); the
		 * dispatch loop wants both. Without this fix drv->last_applied
		 * stays false even after a successful SCHED_DEADLINE call, the
		 * idempotency short-circuit below never engages for the driver,
		 * and the HDL-W091-DRIVER-BUDGET-BELOW-MEAN diagnostic at
		 * `anchor->is_driver` is unreachable. */
		anchor = find_node_any_by_id(impl, g->leader_id);
		if (anchor != NULL && anchor->last_applied &&
				anchor->last_runtime == g->sum_runtime &&
				anchor->last_deadline == kernel_deadline &&
				anchor->last_period == kernel_deadline &&
				anchor->last_cpu == g->cpu) {
			impl->sched_calls_skipped++;
			continue;
		}

		/* Driver-budget sanity diagnostic. When the soft-redistribute
		 * heuristic clips a chain that contains the driver, the
		 * driver thread can come out the other side with a runtime
		 * reservation well below its empirical mean -- and the
		 * driver is the timing root of the graph, so any kernel-side
		 * throttle on its activation propagates into an ALSA buffer
		 * underrun (which the DAC then replays, producing the
		 * audible "looping" symptom the live test hit). The emission
		 * is rate-limited to one line per leader id per apply pass
		 * via the existing last_applied gate above, and only fires
		 * when the conformal estimator has a non-zero EWMA location
		 * to compare against (an un-admitted driver has no real
		 * conformal location yet and is unaffected here). The
		 * threshold (sum_runtime < mu) is the smallest signal that
		 * still surfaces in practice: any reservation below the
		 * average cycle cost lets the kernel throttle on more than
		 * half of the activations. */
		if (anchor != NULL && anchor->is_driver &&
				anchor->conformal != NULL) {
			double mu = rt_conformal_mu_ns(anchor->conformal);
			if (mu > 0.0 && (double)g->sum_runtime < mu) {
				pw_log_warn("HDL-W091-DRIVER-BUDGET-BELOW-MEAN: "
						"driver tid=%d (id=%u) about to "
						"receive runtime=%" PRIu64
						" ns below its empirical EWMA "
						"location %.0f ns "
						"(deadline=%" PRIu64
						" period=%" PRIu64
						"); ALSA buffer underruns are "
						"likely on the next activation",
						(int)g->tid, anchor->node_id,
						g->sum_runtime, mu,
						kernel_deadline, g->period);
			}
		}

		/* Pass deadline as the kernel period so the task is
		 * implicit-deadline (dl_deadline == dl_period) from the
		 * CBS perspective.  Constrained-deadline tasks (D < P)
		 * are systematically throttled by dl_check_constrained_dl
		 * on every wakeup where the absolute deadline expired
		 * before the CBS period boundary — which, for an
		 * event-driven audio node that sleeps for nearly one
		 * graph period while its CBS deadline expires after a
		 * few hundred microseconds, is most cycles (any cycle
		 * where ALSA-timer jitter puts the wakeup a few
		 * nanoseconds before dl_next_period).  The throttle arms
		 * an hrtimer that fires at dl_next_period; the resulting
		 * 200–800 us delay is the gap between the ALSA wakeup
		 * and the CBS period boundary.
		 *
		 * With period == deadline the kernel's dl_is_implicit()
		 * returns true, the constrained check is skipped, and
		 * the normal CBS wakeup rule applies: the task receives
		 * a fresh budget and an absolute deadline of now +
		 * dl_deadline on every wakeup.  The split deadline still
		 * governs EDF priority (shorter deadline → earlier
		 * absolute deadline → higher priority), so the
		 * processing-chain order the DAG analysis computed is
		 * preserved.  The actual graph period is enforced by the
		 * event-driven signaling topology, not by the kernel
		 * CBS.
		 *
		 * Linux ≥ 7.0 rejects sched_period below 100 µs
		 * (EINVAL); clamp both sides of the implicit pair to
		 * that floor.  For sub-100 µs nodes the EDF ordering
		 * is lost, but all such nodes finish well within 100 µs
		 * and the event-driven graph topology still sequences
		 * them correctly. */
		uint64_t kernel_period = kernel_deadline < 100000
				? 100000 : kernel_deadline;
		rc_sched = set_deadline_sched(g->tid, g->sum_runtime,
				kernel_period, kernel_period,
				impl->sched_reclaim);
		/* If the deadline syscall already reports the TID is gone
		 * (ESRCH), don't bother with the affinity syscall on the
		 * same dead TID -- it would return ESRCH too, just adding
		 * to the debug noise without any new information. */
		if (rc_sched == -ESRCH)
			rc_aff = -ESRCH;
		else
			rc_aff = set_cpu_affinity(g->tid, impl->cpus[g->cpu]);

		if (anchor == NULL)
			continue;

		if (rc_sched == 0 && rc_aff == 0) {
			anchor->last_runtime  = g->sum_runtime;
			anchor->last_deadline = kernel_deadline;
			anchor->last_period   = kernel_deadline;
			anchor->last_cpu      = g->cpu;
			anchor->last_applied  = true;
		} else if (rc_sched == -ESRCH || rc_aff == -ESRCH) {
			/* Graceful follower removal: the thread exited
			 * between snapshot capture and the apply pass.
			 * Drop the per-anchor cache so a TID-reuse case
			 * (different node hopping onto the same id) is
			 * re-evaluated, but do NOT trigger the
			 * SOFT_DEGRADED transition -- the kernel did not
			 * reject any schedule we computed; the schedulee
			 * simply ceased to exist. The next topology
			 * snapshot will drop the dead follower. */
			anchor->last_applied = false;
		} else {
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
 * assume-max or on a host where scaling_max_freq == scaling_min_freq.
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
 *      Always wins when set and admits the follower immediately.
 *   2. RT_DIAG_BUDGET_DETERMINISTIC_WCET -- a hard static bound
 *      supplied by the plugin (no plumbing yet; reserved for a
 *      future plugin attribute). Used when the plugin exports it
 *      and also admits immediately.
 *   3. RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL -- the adaptive-conformal
 *      estimator's published budget, ONLY when the estimator has
 *      cleared bootstrap (state == VALID or SHIFT). While the
 *      conformal is in INSUFFICIENT_DATA or BOOTSTRAP this branch
 *      returns value_ns = 0, signalling "not admitted yet": the
 *      caller leaves the follower at whatever module-rt gave it
 *      (SCHED_FIFO at the audio priority) and the worker filters
 *      the node out of the DAG analysis upstream. See the
 *      function-level comment for the rationale.
 *
 * The operator can constrain the hierarchy via deadline.budget.source:
 * BUDGET_SOURCE_BOOTSTRAP is an "opt out of admission" knob (the
 * follower stays on SCHED_FIFO regardless of estimator state);
 * the manual / deterministic / adaptive_conformal values skip the
 * kinds above the requested one. BUDGET_SOURCE_AUTO (the default)
 * walks the full hierarchy.
 *
 * Pure function: no PipeWire side effects, no global state mutation.
 * sample_ref and peak_hold are legacy parameters that are no longer
 * consulted (the peak-hold floor was removed when the admission
 * predicate landed); both are kept in the signature for call-site
 * stability and marked SPA_UNUSED below. period_ns is forwarded to
 * the conformal layer as the upper clamp on the published budget.
 */
struct runtime_select_result {
	enum rt_diag_budget_kind kind;
	uint64_t                 value_ns;
	uint64_t                 sample_count;
};

static inline uint8_t sample_cpu_to_mode_class(const struct impl *impl,
		uint32_t sample_cpu);

/* Derive the conformal mode-key class for a node's current placement.
 * Returns the core_class of the node's last-applied CPU when one is
 * known; falls back to the impl-wide default warm-up class before the
 * first placement so a freshly-created follower queries the bucket
 * that the topology can actually fill (LITTLE on hosts with any
 * LITTLE-class CPU, BIG on an all-BIG host). The cast is wire-safe
 * because rt_core_class and rt_core_class_compat share the LITTLE=0
 * / BIG=1 encoding. */
static inline uint8_t node_target_mode_class(const struct impl *impl,
		const struct node *n)
{
	if (impl == NULL || n == NULL)
		return (uint8_t)RT_CONF_CORE_LITTLE;
	if (!n->last_applied)
		return impl->default_warmup_class;
	return sample_cpu_to_mode_class(impl, n->last_cpu);
}

static struct runtime_select_result runtime_select_for_node(
		const struct impl *impl,
		struct node *n,
		uint64_t sample_ref SPA_UNUSED,
		uint64_t peak_hold SPA_UNUSED,
		uint64_t period_ns,
		uint32_t sample_rate_hz,
		uint32_t quantum_frames)
{
	/*
	 * Admission discipline. A follower is "admitted" once the
	 * adaptive-conformal estimator (or the mode-keyed table for
	 * the follower's target class) has cleared bootstrap and can
	 * publish a budget derived from real per-cycle statistics.
	 * Until then this function returns value_ns = 0, which the
	 * caller treats as "not admitted yet": the follower is left at
	 * whatever module-rt gave it (SCHED_FIFO at the audio priority)
	 * and is filtered out of the DAG analysis upstream of any
	 * sched_setattr emission. The conformal state machine
	 * (INSUFFICIENT_DATA / BOOTSTRAP / VALID / SHIFT / DISABLED)
	 * decides the gate via rt_conformal_state.
	 *
	 * The previous peak-hold floor (n->wcet = monotonic running
	 * max of historical samples during bootstrap) was the source of
	 * the cold-start HDL-W030 storms: one inflated first-touch
	 * sample (a Pulse client connect, a soundfont stream loading on
	 * cycle 1) pinned the follower's WCET high enough that the
	 * chain critical path exceeded the period for the remainder of
	 * the bootstrap window. Holding the follower at SCHED_FIFO
	 * until the estimator can speak with statistics removes both
	 * the pin and the DAG inflation it caused.
	 *
	 * sample_ref and peak_hold are retained in the signature for
	 * call-site stability; both are now unused.
	 */
	enum rt_budget_source pref = impl->budget_source;
	struct runtime_select_result r = {
		.kind = RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL,
		.value_ns = 0,
		.sample_count = 0,
	};

	/*
	 * Per-node manual override hook. No plugin property is wired
	 * to it yet; once a per-node deadline.manual_override.runtime_ns
	 * property exists this branch picks it up. Manual override
	 * always wins and admits the follower immediately.
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
	 * or equivalent. A deterministic bound, when supplied, admits
	 * the follower immediately.
	 */
	if (false /* placeholder until plugin attribute lands */) {
		r.kind = RT_DIAG_BUDGET_DETERMINISTIC_WCET;
		return r;
	}
	if (pref == BUDGET_SOURCE_DETERMINISTIC)
		return r;

	/*
	 * BUDGET_SOURCE_BOOTSTRAP is now an "opt out of admission"
	 * knob: the operator has explicitly asked to never promote the
	 * follower to SCHED_DEADLINE, so we return 0 without consulting
	 * any estimator. Useful for benchmarking against the
	 * pure-SCHED_FIFO baseline without rebuilding the daemon.
	 */
	if (pref == BUDGET_SOURCE_BOOTSTRAP)
		return r;

	/*
	 * Class-aware adaptive-conformal upper budget. When the
	 * mode-keyed table has cleared bootstrap for the follower's
	 * target class, its budget is the kernel-side runtime. The
	 * LITTLE -> BIG bootstrap fallback inside the library still
	 * lets a fresh BIG placement borrow the LITTLE window so a
	 * new node on a BIG core gets admitted as soon as either class
	 * window is warm; HDL-W011 in the caller surfaces that case.
	 *
	 * The period_ns argument is the upper clamp on the published
	 * budget: a runaway prediction (a single outlier dominating
	 * the empirical quantile, or a sustained-spike pile-up) cannot
	 * publish a WCET above the kernel-side period, which would
	 * otherwise make this single node "infeasible" before any
	 * chain analysis runs.
	 */
	if (n->conformal_table != NULL && sample_rate_hz != 0 &&
			quantum_frames != 0) {
		uint8_t target_class = node_target_mode_class(impl, n);
		bool used_bootstrap = false;
		uint64_t c = rt_conformal_table_budget_for_class(
				n->conformal_table,
				sample_rate_hz, quantum_frames,
				target_class, period_ns, &used_bootstrap);
		if (c > 0) {
			r.kind = RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL;
			r.value_ns = c;
			/* Borrow the legacy estimator's sample-count
			 * reading as a stand-in: the table holds many
			 * sub-estimators and exposing a per-class total
			 * here would require a new accessor; the legacy
			 * value still gives an order-of-magnitude
			 * "how warmed up is this follower?" signal. */
			if (n->conformal != NULL)
				r.sample_count = rt_conformal_samples_used(
						n->conformal);
			/* Stash the bootstrap flag on the node for the
			 * caller's diagnostic emission. */
			n->budget_used_bootstrap = used_bootstrap;
			return r;
		}
	}

	/*
	 * Legacy single-estimator adaptive-conformal budget. Only
	 * VALID / SHIFT states publish; INSUFFICIENT_DATA / BOOTSTRAP
	 * return 0 here so the caller leaves the follower on SCHED_FIFO
	 * (see the function-level admission rationale above). SHIFT is
	 * still publishable: the value remains a valid one-sided
	 * bound; the drift flag rides in the diagnostic surface.
	 */
	if (n->conformal != NULL) {
		enum rt_conformal_state cs = rt_conformal_state(n->conformal);
		if (cs == RT_CONF_VALID || cs == RT_CONF_SHIFT) {
			uint64_t c = rt_conformal_budget(n->conformal, period_ns);
			if (c > 0) {
				r.kind = RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL;
				r.value_ns = c;
				r.sample_count = rt_conformal_samples_used(n->conformal);
				return r;
			}
		}
	}

	/*
	 * Not yet admitted. r.value_ns is still 0; the follower stays
	 * on whatever module-rt gave it. apply_sched_groups will not
	 * issue sched_setattr for a node whose wcet is 0, and the
	 * worker filters wcet=0 nodes out of the DAG before reconcile
	 * sees them.
	 */
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
/* Derive the conformal mode-key components from a placement CPU.
 * Returns the core_class enum value (cast to uint8_t to match the
 * mode_key field type) when the topology has been built and the
 * cpu is in range; falls back to the impl-wide default warm-up
 * class for an unknown or out-of-range sample CPU so a pre-
 * topology sample routes deterministically into the bucket the
 * topology can actually fill. A pre-init impl has no default yet:
 * RT_CONF_CORE_LITTLE is the unconditional fallback there so the
 * mode key remains stable across the rest of module init. */
static inline uint8_t sample_cpu_to_mode_class(const struct impl *impl,
		uint32_t sample_cpu)
{
	if (impl == NULL || impl->topology.num_cpus == 0)
		return (uint8_t)RT_CONF_CORE_LITTLE;
	if (sample_cpu == SAMPLE_CPU_UNKNOWN ||
			sample_cpu >= impl->topology.num_cpus)
		return impl->default_warmup_class;
	enum rt_core_class c = impl->topology.cpus[sample_cpu].core_class;
	return (c == RT_CORE_BIG) ? (uint8_t)RT_CONF_CORE_BIG
				  : (uint8_t)RT_CONF_CORE_LITTLE;
}

static void apply_sample(struct impl *impl, struct node *n,
		uint64_t runtime, uint64_t cycles,
		uint32_t sample_cpu, uint64_t period,
		uint32_t sample_rate_hz, uint32_t quantum_frames,
		bool xrun)
{
	if (n->period != period) {
		if (n->conformal != NULL)
			rt_conformal_invalidate(n->conformal,
					RT_CONF_INVALIDATED_PERIOD);
		if (n->conformal_table != NULL)
			rt_conformal_table_invalidate_all(n->conformal_table,
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

	/* Lazy-init the mode-keyed table alongside the legacy single
	 * estimator. Per-class statistics are accumulated through the
	 * table; read sites continue to consult the legacy estimator
	 * for now, so the dual-write keeps the kernel-apply path
	 * bit-for-bit unchanged while exposing the per-class data
	 * surface a class-aware budget query can read. */
	if (n->conformal_table == NULL) {
		n->conformal_table = rt_conformal_table_create(
				&impl->conformal_cfg,
				RT_CONFORMAL_TABLE_MAX_MODES);
		if (n->conformal_table == NULL)
			pw_log_warn("node %d: conformal table init failed",
				n->node ? n->node->info.id : (uint32_t)-1);
	}

	/* Prefer cycles when available: they are frequency-invariant
	 * by construction and yield a precise reference-CPU WCET
	 * without the assume-max inflation. */
	double sample_ref = wcet_cycles_to_reference_ns(impl, cycles, sample_cpu);
	if (sample_ref <= 0.0)
		sample_ref = wcet_sample_to_reference(impl, runtime, sample_cpu);

	/* Outlier guard: a sustainable audio follower must complete its
	 * process() invocation within one period. A sample larger than a
	 * small multiple of the period is almost always an initialisation
	 * spike (a plugin allocating its first scratch buffer; Surge XT
	 * priming its wavetables on the first cycle; a soundfont stream
	 * loading; a JIT first-touch) or an unbounded kernel-side
	 * preemption that the CPU-time counter accumulated under an
	 * unrelated workload. If we let such a value into either the
	 * peak-hold floor or the conformal score ring it dominates the
	 * estimator until enough normal samples flush it out -- meanwhile
	 * the soft-redistribute heuristic, which proportions deadlines by
	 * WCET, hands the misbehaving follower almost the entire period
	 * and starves the well-behaved peers with sub-microsecond
	 * reservations.  Discard the sample and let the existing budget
	 * stand. The drop is logged at debug so the operator can correlate
	 * an observed transient with the actual measurement.
	 *
	 * Threshold: 2 * period_ns. A single-period overshoot is plausible
	 * on a preempted activation; sustained breaches accumulate over
	 * several recalc passes and surface through HDL-W030 instead. */
	if (period > 0 && sample_ref > 2.0 * (double)period) {
		pw_log_debug("node %u: discarding outlier sample %.0f ns "
				"(period=%" PRIu64 " ns, threshold=2x); "
				"runtime_ns=%" PRIu64 " cycles=%" PRIu64,
				n->node ? n->node->info.id : (uint32_t)-1,
				sample_ref, period, runtime, cycles);
		n->period = period;
		return;
	}

	/* Post-topology-change warm-up: drop the first
	 * TOPO_CHANGE_WARMUP_SAMPLES samples after a fingerprint bump so
	 * the one-off cost of running through a freshly-built graph
	 * (allocations, page-faults, JIT, prefetch warm-up) cannot enter
	 * the empirical quantile or the peak-hold floor. The counter is
	 * armed by the main thread on every fingerprint change in
	 * snapshot_topology_main; here on the worker we just consume one
	 * tick and return. n->period is still refreshed because the
	 * legacy invalidation path keys on n->period == period to decide
	 * whether a reset is needed. */
	{
		uint32_t left = SPA_ATOMIC_LOAD(n->warmup_samples_remaining);
		if (left > 0) {
			SPA_ATOMIC_STORE(n->warmup_samples_remaining, left - 1);
			pw_log_debug("node %u: warm-up window dropping sample "
					"(%.0f ns, %u tick(s) left)",
					n->node ? n->node->info.id : (uint32_t)-1,
					sample_ref, left - 1);
			n->period = period;
			return;
		}
	}

	/* Anomalous-sample diagnostic. The conformal estimator uses the
	 * standardised score (sample - mu) / (scale + sigma_floor) as the
	 * empirical-quantile rank input, and a single sample whose score
	 * sits dozens of standard deviations above the current EWMA
	 * location can dominate the (1 - alpha_eff)-quantile for the
	 * lifetime of the score ring (~5 s at the 1.3 ms graph period).
	 * The observation is still admitted (the operator-facing knobs
	 * for outlier rejection live in the conformal config; this is a
	 * diagnostic surface only), but emit one warn line per occurrence
	 * so a post-mortem can correlate a budget explosion with the
	 * actual sample that caused it.
	 *
	 * Combined gate: the score test alone over-fires on
	 * low-baseline followers (e.g. the ALSA driver thread with
	 * mu approx 2-3 microseconds, scale approx 0.5 microsecond) where a
	 * perfectly normal 10-20 microsecond cycle reads as 20-40 sigma
	 * above mean even though the absolute deviation is harmless.
	 * Require both score > 20 AND a non-trivial absolute deviation:
	 * the sample must be at least 5x the EWMA location OR at least
	 * 50 microseconds above it. Whole-period scores (the audio-loop
	 * pathology, hundreds of sigma and tens of milliseconds of
	 * deviation) still cross both gates. */
	if (n->conformal != NULL && sample_ref > 0.0) {
		double mu = rt_conformal_mu_ns(n->conformal);
		double scale = rt_conformal_scale_ns(n->conformal);
		if (mu > 0.0 && scale > 0.0) {
			double denom = scale + 1.0;
			double score = (sample_ref - mu) / denom;
			double deviation = sample_ref - mu;
			bool abs_significant = deviation > 50000.0;
			bool rel_significant = sample_ref > 5.0 * mu;
			if (score > 20.0 &&
			    (abs_significant || rel_significant)) {
				pw_log_warn("HDL-W090-SAMPLE-ANOMALOUS: "
						"node %u: sample %.0f ns is "
						"%.1f sigma above EWMA "
						"location (mu=%.0f scale=%.0f); "
						"kept in the conformal ring",
						n->node_id, sample_ref, score,
						mu, scale);
			}
		}
	}

	if (!xrun) {
		if (runtime > 0 && n->conformal != NULL && sample_ref > 0.0)
			(void)rt_conformal_observe(n->conformal,
					(uint64_t)sample_ref);

		if (runtime > 0 && n->conformal_table != NULL &&
		    sample_ref > 0.0) {
			struct rt_conformal_mode_key key = {
				.sample_rate_hz = sample_rate_hz,
				.quantum_frames = quantum_frames,
				.core_class     = sample_cpu_to_mode_class(
						impl, sample_cpu),
			};
			(void)rt_conformal_table_observe(n->conformal_table,
					&key, (uint64_t)sample_ref);
		}
	} else {
		pw_log_debug("node %u: dropping xrun-tainted sample "
				"(runtime=%" PRIu64 " ns) from conformal "
				"observation", n->node_id, runtime);
	}

	/* Refresh the per-node WCET from the admission predicate. While
	 * the conformal estimator has not cleared bootstrap, value_ns
	 * stays at 0 and the follower is left on whatever module-rt
	 * gave it (SCHED_FIFO at the audio priority). Once the
	 * estimator publishes a real budget the WCET flips positive
	 * and the upstream DAG / sched_setattr path admits the node to
	 * SCHED_DEADLINE on the next worker pass.
	 *
	 * The previous design fed the predicate a "peak-hold" floor
	 * (max of historical samples) so a fresh follower had a budget
	 * from cycle 1. That pinned the WCET to the worst cold-start
	 * sample for the entire bootstrap window and produced the
	 * HDL-W030 critical-path storms during startup; the new
	 * contract is "no budget until statistics are good". */
	{
		uint64_t prev_wcet = n->wcet;
		bool prev_used_bootstrap = n->budget_used_bootstrap;
		n->budget_used_bootstrap = false;
		struct runtime_select_result sel = runtime_select_for_node(
				impl, n, 0, 0, period,
				sample_rate_hz, quantum_frames);
		n->wcet = sel.value_ns;
		n->budget_kind = sel.kind;

		uint32_t fid = n->node_id;

		/* Admission transition diagnostics. The handoff from
		 * BOOTSTRAP (wcet = 0, follower on SCHED_FIFO) to
		 * VALID / SHIFT (wcet > 0, follower about to be admitted
		 * to SCHED_DEADLINE on the next worker pass) is the
		 * moment the kernel-side policy changes; surface it once
		 * per transition so the operator can correlate the
		 * scheduling change with the conformal state. A subsequent
		 * invalidation (period change, fusion-group reshape) that
		 * drops wcet back to 0 is reported as a demotion. */
		if (prev_wcet == 0 && n->wcet > 0) {
			pw_log_info("HDL-I010-NODE-ADMITTED: node %u: "
					"conformal cleared bootstrap "
					"(budget %" PRIu64 " ns, period "
					"%" PRIu64 " ns); follower will "
					"be promoted to SCHED_DEADLINE on "
					"the next reconcile pass",
					fid, n->wcet, period);
		} else if (prev_wcet > 0 && n->wcet == 0) {
			pw_log_info("HDL-I011-NODE-DEMOTED: node %u: "
					"conformal returned to bootstrap "
					"(was %" PRIu64 " ns); follower "
					"will be left on SCHED_FIFO until "
					"statistics warm up again",
					fid, prev_wcet);
		}

		uint64_t sample_ref_u64 = sample_ref > 0.0
			? (uint64_t)sample_ref : 0;
		struct budget_warning_inputs win = {
			.sel_kind         = sel.kind,
			.sel_value_ns     = sel.value_ns,
			.sel_sample_count = sel.sample_count,
			.sample_ref       = sample_ref_u64,
			.runtime          = runtime,
			.conformal_table_present = (n->conformal_table != NULL),
			.current_used_bootstrap  = n->budget_used_bootstrap,
			.prev_used_bootstrap     = prev_used_bootstrap,
			.prior_warned_no_class_stats =
					n->warned_no_class_stats,
			.prior_warned_big_bootstrap =
					n->warned_big_bootstrap_from_little,
			.prior_warned_predicted_below_cputime =
					n->warned_predicted_below_cputime,
		};
		struct budget_warning_events ev =
				compute_budget_warning_events(&win);

		if (ev.fire_w011) {
			pw_log_warn("HDL-W011-BIG-BOOTSTRAP-FROM-LITTLE: "
					"node %u: BIG core_class has no ready "
					"conformal statistics yet; reservation "
					"budget %lu ns derived from LITTLE "
					"window. Stops once BIG window warms up.",
					fid, sel.value_ns);
			n->warned_big_bootstrap_from_little = true;
		} else if (ev.clear_w011) {
			pw_log_info("node %u: BIG core_class statistics "
					"ready; bootstrap from LITTLE no longer "
					"in use (HDL-W011 cleared)", fid);
			n->warned_big_bootstrap_from_little = false;
		}

		if (ev.fire_w010) {
			pw_log_warn("HDL-W010-MISSING-CLASS-STATS: node %u: "
					"no per-class conformal statistics "
					"ready yet; follower remains on "
					"SCHED_FIFO (admission predicate "
					"reports value_ns=%lu) until the "
					"window fills.",
					fid, sel.value_ns);
			n->warned_no_class_stats = true;
		} else if (ev.clear_w010) {
			pw_log_info("node %u: per-class conformal statistics "
					"now ready (HDL-W010 cleared)", fid);
			n->warned_no_class_stats = false;
		}

		if (ev.fire_w020) {
			pw_log_warn("HDL-W020-PREDICTED-BELOW-LAST-CPUTIME: "
					"node %u: predicted budget %lu ns is "
					"below measured runtime %lu ns on at "
					"least one sample. The estimator's "
					"score ring publishes a one-sided "
					"upper bound; a sample above the bound "
					"can occur transiently while the ring "
					"catches up.",
					fid, sel.value_ns, runtime);
			n->warned_predicted_below_cputime = true;
		}
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
			char alpha_buf[32];
			int an;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			/* JSON requires '.' as the decimal separator
			 * (RFC 8259 §6). snprintf honours LC_NUMERIC,
			 * so a daemon launched under e.g. it_IT would
			 * otherwise emit "1,5e-05" and break every
			 * downstream parser. Format the alpha_eff
			 * through a private buffer and rewrite any
			 * comma into a dot before emission -- the same
			 * trick the JSON-snapshot path uses. */
			an = snprintf(alpha_buf, sizeof(alpha_buf), "%g",
					rt_conformal_alpha_eff(n->conformal));
			if (an < 0 || (size_t)an >= sizeof(alpha_buf)) {
				alpha_buf[0] = '0';
				alpha_buf[1] = '\0';
			} else {
				for (char *p = alpha_buf; *p != '\0'; p++)
					if (*p == ',')
						*p = '.';
			}
			fprintf(impl->conformal_trace_fp,
				"{\"timestamp_ns\":%llu,"
				"\"entity_id\":%u,"
				"\"period_ns\":%llu,"
				"\"runtime_ns\":%llu,"
				"\"budget_ns\":%llu,"
				"\"budget_kind\":\"%s\","
				"\"conformal_state\":\"%s\","
				"\"conformal_alpha_eff\":%s,"
				"\"conformal_samples_used\":%llu}\n",
				(unsigned long long)
					((uint64_t)ts.tv_sec * 1000000000ULL +
					 (uint64_t)ts.tv_nsec),
				n->node_id,
				(unsigned long long)period,
				(unsigned long long)(sample_ref > 0.0 ?
					(uint64_t)sample_ref : 0),
				(unsigned long long)n->wcet,
				rt_diag_budget_kind_name(n->budget_kind),
				rt_conformal_state_name(
					rt_conformal_state(n->conformal)),
				alpha_buf,
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
		if (impl->heterogeneous) {
			reconcile_state_set_heterogeneous_iterations(
					drv->reconcile,
					impl->heterogeneous_iterations);
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

		/* Driver self-entry in target_list. When driver.schedule
		 * is off, the driver's own runtime is not a scheduling
		 * input and the entry is skipped here (matching the
		 * async producer-side skip in rt_push_samples). When on,
		 * the entry is kept and the driver becomes a regular
		 * follower in the analysis: its runtime is sampled, a
		 * struct node already exists (created by
		 * context_driver_added), and its WCET feeds the DAG
		 * along with the followers'. */
		if (tnode == node && !impl->driver_schedule)
			continue;

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
				period, node->target_rate.denom,
				node->target_quantum, false);

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

	rtopo.timing_root_id = 0;
	if (impl->driver_schedule) {
		for (uint32_t si = 0; si < n_followers; si++) {
			if (followers[si].id == drv->node_id) {
				rtopo.timing_root_id = drv->node_id;
				break;
			}
		}
	}

	sched_groups_reset(&impl->sched_groups);
	(void)reconcile_apply(drv->reconcile, &rtopo, sched_cb, impl);
	apply_sched_groups(impl, drv);

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
static void rt_push_samples(struct node *drv, uint64_t period,
		bool driver_incomplete)
{
	struct pw_impl_node *node = drv->node;
	struct pw_node_target *t;
	uint32_t buf_size_bytes = drv->ring_capacity * sizeof(struct sample);

	spa_list_for_each(t, &node->rt.target_list, link) {
		struct pw_impl_node *tnode = t->node;
		uint64_t runtime;

		/* pw_impl_node_register hooks the driver into its own
		 * target_list via from_driver_peer / to_driver_peer
		 * (impl-node.c:1063-1064). When driver.schedule is off
		 * the driver's self-sample is dropped at the producer
		 * because the worker is then deliberately blind to it
		 * (the worker's find_node_any_by_id lookup below resolves
		 * the driver entry, but routing a driver-id sample into
		 * apply_sample would still feed an unused WCET slot --
		 * cheap, but pointless). When driver.schedule is on the
		 * sample is kept: worker_drain_samples below resolves
		 * the driver's pre-existing struct node via
		 * find_node_any_by_id (no phantom is created), and the
		 * driver's WCET feeds the DAG analysis the same way a
		 * follower's does. The historical risk noted here -- a
		 * phantom shadowing the driver in nodes_by_id when the
		 * consumer used find_node_by_id and got NULL -- is gone
		 * because the consumer now uses the any-variant. */
		if (tnode == node && !drv->impl->driver_schedule)
			continue;

		runtime = get_runtime_ns(tnode, t->activation);

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
		/* find_node_any_by_id (not the follower-only variant):
		 * when tnode is the driver itself (driver.schedule on),
		 * the follower-only lookup returns NULL and the placement
		 * CPU falls to SAMPLE_CPU_UNKNOWN even after the driver
		 * has been pinned -- losing the cycles-to-reference
		 * normalisation. The any-variant returns whichever entry
		 * exists; for follower ids the result is identical. */
		struct node *n_lookup = find_node_any_by_id(drv->impl,
				tnode->info.id);
		s->cpu = (n_lookup != NULL && n_lookup->last_applied) ?
				n_lookup->last_cpu : SAMPLE_CPU_UNKNOWN;
		s->runtime_ns = runtime;
		s->cycles = SPA_ATOMIC_LOAD(t->activation->prev_run_cycles);
		s->period_ns = period;

		uint32_t xc = SPA_ATOMIC_LOAD(t->activation->xrun_count);
		bool target_xrun = (n_lookup != NULL &&
				xc != n_lookup->last_xrun_count);
		if (n_lookup != NULL)
			n_lookup->last_xrun_count = xc;
		s->xrun = (uint8_t)(driver_incomplete || target_xrun);

		spa_ringbuffer_write_update(&drv->ring, widx + sizeof(struct sample));

		/* Pick up deferred xrun runtime.  When a node completes
		 * after the driver has started the next cycle (xrun),
		 * process_node / signal_sync deposit the real completed
		 * runtime in xrun_run_time.  XCHG atomically reads and
		 * clears so each deferred sample is consumed once.
		 * Pushed as a non-xrun sample: the measurement is a
		 * valid completed runtime and should feed the WCET
		 * estimator. */
		uint64_t deferred_rt =
			SPA_ATOMIC_XCHG(t->activation->xrun_run_time, 0);
		if (deferred_rt != 0 && deferred_rt <= UINT64_MAX / 2) {
			uint32_t widx2;
			int32_t filled2 = spa_ringbuffer_get_write_index(
					&drv->ring, &widx2);
			if (filled2 >= 0 &&
					(uint32_t)filled2 < buf_size_bytes) {
				uint32_t off2 = (widx2 % buf_size_bytes);
				struct sample *s2 = (struct sample *)
					((uint8_t *)drv->ring_slots + off2);
				s2->node_id = tnode->info.id;
				s2->cpu = s->cpu;
				s2->runtime_ns = deferred_rt;
				s2->cycles = SPA_ATOMIC_XCHG(
					t->activation->xrun_run_cycles, 0);
				s2->period_ns = period;
				s2->xrun = 0;
				spa_ringbuffer_write_update(&drv->ring,
						widx2 + sizeof(struct sample));
			}
		}
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

		/* find_node_any_by_id: when driver.schedule is on, the
		 * driver's self-sample arrives in this ring; the
		 * follower-only lookup would return NULL for the driver
		 * and the calloc branch below would insert a phantom
		 * struct under the driver's id (the historical bug the
		 * producer-side self-skip used to mask). The any-variant
		 * resolves the driver entry created by
		 * context_driver_added and the phantom branch is reserved
		 * for genuinely new follower ids that arrive via samples
		 * before node_added has run. */
		struct node *n = find_node_any_by_id(impl, s->node_id);
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
		if (n) {
			/* Read the driver's current rate/quantum for the
			 * mode key. A race with a concurrent rate/quantum
			 * change is tolerated: the table just opens a new
			 * entry under the post-change key and the previous
			 * entry's samples remain valid for diagnostic
			 * inspection. drv->node may be NULL on a worker-
			 * created driver entry; the fall-back of (0, 0)
			 * still creates a deterministic mode key for the
			 * pre-binding interval. */
			uint32_t rate_hz = 0;
			uint32_t quantum = 0;
			if (drv->node != NULL) {
				rate_hz = drv->node->target_rate.denom;
				quantum = drv->node->target_quantum;
			}
			apply_sample(impl, n, s->runtime_ns, s->cycles,
					s->cpu, s->period_ns, rate_hz, quantum,
					s->xrun);
		}

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
		/* The driver landed in the scheduling DAG with 94b69ad24
		 * and now goes through sched_groups_dispatch as a regular
		 * SCHED_DEADLINE task; emit it alongside the followers so
		 * the snapshot reflects every node the kernel actually
		 * holds a reservation for. The driver-aware lookup below
		 * resolves both follower and driver struct nodes; the
		 * sched_dag_follower_in_set predicate already requires the
		 * dynamic-loop + published-TID pair the driver carries when
		 * context.dynamic-data-loops is on, so the driver is
		 * naturally gated by the same condition followers are. */
		if (!sched_dag_follower_in_set(n_iter))
			continue;

		struct node *mn = find_node_any_by_id(impl, n_iter->info.id);
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
			/* Translate the internal CPU index (the offset
			 * into impl->cpus[]) to the physical CPU number
			 * the kernel actually sees -- the same value
			 * sched_groups dispatch hands to sched_setaffinity
			 * at line ~1310. Storing the index in last_cpu is
			 * intentional (the LITTLE/BIG mode-class lookup at
			 * sample_cpu_to_mode_class indexes impl->topology
			 * with the same offset), but a dashboard reader
			 * expects the value to match `taskset -pc`. */
			pn.cpu = (mn->last_cpu < (uint32_t)impl->n_cpus)
				? (uint32_t)impl->cpus[mn->last_cpu]
				: mn->last_cpu;
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
			pn.budget_used_bootstrap = mn->budget_used_bootstrap;
		} else {
			pn.budget_kind = RT_DIAG_BUDGET_ADAPTIVE_CONFORMAL;
			pn.budget_used_bootstrap = false;
		}

		/* Predicted vs scheduled runtime split. The dag layer
		 * populates these fields on the dag_node during recalc;
		 * surface them through the same lookup that already
		 * resolved the macro-node leader so the JSON snapshot
		 * exposes the divergence per follower. Driver sentinels
		 * and pre-recalc followers report zero on both, which
		 * is the documented "not yet populated" state. */
		if (drv->reconcile != NULL) {
			reconcile_state_node_runtime_split(drv->reconcile,
					n_iter->info.id,
					&pn.predicted_runtime_ns,
					&pn.scheduled_runtime_ns);
		} else {
			pn.predicted_runtime_ns = 0;
			pn.scheduled_runtime_ns = 0;
		}
		/* Release-barrier surfacing. required_external_inputs
		 * comes from the contracted DAG (the post-contraction
		 * predecessor count for this follower's macro-node). */
		pn.required_external_inputs =
			reconcile_state_node_required_external_inputs(
				drv->reconcile, n_iter->info.id);

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
		/* The driver appears at the head of its own follower_list
		 * (impl-node.c registers the driver into its own list at
		 * pw_impl_node_register). When driver.schedule is off it
		 * is skipped here and never reaches the scheduling DAG;
		 * when on, it is treated as just-another-node. The
		 * dynamic-loop / TID filter below acts as the safe
		 * fallback: a driver still on the shared main loop has
		 * no TID published and is silently excluded. */
		if (follower == dnode && !a->impl->driver_schedule)
			continue;
		if (!pw_properties_get_bool(follower->properties,
					    PW_KEY_NODE_LOOP_DYNAMIC, false))
			continue;
		pid_t tid = pw_properties_get_int32(follower->properties,
						    PW_KEY_NODE_LOOP_TID, -1);
		if (tid == -1) {
			/* The follower opted into a dynamic data loop but
			 * its PW_KEY_NODE_LOOP_TID property is absent.  On
			 * a locally-owned node this means the do_gettid
			 * invoke at pw_impl_node_new has not yet returned,
			 * which is a momentary startup race; on a remote
			 * proxy (audio-dsp-filter, pw-jack clients) it
			 * means the client has not yet emitted the info
			 * update that publishes its data-loop TID back to
			 * the server -- typically after a set_loop_group
			 * relocation.
			 *
			 * Either way the follower is silently excluded from
			 * the scheduling DAG and cannot receive a
			 * SCHED_DEADLINE tuple, even though every other
			 * eligibility check passed. If the missing-TID
			 * window persists across multiple topology
			 * generations the node ends up running at whatever
			 * default policy its loop was created with (the
			 * libpipewire default is SCHED_FIFO) and never
			 * recovers, because module-deadline only applies
			 * SCHED_DEADLINE -- it never lowers a thread back
			 * to a different policy that would be visible to
			 * the scheduler-state classifier.
			 *
			 * Surface the eligibility gap so the operator can
			 * correlate a stuck SCHED_FIFO follower with the
			 * missing TID. The warning is bounded by the
			 * topology generation: it fires only when the
			 * follower list is walked, which happens on
			 * topo.generation changes. */
			pw_log_warn("HDL-W080-FOLLOWER-TID-MISSING: "
					"follower id=%u name=\"%s\" remote=%d "
					"exported=%d has node.loop.dynamic=true "
					"but node.loop.tid is unpublished; "
					"excluded from scheduling DAG (will run "
					"at its loop's default policy until the "
					"client/proxy publishes its TID)",
					follower->info.id,
					follower->name ? follower->name : "",
					follower->remote ? 1 : 0,
					follower->exported ? 1 : 0);
			continue;
		}

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
		/* Same driver self-skip rationale as the node loop above:
		 * walking the driver's output_ports here is what supplies
		 * the driver-as-source edges that capture graphs need.
		 * Edges whose other endpoint is the driver are already
		 * picked up by the follower iterations because every
		 * follower with a link to the driver has the driver as
		 * its output_link target. */
		if (follower == dnode && !a->impl->driver_schedule)
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
			/* Arm the post-topology-change warm-up: for each
			 * follower the apply_sample path drops the next
			 * TOPO_CHANGE_WARMUP_SAMPLES samples so the
			 * one-off plugin first-touch cost (scratch buffer
			 * allocation, convolution priming, JIT warm-up)
			 * that the very first cycle after a new edge or a
			 * new node typically carries cannot enter the
			 * conformal ring. Also clear the peak-hold so the
			 * elevated value the previous topology installed
			 * does not persist into the new graph. The counter
			 * is advisory; missing one follower (a node added
			 * after the fingerprint was hashed, a transient
			 * lookup miss) just lets that follower's first
			 * post-change sample land in the ring, which is
			 * the legacy behaviour. */
			for (i = 0; i < t->n_nodes; i++) {
				struct node *fn = find_node_any_by_id(drv->impl,
						t->nodes[i].id);
				if (fn != NULL) {
					SPA_ATOMIC_STORE(
						fn->warmup_samples_remaining,
						TOPO_CHANGE_WARMUP_SAMPLES);
				}
			}
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
		if (impl->heterogeneous) {
			reconcile_state_set_heterogeneous_iterations(
					drv->reconcile,
					impl->heterogeneous_iterations);
		}
	}

	followers = calloc(t->n_nodes, sizeof(*followers));
	edges = t->n_edges ? calloc(t->n_edges, sizeof(*edges)) : NULL;
	if (!followers || (t->n_edges && !edges)) {
		free(followers);
		free(edges);
		return;
	}

	/* Admission filter. A follower whose conformal estimator has
	 * not yet cleared bootstrap publishes wcet = 0 through
	 * runtime_select_for_node, signalling "leave this thread on
	 * whatever module-rt gave it; do not synthesize a budget".
	 * Drop those nodes and any edge touching them before reconcile
	 * sees the topology, so:
	 *   - the chain critical path is computed only over admitted
	 *     followers (no bootstrap-time outliers inflate it);
	 *   - sched_cb is never invoked for a wcet=0 node, so
	 *     apply_sched_groups never tries to emit an invalid
	 *     SCHED_DEADLINE tuple for a follower that is not ready;
	 *   - the kernel-side scheduling policy for an unadmitted
	 *     follower is whatever module-rt installed
	 *     (SCHED_FIFO at the audio priority by default), so the
	 *     graph still runs at low latency while statistics warm.
	 * Once the follower's conformal flips to VALID/SHIFT, the next
	 * apply_sample pass writes a non-zero wcet, the follower
	 * re-enters the filter as admitted on the next reconcile, and
	 * the standard sched_setattr emission path takes over (the
	 * one-shot HDL-I010 info line marks the handoff). */
	uint32_t admitted_count = 0;
	for (i = 0; i < t->n_nodes; i++) {
		/* find_node_any_by_id (not find_node_by_id):
		 * when driver.schedule is true, t->nodes[i].id
		 * may be the driver's own id and the follower
		 * variant deliberately filters drivers out.
		 * Driver and follower struct nodes share the
		 * same id-index and the same wcet/conformal
		 * slots, so the any-variant returns the right
		 * entry whichever it is. */
		struct node *n = find_node_any_by_id(impl,
				t->nodes[i].id);
		uint64_t w = n ? n->wcet : 0;
		if (w == 0)
			continue;
		followers[admitted_count].id = t->nodes[i].id;
		followers[admitted_count].tid = t->nodes[i].tid;
		followers[admitted_count].wcet = w;
		admitted_count++;
	}
	uint32_t admitted_edges = 0;
	for (i = 0; i < t->n_edges; i++) {
		uint32_t src = t->edges[i].src;
		uint32_t dst = t->edges[i].dst;
		bool src_admitted = false, dst_admitted = false;
		uint32_t j;
		for (j = 0; j < admitted_count; j++) {
			if (followers[j].id == src)
				src_admitted = true;
			if (followers[j].id == dst)
				dst_admitted = true;
			if (src_admitted && dst_admitted)
				break;
		}
		if (!(src_admitted && dst_admitted))
			continue;
		edges[admitted_edges].src = src;
		edges[admitted_edges].dst = dst;
		admitted_edges++;
	}

	rtopo.followers = followers;
	rtopo.n_followers = admitted_count;
	rtopo.edges = edges;
	rtopo.n_edges = admitted_edges;
	rtopo.period = t->period;
	rtopo.generation = SPA_ATOMIC_LOAD(t->generation);

	rtopo.timing_root_id = 0;
	if (impl->driver_schedule) {
		for (i = 0; i < admitted_count; i++) {
			if (followers[i].id == drv->node_id) {
				rtopo.timing_root_id = drv->node_id;
				break;
			}
		}
	}

	sched_groups_reset(&impl->sched_groups);
	(void)reconcile_apply(drv->reconcile, &rtopo, sched_cb, impl);
	apply_sched_groups(impl, drv);

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
static void rt_hook_impl(void *data, bool driver_incomplete)
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
		rt_push_samples(drv, period, driver_incomplete);
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

static void rt_hook_complete(void *data)   { rt_hook_impl(data, false); }
static void rt_hook_incomplete(void *data) { rt_hook_impl(data, true);  }

static const struct pw_impl_node_rt_events node_rt_events = {
	PW_VERSION_IMPL_NODE_RT_EVENTS,
	.complete   = rt_hook_complete,
	.incomplete = rt_hook_incomplete,
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
	/* pw_context_emit_driver_added fires from inside insert_driver(),
	 * which runs before pw_impl_node_register assigns node->info.id
	 * (impl-node.c reads as: insert_driver(); registered=true;
	 *  info.id = global->id). So node->info.id is still zero here for
	 * every driver, which collapses the id-keyed index and breaks
	 * snapshot routing. The global is already created by this point,
	 * and global->id is what info.id will be set to a few lines later,
	 * so use it as the stable id. */
	n->node_id = node->global ? pw_global_get_id(node->global)
				 : node->info.id;
	n->is_driver = true;

	/* Allocate the per-driver SPSC sample ring. Worker reads, RT
	 * thread writes. Drop-oldest on overflow. */
	n->ring_capacity = WORKER_RING_CAPACITY;
	n->ring_slots = calloc(n->ring_capacity, sizeof(struct sample));
	if (n->ring_slots == NULL) {
		pw_log_warn("driver %u: failed to allocate sample ring; module disabled for this driver",
			    n->node_id);
		free(n);
		return;
	}
	spa_ringbuffer_init(&n->ring);

	spa_list_append(&impl->node_list, &n->link);
	if (node_register(impl, n) < 0) {
		pw_log_warn("driver %u: failed to register in id index; module disabled for this driver",
			    n->node_id);
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

/* Fill impl->cpus[] with the default deadline-CPU set [ 0 1 2 3 ].
 * Used when cpus.available is unset or empty: the operator picks a
 * different subset by setting the property explicitly. */
static int default_cpus(struct impl *impl)
{
	int i;
	for (i = 0; i < 4; i++)
		impl->cpus[i] = i;
	return 4;
}

static void parse_cpus(struct impl *impl, const char *cpus_str)
{
	struct spa_json it[3];
	int i = 0, v;

	if (cpus_str == NULL || cpus_str[0] == '\0') {
		impl->n_cpus = default_cpus(impl);
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
		i = default_cpus(impl);
	impl->n_cpus = i;
}

static enum cpu_smt_policy parse_smt_policy(const char *s)
{
	if (s == NULL)
		return CPU_SMT_DEDUPE;
	if (strcmp(s, "strict") == 0)
		return CPU_SMT_STRICT;
	if (strcmp(s, "dedupe") == 0)
		return CPU_SMT_DEDUPE;
	if (strcmp(s, "ignore") == 0)
		return CPU_SMT_IGNORE;
	pw_log_warn("cpus.smt-policy '%s' not recognised; using 'dedupe'", s);
	return CPU_SMT_DEDUPE;
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

	/* cpus.classes overrides the auto-classification on a per-CPU
	 * basis. Format: a JSON array of strings aligned with the
	 * cpus.available order, each "little" or "big". Anything else is
	 * logged and skipped. On homogeneous x86 the auto-classifier
	 * stamps every CPU as big; this knob lets the live-test runners
	 * exercise the heterogeneous worst-fit and warm-up state machine
	 * by declaring a fake split. */
	const char *classes_str = pw_properties_get(props, "cpus.classes");
	if (classes_str != NULL && classes_str[0] != '\0') {
		struct spa_json it[2];
		spa_json_init(&it[0], classes_str, strlen(classes_str));
		if (spa_json_enter_array(&it[0], &it[1]) <= 0)
			spa_json_init(&it[1], classes_str, strlen(classes_str));
		char val[16];
		uint32_t idx = 0;
		while (spa_json_get_string(&it[1], val, sizeof(val)) > 0 &&
				idx < impl->topology.num_cpus) {
			enum rt_core_class cc;
			if (strncmp(val, "little", 6) == 0)
				cc = RT_CORE_LITTLE;
			else if (strncmp(val, "big", 3) == 0)
				cc = RT_CORE_BIG;
			else {
				pw_log_warn("cpus.classes[%u]=%s ignored "
						"(expected \"little\" or "
						"\"big\")", idx, val);
				idx++;
				continue;
			}
			cpu_topology_set_core_class(&impl->topology,
					impl->topology.cpus[idx].cpu_id, cc);
			idx++;
		}
	}

	/* cpus.freq-source picks the default frequency basis the placer
	 * uses for cycles-to-runtime conversion: scaling_min (the safe
	 * default, identical to the cpufreq conservative policy),
	 * scaling_max, or user (each CPU's value must come from
	 * cpus.freq.<id>.user-hz; CPUs without an override keep the
	 * dvfs-policy fallback). */
	const char *freq_source_str = pw_properties_get(props, "cpus.freq-source");
	if (freq_source_str != NULL && freq_source_str[0] != '\0') {
		enum cpu_freq_source src;
		if (strcmp(freq_source_str, "scaling_min") == 0 ||
				strcmp(freq_source_str, "min") == 0)
			src = CPU_FREQ_SCALING_MIN;
		else if (strcmp(freq_source_str, "scaling_max") == 0 ||
				strcmp(freq_source_str, "max") == 0)
			src = CPU_FREQ_SCALING_MAX;
		else if (strcmp(freq_source_str, "user") == 0)
			src = CPU_FREQ_USER;
		else {
			pw_log_warn("cpus.freq-source=%s ignored "
					"(expected scaling_min|scaling_max|user)",
					freq_source_str);
			src = (impl->dvfs_policy == CPU_DVFS_ASSUME_MAX) ?
				CPU_FREQ_SCALING_MAX : CPU_FREQ_SCALING_MIN;
		}
		cpu_topology_resolve_frequencies(&impl->topology, src);
	}

	/* Per-CPU user frequency override. Keys take the form
	 * cpus.freq.<cpu_id>.user-hz=<value-in-Hz>. */
	for (i = 0; i < impl->topology.num_cpus; i++) {
		char key[64];
		snprintf(key, sizeof(key), "cpus.freq.%u.user-hz",
				impl->topology.cpus[i].cpu_id);
		const char *v = pw_properties_get(props, key);
		if (v == NULL || v[0] == '\0')
			continue;
		char *end;
		unsigned long long uhz = strtoull(v, &end, 0);
		if (end == v || uhz == 0) {
			pw_log_warn("%s=%s ignored (expected positive Hz)",
					key, v);
			continue;
		}
		cpu_topology_set_freq_override(&impl->topology,
				impl->topology.cpus[i].cpu_id, (uint64_t)uhz);
	}

	/* Re-derive relative_capacity from the now-final
	 * sched_frequency_hz so a fake big.LITTLE config (uniform
	 * cpufreq sysfs, heterogeneous user-hz overrides) produces a
	 * non-uniform capacity vector for the dag library. On a host
	 * whose sched_frequency_hz happens to be uniform the
	 * recomputation is a no-op (every entry stays at 1.0). After
	 * the refresh, mirror the updated topology vector back into
	 * impl->relative_capacity / impl->relative_capacity_nominal --
	 * those copies are what reconcile_init eventually forwards to
	 * dag_create, so without the mirror the dag library would
	 * still see the probe-time uniform values. */
	cpu_topology_refresh_relative_from_sched_freq(&impl->topology);
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
				" relative_capacity=%.3f nominal=%.3f "
				"core_class=%s sched_freq_hz=%" PRIu64
				" freq_source=%s",
				ci->cpu_id, ci->core_id, ci->island_id,
				ci->raw_capacity, ci->min_freq_khz,
				ci->max_freq_khz, ci->relative_capacity,
				ci->relative_capacity_nominal,
				rt_core_class_name(ci->core_class),
				ci->sched_frequency_hz,
				cpu_freq_source_name(ci->sched_frequency_source));
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

	/* Hard precondition: the executor measures per-cycle CPU work via
	 * perf_event_open(PERF_COUNT_HW_CPU_CYCLES) bound to each data-loop
	 * thread. Without that counter the adaptive-conformal estimator
	 * cannot observe frequency-invariant samples and the heterogeneous
	 * runtime model degrades to a wall-clock-only path that the
	 * scheduler is no longer designed to support. Refuse to load
	 * unconditionally when the kernel will not grant the open: the
	 * operator must either lower /proc/sys/kernel/perf_event_paranoid to
	 * <= 1 (sysctl, /etc/sysctl.d, or a tuned profile) or grant
	 * CAP_PERFMON to the daemon. Honour PIPEWIRE_DISABLE_MODULE_DEADLINE
	 * first so a disabled module never trips this gate. */
	{
		const char *disabled = getenv("PIPEWIRE_DISABLE_MODULE_DEADLINE");
		bool is_disabled = disabled != NULL && disabled[0] != '\0' &&
				strcmp(disabled, "0") != 0;
		if (!is_disabled && !pw_cycle_counter_supported()) {
			pw_log_error("module-deadline refuses to load: perf "
					"hardware CPU cycles are not available to "
					"user space (kernel.perf_event_paranoid > 1 "
					"or perf disabled). Set "
					"kernel.perf_event_paranoid <= 1 (sysctl "
					"or /etc/sysctl.d) and retry, or set "
					"PIPEWIRE_DISABLE_MODULE_DEADLINE=1 to "
					"keep the module loaded but inert.");
			return -EACCES;
		}
	}

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
		char *end; double v = spa_strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->conformal_cfg.alpha_target = v;
		else
			pw_log_warn("deadline.conformal.alpha_graph %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.alpha_min")) != NULL) {
		char *end; double v = spa_strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->conformal_cfg.alpha_min = v;
		else
			pw_log_warn("deadline.conformal.alpha_min %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.alpha_max")) != NULL) {
		char *end; double v = spa_strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->conformal_cfg.alpha_max = v;
		else
			pw_log_warn("deadline.conformal.alpha_max %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.eta")) != NULL) {
		char *end; double v = spa_strtod(s, &end);
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
		char *end; double v = spa_strtod(s, &end);
		if (end != s && v > 0.0 && v < 1.0)
			impl->conformal_cfg.ewma_location_lambda = v;
		else
			pw_log_warn("deadline.conformal.ewma_location_lambda %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.ewma_scale_lambda")) != NULL) {
		char *end; double v = spa_strtod(s, &end);
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
		char *end; double v = spa_strtod(s, &end);
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
	if ((s = pw_properties_get(props, "deadline.conformal.burst_threshold")) != NULL) {
		char *end; unsigned long v = strtoul(s, &end, 10);
		if (end != s && v <= UINT32_MAX)
			impl->conformal_cfg.burst_threshold = (uint32_t)v;
		else
			pw_log_warn("deadline.conformal.burst_threshold %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.burst_penalty")) != NULL) {
		char *end; double v = spa_strtod(s, &end);
		if (end != s && v >= 1.0 && v <= 1e6)
			impl->conformal_cfg.burst_penalty = v;
		else
			pw_log_warn("deadline.conformal.burst_penalty %s ignored", s);
	}
	if ((s = pw_properties_get(props, "deadline.conformal.shift_burst_threshold")) != NULL) {
		char *end; unsigned long v = strtoul(s, &end, 10);
		if (end != s && v <= UINT32_MAX)
			impl->conformal_cfg.shift_burst_threshold = (uint32_t)v;
		else
			pw_log_warn("deadline.conformal.shift_burst_threshold %s ignored", s);
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
		double v = spa_strtod(s, &end);
		if (end != s && v >= 0.0 && v < 1.0)
			impl->wcet_recalc_threshold = v;
		else
			pw_log_warn("wcet.recalc-threshold %s ignored", s);
	}

	impl->heterogeneous = pw_properties_get_bool(props,
			"deadline.heterogeneous", false);
	impl->heterogeneous_iterations = 2;
	if ((s = pw_properties_get(props, "deadline.iterations")) != NULL) {
		char *end;
		unsigned long v = strtoul(s, &end, 10);
		if (end != s && v >= 1 && v <= 8) {
			impl->heterogeneous_iterations = (uint32_t)v;
		} else {
			pw_log_warn("deadline.iterations %s ignored "
					"(must be in [1, 8])", s);
		}
	}
	pw_log_info("deadline.heterogeneous = %s, iterations = %u",
			impl->heterogeneous ? "true" : "false",
			impl->heterogeneous_iterations);

	/* Pick the class an unplaced follower defaults to during the
	 * warm-up phase. On a topology that includes at least one
	 * LITTLE-class CPU the warm-up routes into the LITTLE bucket
	 * so a later migration onto a BIG core can borrow LITTLE
	 * statistics through the directional bootstrap fallback. On
	 * an all-BIG topology the warm-up routes directly into the
	 * BIG bucket; the bootstrap fallback has no LITTLE source and
	 * never needs to fire. Both paths are silent steady states --
	 * heterogeneous=true on a uniform host is not an error, it
	 * just collapses to BIG-only behaviour. */
	{
		uint32_t little_cpus = 0;
		for (uint32_t k = 0; k < impl->topology.num_cpus; k++) {
			if (impl->topology.cpus[k].core_class == RT_CORE_LITTLE)
				little_cpus++;
		}
		impl->default_warmup_class = (little_cpus > 0)
				? (uint8_t)RT_CONF_CORE_LITTLE
				: (uint8_t)RT_CONF_CORE_BIG;
		pw_log_info("deadline.default_warmup_class = %s "
				"(LITTLE cpus = %u of %u)",
				impl->default_warmup_class == RT_CONF_CORE_LITTLE
					? "little" : "big",
				little_cpus, impl->topology.num_cpus);
	}

	impl->on_infeasible = DEADLINE_ON_INFEASIBLE_KEEP_PREVIOUS;
	if ((s = pw_properties_get(props, "deadline.on-infeasible")) != NULL) {
		if (spa_streq(s, "keep-previous")) {
			impl->on_infeasible = DEADLINE_ON_INFEASIBLE_KEEP_PREVIOUS;
		} else if (spa_streq(s, "rt-fallback")) {
			impl->on_infeasible = DEADLINE_ON_INFEASIBLE_RT_FALLBACK;
		} else if (spa_streq(s, "apply-degraded")) {
			impl->on_infeasible = DEADLINE_ON_INFEASIBLE_APPLY_DEGRADED;
		} else if (spa_streq(s, "reject-new-graph")) {
			impl->on_infeasible = DEADLINE_ON_INFEASIBLE_REJECT_NEW_GRAPH;
		} else {
			pw_log_warn("deadline.on-infeasible %s ignored "
					"(must be one of keep-previous, "
					"rt-fallback, apply-degraded, "
					"reject-new-graph)", s);
		}
	}

	impl->driver_schedule = pw_properties_get_bool(props,
			"driver.schedule", false);

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

		/* "thread-loop.start-signal" tells do_loop to call
		 * pw_thread_loop_signal() right after pw_loop_enter()
		 * sets impl->thread. Without this the next pw_loop_invoke
		 * can race the new thread: while impl->thread is still 0,
		 * loop_queue_invoke takes the in_thread=true path and runs
		 * the callback synchronously on the caller (the main
		 * thread), so worker_setup would promote the WRONG tid
		 * (the daemon main loop) to SCHED_FIFO and leave the
		 * actual worker at SCHED_OTHER. */
		struct pw_properties *tloop_props = pw_properties_new(
			"thread-loop.start-signal", "true",
			NULL);
		impl->worker_tloop = pw_thread_loop_new("deadline-recalc",
				tloop_props ? &tloop_props->dict : NULL);
		pw_properties_free(tloop_props);
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

		/* Lock + start + wait pattern: the lock is released by
		 * pw_thread_loop_wait() while sleeping on the condvar,
		 * so the worker thread can reach pw_loop_enter() (which
		 * takes the same mutex). When the worker signals from
		 * do_loop after entering, wait() returns with the lock
		 * held and impl->thread already set to the worker's
		 * pthread_t. */
		pw_thread_loop_lock(impl->worker_tloop);
		if (pw_thread_loop_start(impl->worker_tloop) < 0) {
			pw_thread_loop_unlock(impl->worker_tloop);
			pw_log_error("failed to start deadline-recalc thread: %m");
			pw_loop_destroy_source(impl->worker_loop, impl->worker_wake);
			impl->worker_wake = NULL;
			pw_thread_loop_destroy(impl->worker_tloop);
			impl->worker_tloop = NULL;
			goto worker_failed;
		}
		pw_thread_loop_wait(impl->worker_tloop);
		pw_thread_loop_unlock(impl->worker_tloop);

		/* impl->thread is now set; loop_queue_invoke will take
		 * the cross-thread path and post worker_setup to the
		 * worker's queue. Failure is non-fatal: the worker still
		 * works, just at SCHED_OTHER. */
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
