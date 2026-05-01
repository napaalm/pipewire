#ifndef MODULE_DEADLINE_SCHED_GROUPS_H
#define MODULE_DEADLINE_SCHED_GROUPS_H

/*
 * sched_groups.h
 *
 * Per-TID accumulator used by module-deadline to collapse a
 * dag_foreach_node pass into one SCHED_DEADLINE update per OS
 * thread.
 *
 * When libpipewire's adjacent-chain merge consolidates several
 * follower nodes onto a single data-loop thread, those followers
 * all publish the same PW_KEY_NODE_LOOP_TID. The library's analysis
 * still produces a per-node (runtime, deadline, cpu) triple -- one
 * call per real node into the foreach callback -- but the kernel
 * call must apply a single (summed runtime, summed deadline,
 * period, cpu) per TID, otherwise the last call wins and the chain
 * gets just one member's budget.
 *
 * This accumulator is the bridge: the callback folds per-node
 * observations into per-TID slots, and the apply pass walks the
 * slots once the foreach is finished.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One slot per distinct TID observed in a pass. period and cpu are
 * the same for every member of a group by construction (global
 * period; single-CPU placement enforced by dag_set_node_group), so
 * we just keep the latest observation. leader_id is the lowest
 * follower id seen for this TID; it's the cache anchor module-
 * deadline uses to skip a redundant sched_setattr when the
 * applied tuple matches the last applied value.
 *
 * Runtime is summed: a fused thread runs its members serially, so
 * the kernel must reserve the sum of per-member runtimes.
 *
 * The deadline handed to sched_setattr is NOT the sum of member
 * deadlines: per Linux kernel sched-deadline.rst the configured
 * deadline is a relative quantity against the task's own
 * activation, while a per-member splitter slice represents a node's
 * relative budget. Summing the slices conflates the two and
 * produces a fused-thread deadline that has no defined meaning. The
 * accumulator therefore records:
 *
 *   - leader_local_deadline: the leader follower's own kernel-API
 *     local deadline. For a singleton (n_members == 1) this is the
 *     deadline the kernel must see -- the un-fused node's
 *     splitter slice.
 *   - max_cumulative_deadline: the maximum graph-relative
 *     cumulative deadline across the group's members. For a
 *     multi-member (chain-fused) thread this is a conservative
 *     hotfix kernel deadline, valid as long as the group is
 *     externally atomic: the chain must finish by the latest
 *     graph-relative milestone its members owe. The contracted-
 *     DAG re-assignment that lands in a follow-up cycle will
 *     replace this stopgap with a properly re-derived
 *     local_deadline computed on a macro-node.
 *
 * The caller picks between the two at apply time according to
 * group composition.
 */
struct sched_group {
	pid_t    tid;
	uint32_t leader_id;
	uint64_t sum_runtime;
	uint64_t leader_local_deadline;
	uint64_t max_cumulative_deadline;
	uint64_t period;
	uint32_t cpu;
	uint32_t n_members;
};

struct sched_groups {
	struct sched_group *entries;
	uint32_t            count;
	uint32_t            cap;
};

/* Zero-initialise the struct. Safe to call on stack-allocated
 * struct sched_groups. */
void sched_groups_init(struct sched_groups *sg);

/* Free the backing array. After fini the struct is back to the
 * initial state (count = cap = 0, entries = NULL); calling init
 * again is fine. */
void sched_groups_fini(struct sched_groups *sg);

/* Reset count to zero without freeing. Used at the start of every
 * reconcile pass so the buffer is reused across calls. */
void sched_groups_reset(struct sched_groups *sg);

/* Look up the slot for `tid`. Returns the existing slot if `tid`
 * was already inserted in this pass; allocates and returns a fresh
 * (zero-initialised, leader_id = UINT32_MAX) slot otherwise.
 * Returns NULL on ENOMEM (errno is left at ENOMEM in that case). */
struct sched_group *sched_groups_find_or_insert(struct sched_groups *sg, pid_t tid);

/* Convenience: fold one per-node observation into the matching TID
 * slot.
 *
 * - Adds `runtime` to sum_runtime (fused threads run members
 *   serially).
 * - Tracks max_cumulative_deadline = max over members. Used by the
 *   caller as the chain-fused thread's kernel deadline pending the
 *   contracted-DAG re-assignment that lands later.
 * - leader_id moves to min(existing leader, id); on every move,
 *   leader_local_deadline is rewritten to the new leader's local
 *   deadline so the singleton kernel-deadline path stays anchored
 *   on the lowest-id member.
 * - Overwrites period and cpu with the latest values (constant
 *   across a group's members by construction).
 * - Bumps n_members.
 *
 * tid <= 0 is treated as "no thread to merge onto" and returns
 * -EINVAL without touching sg. ENOMEM from a backing-array realloc
 * returns -ENOMEM. Returns 0 on success. */
int sched_groups_add(struct sched_groups *sg, uint32_t id, pid_t tid,
		uint64_t runtime,
		uint64_t cumulative_deadline, uint64_t local_deadline,
		uint64_t period, uint32_t cpu);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_SCHED_GROUPS_H */
