/*
 * This is an in-memory implementation of the Bauer-Cox-Rosone (BCR) algorithms
 * for constructing BWT for multiple short strings. It differs the orginal BCR
 * in that this implementation: 1) keeps the partial BWTs in memory; 2)
 * supports partial multi-threading (in the sense that not every step is
 * parallelized) and 3) optionally sorts strings into reverse lexicographical
 * order (RLO) while constructing BWT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

int bcr_verbose = 2;

#ifndef kroundup32
#define kroundup32(x) (--(x), (x)|=(x)>>1, (x)|=(x)>>2, (x)|=(x)>>4, (x)|=(x)>>8, (x)|=(x)>>16, ++(x))
#endif

/**********************************************
 *** Lightweight run-length encoder/decoder ***
 **********************************************/

#define RLL_BLOCK_SIZE 0x100000

typedef struct {
	int c;
	int64_t l;
	uint8_t *q, **i;
} rllitr_t;

typedef struct {
	int n, m;
	uint8_t **z;
	int64_t l, mc[6];
} rll_t;

static rll_t *rll_init(void)
{
	rll_t *e;
	e = calloc(1, sizeof(rll_t));
	e->n = e->m = 1;
	e->z = malloc(sizeof(void*));
	e->z[0] = calloc(RLL_BLOCK_SIZE, 1);
	e->z[0][0] = 7;
	return e;
}

static void rll_destroy(rll_t *e)
{
	int i;
	if (e == 0) return;
	for (i = 0; i < e->n; ++i) free(e->z[i]);
	free(e->z); free(e);
}

static void rll_itr_init(const rll_t *e, rllitr_t *itr)
{
	itr->i = e->z; itr->q = *itr->i; itr->c = -1; itr->l = 0;
}

static inline void rll_enc0(rll_t *e, rllitr_t *itr, int l, uint8_t c)
{
	*itr->q++ = l<<3 | c;
	e->mc[c] += l;
	if (itr->q - *itr->i == RLL_BLOCK_SIZE) {
		if (e->n == e->m) {
			e->m <<= 1;
			e->z = realloc(e->z, e->m * sizeof(void*));
			memset(e->z + e->n, 0, (e->m - e->n) * sizeof(void*));
		}
		++e->n;
		itr->i = e->z + e->n - 1;
		itr->q = *itr->i = calloc(RLL_BLOCK_SIZE, 1);
	}
}

static inline void rll_enc(rll_t *e, rllitr_t *itr, int64_t l, uint8_t c)
{
	if (itr->c != c) {
		if (itr->l) {
			if (itr->l > 31) 
				for (; itr->l > 31; itr->l -= 31)
					rll_enc0(e, itr, 31, itr->c);
			rll_enc0(e, itr, itr->l, itr->c);
		}
		itr->l = l; itr->c = c;
	} else itr->l += l;
}

static void rll_enc_finalize(rll_t *e, rllitr_t *itr)
{
	int c;
	rll_enc(e, itr, 0, -1);
	*itr->q = 7; // end marker; there is always room for an extra symbol
	for (e->l = 0, c = 0; c < 6; ++c) e->l += e->mc[c];
}

static inline int64_t rll_dec(const rll_t *e, rllitr_t *itr, int *c, int is_free)
{
	int64_t l;
	if (*itr->q == 7) return -1;
	l = *itr->q>>3; *c = *itr->q&7;
	if (++itr->q - *itr->i == RLL_BLOCK_SIZE) {
		if (is_free) {
			free(*itr->i);
			*itr->i = 0;
		}
		itr->q = *++itr->i;
	}
	return l;
}

static inline void rll_copy(rll_t *e, rllitr_t *itr, const rll_t *e0, rllitr_t *itr0, int64_t k)
{
	if (itr0->l >= k) { // there are more pending symbols
		rll_enc(e, itr, k, itr0->c);
		itr0->l -= k; // l - k symbols remains
	} else { // use up all pending symbols
		int c = -1; // to please gcc
		int64_t l;
		rll_enc(e, itr, itr0->l, itr0->c); // write all pending symbols
		k -= itr0->l;
		for (; k > 0; k -= l) { // we always go into this loop because l0<k
			l = rll_dec(e0, itr0, &c, 1);
			rll_enc(e, itr, k < l? k : l, c);
		}
		itr0->l = -k; itr0->c = c;
	}
}

