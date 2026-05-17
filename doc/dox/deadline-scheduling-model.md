\page deadline_scheduling_model module-deadline scheduling model

This page is the canonical reference for the data and timing
model used by `module-deadline` (sources under
`src/modules/module-deadline.c` and
`src/modules/module-deadline/*`). It documents the entities the
module reasons about and the invariants it maintains, so that
future contributors (and external auditors) can verify the
implementation against a stable definition without having to
reverse-engineer it from the code.

## Runtime graph

The **runtime graph** is the PipeWire data-plane DAG as a driver
sees it: a set of follower `pw_impl_node`s plus the directed
edges induced by `pw_impl_link`s between their output and input
ports. The runtime graph is the union of every edge that could
fire data during a cycle, including async edges (`l->feedback`
or endpoints with `pw_impl_node.async`) which carry a one-cycle
delay.

## Scheduling DAG

The **scheduling DAG** is the runtime graph projected onto its
in-period precedence skeleton. Edges that introduce no
within-period dependency are removed; specifically:

  * Feedback edges (`l->feedback == true`) -- the consumer reads
    last cycle's buffer, so there is no current-cycle precedence.
  * Async edges (either endpoint's `pw_impl_node.async == true`)
    -- same one-cycle-delay semantics, just declared by port
    flags instead of by link build order.

The scheduling DAG is the input to deadline assignment and
partitioning. The library guarantees acyclicity: the runtime
graph may contain cycles only via feedback or async edges, and
those are precisely the ones the projection removes.

## Contracted DAG

Fusion is a graph transformation. When the validator accepts a
candidate group `F = {v_1, ..., v_k}` as a single macro-actor,
the **contracted DAG** replaces the members with one macro-node
that aggregates their per-cycle work. The macro-node's WCET is
the sum of member WCETs plus a measured overhead bound
(`cn->overhead_ns` in `contracted.h`); its in-edges and
out-edges are the external edges that crossed the group's
boundary in the scheduling DAG, with parallel edges between the
same pair of contracted nodes deduplicated.

A contracted node has exactly one schedulable thread (the
group's data-loop TID); the kernel sees one SCHED_DEADLINE entity
per contracted node. The contracted DAG is the unit on which all
post-fusion analysis runs -- deadline splitting, CPU
partitioning, density / DBF feasibility. Unfused nodes appear as
singleton contracted nodes so the same data structure carries
both paths.

## Cumulative deadline

The **cumulative deadline** of a contracted node `v`,
`cumulative_deadline_ns`, is the time elapsed from the driver
graph's release until `v` must complete. Cumulative deadlines
are graph-relative: they're milestones along the longest path
from the source to `v`.

The splitter assigns them so that

```text
0 < cumulative_deadline_ns[v] <= D                  (end-to-end deadline)
cumulative_deadline_ns[u] <= cumulative_deadline_ns[v]   for every edge u -> v
```

(monotonicity along every contracted edge). The splitter
distributes slack proportional to per-node WCETs along each
critical path.

## Local deadline

The **local deadline** of a contracted node,
`local_deadline_ns`, is the time the node has to complete
*relative to its own release*. For a source contracted node it
equals the cumulative deadline (the node is released at the
driver activation); for any other node it is

```text
local_deadline_ns[v] = cumulative_deadline_ns[v] - max(cumulative_deadline_ns[p])
                                                   over p in predecessors(v)
```

This is the value passed to the kernel as `sched_deadline`. It
is *not* the cumulative milestone; under no circumstance does
the per-TID accumulator sum local deadlines to obtain a fused
thread's deadline -- the contracted DAG analysis already
computes one local deadline per macro-node.

Kernel-API contract:

```text
0 < runtime_budget_ns[v] <= local_deadline_ns[v] <= period_ns
```

## Hard mode

The published schedule is in **hard mode** when both:

  * the contracted DAG analysis admitted the schedule (cumulative
    monotonicity, positive local deadlines, path sums within
    `D`), and
  * the per-CPU partition passes the configured feasibility
    test (density-sufficient or DBF-exact).

Hard mode is the operator-visible claim that every contracted
node will meet its local deadline on every activation, subject
only to the kernel's SCHED_DEADLINE admission test.

The `feasibility.method` field of the JSON snapshot identifies
which predicate cleared (`density`, `dbf`, or `none` when the
classification fell through), and `feasibility.status` is
`feasible` in hard mode.

## Soft-degraded mode

When the analysis cannot meet the hard-mode invariants on the
current sample of WCETs the schedule does *not* fall back to
denial: the module transitions to **soft-degraded mode** and
keeps running with the least-bad parameter set it can produce.
The published mode flips to `soft_degraded` and the
`feasibility.status` field reads `infeasible`. A transition
warning is logged once per hard -> soft / soft -> hard
transition; per-cycle re-emission is suppressed by the
hysteresis counter (`feas.consecutive_hard_passes`, default
threshold 3).

The redistribution is deliberately minimal in the current
implementation: tuples that would violate
`runtime <= deadline <= period` are capped at 95 % of the
local deadline so the kernel admission test accepts them. The
follower then runs under a CBS budget below its measured demand
and may be throttled (the "throttling risk is expected"
operator-visible behaviour); the `kernel_rejected_or_invalid_params`
reason on the published feasibility report is the cause-of-
transition signal.

Mode classification is read by an operator via the snapshot's
top-level `mode` field; the underlying transition mechanism is
`reconcile_state_force_soft` in
`src/modules/module-deadline/reconcile.c`.

## References

  * Cucinotta et al., "Multi-criteria Optimization of Real-time
    DAGs on Heterogeneous Platforms under P-EDF", ACM TECS 23(1),
    2024 -- the source for the deadline-splitting and
    P-EDF placement formulation the post-fusion analysis
    implements.
  * Baruah, Howell, Rosier, "Algorithms and complexity concerning
    the preemptive scheduling of periodic, real-time tasks on
    one processor", Real-Time Systems 2(4), 1990 -- the
    constrained-deadline density bound and processor-demand
    feasibility test used in `dag_density_*` and `dag_dbf_*`.
  * Cucu-Grosjean et al., "Measurement-Based Probabilistic Timing
    Analysis for Multi-path Programs", ECRTS 2012 -- the source
    for the per-node MBPTA pWCET estimator wired into the runtime
    budget selection.
  * Linux SCHED_DEADLINE documentation
    (`Documentation/scheduler/sched-deadline.rst`) -- the
    kernel-API contract the module's output has to satisfy.
