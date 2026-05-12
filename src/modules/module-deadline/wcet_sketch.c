/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * wcet_sketch.c
 *
 * t-digest implementation following T. Dunning and O. Ertl,
 * "Computing Extremely Accurate Quantiles Using t-Digests",
 * arXiv:1902.04023, 2019.
 *
 * Section/equation references in the comments below point into that
 * paper. The construction here uses:
 *   - Algorithm 1 (merging variant, paper p. 9)
 *   - Scale function k_1 (eq. 3 and eq. 7)
 *   - CDF interpolation cases (a)-(d) of section 2.9
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include "wcet_sketch.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


/* Buffer size factor: paper section 2.6 reports ~10*delta as a sweet
 * spot for amortizing merge cost; we pick 5* since queries are frequent
 * and we want bounded memory. Memory at delta=100: 101 centroids
 * (16 B each) + 505 buffer slots (8 B each) ~= 5.7 kB per digest. */
#define TDIGEST_BUFFER_FACTOR	5u


/* k_1 scale function and its inverse (paper eq. 3):
 *
 *   k_1(q)     = (delta / (2 pi)) * arcsin(2q - 1)
 *   k_1^-1(k)  = (sin(2 pi k / delta) + 1) / 2
 *
 * k(q) ranges over [-delta/4, +delta/4] for q in [0, 1]. We clamp
 * out-of-range inputs so the merging loop terminates cleanly when
 * k(q_0) + 1 spills past the upper edge. */
static inline double k1(double q, double compression)
{
	if (q <= 0.0)
		return -compression / 4.0;
	if (q >= 1.0)
		return compression / 4.0;
	return (compression / (2.0 * M_PI)) * asin(2.0 * q - 1.0);
}

static inline double k1_inv(double k, double compression)
{
	double k_max = compression / 4.0;
	if (k <= -k_max)
		return 0.0;
	if (k >= k_max)
		return 1.0;
	return (sin(2.0 * M_PI * k / compression) + 1.0) * 0.5;
}

static int centroid_cmp(const void *a, const void *b)
{
	const td_centroid_t *ca = a;
	const td_centroid_t *cb = b;
	if (ca->mean < cb->mean) return -1;
	if (ca->mean > cb->mean) return 1;
	return 0;
}

/*
 * Algorithm 1 of the paper (p. 9). The caller hands us a sorted
 * sequence of weighted points (raw samples have weight 1) along with
 * its total weight S and a compression delta. We greedily merge along
 * the cumulative-weight axis subject to the k_1 size bound (eq. 4)
 *
 *     |C|_k = k(q_right) - k(q_left) <= 1
 *
 * writing the consolidated cluster sequence back into the same array
 * (in place) and returning its new length.
 */
static uint32_t cluster_merge_inplace(td_centroid_t *xs, uint32_t n,
				      double S, double compression)
{
	if (n == 0 || S <= 0.0)
		return 0;

	uint32_t out_n = 0;
	double q0 = 0.0;
	double q_limit = k1_inv(k1(q0, compression) + 1.0, compression);
	td_centroid_t sigma = xs[0];

	for (uint32_t i = 1; i < n; i++) {
		double q = q0 + (sigma.count + xs[i].count) / S;
		if (q <= q_limit) {
			/* Fold xs[i] into sigma. Standard incremental
			 * weighted-mean update; numerically stable for the
			 * sample magnitudes we work with (nanoseconds). */
			double new_count = sigma.count + xs[i].count;
			sigma.mean += (xs[i].mean - sigma.mean) *
			    (xs[i].count / new_count);
			sigma.count = new_count;
		} else {
			xs[out_n++] = sigma;
			q0 += sigma.count / S;
			q_limit = k1_inv(k1(q0, compression) + 1.0,
			    compression);
			sigma = xs[i];
		}
	}
	xs[out_n++] = sigma;
	return out_n;
}

/*
 * CDF interpolation per section 2.9 (figures 3-5).
 *
 * Each centroid c_i covers a quantile range [W_left/N, W_right/N]; its
 * mean is placed at the midpoint W_left/N + c_i.count/(2N) (eq. (b)).
 * Adjacent centroids are linked by a linear segment of length
 * (c_i.count + c_{i+1}.count)/(2N).
 *
 * Case (d) special-cases the first and last cluster against the
 * observed min/max; singletons collapse to step interpolation
 * (Figure 4) because the side with count==1 has zero half-width.
 *
 * `xs` must be the centroid array of a fully merged digest (no
 * residual buffer), sorted by mean.
 */