/*************************************************
 *** Data structure for long 2-bit encoded DNA ***
 *************************************************/

#define LD_SHIFT 20
#define LD_MASK  ((1U<<LD_SHIFT) - 1)

typedef struct {
	int max;
	uint64_t **a;
} longdna_t; // to allocate, simply call calloc()

void ld_destroy(longdna_t *ld)
{
	int j;
	for (j = 0; j < ld->max; ++j) free(ld->a[j]);
	free(ld->a); free(ld);
}

inline void ld_set(longdna_t *h, int64_t x, int c)
{
	int k = x >> LD_SHIFT, l = x & LD_MASK;
	if (k >= h->max) {
		int j, old_max = h->max;
		h->max = k + 1;
		kroundup32(h->max);
		h->a = realloc(h->a, sizeof(void*) * h->max);
		for (j = old_max; j < h->max; ++j) h->a[j] = 0;
	}
	if (h->a[k] == 0) h->a[k] = calloc(1<<LD_SHIFT>>5, 8);
	h->a[k][l>>5] |= (uint64_t)(c&3)<<((l&31)<<1); // NB: we cannot set the same position multiple times
}

inline int ld_get(longdna_t *h, int64_t x)
{
	return h->a[x>>LD_SHIFT][(x&LD_MASK)>>5]>>((x&31)<<1)&3;
}

void ld_dump(const longdna_t *ld, FILE *fp)
{
	int i, x, zero = 0;
	fwrite(&ld->max, sizeof(int), 1, fp);
	for (i = 0; i < ld->max; ++i)
		if (ld->a[i]) {
			x = 1<<LD_SHIFT>>5;
			fwrite(&x, sizeof(int), 1, fp);
			fwrite(ld->a[i], 8, 1<<LD_SHIFT>>5, fp);
		} else fwrite(&zero, sizeof(int), 1, fp);
}

longdna_t *ld_restore(FILE *fp)
{
	longdna_t *ld;
	int i, x;
	ld = calloc(1, sizeof(longdna_t));
	fread(&ld->max, sizeof(int), 1, fp);
	ld->a = calloc(ld->max, sizeof(void*));
	for (i = 0; i < ld->max; ++i) {
		fread(&x, sizeof(int), 1, fp);
		if (x) {
			ld->a[i] = malloc(x *8);
			fread(ld->a[i], 8, x, fp);
		}
	}
	return ld;
}

/******************
 *** Radix sort ***
 ******************/

typedef struct {
	uint64_t u, v; // $u: position in partial BWT; $v: seq_id:45, seq_len:16, base:3
} pair64_t;

#define rstype_t pair64_t
#define rskey(x) ((x).u)

#define RS_MIN_SIZE 64

typedef struct {
	rstype_t *b, *e;
} rsbucket_t;

void rs_sort(rstype_t *beg, rstype_t *end, int n_bits, int s)
{
	rstype_t *i;
	int size = 1<<n_bits, m = size - 1;
	rsbucket_t *k, b[size], *be = b + size;

	for (k = b; k != be; ++k) k->b = k->e = beg;
	for (i = beg; i != end; ++i) ++b[rskey(*i)>>s&m].e; // count radix
	for (k = b + 1; k != be; ++k) // set start and end of each bucket
		k->e += (k-1)->e - beg, k->b = (k-1)->e;
	for (k = b; k != be;) { // in-place classification based on radix
		if (k->b != k->e) { // the bucket is not full
			rsbucket_t *l;
			if ((l = b + (rskey(*k->b)>>s&m)) != k) { // destination different
				rstype_t tmp = *k->b, swap;
				do { // swap until we find an element in $k
					swap = tmp; tmp = *l->b; *l->b++ = swap;
					l = b + (rskey(tmp)>>s&m);
				} while (l != k);
				*k->b++ = tmp;
			} else ++k->b;
		} else ++k;
	}
	for (b->b = beg, k = b + 1; k != be; ++k) k->b = (k-1)->e; // reset k->b
	if (s) { // if $s is non-zero, we need to sort buckets
		s = s > n_bits? s - n_bits : 0;
		for (k = b; k != be; ++k)
			if (k->e - k->b > RS_MIN_SIZE) rs_sort(k->b, k->e, n_bits, s);
			else if (k->e - k->b > 1) // then use an insertion sort
				for (i = k->b + 1; i < k->e; ++i)
					if (rskey(*i) < rskey(*(i - 1))) {
						rstype_t *j, tmp = *i;
						for (j = i; j > k->b && rskey(tmp) < rskey(*(j-1)); --j)
							*j = *(j - 1);
						*j = tmp;
					}
	}
}

