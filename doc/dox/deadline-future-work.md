\page deadline_future_work module-deadline future work

This page tracks improvements to the deadline-scheduling stack
(`src/modules/module-deadline.c` plus
`src/modules/module-deadline/*`) that are intentionally not part
of the current implementation cycle. Each item names the
mechanism it would touch, the public reference that motivates
it, and the gating constraint that keeps it out of scope today.

## Per-node non-blocking capability declaration

Every data-loop follower is implicitly assumed to be
non-blocking inside its `process()` callback today -- the
contracted-DAG EDF feasibility proof (Baruah, Howell & Rosier
1990) only holds for non-self-suspending tasks, so the
assumption is load-bearing for the hard-mode classification
but the daemon never verifies it. A future commit would let
plugins declare the property explicitly through a per-node
RT capability bitmask, route the bits through
`reconcile_topo_t` into a new fusion-validator
"blocking-closure" predicate that rejects candidate groups
containing a node without the bit, and condition hard mode
on every member having declared it. The implementation is
gated on the spa_node side of the capability vocabulary,
which is not yet specified.

## Runtime voluntary-context-switch monitoring

The non-blocking assumption above is a static contract; a
plugin that silently violates it (a sleep, a futex wait, a
blocking syscall, an unbounded lock acquisition inside
`process()`) is a correctness failure no current code path
catches. A future runtime check would sample
`voluntary_ctxt_switches` from `/proc/<tid>/status` once per
reconcile pass, compare the delta against the expected
deadline-wait baseline (one yield per activation across the
interval), and demote the schedule to soft-degraded with a
typed `process_blocked_inside_rt` reason when the excess
exceeds a calibrated noise margin. The mechanism needs a
budget for the per-recalc /proc reads (O(followers) syscalls
off the RT path) and characterisation of the noise floor on
real workloads (lazy-init epoll paths, reservation
handshakes, topology-flip transients legitimately yield
beyond the per-cycle baseline) before it can land without
false demotions.

## Analyzability flags through reconcile_topo_t

The fusion-eligibility validator
(`src/modules/module-deadline/fusion_validator.c`) carries
analyzability predicates (`CROSS_DRIVER` / `MAIN_LOOP` /
`EXPORTED` / `REMOTE_TID_UNKNOWN`) but its analyzability
branches are vacuous on the production code path because
`reconcile_topo_t` does not yet carry per-follower
analyzability flags (main-loop / exported / remote). The
topology snapshot pre-filters out the unsupported cases
before they reach the validator, so the missing plumbing is
harmless today. Adding the fields lets the validator catch a
future code path that bypasses the pre-filter and makes the
JSON snapshot's rejection-reason column genuinely
informative.

## Runtime release barrier for fused groups

`fusion_validator_predecessor_closure_accept` is the safe
structural fallback to the runtime release barrier the plan
calls for. Strict predecessor closure rejects fewer groups than
a barrier would but it is non-self-suspending by construction.
A future commit can add a per-macro-node
`required_external_inputs` counter on `pw_impl_node`'s
activation chain so the group data loop wakes only after every
external predecessor has fired in the current period, unlocking
the fusions strict-closure currently turns away.

## Cpuset / cgroup partitioning

`set_cpu_affinity` uses raw `sched_setaffinity` to pin each
follower's data-loop thread to its assigned CPU. A cgroup-v2 /
cpuset-aware admission layer would let an operator declare the
deadline-scheduled subgraph as a single cpuset, isolate it from
non-RT housekeeping, and reason about its bandwidth budget in
the same vocabulary as systemd slices. Out of scope today
because PipeWire's broader policy on cgroup integration is not
yet settled.

## Warm-up quarantine for strict hard-real-time

The current contract starts every new follower immediately
under the bootstrap fallback budget; the MBPTA estimator catches
up over its warm-up window. A stricter hard-real-time mode
might want to defer activation until the estimator reports
`PWCET_VALID`. The MBPTA `INSUFFICIENT_DATA` /
`PENDING_CONVERGENCE` states give the dispatcher the signal it
would need; the activation deferral itself is the missing
piece.

## Strict admission denial

`reconcile_apply` accepts every topology the daemon hands it,
soft-degrading the schedule when feasibility predicates fail.
A future strict mode would deny insertion of nodes or links
that make the graph infeasible, rather than letting the soft
redistribution scale runtime down on the affected CPU. This
needs a policy decision on how strict-mode failure is surfaced
to clients (link rejection? device suspension? operator
override?) before the implementation can land.

## WCET-growth invalidation policy

When an MBPTA estimator transitions from `PWCET_VALID` to
`DRIFT` because sustained i.i.d. rejection invalidates the
fit, the dispatcher currently keeps the previously-cached
pWCET in place until the next reconcile and falls back to the
empirical / bootstrap budget. Strict mode might want to reject
the new estimate, degrade audio quality (e.g. via period
extension), disable fusion on the affected node, or stop the
stream. Each is a policy choice with operator-visible
consequences; the plan's intent is to defer the choice until
the simpler soft-degradation path has lived in production for
long enough to characterise the failure modes.

## Milestone-aware fusion