static double cdf_interpolate(const td_centroid_t *xs, uint32_t n,
			      double total, double min_v, double max_v,
			      double q)
{
	if (n == 0)
		return 0.0;
	if (q <= 0.0)
		return min_v;
	if (q >= 1.0)
		return max_v;

	double target = q * total;
	const td_centroid_t *c0 = &xs[0];
	const td_centroid_t *cN = &xs[n - 1];

	if (n == 1) {
		if (c0->count <= 1.0)
			return c0->mean;
		double half = c0->count / 2.0;
		if (target < half)
			return min_v + (target / half) * (c0->mean - min_v);
		return c0->mean + ((target - half) / half) *
		    (max_v - c0->mean);
	}

	if (target < c0->count / 2.0) {
		if (c0->count <= 1.0)
			return c0->mean;
		double frac = target / (c0->count / 2.0);
		return min_v + frac * (c0->mean - min_v);
	}

	if (target > total - cN->count / 2.0) {
		if (cN->count <= 1.0)
			return cN->mean;
		double frac = (target - (total - cN->count / 2.0)) /
		    (cN->count / 2.0);
		return cN->mean + frac * (max_v - cN->mean);
	}

	double cum = c0->count / 2.0;
	for (uint32_t i = 0; i + 1 < n; i++) {
		const td_centroid_t *c1 = &xs[i];
		const td_centroid_t *c2 = &xs[i + 1];
		double width = (c1->count + c2->count) / 2.0;
		double next_cum = cum + width;
		if (target <= next_cum) {
			if (width <= 0.0)
				return c1->mean;
			double frac = (target - cum) / width;
			return c1->mean + frac * (c2->mean - c1->mean);
		}
		cum = next_cum;
	}
	return cN->mean;
}


/* ------------------------------------------------------------------ */
/* Single t-digest API                                                */
/* ------------------------------------------------------------------ */

int tdigest_init(tdigest_t *td, double compression)
{
	if (td == NULL || !(compression > 0.0))
		return -EINVAL;

	memset(td, 0, sizeof(*td));
	td->compression = compression;

	/* Worst-case centroid count per the paper is ceil(delta). One slot
	 * of slack lets the merge loop emit safely. */
	td->max_centroids = (uint32_t)ceil(compression) + 1u;
	td->max_buffered = TDIGEST_BUFFER_FACTOR * td->max_centroids;

	td->centroids = calloc(td->max_centroids, sizeof(*td->centroids));
	td->buffer = calloc(td->max_buffered, sizeof(*td->buffer));
	if (td->centroids == NULL || td->buffer == NULL) {
		free(td->centroids);
		free(td->buffer);
		td->centroids = NULL;
		td->buffer = NULL;
		return -ENOMEM;
	}

	td->min_seen = DBL_MAX;
	td->max_seen = -DBL_MAX;
	return 0;
}

void tdigest_fini(tdigest_t *td)
{
	if (td == NULL)
		return;
	free(td->centroids);
	free(td->buffer);
	td->centroids = NULL;
	td->buffer = NULL;
	td->n_centroids = 0;
	td->n_buffered = 0;
	td->total_count = 0.0;
}

void tdigest_reset(tdigest_t *td)
{
	td->n_centroids = 0;
	td->n_buffered = 0;
	td->total_count = 0.0;
	td->min_seen = DBL_MAX;
	td->max_seen = -DBL_MAX;
}

void tdigest_add(tdigest_t *td, double sample)
{
	if (sample < td->min_seen)
		td->min_seen = sample;
	if (sample > td->max_seen)
		td->max_seen = sample;

	td->buffer[td->n_buffered++] = sample;

	if (td->n_buffered >= td->max_buffered)
		tdigest_compress(td);
}

void tdigest_compress(tdigest_t *td)
{
	if (td->n_buffered == 0)
		return;

	uint32_t total = td->n_centroids + td->n_buffered;

	/* Stage existing centroids and buffered raw samples into one
	 * sorted array, then run Algorithm 1 in place. */
	td_centroid_t *xs = malloc(total * sizeof(*xs));
	if (xs == NULL) {
		/* Out of memory: drop the buffer rather than letting it
		 * grow unbounded. Older centroids stay usable. */
		td->n_buffered = 0;
		return;
	}

	uint32_t idx = 0;
	for (uint32_t i = 0; i < td->n_centroids; i++)
		xs[idx++] = td->centroids[i];
	for (uint32_t i = 0; i < td->n_buffered; i++) {
		xs[idx].mean = td->buffer[i];
		xs[idx].count = 1.0;
		idx++;
	}

	qsort(xs, total, sizeof(*xs), centroid_cmp);

	double S = td->total_count + (double)td->n_buffered;
	uint32_t out_n = cluster_merge_inplace(xs, total, S, td->compression);

	/* out_n is bounded by ceil(delta), which is <= max_centroids. */
	memcpy(td->centroids, xs, out_n * sizeof(*xs));
	td->n_centroids = out_n;
	td->n_buffered = 0;
	td->total_count = S;

	free(xs);
}