/******************************
 *** Classify pair64_t::v&7 ***
 ******************************/

void rs_classify_alt(rstype_t *beg, rstype_t *end, int64_t *ac) // very similar to the first half of rs_sort()
{
	rsbucket_t *k, b[8], *be = b + 8;
	for (k = b; k != be; ++k) k->b = beg + ac[k - b];
	for (k = b; k != be - 1; ++k) k->e = k[1].b;
	k->e = end;
	for (k = b; k != be;) {
		if (k->b != k->e) {
			rsbucket_t *l;
			if ((l = b + ((*k->b).v&7)) != k) {
				rstype_t tmp = *k->b, swap;
				do {
					swap = tmp; tmp = *l->b; *l->b++ = swap;
					l = b + (tmp.v&7);
				} while (l != k);
				*k->b++ = tmp;
			} else ++k->b;
		} else ++k;
	}
}

/************************
 *** System utilities ***
 ************************/

#include <sys/time.h>
#include <sys/resource.h>

static void bcr_gettime(double *rt, double *ct)
{
	struct rusage r;
	struct timeval tp;
	struct timezone tzp;
	getrusage(RUSAGE_SELF, &r);
	gettimeofday(&tp, &tzp);
	*ct = r.ru_utime.tv_sec + r.ru_stime.tv_sec + 1e-6 * (r.ru_utime.tv_usec + r.ru_stime.tv_usec);
	*rt = tp.tv_sec + tp.tv_usec * 1e-6;
}

/***********
 *** BCR ***
 ***********/

#include <pthread.h>
#include "bcr.h"

typedef struct {
	rll_t *e;
	int64_t n, c[6];
	pair64_t *a;
} bucket_t;

typedef struct {
	struct bcr_s *bcr;
	int class, pos;
	volatile int toproc;
} worker_t;

struct bcr_s {
	int max_len, n_threads, flag;
	uint64_t n_seqs, m_seqs, c[6], tot;
	uint16_t *len;
	longdna_t **seq;
	bucket_t bwt[6];
	volatile int proc_cnt; // for multi-threading
	double rt0, ct0; // for timing
};

typedef struct {
	double rt, ct;
	size_t mem;
} bcrstat_t;

bcr_t *bcr_init()
{
	bcr_t *b;
	int i;
	b = calloc(1, sizeof(bcr_t));
	bcr_gettime(&b->rt0, &b->ct0);
	for (i = 0; i < 6; ++i) b->bwt[i].e = rll_init();
	return b;
}

void bcr_destroy(bcr_t *b)
{
	int i;
	for (i = 0; i < 6; ++i) rll_destroy(b->bwt[i].e);
	free(b->len); free(b->seq);
	free(b);
}

size_t bcr_bwtmem(const bcr_t *b)
{
	int i;
	size_t mem = 0;
	for (i = 0; i < 6; ++i)
		mem += (size_t)b->bwt[i].e->n * RLL_BLOCK_SIZE;
	return mem;
}

void bcr_append(bcr_t *b, int len, const uint8_t *seq) // add a sequence
{
	int i;
	assert(len >= 1 && len < 65536);
	if (len > b->max_len) { // find a longer read
		b->seq = realloc(b->seq, len * sizeof(void*));
		for (i = b->max_len; i < len; ++i)
			b->seq[i] = calloc(1, sizeof(longdna_t));
		b->max_len = len;
	}
	if (b->n_seqs == b->m_seqs) {
		b->m_seqs = b->m_seqs? b->m_seqs<<1 : 256;
		b->len = realloc(b->len, b->m_seqs * 2);
	}
	b->len[b->n_seqs] = len;
	for (i = 0; i < len; ++i)
		ld_set(b->seq[i], b->n_seqs, seq[len - 1 - i] - 1);
	++b->n_seqs;
}

