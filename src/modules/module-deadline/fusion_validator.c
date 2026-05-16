/*
 * fusion_validator.c
 *
 * Implementation of the structural eligibility predicate for
 * fusion candidate groups declared in fusion_validator.h.
 *
 * Pure data: no PipeWire runtime symbols, no syscalls. Tests
 * drive the predicate with synthetic inputs; production wiring in
 * reconcile.c plugs the validator between the cost model's
 * candidate generator and the contracted-DAG builder.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fusion_validator.h"

bool fusion_validator_accept(const struct fusion_candidate_member *members,
		uint32_t n_members,
		enum fusion_reject_reason *out_reason)
{
	enum fusion_reject_reason ignore = FUSION_REJ_NONE;
	uint32_t i;
	uint32_t driver_id_seen = 0;
	bool driver_id_pinned = false;

	if (out_reason == NULL)
		out_reason = &ignore;
	*out_reason = FUSION_REJ_NONE;

	if (n_members == 0 || members == NULL)
		return true;

	for (i = 0; i < n_members; i++) {
		const struct fusion_candidate_member *m = &members[i];

		/* Same driver DAG: every member must belong to the same
		 * driver. The cost model never proposes a cross-driver
		 * group on the supported code paths, but a future
		 * generator might; rejecting here is defense in depth. */
		if (!driver_id_pinned) {
			driver_id_seen = m->driver_id;
			driver_id_pinned = true;
		} else if (m->driver_id != driver_id_seen) {
			*out_reason = FUSION_REJ_CROSS_DRIVER;
			return false;
		}

		/* Main-loop nodes do their work on the context's main
		 * loop, not on a data loop the daemon can put under
		 * SCHED_DEADLINE; they cannot participate in a fused
		 * data-loop reservation. */
		if (m->flags & FUSION_MEMBER_MAIN_LOOP) {
			*out_reason = FUSION_REJ_MAIN_LOOP;
			return false;
		}

		/* Exported nodes are owned by an out-of-process client
		 * the daemon does not control; the implementation cycle
		 * has no external-reservation model so they are
		 * categorically rejected. */
		if (m->flags & FUSION_MEMBER_EXPORTED) {
			*out_reason = FUSION_REJ_EXPORTED;
			return false;
		}

		/* Remote nodes are allowed iff their processing TID has
		 * been published; otherwise the daemon has no thread to
		 * configure. The TID==-1 sentinel matches what
		 * PW_KEY_NODE_LOOP_TID returns when the property is
		 * absent. */
		if ((m->flags & FUSION_MEMBER_REMOTE) && m->tid <= 0) {
			*out_reason = FUSION_REJ_REMOTE_TID_UNKNOWN;
			return false;
		}
	}

	return true;
}

const char *fusion_reject_reason_name(enum fusion_reject_reason r)
{
	switch (r) {
	case FUSION_REJ_NONE:               return "none";
	case FUSION_REJ_CROSS_DRIVER:       return "cross_driver";
	case FUSION_REJ_MAIN_LOOP:          return "main_loop";
	case FUSION_REJ_EXPORTED:           return "exported";
	case FUSION_REJ_REMOTE_TID_UNKNOWN: return "remote_tid_unknown";
	case FUSION_REJ_WOULD_SELF_SUSPEND: return "would_self_suspend";
	case FUSION_REJ_NON_CONVEX:         return "non_convex";
	case FUSION_REJ_INTERNAL_MILESTONE: return "internal_milestone";
	case FUSION_REJ_BLOCKING_RISK:      return "blocking_risk";
	}
	return "unknown";
}
