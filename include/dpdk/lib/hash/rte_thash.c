/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#include <stdalign.h>
#include <sys/queue.h>

#include <rte_thash.h>
#include <rte_tailq.h>
#include <rte_random.h>
#include <rte_memcpy.h>
#include <rte_errno.h>
#include <rte_eal_memconfig.h>
#include <rte_log.h>
#include <rte_malloc.h>

RTE_LOG_REGISTER_SUFFIX(thash_logtype, thash, INFO);
#define RTE_LOGTYPE_HASH thash_logtype
#define HASH_LOG(level, ...) \
	RTE_LOG_LINE(level, HASH, "" __VA_ARGS__)

#define THASH_NAME_LEN		64
#define TOEPLITZ_HASH_LEN	32

#define RETA_SZ_IN_RANGE(reta_sz)	((reta_sz >= RTE_THASH_RETA_SZ_MIN) &&\
					(reta_sz <= RTE_THASH_RETA_SZ_MAX))

TAILQ_HEAD(rte_thash_list, rte_tailq_entry);
static struct rte_tailq_elem rte_thash_tailq = {
	.name = "RTE_THASH",
};
EAL_REGISTER_TAILQ(rte_thash_tailq)

struct thash_lfsr {
	uint32_t	ref_cnt;
	uint32_t	poly;
	/**< polynomial associated with the lfsr */
	uint32_t	rev_poly;
	/**< polynomial to generate the sequence in reverse direction */
	uint32_t	state;
	/**< current state of the lfsr */
	uint32_t	rev_state;
	/**< current state of the lfsr for reverse direction */
	uint32_t	deg;	/**< polynomial degree*/
	uint32_t	bits_cnt;  /**< number of bits generated by lfsr*/
};

struct rte_thash_subtuple_helper {
	char	name[THASH_NAME_LEN];	/** < Name of subtuple configuration */
	LIST_ENTRY(rte_thash_subtuple_helper)	next;
	struct thash_lfsr	*lfsr;
	uint32_t	offset;		/** < Offset of the m-sequence */
	uint32_t	len;		/** < Length of the m-sequence */
	uint32_t	tuple_offset;	/** < Offset in bits of the subtuple */
	uint32_t	tuple_len;	/** < Length in bits of the subtuple */
	uint32_t	lsb_msk;	/** < (1 << reta_sz_log) - 1 */
	alignas(RTE_CACHE_LINE_SIZE) uint32_t	compl_table[];
	/** < Complementary table */
};

struct rte_thash_ctx {
	char		name[THASH_NAME_LEN];
	LIST_HEAD(, rte_thash_subtuple_helper) head;
	uint32_t	key_len;	/** < Length of the NIC RSS hash key */
	uint32_t	reta_sz_log;	/** < size of the RSS ReTa in bits */
	uint32_t	subtuples_nb;	/** < number of subtuples */
	uint32_t	flags;
	uint64_t	*matrices;
	/**< matrices used with rte_thash_gfni implementation */
	uint8_t		hash_key[];
};

int
rte_thash_gfni_supported(void)
{
#ifdef RTE_THASH_GFNI_DEFINED
	if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_GFNI) &&
			(rte_vect_get_max_simd_bitwidth() >=
			RTE_VECT_SIMD_512))
		return 1;
#endif

	return 0;
};

void
rte_thash_complete_matrix(uint64_t *matrixes, const uint8_t *rss_key, int size)
{
	int i, j;
	uint8_t *m = (uint8_t *)matrixes;
	uint8_t left_part, right_part;

	for (i = 0; i < size; i++) {
		for (j = 0; j < 8; j++) {
			left_part = rss_key[i] << j;
			right_part = (uint16_t)(rss_key[(i + 1) % size]) >>
				(8 - j);
			m[i * 8 + j] = left_part|right_part;
		}
	}
}

static inline uint32_t
get_bit_lfsr(struct thash_lfsr *lfsr)
{
	uint32_t bit, ret;

	/*
	 * masking the TAP bits defined by the polynomial and
	 * calculating parity
	 */
	bit = rte_popcount32(lfsr->state & lfsr->poly) & 0x1;
	ret = lfsr->state & 0x1;
	lfsr->state = ((lfsr->state >> 1) | (bit << (lfsr->deg - 1))) &
		((1 << lfsr->deg) - 1);

	lfsr->bits_cnt++;
	return ret;
}

