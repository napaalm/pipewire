/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#ifndef WCET_SKETCH_H
#define WCET_SKETCH_H

/*
 * wcet_sketch.h
 *
 * Streaming-quantile estimator over a sliding window of execution-time
 * samples, used by module-deadline to set the SCHED_DEADLINE runtime
 * budget without permanently inflating it from one-shot transients
 * (startup, IRQ storms, cold caches, etc.).
 *
 * Core data structure: the t-digest of T. Dunning and O. Ertl,
 * "Computing Extremely Accurate Quantiles Using t-Digests",
 * arXiv:1902.04023, 2019.
 *
 * We implement Algorithm 1 (the merging variant) with the k_1 scale
 * function (eq. 3 of the paper) and the four-case CDF interpolation
 * described in section 2.9.
 *
 * For sliding-window forgetting we keep two digests in alternation:
 * new samples land in `cur`; once `cur` has window_size/2 samples we
 * rotate (prev <- cur, cur <- empty). Queries are answered from
 * (prev union cur), so the effective window oscillates between
 * window_size/2 (just after a rotation) and window_size (just before
 * the next). This is the standard two-bucket sliding-window approach
 * for mergeable summaries and is straightforward because t-digests
 * are themselves mergeable (paper, section 2.5).
 *
 * Thread safety: a single sketch is not thread-safe. In module-
 * deadline each per-node sketch is touched only from the driver's
 * data-loop thread inside recalc_params(), so no synchronization is
 * needed.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A single cluster in the digest: weighted mean of the samples it absorbs. */
typedef struct {
	double mean;
	double count;
} td_centroid_t;

typedef struct tdigest {
	double compression;          /* delta in the paper */

	td_centroid_t *centroids;    /* sorted by mean, length n_centroids */
	uint32_t n_centroids;
	uint32_t max_centroids;

	double *buffer;              /* unsorted incoming raw samples */
	uint32_t n_buffered;
	uint32_t max_buffered;

	double total_count;          /* total weight across all centroids */
	double min_seen;
	double max_seen;
} tdigest_t;

typedef struct wcet_sketch {
	uint32_t window_size;        /* tunable target window length */
	uint32_t rotate_at;          /* = window_size / 2, recomputed on init */
	double quantile;             /* in (0, 1), e.g. 0.999 */
	double compression;          /* per-digest delta */

	tdigest_t prev;              /* completed half of the window */
	tdigest_t cur;               /* currently filling half */
	uint32_t cur_count;          /* samples added to cur since last rotation */
} wcet_sketch_t;

/*
 * Low-level t-digest API. Allocates internal arrays sized off the
 * compression parameter. Buffer size follows Dunning's recommendation
 * of ~5*ceil(delta) (paper, section 2.6).
 */
int  tdigest_init(tdigest_t *td, double compression);
void tdigest_fini(tdigest_t *td);
void tdigest_reset(tdigest_t *td);

/* Stage one sample with unit weight. Triggers a merge if the buffer fills. */
void tdigest_add(tdigest_t *td, double sample);

/*
 * Pump the staged buffer through Algorithm 1 of the paper, producing
 * a fully merged digest. Safe to call when the buffer is empty.
 */
void tdigest_compress(tdigest_t *td);

/*
 * Return the q-quantile (q in [0, 1]) of the samples in this digest.
 * Forces a compress() first if anything is buffered. Returns 0.0 on
 * an empty digest.
 */
double tdigest_quantile(tdigest_t *td, double q);

/* Sum of weights observed. */
double tdigest_total_count(const tdigest_t *td);

/*
 * Sliding-window sketch API. The sketch owns its two embedded
 * digests; init/fini manage their internal allocations.
 *
 * compression  - t-digest delta (Dunning recommends 100; larger gives
 *                more accuracy in the tails at proportional memory cost).
 * window_size  - total samples retained at most (effective window
 *                oscillates in [window_size/2, window_size]).
 * quantile     - the q in (0, 1) the sketch will report on query.
 */
int  wcet_sketch_init(wcet_sketch_t *s, uint32_t window_size,
		      double compression, double quantile);
void wcet_sketch_fini(wcet_sketch_t *s);

/* Drop all samples. Use on period changes when prior samples no
 * longer represent the same workload. */
void wcet_sketch_reset(wcet_sketch_t *s);

/* Insert a new execution-time observation. */
void wcet_sketch_add(wcet_sketch_t *s, double sample);

/*
 * Compute the configured quantile across the current window. Returns
 * 0.0 if the sketch is empty.
 */
double wcet_sketch_quantile(wcet_sketch_t *s);

/* Total samples currently retained across prev + cur. */
uint32_t wcet_sketch_count(const wcet_sketch_t *s);

#ifdef __cplusplus
}
#endif

#endif /* WCET_SKETCH_H */
