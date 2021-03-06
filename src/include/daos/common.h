/**
 * (C) Copyright 2015, 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */

#ifndef __DAOS_COMMON_H__
#define __DAOS_COMMON_H__

#include <sys/time.h>
#include <sys/types.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <byteswap.h>

#include <daos_types.h>
#include <pouch/common.h>
#include <daos/debug.h>

/**
 * NB: hide the dark secret that
 * uuid_t is an array not a structure
 */
struct daos_uuid {
	uuid_t	uuid;
};

#define DF_U64		"%" PRIu64
#define DF_X64		"%" PRIx64

#define DF_OID		DF_U64"."DF_U64"."DF_U64
#define DP_OID(o)	(o).hi, (o).mid, (o).lo

#define DF_UOID		DF_OID".%u"
#define DP_UOID(uo)	DP_OID((uo).id_pub), (uo).id_shard

/*
 * Each thread has DF_UUID_MAX number of thread-local buffers for UUID strings.
 * Each debug message can have at most this many DP_UUIDs.
 *
 * DF_UUID prints the first eight characters of the string representation,
 * while DF_UUIDF prints the full 36-character string representation. DP_UUID()
 * matches both DF_UUID and DF_UUIDF.
 */
#define DF_UUID_MAX	8
#define DF_UUID		"%.8s"
#define DF_UUIDF	"%s"
char *DP_UUID(const void *uuid);

/* For prefixes of error messages about a container */
#define DF_CONT			DF_UUID"/"DF_UUID
#define DP_CONT(puuid, cuuid)	DP_UUID(puuid), DP_UUID(cuuid)

#define DD_ALLOC_PADDING		(dd_tune_alloc ? 4 : 0)
#define DD_ALLOC_MAGIC			0xdeadbeef
#define DD_ALLOC_POISON			0x3d

/* memory allocating macros */
#define D_ALLOC(ptr, size)						\
do {									\
	(ptr) = (__typeof__(ptr))calloc(1, (size + DD_ALLOC_PADDING));	\
	if ((ptr) == NULL) {						\
		D_ERROR("out of memory (alloc '" #ptr "' = %d)",	\
			(int)(size));					\
	}								\
	D_DEBUG(DB_MEM, "alloc #ptr : %d at %p.\n", (int)(size), ptr);	\
	if (DD_ALLOC_PADDING != 0) {					\
		void *__ptr = (void *)(ptr) + size;			\
		*(unsigned int *)__ptr = DD_ALLOC_MAGIC;		\
	}								\
} while (0)

# define D_FREE(ptr, size)						\
do {									\
	D_DEBUG(DB_MEM, "free #ptr : %d at %p.\n", (int)(size), ptr);	\
	if (DD_ALLOC_PADDING != 0) {					\
		void *__ptr = (void *)(ptr) + size;			\
		D_ASSERT(*(unsigned int *)__ptr == DD_ALLOC_MAGIC);	\
		if (size <= 2048) {					\
			memset((void *)(ptr), DD_ALLOC_POISON,		\
				size + DD_ALLOC_PADDING);		\
		}							\
	}								\
	free(ptr);							\
	(ptr) = NULL;							\
} while (0)

#define D_ALLOC_PTR(ptr)        D_ALLOC(ptr, sizeof *(ptr))
#define D_FREE_PTR(ptr)         D_FREE(ptr, sizeof *(ptr))

#define DAOS_GOLDEN_RATIO_PRIME_64	0xcbf29ce484222325ULL
#define DAOS_GOLDEN_RATIO_PRIME_32	0x9e370001UL

static inline uint64_t
daos_u64_hash(uint64_t val, unsigned int bits)
{
	uint64_t hash = val;

	hash *= DAOS_GOLDEN_RATIO_PRIME_64;
	return hash >> (64 - bits);
}

static inline uint32_t
daos_u32_hash(uint64_t key, unsigned int bits)
{
	return (DAOS_GOLDEN_RATIO_PRIME_32 * key) >> (32 - bits);
}

uint64_t daos_hash_mix64(uint64_t key);
uint32_t daos_hash_mix96(uint32_t a, uint32_t b, uint32_t c);

/** consistent hash search */
unsigned int daos_chash_srch_u64(uint64_t *hashes, unsigned int nhashes,
				 uint64_t value);

/** djb2 hash a string to a uint32_t value */
uint32_t daos_hash_string_u32(const char *string, unsigned int len);
/** murmur hash (64 bits) */
uint64_t daos_hash_murmur64(const unsigned char *key, unsigned int key_len,
			    unsigned int seed);

#define LOWEST_BIT_SET(x)       ((x) & ~((x) - 1))

static inline unsigned int
daos_power2_nbits(unsigned int val)
{
	unsigned int shift;

	for (shift = 1; (val >> shift) != 0; shift++);

	return val == LOWEST_BIT_SET(val) ? shift - 1 : shift;
}

/** Function table for combsort and binary search */
typedef struct {
	void    (*so_swap)(void *array, int a, int b);
	/**
	 * For ascending order:
	 * 0	array[a] == array[b]
	 * 1	array[a] > array[b]
	 * -1	array[a] < array[b]
	 */
	int     (*so_cmp)(void *array, int a, int b);
	/** for binary search */
	int	(*so_cmp_key)(void *array, int i, uint64_t key);
} daos_sort_ops_t;

int daos_array_sort(void *array, unsigned int len, bool unique,
		    daos_sort_ops_t *ops);
int daos_array_find(void *array, unsigned int len, uint64_t key,
		    daos_sort_ops_t *ops);