The externally-atomic predicate rejects groups whose
non-terminal members have external successors because the
macro-node deadline cannot pin the intermediate-output
milestone. A future milestone-aware dispatcher would assign per-
segment internal deadlines and dispatch members via
earliest-internal-deadline-first within the group, accepting
the groups today's policy turns away. The dispatcher rework is
substantial and unlikely to land before the simpler externally-
atomic policy has been exercised on real workloads.

## Suspension-aware analysis

If the implicit non-blocking assumption (see above) is
relaxed to admit nodes that can suspend in `process()`, the
feasibility analysis must switch from constrained-deadline
EDF (the current Baruah, Howell & Rosier 1990 model) to one
of the suspension-aware variants surveyed in Chen, Nelissen,
Huang, Yang, Brandenburg, Bletsas, Liu, Richard, Ridouard,
Audsley, Rajkumar, de Niz, von der Bruggen, "Many
Suspensions, Many Problems", Real-Time Systems
55(1):144-207, 2019. The trade-off is admission
restrictiveness versus model complexity; the current code
plays it safe by trusting the non-blocking assumption and
never claiming hard guarantees for work that turns out to be
self-suspending.

## MIQCP / offline optimal solver integration

Cucinotta, Amory, Ara, Paladino, Di Natale, "Multi-criteria
Optimization of Real-time DAGs on Heterogeneous Platforms
under P-EDF", ACM TECS 23(1), 2024, formulates the joint
deadline-assignment + CPU-partitioning problem as a Mixed-
Integer Quadratically Constrained Program. The in-process
splitter and worst-fit placer are heuristics; an offline MIQCP
solver could serve as a debug oracle for small graphs (compare
the heuristic's output against the optimal) or as the
production choice for graphs where the heuristic underperforms.
The integration shape -- direct in-process Z3 / Gurobi call vs
external solver pipe -- is the gating question; not in scope
today.

## True MBPTA / pWCET measurement chain

The MBPTA estimator implements the Cucu-Grosjean et al. 2012
ECRTS pipeline directly except for the ET (Exponential Tail)
test that decides whether the block-maxima distribution falls
in the Gumbel sub-family of GEV. The implementation assumes
Gumbel; a future commit would land the Gomes & Pestana ET test
referenced by Cucu-Grosjean §II-A and route a rejection through
the reserved `MBPTA_NON_GUMBEL` state. A Frechet / Weibull
branch for non-Gumbel tails is a further follow-up; the audio
workload's published evidence is well within Gumbel territory,
so the gain from those branches is uncertain.

## Controlled multithreaded-plugin reservations

A plugin that spawns its own worker threads needs a group
reservation that covers every thread's combined budget, not a
single SCHED_DEADLINE call on the PipeWire-facing thread. The
fusion-validator's analyzability table rejects such plugins
from hard fusion today (no public mechanism exists to enumerate
a plugin's worker pool). A future contract on
`spa_node`-side capability declaration would let the validator
admit multi-thread plugins in soft mode and, eventually, in
hard mode via a multi-task group reservation.

## Heterogeneous CPU model

The `cpu_topology` module exposes per-CPU `relative_capacity`
already, and the feasibility predicates / sketch normalisation
both consume it. What's missing is a topology model where CPU
classes are distinct (e.g. big.LITTLE, P-cores / E-cores) so
placement can reason about cache hierarchy and migration cost
rather than just throughput. The Cucinotta et al. 2024 P-EDF
formulation provides the algebra; the in-process implementation
would need a new placement policy plus diagnostics for class
membership.

## MBPTA Frechet / Weibull branch

The MBPTA pipeline in `src/modules/module-deadline/mbpta.c`
implements the Gumbel sub-family of GEV: the Hosking-PWM ET
test on the shape parameter k routes a rejection to
`NON_GUMBEL`, at which point the node falls back to empirical
or fallback budgets. Extending to the heavy-tailed Frechet
(k > 0) and bounded Weibull (k < 0) branches of GEV would let
those rejections still produce a probabilistic budget, with the
appropriate shape-aware tail extrapolation
`pWCET(eps) = mu + sigma / k * ((-log(1 - eps))^(-k) - 1)`
in place of the Gumbel inverse CDF. Cucu-Grosjean 2012 §II-A
sketches the full GEV machinery; an implementation needs
shape-aware fit, a confidence interval on k, and a separate
goodness-of-fit test per branch. The current "Gumbel-or-bust"
behaviour is sound but pessimistic for workloads whose
extremes follow either of the other two sub-families.

## Tree-based compositional pWCET aggregation

The current MBPTA fit treats each contracted node as a single
MBPTA subject: a sample is the per-cycle execution time of one
data-loop thread, and the pWCET is the distribution's
extrapolated quantile. Cucu-Grosjean 2012 §IV describes a
tree-based compositional aggregation that lets a system-level
pWCET be derived from per-segment pWCETs (echoing the
Bernat/Burns analyses), trading some per-node pessimism for
graph-level tightness. Implementing it in this code base would
require per-segment sample collection (today every contracted
node emits one sample per cycle), a composition operator on
two Gumbel distributions that respects the dependency
structure of the contracted DAG, and a worked example
demonstrating that the composed pWCET is tighter than the
per-node sum under realistic workloads.
