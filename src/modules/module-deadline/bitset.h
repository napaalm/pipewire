/*-
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Paul Vixie.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef _BITSET_H
#define _BITSET_H

typedef	unsigned long bitset_t;

/* internal macros */
				/* word of the bitset bit is in */
#define	_bitset_word(bit) \
	((bit) >> 6)

				/* mask for the bit within its word */
#define	_bitset_mask(bit) \
	(1ul << ((bit)&0x3f))

/* external macros */
				/* words in a bitset of nbits bits */
#define	bitset_words(nbits) \
	(((nbits) + 63) >> 6)

				/* allocate a bitset */
#define	bitset_alloc(nbits) \
	(bitset_t *)calloc((size_t)bitset_words(nbits), sizeof(bitset_t))

				/* allocate a bitset on the stack */
#define	bitset_decl(name, nbits) \
	((name)[bitset_words(nbits)])

				/* is bit N of bitset name set? */
#define	bitset_test(name, bit) \
	((name)[_bitset_word(bit)] & _bitset_mask(bit))

				/* set bit N of bitset name */
#define	bitset_set(name, bit) \
	((name)[_bitset_word(bit)] |= _bitset_mask(bit))

				/* clear bit N of bitset name */
#define	bitset_clear(name, bit) \
	((name)[_bitset_word(bit)] &= ~_bitset_mask(bit))

				/* clear bits start ... stop in bitset */
#define	bitset_nclear(name, start, stop) do { \
	register bitset_t *_name = (name); \
	register int _start = (start), _stop = (stop); \
	register int _startword = _bitset_word(_start); \
	register int _stopword = _bitset_word(_stop); \
	if (_startword == _stopword) { \
		_name[_startword] &= ((-1ul >> (64 - (_start & 0x3f))) | \
				      (-1ul << ((_stop & 0x3f) + 1))); \
	} else { \
		_name[_startword] &= -1ul >> (64 - (_start & 0x3f)); \
		while (++_startword < _stopword) \
			_name[_startword] = 0; \
		_name[_stopword] &= -1ul << ((_stop & 0x3f) + 1); \
	} \
} while (0)

				/* set bits start ... stop in bitset */
#define	bitset_nset(name, start, stop) do { \
	register bitset_t *_name = (name); \
	register int _start = (start), _stop = (stop); \
	register int _startword = _bitset_word(_start); \
	register int _stopword = _bitset_word(_stop); \
	if (_startword == _stopword) { \
		_name[_startword] |= ((-1ul << (_start & 0x3f)) & \
				    (-1ul >> (63 - (_stop & 0x3f)))); \
	} else { \
		_name[_startword] |= -1ul << ((_start) & 0x3f); \
		while (++_startword < _stopword) \
	    		_name[_startword] = -1ul; \
		_name[_stopword] |= -1ul >> (63 - (_stop & 0x3f)); \
	} \
} while (0)

				/* find first bit clear in name */
#define	bitset_ffc(name, nbits, value) do { \
	register bitset_t *_name = (name); \
	register int _word, _nbits = (nbits); \
	register int _stopword = _bitset_word(_nbits - 1), _value = -1; \
	if (_nbits > 0) \
		for (_word = 0; _word <= _stopword; ++_word) \
			if (_name[_word] != -1ul) { \
				bitset_t _lb; \
				_value = _word << 6; \
				for (_lb = _name[_word]; (_lb & 1ul); \
				    ++_value, _lb >>= 1); \
				break; \
			} \
	if (_value >= nbits) \
		_value = -1; \
	*(value) = _value; \
} while (0)

				/* find first bit set in name */
#define	bitset_ffs(name, nbits, value) do { \
	register bitset_t *_name = (name); \
	register int _word, _nbits = (nbits); \
	register int _stopword = _bitset_word(_nbits - 1), _value = -1; \
	if (_nbits > 0) \
		for (_word = 0; _word <= _stopword; ++_word) \
			if (_name[_word]) { \
				bitset_t _lb; \
				_value = _word << 6; \
				for (_lb = _name[_word]; !(_lb & 1ul); \
				    ++_value, _lb >>= 1); \
				break; \
			} \
	if (_value >= nbits) \
		_value = -1; \
	*(value) = _value; \
} while (0)

#define bitset_cpy(a, b, nbits) \
	memcpy(a, b, bitset_words(nbits) * sizeof(bitset_t))

#define bitset_zero(a, nbits) \
	memset(a, 0, bitset_words(nbits) * sizeof(bitset_t))

// do NOT use as single body of e.g., if-else, for or while loops
#define bitset_decl_cpy(name, src, nbits)                \
	bitset_t bitset_decl(name, nbits);               \
	bitset_cpy(name, src, nbits);

// do NOT use as single body of if-else, or loop statements
#define bitset_decl_zero(name, nbits)                        \
	bitset_t bitset_decl(name, nbits);                   \
	for (int i = 0; i <= _bitset_word(nbits - 1); i++)   \
		(name)[i] = 0

static inline int bitset_empty(bitset_t *s, int nbits) {
	int first_set;
	bitset_ffs(s, nbits, &first_set);
	return first_set == -1;
}

static inline void bitset_or(bitset_t *dst, bitset_t *src, int nbits) {
	register int _stopword = _bitset_word(nbits - 1);
	for (int _word = 0; _word <= _stopword; _word++)
		dst[_word] |= src[_word];
}

static inline void bitset_andnot(bitset_t *dst, bitset_t *src, int nbits) {
	register int _stopword = _bitset_word(nbits - 1);
	for (int _word = 0; _word <= _stopword; _word++)
		dst[_word] &= ~src[_word];
}

// Check if set a includes set b
static inline int bitset_includes(bitset_t *a, bitset_t *b, int nbits) {
	register int _stopword = _bitset_word(nbits - 1);
	for (int _word = 0; _word <= _stopword; _word++)
		if ((a[_word] & b[_word]) != b[_word])
			return 0;
	return 1;
}

// return 1 if after call s contains the next valid subset, 0 if iteration ended
static inline int bitset_subset_next(bitset_t *s, int nbits) {
	for (; ; s++, nbits -= 64) {
		s[0]++;
		if (nbits == 64 && s[0] == 0) {
			return 0;
		} else if (nbits > 64 && s[0] == 0) {
			continue;
		} else if (s[0] == (1ul << nbits)) {
			return 0;
		}
		assert(s[0] != 0ul);
		return 1;
	}
}

static inline int bitset_next_set(bitset_t *s, int nbits, int after) {
	for (int bit = after + 1; bit < nbits; bit++) {
		if (bitset_test(s, bit))
			return bit;
	}
	return -1;
}

static inline char *bitset2str(bitset_t *a, int nbits) {
	static char buf[80];
	assert(nbits < 80);
	for (int i = 0; i < nbits; i++)
		buf[i] = bitset_test(a, i) ? '1' : '0';
	return buf;
}

#endif /* !_BITSET_H */