void daos_array_shuffle(void *arr, unsigned int len, daos_sort_ops_t *ops);

int  daos_sgl_init(daos_sg_list_t *sgl, unsigned int nr);
void daos_sgl_fini(daos_sg_list_t *sgl, bool free_iovs);
daos_size_t daos_sgl_data_len(daos_sg_list_t *sgl);
daos_size_t daos_sgl_buf_len(daos_sg_list_t *sgl);
daos_size_t daos_iod_len(daos_iod_t *iod);

char *daos_str_trimwhite(char *str);
int daos_iov_copy(daos_iov_t *dst, daos_iov_t *src);
void daos_iov_free(daos_iov_t *iov);

#if !defined(container_of)
/* given a pointer @ptr to the field @member embedded into type (usually
 *  * struct) @type, return pointer to the embedding instance of @type. */
# define container_of(ptr, type, member)		\
	        ((type *)((char *)(ptr)-(char *)(&((type *)0)->member)))
#endif

#ifndef offsetof
# define offsetof(typ,memb)	((long)((char *)&(((typ *)0)->memb)))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef MIN
# define MIN(a,b) (((a)<(b)) ? (a): (b))
#endif
#ifndef MAX
# define MAX(a,b) (((a)>(b)) ? (a): (b))
#endif

#ifndef min
#define min(x,y) ((x)<(y) ? (x) : (y))
#endif

#ifndef max
#define max(x,y) ((x)>(y) ? (x) : (y))
#endif

#ifndef min_t
#define min_t(type,x,y) \
	        ({ type __x = (x); type __y = (y); __x < __y ? __x: __y; })
#endif
#ifndef max_t
#define max_t(type,x,y) \
	        ({ type __x = (x); type __y = (y); __x > __y ? __x: __y; })
#endif

#define DAOS_UUID_STR_SIZE 37	/* 36 + 1 for '\0' */

/* byte swapper */
#define D_SWAP16(x)	bswap_16(x)
#define D_SWAP32(x)	bswap_32(x)
#define D_SWAP64(x)	bswap_64(x)
#define D_SWAP16S(x)	do { *(x) = D_SWAP16(*(x)); } while (0)
#define D_SWAP32S(x)	do { *(x) = D_SWAP32(*(x)); } while (0)
#define D_SWAP64S(x)	do { *(x) = D_SWAP64(*(x)); } while (0)

static inline unsigned int
daos_env2uint(char *string)
{
	unsigned int	result = 0;
	char		*end;
	unsigned long	temp;

	if (string == NULL)
		return 0;

	errno	= 0;
	temp	= strtoul(string, &end, 0);
	if (*end == '\0' && errno == 0)
		result = (unsigned int) temp;

	return result;
}

static inline int
daos_errno2der(int err)
{
	switch (err) {
	case 0:		return 0;
	case EPERM:
	case EACCES:	return -DER_NO_PERM;
	case ENOMEM:	return -DER_NOMEM;
	case EDQUOT:
	case ENOSPC:	return -DER_NOSPACE;
	case EEXIST:	return -DER_EXIST;
	case ENOENT:	return -DER_NONEXIST;
	case ECANCELED:	return -DER_CANCELED;
	default:	return -DER_INVAL;
	}
}

static inline bool
daos_crt_network_error(int err)
{
	return err == -DER_CRT_HG || err == -DER_CRT_ADDRSTR_GEN ||
	       err == -DER_CRT_PMIX || err == -DER_CRT_UNREG ||
	       err == -DER_UNREACH || err == -DER_CANCELED;
}

#define daos_rank_list_dup		crt_rank_list_dup
#define daos_rank_list_dup_sort_uniq	crt_rank_list_dup_sort_uniq
#define daos_rank_list_alloc		crt_rank_list_alloc
#define daos_rank_list_free		crt_rank_list_free
#define daos_rank_list_copy		crt_rank_list_copy
#define daos_rank_list_sort		crt_rank_list_sort
#define daos_rank_list_find		crt_rank_list_find
#define daos_rank_list_identical	crt_rank_list_identical
#define daos_rank_in_rank_list		crt_rank_in_rank_list
#define daos_rank_list_append		crt_rank_list_append

daos_rank_list_t *daos_rank_list_parse(const char *str, const char *sep);

void
daos_fail_loc_set(uint64_t id);
void
daos_fail_value_set(uint64_t val);

int
daos_fail_check(uint64_t id);

/**
 * DAOS FAIL Mask
 *
 * fail loc 0-24
 * fail mode 24-32
 * unused 32-64
 **/

/* fail mode */
#define DAOS_FAIL_ONCE		0x1000000
#define DAOS_FAIL_SOME		0x2000000

#define DAOS_SHARD_OBJ_UPDATE_TIMEOUT	(0x001)
#define DAOS_SHARD_OBJ_FETCH_TIMEOUT	(0x002)
#define DAOS_SHARD_OBJ_FAIL		(0x003)
#define DAOS_OBJ_UPDATE_NOSPACE		(0x004)
#define DAOS_SHARD_OBJ_RW_CRT_ERROR	(0x005)
#define DAOS_OBJ_REQ_CREATE_TIMEOUT	(0x006)
#define DAOS_SHARD_OBJ_UPDATE_TIMEOUT_SINGLE	(0x007)

#define DAOS_FAIL_CHECK(id) daos_fail_check(id)

static inline int __is_po2(unsigned long long val)
{
	return !(val & (val - 1));
}

#define IS_PO2(val)	__is_po2((unsigned long long)(val))

#endif /* __DAOS_COMMON_H__ */