static inline uint32_t
get_rev_bit_lfsr(struct thash_lfsr *lfsr)
{
	uint32_t bit, ret;

	bit = rte_popcount32(lfsr->rev_state & lfsr->rev_poly) & 0x1;
	ret = lfsr->rev_state & (1 << (lfsr->deg - 1));
	lfsr->rev_state = ((lfsr->rev_state << 1) | bit) &
		((1 << lfsr->deg) - 1);

	lfsr->bits_cnt++;
	return ret;
}

static inline uint32_t
get_rev_poly(uint32_t poly, int degree)
{
	int i;
	/*
	 * The implicit highest coefficient of the polynomial
	 * becomes the lowest after reversal.
	 */
	uint32_t rev_poly = 1;
	uint32_t mask = (1 << degree) - 1;

	/*
	 * Here we assume "poly" argument is an irreducible polynomial,
	 * thus the lowest coefficient of the "poly" must always be equal to "1".
	 * After the reversal, this the lowest coefficient becomes the highest and
	 * it is omitted since the highest coefficient is implicitly determined by
	 * degree of the polynomial.
	 */
	for (i = 1; i < degree; i++)
		rev_poly |= ((poly >> i) & 0x1) << (degree - i);

	return rev_poly & mask;
}

static struct thash_lfsr *
alloc_lfsr(uint32_t poly_degree)
{
	struct thash_lfsr *lfsr;
	uint32_t i;

	if ((poly_degree > 32) || (poly_degree == 0))
		return NULL;

	lfsr = rte_zmalloc(NULL, sizeof(struct thash_lfsr), 0);
	if (lfsr == NULL)
		return NULL;

	lfsr->deg = poly_degree;
	lfsr->poly = thash_get_rand_poly(lfsr->deg);
	do {
		lfsr->state = rte_rand() & ((1 << lfsr->deg) - 1);
	} while (lfsr->state == 0);
	/* init reverse order polynomial */
	lfsr->rev_poly = get_rev_poly(lfsr->poly, lfsr->deg);
	/* init proper rev_state*/
	lfsr->rev_state = lfsr->state;
	for (i = 0; i <= lfsr->deg; i++)
		get_rev_bit_lfsr(lfsr);

	/* clear bits_cnt after rev_state was inited */
	lfsr->bits_cnt = 0;
	lfsr->ref_cnt = 1;

	return lfsr;
}

static void
attach_lfsr(struct rte_thash_subtuple_helper *h, struct thash_lfsr *lfsr)
{
	lfsr->ref_cnt++;
	h->lfsr = lfsr;
}

static void
free_lfsr(struct thash_lfsr *lfsr)
{
	lfsr->ref_cnt--;
	if (lfsr->ref_cnt == 0)
		rte_free(lfsr);
}