static pair64_t *set_bwt(bcr_t *bcr, pair64_t *a, int pos)
{
	int64_t k, c[8], ac[8], m;
	int j, l;
	// compute the absolute position in the new BWT
	memset(c, 0, 64);
	if (pos == 0) { // we are going to insert the last column (the first insertion)
		if (!(bcr->flag & BCR_F_RLO)) {
			for (k = 0; k < bcr->n_seqs; ++k) {
				pair64_t *u = &a[k];
				u->u += c[u->v&7]++;
			}
		} else c[0] = bcr->n_seqs; // with RLO, we are going to break ties later in next_bwt()
	} else { // a[k].u computed in next_bwt() doesn't consider symbols inserted to other buckets. We need to fix this in the following code block.
		int b;
		for (b = m = 0; b < 6; ++b) { // loop through each bucket
			int64_t pc[8]; // partial counts
			bucket_t *bwt = &bcr->bwt[b]; // the bucket
			memcpy(pc, c, 64); // the accumulated counts prior to the current bucket
			for (k = 0; k < bwt->n; ++k) {
				pair64_t *u = &bwt->a[k];
				if ((u->v&7) == 0) continue; // come to the beginning of a string; no need to consider it in the next round
				u->u += pc[u->v&7]; // correct for symbols inserted to other buckets
				++c[u->v&7];
				if (m == u - a) ++m;
				else a[m++] = *u;
			}
		}
		// if (bcr->n_seqs > m) a = realloc(a, m * sizeof(pair64_t));
		bcr->n_seqs = m; // $m is the new size of the $a array
	}
	for (k = 1, ac[0] = 0; k < 8; ++k) ac[k] = ac[k - 1] + c[k - 1]; // accumulative counts; NB: MUST BE "8"; otherwise rs_classify_alt() will fail
	for (k = 0; k < bcr->n_seqs; ++k) a[k].u += ac[a[k].v&7];
	// classify into each bucket
	if (bcr->flag & BCR_F_FAST) {
		pair64_t *aa, *b[8];
		aa = malloc(bcr->n_seqs * sizeof(pair64_t));
		for (k = 0; k < 8; ++k) b[k] = &aa[ac[k]];
		for (k = 0; k < bcr->n_seqs; ++k) *b[a[k].v&7]++ = a[k];
		free(a); a = aa;
	} else rs_classify_alt(a, a + bcr->n_seqs, ac); // This is a bottleneck.
	for (j = 0; j < 6; ++j) bcr->bwt[j].a = a + ac[j];
	// update counts: $bcr->bwt[j].c[l] equals the number of symbol $l prior to bucket $j; needed by next_bwt()
	for (l = 0; l < 6; ++l)
		for (j = 1, bcr->bwt[0].c[l] = 0; j < 6; ++j)
			bcr->bwt[j].c[l] = bcr->bwt[j-1].c[l] + bcr->bwt[j-1].e->mc[l];
	for (j = 0; j < 6; ++j) bcr->bwt[j].n = c[j], bcr->c[j] += ac[j];
	bcr->tot += bcr->n_seqs;
	return a;
}

static void sort_alt(pair64_t *beg, pair64_t *end) // sort [beg,end) according to ->v&7; ->u should all be equal
{
	pair64_t *i;
	if (end - beg < 64) { // insertion sort, which is faster for small arrays
		for (i = beg + 1; i < end; ++i)
			if ((i->v&7) < ((i-1)->v&7)) {
				pair64_t *j, tmp = *i;
				for (j = i; j > beg && (tmp.v&7) < ((j-1)->v&7); --j)
					*j = *(j - 1);
				*j = tmp;
			}
	} else { // radix sort, which is faster for large arrays
		int64_t ac[8], c[8];
		int a;
		for (a = 0; a < 8; ++a) a[c] = 0;
		for (i = beg; i != end; ++i) ++c[i->v&7];
		for (a = 1, ac[0] = 0; a < 8; ++a) ac[a] = ac[a - 1] + c[a - 1];
		rs_classify_alt(beg, end, ac); // TODO: in the fast mode, we can use counting sort, which is faster but uses more RAM
	}
	if (end - beg > 1) { // update ->u: break ties
		pair64_t *u;
		for (i = beg + 1, u = beg; i < end; ++i)
			if ((u->v&7) == (i->v&7)) i->u = u->u;
			else i->u += i - beg, u = i;
	}
}