double tdigest_total_count(const tdigest_t *td)
{
	return td->total_count + (double)td->n_buffered;
}

double tdigest_quantile(tdigest_t *td, double q)
{
	if (td->n_buffered > 0)
		tdigest_compress(td);
	if (td->n_centroids == 0)
		return 0.0;

	return cdf_interpolate(td->centroids, td->n_centroids,
	    td->total_count, td->min_seen, td->max_seen, q);
}


/* ------------------------------------------------------------------ */
/* Sliding-window wrapper                                             */
/* ------------------------------------------------------------------ */

int wcet_sketch_init(wcet_sketch_t *s, uint32_t window_size,
		     double compression, double quantile)
{
	int ret;

	if (s == NULL || window_size < 2 || !(quantile > 0.0 && quantile < 1.0))
		return -EINVAL;

	memset(s, 0, sizeof(*s));
	s->window_size = window_size;
	s->rotate_at = window_size / 2u;
	if (s->rotate_at < 1u)
		s->rotate_at = 1u;
	s->quantile = quantile;
	s->compression = compression;

	ret = tdigest_init(&s->prev, compression);
	if (ret < 0)
		return ret;
	ret = tdigest_init(&s->cur, compression);
	if (ret < 0) {
		tdigest_fini(&s->prev);
		return ret;
	}
	return 0;
}

void wcet_sketch_fini(wcet_sketch_t *s)
{
	if (s == NULL)
		return;
	tdigest_fini(&s->prev);
	tdigest_fini(&s->cur);
	s->cur_count = 0;
}

void wcet_sketch_reset(wcet_sketch_t *s)
{
	tdigest_reset(&s->prev);
	tdigest_reset(&s->cur);
	s->cur_count = 0;
}

void wcet_sketch_add(wcet_sketch_t *s, double sample)
{
	tdigest_add(&s->cur, sample);
	s->cur_count++;

	if (s->cur_count >= s->rotate_at) {
		/* Rotate: discard prev, promote cur into prev, start a
		 * fresh cur. Swap the embedded tdigest structs so we keep
		 * their already-allocated arrays. */
		tdigest_t tmp = s->prev;
		s->prev = s->cur;
		s->cur = tmp;
		tdigest_reset(&s->cur);
		s->cur_count = 0;
	}
}

double wcet_sketch_quantile(wcet_sketch_t *s)
{
	if (wcet_sketch_count(s) == 0)
		return 0.0;

	/* Force both halves into fully-merged form so all samples live
	 * in centroids. */
	tdigest_compress(&s->prev);
	tdigest_compress(&s->cur);

	uint32_t total = s->prev.n_centroids + s->cur.n_centroids;
	if (total == 0)
		return 0.0;

	/* Two delta-bounded digests union to at most ~2*delta centroids.
	 * Allocate that much scratch and run Algorithm 1 once over the
	 * union -- t-digests are mergeable (paper section 2.5). */
	td_centroid_t *xs = malloc(total * sizeof(*xs));
	if (xs == NULL) {
		/* Memory pressure: fall back to whichever half has more
		 * samples. Conservative but consistent. */
		const tdigest_t *t =
		    tdigest_total_count(&s->prev) >=
		    tdigest_total_count(&s->cur)
		    ? &s->prev : &s->cur;
		return cdf_interpolate(t->centroids, t->n_centroids,
		    t->total_count, t->min_seen, t->max_seen, s->quantile);
	}

	uint32_t idx = 0;
	for (uint32_t i = 0; i < s->prev.n_centroids; i++)
		xs[idx++] = s->prev.centroids[i];
	for (uint32_t i = 0; i < s->cur.n_centroids; i++)
		xs[idx++] = s->cur.centroids[i];

	qsort(xs, total, sizeof(*xs), centroid_cmp);

	double S = s->prev.total_count + s->cur.total_count;
	uint32_t merged_n = cluster_merge_inplace(xs, total, S, s->compression);

	double min_v = s->prev.min_seen < s->cur.min_seen
	    ? s->prev.min_seen : s->cur.min_seen;
	double max_v = s->prev.max_seen > s->cur.max_seen
	    ? s->prev.max_seen : s->cur.max_seen;

	double q = cdf_interpolate(xs, merged_n, S, min_v, max_v, s->quantile);
	free(xs);
	return q;
}

uint32_t wcet_sketch_count(const wcet_sketch_t *s)
{
	double n = tdigest_total_count(&s->prev) + tdigest_total_count(&s->cur);
	if (n > (double)UINT32_MAX)
		return UINT32_MAX;
	return (uint32_t)n;
}