struct rte_thash_ctx *
rte_thash_init_ctx(const char *name, uint32_t key_len, uint32_t reta_sz,
	uint8_t *key, uint32_t flags)
{
	struct rte_thash_ctx *ctx;
	struct rte_tailq_entry *te;
	struct rte_thash_list *thash_list;
	uint32_t i;

	if ((name == NULL) || (key_len == 0) || !RETA_SZ_IN_RANGE(reta_sz)) {
		rte_errno = EINVAL;
		return NULL;
	}

	thash_list = RTE_TAILQ_CAST(rte_thash_tailq.head, rte_thash_list);

	rte_mcfg_tailq_write_lock();

	/* guarantee there's no existing */
	TAILQ_FOREACH(te, thash_list, next) {
		ctx = (struct rte_thash_ctx *)te->data;
		if (strncmp(name, ctx->name, sizeof(ctx->name)) == 0)
			break;
	}
	ctx = NULL;
	if (te != NULL) {
		rte_errno = EEXIST;
		goto exit;
	}

	/* allocate tailq entry */
	te = rte_zmalloc("THASH_TAILQ_ENTRY", sizeof(*te), 0);
	if (te == NULL) {
		HASH_LOG(ERR,
			"Can not allocate tailq entry for thash context %s",
			name);
		rte_errno = ENOMEM;
		goto exit;
	}

	ctx = rte_zmalloc(NULL, sizeof(struct rte_thash_ctx) + key_len, 0);
	if (ctx == NULL) {
		HASH_LOG(ERR, "thash ctx %s memory allocation failed",
			name);
		rte_errno = ENOMEM;
		goto free_te;
	}

	rte_strlcpy(ctx->name, name, sizeof(ctx->name));
	ctx->key_len = key_len;
	ctx->reta_sz_log = reta_sz;
	LIST_INIT(&ctx->head);
	ctx->flags = flags;

	if (key)
		rte_memcpy(ctx->hash_key, key, key_len);
	else {
		for (i = 0; i < key_len; i++)
			ctx->hash_key[i] = rte_rand();
	}

	if (rte_thash_gfni_supported()) {
		ctx->matrices = rte_zmalloc(NULL, key_len * sizeof(uint64_t),
			RTE_CACHE_LINE_SIZE);
		if (ctx->matrices == NULL) {
			HASH_LOG(ERR, "Cannot allocate matrices");
			rte_errno = ENOMEM;
			goto free_ctx;
		}

		rte_thash_complete_matrix(ctx->matrices, ctx->hash_key,
			key_len);
	}

	te->data = (void *)ctx;
	TAILQ_INSERT_TAIL(thash_list, te, next);

	rte_mcfg_tailq_write_unlock();

	return ctx;

free_ctx:
	rte_free(ctx);
free_te:
	rte_free(te);
exit:
	rte_mcfg_tailq_write_unlock();
	return NULL;
}

struct rte_thash_ctx *
rte_thash_find_existing(const char *name)
{
	struct rte_thash_ctx *ctx;
	struct rte_tailq_entry *te;
	struct rte_thash_list *thash_list;

	thash_list = RTE_TAILQ_CAST(rte_thash_tailq.head, rte_thash_list);

	rte_mcfg_tailq_read_lock();
	TAILQ_FOREACH(te, thash_list, next) {
		ctx = (struct rte_thash_ctx *)te->data;
		if (strncmp(name, ctx->name, sizeof(ctx->name)) == 0)
			break;
	}

	rte_mcfg_tailq_read_unlock();

	if (te == NULL) {
		rte_errno = ENOENT;
		return NULL;
	}

	return ctx;
}

void
rte_thash_free_ctx(struct rte_thash_ctx *ctx)
{
	struct rte_tailq_entry *te;
	struct rte_thash_list *thash_list;
	struct rte_thash_subtuple_helper *ent, *tmp;

	if (ctx == NULL)
		return;

	thash_list = RTE_TAILQ_CAST(rte_thash_tailq.head, rte_thash_list);
	rte_mcfg_tailq_write_lock();
	TAILQ_FOREACH(te, thash_list, next) {
		if (te->data == (void *)ctx)
			break;
	}

	if (te != NULL)
		TAILQ_REMOVE(thash_list, te, next);

	rte_mcfg_tailq_write_unlock();
	ent = LIST_FIRST(&(ctx->head));
	while (ent) {
		free_lfsr(ent->lfsr);
		tmp = ent;
		ent = LIST_NEXT(ent, next);
		LIST_REMOVE(tmp, next);
		rte_free(tmp);
	}

	rte_free(ctx);
	rte_free(te);
}

static inline void
set_bit(uint8_t *ptr, uint32_t bit, uint32_t pos)
{
	uint32_t byte_idx = pos / CHAR_BIT;
	/* index of the bit int byte, indexing starts from MSB */
	uint32_t bit_idx = (CHAR_BIT - 1) - (pos & (CHAR_BIT - 1));
	uint8_t tmp;

	tmp = ptr[byte_idx];
	tmp &= ~(1 << bit_idx);
	tmp |= bit << bit_idx;
	ptr[byte_idx] = tmp;
}