/* The following function can be written without differentiating the RLO and
 * non-RLO modes, by sorting the bcr->bwt[class].a array by (.u<<3 | (.v&0x7)).
 * This will merge rs_sort() and sort_alt().
 */
static void next_bwt(bcr_t *bcr, int class, int pos)
{
	int64_t k, l;
	rllitr_t ir, iw;
	bucket_t *bwt = &bcr->bwt[class];
	rll_t *ew, *er = bwt->e;

	if (bwt->n == 0) return;
	for (k = bcr->tot, l = 0; k; k >>= 1, ++l);
	if (class && !(bcr->flag & BCR_F_FAST))
		rs_sort(bwt->a, bwt->a + bwt->n, 8, l > 7? l - 7 : 0); // sort by the absolute position in the new BWT
	for (k = 0; k < bwt->n; ++k) { // compute the relative position in the old bucket
		pair64_t *u = &bwt->a[k];
		u->v = (u->v&~7ULL) | (pos >= (u->v>>3&0xffff)? 0 : ld_get(bcr->seq[pos], u->v>>19) + 1); // set the base to insert
		u->u -= bcr->c[class]; // the relative position in the old bucket
	}
	if (bcr->flag & BCR_F_RLO) { // counting sort symbols inserted to the same position; in the non-RLO mode, the following still works, but not necessary
		int64_t beg;
		for (k = 1, beg = 0; k <= bwt->n; ++k)
			if (k == bwt->n || bwt->a[k].u != bwt->a[k-1].u) {
				sort_alt(&bwt->a[beg], &bwt->a[k]);
				beg = k;
			}
	}
	// insert the column to the existing BWT $er and write to $ew; $er will be destroyed gradually
	ew = rll_init();
	rll_itr_init(er, &ir);
	rll_itr_init(ew, &iw);
	if (bcr->flag & BCR_F_RLO) { // in the RLO mode, if a[k].u==a[l].u, the updated .u must be equal
		int64_t old_u, new_u, streak;
		for (k = l = 0, streak = 0, old_u = new_u = -1; k < bwt->n; ++k) {
			pair64_t *u = &bwt->a[k];
			int a = u->v&7;
			if (u->u == old_u) ++streak;
			else streak = 0;
			if (u->u + streak > l) rll_copy(ew, &iw, er, &ir, u->u + streak - l); // copy u->u + streak - l symbols from the old BWT to the new
			rll_enc(ew, &iw, 1, a); // write the current symbol in the new column
			l = u->u + streak + 1;
			if (u->u != old_u) {
				old_u = u->u;
				new_u = u->u = (ew->mc[a] + iw.l - 1) + bcr->c[a] + bwt->c[a]; // compute the incomplete position in the new BWT
			} else u->u = new_u;
		}
	} else { // in the non-RLO mode, every ->u is different; the following is a special case of the code block above, but should be a bit faster
		for (k = l = 0; k < bwt->n; ++k) {
			pair64_t *u = &bwt->a[k];
			int a = u->v&7;
			if (u->u > l) rll_copy(ew, &iw, er, &ir, u->u - l);
			l = u->u + 1;
			rll_enc(ew, &iw, 1, a);
			u->u = (ew->mc[a] + iw.l - 1) + bcr->c[a] + bwt->c[a]; // compute the incomplete position in the new BWT
		}
	}
	if (l - bwt->n < er->l) rll_copy(ew, &iw, er, &ir, er->l - (l - bwt->n));
	rll_enc_finalize(ew, &iw);
	rll_destroy(er);
	bwt->e = ew;
}

static int worker_aux(worker_t *w)
{
	struct timespec req, rem;
	req.tv_sec = 0; req.tv_nsec = 1000000;
	while (!__sync_bool_compare_and_swap(&w->toproc, 1, 0)) nanosleep(&req, &rem);
	next_bwt(w->bcr, w->class, w->pos);
	__sync_add_and_fetch(&w->bcr->proc_cnt, 1);
	return (w->bcr->max_len == w->pos);
}

static void *worker(void *data) { while (worker_aux(data) == 0); return 0; }

