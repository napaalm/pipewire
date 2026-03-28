# module-deadline manual lab

This directory is tracked in git. The harness keeps transient state and logs
under `build/deadline-lab/`, so the source tree stays clean.

The main entry point is:

```bash
./test/manual/module-deadline/deadline-lab.sh
```

It runs PipeWire from the local build tree, on its own runtime socket, with a compact config that:

- enables `libpipewire-module-deadline`
- enables dynamic data loops
- exposes `audiotestsrc`
- keeps the graph under direct control, without a session manager

It is meant to operationalize the manual checks described in `VERIFICATION.txt`
and the post-`622bb0019` DAG regressions covered by `test-module-deadline-dag`.

## Quick start

Build the current tree:

```bash
./test/manual/module-deadline/deadline-lab.sh compile
```

Run the DAG regression executable:

```bash
./test/manual/module-deadline/deadline-lab.sh unit
```

Run the live verification pack:

```bash
./test/manual/module-deadline/deadline-lab.sh live-matrix
```

Run everything:

```bash
./test/manual/module-deadline/deadline-lab.sh all
```

Override the default build tree or lab output directory with `MDL_BUILD_DIR`
and `MDL_LAB_DIR` if needed. The default ALSA sink target is `hw:0,0`, and the
card scenario is skipped cleanly if that sink cannot be opened.

## Variants

- `base`: explicit `cpus.available` and `cpus.utilization`
- `no-cpus`: omits `cpus.available` to exercise affinity-mask fallback
- `no-util`: omits `cpus.utilization` to exercise the documented `0.95` default
- `no-dynamic-loop`: disables dynamic data loops to hit the warning path in `VERIFICATION.txt`

## Coverage map

The harness is meant to cover the post-`622bb0019` surface in two layers.

Compile and unit layer:

- build with the current `sched_attr` ABI detection path
- run `test-module-deadline-dag` for residual-budget propagation
- run `test-module-deadline-dag` for timing input contracts
- run `test-module-deadline-dag` for numeric load checks
- run `test-module-deadline-dag` for feasible positive deadlines
- run `test-module-deadline-dag` for stale scheduling invalidation
- run `test-module-deadline-dag` for unrelated-set CPU accounting
- run `test-module-deadline-dag` for DAG structure validation

Live daemon layer:

- direct daemon-only chain scheduling
- persistent DAG reuse in steady state
- topology growth without DAG recreation
- topology shrink and stale-node disappearance
- client-node scheduling through `pw-loopback`
- unrelated-set scheduling with two disconnected live graphs
- single ALSA card activation on `hw:0,0` by default
- alternate quantum and sample-rate attempts on the live graph
- `cpus.available` fallback to the current affinity mask
- `cpus.utilization` fallback to the module default
- warning-path coverage when nodes are not on dynamic data loops
- module unload while drivers are active
- experimental cyclic-graph attempt

## Artifacts

The script writes:

- logs under `build/deadline-lab/logs/`
- graph snapshots under `build/deadline-lab/logs/snapshots/`
- transient runtime/config state under `build/deadline-lab/state/`

`latest-summary.txt` contains the last `live-matrix` summary.