/**
 * writes m-sequence to the hash_key for range [start, end]
 * (i.e. including start and end positions)
 */
static int
generate_subkey(struct rte_thash_ctx *ctx, struct thash_lfsr *lfsr,
	uint32_t start, uint32_t end)
{
	uint32_t i;
	uint32_t req_bits = (start < end) ? (end - start) : (start - end);
	req_bits++; /* due to including end */

	/* check if lfsr overflow period of the m-sequence */
	if (((lfsr->bits_cnt + req_bits) > (1ULL << lfsr->deg) - 1) &&
			((ctx->flags & RTE_THASH_IGNORE_PERIOD_OVERFLOW) !=
			RTE_THASH_IGNORE_PERIOD_OVERFLOW)) {
		HASH_LOG(ERR,
			"Can't generate m-sequence due to period overflow");
		return -ENOSPC;
	}

	if (start < end) {
		/* original direction (from left to right)*/
		for (i = start; i <= end; i++)
			set_bit(ctx->hash_key, get_bit_lfsr(lfsr), i);

	} else {
		/* reverse direction (from right to left) */
		for (i = end; i >= start; i--)
			set_bit(ctx->hash_key, get_rev_bit_lfsr(lfsr), i);
	}

	if (ctx->matrices != NULL)
		rte_thash_complete_matrix(ctx->matrices, ctx->hash_key,
			ctx->key_len);

	return 0;
}

static inline uint32_t
get_subvalue(struct rte_thash_ctx *ctx, uint32_t offset)
{
	uint32_t *tmp, val;

	tmp = (uint32_t *)(&ctx->hash_key[offset >> 3]);
	val = rte_be_to_cpu_32(*tmp);
	val >>= (TOEPLITZ_HASH_LEN - ((offset & (CHAR_BIT - 1)) +
		ctx->reta_sz_log));

	return val & ((1 << ctx->reta_sz_log) - 1);
}

static inline void
generate_complement_table(struct rte_thash_ctx *ctx,
	struct rte_thash_subtuple_helper *h)
{
	int i, j, k;
	uint32_t val;
	uint32_t start;

	start = h->offset + h->len - (2 * ctx->reta_sz_log - 1);

	for (i = 1; i < (1 << ctx->reta_sz_log); i++) {
		val = 0;
		for (j = i; j; j &= (j - 1)) {
			k = rte_bsf32(j);
			val ^= get_subvalue(ctx, start - k +
				ctx->reta_sz_log - 1);
		}
		h->compl_table[val] = i;
	}
}

static inline int
insert_before(struct rte_thash_ctx *ctx,
	struct rte_thash_subtuple_helper *ent,
	struct rte_thash_subtuple_helper *cur_ent,
	struct rte_thash_subtuple_helper *next_ent,
	uint32_t start, uint32_t end, uint32_t range_end)
{
	int ret;

	if (end < cur_ent->offset) {
		ent->lfsr = alloc_lfsr(ctx->reta_sz_log);
		if (ent->lfsr == NULL) {
			rte_free(ent);
			return -ENOMEM;
		}
		/* generate nonoverlapping range [start, end) */
		ret = generate_subkey(ctx, ent->lfsr, start, end - 1);
		if (ret != 0) {
			free_lfsr(ent->lfsr);
			rte_free(ent);
			return ret;
		}
	} else if ((next_ent != NULL) && (end > next_ent->offset)) {
		HASH_LOG(ERR,
			"Can't add helper %s due to conflict with existing"
			" helper %s", ent->name, next_ent->name);
		rte_free(ent);
		return -ENOSPC;
	}
	attach_lfsr(ent, cur_ent->lfsr);

	/**
	 * generate partially overlapping range
	 * [start, cur_ent->start) in reverse order
	 */
	ret = generate_subkey(ctx, ent->lfsr, cur_ent->offset - 1, start);
	if (ret != 0) {
		free_lfsr(ent->lfsr);
		rte_free(ent);
		return ret;
	}

	if (end > range_end) {
		/**
		 * generate partially overlapping range
		 * (range_end, end)
		 */
		ret = generate_subkey(ctx, ent->lfsr, range_end, end - 1);
		if (ret != 0) {
			free_lfsr(ent->lfsr);
			rte_free(ent);
			return ret;
		}
	}

	LIST_INSERT_BEFORE(cur_ent, ent, next);
	generate_complement_table(ctx, ent);
	ctx->subtuples_nb++;
	return 0;
}