void bcr_build(bcr_t *b, int flag, const char *tmpfn)
{
	int64_t k;
	int pos, c, i, n_threads;
	pair64_t *a;
	FILE *tmpfp = 0;
	double ct, rt;
	pthread_t *tid = 0;
	worker_t *w = 0;

	b->flag = flag;
	n_threads = (flag&BCR_F_THR)? 4 : 1;
	bcr_gettime(&rt, &ct);
	if (bcr_verbose >= 3) fprintf(stderr, "Read sequences into memory (%.3fs, %.3fs, %.3fM)\n", rt-b->rt0, ct-b->ct0, bcr_bwtmem(b)/1024./1024.);
	b->m_seqs = b->n_seqs;
	b->len = realloc(b->len, b->n_seqs * 2);
	if (tmpfn) { // dump the transposed sequences to a temporary file
		tmpfp = fopen(tmpfn, "wb");
		for (pos = 0; pos < b->max_len; ++pos) {
			ld_dump(b->seq[pos], tmpfp);
			ld_destroy(b->seq[pos]);
		}
		fclose(tmpfp);
		tmpfp = fopen(tmpfn, "rb");
		bcr_gettime(&rt, &ct);
		if (bcr_verbose >= 3) fprintf(stderr, "Saved sequences to the temporary file (%.3fs, %.3fs, %.3fM)\n", rt-b->rt0, ct-b->ct0, bcr_bwtmem(b)/1024./1024.);
	}
	if (n_threads > 1) {
		tid = alloca(n_threads * sizeof(pthread_t)); // tid[0] is not used, as the worker 0 is launched by the master
		w = alloca(n_threads * sizeof(worker_t));
		memset(w, 0, n_threads * sizeof(worker_t));
		for (i = 0; i < n_threads; ++i) w[i].class = i + 1, w[i].bcr = b;
		for (i = 1; i < n_threads; ++i) pthread_create(&tid[i], 0, worker, &w[i]);
	}
	a = malloc(b->n_seqs * 16);
	for (k = 0; k < b->n_seqs; ++k) a[k].u = 0, a[k].v = k<<19|b->len[k]<<3; // keep the sequence lengths in the $a array: reduce memory and cache misses
	free(b->len); b->len = 0; // we do not need $b->len now
	for (pos = 0; pos <= b->max_len; ++pos) { // "==" to add the sentinels
		a = set_bwt(b, a, pos);
		if (pos != b->max_len && tmpfp) b->seq[pos] = ld_restore(tmpfp);
		if (pos) {
			if (n_threads > 1) {
				for (c = 0; c < n_threads; ++c) {
					volatile int *p = &w[c].toproc;
					w[c].pos = pos;
					while (!__sync_bool_compare_and_swap(p, 0, 1));
				}
				worker_aux(&w[0]);
				while (!__sync_bool_compare_and_swap(&b->proc_cnt, n_threads, 0));
			} else for (c = 1; c <= 4; ++c) next_bwt(b, c, pos);
		} else next_bwt(b, 0, pos);
		if (pos != b->max_len) ld_destroy(b->seq[pos]);
		bcr_gettime(&rt, &ct);
		if (bcr_verbose >= 3) fprintf(stderr, "Finished cycle %d (%.3fs, %.3fs, %.3fM)\n", pos, rt-b->rt0, ct-b->ct0, bcr_bwtmem(b)/1024./1024.);
	}
	free(a);
	if (tmpfp) {
		fclose(tmpfp);
		unlink(tmpfn);
	}
	for (i = 1; i < n_threads; ++i) pthread_join(tid[i], 0);
}

/****************
 *** Iterator ***
 ****************/

struct bcritr_s {
	const bcr_t *b;
	int c, i;
};

bcritr_t *bcr_itr_init(const bcr_t *b)
{
	bcritr_t *itr;
	itr = calloc(1, sizeof(bcritr_t));
	itr->b = b; itr->i = -1;
	return itr;
}

const uint8_t *bcr_itr_next(bcritr_t *itr, int *l)
{
	rll_t *e;
	const uint8_t *s;
	if (itr->c == 6) return 0;
	++itr->i;
	if (itr->i == itr->b->bwt[itr->c].e->n) {
		if (++itr->c == 6) return 0;
		itr->i = 0;
	}
	e = itr->b->bwt[itr->c].e;
	s = e->z[itr->i];
	if (itr->i == e->n - 1) {
		for (*l = 0; *l < RLL_BLOCK_SIZE; ++*l)
			if (s[*l] == 7) break;
	} else *l = RLL_BLOCK_SIZE;
	return s;
}
