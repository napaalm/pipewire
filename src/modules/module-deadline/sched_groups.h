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
 * deadline uses to skip a redundant sched_setattr when the summed
 * tuple matches the last applied value. */
struct sched_group {
	pid_t    tid;
	uint32_t leader_id;
	uint64_t sum_runtime;
	uint64_t sum_deadline;
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
 * - Updates leader_id to min(existing leader, id). The first
 *   observation initialises it to `id`.
 * - Adds runtime / deadline to the running sums.
 * - Overwrites period and cpu with the latest values (these are
 *   constant across a group's members by construction; the
 *   overwrite is just simpler than tracking "first observation
 *   only").
 * - Bumps n_members.
 *
 * tid <= 0 is treated as "no thread to merge onto" and returns
 * -EINVAL without touching sg. ENOMEM from a backing-array realloc
 * returns -ENOMEM. Returns 0 on success. */
int sched_groups_add(struct sched_groups *sg, uint32_t id, pid_t tid,
		uint64_t runtime, uint64_t deadline,
		uint64_t period, uint32_t cpu);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_SCHED_GROUPS_H */