static inline int
insert_after(struct rte_thash_ctx *ctx,
	struct rte_thash_subtuple_helper *ent,
	struct rte_thash_subtuple_helper *cur_ent,
	struct rte_thash_subtuple_helper *next_ent,
	struct rte_thash_subtuple_helper *prev_ent,
	uint32_t end, uint32_t range_end)
{
	int ret;

	if ((next_ent != NULL) && (end > next_ent->offset)) {
		HASH_LOG(ERR,
			"Can't add helper %s due to conflict with existing"
			" helper %s", ent->name, next_ent->name);
		rte_free(ent);
		return -EEXIST;
	}

	attach_lfsr(ent, cur_ent->lfsr);
	if (end > range_end) {
		/**
		 * generate partially overlapping range
		 * (range_end, end)
		 */
		ret = generate_subkey(ctx, ent->lfsr, range_end, end - 1);
		if (ret != 0) {
			free_lfsr(ent->lfsr);
			rte_free(ent);
			return ret;
		}
	}

	LIST_INSERT_AFTER(prev_ent, ent, next);
	generate_complement_table(ctx, ent);
	ctx->subtuples_nb++;

	return 0;
}

int
rte_thash_add_helper(struct rte_thash_ctx *ctx, const char *name, uint32_t len,
	uint32_t offset)
{
	struct rte_thash_subtuple_helper *ent, *cur_ent, *prev_ent, *next_ent;
	uint32_t start, end;
	int ret;

	if ((ctx == NULL) || (name == NULL) || (len < ctx->reta_sz_log) ||
			((offset + len + TOEPLITZ_HASH_LEN - 1) >
			ctx->key_len * CHAR_BIT))
		return -EINVAL;

	/* Check for existing name*/
	LIST_FOREACH(cur_ent, &ctx->head, next) {
		if (strncmp(name, cur_ent->name, sizeof(cur_ent->name)) == 0)
			return -EEXIST;
	}

	end = offset + len + TOEPLITZ_HASH_LEN - 1;
	start = ((ctx->flags & RTE_THASH_MINIMAL_SEQ) ==
		RTE_THASH_MINIMAL_SEQ) ? (end - (2 * ctx->reta_sz_log - 1)) :
		offset;

	ent = rte_zmalloc(NULL, sizeof(struct rte_thash_subtuple_helper) +
		sizeof(uint32_t) * (1 << ctx->reta_sz_log),
		RTE_CACHE_LINE_SIZE);
	if (ent == NULL)
		return -ENOMEM;

	rte_strlcpy(ent->name, name, sizeof(ent->name));
	ent->offset = start;
	ent->len = end - start;
	ent->tuple_offset = offset;
	ent->tuple_len = len;
	ent->lsb_msk = (1 << ctx->reta_sz_log) - 1;

	cur_ent = LIST_FIRST(&ctx->head);
	while (cur_ent) {
		uint32_t range_end = cur_ent->offset + cur_ent->len;
		next_ent = LIST_NEXT(cur_ent, next);
		prev_ent = cur_ent;
		/* Iterate through overlapping ranges */
		while ((next_ent != NULL) && (next_ent->offset < range_end)) {
			range_end = RTE_MAX(next_ent->offset + next_ent->len,
				range_end);
			if (start > next_ent->offset)
				prev_ent = next_ent;

			next_ent = LIST_NEXT(next_ent, next);
		}

		if (start < cur_ent->offset)
			return insert_before(ctx, ent, cur_ent, next_ent,
				start, end, range_end);
		else if (start < range_end)
			return insert_after(ctx, ent, cur_ent, next_ent,
				prev_ent, end, range_end);

		cur_ent = next_ent;
		continue;
	}

	ent->lfsr = alloc_lfsr(ctx->reta_sz_log);
	if (ent->lfsr == NULL) {
		rte_free(ent);
		return -ENOMEM;
	}

	/* generate nonoverlapping range [start, end) */
	ret = generate_subkey(ctx, ent->lfsr, start, end - 1);
	if (ret != 0) {
		free_lfsr(ent->lfsr);
		rte_free(ent);
		return ret;
	}
	if (LIST_EMPTY(&ctx->head)) {
		LIST_INSERT_HEAD(&ctx->head, ent, next);
	} else {
		LIST_FOREACH(next_ent, &ctx->head, next)
			prev_ent = next_ent;

		LIST_INSERT_AFTER(prev_ent, ent, next);
	}
	generate_complement_table(ctx, ent);
	ctx->subtuples_nb++;

	return 0;
}

