/**
 * Copyright (c) 2013 Heng Li
 *
 * Notice: This work is under the MIT License; see 
 *         https://github.com/lh3/ropebwt and bcr.h 
 *         for license details (Accessed on August 22, 2013).
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "bcr-demo.h"

typedef struct {
	uint64_t u, v;
} pair64_t;

static const uint8_t **split_str(long Tlen, const uint8_t *T, long *n_)
{
	long n, max;
	uint8_t *p, *q;
	const uint8_t **P = 0, *end;
	for (p = q = (uint8_t*)T, end = T + Tlen, n = max = 0; p != end; ++p) {
		if (*p) continue;
		if (n == max) {
			max = max? max<<1 : 256;
			P = realloc(P, max * sizeof(void*));
		}
		P[n++] = q, q = p + 1;
	}
	P = realloc(P, (n + 1) * sizeof(void*));
	P[n] = q;
	*n_ = n;
	return P;
}

/**
 * Append $T to existing BWT $B.
 *
 * @param Blen    length of the existing BWT
 * @param B       existing BWT; set to NULL if non-existing
 * @param Tlen    length of input string
 * @param T       input string; '\0' represents a sentinel
 *
 * @return  the new BWT string
 */
uint8_t *bcr_lite(long Blen, uint8_t *B, long Tlen, const uint8_t *T)
{
	long i, k, n, n0;
	uint8_t *p, *q, *B0;
	const uint8_t *end, **P;
	pair64_t *a;
	int c;

	if (T == 0 || Tlen == 0) return B;
	// initialize
	P = split_str(Tlen, T, &n);
	for (p = B, end = B + Blen, i = 0; p < end; ++p) i += (*p == 0); // count # of sentinels
	a = malloc(sizeof(pair64_t) * n);
	for (k = 0; k < n; ++k) a[k].u = k + i, a[k].v = k<<8;
	B = realloc(B, Blen + Tlen);
	memmove(B + Tlen, B, Blen); // finished BWT is always placed at the end of $B
	B = B0 = B + Tlen;
	// core loop
	for (i = 0, n0 = n; n0; ++i) {
		long l, pre, ac[256], mc[256], mc2[256];
		pair64_t *b[256], *aa;
		for (c = 0; c != 256; ++c) mc[c] = mc2[c] = 0;
		end = B0 + Blen; Blen += n0; B -= n0;
		for (n = k = 0, p = B0, q = B, pre = 0; k < n0; ++k) {
			pair64_t *u = &a[k];
			c = P[(u->v>>8) + 1] - 2 - i >= P[u->v>>8]? *(P[(u->v>>8) + 1] - 2 - i) : 0; // symbol to insert
			u->v = (u->v&~0xffULL) | c;
			for (l = 0; l != u->u - pre; ++l) // copy ($u->u - $pre - 1) symbols from B0 to B
				++mc[*p], *q++ = *p++; // $mc: marginal counts of all processed symbols
			*q++ = c;
			pre = u->u + 1; u->u = mc[c]++;
			if (c) a[n++] = a[k], ++mc2[c]; // $mc2: marginal counts of the current column
		}
		while (p < end) ++mc[*p], *q++ = *p++; // copy the rest of $B0 to $B
		for (c = 1, ac[0] = 0; c != 256; ++c) ac[c] = ac[c-1] + mc[c-1]; // accumulative count
		for (k = 0; k < n; ++k) a[k].u += ac[a[k].v&0xff] + n; // compute positions for the next round
		//printf("===> %ld: '", i); for (k = 0; k < Blen-n0; ++k) putchar(B0[k]); printf("' <===\n"); for (k = 0; k < n0; ++k) printf("%lld\t%lld\n", a[k].v>>8, a[k].u);
		// stable counting sort ($a[k].v&0xff); also possible with an in-place non-stable radix sort, which is slower
		aa = malloc(sizeof(pair64_t) * n);
		for (c = 1, b[0] = aa; c != 256; ++c) b[c] = b[c-1] + mc2[c-1];
		for (k = 0; k < n; ++k) *b[a[k].v&0xff]++ = a[k]; // this works because $a is already partially sorted
		free(a); a = aa; // $aa now becomes $a
		B0 = B; n0 = n;
	}
	free(P); free(a);
	return B;
}