struct rte_thash_subtuple_helper *
rte_thash_get_helper(struct rte_thash_ctx *ctx, const char *name)
{
	struct rte_thash_subtuple_helper *ent;

	if ((ctx == NULL) || (name == NULL))
		return NULL;

	LIST_FOREACH(ent, &ctx->head, next) {
		if (strncmp(name, ent->name, sizeof(ent->name)) == 0)
			return ent;
	}

	return NULL;
}

uint32_t
rte_thash_get_complement(struct rte_thash_subtuple_helper *h,
	uint32_t hash, uint32_t desired_hash)
{
	return h->compl_table[(hash ^ desired_hash) & h->lsb_msk];
}

const uint8_t *
rte_thash_get_key(struct rte_thash_ctx *ctx)
{
	return ctx->hash_key;
}

const uint64_t *
rte_thash_get_gfni_matrices(struct rte_thash_ctx *ctx)
{
	return ctx->matrices;
}

static inline uint8_t
read_unaligned_byte(uint8_t *ptr, unsigned int offset)
{
	uint8_t ret = 0;

	ret = ptr[offset / CHAR_BIT];
	if (offset % CHAR_BIT) {
		ret <<= (offset % CHAR_BIT);
		ret |= ptr[(offset / CHAR_BIT) + 1] >>
			(CHAR_BIT - (offset % CHAR_BIT));
	}

	return ret;
}

static inline uint32_t
read_unaligned_bits(uint8_t *ptr, int len, int offset)
{
	uint32_t ret = 0;
	int shift;

	len = RTE_MAX(len, 0);
	len = RTE_MIN(len, (int)(sizeof(uint32_t) * CHAR_BIT));

	while (len > 0) {
		ret <<= CHAR_BIT;

		ret |= read_unaligned_byte(ptr, offset);
		offset += CHAR_BIT;
		len -= CHAR_BIT;
	}

	shift = (len == 0) ? 0 :
		(CHAR_BIT - ((len + CHAR_BIT) % CHAR_BIT));
	return ret >> shift;
}

/* returns mask for len bits with given offset inside byte */
static inline uint8_t
get_bits_mask(unsigned int len, unsigned int offset)
{
	unsigned int last_bit;

	offset %= CHAR_BIT;
	/* last bit within byte */
	last_bit = RTE_MIN((unsigned int)CHAR_BIT, offset + len);

	return ((1 << (CHAR_BIT - offset)) - 1) ^
		((1 << (CHAR_BIT - last_bit)) - 1);
}

static inline void
write_unaligned_byte(uint8_t *ptr, unsigned int len,
	unsigned int offset, uint8_t val)
{
	uint8_t tmp;

	tmp = ptr[offset / CHAR_BIT];
	tmp &= ~get_bits_mask(len, offset);
	tmp |= ((val << (CHAR_BIT - len)) >> (offset % CHAR_BIT));
	ptr[offset / CHAR_BIT] = tmp;
	if (((offset + len) / CHAR_BIT) != (offset / CHAR_BIT)) {
		int rest_len = (offset + len) % CHAR_BIT;
		tmp = ptr[(offset + len) / CHAR_BIT];
		tmp &= ~get_bits_mask(rest_len, 0);
		tmp |= val << (CHAR_BIT - rest_len);
		ptr[(offset + len) / CHAR_BIT] = tmp;
	}
}

static inline void
write_unaligned_bits(uint8_t *ptr, int len, int offset, uint32_t val)
{
	uint8_t tmp;
	unsigned int part_len;

	len = RTE_MAX(len, 0);
	len = RTE_MIN(len, (int)(sizeof(uint32_t) * CHAR_BIT));

	while (len > 0) {
		part_len = RTE_MIN(CHAR_BIT, len);
		tmp = (uint8_t)val & ((1 << part_len) - 1);
		write_unaligned_byte(ptr, part_len,
			offset + len - part_len, tmp);
		len -= CHAR_BIT;
		val >>= CHAR_BIT;
	}
}

int
rte_thash_adjust_tuple(struct rte_thash_ctx *ctx,
	struct rte_thash_subtuple_helper *h,
	uint8_t *tuple, unsigned int tuple_len,
	uint32_t desired_value,	unsigned int attempts,
	rte_thash_check_tuple_t fn, void *userdata)
{
	uint32_t tmp_tuple[tuple_len / sizeof(uint32_t)];
	unsigned int i, j, ret = 0;
	uint32_t hash, adj_bits;
	const uint8_t *hash_key;
	uint32_t tmp;
	int offset;
	int tmp_len;

	if ((ctx == NULL) || (h == NULL) || (tuple == NULL) ||
			(tuple_len % sizeof(uint32_t) != 0) || (attempts <= 0))
		return -EINVAL;

	hash_key = rte_thash_get_key(ctx);

	attempts = RTE_MIN(attempts, 1U << (h->tuple_len - ctx->reta_sz_log));

	for (i = 0; i < attempts; i++) {
		if (ctx->matrices != NULL)
			hash = rte_thash_gfni(ctx->matrices, tuple, tuple_len);
		else {
			for (j = 0; j < (tuple_len / 4); j++)
				tmp_tuple[j] =
					rte_be_to_cpu_32(
						*(uint32_t *)&tuple[j * 4]);

			hash = rte_softrss(tmp_tuple, tuple_len / 4, hash_key);
		}

		adj_bits = rte_thash_get_complement(h, hash, desired_value);

		/*
		 * Hint: LSB of adj_bits corresponds to
		 * offset + len bit of the subtuple
		 */
		offset =  h->tuple_offset + h->tuple_len - ctx->reta_sz_log;
		tmp = read_unaligned_bits(tuple, ctx->reta_sz_log, offset);
		tmp ^= adj_bits;
		write_unaligned_bits(tuple, ctx->reta_sz_log, offset, tmp);

		if (fn != NULL) {
			ret = (fn(userdata, tuple)) ? 0 : -EEXIST;
			if (ret == 0)
				return 0;
			else if (i < (attempts - 1)) {
				/* increment subtuple part by 1 */
				tmp_len = RTE_MIN(sizeof(uint32_t) * CHAR_BIT,
					h->tuple_len - ctx->reta_sz_log);
				offset -= tmp_len;
				tmp = read_unaligned_bits(tuple, tmp_len,
					offset);
				tmp++;
				tmp &= (1 << tmp_len) - 1;
				write_unaligned_bits(tuple, tmp_len, offset,
					tmp);
			}
		} else
			return 0;
	}

	return ret;
}

int
rte_thash_gen_key(uint8_t *key, size_t key_len, size_t reta_sz_log,
	uint32_t entropy_start, size_t entropy_sz)
{
	size_t i, end, start;

	/* define lfsr sequence range*/
	end = entropy_start + entropy_sz + TOEPLITZ_HASH_LEN - 1;
	start = end - (entropy_sz + reta_sz_log - 1);

	if ((key == NULL) || (key_len * CHAR_BIT < entropy_start + entropy_sz) ||
			(entropy_sz < reta_sz_log) || (reta_sz_log > TOEPLITZ_HASH_LEN))
		return -EINVAL;

	struct thash_lfsr *lfsr = alloc_lfsr(reta_sz_log);
	if (lfsr == NULL)
		return -ENOMEM;

	for (i = start; i < end; i++)
		set_bit(key, get_bit_lfsr(lfsr), i);

	free_lfsr(lfsr);

	return 0;
}
