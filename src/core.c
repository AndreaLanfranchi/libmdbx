/*
 * Copyright 2015-2022 Leonid Yuriev <leo@yuriev.ru>.
 * and other libmdbx authors: please see AUTHORS file.
 * All rights reserved.
 *
 * This code is derived from "LMDB engine" written by
 * Howard Chu (Symas Corporation), which itself derived from btree.c
 * written by Martin Hedenfalk.
 *
 * ---
 *
 * Portions Copyright 2011-2015 Howard Chu, Symas Corp. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 *
 * ---
 *
 * Portions Copyright (c) 2009, 2010 Martin Hedenfalk <martin@bzero.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include "internals.h"

/*------------------------------------------------------------------------------
 * Internal inline functions */

MDBX_NOTHROW_CONST_FUNCTION static unsigned branchless_abs(int value) {
  assert(value > INT_MIN);
  const unsigned expanded_sign =
      (unsigned)(value >> (sizeof(value) * CHAR_BIT - 1));
  return ((unsigned)value + expanded_sign) ^ expanded_sign;
}

/* Pack/Unpack 16-bit values for Grow step & Shrink threshold */
MDBX_NOTHROW_CONST_FUNCTION static __inline pgno_t me2v(unsigned m,
                                                        unsigned e) {
  assert(m < 2048 && e < 8);
  return (pgno_t)(32768 + ((m + 1) << (e + 8)));
}

MDBX_NOTHROW_CONST_FUNCTION static __inline uint16_t v2me(size_t v,
                                                          unsigned e) {
  assert(v > (e ? me2v(2047, e - 1) : 32768));
  assert(v <= me2v(2047, e));
  size_t m = (v - 32768 + ((size_t)1 << (e + 8)) - 1) >> (e + 8);
  m -= m > 0;
  assert(m < 2048 && e < 8);
  // f e d c b a 9 8 7 6 5 4 3 2 1 0
  // 1 e e e m m m m m m m m m m m 1
  const uint16_t pv = (uint16_t)(0x8001 + (e << 12) + (m << 1));
  assert(pv != 65535);
  return pv;
}

/* Convert 16-bit packed (exponential quantized) value to number of pages */
MDBX_NOTHROW_CONST_FUNCTION static pgno_t pv2pages(uint16_t pv) {
  if ((pv & 0x8001) != 0x8001)
    return pv;
  if (pv == 65535)
    return 65536;
  // f e d c b a 9 8 7 6 5 4 3 2 1 0
  // 1 e e e m m m m m m m m m m m 1
  return me2v((pv >> 1) & 2047, (pv >> 12) & 7);
}

/* Convert number of pages to 16-bit packed (exponential quantized) value */
MDBX_NOTHROW_CONST_FUNCTION static uint16_t pages2pv(size_t pages) {
  if (pages < 32769 || (pages < 65536 && (pages & 1) == 0))
    return (uint16_t)pages;
  if (pages <= me2v(2047, 0))
    return v2me(pages, 0);
  if (pages <= me2v(2047, 1))
    return v2me(pages, 1);
  if (pages <= me2v(2047, 2))
    return v2me(pages, 2);
  if (pages <= me2v(2047, 3))
    return v2me(pages, 3);
  if (pages <= me2v(2047, 4))
    return v2me(pages, 4);
  if (pages <= me2v(2047, 5))
    return v2me(pages, 5);
  if (pages <= me2v(2047, 6))
    return v2me(pages, 6);
  return (pages < me2v(2046, 7)) ? v2me(pages, 7) : 65533;
}

/*------------------------------------------------------------------------------
 * Unaligned access */

MDBX_MAYBE_UNUSED MDBX_NOTHROW_CONST_FUNCTION static __always_inline unsigned
field_alignment(unsigned alignment_baseline, size_t field_offset) {
  unsigned merge = alignment_baseline | (unsigned)field_offset;
  return merge & -(int)merge;
}

/* read-thunk for UB-sanitizer */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline uint8_t
peek_u8(const uint8_t *const __restrict ptr) {
  return *ptr;
}

/* write-thunk for UB-sanitizer */
static __always_inline void poke_u8(uint8_t *const __restrict ptr,
                                    const uint8_t v) {
  *ptr = v;
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline uint16_t
unaligned_peek_u16(const unsigned expected_alignment, const void *const ptr) {
  assert((uintptr_t)ptr % expected_alignment == 0);
  if (MDBX_UNALIGNED_OK || (expected_alignment % sizeof(uint16_t)) == 0)
    return *(const uint16_t *)ptr;
  else {
    uint16_t v;
    memcpy(&v, ptr, sizeof(v));
    return v;
  }
}

static __always_inline void
unaligned_poke_u16(const unsigned expected_alignment,
                   void *const __restrict ptr, const uint16_t v) {
  assert((uintptr_t)ptr % expected_alignment == 0);
  if (MDBX_UNALIGNED_OK || (expected_alignment % sizeof(v)) == 0)
    *(uint16_t *)ptr = v;
  else
    memcpy(ptr, &v, sizeof(v));
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline uint32_t unaligned_peek_u32(
    const unsigned expected_alignment, const void *const __restrict ptr) {
  assert((uintptr_t)ptr % expected_alignment == 0);
  if (MDBX_UNALIGNED_OK || (expected_alignment % sizeof(uint32_t)) == 0)
    return *(const uint32_t *)ptr;
  else if ((expected_alignment % sizeof(uint16_t)) == 0) {
    const uint16_t lo =
        ((const uint16_t *)ptr)[__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__];
    const uint16_t hi =
        ((const uint16_t *)ptr)[__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__];
    return lo | (uint32_t)hi << 16;
  } else {
    uint32_t v;
    memcpy(&v, ptr, sizeof(v));
    return v;
  }
}

static __always_inline void
unaligned_poke_u32(const unsigned expected_alignment,
                   void *const __restrict ptr, const uint32_t v) {
  assert((uintptr_t)ptr % expected_alignment == 0);
  if (MDBX_UNALIGNED_OK || (expected_alignment % sizeof(v)) == 0)
    *(uint32_t *)ptr = v;
  else if ((expected_alignment % sizeof(uint16_t)) == 0) {
    ((uint16_t *)ptr)[__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__] = (uint16_t)v;
    ((uint16_t *)ptr)[__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__] =
        (uint16_t)(v >> 16);
  } else
    memcpy(ptr, &v, sizeof(v));
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline uint64_t unaligned_peek_u64(
    const unsigned expected_alignment, const void *const __restrict ptr) {
  assert((uintptr_t)ptr % expected_alignment == 0);
  if (MDBX_UNALIGNED_OK || (expected_alignment % sizeof(uint64_t)) == 0)
    return *(const uint64_t *)ptr;
  else if ((expected_alignment % sizeof(uint32_t)) == 0) {
    const uint32_t lo =
        ((const uint32_t *)ptr)[__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__];
    const uint32_t hi =
        ((const uint32_t *)ptr)[__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__];
    return lo | (uint64_t)hi << 32;
  } else {
    uint64_t v;
    memcpy(&v, ptr, sizeof(v));
    return v;
  }
}

static __always_inline void
unaligned_poke_u64(const unsigned expected_alignment,
                   void *const __restrict ptr, const uint64_t v) {
  assert((uintptr_t)ptr % expected_alignment == 0);
  if (MDBX_UNALIGNED_OK || (expected_alignment % sizeof(v)) == 0)
    *(uint64_t *)ptr = v;
  else if ((expected_alignment % sizeof(uint32_t)) == 0) {
    ((uint32_t *)ptr)[__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__] = (uint32_t)v;
    ((uint32_t *)ptr)[__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__] =
        (uint32_t)(v >> 32);
  } else
    memcpy(ptr, &v, sizeof(v));
}

#define UNALIGNED_PEEK_8(ptr, struct, field)                                   \
  peek_u8((const uint8_t *)(ptr) + offsetof(struct, field))
#define UNALIGNED_POKE_8(ptr, struct, field, value)                            \
  poke_u8((uint8_t *)(ptr) + offsetof(struct, field), value)

#define UNALIGNED_PEEK_16(ptr, struct, field)                                  \
  unaligned_peek_u16(1, (const char *)(ptr) + offsetof(struct, field))
#define UNALIGNED_POKE_16(ptr, struct, field, value)                           \
  unaligned_poke_u16(1, (char *)(ptr) + offsetof(struct, field), value)

#define UNALIGNED_PEEK_32(ptr, struct, field)                                  \
  unaligned_peek_u32(1, (const char *)(ptr) + offsetof(struct, field))
#define UNALIGNED_POKE_32(ptr, struct, field, value)                           \
  unaligned_poke_u32(1, (char *)(ptr) + offsetof(struct, field), value)

#define UNALIGNED_PEEK_64(ptr, struct, field)                                  \
  unaligned_peek_u64(1, (const char *)(ptr) + offsetof(struct, field))
#define UNALIGNED_POKE_64(ptr, struct, field, value)                           \
  unaligned_poke_u64(1, (char *)(ptr) + offsetof(struct, field), value)

/* Get the page number pointed to by a branch node */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline pgno_t
node_pgno(const MDBX_node *const __restrict node) {
  pgno_t pgno = UNALIGNED_PEEK_32(node, MDBX_node, mn_pgno32);
  if (sizeof(pgno) > 4)
    pgno |= ((uint64_t)UNALIGNED_PEEK_8(node, MDBX_node, mn_extra)) << 32;
  return pgno;
}

/* Set the page number in a branch node */
static __always_inline void node_set_pgno(MDBX_node *const __restrict node,
                                          pgno_t pgno) {
  assert(pgno >= MIN_PAGENO && pgno <= MAX_PAGENO);

  UNALIGNED_POKE_32(node, MDBX_node, mn_pgno32, (uint32_t)pgno);
  if (sizeof(pgno) > 4)
    UNALIGNED_POKE_8(node, MDBX_node, mn_extra,
                     (uint8_t)((uint64_t)pgno >> 32));
}

/* Get the size of the data in a leaf node */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline size_t
node_ds(const MDBX_node *const __restrict node) {
  return UNALIGNED_PEEK_32(node, MDBX_node, mn_dsize);
}

/* Set the size of the data for a leaf node */
static __always_inline void node_set_ds(MDBX_node *const __restrict node,
                                        size_t size) {
  assert(size < INT_MAX);
  UNALIGNED_POKE_32(node, MDBX_node, mn_dsize, (uint32_t)size);
}

/* The size of a key in a node */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline size_t
node_ks(const MDBX_node *const __restrict node) {
  return UNALIGNED_PEEK_16(node, MDBX_node, mn_ksize);
}

/* Set the size of the key for a leaf node */
static __always_inline void node_set_ks(MDBX_node *const __restrict node,
                                        size_t size) {
  assert(size < INT16_MAX);
  UNALIGNED_POKE_16(node, MDBX_node, mn_ksize, (uint16_t)size);
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline uint8_t
node_flags(const MDBX_node *const __restrict node) {
  return UNALIGNED_PEEK_8(node, MDBX_node, mn_flags);
}

static __always_inline void node_set_flags(MDBX_node *const __restrict node,
                                           uint8_t flags) {
  UNALIGNED_POKE_8(node, MDBX_node, mn_flags, flags);
}

/* Size of the node header, excluding dynamic data at the end */
#define NODESIZE offsetof(MDBX_node, mn_data)

/* Address of the key for the node */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline void *
node_key(const MDBX_node *const __restrict node) {
  return (char *)node + NODESIZE;
}

/* Address of the data for a node */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline void *
node_data(const MDBX_node *const __restrict node) {
  return (char *)node_key(node) + node_ks(node);
}

/* Size of a node in a leaf page with a given key and data.
 * This is node header plus key plus data size. */
MDBX_NOTHROW_CONST_FUNCTION static __always_inline size_t
node_size_len(const size_t key_len, const size_t value_len) {
  return NODESIZE + EVEN(key_len + value_len);
}
MDBX_NOTHROW_PURE_FUNCTION static __always_inline size_t
node_size(const MDBX_val *key, const MDBX_val *value) {
  return node_size_len(key ? key->iov_len : 0, value ? value->iov_len : 0);
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline pgno_t
peek_pgno(const void *const __restrict ptr) {
  if (sizeof(pgno_t) == sizeof(uint32_t))
    return (pgno_t)unaligned_peek_u32(1, ptr);
  else if (sizeof(pgno_t) == sizeof(uint64_t))
    return (pgno_t)unaligned_peek_u64(1, ptr);
  else {
    pgno_t pgno;
    memcpy(&pgno, ptr, sizeof(pgno));
    return pgno;
  }
}

static __always_inline void poke_pgno(void *const __restrict ptr,
                                      const pgno_t pgno) {
  if (sizeof(pgno) == sizeof(uint32_t))
    unaligned_poke_u32(1, ptr, pgno);
  else if (sizeof(pgno) == sizeof(uint64_t))
    unaligned_poke_u64(1, ptr, pgno);
  else
    memcpy(ptr, &pgno, sizeof(pgno));
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline pgno_t
node_largedata_pgno(const MDBX_node *const __restrict node) {
  assert(node_flags(node) & F_BIGDATA);
  return peek_pgno(node_data(node));
}

/*------------------------------------------------------------------------------
 * Nodes, Keys & Values length limitation factors:
 *
 * BRANCH_NODE_MAX
 *   Branch-page must contain at least two nodes, within each a key and a child
 *   page number. But page can't be splitted if it contains less that 4 keys,
 *   i.e. a page should not overflow before adding the fourth key. Therefore,
 *   at least 3 branch-node should fit in the single branch-page. Further, the
 *   first node of a branch-page doesn't contain a key, i.e. the first node
 *   is always require space just for itself. Thus:
 *       PAGEROOM = pagesize - page_hdr_len;
 *       BRANCH_NODE_MAX = even_floor(
 *         (PAGEROOM - sizeof(indx_t) - NODESIZE) / (3 - 1) - sizeof(indx_t));
 *       KEYLEN_MAX = BRANCH_NODE_MAX - node_hdr_len;
 *
 * LEAF_NODE_MAX
 *   Leaf-node must fit into single leaf-page, where a value could be placed on
 *   a large/overflow page. However, may require to insert a nearly page-sized
 *   node between two large nodes are already fill-up a page. In this case the
 *   page must be splitted to two if some pair of nodes fits on one page, or
 *   otherwise the page should be splitted to the THREE with a single node
 *   per each of ones. Such 1-into-3 page splitting is costly and complex since
 *   requires TWO insertion into the parent page, that could lead to split it
 *   and so on up to the root. Therefore double-splitting is avoided here and
 *   the maximum node size is half of a leaf page space:
 *       LEAF_NODE_MAX = even_floor(PAGEROOM / 2 - sizeof(indx_t));
 *       DATALEN_NO_OVERFLOW = LEAF_NODE_MAX - KEYLEN_MAX;
 *
 *  - SubDatabase-node must fit into one leaf-page:
 *       SUBDB_NAME_MAX = LEAF_NODE_MAX - node_hdr_len - sizeof(MDBX_db);
 *
 *  - Dupsort values itself are a keys in a dupsort-subdb and couldn't be longer
 *    than the KEYLEN_MAX. But dupsort node must not great than LEAF_NODE_MAX,
 *    since dupsort value couldn't be placed on a large/overflow page:
 *       DUPSORT_DATALEN_MAX = min(KEYLEN_MAX,
 *                                 max(DATALEN_NO_OVERFLOW, sizeof(MDBX_db));
 */

#define PAGEROOM(pagesize) ((pagesize)-PAGEHDRSZ)
#define EVEN_FLOOR(n) ((n) & ~(size_t)1)
#define BRANCH_NODE_MAX(pagesize)                                              \
  (EVEN_FLOOR((PAGEROOM(pagesize) - sizeof(indx_t) - NODESIZE) / (3 - 1) -     \
              sizeof(indx_t)))
#define LEAF_NODE_MAX(pagesize)                                                \
  (EVEN_FLOOR(PAGEROOM(pagesize) / 2) - sizeof(indx_t))
#define MAX_GC1OVPAGE(pagesize) (PAGEROOM(pagesize) / sizeof(pgno_t) - 1)

static __inline unsigned keysize_max(size_t pagesize, MDBX_db_flags_t flags) {
  assert(pagesize >= MIN_PAGESIZE && pagesize <= MAX_PAGESIZE &&
         is_powerof2(pagesize));
  STATIC_ASSERT(BRANCH_NODE_MAX(MIN_PAGESIZE) - NODESIZE >= 8);
  if (flags & MDBX_INTEGERKEY)
    return 8 /* sizeof(uint64_t) */;

  const intptr_t max_branch_key = BRANCH_NODE_MAX(pagesize) - NODESIZE;
  STATIC_ASSERT(LEAF_NODE_MAX(MIN_PAGESIZE) - NODESIZE -
                    /* sizeof(uint64) as a key */ 8 >
                sizeof(MDBX_db));
  if (flags &
      (MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_REVERSEDUP | MDBX_INTEGERDUP)) {
    const intptr_t max_dupsort_leaf_key =
        LEAF_NODE_MAX(pagesize) - NODESIZE - sizeof(MDBX_db);
    return (max_branch_key < max_dupsort_leaf_key)
               ? (unsigned)max_branch_key
               : (unsigned)max_dupsort_leaf_key;
  }
  return (unsigned)max_branch_key;
}

static __inline size_t valsize_max(size_t pagesize, MDBX_db_flags_t flags) {
  assert(pagesize >= MIN_PAGESIZE && pagesize <= MAX_PAGESIZE &&
         is_powerof2(pagesize));

  if (flags & MDBX_INTEGERDUP)
    return 8 /* sizeof(uint64_t) */;

  if (flags & (MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_REVERSEDUP))
    return keysize_max(pagesize, 0);

  const unsigned page_ln2 = log2n_powerof2(pagesize);
  const size_t hard = 0x7FF00000ul;
  const size_t hard_pages = hard >> page_ln2;
  STATIC_ASSERT(MDBX_PGL_LIMIT <= MAX_PAGENO);
  const size_t pages_limit = MDBX_PGL_LIMIT / 4;
  const size_t limit =
      (hard_pages < pages_limit) ? hard : (pages_limit << page_ln2);
  return (limit < MAX_MAPSIZE / 2) ? limit : MAX_MAPSIZE / 2;
}

__cold int mdbx_env_get_maxkeysize(const MDBX_env *env) {
  return mdbx_env_get_maxkeysize_ex(env, MDBX_DUPSORT);
}

__cold int mdbx_env_get_maxkeysize_ex(const MDBX_env *env,
                                      MDBX_db_flags_t flags) {
  if (unlikely(!env || env->me_signature.weak != MDBX_ME_SIGNATURE))
    return -1;

  return (int)mdbx_limits_keysize_max((intptr_t)env->me_psize, flags);
}

size_t mdbx_default_pagesize(void) {
  size_t pagesize = mdbx_syspagesize();
  mdbx_ensure(nullptr, is_powerof2(pagesize));
  pagesize = (pagesize >= MIN_PAGESIZE) ? pagesize : MIN_PAGESIZE;
  pagesize = (pagesize <= MAX_PAGESIZE) ? pagesize : MAX_PAGESIZE;
  return pagesize;
}

__cold intptr_t mdbx_limits_keysize_max(intptr_t pagesize,
                                        MDBX_db_flags_t flags) {
  if (pagesize < 1)
    pagesize = (intptr_t)mdbx_default_pagesize();
  if (unlikely(pagesize < (intptr_t)MIN_PAGESIZE ||
               pagesize > (intptr_t)MAX_PAGESIZE ||
               !is_powerof2((size_t)pagesize)))
    return -1;

  return keysize_max(pagesize, flags);
}

__cold int mdbx_env_get_maxvalsize_ex(const MDBX_env *env,
                                      MDBX_db_flags_t flags) {
  if (unlikely(!env || env->me_signature.weak != MDBX_ME_SIGNATURE))
    return -1;

  return (int)mdbx_limits_valsize_max((intptr_t)env->me_psize, flags);
}

__cold intptr_t mdbx_limits_valsize_max(intptr_t pagesize,
                                        MDBX_db_flags_t flags) {
  if (pagesize < 1)
    pagesize = (intptr_t)mdbx_default_pagesize();
  if (unlikely(pagesize < (intptr_t)MIN_PAGESIZE ||
               pagesize > (intptr_t)MAX_PAGESIZE ||
               !is_powerof2((size_t)pagesize)))
    return -1;

  return valsize_max(pagesize, flags);
}

/* Calculate the size of a leaf node.
 *
 * The size depends on the environment's page size; if a data item
 * is too large it will be put onto an overflow page and the node
 * size will only include the key and not the data. Sizes are always
 * rounded up to an even number of bytes, to guarantee 2-byte alignment
 * of the MDBX_node headers. */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline size_t
leaf_size(const MDBX_env *env, const MDBX_val *key, const MDBX_val *data) {
  size_t node_bytes = node_size(key, data);
  if (node_bytes > env->me_leaf_nodemax) {
    /* put on overflow page */
    node_bytes = node_size_len(key->iov_len, 0) + sizeof(pgno_t);
  }

  return node_bytes + sizeof(indx_t);
}

/* Calculate the size of a branch node.
 *
 * The size should depend on the environment's page size but since
 * we currently don't support spilling large keys onto overflow
 * pages, it's simply the size of the MDBX_node header plus the
 * size of the key. Sizes are always rounded up to an even number
 * of bytes, to guarantee 2-byte alignment of the MDBX_node headers.
 *
 * [in] env The environment handle.
 * [in] key The key for the node.
 *
 * Returns The number of bytes needed to store the node. */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline size_t
branch_size(const MDBX_env *env, const MDBX_val *key) {
  /* Size of a node in a branch page with a given key.
   * This is just the node header plus the key, there is no data. */
  size_t node_bytes = node_size(key, nullptr);
  if (unlikely(node_bytes > env->me_leaf_nodemax)) {
    /* put on overflow page */
    /* not implemented */
    mdbx_assert_fail(env, "INDXSIZE(key) <= env->me_nodemax", __func__,
                     __LINE__);
    node_bytes = node_size(key, nullptr) + sizeof(pgno_t);
  }

  return node_bytes + sizeof(indx_t);
}

MDBX_NOTHROW_CONST_FUNCTION static __always_inline uint16_t
flags_db2sub(uint16_t db_flags) {
  uint16_t sub_flags = db_flags & MDBX_DUPFIXED;

  /* MDBX_INTEGERDUP => MDBX_INTEGERKEY */
#define SHIFT_INTEGERDUP_TO_INTEGERKEY 2
  STATIC_ASSERT((MDBX_INTEGERDUP >> SHIFT_INTEGERDUP_TO_INTEGERKEY) ==
                MDBX_INTEGERKEY);
  sub_flags |= (db_flags & MDBX_INTEGERDUP) >> SHIFT_INTEGERDUP_TO_INTEGERKEY;

  /* MDBX_REVERSEDUP => MDBX_REVERSEKEY */
#define SHIFT_REVERSEDUP_TO_REVERSEKEY 5
  STATIC_ASSERT((MDBX_REVERSEDUP >> SHIFT_REVERSEDUP_TO_REVERSEKEY) ==
                MDBX_REVERSEKEY);
  sub_flags |= (db_flags & MDBX_REVERSEDUP) >> SHIFT_REVERSEDUP_TO_REVERSEKEY;

  return sub_flags;
}

/*----------------------------------------------------------------------------*/

MDBX_NOTHROW_PURE_FUNCTION static __always_inline size_t
pgno2bytes(const MDBX_env *env, pgno_t pgno) {
  mdbx_assert(env, (1u << env->me_psize2log) == env->me_psize);
  return ((size_t)pgno) << env->me_psize2log;
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline MDBX_page *
pgno2page(const MDBX_env *env, pgno_t pgno) {
  return (MDBX_page *)(env->me_map + pgno2bytes(env, pgno));
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline pgno_t
bytes2pgno(const MDBX_env *env, size_t bytes) {
  mdbx_assert(env, (env->me_psize >> env->me_psize2log) == 1);
  return (pgno_t)(bytes >> env->me_psize2log);
}

MDBX_NOTHROW_PURE_FUNCTION static size_t
pgno_align2os_bytes(const MDBX_env *env, pgno_t pgno) {
  return ceil_powerof2(pgno2bytes(env, pgno), env->me_os_psize);
}

MDBX_NOTHROW_PURE_FUNCTION static pgno_t pgno_align2os_pgno(const MDBX_env *env,
                                                            pgno_t pgno) {
  return bytes2pgno(env, pgno_align2os_bytes(env, pgno));
}

MDBX_NOTHROW_PURE_FUNCTION static size_t
bytes_align2os_bytes(const MDBX_env *env, size_t bytes) {
  return ceil_powerof2(ceil_powerof2(bytes, env->me_psize), env->me_os_psize);
}

/* Address of first usable data byte in a page, after the header */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline void *
page_data(const MDBX_page *mp) {
  return (char *)mp + PAGEHDRSZ;
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline const MDBX_page *
data_page(const void *data) {
  return container_of(data, MDBX_page, mp_ptrs);
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline MDBX_meta *
page_meta(MDBX_page *mp) {
  return (MDBX_meta *)page_data(mp);
}

/* Number of nodes on a page */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline unsigned
page_numkeys(const MDBX_page *mp) {
  return mp->mp_lower >> 1;
}

/* The amount of space remaining in the page */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline unsigned
page_room(const MDBX_page *mp) {
  return mp->mp_upper - mp->mp_lower;
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline unsigned
page_space(const MDBX_env *env) {
  STATIC_ASSERT(PAGEHDRSZ % 2 == 0);
  return env->me_psize - PAGEHDRSZ;
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline unsigned
page_used(const MDBX_env *env, const MDBX_page *mp) {
  return page_space(env) - page_room(mp);
}

/* The percentage of space used in the page, in a percents. */
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __inline double
page_fill(const MDBX_env *env, const MDBX_page *mp) {
  return page_used(env, mp) * 100.0 / page_space(env);
}

/* The number of overflow pages needed to store the given size. */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline pgno_t
number_of_ovpages(const MDBX_env *env, size_t bytes) {
  return bytes2pgno(env, PAGEHDRSZ - 1 + bytes) + 1;
}

__cold static int MDBX_PRINTF_ARGS(2, 3)
    bad_page(const MDBX_page *mp, const char *fmt, ...) {
  if (mdbx_log_enabled(MDBX_LOG_ERROR)) {
    static const MDBX_page *prev;
    if (prev != mp) {
      prev = mp;
      const char *type;
      switch (mp->mp_flags & (P_BRANCH | P_LEAF | P_OVERFLOW | P_META |
                              P_LEAF2 | P_BAD | P_SUBP)) {
      case P_BRANCH:
        type = "branch";
        break;
      case P_LEAF:
        type = "leaf";
        break;
      case P_LEAF | P_SUBP:
        type = "subleaf";
        break;
      case P_LEAF | P_LEAF2:
        type = "dupfixed-leaf";
        break;
      case P_LEAF | P_LEAF2 | P_SUBP:
        type = "dupfixed-subleaf";
        break;
      case P_OVERFLOW:
        type = "large";
        break;
      default:
        type = "broken";
      }
      mdbx_debug_log(MDBX_LOG_ERROR, "badpage", 0,
                     "corrupted %s-page #%u, mod-txnid %" PRIaTXN "\n", type,
                     mp->mp_pgno, mp->mp_txnid);
    }

    va_list args;
    va_start(args, fmt);
    mdbx_debug_log_va(MDBX_LOG_ERROR, "badpage", 0, fmt, args);
    va_end(args);
  }
  return MDBX_CORRUPTED;
}

/* Address of node i in page p */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline MDBX_node *
page_node(const MDBX_page *mp, unsigned i) {
  assert((mp->mp_flags & (P_LEAF2 | P_OVERFLOW | P_META)) == 0);
  assert(page_numkeys(mp) > (unsigned)(i));
  assert(mp->mp_ptrs[i] % 2 == 0);
  return (MDBX_node *)((char *)mp + mp->mp_ptrs[i] + PAGEHDRSZ);
}

/* The address of a key in a LEAF2 page.
 * LEAF2 pages are used for MDBX_DUPFIXED sorted-duplicate sub-DBs.
 * There are no node headers, keys are stored contiguously. */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline void *
page_leaf2key(const MDBX_page *mp, unsigned i, size_t keysize) {
  assert((mp->mp_flags & (P_BRANCH | P_LEAF | P_LEAF2 | P_OVERFLOW | P_META)) ==
         (P_LEAF | P_LEAF2));
  assert(mp->mp_leaf2_ksize == keysize);
  (void)keysize;
  return (char *)mp + PAGEHDRSZ + (i * mp->mp_leaf2_ksize);
}

/* Set the node's key into keyptr. */
static __always_inline void get_key(const MDBX_node *node, MDBX_val *keyptr) {
  keyptr->iov_len = node_ks(node);
  keyptr->iov_base = node_key(node);
}

/* Set the node's key into keyptr, if requested. */
static __always_inline void
get_key_optional(const MDBX_node *node, MDBX_val *keyptr /* __may_null */) {
  if (keyptr)
    get_key(node, keyptr);
}

/*------------------------------------------------------------------------------
 * Workaround for mmaped-lookahead-cross-page-boundary bug
 * in an obsolete versions of Elbrus's libc and kernels. */
#if defined(__e2k__) && defined(MDBX_E2K_MLHCPB_WORKAROUND) &&                 \
    MDBX_E2K_MLHCPB_WORKAROUND
int __hot mdbx_e2k_memcmp_bug_workaround(const void *s1, const void *s2,
                                         size_t n) {
  if (unlikely(n > 42
               /* LY: align followed access if reasonable possible */
               && (((uintptr_t)s1) & 7) != 0 &&
               (((uintptr_t)s1) & 7) == (((uintptr_t)s2) & 7))) {
    if (((uintptr_t)s1) & 1) {
      const int diff = *(uint8_t *)s1 - *(uint8_t *)s2;
      if (diff)
        return diff;
      s1 = (char *)s1 + 1;
      s2 = (char *)s2 + 1;
      n -= 1;
    }

    if (((uintptr_t)s1) & 2) {
      const uint16_t a = *(uint16_t *)s1;
      const uint16_t b = *(uint16_t *)s2;
      if (likely(a != b))
        return (__builtin_bswap16(a) > __builtin_bswap16(b)) ? 1 : -1;
      s1 = (char *)s1 + 2;
      s2 = (char *)s2 + 2;
      n -= 2;
    }

    if (((uintptr_t)s1) & 4) {
      const uint32_t a = *(uint32_t *)s1;
      const uint32_t b = *(uint32_t *)s2;
      if (likely(a != b))
        return (__builtin_bswap32(a) > __builtin_bswap32(b)) ? 1 : -1;
      s1 = (char *)s1 + 4;
      s2 = (char *)s2 + 4;
      n -= 4;
    }
  }

  while (n >= 8) {
    const uint64_t a = *(uint64_t *)s1;
    const uint64_t b = *(uint64_t *)s2;
    if (likely(a != b))
      return (__builtin_bswap64(a) > __builtin_bswap64(b)) ? 1 : -1;
    s1 = (char *)s1 + 8;
    s2 = (char *)s2 + 8;
    n -= 8;
  }

  if (n & 4) {
    const uint32_t a = *(uint32_t *)s1;
    const uint32_t b = *(uint32_t *)s2;
    if (likely(a != b))
      return (__builtin_bswap32(a) > __builtin_bswap32(b)) ? 1 : -1;
    s1 = (char *)s1 + 4;
    s2 = (char *)s2 + 4;
  }

  if (n & 2) {
    const uint16_t a = *(uint16_t *)s1;
    const uint16_t b = *(uint16_t *)s2;
    if (likely(a != b))
      return (__builtin_bswap16(a) > __builtin_bswap16(b)) ? 1 : -1;
    s1 = (char *)s1 + 2;
    s2 = (char *)s2 + 2;
  }

  return (n & 1) ? *(uint8_t *)s1 - *(uint8_t *)s2 : 0;
}

int __hot mdbx_e2k_strcmp_bug_workaround(const char *s1, const char *s2) {
  while (true) {
    int diff = *(uint8_t *)s1 - *(uint8_t *)s2;
    if (likely(diff != 0) || *s1 == '\0')
      return diff;
    s1 += 1;
    s2 += 1;
  }
}

int __hot mdbx_e2k_strncmp_bug_workaround(const char *s1, const char *s2,
                                          size_t n) {
  while (n > 0) {
    int diff = *(uint8_t *)s1 - *(uint8_t *)s2;
    if (likely(diff != 0) || *s1 == '\0')
      return diff;
    s1 += 1;
    s2 += 1;
    n -= 1;
  }
  return 0;
}

size_t __hot mdbx_e2k_strlen_bug_workaround(const char *s) {
  size_t n = 0;
  while (*s) {
    s += 1;
    n += 1;
  }
  return n;
}

size_t __hot mdbx_e2k_strnlen_bug_workaround(const char *s, size_t maxlen) {
  size_t n = 0;
  while (maxlen > n && *s) {
    s += 1;
    n += 1;
  }
  return n;
}
#endif /* MDBX_E2K_MLHCPB_WORKAROUND */

/*------------------------------------------------------------------------------
 * safe read/write volatile 64-bit fields on 32-bit architectures. */

MDBX_MAYBE_UNUSED static __always_inline uint64_t
atomic_store64(MDBX_atomic_uint64_t *p, const uint64_t value,
               enum MDBX_memory_order order) {
  STATIC_ASSERT(sizeof(MDBX_atomic_uint64_t) == 8);
#if MDBX_64BIT_ATOMIC
#ifdef MDBX_HAVE_C11ATOMICS
  assert(atomic_is_lock_free(MDBX_c11a_rw(uint64_t, p)));
  atomic_store_explicit(MDBX_c11a_rw(uint64_t, p), value, mo_c11_store(order));
#else  /* MDBX_HAVE_C11ATOMICS */
  if (order != mo_Relaxed)
    mdbx_compiler_barrier();
  p->weak = value;
  mdbx_memory_fence(order, true);
#endif /* MDBX_HAVE_C11ATOMICS */
#else  /* !MDBX_64BIT_ATOMIC */
  mdbx_compiler_barrier();
  atomic_store32(&p->low, (uint32_t)value, mo_Relaxed);
  mdbx_jitter4testing(true);
  atomic_store32(&p->high, (uint32_t)(value >> 32), order);
  mdbx_jitter4testing(true);
#endif /* !MDBX_64BIT_ATOMIC */
  return value;
}

MDBX_MAYBE_UNUSED static
#if MDBX_64BIT_ATOMIC
    __always_inline
#endif /* MDBX_64BIT_ATOMIC */
        uint64_t
        atomic_load64(const MDBX_atomic_uint64_t *p,
                      enum MDBX_memory_order order) {
  STATIC_ASSERT(sizeof(MDBX_atomic_uint64_t) == 8);
#if MDBX_64BIT_ATOMIC
#ifdef MDBX_HAVE_C11ATOMICS
  assert(atomic_is_lock_free(MDBX_c11a_ro(uint64_t, p)));
  return atomic_load_explicit(MDBX_c11a_ro(uint64_t, p), mo_c11_load(order));
#else  /* MDBX_HAVE_C11ATOMICS */
  mdbx_memory_fence(order, false);
  const uint64_t value = p->weak;
  if (order != mo_Relaxed)
    mdbx_compiler_barrier();
  return value;
#endif /* MDBX_HAVE_C11ATOMICS */
#else  /* !MDBX_64BIT_ATOMIC */
  mdbx_compiler_barrier();
  uint64_t value = (uint64_t)atomic_load32(&p->high, order) << 32;
  mdbx_jitter4testing(true);
  value |= atomic_load32(&p->low, (order == mo_Relaxed) ? mo_Relaxed
                                                        : mo_AcquireRelease);
  mdbx_jitter4testing(true);
  for (;;) {
    mdbx_compiler_barrier();
    uint64_t again = (uint64_t)atomic_load32(&p->high, order) << 32;
    mdbx_jitter4testing(true);
    again |= atomic_load32(&p->low, (order == mo_Relaxed) ? mo_Relaxed
                                                          : mo_AcquireRelease);
    mdbx_jitter4testing(true);
    if (likely(value == again))
      return value;
    value = again;
  }
#endif /* !MDBX_64BIT_ATOMIC */
}

static __always_inline void atomic_yield(void) {
#if defined(_WIN32) || defined(_WIN64)
  YieldProcessor();
#elif defined(__ia32__) || defined(__e2k__)
  __builtin_ia32_pause();
#elif defined(__ia64__)
#if defined(__HP_cc__) || defined(__HP_aCC__)
  _Asm_hint(_HINT_PAUSE);
#else
  __asm__ __volatile__("hint @pause");
#endif
#elif defined(__aarch64__) || (defined(__ARM_ARCH) && __ARM_ARCH > 6) ||       \
    defined(__ARM_ARCH_6K__)
#ifdef __CC_ARM
  __yield();
#else
  __asm__ __volatile__("yield");
#endif
#elif (defined(__mips64) || defined(__mips64__)) && defined(__mips_isa_rev) && \
    __mips_isa_rev >= 2
  __asm__ __volatile__("pause");
#elif defined(__mips) || defined(__mips__) || defined(__mips64) ||             \
    defined(__mips64__) || defined(_M_MRX000) || defined(_MIPS_) ||            \
    defined(__MWERKS__) || defined(__sgi)
  __asm__ __volatile__(".word 0x00000140");
#elif defined(__linux__) || defined(__gnu_linux__) || defined(_UNIX03_SOURCE)
  sched_yield();
#elif (defined(_GNU_SOURCE) && __GLIBC_PREREQ(2, 1)) || defined(_OPEN_THREADS)
  pthread_yield();
#endif
}

#if MDBX_64BIT_CAS
static __always_inline bool atomic_cas64(MDBX_atomic_uint64_t *p, uint64_t c,
                                         uint64_t v) {
#ifdef MDBX_HAVE_C11ATOMICS
  STATIC_ASSERT(sizeof(long long) >= sizeof(uint64_t));
#ifdef ATOMIC_LLONG_LOCK_FREE
  STATIC_ASSERT(ATOMIC_LLONG_LOCK_FREE > 0);
#if ATOMIC_LLONG_LOCK_FREE < 2
  assert(atomic_is_lock_free(MDBX_c11a_rw(uint64_t, p)));
#endif /* ATOMIC_LLONG_LOCK_FREE < 2 */
#else  /* defined(ATOMIC_LLONG_LOCK_FREE) */
  assert(atomic_is_lock_free(MDBX_c11a_rw(uint64_t, p)));
#endif
  return atomic_compare_exchange_strong(MDBX_c11a_rw(uint64_t, p), &c, v);
#elif defined(__GNUC__) || defined(__clang__)
  return __sync_bool_compare_and_swap(&p->weak, c, v);
#elif defined(_MSC_VER)
  return c == (uint64_t)_InterlockedCompareExchange64(
                  (volatile __int64 *)&p->weak, v, c);
#elif defined(__APPLE__)
  return OSAtomicCompareAndSwap64Barrier(c, v, &p->weak);
#else
#error FIXME: Unsupported compiler
#endif
}
#endif /* MDBX_64BIT_CAS */

static __always_inline bool atomic_cas32(MDBX_atomic_uint32_t *p, uint32_t c,
                                         uint32_t v) {
#ifdef MDBX_HAVE_C11ATOMICS
  STATIC_ASSERT(sizeof(int) >= sizeof(uint32_t));
#ifdef ATOMIC_INT_LOCK_FREE
  STATIC_ASSERT(ATOMIC_INT_LOCK_FREE > 0);
#if ATOMIC_INT_LOCK_FREE < 2
  assert(atomic_is_lock_free(MDBX_c11a_rw(uint32_t, p)));
#endif
#else
  assert(atomic_is_lock_free(MDBX_c11a_rw(uint32_t, p)));
#endif
  return atomic_compare_exchange_strong(MDBX_c11a_rw(uint32_t, p), &c, v);
#elif defined(__GNUC__) || defined(__clang__)
  return __sync_bool_compare_and_swap(&p->weak, c, v);
#elif defined(_MSC_VER)
  STATIC_ASSERT(sizeof(volatile long) == sizeof(volatile uint32_t));
  return c ==
         (uint32_t)_InterlockedCompareExchange((volatile long *)&p->weak, v, c);
#elif defined(__APPLE__)
  return OSAtomicCompareAndSwap32Barrier(c, v, &p->weak);
#else
#error FIXME: Unsupported compiler
#endif
}

static __always_inline uint32_t atomic_add32(MDBX_atomic_uint32_t *p,
                                             uint32_t v) {
#ifdef MDBX_HAVE_C11ATOMICS
  STATIC_ASSERT(sizeof(int) >= sizeof(uint32_t));
#ifdef ATOMIC_INT_LOCK_FREE
  STATIC_ASSERT(ATOMIC_INT_LOCK_FREE > 0);
#if ATOMIC_INT_LOCK_FREE < 2
  assert(atomic_is_lock_free(MDBX_c11a_rw(uint32_t, p)));
#endif
#else
  assert(atomic_is_lock_free(MDBX_c11a_rw(uint32_t, p)));
#endif
  return atomic_fetch_add(MDBX_c11a_rw(uint32_t, p), v);
#elif defined(__GNUC__) || defined(__clang__)
  return __sync_fetch_and_add(&p->weak, v);
#elif defined(_MSC_VER)
  STATIC_ASSERT(sizeof(volatile long) == sizeof(volatile uint32_t));
  return (uint32_t)_InterlockedExchangeAdd((volatile long *)&p->weak, v);
#elif defined(__APPLE__)
  return OSAtomicAdd32Barrier(v, &p->weak);
#else
#error FIXME: Unsupported compiler
#endif
}

#define atomic_sub32(p, v) atomic_add32(p, 0 - (v))

static __always_inline uint64_t safe64_txnid_next(uint64_t txnid) {
  txnid += xMDBX_TXNID_STEP;
#if !MDBX_64BIT_CAS
  /* avoid overflow of low-part in safe64_reset() */
  txnid += (UINT32_MAX == (uint32_t)txnid);
#endif
  return txnid;
}

static __always_inline void safe64_reset(MDBX_atomic_uint64_t *p,
                                         bool single_writer) {
#if !MDBX_64BIT_CAS
  if (!single_writer) {
    STATIC_ASSERT(xMDBX_TXNID_STEP > 1);
    /* it is safe to increment low-part to avoid ABA, since xMDBX_TXNID_STEP > 1
     * and overflow was preserved in safe64_txnid_next() */
    atomic_add32(&p->low, 1) /* avoid ABA in safe64_reset_compare() */;
    atomic_store32(
        &p->high, UINT32_MAX,
        mo_Relaxed) /* atomically make >= SAFE64_INVALID_THRESHOLD */;
    atomic_add32(&p->low, 1) /* avoid ABA in safe64_reset_compare() */;
  } else
#endif /* !MDBX_64BIT_CAS */
#if MDBX_64BIT_ATOMIC
    /* atomically make value >= SAFE64_INVALID_THRESHOLD by 64-bit operation */
    atomic_store64(p, UINT64_MAX,
                   single_writer ? mo_AcquireRelease
                                 : mo_SequentialConsistency);
#else
  /* atomically make value >= SAFE64_INVALID_THRESHOLD by 32-bit operation */
  atomic_store32(&p->high, UINT32_MAX,
                 single_writer ? mo_AcquireRelease : mo_SequentialConsistency);
#endif /* MDBX_64BIT_ATOMIC */
  assert(p->weak >= SAFE64_INVALID_THRESHOLD);
  mdbx_jitter4testing(true);
}

static __always_inline bool safe64_reset_compare(MDBX_atomic_uint64_t *p,
                                                 txnid_t compare) {
  /* LY: This function is used to reset `mr_txnid` from hsr-handler in case
   *     the asynchronously cancellation of read transaction. Therefore,
   *     there may be a collision between the cleanup performed here and
   *     asynchronous termination and restarting of the read transaction
   *     in another proces/thread. In general we MUST NOT reset the `mr_txnid`
   *     if a new transaction was started (i.e. if `mr_txnid` was changed). */
#if MDBX_64BIT_CAS
  bool rc = atomic_cas64(p, compare, UINT64_MAX);
#else
  /* LY: There is no gold ratio here since shared mutex is too costly,
   *     in such way we must acquire/release it for every update of mr_txnid,
   *     i.e. twice for each read transaction). */
  bool rc = false;
  if (likely(atomic_load32(&p->low, mo_AcquireRelease) == (uint32_t)compare &&
             atomic_cas32(&p->high, (uint32_t)(compare >> 32), UINT32_MAX))) {
    if (unlikely(atomic_load32(&p->low, mo_AcquireRelease) !=
                 (uint32_t)compare))
      atomic_cas32(&p->high, UINT32_MAX, (uint32_t)(compare >> 32));
    else
      rc = true;
  }
#endif /* MDBX_64BIT_CAS */
  mdbx_jitter4testing(true);
  return rc;
}

static __always_inline void safe64_write(MDBX_atomic_uint64_t *p,
                                         const uint64_t v) {
  assert(p->weak >= SAFE64_INVALID_THRESHOLD);
#if MDBX_64BIT_ATOMIC
  atomic_store64(p, v, mo_AcquireRelease);
#else  /* MDBX_64BIT_ATOMIC */
  mdbx_compiler_barrier();
  /* update low-part but still value >= SAFE64_INVALID_THRESHOLD */
  atomic_store32(&p->low, (uint32_t)v, mo_Relaxed);
  assert(p->weak >= SAFE64_INVALID_THRESHOLD);
  mdbx_jitter4testing(true);
  /* update high-part from SAFE64_INVALID_THRESHOLD to actual value */
  atomic_store32(&p->high, (uint32_t)(v >> 32), mo_AcquireRelease);
#endif /* MDBX_64BIT_ATOMIC */
  assert(p->weak == v);
  mdbx_jitter4testing(true);
}

static __always_inline uint64_t safe64_read(const MDBX_atomic_uint64_t *p) {
  mdbx_jitter4testing(true);
  uint64_t v = atomic_load64(p, mo_AcquireRelease);
  mdbx_jitter4testing(true);
  return v;
}

#if 0 /* unused for now */
MDBX_MAYBE_UNUSED static __always_inline bool safe64_is_valid(uint64_t v) {
#if MDBX_WORDBITS >= 64
  return v < SAFE64_INVALID_THRESHOLD;
#else
  return (v >> 32) != UINT32_MAX;
#endif /* MDBX_WORDBITS */
}

MDBX_MAYBE_UNUSED static __always_inline bool
 safe64_is_valid_ptr(const MDBX_atomic_uint64_t *p) {
#if MDBX_64BIT_ATOMIC
  return atomic_load64(p, mo_AcquireRelease) < SAFE64_INVALID_THRESHOLD;
#else
  return atomic_load32(&p->high, mo_AcquireRelease) != UINT32_MAX;
#endif /* MDBX_64BIT_ATOMIC */
}
#endif /* unused for now */

/* non-atomic write with safety for reading a half-updated value */
static __always_inline void safe64_update(MDBX_atomic_uint64_t *p,
                                          const uint64_t v) {
#if MDBX_64BIT_ATOMIC
  atomic_store64(p, v, mo_Relaxed);
#else
  safe64_reset(p, true);
  safe64_write(p, v);
#endif /* MDBX_64BIT_ATOMIC */
}

/* non-atomic increment with safety for reading a half-updated value */
MDBX_MAYBE_UNUSED static
#if MDBX_64BIT_ATOMIC
    __always_inline
#endif /* MDBX_64BIT_ATOMIC */
    void
    safe64_inc(MDBX_atomic_uint64_t *p, const uint64_t v) {
  assert(v > 0);
  safe64_update(p, atomic_load64(p, mo_Relaxed) + v);
}

/*----------------------------------------------------------------------------*/
/* rthc (tls keys and destructors) */

typedef struct rthc_entry_t {
  MDBX_reader *begin;
  MDBX_reader *end;
  mdbx_thread_key_t thr_tls_key;
  bool key_valid;
} rthc_entry_t;

#if MDBX_DEBUG
#define RTHC_INITIAL_LIMIT 1
#else
#define RTHC_INITIAL_LIMIT 16
#endif

static bin128_t bootid;

#if defined(_WIN32) || defined(_WIN64)
static CRITICAL_SECTION rthc_critical_section;
static CRITICAL_SECTION lcklist_critical_section;
#else
int __cxa_thread_atexit_impl(void (*dtor)(void *), void *obj, void *dso_symbol)
    __attribute__((__weak__));
#ifdef __APPLE__ /* FIXME: Thread-Local Storage destructors & DSO-unloading */
int __cxa_thread_atexit_impl(void (*dtor)(void *), void *obj,
                             void *dso_symbol) {
  (void)dtor;
  (void)obj;
  (void)dso_symbol;
  return -1;
}
#endif           /* __APPLE__ */

static pthread_mutex_t lcklist_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rthc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rthc_cond = PTHREAD_COND_INITIALIZER;
static mdbx_thread_key_t rthc_key;
static MDBX_atomic_uint32_t rthc_pending;

__cold static void workaround_glibc_bug21031(void) {
  /* Workaround for https://sourceware.org/bugzilla/show_bug.cgi?id=21031
   *
   * Due race between pthread_key_delete() and __nptl_deallocate_tsd()
   * The destructor(s) of thread-local-storage object(s) may be running
   * in another thread(s) and be blocked or not finished yet.
   * In such case we get a SEGFAULT after unload this library DSO.
   *
   * So just by yielding a few timeslices we give a chance
   * to such destructor(s) for completion and avoids segfault. */
  sched_yield();
  sched_yield();
  sched_yield();
}
#endif

static unsigned rthc_count, rthc_limit;
static rthc_entry_t *rthc_table;
static rthc_entry_t rthc_table_static[RTHC_INITIAL_LIMIT];

static __inline void rthc_lock(void) {
#if defined(_WIN32) || defined(_WIN64)
  EnterCriticalSection(&rthc_critical_section);
#else
  mdbx_ensure(nullptr, pthread_mutex_lock(&rthc_mutex) == 0);
#endif
}

static __inline void rthc_unlock(void) {
#if defined(_WIN32) || defined(_WIN64)
  LeaveCriticalSection(&rthc_critical_section);
#else
  mdbx_ensure(nullptr, pthread_mutex_unlock(&rthc_mutex) == 0);
#endif
}

static __inline int thread_key_create(mdbx_thread_key_t *key) {
  int rc;
#if defined(_WIN32) || defined(_WIN64)
  *key = TlsAlloc();
  rc = (*key != TLS_OUT_OF_INDEXES) ? MDBX_SUCCESS : GetLastError();
#else
  rc = pthread_key_create(key, nullptr);
#endif
  mdbx_trace("&key = %p, value %" PRIuPTR ", rc %d",
             __Wpedantic_format_voidptr(key), (uintptr_t)*key, rc);
  return rc;
}

static __inline void thread_key_delete(mdbx_thread_key_t key) {
  mdbx_trace("key = %" PRIuPTR, (uintptr_t)key);
#if defined(_WIN32) || defined(_WIN64)
  mdbx_ensure(nullptr, TlsFree(key));
#else
  mdbx_ensure(nullptr, pthread_key_delete(key) == 0);
  workaround_glibc_bug21031();
#endif
}

static __inline void *thread_rthc_get(mdbx_thread_key_t key) {
#if defined(_WIN32) || defined(_WIN64)
  return TlsGetValue(key);
#else
  return pthread_getspecific(key);
#endif
}

static void thread_rthc_set(mdbx_thread_key_t key, const void *value) {
#if defined(_WIN32) || defined(_WIN64)
  mdbx_ensure(nullptr, TlsSetValue(key, (void *)value));
#else
#define MDBX_THREAD_RTHC_ZERO 0
#define MDBX_THREAD_RTHC_REGISTERED 1
#define MDBX_THREAD_RTHC_COUNTED 2
  static __thread char thread_registration_state;
  if (value && unlikely(thread_registration_state == MDBX_THREAD_RTHC_ZERO)) {
    thread_registration_state = MDBX_THREAD_RTHC_REGISTERED;
    mdbx_trace("thread registered 0x%" PRIxPTR, mdbx_thread_self());
    if (&__cxa_thread_atexit_impl == nullptr ||
        __cxa_thread_atexit_impl(mdbx_rthc_thread_dtor,
                                 &thread_registration_state,
                                 (void *)&mdbx_version /* dso_anchor */)) {
      mdbx_ensure(nullptr, pthread_setspecific(
                               rthc_key, &thread_registration_state) == 0);
      thread_registration_state = MDBX_THREAD_RTHC_COUNTED;
      const unsigned count_before = atomic_add32(&rthc_pending, 1);
      mdbx_ensure(nullptr, count_before < INT_MAX);
      mdbx_trace("fallback to pthreads' tsd, key %" PRIuPTR ", count %u",
                 (uintptr_t)rthc_key, count_before);
      (void)count_before;
    }
  }
  mdbx_ensure(nullptr, pthread_setspecific(key, value) == 0);
#endif
}

__cold void mdbx_rthc_global_init(void) {
  rthc_limit = RTHC_INITIAL_LIMIT;
  rthc_table = rthc_table_static;
#if defined(_WIN32) || defined(_WIN64)
  InitializeCriticalSection(&rthc_critical_section);
  InitializeCriticalSection(&lcklist_critical_section);
#else
  mdbx_ensure(nullptr,
              pthread_key_create(&rthc_key, mdbx_rthc_thread_dtor) == 0);
  mdbx_trace("pid %d, &mdbx_rthc_key = %p, value 0x%x", mdbx_getpid(),
             __Wpedantic_format_voidptr(&rthc_key), (unsigned)rthc_key);
#endif
  /* checking time conversion, this also avoids racing on 32-bit architectures
   * during writing calculated 64-bit ratio(s) into memory. */
  uint32_t proba = UINT32_MAX;
  while (true) {
    unsigned time_conversion_checkup =
        mdbx_osal_monotime_to_16dot16(mdbx_osal_16dot16_to_monotime(proba));
    unsigned one_more = (proba < UINT32_MAX) ? proba + 1 : proba;
    unsigned one_less = (proba > 0) ? proba - 1 : proba;
    mdbx_ensure(nullptr, time_conversion_checkup >= one_less &&
                             time_conversion_checkup <= one_more);
    if (proba == 0)
      break;
    proba >>= 1;
  }

  bootid = mdbx_osal_bootid();
#if 0 /* debug */
  for (unsigned i = 0; i < 65536; ++i) {
    size_t pages = pv2pages(i);
    unsigned x = pages2pv(pages);
    size_t xp = pv2pages(x);
    if (!(x == i || (x % 2 == 0 && x < 65536)) || pages != xp)
      printf("%u => %zu => %u => %zu\n", i, pages, x, xp);
    assert(pages == xp);
  }
  fflush(stdout);
#endif
}

/* dtor called for thread, i.e. for all mdbx's environment objects */
__cold void mdbx_rthc_thread_dtor(void *ptr) {
  rthc_lock();
  mdbx_trace(">> pid %d, thread 0x%" PRIxPTR ", rthc %p", mdbx_getpid(),
             mdbx_thread_self(), ptr);

  const uint32_t self_pid = mdbx_getpid();
  for (unsigned i = 0; i < rthc_count; ++i) {
    if (!rthc_table[i].key_valid)
      continue;
    const mdbx_thread_key_t key = rthc_table[i].thr_tls_key;
    MDBX_reader *const rthc = thread_rthc_get(key);
    if (rthc < rthc_table[i].begin || rthc >= rthc_table[i].end)
      continue;
#if !defined(_WIN32) && !defined(_WIN64)
    if (pthread_setspecific(key, nullptr) != 0) {
      mdbx_trace("== thread 0x%" PRIxPTR
                 ", rthc %p: ignore race with tsd-key deletion",
                 mdbx_thread_self(), ptr);
      continue /* ignore race with tsd-key deletion by mdbx_env_close() */;
    }
#endif

    mdbx_trace("== thread 0x%" PRIxPTR
               ", rthc %p, [%i], %p ... %p (%+i), rtch-pid %i, "
               "current-pid %i",
               mdbx_thread_self(), __Wpedantic_format_voidptr(rthc), i,
               __Wpedantic_format_voidptr(rthc_table[i].begin),
               __Wpedantic_format_voidptr(rthc_table[i].end),
               (int)(rthc - rthc_table[i].begin), rthc->mr_pid.weak, self_pid);
    if (atomic_load32(&rthc->mr_pid, mo_Relaxed) == self_pid) {
      mdbx_trace("==== thread 0x%" PRIxPTR ", rthc %p, cleanup",
                 mdbx_thread_self(), __Wpedantic_format_voidptr(rthc));
      atomic_store32(&rthc->mr_pid, 0, mo_AcquireRelease);
    }
  }

#if defined(_WIN32) || defined(_WIN64)
  mdbx_trace("<< thread 0x%" PRIxPTR ", rthc %p", mdbx_thread_self(), ptr);
  rthc_unlock();
#else
  const char self_registration = *(volatile char *)ptr;
  *(volatile char *)ptr = MDBX_THREAD_RTHC_ZERO;
  mdbx_trace("== thread 0x%" PRIxPTR ", rthc %p, pid %d, self-status %d",
             mdbx_thread_self(), ptr, mdbx_getpid(), self_registration);
  if (self_registration == MDBX_THREAD_RTHC_COUNTED)
    mdbx_ensure(nullptr, atomic_sub32(&rthc_pending, 1) > 0);

  if (atomic_load32(&rthc_pending, mo_AcquireRelease) == 0) {
    mdbx_trace("== thread 0x%" PRIxPTR ", rthc %p, pid %d, wake",
               mdbx_thread_self(), ptr, mdbx_getpid());
    mdbx_ensure(nullptr, pthread_cond_broadcast(&rthc_cond) == 0);
  }

  mdbx_trace("<< thread 0x%" PRIxPTR ", rthc %p", mdbx_thread_self(), ptr);
  /* Allow tail call optimization, i.e. gcc should generate the jmp instruction
   * instead of a call for pthread_mutex_unlock() and therefore CPU could not
   * return to current DSO's code section, which may be unloaded immediately
   * after the mutex got released. */
  pthread_mutex_unlock(&rthc_mutex);
#endif
}

__cold void mdbx_rthc_global_dtor(void) {
  mdbx_trace(">> pid %d", mdbx_getpid());

  rthc_lock();
#if !defined(_WIN32) && !defined(_WIN64)
  char *rthc = pthread_getspecific(rthc_key);
  mdbx_trace(
      "== thread 0x%" PRIxPTR ", rthc %p, pid %d, self-status %d, left %d",
      mdbx_thread_self(), __Wpedantic_format_voidptr(rthc), mdbx_getpid(),
      rthc ? *rthc : -1, atomic_load32(&rthc_pending, mo_Relaxed));
  if (rthc) {
    const char self_registration = *rthc;
    *rthc = MDBX_THREAD_RTHC_ZERO;
    if (self_registration == MDBX_THREAD_RTHC_COUNTED)
      mdbx_ensure(nullptr, atomic_sub32(&rthc_pending, 1) > 0);
  }

  struct timespec abstime;
  mdbx_ensure(nullptr, clock_gettime(CLOCK_REALTIME, &abstime) == 0);
  abstime.tv_nsec += 1000000000l / 10;
  if (abstime.tv_nsec >= 1000000000l) {
    abstime.tv_nsec -= 1000000000l;
    abstime.tv_sec += 1;
  }
#if MDBX_DEBUG > 0
  abstime.tv_sec += 600;
#endif

  for (unsigned left;
       (left = atomic_load32(&rthc_pending, mo_AcquireRelease)) > 0;) {
    mdbx_trace("pid %d, pending %u, wait for...", mdbx_getpid(), left);
    const int rc = pthread_cond_timedwait(&rthc_cond, &rthc_mutex, &abstime);
    if (rc && rc != EINTR)
      break;
  }
  thread_key_delete(rthc_key);
#endif

  const uint32_t self_pid = mdbx_getpid();
  for (unsigned i = 0; i < rthc_count; ++i) {
    if (!rthc_table[i].key_valid)
      continue;
    const mdbx_thread_key_t key = rthc_table[i].thr_tls_key;
    thread_key_delete(key);
    for (MDBX_reader *rthc = rthc_table[i].begin; rthc < rthc_table[i].end;
         ++rthc) {
      mdbx_trace(
          "== [%i] = key %" PRIuPTR ", %p ... %p, rthc %p (%+i), "
          "rthc-pid %i, current-pid %i",
          i, (uintptr_t)key, __Wpedantic_format_voidptr(rthc_table[i].begin),
          __Wpedantic_format_voidptr(rthc_table[i].end),
          __Wpedantic_format_voidptr(rthc), (int)(rthc - rthc_table[i].begin),
          rthc->mr_pid.weak, self_pid);
      if (atomic_load32(&rthc->mr_pid, mo_Relaxed) == self_pid) {
        atomic_store32(&rthc->mr_pid, 0, mo_AcquireRelease);
        mdbx_trace("== cleanup %p", __Wpedantic_format_voidptr(rthc));
      }
    }
  }

  rthc_limit = rthc_count = 0;
  if (rthc_table != rthc_table_static)
    mdbx_free(rthc_table);
  rthc_table = nullptr;
  rthc_unlock();

#if defined(_WIN32) || defined(_WIN64)
  DeleteCriticalSection(&lcklist_critical_section);
  DeleteCriticalSection(&rthc_critical_section);
#else
  /* LY: yielding a few timeslices to give a more chance
   * to racing destructor(s) for completion. */
  workaround_glibc_bug21031();
#endif

  mdbx_trace("<< pid %d\n", mdbx_getpid());
}

__cold int mdbx_rthc_alloc(mdbx_thread_key_t *key, MDBX_reader *begin,
                           MDBX_reader *end) {
  int rc;
  if (key) {
#ifndef NDEBUG
    *key = (mdbx_thread_key_t)0xBADBADBAD;
#endif /* NDEBUG */
    rc = thread_key_create(key);
    if (rc != MDBX_SUCCESS)
      return rc;
  }

  rthc_lock();
  const mdbx_thread_key_t new_key = key ? *key : 0;
  mdbx_trace(">> key %" PRIuPTR ", rthc_count %u, rthc_limit %u",
             (uintptr_t)new_key, rthc_count, rthc_limit);
  if (rthc_count == rthc_limit) {
    rthc_entry_t *new_table =
        mdbx_realloc((rthc_table == rthc_table_static) ? nullptr : rthc_table,
                     sizeof(rthc_entry_t) * rthc_limit * 2);
    if (new_table == nullptr) {
      rc = MDBX_ENOMEM;
      goto bailout;
    }
    if (rthc_table == rthc_table_static)
      memcpy(new_table, rthc_table_static, sizeof(rthc_table_static));
    rthc_table = new_table;
    rthc_limit *= 2;
  }
  mdbx_trace("== [%i] = key %" PRIuPTR ", %p ... %p", rthc_count,
             (uintptr_t)new_key, __Wpedantic_format_voidptr(begin),
             __Wpedantic_format_voidptr(end));
  rthc_table[rthc_count].key_valid = key ? true : false;
  rthc_table[rthc_count].thr_tls_key = key ? new_key : 0;
  rthc_table[rthc_count].begin = begin;
  rthc_table[rthc_count].end = end;
  ++rthc_count;
  mdbx_trace("<< key %" PRIuPTR ", rthc_count %u, rthc_limit %u",
             (uintptr_t)new_key, rthc_count, rthc_limit);
  rthc_unlock();
  return MDBX_SUCCESS;

bailout:
  if (key)
    thread_key_delete(*key);
  rthc_unlock();
  return rc;
}

__cold void mdbx_rthc_remove(const mdbx_thread_key_t key) {
  thread_key_delete(key);
  rthc_lock();
  mdbx_trace(">> key %zu, rthc_count %u, rthc_limit %u", (size_t)key,
             rthc_count, rthc_limit);

  for (unsigned i = 0; i < rthc_count; ++i) {
    if (rthc_table[i].key_valid && key == rthc_table[i].thr_tls_key) {
      const uint32_t self_pid = mdbx_getpid();
      mdbx_trace("== [%i], %p ...%p, current-pid %d", i,
                 __Wpedantic_format_voidptr(rthc_table[i].begin),
                 __Wpedantic_format_voidptr(rthc_table[i].end), self_pid);

      for (MDBX_reader *rthc = rthc_table[i].begin; rthc < rthc_table[i].end;
           ++rthc) {
        if (atomic_load32(&rthc->mr_pid, mo_Relaxed) == self_pid) {
          atomic_store32(&rthc->mr_pid, 0, mo_AcquireRelease);
          mdbx_trace("== cleanup %p", __Wpedantic_format_voidptr(rthc));
        }
      }
      if (--rthc_count > 0)
        rthc_table[i] = rthc_table[rthc_count];
      else if (rthc_table != rthc_table_static) {
        mdbx_free(rthc_table);
        rthc_table = rthc_table_static;
        rthc_limit = RTHC_INITIAL_LIMIT;
      }
      break;
    }
  }

  mdbx_trace("<< key %zu, rthc_count %u, rthc_limit %u", (size_t)key,
             rthc_count, rthc_limit);
  rthc_unlock();
}

//------------------------------------------------------------------------------

#define RTHC_ENVLIST_END ((MDBX_env *)((uintptr_t)50459))
static MDBX_env *inprocess_lcklist_head = RTHC_ENVLIST_END;

static __inline void lcklist_lock(void) {
#if defined(_WIN32) || defined(_WIN64)
  EnterCriticalSection(&lcklist_critical_section);
#else
  mdbx_ensure(nullptr, pthread_mutex_lock(&lcklist_mutex) == 0);
#endif
}

static __inline void lcklist_unlock(void) {
#if defined(_WIN32) || defined(_WIN64)
  LeaveCriticalSection(&lcklist_critical_section);
#else
  mdbx_ensure(nullptr, pthread_mutex_unlock(&lcklist_mutex) == 0);
#endif
}

MDBX_NOTHROW_CONST_FUNCTION static uint64_t rrxmrrxmsx_0(uint64_t v) {
  /* Pelle Evensen's mixer, https://bit.ly/2HOfynt */
  v ^= (v << 39 | v >> 25) ^ (v << 14 | v >> 50);
  v *= UINT64_C(0xA24BAED4963EE407);
  v ^= (v << 40 | v >> 24) ^ (v << 15 | v >> 49);
  v *= UINT64_C(0x9FB21C651E98DF25);
  return v ^ v >> 28;
}

static int uniq_peek(const mdbx_mmap_t *pending, mdbx_mmap_t *scan) {
  int rc;
  uint64_t bait;
  MDBX_lockinfo *const pending_lck = pending->lck;
  MDBX_lockinfo *const scan_lck = scan->lck;
  if (pending_lck) {
    bait = atomic_load64(&pending_lck->mti_bait_uniqueness, mo_AcquireRelease);
    rc = MDBX_SUCCESS;
  } else {
    bait = 0 /* hush MSVC warning */;
    rc = mdbx_msync(scan, 0, sizeof(MDBX_lockinfo), MDBX_SYNC_DATA);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_pread(pending->fd, &bait, sizeof(scan_lck->mti_bait_uniqueness),
                      offsetof(MDBX_lockinfo, mti_bait_uniqueness));
  }
  if (likely(rc == MDBX_SUCCESS) &&
      bait == atomic_load64(&scan_lck->mti_bait_uniqueness, mo_AcquireRelease))
    rc = MDBX_RESULT_TRUE;

  mdbx_trace("uniq-peek: %s, bait 0x%016" PRIx64 ",%s rc %d",
             pending_lck ? "mem" : "file", bait,
             (rc == MDBX_RESULT_TRUE) ? " found," : (rc ? " FAILED," : ""), rc);
  return rc;
}

static int uniq_poke(const mdbx_mmap_t *pending, mdbx_mmap_t *scan,
                     uint64_t *abra) {
  if (*abra == 0) {
    const uintptr_t tid = mdbx_thread_self();
    uintptr_t uit = 0;
    memcpy(&uit, &tid, (sizeof(tid) < sizeof(uit)) ? sizeof(tid) : sizeof(uit));
    *abra =
        rrxmrrxmsx_0(mdbx_osal_monotime() + UINT64_C(5873865991930747) * uit);
  }
  const uint64_t cadabra =
      rrxmrrxmsx_0(*abra + UINT64_C(7680760450171793) * (unsigned)mdbx_getpid())
          << 24 |
      *abra >> 40;
  MDBX_lockinfo *const scan_lck = scan->lck;
  atomic_store64(&scan_lck->mti_bait_uniqueness, cadabra,
                 mo_SequentialConsistency);
  *abra = *abra * UINT64_C(6364136223846793005) + 1;
  return uniq_peek(pending, scan);
}

__cold static int uniq_check(const mdbx_mmap_t *pending, MDBX_env **found) {
  *found = nullptr;
  uint64_t salt = 0;
  for (MDBX_env *scan = inprocess_lcklist_head; scan != RTHC_ENVLIST_END;
       scan = scan->me_lcklist_next) {
    MDBX_lockinfo *const scan_lck = scan->me_lck_mmap.lck;
    int err = atomic_load64(&scan_lck->mti_bait_uniqueness, mo_AcquireRelease)
                  ? uniq_peek(pending, &scan->me_lck_mmap)
                  : uniq_poke(pending, &scan->me_lck_mmap, &salt);
    if (err == MDBX_ENODATA) {
      uint64_t length;
      if (likely(mdbx_filesize(pending->fd, &length) == MDBX_SUCCESS &&
                 length == 0)) {
        /* LY: skip checking since LCK-file is empty, i.e. just created. */
        mdbx_debug("uniq-probe: %s", "unique (new/empty lck)");
        return MDBX_RESULT_TRUE;
      }
    }
    if (err == MDBX_RESULT_TRUE)
      err = uniq_poke(pending, &scan->me_lck_mmap, &salt);
    if (err == MDBX_RESULT_TRUE) {
      (void)mdbx_msync(&scan->me_lck_mmap, 0, sizeof(MDBX_lockinfo),
                       MDBX_SYNC_NONE);
      err = uniq_poke(pending, &scan->me_lck_mmap, &salt);
    }
    if (err == MDBX_RESULT_TRUE) {
      err = uniq_poke(pending, &scan->me_lck_mmap, &salt);
      *found = scan;
      mdbx_debug("uniq-probe: found %p", __Wpedantic_format_voidptr(*found));
      return MDBX_RESULT_FALSE;
    }
    if (unlikely(err != MDBX_SUCCESS)) {
      mdbx_debug("uniq-probe: failed rc %d", err);
      return err;
    }
  }

  mdbx_debug("uniq-probe: %s", "unique");
  return MDBX_RESULT_TRUE;
}

static int lcklist_detach_locked(MDBX_env *env) {
  MDBX_env *inprocess_neighbor = nullptr;
  int rc = MDBX_SUCCESS;
  if (env->me_lcklist_next != nullptr) {
    mdbx_ensure(env, env->me_lcklist_next != nullptr);
    mdbx_ensure(env, inprocess_lcklist_head != RTHC_ENVLIST_END);
    for (MDBX_env **ptr = &inprocess_lcklist_head; *ptr != RTHC_ENVLIST_END;
         ptr = &(*ptr)->me_lcklist_next) {
      if (*ptr == env) {
        *ptr = env->me_lcklist_next;
        env->me_lcklist_next = nullptr;
        break;
      }
    }
    mdbx_ensure(env, env->me_lcklist_next == nullptr);
  }

  rc = likely(mdbx_getpid() == env->me_pid)
           ? uniq_check(&env->me_lck_mmap, &inprocess_neighbor)
           : MDBX_PANIC;
  if (!inprocess_neighbor && env->me_live_reader)
    (void)mdbx_rpid_clear(env);
  if (!MDBX_IS_ERROR(rc))
    rc = mdbx_lck_destroy(env, inprocess_neighbor);
  return rc;
}

/*------------------------------------------------------------------------------
 * LY: State of the art quicksort-based sorting, with internal stack
 * and network-sort for small chunks.
 * Thanks to John M. Gamble for the http://pages.ripco.net/~jgamble/nw.html */

#define SORT_CMP_SWAP(TYPE, CMP, a, b)                                         \
  do {                                                                         \
    const TYPE swap_tmp = (a);                                                 \
    const bool swap_cmp = CMP(swap_tmp, b);                                    \
    (a) = swap_cmp ? swap_tmp : b;                                             \
    (b) = swap_cmp ? b : swap_tmp;                                             \
  } while (0)

//  3 comparators, 3 parallel operations
//  o-----^--^--o
//        |  |
//  o--^--|--v--o
//     |  |
//  o--v--v-----o
//
//  [[1,2]]
//  [[0,2]]
//  [[0,1]]
#define SORT_NETWORK_3(TYPE, CMP, begin)                                       \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
  } while (0)

//  5 comparators, 3 parallel operations
//  o--^--^--------o
//     |  |
//  o--v--|--^--^--o
//        |  |  |
//  o--^--v--|--v--o
//     |     |
//  o--v-----v-----o
//
//  [[0,1],[2,3]]
//  [[0,2],[1,3]]
//  [[1,2]]
#define SORT_NETWORK_4(TYPE, CMP, begin)                                       \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
  } while (0)

//  9 comparators, 5 parallel operations
//  o--^--^-----^-----------o
//     |  |     |
//  o--|--|--^--v-----^--^--o
//     |  |  |        |  |
//  o--|--v--|--^--^--|--v--o
//     |     |  |  |  |
//  o--|-----v--|--v--|--^--o
//     |        |     |  |
//  o--v--------v-----v--v--o
//
//  [[0,4],[1,3]]
//  [[0,2]]
//  [[2,4],[0,1]]
//  [[2,3],[1,4]]
//  [[1,2],[3,4]]
#define SORT_NETWORK_5(TYPE, CMP, begin)                                       \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
  } while (0)

//  12 comparators, 6 parallel operations
//  o-----^--^--^-----------------o
//        |  |  |
//  o--^--|--v--|--^--------^-----o
//     |  |     |  |        |
//  o--v--v-----|--|--^--^--|--^--o
//              |  |  |  |  |  |
//  o-----^--^--v--|--|--|--v--v--o
//        |  |     |  |  |
//  o--^--|--v-----v--|--v--------o
//     |  |           |
//  o--v--v-----------v-----------o
//
//  [[1,2],[4,5]]
//  [[0,2],[3,5]]
//  [[0,1],[3,4],[2,5]]
//  [[0,3],[1,4]]
//  [[2,4],[1,3]]
//  [[2,3]]
#define SORT_NETWORK_6(TYPE, CMP, begin)                                       \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
  } while (0)

//  16 comparators, 6 parallel operations
//  o--^--------^-----^-----------------o
//     |        |     |
//  o--|--^-----|--^--v--------^--^-----o
//     |  |     |  |           |  |
//  o--|--|--^--v--|--^-----^--|--v-----o
//     |  |  |     |  |     |  |
//  o--|--|--|-----v--|--^--v--|--^--^--o
//     |  |  |        |  |     |  |  |
//  o--v--|--|--^-----v--|--^--v--|--v--o
//        |  |  |        |  |     |
//  o-----v--|--|--------v--v-----|--^--o
//           |  |                 |  |
//  o--------v--v-----------------v--v--o
//
//  [[0,4],[1,5],[2,6]]
//  [[0,2],[1,3],[4,6]]
//  [[2,4],[3,5],[0,1]]
//  [[2,3],[4,5]]
//  [[1,4],[3,6]]
//  [[1,2],[3,4],[5,6]]
#define SORT_NETWORK_7(TYPE, CMP, begin)                                       \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
  } while (0)

//  19 comparators, 6 parallel operations
//  o--^--------^-----^-----------------o
//     |        |     |
//  o--|--^-----|--^--v--------^--^-----o
//     |  |     |  |           |  |
//  o--|--|--^--v--|--^-----^--|--v-----o
//     |  |  |     |  |     |  |
//  o--|--|--|--^--v--|--^--v--|--^--^--o
//     |  |  |  |     |  |     |  |  |
//  o--v--|--|--|--^--v--|--^--v--|--v--o
//        |  |  |  |     |  |     |
//  o-----v--|--|--|--^--v--v-----|--^--o
//           |  |  |  |           |  |
//  o--------v--|--v--|--^--------v--v--o
//              |     |  |
//  o-----------v-----v--v--------------o
//
//  [[0,4],[1,5],[2,6],[3,7]]
//  [[0,2],[1,3],[4,6],[5,7]]
//  [[2,4],[3,5],[0,1],[6,7]]
//  [[2,3],[4,5]]
//  [[1,4],[3,6]]
//  [[1,2],[3,4],[5,6]]
#define SORT_NETWORK_8(TYPE, CMP, begin)                                       \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
  } while (0)

//  25 comparators, 9 parallel operations
//  o--^-----^--^-----^-----------------------------------o
//     |     |  |     |
//  o--v--^--v--|-----|--^-----^-----------^--------------o
//        |     |     |  |     |           |
//  o-----v-----|-----|--|-----|--^-----^--|--^-----^--^--o
//              |     |  |     |  |     |  |  |     |  |
//  o--^-----^--v--^--v--|-----|--|-----|--v--|-----|--v--o
//     |     |     |     |     |  |     |     |     |
//  o--v--^--v-----|-----v--^--v--|-----|-----|--^--v-----o
//        |        |        |     |     |     |  |
//  o-----v--------|--------|-----v--^--v--^--|--|--^-----o
//                 |        |        |     |  |  |  |
//  o--^-----^-----v--------|--------|-----|--v--v--v-----o
//     |     |              |        |     |
//  o--v--^--v--------------v--------|-----v--------------o
//        |                          |
//  o-----v--------------------------v--------------------o
//
//  [[0,1],[3,4],[6,7]]
//  [[1,2],[4,5],[7,8]]
//  [[0,1],[3,4],[6,7],[2,5]]
//  [[0,3],[1,4],[5,8]]
//  [[3,6],[4,7],[2,5]]
//  [[0,3],[1,4],[5,7],[2,6]]
//  [[1,3],[4,6]]
//  [[2,4],[5,6]]
//  [[2,3]]
#define SORT_NETWORK_9(TYPE, CMP, begin)                                       \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
  } while (0)

//  29 comparators, 9 parallel operations
//  o--------------^-----^--^--^-----------------------o
//                 |     |  |  |
//  o-----------^--|--^--|--|--v--^--------^-----------o
//              |  |  |  |  |     |        |
//  o--------^--|--|--|--|--v--^--v-----^--|--^--------o
//           |  |  |  |  |     |        |  |  |
//  o-----^--|--|--|--|--v--^--|-----^--|--v--v--^-----o
//        |  |  |  |  |     |  |     |  |        |
//  o--^--|--|--|--|--v-----|--v--^--|--|--^-----v--^--o
//     |  |  |  |  |        |     |  |  |  |        |
//  o--|--|--|--|--v--^-----|--^--|--v--v--|-----^--v--o
//     |  |  |  |     |     |  |  |        |     |
//  o--|--|--|--v--^--|-----v--|--v--^-----|--^--v-----o
//     |  |  |     |  |        |     |     |  |
//  o--|--|--v-----|--|--^-----v--^--|-----v--v--------o
//     |  |        |  |  |        |  |
//  o--|--v--------|--v--|--^-----v--v-----------------o
//     |           |     |  |
//  o--v-----------v-----v--v--------------------------o
//
//  [[4,9],[3,8],[2,7],[1,6],[0,5]]
//  [[1,4],[6,9],[0,3],[5,8]]
//  [[0,2],[3,6],[7,9]]
//  [[0,1],[2,4],[5,7],[8,9]]
//  [[1,2],[4,6],[7,8],[3,5]]
//  [[2,5],[6,8],[1,3],[4,7]]
//  [[2,3],[6,7]]
//  [[3,4],[5,6]]
//  [[4,5]]
#define SORT_NETWORK_10(TYPE, CMP, begin)                                      \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
  } while (0)

//  35 comparators, 9 parallel operations
//  o--^-----^-----------------^--------^--------------------o
//     |     |                 |        |
//  o--v--^--|--^--^--------^--|--------|--^-----------------o
//        |  |  |  |        |  |        |  |
//  o--^--|--v--v--|-----^--|--|--------|--|-----^--^--------o
//     |  |        |     |  |  |        |  |     |  |
//  o--v--v--------|-----|--|--|--^-----|--|--^--v--|--^--^--o
//                 |     |  |  |  |     |  |  |     |  |  |
//  o--^-----^-----|-----|--|--v--|--^--v--v--|-----v--|--v--o
//     |     |     |     |  |     |  |        |        |
//  o--v--^--|--^--v--^--|--v-----|--|--------|--------v--^--o
//        |  |  |     |  |        |  |        |           |
//  o--^--|--v--v--^--|--v--^-----|--|--------|--------^--v--o
//     |  |        |  |     |     |  |        |        |
//  o--v--v--------|--|-----|-----v--|--^-----|-----^--|--^--o
//                 |  |     |        |  |     |     |  |  |
//  o--^--^--------|--|-----|--------v--|-----v--^--|--v--v--o
//     |  |        |  |     |           |        |  |
//  o--v--|--^-----|--v-----|-----------|--------v--v--------o
//        |  |     |        |           |
//  o-----v--v-----v--------v-----------v--------------------o
//
//  [[0,1],[2,3],[4,5],[6,7],[8,9]]
//  [[1,3],[5,7],[0,2],[4,6],[8,10]]
//  [[1,2],[5,6],[9,10],[0,4],[3,7]]
//  [[1,5],[6,10],[4,8]]
//  [[5,9],[2,6],[0,4],[3,8]]
//  [[1,5],[6,10],[2,3],[8,9]]
//  [[1,4],[7,10],[3,5],[6,8]]
//  [[2,4],[7,9],[5,6]]
//  [[3,4],[7,8]]
#define SORT_NETWORK_11(TYPE, CMP, begin)                                      \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[8]);                              \
  } while (0)

//  39 comparators, parallel operations
//  o--^-----^-----------------^--------^--------------------o
//     |     |                 |        |
//  o--v--^--|--^--^--------^--|--------|--^-----------------o
//        |  |  |  |        |  |        |  |
//  o--^--|--v--v--|-----^--|--|--------|--|-----^--^--------o
//     |  |        |     |  |  |        |  |     |  |
//  o--v--v--------|-----|--|--|--^-----|--|--^--v--|--^--^--o
//                 |     |  |  |  |     |  |  |     |  |  |
//  o--^-----^-----|-----|--|--v--|--^--v--v--|-----v--|--v--o
//     |     |     |     |  |     |  |        |        |
//  o--v--^--|--^--v--^--|--v-----|--|--------|--------v--^--o
//        |  |  |     |  |        |  |        |           |
//  o--^--|--v--v--^--|--v--^-----|--|--------|--------^--v--o
//     |  |        |  |     |     |  |        |        |
//  o--v--v--------|--|-----|--^--v--|--^--^--|-----^--|--^--o
//                 |  |     |  |     |  |  |  |     |  |  |
//  o--^-----^-----|--|-----|--|-----v--|--|--v--^--|--v--v--o
//     |     |     |  |     |  |        |  |     |  |
//  o--v--^--|--^--|--v-----|--|--------|--|-----v--v--------o
//        |  |  |  |        |  |        |  |
//  o--^--|--v--v--v--------v--|--------|--v-----------------o
//     |  |                    |        |
//  o--v--v--------------------v--------v--------------------o
//
//  [[0,1],[2,3],[4,5],[6,7],[8,9],[10,11]]
//  [[1,3],[5,7],[9,11],[0,2],[4,6],[8,10]]
//  [[1,2],[5,6],[9,10],[0,4],[7,11]]
//  [[1,5],[6,10],[3,7],[4,8]]
//  [[5,9],[2,6],[0,4],[7,11],[3,8]]
//  [[1,5],[6,10],[2,3],[8,9]]
//  [[1,4],[7,10],[3,5],[6,8]]
//  [[2,4],[7,9],[5,6]]
//  [[3,4],[7,8]]
#define SORT_NETWORK_12(TYPE, CMP, begin)                                      \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[11]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[8]);                              \
  } while (0)

//  45 comparators, 10 parallel operations
//  o--------^--^-----^-----------------------------^-----------------o
//           |  |     |                             |
//  o--^-----|--v-----|-----^--------------^-----^--|-----^-----------o
//     |     |        |     |              |     |  |     |
//  o--|-----|--^--^--v-----|--------------|--^--|--|--^--v--^--------o
//     |     |  |  |        |              |  |  |  |  |     |
//  o--|--^--|--|--v-----^--|--------^-----|--|--v--|--|--^--v-----^--o
//     |  |  |  |        |  |        |     |  |     |  |  |        |
//  o--|--v--|--|--^-----|--v-----^--v-----|--|--^--|--|--|--^--^--v--o
//     |     |  |  |     |        |        |  |  |  |  |  |  |  |
//  o--|--^--|--|--|--^--|--------|-----^--|--|--|--v--v--v--|--v--^--o
//     |  |  |  |  |  |  |        |     |  |  |  |           |     |
//  o--|--|--|--v--v--|--|--^-----|--^--v--|--v--|--^--------v--^--v--o
//     |  |  |        |  |  |     |  |     |     |  |           |
//  o--v--|--|-----^--|--v--|--^--|--|-----v-----v--|--^--------v-----o
//        |  |     |  |     |  |  |  |              |  |
//  o-----v--|--^--|--|-----|--v--|--|--^-----^-----v--v--^-----------o
//           |  |  |  |     |     |  |  |     |           |
//  o--^-----|--|--|--v-----|-----v--|--v--^--|--^--------v-----------o
//     |     |  |  |        |        |     |  |  |
//  o--|-----|--|--|--^-----|--------v--^--|--v--v--------------------o
//     |     |  |  |  |     |           |  |
//  o--v-----|--v--|--v-----|--^--------v--v--------------------------o
//           |     |        |  |
//  o--------v-----v--------v--v--------------------------------------o
//
//  [[1,7],[9,11],[3,4],[5,8],[0,12],[2,6]]
//  [[0,1],[2,3],[4,6],[8,11],[7,12],[5,9]]
//  [[0,2],[3,7],[10,11],[1,4],[6,12]]
//  [[7,8],[11,12],[4,9],[6,10]]
//  [[3,4],[5,6],[8,9],[10,11],[1,7]]
//  [[2,6],[9,11],[1,3],[4,7],[8,10],[0,5]]
//  [[2,5],[6,8],[9,10]]
//  [[1,2],[3,5],[7,8],[4,6]]
//  [[2,3],[4,5],[6,7],[8,9]]
//  [[3,4],[5,6]]
#define SORT_NETWORK_13(TYPE, CMP, begin)                                      \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[11]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[11], begin[12]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[11]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
  } while (0)

/* *INDENT-OFF* */
/* clang-format off */

//  51 comparators, 10 parallel operations
//  o--^--^-----^-----------^-----------------------------------------------------------o
//     |  |     |           |
//  o--v--|--^--|--^--------|--^-----^-----------------------^--------------------------o
//        |  |  |  |        |  |     |                       |
//  o--^--v--|--|--|--^-----|--|--^--v-----------------------|--^--^--------------------o
//     |     |  |  |  |     |  |  |                          |  |  |
//  o--v-----v--|--|--|--^--|--|--|--^--------------^--------|--|--|--^--^--^-----------o
//              |  |  |  |  |  |  |  |              |        |  |  |  |  |  |
//  o--^--^-----v--|--|--|--|--|--|--|--^-----------|-----^--v--|--v--|--|--v-----------o
//     |  |        |  |  |  |  |  |  |  |           |     |     |     |  |
//  o--v--|--^-----v--|--|--|--|--|--|--|--^--^-----|-----|-----|--^--|--v-----^--------o
//        |  |        |  |  |  |  |  |  |  |  |     |     |     |  |  |        |
//  o--^--v--|--------v--|--|--|--|--|--|--|--|--^--|-----|-----|--v--|-----^--v-----^--o
//     |     |           |  |  |  |  |  |  |  |  |  |     |     |     |     |        |
//  o--v-----v-----------v--|--|--|--|--|--|--|--|--|--^--|--^--|-----|--^--|--^--^--v--o
//                          |  |  |  |  |  |  |  |  |  |  |  |  |     |  |  |  |  |
//  o--^--^-----^-----------v--|--|--|--|--|--|--|--|--|--v--|--v-----v--|--v--|--v--^--o
//     |  |     |              |  |  |  |  |  |  |  |  |     |           |     |     |
//  o--v--|--^--|--^-----------v--|--|--|--|--|--v--|--|-----|--^--------|-----v--^--v--o
//        |  |  |  |              |  |  |  |  |     |  |     |  |        |        |
//  o--^--v--|--|--|--------------v--|--|--|--v-----|--|-----|--v--------|--^-----v-----o
//     |     |  |  |                 |  |  |        |  |     |           |  |
//  o--v-----v--|--|-----------------v--|--|--------|--v-----|--^--------|--|--^--------o
//              |  |                    |  |        |        |  |        |  |  |
//  o--^--------v--|--------------------v--|--------v--------|--|--------v--v--v--------o
//     |           |                       |                 |  |
//  o--v-----------v-----------------------v-----------------v--v-----------------------o
//
//  [[0,1],[2,3],[4,5],[6,7],[8,9],[10,11],[12,13]]
//  [[0,2],[4,6],[8,10],[1,3],[5,7],[9,11]]
//  [[0,4],[8,12],[1,5],[9,13],[2,6],[3,7]]
//  [[0,8],[1,9],[2,10],[3,11],[4,12],[5,13]]
//  [[5,10],[6,9],[3,12],[7,11],[1,2],[4,8]]
//  [[1,4],[7,13],[2,8],[5,6],[9,10]]
//  [[2,4],[11,13],[3,8],[7,12]]
//  [[6,8],[10,12],[3,5],[7,9]]
//  [[3,4],[5,6],[7,8],[9,10],[11,12]]
//  [[6,7],[8,9]]

/* *INDENT-ON* */
/* clang-format on */

#define SORT_NETWORK_14(TYPE, CMP, begin)                                      \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[11]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[12], begin[13]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[13]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[13]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[13]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[11], begin[13]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[12]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[11], begin[12]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
  } while (0)

/* *INDENT-OFF* */
/* clang-format off */

//  56 comparators, 10 parallel operations
//  o--^--^-----^-----------^--------------------------------------------------------------o
//     |  |     |           |
//  o--v--|--^--|--^--------|--^-----^--------------------------^--------------------------o
//        |  |  |  |        |  |     |                          |
//  o--^--v--|--|--|--^-----|--|--^--v--------------------------|--^--^--------------------o
//     |     |  |  |  |     |  |  |                             |  |  |
//  o--v-----v--|--|--|--^--|--|--|--^-----------------^--------|--|--|--^--^--^-----------o
//              |  |  |  |  |  |  |  |                 |        |  |  |  |  |  |
//  o--^--^-----v--|--|--|--|--|--|--|--^--------------|-----^--v--|--v--|--|--v-----------o
//     |  |        |  |  |  |  |  |  |  |              |     |     |     |  |
//  o--v--|--^-----v--|--|--|--|--|--|--|--^-----^-----|-----|-----|--^--|--v-----^--------o
//        |  |        |  |  |  |  |  |  |  |     |     |     |     |  |  |        |
//  o--^--v--|--------v--|--|--|--|--|--|--|--^--|--^--|-----|-----|--v--|-----^--v-----^--o
//     |     |           |  |  |  |  |  |  |  |  |  |  |     |     |     |     |        |
//  o--v-----v-----------v--|--|--|--|--|--|--|--|--|--|--^--|--^--|-----|--^--|--^--^--v--o
//                          |  |  |  |  |  |  |  |  |  |  |  |  |  |     |  |  |  |  |
//  o--^--^-----^-----------v--|--|--|--|--|--|--|--|--|--|--v--|--v-----v--|--v--|--v--^--o
//     |  |     |              |  |  |  |  |  |  |  |  |  |     |           |     |     |
//  o--v--|--^--|--^-----------v--|--|--|--|--|--|--v--|--|-----|--^--------|-----v--^--v--o
//        |  |  |  |              |  |  |  |  |  |     |  |     |  |        |        |
//  o--^--v--|--|--|--^-----------v--|--|--|--|--v-----|--|-----|--v--------|--^-----v-----o
//     |     |  |  |  |              |  |  |  |        |  |     |           |  |
//  o--v-----v--|--|--|--------------v--|--|--|--------|--v-----|--^--^-----|--|--^--------o
//              |  |  |                 |  |  |        |        |  |  |     |  |  |
//  o--^--^-----v--|--|-----------------v--|--|--------v--------|--|--|-----v--v--v--------o
//     |  |        |  |                    |  |                 |  |  |
//  o--v--|--------v--|--------------------v--|--^--------------v--|--v--------------------o
//        |           |                       |  |                 |
//  o-----v-----------v-----------------------v--v-----------------v-----------------------o
//
//  [[0,1],[2,3],[4,5],[6,7],[8,9],[10,11],[12,13]]
//  [[0,2],[4,6],[8,10],[12,14],[1,3],[5,7],[9,11]]
//  [[0,4],[8,12],[1,5],[9,13],[2,6],[10,14],[3,7]]
//  [[0,8],[1,9],[2,10],[3,11],[4,12],[5,13],[6,14]]
//  [[5,10],[6,9],[3,12],[13,14],[7,11],[1,2],[4,8]]
//  [[1,4],[7,13],[2,8],[11,14],[5,6],[9,10]]
//  [[2,4],[11,13],[3,8],[7,12]]
//  [[6,8],[10,12],[3,5],[7,9]]
//  [[3,4],[5,6],[7,8],[9,10],[11,12]]
//  [[6,7],[8,9]]

/* *INDENT-ON* */
/* clang-format on */

#define SORT_NETWORK_15(TYPE, CMP, begin)                                      \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[11]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[12], begin[13]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[12], begin[14]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[13]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[14]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[13]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[14]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[13], begin[14]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[13]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[11], begin[14]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[11], begin[13]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[12]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[11], begin[12]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
  } while (0)

/* *INDENT-OFF* */
/* clang-format off */

//  60 comparators, 10 parallel operations
//  o--^--^-----^-----------^-----------------------------------------------------------------o
//     |  |     |           |
//  o--v--|--^--|--^--------|--^-----^-----------------------------^--------------------------o
//        |  |  |  |        |  |     |                             |
//  o--^--v--|--|--|--^-----|--|--^--v-----------------------------|--^--^--------------------o
//     |     |  |  |  |     |  |  |                                |  |  |
//  o--v-----v--|--|--|--^--|--|--|--^--------------------^--------|--|--|--^--^--^-----------o
//              |  |  |  |  |  |  |  |                    |        |  |  |  |  |  |
//  o--^--^-----v--|--|--|--|--|--|--|--^-----------------|-----^--v--|--v--|--|--v-----------o
//     |  |        |  |  |  |  |  |  |  |                 |     |     |     |  |
//  o--v--|--^-----v--|--|--|--|--|--|--|--^--------^-----|-----|-----|--^--|--v-----^--------o
//        |  |        |  |  |  |  |  |  |  |        |     |     |     |  |  |        |
//  o--^--v--|--------v--|--|--|--|--|--|--|--^-----|--^--|-----|-----|--v--|-----^--v-----^--o
//     |     |           |  |  |  |  |  |  |  |     |  |  |     |     |     |     |        |
//  o--v-----v-----------v--|--|--|--|--|--|--|--^--|--|--|--^--|--^--|-----|--^--|--^--^--v--o
//                          |  |  |  |  |  |  |  |  |  |  |  |  |  |  |     |  |  |  |  |
//  o--^--^-----^-----------v--|--|--|--|--|--|--|--|--|--|--|--v--|--v-----v--|--v--|--v--^--o
//     |  |     |              |  |  |  |  |  |  |  |  |  |  |     |           |     |     |
//  o--v--|--^--|--^-----------v--|--|--|--|--|--|--|--v--|--|-----|--^--------|-----v--^--v--o
//        |  |  |  |              |  |  |  |  |  |  |     |  |     |  |        |        |
//  o--^--v--|--|--|--^-----------v--|--|--|--|--|--v-----|--|-----|--v--------|--^-----v-----o
//     |     |  |  |  |              |  |  |  |  |        |  |     |           |  |
//  o--v-----v--|--|--|--^-----------v--|--|--|--|--------|--v-----|--^--^-----|--|--^--------o
//              |  |  |  |              |  |  |  |        |        |  |  |     |  |  |
//  o--^--^-----v--|--|--|--------------v--|--|--|--------v--------|--|--|-----v--v--v--------o
//     |  |        |  |  |                 |  |  |                 |  |  |
//  o--v--|--^-----v--|--|-----------------v--|--|--^--------------v--|--v--------------------o
//        |  |        |  |                    |  |  |                 |
//  o--^--v--|--------v--|--------------------v--|--v-----------------v-----------------------o
//     |     |           |                       |
//  o--v-----v-----------v-----------------------v--------------------------------------------o
//
//  [[0,1],[2,3],[4,5],[6,7],[8,9],[10,11],[12,13],[14,15]]
//  [[0,2],[4,6],[8,10],[12,14],[1,3],[5,7],[9,11],[13,15]]
//  [[0,4],[8,12],[1,5],[9,13],[2,6],[10,14],[3,7],[11,15]]
//  [[0,8],[1,9],[2,10],[3,11],[4,12],[5,13],[6,14],[7,15]]
//  [[5,10],[6,9],[3,12],[13,14],[7,11],[1,2],[4,8]]
//  [[1,4],[7,13],[2,8],[11,14],[5,6],[9,10]]
//  [[2,4],[11,13],[3,8],[7,12]]
//  [[6,8],[10,12],[3,5],[7,9]]
//  [[3,4],[5,6],[7,8],[9,10],[11,12]]
//  [[6,7],[8,9]]

/* *INDENT-ON* */
/* clang-format on */

#define SORT_NETWORK_16(TYPE, CMP, begin)                                      \
  do {                                                                         \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[11]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[12], begin[13]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[14], begin[15]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[12], begin[14]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[3]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[13], begin[15]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[13]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[14]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[11], begin[15]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[13]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[14]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[15]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[13], begin[14]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[11]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[2]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[4], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[1], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[13]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[11], begin[14]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[2], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[11], begin[13]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[12]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[10], begin[12]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[5]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[9]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[3], begin[4]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[5], begin[6]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[7], begin[8]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[9], begin[10]);                             \
    SORT_CMP_SWAP(TYPE, CMP, begin[11], begin[12]);                            \
    SORT_CMP_SWAP(TYPE, CMP, begin[6], begin[7]);                              \
    SORT_CMP_SWAP(TYPE, CMP, begin[8], begin[9]);                              \
  } while (0)

#define SORT_INNER(TYPE, CMP, begin, end, len)                                 \
  switch (len) {                                                               \
  default:                                                                     \
    __unreachable();                                                           \
  case 0:                                                                      \
  case 1:                                                                      \
    break;                                                                     \
  case 2:                                                                      \
    SORT_CMP_SWAP(TYPE, CMP, begin[0], begin[1]);                              \
    break;                                                                     \
  case 3:                                                                      \
    SORT_NETWORK_3(TYPE, CMP, begin);                                          \
    break;                                                                     \
  case 4:                                                                      \
    SORT_NETWORK_4(TYPE, CMP, begin);                                          \
    break;                                                                     \
  case 5:                                                                      \
    SORT_NETWORK_5(TYPE, CMP, begin);                                          \
    break;                                                                     \
  case 6:                                                                      \
    SORT_NETWORK_6(TYPE, CMP, begin);                                          \
    break;                                                                     \
  case 7:                                                                      \
    SORT_NETWORK_7(TYPE, CMP, begin);                                          \
    break;                                                                     \
  case 8:                                                                      \
    SORT_NETWORK_8(TYPE, CMP, begin);                                          \
    break;                                                                     \
  case 9:                                                                      \
    SORT_NETWORK_9(TYPE, CMP, begin);                                          \
    break;                                                                     \
  case 10:                                                                     \
    SORT_NETWORK_10(TYPE, CMP, begin);                                         \
    break;                                                                     \
  case 11:                                                                     \
    SORT_NETWORK_11(TYPE, CMP, begin);                                         \
    break;                                                                     \
  case 12:                                                                     \
    SORT_NETWORK_12(TYPE, CMP, begin);                                         \
    break;                                                                     \
  case 13:                                                                     \
    SORT_NETWORK_13(TYPE, CMP, begin);                                         \
    break;                                                                     \
  case 14:                                                                     \
    SORT_NETWORK_14(TYPE, CMP, begin);                                         \
    break;                                                                     \
  case 15:                                                                     \
    SORT_NETWORK_15(TYPE, CMP, begin);                                         \
    break;                                                                     \
  case 16:                                                                     \
    SORT_NETWORK_16(TYPE, CMP, begin);                                         \
    break;                                                                     \
  }

#define SORT_SWAP(TYPE, a, b)                                                  \
  do {                                                                         \
    const TYPE swap_tmp = (a);                                                 \
    (a) = (b);                                                                 \
    (b) = swap_tmp;                                                            \
  } while (0)

#define SORT_PUSH(low, high)                                                   \
  do {                                                                         \
    top->lo = (low);                                                           \
    top->hi = (high);                                                          \
    ++top;                                                                     \
  } while (0)

#define SORT_POP(low, high)                                                    \
  do {                                                                         \
    --top;                                                                     \
    low = top->lo;                                                             \
    high = top->hi;                                                            \
  } while (0)

#define SORT_IMPL(NAME, EXPECT_LOW_CARDINALITY_OR_PRESORTED, TYPE, CMP)        \
                                                                               \
  static __inline bool NAME##_is_sorted(const TYPE *first, const TYPE *last) { \
    while (++first <= last)                                                    \
      if (CMP(first[0], first[-1]))                                            \
        return false;                                                          \
    return true;                                                               \
  }                                                                            \
                                                                               \
  typedef struct {                                                             \
    TYPE *lo, *hi;                                                             \
  } NAME##_stack;                                                              \
                                                                               \
  static __hot void NAME(TYPE *const begin, TYPE *const end) {                 \
    NAME##_stack stack[sizeof(unsigned) * CHAR_BIT], *top = stack;             \
                                                                               \
    TYPE *hi = end - 1;                                                        \
    TYPE *lo = begin;                                                          \
    while (true) {                                                             \
      const ptrdiff_t len = hi - lo;                                           \
      if (len < 16) {                                                          \
        SORT_INNER(TYPE, CMP, lo, hi + 1, len + 1);                            \
        if (unlikely(top == stack))                                            \
          break;                                                               \
        SORT_POP(lo, hi);                                                      \
        continue;                                                              \
      }                                                                        \
                                                                               \
      TYPE *mid = lo + (len >> 1);                                             \
      SORT_CMP_SWAP(TYPE, CMP, *lo, *mid);                                     \
      SORT_CMP_SWAP(TYPE, CMP, *mid, *hi);                                     \
      SORT_CMP_SWAP(TYPE, CMP, *lo, *mid);                                     \
                                                                               \
      TYPE *right = hi - 1;                                                    \
      TYPE *left = lo + 1;                                                     \
      while (1) {                                                              \
        while (CMP(*left, *mid))                                               \
          ++left;                                                              \
        while (CMP(*mid, *right))                                              \
          --right;                                                             \
        if (unlikely(left > right)) {                                          \
          if (EXPECT_LOW_CARDINALITY_OR_PRESORTED) {                           \
            if (NAME##_is_sorted(lo, right))                                   \
              lo = right + 1;                                                  \
            if (NAME##_is_sorted(left, hi))                                    \
              hi = left;                                                       \
          }                                                                    \
          break;                                                               \
        }                                                                      \
        SORT_SWAP(TYPE, *left, *right);                                        \
        mid = (mid == left) ? right : (mid == right) ? left : mid;             \
        ++left;                                                                \
        --right;                                                               \
      }                                                                        \
                                                                               \
      if (right - lo > hi - left) {                                            \
        SORT_PUSH(lo, right);                                                  \
        lo = left;                                                             \
      } else {                                                                 \
        SORT_PUSH(left, hi);                                                   \
        hi = right;                                                            \
      }                                                                        \
    }                                                                          \
                                                                               \
    if (mdbx_audit_enabled()) {                                                \
      for (TYPE *scan = begin + 1; scan < end; ++scan)                         \
        assert(CMP(scan[-1], scan[0]));                                        \
    }                                                                          \
  }

/*------------------------------------------------------------------------------
 * LY: radix sort for large chunks */

#define RADIXSORT_IMPL(NAME, TYPE, EXTRACT_KEY, BUFFER_PREALLOCATED, END_GAP)  \
                                                                               \
  __hot static bool NAME##_radixsort(TYPE *const begin,                        \
                                     const unsigned length) {                  \
    TYPE *tmp;                                                                 \
    if (BUFFER_PREALLOCATED) {                                                 \
      tmp = begin + length + END_GAP;                                          \
      /* memset(tmp, 0xDeadBeef, sizeof(TYPE) * length); */                    \
    } else {                                                                   \
      tmp = mdbx_malloc(sizeof(TYPE) * length);                                \
      if (unlikely(!tmp))                                                      \
        return false;                                                          \
    }                                                                          \
                                                                               \
    unsigned key_shift = 0, key_diff_mask;                                     \
    do {                                                                       \
      struct {                                                                 \
        unsigned a[256], b[256];                                               \
      } counters;                                                              \
      memset(&counters, 0, sizeof(counters));                                  \
                                                                               \
      key_diff_mask = 0;                                                       \
      unsigned prev_key = EXTRACT_KEY(begin) >> key_shift;                     \
      TYPE *r = begin, *end = begin + length;                                  \
      do {                                                                     \
        const unsigned key = EXTRACT_KEY(r) >> key_shift;                      \
        counters.a[key & 255]++;                                               \
        counters.b[(key >> 8) & 255]++;                                        \
        key_diff_mask |= prev_key ^ key;                                       \
        prev_key = key;                                                        \
      } while (++r != end);                                                    \
                                                                               \
      unsigned ta = 0, tb = 0;                                                 \
      for (unsigned i = 0; i < 256; ++i) {                                     \
        const unsigned ia = counters.a[i];                                     \
        counters.a[i] = ta;                                                    \
        ta += ia;                                                              \
        const unsigned ib = counters.b[i];                                     \
        counters.b[i] = tb;                                                    \
        tb += ib;                                                              \
      }                                                                        \
                                                                               \
      r = begin;                                                               \
      do {                                                                     \
        const unsigned key = EXTRACT_KEY(r) >> key_shift;                      \
        tmp[counters.a[key & 255]++] = *r;                                     \
      } while (++r != end);                                                    \
                                                                               \
      if (unlikely(key_diff_mask < 256)) {                                     \
        memcpy(begin, tmp, (char *)end - (char *)begin);                       \
        break;                                                                 \
      }                                                                        \
      end = (r = tmp) + length;                                                \
      do {                                                                     \
        const unsigned key = EXTRACT_KEY(r) >> key_shift;                      \
        begin[counters.b[(key >> 8) & 255]++] = *r;                            \
      } while (++r != end);                                                    \
                                                                               \
      key_shift += 16;                                                         \
    } while (key_diff_mask >> 16);                                             \
                                                                               \
    if (!(BUFFER_PREALLOCATED))                                                \
      mdbx_free(tmp);                                                          \
    return true;                                                               \
  }

/*------------------------------------------------------------------------------
 * LY: Binary search */

#define SEARCH_IMPL(NAME, TYPE_LIST, TYPE_ARG, CMP)                            \
  static __always_inline const TYPE_LIST *NAME(                                \
      const TYPE_LIST *first, unsigned length, const TYPE_ARG item) {          \
    const TYPE_LIST *const begin = first, *const end = begin + length;         \
                                                                               \
    while (length > 3) {                                                       \
      const unsigned whole = length;                                           \
      length >>= 1;                                                            \
      const TYPE_LIST *const middle = first + length;                          \
      const unsigned left = whole - length - 1;                                \
      const bool cmp = CMP(*middle, item);                                     \
      length = cmp ? left : length;                                            \
      first = cmp ? middle + 1 : first;                                        \
    }                                                                          \
                                                                               \
    switch (length) {                                                          \
    case 3:                                                                    \
      if (!CMP(*first, item))                                                  \
        break;                                                                 \
      ++first;                                                                 \
      __fallthrough /* fall through */;                                        \
    case 2:                                                                    \
      if (!CMP(*first, item))                                                  \
        break;                                                                 \
      ++first;                                                                 \
      __fallthrough /* fall through */;                                        \
    case 1:                                                                    \
      if (!CMP(*first, item))                                                  \
        break;                                                                 \
      ++first;                                                                 \
      __fallthrough /* fall through */;                                        \
    case 0:                                                                    \
      break;                                                                   \
    default:                                                                   \
      __unreachable();                                                         \
    }                                                                          \
                                                                               \
    if (mdbx_audit_enabled()) {                                                \
      for (const TYPE_LIST *scan = begin; scan < first; ++scan)                \
        assert(CMP(*scan, item));                                              \
      for (const TYPE_LIST *scan = first; scan < end; ++scan)                  \
        assert(!CMP(*scan, item));                                             \
      (void)begin, (void)end;                                                  \
    }                                                                          \
                                                                               \
    return first;                                                              \
  }

/*----------------------------------------------------------------------------*/

static __always_inline size_t pnl2bytes(size_t size) {
  assert(size > 0 && size <= MDBX_PGL_LIMIT);
#if MDBX_PNL_PREALLOC_FOR_RADIXSORT
  size += size;
#endif /* MDBX_PNL_PREALLOC_FOR_RADIXSORT */
  STATIC_ASSERT(MDBX_ASSUME_MALLOC_OVERHEAD +
                    (MDBX_PGL_LIMIT * (MDBX_PNL_PREALLOC_FOR_RADIXSORT + 1) +
                     MDBX_PNL_GRANULATE + 2) *
                        sizeof(pgno_t) <
                SIZE_MAX / 4 * 3);
  size_t bytes =
      ceil_powerof2(MDBX_ASSUME_MALLOC_OVERHEAD + sizeof(pgno_t) * (size + 2),
                    MDBX_PNL_GRANULATE * sizeof(pgno_t)) -
      MDBX_ASSUME_MALLOC_OVERHEAD;
  return bytes;
}

static __always_inline pgno_t bytes2pnl(const size_t bytes) {
  size_t size = bytes / sizeof(pgno_t);
  assert(size > 2 && size <= MDBX_PGL_LIMIT);
  size -= 2;
#if MDBX_PNL_PREALLOC_FOR_RADIXSORT
  size >>= 1;
#endif /* MDBX_PNL_PREALLOC_FOR_RADIXSORT */
  return (pgno_t)size;
}

static MDBX_PNL mdbx_pnl_alloc(size_t size) {
  size_t bytes = pnl2bytes(size);
  MDBX_PNL pl = mdbx_malloc(bytes);
  if (likely(pl)) {
#if __GLIBC_PREREQ(2, 12) || defined(__FreeBSD__) || defined(malloc_usable_size)
    bytes = malloc_usable_size(pl);
#endif /* malloc_usable_size */
    pl[0] = bytes2pnl(bytes);
    assert(pl[0] >= size);
    pl[1] = 0;
    pl += 1;
  }
  return pl;
}

static void mdbx_pnl_free(MDBX_PNL pl) {
  if (likely(pl))
    mdbx_free(pl - 1);
}

/* Shrink the PNL to the default size if it has grown larger */
static void mdbx_pnl_shrink(MDBX_PNL *ppl) {
  assert(bytes2pnl(pnl2bytes(MDBX_PNL_INITIAL)) >= MDBX_PNL_INITIAL &&
         bytes2pnl(pnl2bytes(MDBX_PNL_INITIAL)) < MDBX_PNL_INITIAL * 3 / 2);
  assert(MDBX_PNL_SIZE(*ppl) <= MDBX_PGL_LIMIT &&
         MDBX_PNL_ALLOCLEN(*ppl) >= MDBX_PNL_SIZE(*ppl));
  MDBX_PNL_SIZE(*ppl) = 0;
  if (unlikely(MDBX_PNL_ALLOCLEN(*ppl) >
               MDBX_PNL_INITIAL * 2 - MDBX_CACHELINE_SIZE / sizeof(pgno_t))) {
    size_t bytes = pnl2bytes(MDBX_PNL_INITIAL);
    MDBX_PNL pl = mdbx_realloc(*ppl - 1, bytes);
    if (likely(pl)) {
#if __GLIBC_PREREQ(2, 12) || defined(__FreeBSD__) || defined(malloc_usable_size)
      bytes = malloc_usable_size(pl);
#endif /* malloc_usable_size */
      *pl = bytes2pnl(bytes);
      *ppl = pl + 1;
    }
  }
}

/* Grow the PNL to the size growed to at least given size */
static int mdbx_pnl_reserve(MDBX_PNL *ppl, const size_t wanna) {
  const size_t allocated = MDBX_PNL_ALLOCLEN(*ppl);
  assert(MDBX_PNL_SIZE(*ppl) <= MDBX_PGL_LIMIT &&
         MDBX_PNL_ALLOCLEN(*ppl) >= MDBX_PNL_SIZE(*ppl));
  if (likely(allocated >= wanna))
    return MDBX_SUCCESS;

  if (unlikely(wanna > /* paranoia */ MDBX_PGL_LIMIT)) {
    mdbx_error("PNL too long (%zu > %zu)", wanna, (size_t)MDBX_PGL_LIMIT);
    return MDBX_TXN_FULL;
  }

  const size_t size = (wanna + wanna - allocated < MDBX_PGL_LIMIT)
                          ? wanna + wanna - allocated
                          : MDBX_PGL_LIMIT;
  size_t bytes = pnl2bytes(size);
  MDBX_PNL pl = mdbx_realloc(*ppl - 1, bytes);
  if (likely(pl)) {
#if __GLIBC_PREREQ(2, 12) || defined(__FreeBSD__) || defined(malloc_usable_size)
    bytes = malloc_usable_size(pl);
#endif /* malloc_usable_size */
    *pl = bytes2pnl(bytes);
    assert(*pl >= wanna);
    *ppl = pl + 1;
    return MDBX_SUCCESS;
  }
  return MDBX_ENOMEM;
}

/* Make room for num additional elements in an PNL */
static __always_inline int __must_check_result mdbx_pnl_need(MDBX_PNL *ppl,
                                                             size_t num) {
  assert(MDBX_PNL_SIZE(*ppl) <= MDBX_PGL_LIMIT &&
         MDBX_PNL_ALLOCLEN(*ppl) >= MDBX_PNL_SIZE(*ppl));
  assert(num <= MDBX_PGL_LIMIT);
  const size_t wanna = MDBX_PNL_SIZE(*ppl) + num;
  return likely(MDBX_PNL_ALLOCLEN(*ppl) >= wanna)
             ? MDBX_SUCCESS
             : mdbx_pnl_reserve(ppl, wanna);
}

static __always_inline void mdbx_pnl_xappend(MDBX_PNL pl, pgno_t pgno) {
  assert(MDBX_PNL_SIZE(pl) < MDBX_PNL_ALLOCLEN(pl));
  if (mdbx_audit_enabled()) {
    for (unsigned i = MDBX_PNL_SIZE(pl); i > 0; --i)
      assert(pgno != pl[i]);
  }
  MDBX_PNL_SIZE(pl) += 1;
  MDBX_PNL_LAST(pl) = pgno;
}

/* Append an pgno range onto an unsorted PNL */
__always_inline static int __must_check_result
mdbx_pnl_append_range(bool spilled, MDBX_PNL *ppl, pgno_t pgno, unsigned n) {
  assert(n > 0);
  int rc = mdbx_pnl_need(ppl, n);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const MDBX_PNL pnl = *ppl;
#if MDBX_PNL_ASCENDING
  unsigned w = MDBX_PNL_SIZE(pnl);
  do {
    pnl[++w] = pgno;
    pgno += spilled ? 2 : 1;
  } while (--n);
  MDBX_PNL_SIZE(pnl) = w;
#else
  unsigned w = MDBX_PNL_SIZE(pnl) + n;
  MDBX_PNL_SIZE(pnl) = w;
  do {
    pnl[w--] = pgno;
    pgno += spilled ? 2 : 1;
  } while (--n);
#endif

  return MDBX_SUCCESS;
}

/* Append an pgno range into the sorted PNL */
static __hot int __must_check_result mdbx_pnl_insert_range(MDBX_PNL *ppl,
                                                           pgno_t pgno,
                                                           unsigned n) {
  assert(n > 0);
  int rc = mdbx_pnl_need(ppl, n);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const MDBX_PNL pnl = *ppl;
  unsigned r = MDBX_PNL_SIZE(pnl), w = r + n;
  MDBX_PNL_SIZE(pnl) = w;
  while (r && MDBX_PNL_DISORDERED(pnl[r], pgno))
    pnl[w--] = pnl[r--];

  for (pgno_t fill = MDBX_PNL_ASCENDING ? pgno + n : pgno; w > r; --w)
    pnl[w] = MDBX_PNL_ASCENDING ? --fill : fill++;

  return MDBX_SUCCESS;
}

static bool mdbx_pnl_check(const MDBX_PNL pl, const size_t limit) {
  assert(limit >= MIN_PAGENO - MDBX_ENABLE_REFUND);
  if (likely(MDBX_PNL_SIZE(pl))) {
    assert(MDBX_PNL_LEAST(pl) >= MIN_PAGENO);
    assert(MDBX_PNL_MOST(pl) < limit);
    assert(MDBX_PNL_SIZE(pl) <= MDBX_PGL_LIMIT);
    if (unlikely(MDBX_PNL_SIZE(pl) > MDBX_PGL_LIMIT))
      return false;
    if (unlikely(MDBX_PNL_LEAST(pl) < MIN_PAGENO))
      return false;
    if (unlikely(MDBX_PNL_MOST(pl) >= limit))
      return false;
    if (mdbx_audit_enabled()) {
      for (const pgno_t *scan = &MDBX_PNL_LAST(pl); --scan > pl;) {
        assert(MDBX_PNL_ORDERED(scan[0], scan[1]));
        if (unlikely(!MDBX_PNL_ORDERED(scan[0], scan[1])))
          return false;
      }
    }
  }
  return true;
}

static __always_inline bool mdbx_pnl_check4assert(const MDBX_PNL pl,
                                                  const size_t limit) {
  if (unlikely(pl == nullptr))
    return true;
  assert(MDBX_PNL_ALLOCLEN(pl) >= MDBX_PNL_SIZE(pl));
  if (unlikely(MDBX_PNL_ALLOCLEN(pl) < MDBX_PNL_SIZE(pl)))
    return false;
  return mdbx_pnl_check(pl, limit);
}

/* Merge an PNL onto an PNL. The destination PNL must be big enough */
static void __hot mdbx_pnl_xmerge(MDBX_PNL dst, const MDBX_PNL src) {
  assert(mdbx_pnl_check4assert(dst, MAX_PAGENO + 1));
  assert(mdbx_pnl_check(src, MAX_PAGENO + 1));
  const size_t total = MDBX_PNL_SIZE(dst) + MDBX_PNL_SIZE(src);
  assert(MDBX_PNL_ALLOCLEN(dst) >= total);
  pgno_t *w = dst + total;
  pgno_t *d = dst + MDBX_PNL_SIZE(dst);
  const pgno_t *s = src + MDBX_PNL_SIZE(src);
  dst[0] = /* detent for scan below */ (MDBX_PNL_ASCENDING ? 0 : ~(pgno_t)0);
  while (s > src) {
    while (MDBX_PNL_ORDERED(*s, *d))
      *w-- = *d--;
    *w-- = *s--;
  }
  MDBX_PNL_SIZE(dst) = (pgno_t)total;
  assert(mdbx_pnl_check4assert(dst, MAX_PAGENO + 1));
}

static void mdbx_spill_remove(MDBX_txn *txn, unsigned idx, unsigned npages) {
  mdbx_tassert(txn, idx > 0 && idx <= MDBX_PNL_SIZE(txn->tw.spill_pages) &&
                        txn->tw.spill_least_removed > 0);
  txn->tw.spill_least_removed =
      (idx < txn->tw.spill_least_removed) ? idx : txn->tw.spill_least_removed;
  txn->tw.spill_pages[idx] |= 1;
  MDBX_PNL_SIZE(txn->tw.spill_pages) -=
      (idx == MDBX_PNL_SIZE(txn->tw.spill_pages));

  while (unlikely(npages > 1)) {
    const pgno_t pgno = (txn->tw.spill_pages[idx] >> 1) + 1;
    if (MDBX_PNL_ASCENDING) {
      if (++idx > MDBX_PNL_SIZE(txn->tw.spill_pages) ||
          (txn->tw.spill_pages[idx] >> 1) != pgno)
        return;
    } else {
      if (--idx < 1 || (txn->tw.spill_pages[idx] >> 1) != pgno)
        return;
      txn->tw.spill_least_removed = (idx < txn->tw.spill_least_removed)
                                        ? idx
                                        : txn->tw.spill_least_removed;
    }
    txn->tw.spill_pages[idx] |= 1;
    MDBX_PNL_SIZE(txn->tw.spill_pages) -=
        (idx == MDBX_PNL_SIZE(txn->tw.spill_pages));
    --npages;
  }
}

static MDBX_PNL mdbx_spill_purge(MDBX_txn *txn) {
  mdbx_tassert(txn, txn->tw.spill_least_removed > 0);
  const MDBX_PNL sl = txn->tw.spill_pages;
  if (txn->tw.spill_least_removed != INT_MAX) {
    unsigned len = MDBX_PNL_SIZE(sl), r, w;
    for (w = r = txn->tw.spill_least_removed; r <= len; ++r) {
      sl[w] = sl[r];
      w += 1 - (sl[r] & 1);
    }
    for (size_t i = 1; i < w; ++i)
      mdbx_tassert(txn, (sl[i] & 1) == 0);
    MDBX_PNL_SIZE(sl) = w - 1;
    txn->tw.spill_least_removed = INT_MAX;
  } else {
    for (size_t i = 1; i <= MDBX_PNL_SIZE(sl); ++i)
      mdbx_tassert(txn, (sl[i] & 1) == 0);
  }
  return sl;
}

#if MDBX_PNL_ASCENDING
#define MDBX_PNL_EXTRACT_KEY(ptr) (*(ptr))
#else
#define MDBX_PNL_EXTRACT_KEY(ptr) (P_INVALID - *(ptr))
#endif
RADIXSORT_IMPL(pgno, pgno_t, MDBX_PNL_EXTRACT_KEY,
               MDBX_PNL_PREALLOC_FOR_RADIXSORT, 0)

SORT_IMPL(pgno_sort, false, pgno_t, MDBX_PNL_ORDERED)
static __hot void mdbx_pnl_sort(MDBX_PNL pnl, size_t limit4check) {
  if (likely(MDBX_PNL_SIZE(pnl) < MDBX_RADIXSORT_THRESHOLD) ||
      unlikely(!pgno_radixsort(&MDBX_PNL_FIRST(pnl), MDBX_PNL_SIZE(pnl))))
    pgno_sort(MDBX_PNL_BEGIN(pnl), MDBX_PNL_END(pnl));
  assert(mdbx_pnl_check(pnl, limit4check));
  (void)limit4check;
}

/* Search for an pgno in an PNL.
 * Returns The index of the first item greater than or equal to pgno. */
SEARCH_IMPL(pgno_bsearch, pgno_t, pgno_t, MDBX_PNL_ORDERED)

static __hot unsigned mdbx_pnl_search(const MDBX_PNL pnl, pgno_t pgno) {
  assert(mdbx_pnl_check4assert(pnl, MAX_PAGENO + 1));
  const pgno_t *begin = MDBX_PNL_BEGIN(pnl);
  const pgno_t *it = pgno_bsearch(begin, MDBX_PNL_SIZE(pnl), pgno);
  const pgno_t *end = begin + MDBX_PNL_SIZE(pnl);
  assert(it >= begin && it <= end);
  if (it != begin)
    assert(MDBX_PNL_ORDERED(it[-1], pgno));
  if (it != end)
    assert(!MDBX_PNL_ORDERED(it[0], pgno));
  return (unsigned)(it - begin + 1);
}

static __inline unsigned mdbx_pnl_exist(const MDBX_PNL pnl, pgno_t pgno) {
  unsigned n = mdbx_pnl_search(pnl, pgno);
  return (n <= MDBX_PNL_SIZE(pnl) && pnl[n] == pgno) ? n : 0;
}

static __inline unsigned mdbx_pnl_intersect(const MDBX_PNL pnl, pgno_t pgno,
                                            unsigned npages) {
  const unsigned len = MDBX_PNL_SIZE(pnl);
  if (mdbx_log_enabled(MDBX_LOG_EXTRA)) {
    mdbx_debug_extra("PNL len %u [", len);
    for (unsigned i = 1; i <= len; ++i)
      mdbx_debug_extra_print(" %" PRIaPGNO, pnl[i]);
    mdbx_debug_extra_print("%s\n", "]");
  }
  const pgno_t range_last = pgno + npages - 1;
#if MDBX_PNL_ASCENDING
  const unsigned n = mdbx_pnl_search(pnl, pgno);
  assert(n && (n == MDBX_PNL_SIZE(pnl) + 1 || pgno <= pnl[n]));
  const bool rc = n <= MDBX_PNL_SIZE(pnl) && pnl[n] <= range_last;
#else
  const unsigned n = mdbx_pnl_search(pnl, range_last);
  assert(n && (n == MDBX_PNL_SIZE(pnl) + 1 || range_last >= pnl[n]));
  const bool rc = n <= MDBX_PNL_SIZE(pnl) && pnl[n] >= pgno;
#endif
  if (mdbx_assert_enabled()) {
    bool check = false;
    for (unsigned i = 0; i < npages; ++i)
      check |= mdbx_pnl_exist(pnl, pgno + i) != 0;
    assert(check == rc);
  }
  return rc;
}

/*----------------------------------------------------------------------------*/

static __always_inline size_t txl2bytes(const size_t size) {
  assert(size > 0 && size <= MDBX_TXL_MAX * 2);
  size_t bytes =
      ceil_powerof2(MDBX_ASSUME_MALLOC_OVERHEAD + sizeof(txnid_t) * (size + 2),
                    MDBX_TXL_GRANULATE * sizeof(txnid_t)) -
      MDBX_ASSUME_MALLOC_OVERHEAD;
  return bytes;
}

static __always_inline size_t bytes2txl(const size_t bytes) {
  size_t size = bytes / sizeof(txnid_t);
  assert(size > 2 && size <= MDBX_TXL_MAX * 2);
  return size - 2;
}

static MDBX_TXL mdbx_txl_alloc(void) {
  size_t bytes = txl2bytes(MDBX_TXL_INITIAL);
  MDBX_TXL tl = mdbx_malloc(bytes);
  if (likely(tl)) {
#if __GLIBC_PREREQ(2, 12) || defined(__FreeBSD__) || defined(malloc_usable_size)
    bytes = malloc_usable_size(tl);
#endif /* malloc_usable_size */
    tl[0] = bytes2txl(bytes);
    assert(tl[0] >= MDBX_TXL_INITIAL);
    tl[1] = 0;
    tl += 1;
  }
  return tl;
}

static void mdbx_txl_free(MDBX_TXL tl) {
  if (likely(tl))
    mdbx_free(tl - 1);
}

static int mdbx_txl_reserve(MDBX_TXL *ptl, const size_t wanna) {
  const size_t allocated = (size_t)MDBX_PNL_ALLOCLEN(*ptl);
  assert(MDBX_PNL_SIZE(*ptl) <= MDBX_TXL_MAX &&
         MDBX_PNL_ALLOCLEN(*ptl) >= MDBX_PNL_SIZE(*ptl));
  if (likely(allocated >= wanna))
    return MDBX_SUCCESS;

  if (unlikely(wanna > /* paranoia */ MDBX_TXL_MAX)) {
    mdbx_error("TXL too long (%zu > %zu)", wanna, (size_t)MDBX_TXL_MAX);
    return MDBX_TXN_FULL;
  }

  const size_t size = (wanna + wanna - allocated < MDBX_TXL_MAX)
                          ? wanna + wanna - allocated
                          : MDBX_TXL_MAX;
  size_t bytes = txl2bytes(size);
  MDBX_TXL tl = mdbx_realloc(*ptl - 1, bytes);
  if (likely(tl)) {
#if __GLIBC_PREREQ(2, 12) || defined(__FreeBSD__) || defined(malloc_usable_size)
    bytes = malloc_usable_size(tl);
#endif /* malloc_usable_size */
    *tl = bytes2txl(bytes);
    assert(*tl >= wanna);
    *ptl = tl + 1;
    return MDBX_SUCCESS;
  }
  return MDBX_ENOMEM;
}

static __always_inline int __must_check_result mdbx_txl_need(MDBX_TXL *ptl,
                                                             size_t num) {
  assert(MDBX_PNL_SIZE(*ptl) <= MDBX_TXL_MAX &&
         MDBX_PNL_ALLOCLEN(*ptl) >= MDBX_PNL_SIZE(*ptl));
  assert(num <= MDBX_PGL_LIMIT);
  const size_t wanna = (size_t)MDBX_PNL_SIZE(*ptl) + num;
  return likely(MDBX_PNL_ALLOCLEN(*ptl) >= wanna)
             ? MDBX_SUCCESS
             : mdbx_txl_reserve(ptl, wanna);
}

static __always_inline void mdbx_txl_xappend(MDBX_TXL tl, txnid_t id) {
  assert(MDBX_PNL_SIZE(tl) < MDBX_PNL_ALLOCLEN(tl));
  MDBX_PNL_SIZE(tl) += 1;
  MDBX_PNL_LAST(tl) = id;
}

#define TXNID_SORT_CMP(first, last) ((first) > (last))
SORT_IMPL(txnid_sort, false, txnid_t, TXNID_SORT_CMP)
static void mdbx_txl_sort(MDBX_TXL tl) {
  txnid_sort(MDBX_PNL_BEGIN(tl), MDBX_PNL_END(tl));
}

static int __must_check_result mdbx_txl_append(MDBX_TXL *ptl, txnid_t id) {
  if (unlikely(MDBX_PNL_SIZE(*ptl) == MDBX_PNL_ALLOCLEN(*ptl))) {
    int rc = mdbx_txl_need(ptl, MDBX_TXL_GRANULATE);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }
  mdbx_txl_xappend(*ptl, id);
  return MDBX_SUCCESS;
}

/*----------------------------------------------------------------------------*/

#define MDBX_DPL_UNSORTED_BACKLOG 16
#define MDBX_DPL_GAP_FOR_MERGESORT MDBX_DPL_UNSORTED_BACKLOG
#define MDBX_DPL_GAP_FOR_EDGING 2
#define MDBX_DPL_RESERVE_GAP                                                   \
  (MDBX_DPL_GAP_FOR_MERGESORT + MDBX_DPL_GAP_FOR_EDGING)

static __always_inline size_t dpl2bytes(ptrdiff_t size) {
  assert(size > CURSOR_STACK && (size_t)size <= MDBX_PGL_LIMIT);
#if MDBX_DPL_PREALLOC_FOR_RADIXSORT
  size += size;
#endif /* MDBX_DPL_PREALLOC_FOR_RADIXSORT */
  STATIC_ASSERT(MDBX_ASSUME_MALLOC_OVERHEAD + sizeof(MDBX_dpl) +
                    (MDBX_PGL_LIMIT * (MDBX_DPL_PREALLOC_FOR_RADIXSORT + 1) +
                     MDBX_DPL_RESERVE_GAP) *
                        sizeof(MDBX_dp) +
                    MDBX_PNL_GRANULATE * sizeof(void *) * 2 <
                SIZE_MAX / 4 * 3);
  size_t bytes =
      ceil_powerof2(MDBX_ASSUME_MALLOC_OVERHEAD + sizeof(MDBX_dpl) +
                        ((size_t)size + MDBX_DPL_RESERVE_GAP) * sizeof(MDBX_dp),
                    MDBX_PNL_GRANULATE * sizeof(void *) * 2) -
      MDBX_ASSUME_MALLOC_OVERHEAD;
  return bytes;
}

static __always_inline unsigned bytes2dpl(const ptrdiff_t bytes) {
  size_t size = (bytes - sizeof(MDBX_dpl)) / sizeof(MDBX_dp);
  assert(size > CURSOR_STACK + MDBX_DPL_RESERVE_GAP &&
         size <= MDBX_PGL_LIMIT + MDBX_PNL_GRANULATE);
  size -= MDBX_DPL_RESERVE_GAP;
#if MDBX_DPL_PREALLOC_FOR_RADIXSORT
  size >>= 1;
#endif /* MDBX_DPL_PREALLOC_FOR_RADIXSORT */
  return (unsigned)size;
}

static __always_inline unsigned dpl_setlen(MDBX_dpl *dl, unsigned len) {
  static const MDBX_page dpl_stub_pageE = {
      {0}, 0, P_BAD, {0}, /* pgno */ ~(pgno_t)0};
  assert(dpl_stub_pageE.mp_flags == P_BAD &&
         dpl_stub_pageE.mp_pgno == P_INVALID);
  dl->length = len;
  dl->items[len + 1].ptr = (MDBX_page *)&dpl_stub_pageE;
  dl->items[len + 1].pgno = P_INVALID;
  dl->items[len + 1].extra = 0;
  return len;
}

static __always_inline void dpl_clear(MDBX_dpl *dl) {
  static const MDBX_page dpl_stub_pageB = {{0}, 0, P_BAD, {0}, /* pgno */ 0};
  assert(dpl_stub_pageB.mp_flags == P_BAD && dpl_stub_pageB.mp_pgno == 0);
  dl->sorted = dpl_setlen(dl, 0);
  dl->items[0].ptr = (MDBX_page *)&dpl_stub_pageB;
  dl->items[0].pgno = 0;
  dl->items[0].extra = 0;
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
}

static void mdbx_dpl_free(MDBX_txn *txn) {
  if (likely(txn->tw.dirtylist)) {
    mdbx_free(txn->tw.dirtylist);
    txn->tw.dirtylist = NULL;
  }
}

static MDBX_dpl *mdbx_dpl_reserve(MDBX_txn *txn, size_t size) {
  size_t bytes = dpl2bytes((size < MDBX_PGL_LIMIT) ? size : MDBX_PGL_LIMIT);
  MDBX_dpl *const dl = mdbx_realloc(txn->tw.dirtylist, bytes);
  if (likely(dl)) {
#if __GLIBC_PREREQ(2, 12) || defined(__FreeBSD__) || defined(malloc_usable_size)
    bytes = malloc_usable_size(dl);
#endif /* malloc_usable_size */
    dl->detent = bytes2dpl(bytes);
    mdbx_tassert(txn, txn->tw.dirtylist == NULL || dl->length <= dl->detent);
    txn->tw.dirtylist = dl;
  }
  return dl;
}

static int mdbx_dpl_alloc(MDBX_txn *txn) {
  mdbx_tassert(txn, (txn->mt_flags & MDBX_TXN_RDONLY) == 0);
  const int wanna = (txn->mt_env->me_options.dp_initial < txn->mt_geo.upper)
                        ? txn->mt_env->me_options.dp_initial
                        : txn->mt_geo.upper;
  if (txn->tw.dirtylist) {
    dpl_clear(txn->tw.dirtylist);
    const int realloc_threshold = 64;
    if (likely(
            !((int)(txn->tw.dirtylist->detent - wanna) > realloc_threshold ||
              (int)(txn->tw.dirtylist->detent - wanna) < -realloc_threshold)))
      return MDBX_SUCCESS;
  }
  if (unlikely(!mdbx_dpl_reserve(txn, wanna)))
    return MDBX_ENOMEM;
  dpl_clear(txn->tw.dirtylist);
  return MDBX_SUCCESS;
}

#define MDBX_DPL_EXTRACT_KEY(ptr) ((ptr)->pgno)
RADIXSORT_IMPL(dpl, MDBX_dp, MDBX_DPL_EXTRACT_KEY,
               MDBX_DPL_PREALLOC_FOR_RADIXSORT, 1)

#define DP_SORT_CMP(first, last) ((first).pgno < (last).pgno)
SORT_IMPL(dp_sort, false, MDBX_dp, DP_SORT_CMP)

__hot __noinline static MDBX_dpl *mdbx_dpl_sort_slowpath(const MDBX_txn *txn) {
  MDBX_dpl *dl = txn->tw.dirtylist;
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  const unsigned unsorted = dl->length - dl->sorted;
  if (likely(unsorted < MDBX_RADIXSORT_THRESHOLD) ||
      unlikely(!dpl_radixsort(dl->items + 1, dl->length))) {
    if (dl->sorted > unsorted / 4 + 4 &&
        (MDBX_DPL_PREALLOC_FOR_RADIXSORT ||
         dl->length + unsorted < dl->detent + MDBX_DPL_GAP_FOR_MERGESORT)) {
      MDBX_dp *const sorted_begin = dl->items + 1;
      MDBX_dp *const sorted_end = sorted_begin + dl->sorted;
      MDBX_dp *const end =
          dl->items + (MDBX_DPL_PREALLOC_FOR_RADIXSORT
                           ? dl->length + dl->length + 1
                           : dl->detent + MDBX_DPL_RESERVE_GAP);
      MDBX_dp *const tmp = end - unsorted;
      assert(dl->items + dl->length + 1 < tmp);
      /* copy unsorted to the end of allocated space and sort it */
      memcpy(tmp, sorted_end, unsorted * sizeof(MDBX_dp));
      dp_sort(tmp, tmp + unsorted);
      /* merge two parts from end to begin */
      MDBX_dp *w = dl->items + dl->length;
      MDBX_dp *l = dl->items + dl->sorted;
      MDBX_dp *r = end - 1;
      do {
        const bool cmp = l->pgno > r->pgno;
        *w = cmp ? *l : *r;
        l -= cmp;
        r += cmp - 1;
      } while (likely(--w > l));
      assert(r == tmp - 1);
      assert(dl->items[0].pgno == 0 &&
             dl->items[dl->length + 1].pgno == P_INVALID);
      if (mdbx_assert_enabled())
        for (unsigned i = 0; i <= dl->length; ++i)
          assert(dl->items[i].pgno < dl->items[i + 1].pgno);
    } else {
      dp_sort(dl->items + 1, dl->items + dl->length + 1);
      assert(dl->items[0].pgno == 0 &&
             dl->items[dl->length + 1].pgno == P_INVALID);
    }
  } else {
    assert(dl->items[0].pgno == 0 &&
           dl->items[dl->length + 1].pgno == P_INVALID);
  }
  dl->sorted = dl->length;
  return dl;
}

static __always_inline MDBX_dpl *mdbx_dpl_sort(const MDBX_txn *txn) {
  MDBX_dpl *dl = txn->tw.dirtylist;
  assert(dl->length <= MDBX_PGL_LIMIT);
  assert(dl->sorted <= dl->length);
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  return likely(dl->sorted == dl->length) ? dl : mdbx_dpl_sort_slowpath(txn);
}

/* Returns the index of the first dirty-page whose pgno
 * member is greater than or equal to id. */
#define DP_SEARCH_CMP(dp, id) ((dp).pgno < (id))
SEARCH_IMPL(dp_bsearch, MDBX_dp, pgno_t, DP_SEARCH_CMP)

static unsigned __hot mdbx_dpl_search(const MDBX_txn *txn, pgno_t pgno) {
  MDBX_dpl *dl = txn->tw.dirtylist;
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  if (mdbx_audit_enabled()) {
    for (const MDBX_dp *ptr = dl->items + dl->sorted; --ptr > dl->items;) {
      assert(ptr[0].pgno < ptr[1].pgno);
      assert(ptr[0].pgno >= NUM_METAS);
    }
  }

  switch (dl->length - dl->sorted) {
  default:
    /* sort a whole */
    mdbx_dpl_sort_slowpath(txn);
    break;
  case 0:
    /* whole sorted cases */
    break;

#define LINEAR_SEARCH_CASE(N)                                                  \
  case N:                                                                      \
    if (dl->items[dl->length - N + 1].pgno == pgno)                            \
      return dl->length - N + 1;                                               \
    __fallthrough

    /* try linear search until the threshold */
    LINEAR_SEARCH_CASE(16); /* fall through */
    LINEAR_SEARCH_CASE(15); /* fall through */
    LINEAR_SEARCH_CASE(14); /* fall through */
    LINEAR_SEARCH_CASE(13); /* fall through */
    LINEAR_SEARCH_CASE(12); /* fall through */
    LINEAR_SEARCH_CASE(11); /* fall through */
    LINEAR_SEARCH_CASE(10); /* fall through */
    LINEAR_SEARCH_CASE(9);  /* fall through */
    LINEAR_SEARCH_CASE(8);  /* fall through */
    LINEAR_SEARCH_CASE(7);  /* fall through */
    LINEAR_SEARCH_CASE(6);  /* fall through */
    LINEAR_SEARCH_CASE(5);  /* fall through */
    LINEAR_SEARCH_CASE(4);  /* fall through */
    LINEAR_SEARCH_CASE(3);  /* fall through */
    LINEAR_SEARCH_CASE(2);  /* fall through */
  case 1:
    if (dl->items[dl->length].pgno == pgno)
      return dl->length;
    /* continue bsearch on the sorted part */
    break;
  }
  return (unsigned)(dp_bsearch(dl->items + 1, dl->sorted, pgno) - dl->items);
}

MDBX_NOTHROW_PURE_FUNCTION static __inline unsigned
dpl_npages(const MDBX_dpl *dl, unsigned i) {
  assert(0 <= (int)i && i <= dl->length);
  unsigned n = likely(!dl->items[i].multi) ? 1 : dl->items[i].ptr->mp_pages;
  assert(n == (IS_OVERFLOW(dl->items[i].ptr) ? dl->items[i].ptr->mp_pages : 1));
  return n;
}

MDBX_NOTHROW_PURE_FUNCTION static __inline unsigned
dpl_endpgno(const MDBX_dpl *dl, unsigned i) {
  return dpl_npages(dl, i) + dl->items[i].pgno;
}

static __inline bool mdbx_dpl_intersect(const MDBX_txn *txn, pgno_t pgno,
                                        unsigned npages) {
  MDBX_dpl *dl = txn->tw.dirtylist;
  assert(dl->sorted == dl->length);
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  unsigned const n = mdbx_dpl_search(txn, pgno);
  assert(n >= 1 && n <= dl->length + 1);
  assert(pgno <= dl->items[n].pgno);
  assert(pgno > dl->items[n - 1].pgno);
  const bool rc =
      /* intersection with founded */ pgno + npages > dl->items[n].pgno ||
      /* intersection with prev */ dpl_endpgno(dl, n - 1) > pgno;
  if (mdbx_assert_enabled()) {
    bool check = false;
    for (unsigned i = 1; i <= dl->length; ++i) {
      const MDBX_page *const dp = dl->items[i].ptr;
      if (!(dp->mp_pgno /* begin */ >= /* end */ pgno + npages ||
            dpl_endpgno(dl, i) /* end */ <= /* begin */ pgno))
        check |= true;
    }
    assert(check == rc);
  }
  return rc;
}

static __always_inline unsigned mdbx_dpl_exist(MDBX_txn *txn, pgno_t pgno) {
  MDBX_dpl *dl = txn->tw.dirtylist;
  unsigned i = mdbx_dpl_search(txn, pgno);
  assert((int)i > 0);
  return (dl->items[i].pgno == pgno) ? i : 0;
}

MDBX_MAYBE_UNUSED static const MDBX_page *debug_dpl_find(const MDBX_txn *txn,
                                                         const pgno_t pgno) {
  const MDBX_dpl *dl = txn->tw.dirtylist;
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  for (unsigned i = dl->length; i > dl->sorted; --i)
    if (dl->items[i].pgno == pgno)
      return dl->items[i].ptr;

  if (dl->sorted) {
    const unsigned i =
        (unsigned)(dp_bsearch(dl->items + 1, dl->sorted, pgno) - dl->items);
    if (dl->items[i].pgno == pgno)
      return dl->items[i].ptr;
  }
  return nullptr;
}

static void mdbx_dpl_remove(const MDBX_txn *txn, unsigned i) {
  MDBX_dpl *dl = txn->tw.dirtylist;
  assert((int)i > 0 && i <= dl->length);
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  dl->sorted -= dl->sorted >= i;
  dl->length -= 1;
  memmove(dl->items + i, dl->items + i + 1,
          (dl->length - i + 2) * sizeof(dl->items[0]));
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
}

static __always_inline int __must_check_result
mdbx_dpl_append(MDBX_txn *txn, pgno_t pgno, MDBX_page *page, unsigned npages) {
  MDBX_dpl *dl = txn->tw.dirtylist;
  assert(dl->length <= MDBX_PGL_LIMIT + MDBX_PNL_GRANULATE);
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  if (mdbx_audit_enabled()) {
    for (unsigned i = dl->length; i > 0; --i) {
      assert(dl->items[i].pgno != pgno);
      if (unlikely(dl->items[i].pgno == pgno)) {
        mdbx_error("Page %u already exist in the DPL at %u", pgno, i);
        return MDBX_PROBLEM;
      }
    }
  }

  const unsigned length = dl->length + 1;
  const unsigned sorted =
      (dl->sorted == dl->length && dl->items[dl->length].pgno < pgno)
          ? length
          : dl->sorted;

  if (unlikely(dl->length == dl->detent)) {
    if (unlikely(dl->detent >= MDBX_PGL_LIMIT)) {
      mdbx_error("DPL is full (MDBX_PGL_LIMIT %zu)", MDBX_PGL_LIMIT);
      return MDBX_TXN_FULL;
    }
    const size_t size = (dl->detent < MDBX_PNL_INITIAL * 42)
                            ? dl->detent + dl->detent
                            : dl->detent + dl->detent / 2;
    dl = mdbx_dpl_reserve(txn, size);
    if (unlikely(!dl))
      return MDBX_ENOMEM;
    mdbx_tassert(txn, dl->length < dl->detent);
  }

  /* copy the stub beyond the end */
  dl->items[length + 1] = dl->items[length];
  /* append page */
  dl->items[length].ptr = page;
  dl->items[length].pgno = pgno;
  dl->items[length].multi = npages > 1;
  dl->items[length].lru = txn->tw.dirtylru++;
  dl->length = length;
  dl->sorted = sorted;
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  return MDBX_SUCCESS;
}

static __inline uint32_t mdbx_dpl_age(const MDBX_txn *txn, unsigned i) {
  const MDBX_dpl *dl = txn->tw.dirtylist;
  assert((int)i > 0 && i <= dl->length);
  /* overflow could be here */
  return (txn->tw.dirtylru - dl->items[i].lru) & UINT32_C(0x7fffFFFF);
}

/*----------------------------------------------------------------------------*/

uint8_t mdbx_runtime_flags = MDBX_RUNTIME_FLAGS_INIT;
uint8_t mdbx_loglevel = MDBX_LOG_FATAL;
MDBX_debug_func *mdbx_debug_logger;

static __must_check_result __inline int mdbx_page_retire(MDBX_cursor *mc,
                                                         MDBX_page *mp);

static int __must_check_result mdbx_page_dirty(MDBX_txn *txn, MDBX_page *mp,
                                               unsigned npages);
struct page_result {
  MDBX_page *page;
  int err;
};

static struct page_result mdbx_page_alloc(MDBX_cursor *mc, const unsigned num,
                                          int flags);
static txnid_t mdbx_kick_longlived_readers(MDBX_env *env,
                                           const txnid_t laggard);

static struct page_result mdbx_page_new(MDBX_cursor *mc, const unsigned flags,
                                        const unsigned npages);
static int mdbx_page_touch(MDBX_cursor *mc);
static int mdbx_cursor_touch(MDBX_cursor *mc);
static int mdbx_touch_dbi(MDBX_cursor *mc);

#define MDBX_END_NAMES                                                         \
  {                                                                            \
    "committed", "empty-commit", "abort", "reset", "reset-tmp", "fail-begin",  \
        "fail-beginchild"                                                      \
  }
enum {
  /* mdbx_txn_end operation number, for logging */
  MDBX_END_COMMITTED,
  MDBX_END_PURE_COMMIT,
  MDBX_END_ABORT,
  MDBX_END_RESET,
  MDBX_END_RESET_TMP,
  MDBX_END_FAIL_BEGIN,
  MDBX_END_FAIL_BEGINCHILD
};
#define MDBX_END_OPMASK 0x0F  /* mask for mdbx_txn_end() operation number */
#define MDBX_END_UPDATE 0x10  /* update env state (DBIs) */
#define MDBX_END_FREE 0x20    /* free txn unless it is MDBX_env.me_txn0 */
#define MDBX_END_EOTDONE 0x40 /* txn's cursors already closed */
#define MDBX_END_SLOT 0x80    /* release any reader slot if MDBX_NOTLS */
static int mdbx_txn_end(MDBX_txn *txn, const unsigned mode);

__hot static struct page_result __must_check_result
mdbx_page_get_ex(MDBX_cursor *const mc, const pgno_t pgno, txnid_t front);
static __inline int __must_check_result mdbx_page_get(MDBX_cursor *mc,
                                                      pgno_t pgno,
                                                      MDBX_page **mp,
                                                      txnid_t front) {

  struct page_result ret = mdbx_page_get_ex(mc, pgno, front);
  *mp = ret.page;
  return ret.err;
}

static int __must_check_result mdbx_page_search_root(MDBX_cursor *mc,
                                                     const MDBX_val *key,
                                                     int flags);

#define MDBX_PS_MODIFY 1
#define MDBX_PS_ROOTONLY 2
#define MDBX_PS_FIRST 4
#define MDBX_PS_LAST 8
static int __must_check_result mdbx_page_search(MDBX_cursor *mc,
                                                const MDBX_val *key, int flags);
static int __must_check_result mdbx_page_merge(MDBX_cursor *csrc,
                                               MDBX_cursor *cdst);

#define MDBX_SPLIT_REPLACE MDBX_APPENDDUP /* newkey is not new */
static int __must_check_result mdbx_page_split(MDBX_cursor *mc,
                                               const MDBX_val *const newkey,
                                               MDBX_val *const newdata,
                                               pgno_t newpgno, unsigned nflags);

static int __must_check_result mdbx_validate_meta_copy(MDBX_env *env,
                                                       const MDBX_meta *meta,
                                                       MDBX_meta *dest);
static int __must_check_result mdbx_override_meta(MDBX_env *env,
                                                  unsigned target,
                                                  txnid_t txnid,
                                                  const MDBX_meta *shape);
static int __must_check_result mdbx_read_header(MDBX_env *env, MDBX_meta *meta,
                                                const int lck_exclusive,
                                                const mdbx_mode_t mode_bits);
static int __must_check_result mdbx_sync_locked(MDBX_env *env, unsigned flags,
                                                MDBX_meta *const pending);
static int mdbx_env_close0(MDBX_env *env);

struct node_result {
  MDBX_node *node;
  bool exact;
};

static struct node_result mdbx_node_search(MDBX_cursor *mc,
                                           const MDBX_val *key);

static int __must_check_result mdbx_node_add_branch(MDBX_cursor *mc,
                                                    unsigned indx,
                                                    const MDBX_val *key,
                                                    pgno_t pgno);
static int __must_check_result mdbx_node_add_leaf(MDBX_cursor *mc,
                                                  unsigned indx,
                                                  const MDBX_val *key,
                                                  MDBX_val *data,
                                                  unsigned flags);
static int __must_check_result mdbx_node_add_leaf2(MDBX_cursor *mc,
                                                   unsigned indx,
                                                   const MDBX_val *key);

static void mdbx_node_del(MDBX_cursor *mc, size_t ksize);
static void mdbx_node_shrink(MDBX_page *mp, unsigned indx);
static int __must_check_result mdbx_node_move(MDBX_cursor *csrc,
                                              MDBX_cursor *cdst, bool fromleft);
static int __must_check_result mdbx_node_read(MDBX_cursor *mc,
                                              const MDBX_node *leaf,
                                              MDBX_val *data,
                                              const txnid_t front);
static int __must_check_result mdbx_rebalance(MDBX_cursor *mc);
static int __must_check_result mdbx_update_key(MDBX_cursor *mc,
                                               const MDBX_val *key);

static void mdbx_cursor_pop(MDBX_cursor *mc);
static int __must_check_result mdbx_cursor_push(MDBX_cursor *mc, MDBX_page *mp);

static int __must_check_result mdbx_audit_ex(MDBX_txn *txn,
                                             unsigned retired_stored,
                                             bool dont_filter_gc);

static int __must_check_result mdbx_page_check(MDBX_cursor *const mc,
                                               const MDBX_page *const mp,
                                               unsigned options);
static int __must_check_result mdbx_cursor_check(MDBX_cursor *mc,
                                                 unsigned options);
static int __must_check_result mdbx_cursor_del0(MDBX_cursor *mc);
static int __must_check_result mdbx_del0(MDBX_txn *txn, MDBX_dbi dbi,
                                         const MDBX_val *key,
                                         const MDBX_val *data, unsigned flags);
#define SIBLING_LEFT 0
#define SIBLING_RIGHT 2
static int __must_check_result mdbx_cursor_sibling(MDBX_cursor *mc, int dir);
static int __must_check_result mdbx_cursor_next(MDBX_cursor *mc, MDBX_val *key,
                                                MDBX_val *data,
                                                MDBX_cursor_op op);
static int __must_check_result mdbx_cursor_prev(MDBX_cursor *mc, MDBX_val *key,
                                                MDBX_val *data,
                                                MDBX_cursor_op op);
struct cursor_set_result {
  int err;
  bool exact;
};

static struct cursor_set_result mdbx_cursor_set(MDBX_cursor *mc, MDBX_val *key,
                                                MDBX_val *data,
                                                MDBX_cursor_op op);
static int __must_check_result mdbx_cursor_first(MDBX_cursor *mc, MDBX_val *key,
                                                 MDBX_val *data);
static int __must_check_result mdbx_cursor_last(MDBX_cursor *mc, MDBX_val *key,
                                                MDBX_val *data);

static int __must_check_result mdbx_cursor_init(MDBX_cursor *mc, MDBX_txn *txn,
                                                MDBX_dbi dbi);
static int __must_check_result mdbx_xcursor_init0(MDBX_cursor *mc);
static int __must_check_result mdbx_xcursor_init1(MDBX_cursor *mc,
                                                  MDBX_node *node,
                                                  const MDBX_page *mp);
static int __must_check_result mdbx_xcursor_init2(MDBX_cursor *mc,
                                                  MDBX_xcursor *src_mx,
                                                  bool new_dupdata);
static void cursor_copy(const MDBX_cursor *csrc, MDBX_cursor *cdst);

static int __must_check_result mdbx_drop_tree(MDBX_cursor *mc,
                                              const bool may_have_subDBs);
static int __must_check_result mdbx_fetch_sdb(MDBX_txn *txn, MDBX_dbi dbi);
static int __must_check_result mdbx_setup_dbx(MDBX_dbx *const dbx,
                                              const MDBX_db *const db,
                                              const unsigned pagesize);

static MDBX_cmp_func cmp_lexical, cmp_reverse, cmp_int_align4, cmp_int_align2,
    cmp_int_unaligned, cmp_lenfast;

static __inline MDBX_cmp_func *get_default_keycmp(unsigned flags);
static __inline MDBX_cmp_func *get_default_datacmp(unsigned flags);

__cold const char *mdbx_liberr2str(int errnum) {
  /* Table of descriptions for MDBX errors */
  static const char *const tbl[] = {
      "MDBX_KEYEXIST: Key/data pair already exists",
      "MDBX_NOTFOUND: No matching key/data pair found",
      "MDBX_PAGE_NOTFOUND: Requested page not found",
      "MDBX_CORRUPTED: Database is corrupted",
      "MDBX_PANIC: Environment had fatal error",
      "MDBX_VERSION_MISMATCH: DB version mismatch libmdbx",
      "MDBX_INVALID: File is not an MDBX file",
      "MDBX_MAP_FULL: Environment mapsize limit reached",
      "MDBX_DBS_FULL: Too many DBI-handles (maxdbs reached)",
      "MDBX_READERS_FULL: Too many readers (maxreaders reached)",
      NULL /* MDBX_TLS_FULL (-30789): unused in MDBX */,
      "MDBX_TXN_FULL: Transaction has too many dirty pages,"
      " i.e transaction is too big",
      "MDBX_CURSOR_FULL: Cursor stack limit reachedn - this usually indicates"
      " corruption, i.e branch-pages loop",
      "MDBX_PAGE_FULL: Internal error - Page has no more space",
      "MDBX_UNABLE_EXTEND_MAPSIZE: Database engine was unable to extend"
      " mapping, e.g. since address space is unavailable or busy,"
      " or Operation system not supported such operations",
      "MDBX_INCOMPATIBLE: Environment or database is not compatible"
      " with the requested operation or the specified flags",
      "MDBX_BAD_RSLOT: Invalid reuse of reader locktable slot,"
      " e.g. read-transaction already run for current thread",
      "MDBX_BAD_TXN: Transaction is not valid for requested operation,"
      " e.g. had errored and be must aborted, has a child, or is invalid",
      "MDBX_BAD_VALSIZE: Invalid size or alignment of key or data"
      " for target database, either invalid subDB name",
      "MDBX_BAD_DBI: The specified DBI-handle is invalid"
      " or changed by another thread/transaction",
      "MDBX_PROBLEM: Unexpected internal error, transaction should be aborted",
      "MDBX_BUSY: Another write transaction is running,"
      " or environment is already used while opening with MDBX_EXCLUSIVE flag",
  };

  if (errnum >= MDBX_KEYEXIST && errnum <= MDBX_BUSY) {
    int i = errnum - MDBX_KEYEXIST;
    return tbl[i];
  }

  switch (errnum) {
  case MDBX_SUCCESS:
    return "MDBX_SUCCESS: Successful";
  case MDBX_EMULTIVAL:
    return "MDBX_EMULTIVAL: The specified key has"
           " more than one associated value";
  case MDBX_EBADSIGN:
    return "MDBX_EBADSIGN: Wrong signature of a runtime object(s),"
           " e.g. memory corruption or double-free";
  case MDBX_WANNA_RECOVERY:
    return "MDBX_WANNA_RECOVERY: Database should be recovered,"
           " but this could NOT be done automatically for now"
           " since it opened in read-only mode";
  case MDBX_EKEYMISMATCH:
    return "MDBX_EKEYMISMATCH: The given key value is mismatched to the"
           " current cursor position";
  case MDBX_TOO_LARGE:
    return "MDBX_TOO_LARGE: Database is too large for current system,"
           " e.g. could NOT be mapped into RAM";
  case MDBX_THREAD_MISMATCH:
    return "MDBX_THREAD_MISMATCH: A thread has attempted to use a not"
           " owned object, e.g. a transaction that started by another thread";
  case MDBX_TXN_OVERLAPPING:
    return "MDBX_TXN_OVERLAPPING: Overlapping read and write transactions for"
           " the current thread";
  default:
    return NULL;
  }
}

__cold const char *mdbx_strerror_r(int errnum, char *buf, size_t buflen) {
  const char *msg = mdbx_liberr2str(errnum);
  if (!msg && buflen > 0 && buflen < INT_MAX) {
#if defined(_WIN32) || defined(_WIN64)
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
        errnum, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, (DWORD)buflen,
        NULL);
    return size ? buf : "FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM) failed";
#elif defined(_GNU_SOURCE) && defined(__GLIBC__)
    /* GNU-specific */
    if (errnum > 0)
      msg = strerror_r(errnum, buf, buflen);
#elif (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600)
    /* XSI-compliant */
    if (errnum > 0 && strerror_r(errnum, buf, buflen) == 0)
      msg = buf;
#else
    if (errnum > 0) {
      msg = strerror(errnum);
      if (msg) {
        strncpy(buf, msg, buflen);
        msg = buf;
      }
    }
#endif
    if (!msg) {
      (void)snprintf(buf, buflen, "error %d", errnum);
      msg = buf;
    }
    buf[buflen - 1] = '\0';
  }
  return msg;
}

__cold const char *mdbx_strerror(int errnum) {
#if defined(_WIN32) || defined(_WIN64)
  static char buf[1024];
  return mdbx_strerror_r(errnum, buf, sizeof(buf));
#else
  const char *msg = mdbx_liberr2str(errnum);
  if (!msg) {
    if (errnum > 0)
      msg = strerror(errnum);
    if (!msg) {
      static char buf[32];
      (void)snprintf(buf, sizeof(buf) - 1, "error %d", errnum);
      msg = buf;
    }
  }
  return msg;
#endif
}

#if defined(_WIN32) || defined(_WIN64) /* Bit of madness for Windows */
const char *mdbx_strerror_r_ANSI2OEM(int errnum, char *buf, size_t buflen) {
  const char *msg = mdbx_liberr2str(errnum);
  if (!msg && buflen > 0 && buflen < INT_MAX) {
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
        errnum, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, (DWORD)buflen,
        NULL);
    if (!size)
      msg = "FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM) failed";
    else if (!CharToOemBuffA(buf, buf, size))
      msg = "CharToOemBuffA() failed";
    else
      msg = buf;
  }
  return msg;
}

const char *mdbx_strerror_ANSI2OEM(int errnum) {
  static char buf[1024];
  return mdbx_strerror_r_ANSI2OEM(errnum, buf, sizeof(buf));
}
#endif /* Bit of madness for Windows */

__cold void mdbx_debug_log_va(int level, const char *function, int line,
                              const char *fmt, va_list args) {
  if (mdbx_debug_logger)
    mdbx_debug_logger(level, function, line, fmt, args);
  else {
#if defined(_WIN32) || defined(_WIN64)
    if (IsDebuggerPresent()) {
      int prefix_len = 0;
      char *prefix = nullptr;
      if (function && line > 0)
        prefix_len = mdbx_asprintf(&prefix, "%s:%d ", function, line);
      else if (function)
        prefix_len = mdbx_asprintf(&prefix, "%s: ", function);
      else if (line > 0)
        prefix_len = mdbx_asprintf(&prefix, "%d: ", line);
      if (prefix_len > 0 && prefix) {
        OutputDebugStringA(prefix);
        mdbx_free(prefix);
      }
      char *msg = nullptr;
      int msg_len = mdbx_vasprintf(&msg, fmt, args);
      if (msg_len > 0 && msg) {
        OutputDebugStringA(msg);
        mdbx_free(msg);
      }
    }
#else
    if (function && line > 0)
      fprintf(stderr, "%s:%d ", function, line);
    else if (function)
      fprintf(stderr, "%s: ", function);
    else if (line > 0)
      fprintf(stderr, "%d: ", line);
    vfprintf(stderr, fmt, args);
    fflush(stderr);
#endif
  }
}

__cold void mdbx_debug_log(int level, const char *function, int line,
                           const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  mdbx_debug_log_va(level, function, line, fmt, args);
  va_end(args);
}

/* Dump a key in ascii or hexadecimal. */
const char *mdbx_dump_val(const MDBX_val *key, char *const buf,
                          const size_t bufsize) {
  if (!key)
    return "<null>";
  if (!key->iov_len)
    return "<empty>";
  if (!buf || bufsize < 4)
    return nullptr;

  bool is_ascii = true;
  const uint8_t *const data = key->iov_base;
  for (unsigned i = 0; i < key->iov_len; i++)
    if (data[i] < ' ' || data[i] > '~') {
      is_ascii = false;
      break;
    }

  if (is_ascii) {
    int len =
        snprintf(buf, bufsize, "%.*s",
                 (key->iov_len > INT_MAX) ? INT_MAX : (int)key->iov_len, data);
    assert(len > 0 && (unsigned)len < bufsize);
    (void)len;
  } else {
    char *const detent = buf + bufsize - 2;
    char *ptr = buf;
    *ptr++ = '<';
    for (unsigned i = 0; i < key->iov_len; i++) {
      const ptrdiff_t left = detent - ptr;
      assert(left > 0);
      int len = snprintf(ptr, left, "%02x", data[i]);
      if (len < 0 || len >= left)
        break;
      ptr += len;
    }
    if (ptr < detent) {
      ptr[0] = '>';
      ptr[1] = '\0';
    }
  }
  return buf;
}

/*------------------------------------------------------------------------------
 LY: debug stuff */

static const char *mdbx_leafnode_type(MDBX_node *n) {
  static const char *const tp[2][2] = {{"", ": DB"},
                                       {": sub-page", ": sub-DB"}};
  return F_ISSET(node_flags(n), F_BIGDATA)
             ? ": overflow page"
             : tp[F_ISSET(node_flags(n), F_DUPDATA)]
                 [F_ISSET(node_flags(n), F_SUBDATA)];
}

/* Display all the keys in the page. */
MDBX_MAYBE_UNUSED static void mdbx_page_list(MDBX_page *mp) {
  pgno_t pgno = mp->mp_pgno;
  const char *type;
  MDBX_node *node;
  unsigned i, nkeys, nsize, total = 0;
  MDBX_val key;
  DKBUF;

  switch (mp->mp_flags &
          (P_BRANCH | P_LEAF | P_LEAF2 | P_META | P_OVERFLOW | P_SUBP)) {
  case P_BRANCH:
    type = "Branch page";
    break;
  case P_LEAF:
    type = "Leaf page";
    break;
  case P_LEAF | P_SUBP:
    type = "Leaf sub-page";
    break;
  case P_LEAF | P_LEAF2:
    type = "Leaf2 page";
    break;
  case P_LEAF | P_LEAF2 | P_SUBP:
    type = "Leaf2 sub-page";
    break;
  case P_OVERFLOW:
    mdbx_verbose("Overflow page %" PRIaPGNO " pages %u\n", pgno, mp->mp_pages);
    return;
  case P_META:
    mdbx_verbose("Meta-page %" PRIaPGNO " txnid %" PRIu64 "\n", pgno,
                 unaligned_peek_u64(4, page_meta(mp)->mm_txnid_a));
    return;
  default:
    mdbx_verbose("Bad page %" PRIaPGNO " flags 0x%X\n", pgno, mp->mp_flags);
    return;
  }

  nkeys = page_numkeys(mp);
  mdbx_verbose("%s %" PRIaPGNO " numkeys %u\n", type, pgno, nkeys);

  for (i = 0; i < nkeys; i++) {
    if (IS_LEAF2(mp)) { /* LEAF2 pages have no mp_ptrs[] or node headers */
      key.iov_len = nsize = mp->mp_leaf2_ksize;
      key.iov_base = page_leaf2key(mp, i, nsize);
      total += nsize;
      mdbx_verbose("key %u: nsize %u, %s\n", i, nsize, DKEY(&key));
      continue;
    }
    node = page_node(mp, i);
    key.iov_len = node_ks(node);
    key.iov_base = node->mn_data;
    nsize = (unsigned)(NODESIZE + key.iov_len);
    if (IS_BRANCH(mp)) {
      mdbx_verbose("key %u: page %" PRIaPGNO ", %s\n", i, node_pgno(node),
                   DKEY(&key));
      total += nsize;
    } else {
      if (F_ISSET(node_flags(node), F_BIGDATA))
        nsize += sizeof(pgno_t);
      else
        nsize += (unsigned)node_ds(node);
      total += nsize;
      nsize += sizeof(indx_t);
      mdbx_verbose("key %u: nsize %u, %s%s\n", i, nsize, DKEY(&key),
                   mdbx_leafnode_type(node));
    }
    total = EVEN(total);
  }
  mdbx_verbose("Total: header %u + contents %u + unused %u\n",
               IS_LEAF2(mp) ? PAGEHDRSZ : PAGEHDRSZ + mp->mp_lower, total,
               page_room(mp));
}

/*----------------------------------------------------------------------------*/

/* Check if there is an initialized xcursor, so XCURSOR_REFRESH() is proper */
#define XCURSOR_INITED(mc)                                                     \
  ((mc)->mc_xcursor && ((mc)->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED))

/* Update sub-page pointer, if any, in mc->mc_xcursor.
 * Needed when the node which contains the sub-page may have moved.
 * Called with mp = mc->mc_pg[mc->mc_top], ki = mc->mc_ki[mc->mc_top]. */
#define XCURSOR_REFRESH(mc, mp, ki)                                            \
  do {                                                                         \
    MDBX_page *xr_pg = (mp);                                                   \
    MDBX_node *xr_node = page_node(xr_pg, ki);                                 \
    if ((node_flags(xr_node) & (F_DUPDATA | F_SUBDATA)) == F_DUPDATA)          \
      (mc)->mc_xcursor->mx_cursor.mc_pg[0] = node_data(xr_node);               \
  } while (0)

MDBX_MAYBE_UNUSED static bool cursor_is_tracked(const MDBX_cursor *mc) {
  for (MDBX_cursor *scan = mc->mc_txn->tw.cursors[mc->mc_dbi]; scan;
       scan = scan->mc_next)
    if (mc == ((mc->mc_flags & C_SUB) ? &scan->mc_xcursor->mx_cursor : scan))
      return true;
  return false;
}

/* Perform act while tracking temporary cursor mn */
#define WITH_CURSOR_TRACKING(mn, act)                                          \
  do {                                                                         \
    mdbx_cassert(&(mn),                                                        \
                 mn.mc_txn->tw.cursors != NULL /* must be not rdonly txt */);  \
    mdbx_cassert(&(mn), !cursor_is_tracked(&(mn)));                            \
    MDBX_cursor mc_dummy;                                                      \
    MDBX_cursor **tracking_head = &(mn).mc_txn->tw.cursors[mn.mc_dbi];         \
    MDBX_cursor *tracked = &(mn);                                              \
    if ((mn).mc_flags & C_SUB) {                                               \
      mc_dummy.mc_flags = C_INITIALIZED;                                       \
      mc_dummy.mc_top = 0;                                                     \
      mc_dummy.mc_snum = 0;                                                    \
      mc_dummy.mc_xcursor = (MDBX_xcursor *)&(mn);                             \
      tracked = &mc_dummy;                                                     \
    }                                                                          \
    tracked->mc_next = *tracking_head;                                         \
    *tracking_head = tracked;                                                  \
    { act; }                                                                   \
    *tracking_head = tracked->mc_next;                                         \
  } while (0)

int mdbx_cmp(const MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *a,
             const MDBX_val *b) {
  mdbx_assert(NULL, txn->mt_signature == MDBX_MT_SIGNATURE);
  return txn->mt_dbxs[dbi].md_cmp(a, b);
}

int mdbx_dcmp(const MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *a,
              const MDBX_val *b) {
  mdbx_assert(NULL, txn->mt_signature == MDBX_MT_SIGNATURE);
  return txn->mt_dbxs[dbi].md_dcmp(a, b);
}

/* Allocate memory for a page.
 * Re-use old malloc'ed pages first for singletons, otherwise just malloc.
 * Set MDBX_TXN_ERROR on failure. */
static MDBX_page *mdbx_page_malloc(MDBX_txn *txn, unsigned num) {
  MDBX_env *env = txn->mt_env;
  MDBX_page *np = env->me_dp_reserve;
  size_t size = env->me_psize;
  if (likely(num == 1 && np)) {
    mdbx_assert(env, env->me_dp_reserve_len > 0);
    MDBX_ASAN_UNPOISON_MEMORY_REGION(np, size);
    VALGRIND_MEMPOOL_ALLOC(env, np, size);
    VALGRIND_MAKE_MEM_DEFINED(&np->mp_next, sizeof(np->mp_next));
    env->me_dp_reserve = np->mp_next;
    env->me_dp_reserve_len -= 1;
  } else {
    size = pgno2bytes(env, num);
    np = mdbx_malloc(size);
    if (unlikely(!np)) {
      txn->mt_flags |= MDBX_TXN_ERROR;
      return np;
    }
    VALGRIND_MEMPOOL_ALLOC(env, np, size);
  }

  if ((env->me_flags & MDBX_NOMEMINIT) == 0) {
    /* For a single page alloc, we init everything after the page header.
     * For multi-page, we init the final page; if the caller needed that
     * many pages they will be filling in at least up to the last page. */
    size_t skip = PAGEHDRSZ;
    if (num > 1)
      skip += pgno2bytes(env, num - 1);
    memset((char *)np + skip, 0, size - skip);
  }
#if MDBX_DEBUG
  np->mp_pgno = 0;
#endif
  VALGRIND_MAKE_MEM_UNDEFINED(np, size);
  np->mp_flags = 0;
  np->mp_pages = num;
  return np;
}

/* Free a shadow dirty page */
static void mdbx_dpage_free(MDBX_env *env, MDBX_page *dp, unsigned npages) {
  VALGRIND_MAKE_MEM_UNDEFINED(dp, pgno2bytes(env, npages));
  MDBX_ASAN_UNPOISON_MEMORY_REGION(dp, pgno2bytes(env, npages));
  if (MDBX_DEBUG != 0 || unlikely(env->me_flags & MDBX_PAGEPERTURB))
    memset(dp, -1, pgno2bytes(env, npages));
  if (npages == 1 &&
      env->me_dp_reserve_len < env->me_options.dp_reserve_limit) {
    MDBX_ASAN_POISON_MEMORY_REGION((char *)dp + sizeof(dp->mp_next),
                                   pgno2bytes(env, npages) -
                                       sizeof(dp->mp_next));
    dp->mp_next = env->me_dp_reserve;
    VALGRIND_MEMPOOL_FREE(env, dp);
    env->me_dp_reserve = dp;
    env->me_dp_reserve_len += 1;
  } else {
    /* large pages just get freed directly */
    VALGRIND_MEMPOOL_FREE(env, dp);
    mdbx_free(dp);
  }
}

/* Return all dirty pages to dpage list */
static void mdbx_dlist_free(MDBX_txn *txn) {
  MDBX_env *env = txn->mt_env;
  MDBX_dpl *const dl = txn->tw.dirtylist;

  for (unsigned i = 1; i <= dl->length; i++) {
    MDBX_page *dp = dl->items[i].ptr;
    mdbx_dpage_free(env, dp, dpl_npages(dl, i));
  }

  dpl_clear(dl);
}

static __always_inline MDBX_db *mdbx_outer_db(MDBX_cursor *mc) {
  mdbx_cassert(mc, (mc->mc_flags & C_SUB) != 0);
  MDBX_xcursor *mx = container_of(mc->mc_db, MDBX_xcursor, mx_db);
  MDBX_cursor_couple *couple = container_of(mx, MDBX_cursor_couple, inner);
  mdbx_cassert(mc, mc->mc_db == &couple->outer.mc_xcursor->mx_db);
  mdbx_cassert(mc, mc->mc_dbx == &couple->outer.mc_xcursor->mx_dbx);
  return couple->outer.mc_db;
}

MDBX_MAYBE_UNUSED __cold static bool mdbx_dirtylist_check(MDBX_txn *txn) {
  const MDBX_dpl *const dl = txn->tw.dirtylist;
  assert(dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  mdbx_tassert(txn, txn->tw.dirtyroom + dl->length ==
                        (txn->mt_parent ? txn->mt_parent->tw.dirtyroom
                                        : txn->mt_env->me_options.dp_limit));

  if (!mdbx_audit_enabled())
    return true;

  unsigned loose = 0;
  for (unsigned i = dl->length; i > 0; --i) {
    const MDBX_page *const dp = dl->items[i].ptr;
    if (!dp)
      continue;

    mdbx_tassert(txn, dp->mp_pgno == dl->items[i].pgno);
    if (unlikely(dp->mp_pgno != dl->items[i].pgno))
      return false;

    const uint32_t age = mdbx_dpl_age(txn, i);
    mdbx_tassert(txn, age < UINT32_MAX / 3);
    if (unlikely(age > UINT32_MAX / 3))
      return false;

    mdbx_tassert(txn, dp->mp_flags == P_LOOSE || IS_MODIFIABLE(txn, dp));
    if (dp->mp_flags == P_LOOSE) {
      loose += 1;
    } else if (unlikely(!IS_MODIFIABLE(txn, dp)))
      return false;

    const unsigned num = dpl_npages(dl, i);
    mdbx_tassert(txn, txn->mt_next_pgno >= dp->mp_pgno + num);
    if (unlikely(txn->mt_next_pgno < dp->mp_pgno + num))
      return false;

    if (i < dl->sorted) {
      mdbx_tassert(txn, dl->items[i + 1].pgno >= dp->mp_pgno + num);
      if (unlikely(dl->items[i + 1].pgno < dp->mp_pgno + num))
        return false;
    }

    const unsigned rpa = mdbx_pnl_search(txn->tw.reclaimed_pglist, dp->mp_pgno);
    mdbx_tassert(txn, rpa > MDBX_PNL_SIZE(txn->tw.reclaimed_pglist) ||
                          txn->tw.reclaimed_pglist[rpa] != dp->mp_pgno);
    if (rpa <= MDBX_PNL_SIZE(txn->tw.reclaimed_pglist) &&
        unlikely(txn->tw.reclaimed_pglist[rpa] == dp->mp_pgno))
      return false;
    if (num > 1) {
      const unsigned rpb =
          mdbx_pnl_search(txn->tw.reclaimed_pglist, dp->mp_pgno + num - 1);
      mdbx_tassert(txn, rpa == rpb);
      if (unlikely(rpa != rpb))
        return false;
    }
  }

  mdbx_tassert(txn, loose == txn->tw.loose_count);
  if (unlikely(loose != txn->tw.loose_count))
    return false;

  for (unsigned i = 1; i <= MDBX_PNL_SIZE(txn->tw.retired_pages); ++i) {
    const MDBX_page *const dp = debug_dpl_find(txn, txn->tw.retired_pages[i]);
    mdbx_tassert(txn, !dp);
    if (unlikely(dp))
      return false;
  }

  return true;
}

#if MDBX_ENABLE_REFUND
static void mdbx_refund_reclaimed(MDBX_txn *txn) {
  /* Scanning in descend order */
  pgno_t next_pgno = txn->mt_next_pgno;
  const MDBX_PNL pnl = txn->tw.reclaimed_pglist;
  mdbx_tassert(txn, MDBX_PNL_SIZE(pnl) && MDBX_PNL_MOST(pnl) == next_pgno - 1);
#if MDBX_PNL_ASCENDING
  unsigned i = MDBX_PNL_SIZE(pnl);
  mdbx_tassert(txn, pnl[i] == next_pgno - 1);
  while (--next_pgno, --i > 0 && pnl[i] == next_pgno - 1)
    ;
  MDBX_PNL_SIZE(pnl) = i;
#else
  unsigned i = 1;
  mdbx_tassert(txn, pnl[i] == next_pgno - 1);
  unsigned len = MDBX_PNL_SIZE(pnl);
  while (--next_pgno, ++i <= len && pnl[i] == next_pgno - 1)
    ;
  MDBX_PNL_SIZE(pnl) = len -= i - 1;
  for (unsigned move = 0; move < len; ++move)
    pnl[1 + move] = pnl[i + move];
#endif
  mdbx_verbose("refunded %" PRIaPGNO " pages: %" PRIaPGNO " -> %" PRIaPGNO,
               txn->mt_next_pgno - next_pgno, txn->mt_next_pgno, next_pgno);
  txn->mt_next_pgno = next_pgno;
  mdbx_tassert(txn, mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                          txn->mt_next_pgno - 1));
}

static void mdbx_refund_loose(MDBX_txn *txn) {
  mdbx_tassert(txn, txn->tw.loose_pages != nullptr);
  mdbx_tassert(txn, txn->tw.loose_count > 0);

  MDBX_dpl *const dl = txn->tw.dirtylist;
  mdbx_tassert(txn, dl->length >= txn->tw.loose_count);

  pgno_t onstack[MDBX_CACHELINE_SIZE * 8 / sizeof(pgno_t)];
  MDBX_PNL suitable = onstack;

  if (dl->length - dl->sorted > txn->tw.loose_count) {
    /* Dirty list is useless since unsorted. */
    if (bytes2pnl(sizeof(onstack)) < txn->tw.loose_count) {
      suitable = mdbx_pnl_alloc(txn->tw.loose_count);
      if (unlikely(!suitable))
        return /* this is not a reason for transaction fail */;
    }

    /* Collect loose-pages which may be refunded. */
    mdbx_tassert(txn, txn->mt_next_pgno >= MIN_PAGENO + txn->tw.loose_count);
    pgno_t most = MIN_PAGENO;
    unsigned w = 0;
    for (const MDBX_page *lp = txn->tw.loose_pages; lp; lp = lp->mp_next) {
      mdbx_tassert(txn, lp->mp_flags == P_LOOSE);
      mdbx_tassert(txn, txn->mt_next_pgno > lp->mp_pgno);
      if (likely(txn->mt_next_pgno - txn->tw.loose_count <= lp->mp_pgno)) {
        mdbx_tassert(txn,
                     w < ((suitable == onstack) ? bytes2pnl(sizeof(onstack))
                                                : MDBX_PNL_ALLOCLEN(suitable)));
        suitable[++w] = lp->mp_pgno;
        most = (lp->mp_pgno > most) ? lp->mp_pgno : most;
      }
    }

    if (most + 1 == txn->mt_next_pgno) {
      /* Sort suitable list and refund pages at the tail. */
      MDBX_PNL_SIZE(suitable) = w;
      mdbx_pnl_sort(suitable, MAX_PAGENO + 1);

      /* Scanning in descend order */
      const int step = MDBX_PNL_ASCENDING ? -1 : 1;
      const int begin = MDBX_PNL_ASCENDING ? MDBX_PNL_SIZE(suitable) : 1;
      const int end = MDBX_PNL_ASCENDING ? 0 : MDBX_PNL_SIZE(suitable) + 1;
      mdbx_tassert(txn, suitable[begin] >= suitable[end - step]);
      mdbx_tassert(txn, most == suitable[begin]);

      for (int i = begin + step; i != end; i += step) {
        if (suitable[i] != most - 1)
          break;
        most -= 1;
      }
      const unsigned refunded = txn->mt_next_pgno - most;
      mdbx_debug("refund-suitable %u pages %" PRIaPGNO " -> %" PRIaPGNO,
                 refunded, most, txn->mt_next_pgno);
      txn->tw.loose_count -= refunded;
      txn->tw.dirtyroom += refunded;
      assert(txn->tw.dirtyroom <= txn->mt_env->me_options.dp_limit);
      txn->mt_next_pgno = most;

      /* Filter-out dirty list */
      unsigned r = 0;
      w = 0;
      if (dl->sorted) {
        do {
          if (dl->items[++r].pgno < most) {
            if (++w != r)
              dl->items[w] = dl->items[r];
          }
        } while (r < dl->sorted);
        dl->sorted = w;
      }
      while (r < dl->length) {
        if (dl->items[++r].pgno < most) {
          if (++w != r)
            dl->items[w] = dl->items[r];
        }
      }
      dpl_setlen(dl, w);
      mdbx_tassert(txn,
                   txn->tw.dirtyroom + txn->tw.dirtylist->length ==
                       (txn->mt_parent ? txn->mt_parent->tw.dirtyroom
                                       : txn->mt_env->me_options.dp_limit));

      goto unlink_loose;
    }
  } else {
    /* Dirtylist is mostly sorted, just refund loose pages at the end. */
    mdbx_dpl_sort(txn);
    mdbx_tassert(txn, dl->length < 2 ||
                          dl->items[1].pgno < dl->items[dl->length].pgno);
    mdbx_tassert(txn, dl->sorted == dl->length);

    /* Scan dirtylist tail-forward and cutoff suitable pages. */
    unsigned n;
    for (n = dl->length; dl->items[n].pgno == txn->mt_next_pgno - 1 &&
                         dl->items[n].ptr->mp_flags == P_LOOSE;
         --n) {
      mdbx_tassert(txn, n > 0);
      MDBX_page *dp = dl->items[n].ptr;
      mdbx_debug("refund-sorted page %" PRIaPGNO, dp->mp_pgno);
      mdbx_tassert(txn, dp->mp_pgno == dl->items[n].pgno);
      txn->mt_next_pgno -= 1;
    }
    dpl_setlen(dl, n);

    if (dl->sorted != dl->length) {
      const unsigned refunded = dl->sorted - dl->length;
      dl->sorted = dl->length;
      txn->tw.loose_count -= refunded;
      txn->tw.dirtyroom += refunded;
      mdbx_tassert(txn,
                   txn->tw.dirtyroom + txn->tw.dirtylist->length ==
                       (txn->mt_parent ? txn->mt_parent->tw.dirtyroom
                                       : txn->mt_env->me_options.dp_limit));

      /* Filter-out loose chain & dispose refunded pages. */
    unlink_loose:
      for (MDBX_page **link = &txn->tw.loose_pages; *link;) {
        MDBX_page *dp = *link;
        mdbx_tassert(txn, dp->mp_flags == P_LOOSE);
        if (txn->mt_next_pgno > dp->mp_pgno) {
          link = &dp->mp_next;
        } else {
          *link = dp->mp_next;
          if ((txn->mt_flags & MDBX_WRITEMAP) == 0)
            mdbx_dpage_free(txn->mt_env, dp, 1);
        }
      }
    }
  }

  mdbx_tassert(txn, mdbx_dirtylist_check(txn));
  if (suitable != onstack)
    mdbx_pnl_free(suitable);
  txn->tw.loose_refund_wl = txn->mt_next_pgno;
}

static bool mdbx_refund(MDBX_txn *txn) {
  const pgno_t before = txn->mt_next_pgno;

  if (txn->tw.loose_pages && txn->tw.loose_refund_wl > txn->mt_next_pgno)
    mdbx_refund_loose(txn);

  while (true) {
    if (MDBX_PNL_SIZE(txn->tw.reclaimed_pglist) == 0 ||
        MDBX_PNL_MOST(txn->tw.reclaimed_pglist) != txn->mt_next_pgno - 1)
      break;

    mdbx_refund_reclaimed(txn);
    if (!txn->tw.loose_pages || txn->tw.loose_refund_wl <= txn->mt_next_pgno)
      break;

    const pgno_t memo = txn->mt_next_pgno;
    mdbx_refund_loose(txn);
    if (memo == txn->mt_next_pgno)
      break;
  }

  if (before == txn->mt_next_pgno)
    return false;

  if (txn->tw.spill_pages)
    /* Squash deleted pagenums if we refunded any */
    mdbx_spill_purge(txn);

  return true;
}
#else  /* MDBX_ENABLE_REFUND */
static __inline bool mdbx_refund(MDBX_txn *txn) {
  (void)txn;
  /* No online auto-compactification. */
  return false;
}
#endif /* MDBX_ENABLE_REFUND */

__cold static void mdbx_kill_page(MDBX_txn *txn, MDBX_page *mp, pgno_t pgno,
                                  unsigned npages) {
  MDBX_env *const env = txn->mt_env;
  mdbx_debug("kill %u page(s) %" PRIaPGNO, npages, pgno);
  mdbx_assert(env, pgno >= NUM_METAS && npages);
  if (!IS_FROZEN(txn, mp)) {
    const size_t bytes = pgno2bytes(env, npages);
    memset(mp, -1, bytes);
    mp->mp_pgno = pgno;
    if ((env->me_flags & MDBX_WRITEMAP) == 0)
      mdbx_pwrite(env->me_lazy_fd, mp, bytes, pgno2bytes(env, pgno));
  } else {
    struct iovec iov[MDBX_COMMIT_PAGES];
    iov[0].iov_len = env->me_psize;
    iov[0].iov_base = (char *)env->me_pbuf + env->me_psize;
    size_t iov_off = pgno2bytes(env, pgno);
    unsigned n = 1;
    while (--npages) {
      iov[n] = iov[0];
      if (++n == MDBX_COMMIT_PAGES) {
        mdbx_pwritev(env->me_lazy_fd, iov, MDBX_COMMIT_PAGES, iov_off,
                     pgno2bytes(env, MDBX_COMMIT_PAGES));
        iov_off += pgno2bytes(env, MDBX_COMMIT_PAGES);
        n = 0;
      }
    }
    mdbx_pwritev(env->me_lazy_fd, iov, n, iov_off, pgno2bytes(env, n));
  }
}

/* Remove page from dirty list */
static __inline void mdbx_page_wash(MDBX_txn *txn, const unsigned di,
                                    MDBX_page *const mp,
                                    const unsigned npages) {
  mdbx_tassert(txn, di && di <= txn->tw.dirtylist->length &&
                        txn->tw.dirtylist->items[di].ptr == mp);
  mdbx_dpl_remove(txn, di);
  txn->tw.dirtyroom++;
  mdbx_tassert(txn, txn->tw.dirtyroom + txn->tw.dirtylist->length ==
                        (txn->mt_parent ? txn->mt_parent->tw.dirtyroom
                                        : txn->mt_env->me_options.dp_limit));
  mp->mp_txnid = INVALID_TXNID;
  mp->mp_flags = 0xFFFF;
  VALGRIND_MAKE_MEM_UNDEFINED(mp, PAGEHDRSZ);
  if (txn->mt_flags & MDBX_WRITEMAP) {
    VALGRIND_MAKE_MEM_NOACCESS(page_data(mp),
                               pgno2bytes(txn->mt_env, npages) - PAGEHDRSZ);
    MDBX_ASAN_POISON_MEMORY_REGION(page_data(mp),
                                   pgno2bytes(txn->mt_env, npages) - PAGEHDRSZ);
  } else
    mdbx_dpage_free(txn->mt_env, mp, npages);
}

static __inline txnid_t pp_txnid4chk(const MDBX_page *mp, const MDBX_txn *txn) {
  (void)txn;
#if MDBX_DISABLE_PAGECHECKS
  (void)mp;
  return 0;
#else
  return /* maybe zero in legacy DB */ mp->mp_txnid;
#endif /* !MDBX_DISABLE_PAGECHECKS */
}

/* Retire, loosen or free a single page.
 *
 * For dirty pages, saves single pages to a list for future reuse in this same
 * txn. It has been pulled from the GC and already resides on the dirty list,
 * but has been deleted. Use these pages first before pulling again from the GC.
 *
 * If the page wasn't dirtied in this txn, just add it
 * to this txn's free list. */
static int mdbx_page_retire_ex(MDBX_cursor *mc, const pgno_t pgno,
                               MDBX_page *mp /* maybe null */,
                               int pagetype /* maybe unknown/zero */) {
  int rc;
  MDBX_txn *const txn = mc->mc_txn;
  mdbx_tassert(txn, !mp || (mp->mp_pgno == pgno && PAGETYPE(mp) == pagetype));

  /* During deleting entire subtrees, it is reasonable and possible to avoid
   * reading leaf pages, i.e. significantly reduce hard page-faults & IOPs:
   *  - mp is null, i.e. the page has not yet been read;
   *  - pagetype is known and the P_LEAF bit is set;
   *  - we can determine the page status via scanning the lists
   *    of dirty and spilled pages.
   *
   *  On the other hand, this could be suboptimal for WRITEMAP mode, since
   *  requires support the list of dirty pages and avoid explicit spilling.
   *  So for flexibility and avoid extra internal dependencies we just
   *  fallback to reading if dirty list was not allocated yet. */
  unsigned di = 0, si = 0, npages = 1;
  bool is_frozen = false, is_spilled = false, is_shadowed = false;
  if (unlikely(!mp)) {
    if (mdbx_assert_enabled() && pagetype) {
      MDBX_page *check;
      rc = mdbx_page_get(mc, pgno, &check, txn->mt_front);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      mdbx_tassert(txn, (PAGETYPE(check) & ~P_LEAF2) == (pagetype & ~P_FROZEN));
      mdbx_tassert(txn, !(pagetype & P_FROZEN) || IS_FROZEN(txn, check));
    }
    if (pagetype & P_FROZEN) {
      is_frozen = true;
      if (mdbx_assert_enabled()) {
        for (MDBX_txn *scan = txn; scan; scan = scan->mt_parent) {
          mdbx_tassert(txn,
                       !scan->tw.spill_pages ||
                           !mdbx_pnl_exist(scan->tw.spill_pages, pgno << 1));
          mdbx_tassert(txn, !scan->tw.dirtylist || !debug_dpl_find(scan, pgno));
        }
      }
      goto status_done;
    } else if (pagetype && txn->tw.dirtylist) {
      if ((di = mdbx_dpl_exist(txn, pgno)) != 0) {
        mp = txn->tw.dirtylist->items[di].ptr;
        mdbx_tassert(txn, IS_MODIFIABLE(txn, mp));
        goto status_done;
      }
      if (txn->tw.spill_pages &&
          (si = mdbx_pnl_exist(txn->tw.spill_pages, pgno << 1)) != 0) {
        is_spilled = true;
        goto status_done;
      }
      for (MDBX_txn *parent = txn->mt_parent; parent;
           parent = parent->mt_parent) {
        if (mdbx_dpl_exist(parent, pgno)) {
          is_shadowed = true;
          goto status_done;
        }
        if (parent->tw.spill_pages &&
            mdbx_pnl_exist(parent->tw.spill_pages, pgno << 1)) {
          is_spilled = true;
          goto status_done;
        }
      }
      is_frozen = true;
      goto status_done;
    }

    rc = mdbx_page_get(mc, pgno, &mp, txn->mt_front);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    mdbx_tassert(txn, !pagetype || PAGETYPE(mp) == pagetype);
    pagetype = PAGETYPE(mp);
  }

  is_frozen = IS_FROZEN(txn, mp);
  if (!is_frozen) {
    const bool is_dirty = IS_MODIFIABLE(txn, mp);
    is_spilled = IS_SPILLED(txn, mp) && !(txn->mt_flags & MDBX_WRITEMAP);
    is_shadowed = IS_SHADOWED(txn, mp);
    if (is_dirty) {
      mdbx_tassert(txn, !is_spilled);
      mdbx_tassert(txn, !txn->tw.spill_pages ||
                            !mdbx_pnl_exist(txn->tw.spill_pages, pgno << 1));
      mdbx_tassert(txn, debug_dpl_find(txn, pgno) == mp || txn->mt_parent ||
                            (txn->mt_flags & MDBX_WRITEMAP));
    } else {
      mdbx_tassert(txn, !debug_dpl_find(txn, pgno));
    }

    di = is_dirty ? mdbx_dpl_exist(txn, pgno) : 0;
    si = (is_spilled && txn->tw.spill_pages)
             ? mdbx_pnl_exist(txn->tw.spill_pages, pgno << 1)
             : 0;
    mdbx_tassert(txn, !is_dirty || di || (txn->mt_flags & MDBX_WRITEMAP));
  } else {
    mdbx_tassert(txn, !IS_MODIFIABLE(txn, mp));
    mdbx_tassert(txn, !IS_SPILLED(txn, mp));
    mdbx_tassert(txn, !IS_SHADOWED(txn, mp));
  }

status_done:
  if (likely((pagetype & P_OVERFLOW) == 0)) {
    STATIC_ASSERT(P_BRANCH == 1);
    const bool is_branch = pagetype & P_BRANCH;
    if (unlikely(mc->mc_flags & C_SUB)) {
      MDBX_db *outer = mdbx_outer_db(mc);
      mdbx_cassert(mc, !is_branch || outer->md_branch_pages > 0);
      outer->md_branch_pages -= is_branch;
      mdbx_cassert(mc, is_branch || outer->md_leaf_pages > 0);
      outer->md_leaf_pages -= 1 - is_branch;
    }
    mdbx_cassert(mc, !is_branch || mc->mc_db->md_branch_pages > 0);
    mc->mc_db->md_branch_pages -= is_branch;
    mdbx_cassert(mc, (pagetype & P_LEAF) == 0 || mc->mc_db->md_leaf_pages > 0);
    mc->mc_db->md_leaf_pages -= (pagetype & P_LEAF) != 0;
  } else {
    npages = mp->mp_pages;
    mdbx_cassert(mc, mc->mc_db->md_overflow_pages >= npages);
    mc->mc_db->md_overflow_pages -= npages;
  }

  if (is_frozen) {
  retire:
    mdbx_debug("retire %u page %" PRIaPGNO, npages, pgno);
    rc = mdbx_pnl_append_range(false, &txn->tw.retired_pages, pgno, npages);
    mdbx_tassert(txn, mdbx_dirtylist_check(txn));
    return rc;
  }

  /* Возврат страниц в нераспределенный "хвост" БД.
   * Содержимое страниц не уничтожается, а для вложенных транзакций граница
   * нераспределенного "хвоста" БД сдвигается только при их коммите. */
  if (MDBX_ENABLE_REFUND && unlikely(pgno + npages == txn->mt_next_pgno)) {
    const char *kind = nullptr;
    if (di) {
      /* Страница испачкана в этой транзакции, но до этого могла быть
       * аллоцирована, испачкана и пролита в одной из родительских транзакций.
       * Её МОЖНО вытолкнуть в нераспределенный хвост. */
      kind = "dirty";
      /* Remove from dirty list */
      mdbx_page_wash(txn, di, mp, npages);
    } else if (si) {
      /* Страница пролита в этой транзакции, т.е. она аллоцирована
       * и запачкана в этой или одной из родительских транзакций.
       * Её МОЖНО вытолкнуть в нераспределенный хвост. */
      kind = "spilled";
      mdbx_spill_remove(txn, si, npages);
    } else if ((txn->mt_flags & MDBX_WRITEMAP)) {
      kind = "writemap";
      mdbx_tassert(txn, mp && IS_MODIFIABLE(txn, mp));
    } else {
      /* Страница аллоцирована, запачкана и возможно пролита в одной
       * из родительских транзакций.
       * Её МОЖНО вытолкнуть в нераспределенный хвост. */
      kind = "parent's";
      if (mdbx_assert_enabled() && mp) {
        kind = nullptr;
        for (MDBX_txn *parent = txn->mt_parent; parent;
             parent = parent->mt_parent) {
          if (parent->tw.spill_pages &&
              mdbx_pnl_exist(parent->tw.spill_pages, pgno << 1)) {
            kind = "parent-spilled";
            mdbx_tassert(txn, is_spilled);
            break;
          }
          if (mp == debug_dpl_find(parent, pgno)) {
            kind = "parent-dirty";
            mdbx_tassert(txn, !is_spilled);
            break;
          }
        }
        mdbx_tassert(txn, kind != nullptr);
      }
      mdbx_tassert(txn,
                   is_spilled || is_shadowed || (mp && IS_SHADOWED(txn, mp)));
    }
    mdbx_debug("refunded %u %s page %" PRIaPGNO, npages, kind, pgno);
    txn->mt_next_pgno = pgno;
    mdbx_refund(txn);
    return MDBX_SUCCESS;
  }

  if (di) {
    /* Dirty page from this transaction */
    /* If suitable we can reuse it through loose list */
    if (likely(npages == 1 &&
               txn->tw.loose_count < txn->mt_env->me_options.dp_loose_limit &&
               (!MDBX_ENABLE_REFUND ||
                /* skip pages near to the end in favor of compactification */
                txn->mt_next_pgno >
                    pgno + txn->mt_env->me_options.dp_loose_limit ||
                txn->mt_next_pgno <= txn->mt_env->me_options.dp_loose_limit))) {
      mdbx_debug("loosen dirty page %" PRIaPGNO, pgno);
      mp->mp_flags = P_LOOSE;
      mp->mp_next = txn->tw.loose_pages;
      txn->tw.loose_pages = mp;
      txn->tw.loose_count++;
#if MDBX_ENABLE_REFUND
      txn->tw.loose_refund_wl = (pgno + 2 > txn->tw.loose_refund_wl)
                                    ? pgno + 2
                                    : txn->tw.loose_refund_wl;
#endif /* MDBX_ENABLE_REFUND */
      if (MDBX_DEBUG != 0 || unlikely(txn->mt_env->me_flags & MDBX_PAGEPERTURB))
        memset(page_data(mp), -1, txn->mt_env->me_psize - PAGEHDRSZ);
      VALGRIND_MAKE_MEM_NOACCESS(page_data(mp),
                                 txn->mt_env->me_psize - PAGEHDRSZ);
      MDBX_ASAN_POISON_MEMORY_REGION(page_data(mp),
                                     txn->mt_env->me_psize - PAGEHDRSZ);
      return MDBX_SUCCESS;
    }

#if !MDBX_DEBUG && !defined(MDBX_USE_VALGRIND) && !defined(__SANITIZE_ADDRESS__)
    if (unlikely(txn->mt_env->me_flags & MDBX_PAGEPERTURB))
#endif
    {
      /* Страница могла быть изменена в одной из родительских транзакций,
       * в том числе, позже выгружена и затем снова загружена и изменена.
       * В обоих случаях её нельзя затирать на диске и помечать недоступной
       * в asan и/или valgrind */
      for (MDBX_txn *parent = txn->mt_parent;
           parent && (parent->mt_flags & MDBX_TXN_SPILLS);
           parent = parent->mt_parent) {
        if (parent->tw.spill_pages &&
            mdbx_pnl_intersect(parent->tw.spill_pages, pgno << 1, npages << 1))
          goto skip_invalidate;
        if (mdbx_dpl_intersect(parent, pgno, npages))
          goto skip_invalidate;
      }

#if defined(MDBX_USE_VALGRIND) || defined(__SANITIZE_ADDRESS__)
      if (MDBX_DEBUG != 0 || unlikely(txn->mt_env->me_flags & MDBX_PAGEPERTURB))
#endif
        mdbx_kill_page(txn, mp, pgno, npages);
      if (!(txn->mt_flags & MDBX_WRITEMAP)) {
        VALGRIND_MAKE_MEM_NOACCESS(page_data(pgno2page(txn->mt_env, pgno)),
                                   pgno2bytes(txn->mt_env, npages) - PAGEHDRSZ);
        MDBX_ASAN_POISON_MEMORY_REGION(page_data(pgno2page(txn->mt_env, pgno)),
                                       pgno2bytes(txn->mt_env, npages) -
                                           PAGEHDRSZ);
      }
    }
  skip_invalidate:
    /* Remove from dirty list */
    mdbx_page_wash(txn, di, mp, npages);

  reclaim:
    mdbx_debug("reclaim %u %s page %" PRIaPGNO, npages, "dirty", pgno);
    rc = mdbx_pnl_insert_range(&txn->tw.reclaimed_pglist, pgno, npages);
    mdbx_tassert(txn,
                 mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                       txn->mt_next_pgno - MDBX_ENABLE_REFUND));
    mdbx_tassert(txn, mdbx_dirtylist_check(txn));
    return rc;
  }

  if (si) {
    /* Page ws spilled in this txn */
    mdbx_spill_remove(txn, si, npages);
    /* Страница могла быть выделена и затем пролита в этой транзакции,
     * тогда её необходимо поместить в reclaimed-список.
     * Либо она могла быть выделена в одной из родительских транзакций и затем
     * пролита в этой транзакции, тогда её необходимо поместить в
     * retired-список для последующей фильтрации при коммите. */
    for (MDBX_txn *parent = txn->mt_parent; parent;
         parent = parent->mt_parent) {
      if (mdbx_dpl_exist(parent, pgno))
        goto retire;
    }
    /* Страница точно была выделена в этой транзакции
     * и теперь может быть использована повторно. */
    goto reclaim;
  }

  if (is_shadowed) {
    /* Dirty page MUST BE a clone from (one of) parent transaction(s). */
    if (mdbx_assert_enabled()) {
      const MDBX_page *parent_dp = nullptr;
      /* Check parent(s)'s dirty lists. */
      for (MDBX_txn *parent = txn->mt_parent; parent && !parent_dp;
           parent = parent->mt_parent) {
        mdbx_tassert(txn,
                     !parent->tw.spill_pages ||
                         !mdbx_pnl_exist(parent->tw.spill_pages, pgno << 1));
        parent_dp = debug_dpl_find(parent, pgno);
      }
      mdbx_tassert(txn, parent_dp && (!mp || parent_dp == mp));
    }
    /* Страница была выделена в родительской транзакции и теперь может быть
     * использована повторно, но только внутри этой транзакции, либо дочерних.
     */
    goto reclaim;
  }

  /* Страница может входить в доступный читателям MVCC-снимок, либо же она
   * могла быть выделена, а затем пролита в одной из родительских
   * транзакций. Поэтому пока помещаем её в retired-список, который будет
   * фильтроваться относительно dirty- и spilled-списков родительских
   * транзакций при коммите дочерних транзакций, либо же будет записан
   * в GC в неизменном виде. */
  goto retire;
}

static __inline int mdbx_page_retire(MDBX_cursor *mc, MDBX_page *mp) {
  return mdbx_page_retire_ex(mc, mp->mp_pgno, mp, PAGETYPE(mp));
}

struct mdbx_iov_ctx {
  unsigned iov_items;
  size_t iov_bytes;
  size_t iov_off;
  pgno_t flush_begin;
  pgno_t flush_end;
  struct iovec iov[MDBX_COMMIT_PAGES];
};

static __inline void mdbx_iov_init(MDBX_txn *const txn,
                                   struct mdbx_iov_ctx *ctx) {
  ctx->flush_begin = MAX_PAGENO;
  ctx->flush_end = MIN_PAGENO;
  ctx->iov_items = 0;
  ctx->iov_bytes = 0;
  ctx->iov_off = 0;
  (void)txn;
}

static __inline void mdbx_iov_done(MDBX_txn *const txn,
                                   struct mdbx_iov_ctx *ctx) {
  mdbx_tassert(txn, ctx->iov_items == 0);
#if defined(__linux__) || defined(__gnu_linux__)
  MDBX_env *const env = txn->mt_env;
  if (!(txn->mt_flags & MDBX_WRITEMAP) &&
      mdbx_linux_kernel_version < 0x02060b00)
    /* Linux kernels older than version 2.6.11 ignore the addr and nbytes
     * arguments, making this function fairly expensive. Therefore, the
     * whole cache is always flushed. */
    mdbx_flush_incoherent_mmap(
        env->me_map + pgno2bytes(env, ctx->flush_begin),
        pgno2bytes(env, ctx->flush_end - ctx->flush_begin), env->me_os_psize);
#endif /* Linux */
}

static int mdbx_iov_write(MDBX_txn *const txn, struct mdbx_iov_ctx *ctx) {
  mdbx_tassert(txn, !(txn->mt_flags & MDBX_WRITEMAP));
  mdbx_tassert(txn, ctx->iov_items > 0);

  MDBX_env *const env = txn->mt_env;
  int rc;
  if (likely(ctx->iov_items == 1)) {
    mdbx_assert(env, ctx->iov_bytes == (size_t)ctx->iov[0].iov_len);
    rc = mdbx_pwrite(env->me_lazy_fd, ctx->iov[0].iov_base, ctx->iov[0].iov_len,
                     ctx->iov_off);
  } else {
    rc = mdbx_pwritev(env->me_lazy_fd, ctx->iov, ctx->iov_items, ctx->iov_off,
                      ctx->iov_bytes);
  }

  if (unlikely(rc != MDBX_SUCCESS))
    mdbx_error("Write error: %s", mdbx_strerror(rc));
  else {
    VALGRIND_MAKE_MEM_DEFINED(txn->mt_env->me_map + ctx->iov_off,
                              ctx->iov_bytes);
    MDBX_ASAN_UNPOISON_MEMORY_REGION(txn->mt_env->me_map + ctx->iov_off,
                                     ctx->iov_bytes);
  }

  for (unsigned i = 0; i < ctx->iov_items; i++)
    mdbx_dpage_free(env, (MDBX_page *)ctx->iov[i].iov_base,
                    bytes2pgno(env, ctx->iov[i].iov_len));

#if MDBX_ENABLE_PGOP_STAT
  txn->mt_env->me_lck->mti_pgop_stat.wops.weak += ctx->iov_items;
#endif /* MDBX_ENABLE_PGOP_STAT */
  ctx->iov_items = 0;
  ctx->iov_bytes = 0;
  return rc;
}

static int iov_page(MDBX_txn *txn, struct mdbx_iov_ctx *ctx, MDBX_page *dp,
                    unsigned npages) {
  MDBX_env *const env = txn->mt_env;
  mdbx_tassert(txn,
               dp->mp_pgno >= MIN_PAGENO && dp->mp_pgno < txn->mt_next_pgno);
  mdbx_tassert(txn, IS_MODIFIABLE(txn, dp));
  mdbx_tassert(txn,
               !(dp->mp_flags & ~(P_BRANCH | P_LEAF | P_LEAF2 | P_OVERFLOW)));

  ctx->flush_begin =
      (ctx->flush_begin < dp->mp_pgno) ? ctx->flush_begin : dp->mp_pgno;
  ctx->flush_end = (ctx->flush_end > dp->mp_pgno + npages)
                       ? ctx->flush_end
                       : dp->mp_pgno + npages;
  env->me_lck->mti_unsynced_pages.weak += npages;

  if (IS_SHADOWED(txn, dp)) {
    mdbx_tassert(txn, !(txn->mt_flags & MDBX_WRITEMAP));
    dp->mp_txnid = txn->mt_txnid;
    mdbx_tassert(txn, IS_SPILLED(txn, dp));
    const size_t size = pgno2bytes(env, npages);
    if (ctx->iov_off + ctx->iov_bytes != pgno2bytes(env, dp->mp_pgno) ||
        ctx->iov_items == ARRAY_LENGTH(ctx->iov) ||
        ctx->iov_bytes + size > MAX_WRITE) {
      if (ctx->iov_items) {
        int err = mdbx_iov_write(txn, ctx);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
#if defined(__linux__) || defined(__gnu_linux__)
        if (mdbx_linux_kernel_version >= 0x02060b00)
        /* Linux kernels older than version 2.6.11 ignore the addr and nbytes
         * arguments, making this function fairly expensive. Therefore, the
         * whole cache is always flushed. */
#endif /* Linux */
          mdbx_flush_incoherent_mmap(env->me_map + ctx->iov_off, ctx->iov_bytes,
                                     env->me_os_psize);
      }
      ctx->iov_off = pgno2bytes(env, dp->mp_pgno);
    }
    ctx->iov[ctx->iov_items].iov_base = (void *)dp;
    ctx->iov[ctx->iov_items].iov_len = size;
    ctx->iov_items += 1;
    ctx->iov_bytes += size;
  } else {
    mdbx_tassert(txn, txn->mt_flags & MDBX_WRITEMAP);
  }
  return MDBX_SUCCESS;
}

static int spill_page(MDBX_txn *txn, struct mdbx_iov_ctx *ctx, MDBX_page *dp,
                      unsigned npages) {
  mdbx_tassert(txn, !(txn->mt_flags & MDBX_WRITEMAP));
  pgno_t pgno = dp->mp_pgno;
  int err = iov_page(txn, ctx, dp, npages);
  if (likely(err == MDBX_SUCCESS)) {
    err = mdbx_pnl_append_range(true, &txn->tw.spill_pages, pgno << 1, npages);
#if MDBX_ENABLE_PGOP_STAT
    if (likely(err == MDBX_SUCCESS))
      txn->mt_env->me_lck->mti_pgop_stat.spill.weak += npages;
#endif /* MDBX_ENABLE_PGOP_STAT */
  }
  return err;
}

/* Set unspillable LRU-label for dirty pages watched by txn.
 * Returns the number of pages marked as unspillable. */
static unsigned mdbx_cursor_keep(MDBX_txn *txn, MDBX_cursor *mc) {
  unsigned keep = 0;
  while (mc->mc_flags & C_INITIALIZED) {
    for (unsigned i = 0; i < mc->mc_snum; ++i) {
      const MDBX_page *mp = mc->mc_pg[i];
      if (IS_MODIFIABLE(txn, mp) && !IS_SUBP(mp)) {
        unsigned const n = mdbx_dpl_search(txn, mp->mp_pgno);
        if (txn->tw.dirtylist->items[n].pgno == mp->mp_pgno &&
            mdbx_dpl_age(txn, n)) {
          txn->tw.dirtylist->items[n].lru = txn->tw.dirtylru;
          ++keep;
        }
      }
    }
    if (!mc->mc_xcursor)
      break;
    mc = &mc->mc_xcursor->mx_cursor;
  }
  return keep;
}

static unsigned mdbx_txn_keep(MDBX_txn *txn, MDBX_cursor *m0) {
  unsigned keep = m0 ? mdbx_cursor_keep(txn, m0) : 0;
  for (unsigned i = FREE_DBI; i < txn->mt_numdbs; ++i)
    if (F_ISSET(txn->mt_dbistate[i], DBI_DIRTY | DBI_VALID) &&
        txn->mt_dbs[i].md_root != P_INVALID)
      for (MDBX_cursor *mc = txn->tw.cursors[i]; mc; mc = mc->mc_next)
        if (mc != m0)
          keep += mdbx_cursor_keep(txn, mc);
  return keep;
}

/* Returns the spilling priority (0..255) for a dirty page:
 *      0 = should be spilled;
 *    ...
 *  > 255 = must not be spilled. */
static unsigned spill_prio(const MDBX_txn *txn, const unsigned i,
                           const uint32_t reciprocal) {
  MDBX_dpl *const dl = txn->tw.dirtylist;
  const uint32_t age = mdbx_dpl_age(txn, i);
  const unsigned npages = dpl_npages(dl, i);
  const pgno_t pgno = dl->items[i].pgno;
  if (age == 0) {
    mdbx_debug("skip %s %u page %" PRIaPGNO, "keep", npages, pgno);
    return 256;
  }

  MDBX_page *const dp = dl->items[i].ptr;
  if (dp->mp_flags & (P_LOOSE | P_SPILLED)) {
    mdbx_debug("skip %s %u page %" PRIaPGNO,
               (dp->mp_flags & P_LOOSE)   ? "loose"
               : (dp->mp_flags & P_LOOSE) ? "loose"
                                          : "parent-spilled",
               npages, pgno);
    return 256;
  }

  /* Can't spill twice,
   * make sure it's not already in a parent's spill list(s). */
  MDBX_txn *parent = txn->mt_parent;
  if (parent && (parent->mt_flags & MDBX_TXN_SPILLS)) {
    do
      if (parent->tw.spill_pages &&
          mdbx_pnl_intersect(parent->tw.spill_pages, pgno << 1, npages << 1)) {
        mdbx_debug("skip-2 parent-spilled %u page %" PRIaPGNO, npages, pgno);
        dp->mp_flags |= P_SPILLED;
        return 256;
      }
    while ((parent = parent->mt_parent) != nullptr);
  }

  mdbx_tassert(txn, age * (uint64_t)reciprocal < UINT32_MAX);
  unsigned prio = age * reciprocal >> 24;
  mdbx_tassert(txn, prio < 256);
  if (likely(npages == 1))
    return prio = 256 - prio;

  /* make a large/overflow pages be likely to spill */
  uint32_t factor = npages | npages >> 1;
  factor |= factor >> 2;
  factor |= factor >> 4;
  factor |= factor >> 8;
  factor |= factor >> 16;
  factor = prio * log2n_powerof2(factor + 1) + /* golden ratio */ 157;
  factor = (factor < 256) ? 255 - factor : 0;
  mdbx_tassert(txn, factor < 256 && factor < (256 - prio));
  return prio = factor;
}

/* Spill pages from the dirty list back to disk.
 * This is intended to prevent running into MDBX_TXN_FULL situations,
 * but note that they may still occur in a few cases:
 *
 * 1) our estimate of the txn size could be too small. Currently this
 *  seems unlikely, except with a large number of MDBX_MULTIPLE items.
 *
 * 2) child txns may run out of space if their parents dirtied a
 *  lot of pages and never spilled them. TODO: we probably should do
 *  a preemptive spill during mdbx_txn_begin() of a child txn, if
 *  the parent's dirtyroom is below a given threshold.
 *
 * Otherwise, if not using nested txns, it is expected that apps will
 * not run into MDBX_TXN_FULL any more. The pages are flushed to disk
 * the same way as for a txn commit, e.g. their dirty status is cleared.
 * If the txn never references them again, they can be left alone.
 * If the txn only reads them, they can be used without any fuss.
 * If the txn writes them again, they can be dirtied immediately without
 * going thru all of the work of mdbx_page_touch(). Such references are
 * handled by mdbx_page_unspill().
 *
 * Also note, we never spill DB root pages, nor pages of active cursors,
 * because we'll need these back again soon anyway. And in nested txns,
 * we can't spill a page in a child txn if it was already spilled in a
 * parent txn. That would alter the parent txns' data even though
 * the child hasn't committed yet, and we'd have no way to undo it if
 * the child aborted. */
static int mdbx_txn_spill(MDBX_txn *const txn, MDBX_cursor *const m0,
                          const unsigned need) {
#if xMDBX_DEBUG_SPILLING != 1
  /* production mode */
  if (likely(txn->tw.dirtyroom + txn->tw.loose_count >= need))
    return MDBX_SUCCESS;
  unsigned wanna_spill = need - txn->tw.dirtyroom;
#else
  /* debug mode: spill at least one page if xMDBX_DEBUG_SPILLING == 1 */
  unsigned wanna_spill =
      (need > txn->tw.dirtyroom) ? need - txn->tw.dirtyroom : 1;
#endif /* xMDBX_DEBUG_SPILLING */

  const unsigned dirty = txn->tw.dirtylist->length;
  const unsigned spill_min =
      txn->mt_env->me_options.spill_min_denominator
          ? dirty / txn->mt_env->me_options.spill_min_denominator
          : 0;
  const unsigned spill_max =
      dirty - (txn->mt_env->me_options.spill_max_denominator
                   ? dirty / txn->mt_env->me_options.spill_max_denominator
                   : 0);
  wanna_spill = (wanna_spill > spill_min) ? wanna_spill : spill_min;
  wanna_spill = (wanna_spill < spill_max) ? wanna_spill : spill_max;
  if (!wanna_spill)
    return MDBX_SUCCESS;

  mdbx_notice("spilling %u dirty-entries (have %u dirty-room, need %u)",
              wanna_spill, txn->tw.dirtyroom, need);
  mdbx_tassert(txn, txn->tw.dirtylist->length >= wanna_spill);

  struct mdbx_iov_ctx ctx;
  mdbx_iov_init(txn, &ctx);
  int rc = MDBX_SUCCESS;
  if (txn->mt_flags & MDBX_WRITEMAP) {
    MDBX_dpl *const dl = txn->tw.dirtylist;
    const unsigned span = dl->length - txn->tw.loose_count;
    txn->tw.dirtyroom += span;
    unsigned r, w;
    for (w = 0, r = 1; r <= dl->length; ++r) {
      MDBX_page *dp = dl->items[r].ptr;
      if (dp->mp_flags & P_LOOSE)
        dl->items[++w] = dl->items[r];
      else if (!MDBX_FAKE_SPILL_WRITEMAP) {
        rc = iov_page(txn, &ctx, dp, dpl_npages(dl, r));
        mdbx_tassert(txn, rc == MDBX_SUCCESS);
      }
    }

    mdbx_tassert(txn, span == r - 1 - w && w == txn->tw.loose_count);
    dl->sorted = (dl->sorted == dl->length) ? w : 0;
    dpl_setlen(dl, w);
    mdbx_tassert(txn, mdbx_dirtylist_check(txn));

    if (!MDBX_FAKE_SPILL_WRITEMAP && ctx.flush_end > ctx.flush_begin) {
      MDBX_env *const env = txn->mt_env;
#if MDBX_ENABLE_PGOP_STAT
      env->me_lck->mti_pgop_stat.wops.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
      rc = mdbx_msync(&env->me_dxb_mmap,
                      pgno_align2os_bytes(env, ctx.flush_begin),
                      pgno_align2os_bytes(env, ctx.flush_end - ctx.flush_begin),
                      MDBX_SYNC_NONE);
    }
    return rc;
  }

  mdbx_tassert(txn, !(txn->mt_flags & MDBX_WRITEMAP));
  if (!txn->tw.spill_pages) {
    txn->tw.spill_least_removed = INT_MAX;
    txn->tw.spill_pages = mdbx_pnl_alloc(wanna_spill);
    if (unlikely(!txn->tw.spill_pages)) {
      rc = MDBX_ENOMEM;
    bailout:
      txn->mt_flags |= MDBX_TXN_ERROR;
      return rc;
    }
  } else {
    /* purge deleted slots */
    mdbx_spill_purge(txn);
    rc = mdbx_pnl_reserve(&txn->tw.spill_pages, wanna_spill);
    (void)rc /* ignore since the resulting list may be shorter
     and mdbx_pnl_append() will increase pnl on demand */
        ;
  }

  /* Сортируем чтобы запись на диск была полее последовательна */
  MDBX_dpl *const dl = mdbx_dpl_sort(txn);

  /* Preserve pages which may soon be dirtied again */
  const unsigned unspillable = mdbx_txn_keep(txn, m0);
  if (unspillable + txn->tw.loose_count >= dl->length) {
#if xMDBX_DEBUG_SPILLING == 1 /* avoid false failure in debug mode  */
    if (likely(txn->tw.dirtyroom + txn->tw.loose_count >= need))
      return MDBX_SUCCESS;
#endif /* xMDBX_DEBUG_SPILLING */
    mdbx_error("all %u dirty pages are unspillable  since referenced "
               "by a cursor(s), use fewer cursors or increase "
               "MDBX_opt_txn_dp_limit",
               unspillable);
    goto done;
  }

  /* Подзадача: Вытолкнуть часть страниц на диск в соответствии с LRU,
   * но при этом учесть важные поправки:
   *  - лучше выталкивать старые large/overflow страницы, так будет освобождено
   *    больше памяти, а также так как они (в текущем понимании) гораздо реже
   *    повторно изменяются;
   *  - при прочих равных лучше выталкивать смежные страницы, так будет
   *    меньше I/O операций;
   *  - желательно потратить на это меньше времени чем std::partial_sort_copy;
   *
   * Решение:
   *  - Квантуем весь диапазон lru-меток до 256 значений и задействуем один
   *    проход 8-битного radix-sort. В результате получаем 256 уровней
   *    "свежести", в том числе значение lru-метки, старее которой страницы
   *    должны быть выгружены;
   *  - Двигаемся последовательно в сторону увеличения номеров страниц
   *    и выталкиваем страницы с lru-меткой старее отсекающего значения,
   *    пока не вытолкнем достаточно;
   *  - Встречая страницы смежные с выталкиваемыми для уменьшения кол-ва
   *    I/O операций выталкиваем и их, если они попадают в первую половину
   *    между выталкиваемыми и самыми свежими lru-метками;
   *  - дополнительно при сортировке умышленно старим large/overflow страницы,
   *    тем самым повышая их шансы на выталкивание. */

  /* get min/max of LRU-labels */
  uint32_t age_max = 0;
  for (unsigned i = 1; i <= dl->length; ++i) {
    const uint32_t age = mdbx_dpl_age(txn, i);
    age_max = (age_max >= age) ? age_max : age;
  }

  mdbx_verbose("lru-head %u, age-max %u", txn->tw.dirtylru, age_max);

  /* half of 8-bit radix-sort */
  unsigned radix_counters[256], spillable = 0, spilled = 0;
  memset(&radix_counters, 0, sizeof(radix_counters));
  const uint32_t reciprocal = (UINT32_C(255) << 24) / (age_max + 1);
  for (unsigned i = 1; i <= dl->length; ++i) {
    unsigned prio = spill_prio(txn, i, reciprocal);
    if (prio < 256) {
      radix_counters[prio] += 1;
      spillable += 1;
    }
  }

  if (likely(spillable > 0)) {
    unsigned prio2spill = 0, prio2adjacent = 128, amount = radix_counters[0];
    for (unsigned i = 1; i < 256; i++) {
      if (amount < wanna_spill) {
        prio2spill = i;
        prio2adjacent = i + (257 - i) / 2;
        amount += radix_counters[i];
      } else if (amount + amount < spillable + wanna_spill
                 /* РАВНОЗНАЧНО: amount - wanna_spill < spillable - amount */) {
        prio2adjacent = i;
        amount += radix_counters[i];
      } else
        break;
    }

    mdbx_verbose("prio2spill %u, prio2adjacent %u, amount %u, spillable %u, "
                 "wanna_spill %u",
                 prio2spill, prio2adjacent, amount, spillable, wanna_spill);
    mdbx_tassert(txn, prio2spill < prio2adjacent && prio2adjacent <= 256);

    unsigned prev_prio = 256;
    unsigned r, w, prio;
    for (w = 0, r = 1; r <= dl->length && spilled < wanna_spill;
         prev_prio = prio, ++r) {
      prio = spill_prio(txn, r, reciprocal);
      MDBX_page *const dp = dl->items[r].ptr;
      if (prio < prio2adjacent) {
        const pgno_t pgno = dl->items[r].pgno;
        const unsigned npages = dpl_npages(dl, r);
        if (prio <= prio2spill) {
          if (prev_prio < prio2adjacent && prev_prio > prio2spill &&
              dpl_endpgno(dl, r - 1) == pgno) {
            mdbx_debug("co-spill %u prev-adjacent page %" PRIaPGNO
                       " (age %d, prio %u)",
                       dpl_npages(dl, w), dl->items[r - 1].pgno,
                       mdbx_dpl_age(txn, r - 1), prev_prio);
            --w;
            rc = spill_page(txn, &ctx, dl->items[r - 1].ptr,
                            dpl_npages(dl, r - 1));
            if (unlikely(rc != MDBX_SUCCESS))
              break;
            ++spilled;
          }

          mdbx_debug("spill %u page %" PRIaPGNO " (age %d, prio %u)", npages,
                     dp->mp_pgno, mdbx_dpl_age(txn, r), prio);
          rc = spill_page(txn, &ctx, dp, npages);
          if (unlikely(rc != MDBX_SUCCESS))
            break;
          ++spilled;
          continue;
        }

        if (prev_prio <= prio2spill && dpl_endpgno(dl, r - 1) == pgno) {
          mdbx_debug("co-spill %u next-adjacent page %" PRIaPGNO
                     " (age %d, prio %u)",
                     npages, dp->mp_pgno, mdbx_dpl_age(txn, r), prio);
          rc = spill_page(txn, &ctx, dp, npages);
          if (unlikely(rc != MDBX_SUCCESS))
            break;
          prio = prev_prio /* to continue co-spilling next adjacent pages */;
          ++spilled;
          continue;
        }
      }
      dl->items[++w] = dl->items[r];
    }

    mdbx_tassert(txn, spillable == 0 || spilled > 0);

    while (r <= dl->length)
      dl->items[++w] = dl->items[r++];
    mdbx_tassert(txn, r - 1 - w == spilled);

    dl->sorted = dpl_setlen(dl, w);
    txn->tw.dirtyroom += spilled;
    mdbx_tassert(txn, mdbx_dirtylist_check(txn));

    if (ctx.iov_items)
      rc = mdbx_iov_write(txn, &ctx);

    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;

    mdbx_pnl_sort(txn->tw.spill_pages, (size_t)txn->mt_next_pgno << 1);
    txn->mt_flags |= MDBX_TXN_SPILLS;
    mdbx_notice("spilled %u dirty-entries, now have %u dirty-room", spilled,
                txn->tw.dirtyroom);
    mdbx_iov_done(txn, &ctx);
  } else {
    mdbx_tassert(txn, ctx.iov_items == 0 && rc == MDBX_SUCCESS);
    for (unsigned i = 1; i <= dl->length; ++i) {
      MDBX_page *dp = dl->items[i].ptr;
      mdbx_notice(
          "dirtylist[%u]: pgno %u, npages %u, flags 0x%04X, age %u, prio %u", i,
          dp->mp_pgno, dpl_npages(dl, i), dp->mp_flags, mdbx_dpl_age(txn, i),
          spill_prio(txn, i, reciprocal));
    }
  }

#if xMDBX_DEBUG_SPILLING == 2
  if (txn->tw.loose_count + txn->tw.dirtyroom <= need / 2 + 1)
    mdbx_error("dirty-list length: before %u, after %u, parent %i, loose %u; "
               "needed %u, spillable %u; "
               "spilled %u dirty-entries, now have %u dirty-room",
               dl->length + spilled, dl->length,
               (txn->mt_parent && txn->mt_parent->tw.dirtylist)
                   ? (int)txn->mt_parent->tw.dirtylist->length
                   : -1,
               txn->tw.loose_count, need, spillable, spilled,
               txn->tw.dirtyroom);
  mdbx_ensure(txn->mt_env, txn->tw.loose_count + txn->tw.dirtyroom > need / 2);
#endif /* xMDBX_DEBUG_SPILLING */

done:
  return likely(txn->tw.dirtyroom + txn->tw.loose_count >
                ((need > CURSOR_STACK) ? CURSOR_STACK : need))
             ? MDBX_SUCCESS
             : MDBX_TXN_FULL;
}

static int mdbx_cursor_spill(MDBX_cursor *mc, const MDBX_val *key,
                             const MDBX_val *data) {
  MDBX_txn *txn = mc->mc_txn;
  /* Estimate how much space this operation will take: */
  /* 1) Max b-tree height, reasonable enough with including dups' sub-tree */
  unsigned need = CURSOR_STACK + 3;
  /* 2) GC/FreeDB for any payload */
  if (mc->mc_dbi > FREE_DBI) {
    need += txn->mt_dbs[FREE_DBI].md_depth + 3;
    /* 3) Named DBs also dirty the main DB */
    if (mc->mc_dbi > MAIN_DBI)
      need += txn->mt_dbs[MAIN_DBI].md_depth + 3;
  }
#if xMDBX_DEBUG_SPILLING != 2
  /* production mode */
  /* 4) Double the page chain estimation
   * for extensively splitting, rebalance and merging */
  need += need;
  /* 5) Factor the key+data which to be put in */
  need += bytes2pgno(txn->mt_env, node_size(key, data)) + 1;
#else
  /* debug mode */
  (void)key;
  (void)data;
  mc->mc_txn->mt_env->debug_dirtied_est = ++need;
  mc->mc_txn->mt_env->debug_dirtied_act = 0;
#endif /* xMDBX_DEBUG_SPILLING == 2 */

  return mdbx_txn_spill(txn, mc, need);
}

/*----------------------------------------------------------------------------*/

static __always_inline bool meta_bootid_match(const MDBX_meta *meta) {
  return memcmp(&meta->mm_bootid, &bootid, 16) == 0 &&
         (bootid.x | bootid.y) != 0;
}

static bool meta_weak_acceptable(const MDBX_env *env, const MDBX_meta *meta,
                                 const int lck_exclusive) {
  return lck_exclusive
             ? /* exclusive lock */ meta_bootid_match(meta)
             : /* db already opened */ env->me_lck_mmap.lck &&
                   (env->me_lck_mmap.lck->mti_envmode.weak & MDBX_RDONLY) == 0;
}

#define METAPAGE(env, n) page_meta(pgno2page(env, n))
#define METAPAGE_END(env) METAPAGE(env, NUM_METAS)

static __inline txnid_t meta_txnid(const MDBX_env *env, const MDBX_meta *meta,
                                   const bool allow_volatile) {
  mdbx_memory_fence(mo_AcquireRelease, false);
  txnid_t a = unaligned_peek_u64(4, &meta->mm_txnid_a);
  txnid_t b = unaligned_peek_u64(4, &meta->mm_txnid_b);
  if (allow_volatile)
    return (a == b) ? a : 0;
  mdbx_assert(env, a == b);
  (void)env;
  return a;
}

static __inline txnid_t mdbx_meta_txnid_stable(const MDBX_env *env,
                                               const MDBX_meta *meta) {
  return meta_txnid(env, meta, false);
}

static __inline txnid_t mdbx_meta_txnid_fluid(const MDBX_env *env,
                                              const MDBX_meta *meta) {
  return meta_txnid(env, meta, true);
}

static __inline void mdbx_meta_update_begin(const MDBX_env *env,
                                            MDBX_meta *meta, txnid_t txnid) {
  mdbx_assert(env, meta >= METAPAGE(env, 0) && meta < METAPAGE_END(env));
  mdbx_assert(env, unaligned_peek_u64(4, meta->mm_txnid_a) < txnid &&
                       unaligned_peek_u64(4, meta->mm_txnid_b) < txnid);
  (void)env;
  unaligned_poke_u64(4, meta->mm_txnid_b, 0);
  mdbx_memory_fence(mo_AcquireRelease, true);
  unaligned_poke_u64(4, meta->mm_txnid_a, txnid);
}

static __inline void mdbx_meta_update_end(const MDBX_env *env, MDBX_meta *meta,
                                          txnid_t txnid) {
  mdbx_assert(env, meta >= METAPAGE(env, 0) && meta < METAPAGE_END(env));
  mdbx_assert(env, unaligned_peek_u64(4, meta->mm_txnid_a) == txnid);
  mdbx_assert(env, unaligned_peek_u64(4, meta->mm_txnid_b) < txnid);
  (void)env;
  mdbx_jitter4testing(true);
  memcpy(&meta->mm_bootid, &bootid, 16);
  unaligned_poke_u64(4, meta->mm_txnid_b, txnid);
  mdbx_memory_fence(mo_AcquireRelease, true);
}

static __inline void mdbx_meta_set_txnid(const MDBX_env *env, MDBX_meta *meta,
                                         txnid_t txnid) {
  mdbx_assert(env, !env->me_map || meta < METAPAGE(env, 0) ||
                       meta >= METAPAGE_END(env));
  (void)env;
  /* update inconsistent since this function used ONLY for filling meta-image
   * for writing, but not the actual meta-page */
  memcpy(&meta->mm_bootid, &bootid, 16);
  unaligned_poke_u64(4, meta->mm_txnid_a, txnid);
  unaligned_poke_u64(4, meta->mm_txnid_b, txnid);
}

static __inline uint64_t mdbx_meta_sign(const MDBX_meta *meta) {
  uint64_t sign = MDBX_DATASIGN_NONE;
#if 0 /* TODO */
  sign = hippeus_hash64(...);
#else
  (void)meta;
#endif
  /* LY: newer returns MDBX_DATASIGN_NONE or MDBX_DATASIGN_WEAK */
  return (sign > MDBX_DATASIGN_WEAK) ? sign : ~sign;
}

enum meta_choise_mode { prefer_last, prefer_steady };

static __inline bool mdbx_meta_ot(const enum meta_choise_mode mode,
                                  const MDBX_env *env, const MDBX_meta *a,
                                  const MDBX_meta *b) {
  mdbx_jitter4testing(true);
  txnid_t txnid_a = mdbx_meta_txnid_fluid(env, a);
  txnid_t txnid_b = mdbx_meta_txnid_fluid(env, b);

  mdbx_jitter4testing(true);
  switch (mode) {
  default:
    assert(false);
    __unreachable();
    /* fall through */
    __fallthrough;
  case prefer_steady:
    if (META_IS_STEADY(a) != META_IS_STEADY(b))
      return META_IS_STEADY(b);
    /* fall through */
    __fallthrough;
  case prefer_last:
    mdbx_jitter4testing(true);
    if (txnid_a == txnid_b)
      return META_IS_STEADY(b);
    return txnid_a < txnid_b;
  }
}

static __inline bool mdbx_meta_eq(const MDBX_env *env, const MDBX_meta *a,
                                  const MDBX_meta *b) {
  mdbx_jitter4testing(true);
  const txnid_t txnid = mdbx_meta_txnid_fluid(env, a);
  if (!txnid || txnid != mdbx_meta_txnid_fluid(env, b))
    return false;

  mdbx_jitter4testing(true);
  if (META_IS_STEADY(a) != META_IS_STEADY(b))
    return false;

  mdbx_jitter4testing(true);
  return true;
}

static int mdbx_meta_eq_mask(const MDBX_env *env) {
  MDBX_meta *m0 = METAPAGE(env, 0);
  MDBX_meta *m1 = METAPAGE(env, 1);
  MDBX_meta *m2 = METAPAGE(env, 2);

  int rc = mdbx_meta_eq(env, m0, m1) ? 1 : 0;
  if (mdbx_meta_eq(env, m1, m2))
    rc += 2;
  if (mdbx_meta_eq(env, m2, m0))
    rc += 4;
  return rc;
}

static __inline MDBX_meta *mdbx_meta_recent(const enum meta_choise_mode mode,
                                            const MDBX_env *env, MDBX_meta *a,
                                            MDBX_meta *b) {
  const bool a_older_that_b = mdbx_meta_ot(mode, env, a, b);
  mdbx_assert(env, !mdbx_meta_eq(env, a, b));
  return a_older_that_b ? b : a;
}

static __inline MDBX_meta *mdbx_meta_ancient(const enum meta_choise_mode mode,
                                             const MDBX_env *env, MDBX_meta *a,
                                             MDBX_meta *b) {
  const bool a_older_that_b = mdbx_meta_ot(mode, env, a, b);
  mdbx_assert(env, !mdbx_meta_eq(env, a, b));
  return a_older_that_b ? a : b;
}

static __inline MDBX_meta *
mdbx_meta_mostrecent(const enum meta_choise_mode mode, const MDBX_env *env) {
  MDBX_meta *m0 = METAPAGE(env, 0);
  MDBX_meta *m1 = METAPAGE(env, 1);
  MDBX_meta *m2 = METAPAGE(env, 2);

  MDBX_meta *head = mdbx_meta_recent(mode, env, m0, m1);
  head = mdbx_meta_recent(mode, env, head, m2);
  return head;
}

static MDBX_meta *mdbx_meta_steady(const MDBX_env *env) {
  return mdbx_meta_mostrecent(prefer_steady, env);
}

static MDBX_meta *mdbx_meta_head(const MDBX_env *env) {
  return mdbx_meta_mostrecent(prefer_last, env);
}

static txnid_t mdbx_recent_committed_txnid(const MDBX_env *env) {
  while (true) {
    const MDBX_meta *head = mdbx_meta_head(env);
    const txnid_t recent = mdbx_meta_txnid_fluid(env, head);
    mdbx_compiler_barrier();
    if (likely(head == mdbx_meta_head(env) &&
               recent == mdbx_meta_txnid_fluid(env, head)))
      return recent;
  }
}

static txnid_t mdbx_recent_steady_txnid(const MDBX_env *env) {
  while (true) {
    const MDBX_meta *head = mdbx_meta_steady(env);
    const txnid_t recent = mdbx_meta_txnid_fluid(env, head);
    mdbx_compiler_barrier();
    if (likely(head == mdbx_meta_steady(env) &&
               recent == mdbx_meta_txnid_fluid(env, head)))
      return recent;
  }
}

static const char *mdbx_durable_str(const MDBX_meta *const meta) {
  if (META_IS_STEADY(meta))
    return (unaligned_peek_u64(4, meta->mm_datasync_sign) ==
            mdbx_meta_sign(meta))
               ? "Steady"
               : "Tainted";
  return "Weak";
}

/*----------------------------------------------------------------------------*/

/* Find oldest txnid still referenced. */
static txnid_t mdbx_find_oldest(const MDBX_txn *txn) {
  mdbx_tassert(txn, (txn->mt_flags & MDBX_TXN_RDONLY) == 0);
  MDBX_env *env = txn->mt_env;
  const txnid_t edge = mdbx_recent_steady_txnid(env);
  mdbx_tassert(txn, edge <= txn->mt_txnid);

  MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
  if (unlikely(lck == NULL /* exclusive mode */))
    return atomic_store64(&lck->mti_oldest_reader, edge, mo_Relaxed);

  const txnid_t last_oldest =
      atomic_load64(&lck->mti_oldest_reader, mo_AcquireRelease);
  mdbx_tassert(txn, edge >= last_oldest);
  if (likely(last_oldest == edge))
    return edge;

  const uint32_t nothing_changed = MDBX_STRING_TETRAD("None");
  const uint32_t snap_readers_refresh_flag =
      atomic_load32(&lck->mti_readers_refresh_flag, mo_AcquireRelease);
  mdbx_jitter4testing(false);
  if (snap_readers_refresh_flag == nothing_changed)
    return last_oldest;

  txnid_t oldest = edge;
  atomic_store32(&lck->mti_readers_refresh_flag, nothing_changed, mo_Relaxed);
  const unsigned snap_nreaders =
      atomic_load32(&lck->mti_numreaders, mo_AcquireRelease);
  for (unsigned i = 0; i < snap_nreaders; ++i) {
    if (atomic_load32(&lck->mti_readers[i].mr_pid, mo_AcquireRelease)) {
      /* mdbx_jitter4testing(true); */
      const txnid_t snap = safe64_read(&lck->mti_readers[i].mr_txnid);
      if (oldest > snap && last_oldest <= /* ignore pending updates */ snap) {
        oldest = snap;
        if (oldest == last_oldest)
          return oldest;
      }
    }
  }

  if (oldest != last_oldest) {
    mdbx_notice("update oldest %" PRIaTXN " -> %" PRIaTXN, last_oldest, oldest);
    mdbx_tassert(txn, oldest >= lck->mti_oldest_reader.weak);
    atomic_store64(&lck->mti_oldest_reader, oldest, mo_Relaxed);
  }
  return oldest;
}

/* Find largest mvcc-snapshot still referenced. */
__cold static pgno_t mdbx_find_largest(MDBX_env *env, pgno_t largest) {
  MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
  if (likely(lck != NULL /* exclusive mode */)) {
    const unsigned snap_nreaders =
        atomic_load32(&lck->mti_numreaders, mo_AcquireRelease);
    for (unsigned i = 0; i < snap_nreaders; ++i) {
    retry:
      if (atomic_load32(&lck->mti_readers[i].mr_pid, mo_AcquireRelease)) {
        /* mdbx_jitter4testing(true); */
        const pgno_t snap_pages = atomic_load32(
            &lck->mti_readers[i].mr_snapshot_pages_used, mo_Relaxed);
        const txnid_t snap_txnid = safe64_read(&lck->mti_readers[i].mr_txnid);
        if (unlikely(
                snap_pages !=
                    atomic_load32(&lck->mti_readers[i].mr_snapshot_pages_used,
                                  mo_AcquireRelease) ||
                snap_txnid != safe64_read(&lck->mti_readers[i].mr_txnid)))
          goto retry;
        if (largest < snap_pages &&
            atomic_load64(&lck->mti_oldest_reader, mo_AcquireRelease) <=
                /* ignore pending updates */ snap_txnid &&
            snap_txnid <= env->me_txn0->mt_txnid)
          largest = snap_pages;
      }
    }
  }

  return largest;
}

/* Add a page to the txn's dirty list */
static int __must_check_result mdbx_page_dirty(MDBX_txn *txn, MDBX_page *mp,
                                               unsigned npages) {
#if xMDBX_DEBUG_SPILLING == 2
  txn->mt_env->debug_dirtied_act += 1;
  mdbx_ensure(txn->mt_env,
              txn->mt_env->debug_dirtied_act < txn->mt_env->debug_dirtied_est);
  mdbx_ensure(txn->mt_env, txn->tw.dirtyroom + txn->tw.loose_count > 0);
#endif /* xMDBX_DEBUG_SPILLING == 2 */

  int rc;
  mp->mp_txnid = txn->mt_front;
  if (unlikely(txn->tw.dirtyroom == 0)) {
    if (txn->tw.loose_count) {
      MDBX_page *loose = txn->tw.loose_pages;
      mdbx_debug("purge-and-reclaim loose page %" PRIaPGNO, loose->mp_pgno);
      rc = mdbx_pnl_insert_range(&txn->tw.reclaimed_pglist, loose->mp_pgno, 1);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
      unsigned di = mdbx_dpl_search(txn, loose->mp_pgno);
      mdbx_tassert(txn, txn->tw.dirtylist->items[di].ptr == loose);
      mdbx_dpl_remove(txn, di);
      txn->tw.loose_pages = loose->mp_next;
      txn->tw.loose_count--;
      txn->tw.dirtyroom++;
      if (!(txn->mt_flags & MDBX_WRITEMAP))
        mdbx_dpage_free(txn->mt_env, loose, 1);
    } else {
      mdbx_error("Dirtyroom is depleted, DPL length %u",
                 txn->tw.dirtylist->length);
      if (!(txn->mt_flags & MDBX_WRITEMAP))
        mdbx_dpage_free(txn->mt_env, mp, npages);
      return MDBX_TXN_FULL;
    }
  }

  rc = mdbx_dpl_append(txn, mp->mp_pgno, mp, npages);
  if (unlikely(rc != MDBX_SUCCESS)) {
  bailout:
    txn->mt_flags |= MDBX_TXN_ERROR;
    return rc;
  }
  txn->tw.dirtyroom--;
  mdbx_tassert(txn, mdbx_dirtylist_check(txn));
  return MDBX_SUCCESS;
}

#if !(defined(_WIN32) || defined(_WIN64))
MDBX_MAYBE_UNUSED static __always_inline int ignore_enosys(int err) {
#ifdef ENOSYS
  if (err == ENOSYS)
    return MDBX_RESULT_TRUE;
#endif /* ENOSYS */
#ifdef ENOIMPL
  if (err == ENOIMPL)
    return MDBX_RESULT_TRUE;
#endif /* ENOIMPL */
#ifdef ENOTSUP
  if (err == ENOTSUP)
    return MDBX_RESULT_TRUE;
#endif /* ENOTSUP */
#ifdef ENOSUPP
  if (err == ENOSUPP)
    return MDBX_RESULT_TRUE;
#endif /* ENOSUPP */
#ifdef EOPNOTSUPP
  if (err == EOPNOTSUPP)
    return MDBX_RESULT_TRUE;
#endif /* EOPNOTSUPP */
  if (err == EAGAIN)
    return MDBX_RESULT_TRUE;
  return err;
}
#endif /* defined(_WIN32) || defined(_WIN64) */

#if MDBX_ENABLE_MADVISE
/* Turn on/off readahead. It's harmful when the DB is larger than RAM. */
__cold static int mdbx_set_readahead(MDBX_env *env, const pgno_t edge,
                                     const bool enable,
                                     const bool force_whole) {
  mdbx_assert(env, edge >= NUM_METAS && edge <= MAX_PAGENO);
  mdbx_assert(env, (enable & 1) == (enable != 0));
  const bool toggle = force_whole ||
                      ((enable ^ env->me_lck->mti_readahead_anchor) & 1) ||
                      !env->me_lck->mti_readahead_anchor;
  const pgno_t prev_edge = env->me_lck->mti_readahead_anchor >> 1;
  const size_t limit = env->me_dxb_mmap.limit;
  size_t offset =
      toggle ? 0
             : pgno_align2os_bytes(env, (prev_edge < edge) ? prev_edge : edge);
  offset = (offset < limit) ? offset : limit;

  size_t length =
      pgno_align2os_bytes(env, (prev_edge < edge) ? edge : prev_edge);
  length = (length < limit) ? length : limit;
  length -= offset;

  mdbx_assert(env, 0 <= (intptr_t)length);
  if (length == 0)
    return MDBX_SUCCESS;

  mdbx_notice("readahead %s %u..%u", enable ? "ON" : "OFF",
              bytes2pgno(env, offset), bytes2pgno(env, offset + length));

#if defined(F_RDAHEAD)
  if (toggle && unlikely(fcntl(env->me_lazy_fd, F_RDAHEAD, enable) == -1))
    return errno;
#endif /* F_RDAHEAD */

  int err;
  if (enable) {
#if defined(MADV_NORMAL)
    err = madvise(env->me_map + offset, length, MADV_NORMAL)
              ? ignore_enosys(errno)
              : MDBX_SUCCESS;
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(POSIX_MADV_NORMAL)
    err = ignore_enosys(
        posix_madvise(env->me_map + offset, length, POSIX_MADV_NORMAL));
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(POSIX_FADV_NORMAL) && defined(POSIX_FADV_WILLNEED)
    err = ignore_enosys(
        posix_fadvise(env->me_lazy_fd, offset, length, POSIX_FADV_NORMAL));
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(_WIN32) || defined(_WIN64)
    /* no madvise on Windows */
#else
#warning "FIXME"
#endif
    if (toggle) {
      /* NOTE: Seems there is a bug in the Mach/Darwin/OSX kernel,
       * because MADV_WILLNEED with offset != 0 may cause SIGBUS
       * on following access to the hinted region.
       * 19.6.0 Darwin Kernel Version 19.6.0: Tue Jan 12 22:13:05 PST 2021;
       * root:xnu-6153.141.16~1/RELEASE_X86_64 x86_64 */
#if defined(F_RDADVISE)
      struct radvisory hint;
      hint.ra_offset = offset;
      hint.ra_count = length;
      (void)/* Ignore ENOTTY for DB on the ram-disk and so on */ fcntl(
          env->me_lazy_fd, F_RDADVISE, &hint);
#elif defined(MADV_WILLNEED)
      err = madvise(env->me_map + offset, length, MADV_WILLNEED)
                ? ignore_enosys(errno)
                : MDBX_SUCCESS;
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
#elif defined(POSIX_MADV_WILLNEED)
      err = ignore_enosys(
          posix_madvise(env->me_map + offset, length, POSIX_MADV_WILLNEED));
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
#elif defined(_WIN32) || defined(_WIN64)
      if (mdbx_PrefetchVirtualMemory) {
        WIN32_MEMORY_RANGE_ENTRY hint;
        hint.VirtualAddress = env->me_map + offset;
        hint.NumberOfBytes = length;
        (void)mdbx_PrefetchVirtualMemory(GetCurrentProcess(), 1, &hint, 0);
      }
#elif defined(POSIX_FADV_WILLNEED)
      err = ignore_enosys(
          posix_fadvise(env->me_lazy_fd, offset, length, POSIX_FADV_WILLNEED));
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
#else
#warning "FIXME"
#endif
    }
  } else {
#if defined(MADV_RANDOM)
    err = madvise(env->me_map + offset, length, MADV_RANDOM)
              ? ignore_enosys(errno)
              : MDBX_SUCCESS;
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(POSIX_MADV_RANDOM)
    err = ignore_enosys(
        posix_madvise(env->me_map + offset, length, POSIX_MADV_RANDOM));
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(POSIX_FADV_RANDOM)
    err = ignore_enosys(
        posix_fadvise(env->me_lazy_fd, offset, length, POSIX_FADV_RANDOM));
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(_WIN32) || defined(_WIN64)
    /* no madvise on Windows */
#else
#warning "FIXME"
#endif /* MADV_RANDOM */
  }

  env->me_lck->mti_readahead_anchor = (enable & 1) + (edge << 1);
  err = MDBX_SUCCESS;
  return err;
}
#endif /* MDBX_ENABLE_MADVISE */

__cold static int mdbx_mapresize(MDBX_env *env, const pgno_t used_pgno,
                                 const pgno_t size_pgno,
                                 const pgno_t limit_pgno, const bool implicit) {
  const size_t limit_bytes = pgno_align2os_bytes(env, limit_pgno);
  const size_t size_bytes = pgno_align2os_bytes(env, size_pgno);
  const size_t prev_size = env->me_dxb_mmap.current;
  const size_t prev_limit = env->me_dxb_mmap.limit;
#if MDBX_ENABLE_MADVISE || defined(MDBX_USE_VALGRIND)
  const void *const prev_addr = env->me_map;
#endif /* MDBX_ENABLE_MADVISE || MDBX_USE_VALGRIND */

  mdbx_verbose("resize datafile/mapping: "
               "present %" PRIuPTR " -> %" PRIuPTR ", "
               "limit %" PRIuPTR " -> %" PRIuPTR,
               prev_size, size_bytes, prev_limit, limit_bytes);

  mdbx_assert(env, limit_bytes >= size_bytes);
  mdbx_assert(env, bytes2pgno(env, size_bytes) >= size_pgno);
  mdbx_assert(env, bytes2pgno(env, limit_bytes) >= limit_pgno);

  unsigned mresize_flags =
      env->me_flags & (MDBX_RDONLY | MDBX_WRITEMAP | MDBX_UTTERLY_NOSYNC);
#if defined(_WIN32) || defined(_WIN64)
  /* Acquire guard in exclusive mode for:
   *   - to avoid collision between read and write txns around env->me_dbgeo;
   *   - to avoid attachment of new reading threads (see mdbx_rdt_lock); */
  mdbx_srwlock_AcquireExclusive(&env->me_remap_guard);
  mdbx_handle_array_t *suspended = NULL;
  mdbx_handle_array_t array_onstack;
  int rc = MDBX_SUCCESS;
  if (limit_bytes == env->me_dxb_mmap.limit &&
      size_bytes == env->me_dxb_mmap.current &&
      size_bytes == env->me_dxb_mmap.filesize)
    goto bailout;

  if ((env->me_flags & MDBX_NOTLS) == 0) {
    /* 1) Windows allows only extending a read-write section, but not a
     *    corresponding mapped view. Therefore in other cases we must suspend
     *    the local threads for safe remap.
     * 2) At least on Windows 10 1803 the entire mapped section is unavailable
     *    for short time during NtExtendSection() or VirtualAlloc() execution.
     * 3) Under Wine runtime environment on Linux a section extending is not
     *    supported.
     *
     * THEREFORE LOCAL THREADS SUSPENDING IS ALWAYS REQUIRED! */
    array_onstack.limit = ARRAY_LENGTH(array_onstack.handles);
    array_onstack.count = 0;
    suspended = &array_onstack;
    rc = mdbx_suspend_threads_before_remap(env, &suspended);
    if (rc != MDBX_SUCCESS) {
      mdbx_error("failed suspend-for-remap: errcode %d", rc);
      goto bailout;
    }
    mresize_flags |= implicit ? MDBX_MRESIZE_MAY_UNMAP
                              : MDBX_MRESIZE_MAY_UNMAP | MDBX_MRESIZE_MAY_MOVE;
  }
#else  /* Windows */
  /* Acquire guard to avoid collision between read and write txns
   * around env->me_dbgeo */
  int rc = mdbx_fastmutex_acquire(&env->me_remap_guard);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  if (limit_bytes == env->me_dxb_mmap.limit &&
      size_bytes == env->me_dxb_mmap.current)
    goto bailout;

  MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
  if (limit_bytes != env->me_dxb_mmap.limit && !(env->me_flags & MDBX_NOTLS) &&
      lck && !implicit) {
    int err = mdbx_rdt_lock(env) /* lock readers table until remap done */;
    if (unlikely(MDBX_IS_ERROR(err))) {
      rc = err;
      goto bailout;
    }

    /* looking for readers from this process */
    const unsigned snap_nreaders =
        atomic_load32(&lck->mti_numreaders, mo_AcquireRelease);
    mdbx_assert(env, !implicit);
    mresize_flags |= MDBX_MRESIZE_MAY_UNMAP | MDBX_MRESIZE_MAY_MOVE;
    for (unsigned i = 0; i < snap_nreaders; ++i) {
      if (lck->mti_readers[i].mr_pid.weak == env->me_pid &&
          lck->mti_readers[i].mr_tid.weak != mdbx_thread_self()) {
        /* the base address of the mapping can't be changed since
         * the other reader thread from this process exists. */
        mdbx_rdt_unlock(env);
        mresize_flags &= ~(MDBX_MRESIZE_MAY_UNMAP | MDBX_MRESIZE_MAY_MOVE);
        break;
      }
    }
  }
#endif /* ! Windows */

  if ((env->me_flags & MDBX_WRITEMAP) && env->me_lck->mti_unsynced_pages.weak) {
#if MDBX_ENABLE_PGOP_STAT
    env->me_lck->mti_pgop_stat.wops.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
    rc = mdbx_msync(&env->me_dxb_mmap, 0, pgno_align2os_bytes(env, used_pgno),
                    MDBX_SYNC_NONE);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;
  }

#if MDBX_ENABLE_MADVISE
  if (size_bytes < prev_size) {
    mdbx_notice("resize-MADV_%s %u..%u",
                (env->me_flags & MDBX_WRITEMAP) ? "REMOVE" : "DONTNEED",
                size_pgno, bytes2pgno(env, prev_size));
    rc = MDBX_RESULT_TRUE;
#if defined(MADV_REMOVE)
    if (env->me_flags & MDBX_WRITEMAP)
      rc =
          madvise(env->me_map + size_bytes, prev_size - size_bytes, MADV_REMOVE)
              ? ignore_enosys(errno)
              : MDBX_SUCCESS;
#endif /* MADV_REMOVE */
#if defined(MADV_DONTNEED)
    if (rc == MDBX_RESULT_TRUE)
      rc = madvise(env->me_map + size_bytes, prev_size - size_bytes,
                   MADV_DONTNEED)
               ? ignore_enosys(errno)
               : MDBX_SUCCESS;
#elif defined(POSIX_MADV_DONTNEED)
    if (rc == MDBX_RESULT_TRUE)
      rc = ignore_enosys(posix_madvise(env->me_map + size_bytes,
                                       prev_size - size_bytes,
                                       POSIX_MADV_DONTNEED));
#elif defined(POSIX_FADV_DONTNEED)
    if (rc == MDBX_RESULT_TRUE)
      rc = ignore_enosys(posix_fadvise(env->me_lazy_fd, size_bytes,
                                       prev_size - size_bytes,
                                       POSIX_FADV_DONTNEED));
#endif /* MADV_DONTNEED */
    if (unlikely(MDBX_IS_ERROR(rc)))
      goto bailout;
    if (env->me_lck->mti_discarded_tail.weak > size_pgno)
      env->me_lck->mti_discarded_tail.weak = size_pgno;
  }
#endif /* MDBX_ENABLE_MADVISE */

  rc = mdbx_mresize(mresize_flags, &env->me_dxb_mmap, size_bytes, limit_bytes);

#if MDBX_ENABLE_MADVISE
  if (rc == MDBX_SUCCESS) {
    env->me_lck->mti_discarded_tail.weak = size_pgno;
    const bool readahead =
        !(env->me_flags & MDBX_NORDAHEAD) &&
        mdbx_is_readahead_reasonable(size_bytes, -(intptr_t)prev_size);
    const bool force = limit_bytes != prev_limit ||
                       env->me_dxb_mmap.address != prev_addr
#if defined(_WIN32) || defined(_WIN64)
                       || prev_size > size_bytes
#endif /* Windows */
        ;
    rc = mdbx_set_readahead(env, size_pgno, readahead, force);
  }
#endif /* MDBX_ENABLE_MADVISE */

bailout:
  if (rc == MDBX_SUCCESS) {
    mdbx_assert(env, size_bytes == env->me_dxb_mmap.current);
    mdbx_assert(env, size_bytes <= env->me_dxb_mmap.filesize);
    mdbx_assert(env, limit_bytes == env->me_dxb_mmap.limit);
#ifdef MDBX_USE_VALGRIND
    if (prev_limit != env->me_dxb_mmap.limit || prev_addr != env->me_map) {
      VALGRIND_DISCARD(env->me_valgrind_handle);
      env->me_valgrind_handle = 0;
      if (env->me_dxb_mmap.limit)
        env->me_valgrind_handle =
            VALGRIND_CREATE_BLOCK(env->me_map, env->me_dxb_mmap.limit, "mdbx");
    }
#endif /* MDBX_USE_VALGRIND */
  } else {
    if (rc != MDBX_UNABLE_EXTEND_MAPSIZE && rc != MDBX_RESULT_TRUE) {
      mdbx_error("failed resize datafile/mapping: "
                 "present %" PRIuPTR " -> %" PRIuPTR ", "
                 "limit %" PRIuPTR " -> %" PRIuPTR ", errcode %d",
                 prev_size, size_bytes, prev_limit, limit_bytes, rc);
    } else {
      mdbx_warning("unable resize datafile/mapping: "
                   "present %" PRIuPTR " -> %" PRIuPTR ", "
                   "limit %" PRIuPTR " -> %" PRIuPTR ", errcode %d",
                   prev_size, size_bytes, prev_limit, limit_bytes, rc);
    }
    if (!env->me_dxb_mmap.address) {
      env->me_flags |= MDBX_FATAL_ERROR;
      if (env->me_txn)
        env->me_txn->mt_flags |= MDBX_TXN_ERROR;
      rc = MDBX_PANIC;
    }
  }

#if defined(_WIN32) || defined(_WIN64)
  int err = MDBX_SUCCESS;
  mdbx_srwlock_ReleaseExclusive(&env->me_remap_guard);
  if (suspended) {
    err = mdbx_resume_threads_after_remap(suspended);
    if (suspended != &array_onstack)
      mdbx_free(suspended);
  }
#else
  if (env->me_lck_mmap.lck &&
      (mresize_flags & (MDBX_MRESIZE_MAY_UNMAP | MDBX_MRESIZE_MAY_MOVE)) != 0)
    mdbx_rdt_unlock(env);
  int err = mdbx_fastmutex_release(&env->me_remap_guard);
#endif /* Windows */
  if (err != MDBX_SUCCESS) {
    mdbx_fatal("failed resume-after-remap: errcode %d", err);
    return MDBX_PANIC;
  }
  return rc;
}

__cold static int mdbx_mapresize_implicit(MDBX_env *env, const pgno_t used_pgno,
                                          const pgno_t size_pgno,
                                          const pgno_t limit_pgno) {
  const pgno_t mapped_pgno = bytes2pgno(env, env->me_dxb_mmap.limit);
  mdbx_assert(env, mapped_pgno >= used_pgno);
  return mdbx_mapresize(
      env, used_pgno, size_pgno,
      (size_pgno > mapped_pgno)
          ? limit_pgno
          : /* The actual mapsize may be less since the geo.upper may be changed
               by other process. So, avoids remapping until it necessary. */
          mapped_pgno,
      true);
}

static int mdbx_meta_unsteady(MDBX_env *env, const txnid_t last_steady,
                              MDBX_meta *const meta, mdbx_filehandle_t fd) {
  const uint64_t wipe = MDBX_DATASIGN_NONE;
  if (unlikely(META_IS_STEADY(meta)) &&
      mdbx_meta_txnid_stable(env, meta) <= last_steady) {
    mdbx_warning("wipe txn #%" PRIaTXN ", meta %" PRIaPGNO, last_steady,
                 data_page(meta)->mp_pgno);
    if (env->me_flags & MDBX_WRITEMAP)
      unaligned_poke_u64(4, meta->mm_datasync_sign, wipe);
    else
      return mdbx_pwrite(fd, &wipe, sizeof(meta->mm_datasync_sign),
                         (uint8_t *)&meta->mm_datasync_sign - env->me_map);
  }
  return MDBX_SUCCESS;
}

__cold static int mdbx_wipe_steady(MDBX_env *env, const txnid_t last_steady) {
#if MDBX_ENABLE_PGOP_STAT
  env->me_lck->mti_pgop_stat.wops.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
  const mdbx_filehandle_t fd = (env->me_dsync_fd != INVALID_HANDLE_VALUE)
                                   ? env->me_dsync_fd
                                   : env->me_lazy_fd;
  int err = mdbx_meta_unsteady(env, last_steady, METAPAGE(env, 0), fd);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  err = mdbx_meta_unsteady(env, last_steady, METAPAGE(env, 1), fd);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  err = mdbx_meta_unsteady(env, last_steady, METAPAGE(env, 2), fd);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  if (env->me_flags & MDBX_WRITEMAP) {
    mdbx_flush_incoherent_cpu_writeback();
    err = mdbx_msync(&env->me_dxb_mmap, 0, pgno_align2os_bytes(env, NUM_METAS),
                     MDBX_SYNC_DATA);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  } else {
    if (fd == env->me_lazy_fd) {
#if MDBX_USE_SYNCFILERANGE
      static bool syncfilerange_unavailable;
      if (!syncfilerange_unavailable &&
          sync_file_range(env->me_lazy_fd, 0, pgno2bytes(env, NUM_METAS),
                          SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER)) {
        err = errno;
        if (ignore_enosys(err) == MDBX_RESULT_TRUE)
          syncfilerange_unavailable = true;
      }
      if (syncfilerange_unavailable)
#endif /* MDBX_USE_SYNCFILERANGE */
        err = mdbx_fsync(env->me_lazy_fd, MDBX_SYNC_DATA);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
    mdbx_flush_incoherent_mmap(env->me_map, pgno2bytes(env, NUM_METAS),
                               env->me_os_psize);
  }

  /* force oldest refresh */
  atomic_store32(&env->me_lck->mti_readers_refresh_flag, true, mo_Relaxed);
  return MDBX_SUCCESS;
}

/* Allocate page numbers and memory for writing.  Maintain mt_last_reclaimed,
 * mt_reclaimed_pglist and mt_next_pgno.  Set MDBX_TXN_ERROR on failure.
 *
 * If there are free pages available from older transactions, they
 * are re-used first. Otherwise allocate a new page at mt_next_pgno.
 * Do not modify the GC, just merge GC records into mt_reclaimed_pglist
 * and move mt_last_reclaimed to say which records were consumed.  Only this
 * function can create mt_reclaimed_pglist and move
 * mt_last_reclaimed/mt_next_pgno.
 *
 * [in] mc    cursor A cursor handle identifying the transaction and
 *            database for which we are allocating.
 * [in] num   the number of pages to allocate.
 *
 * Returns 0 on success, non-zero on failure.*/

#define MDBX_ALLOC_CACHE 1
#define MDBX_ALLOC_GC 2
#define MDBX_ALLOC_NEW 4
#define MDBX_ALLOC_SLOT 8
#define MDBX_ALLOC_ALL (MDBX_ALLOC_CACHE | MDBX_ALLOC_GC | MDBX_ALLOC_NEW)

__hot static struct page_result mdbx_page_alloc(MDBX_cursor *mc,
                                                const unsigned num, int flags) {
  struct page_result ret;
  MDBX_txn *const txn = mc->mc_txn;
  MDBX_env *const env = txn->mt_env;

  const unsigned coalesce_threshold =
      env->me_maxgc_ov1page - env->me_maxgc_ov1page / 4;
  if (likely(flags & MDBX_ALLOC_GC)) {
    flags |= env->me_flags & (MDBX_COALESCE | MDBX_LIFORECLAIM);
    if (MDBX_PNL_SIZE(txn->tw.reclaimed_pglist) > coalesce_threshold)
      flags &= ~MDBX_COALESCE;
    if (unlikely(
            /* If mc is updating the GC, then the retired-list cannot play
               catch-up with itself by growing while trying to save it. */
            (mc->mc_flags & C_RECLAIMING) ||
            /* avoid (recursive) search inside empty tree and while tree is
               updating, https://github.com/erthink/libmdbx/issues/31 */
            txn->mt_dbs[FREE_DBI].md_entries == 0 ||
            /* If our dirty list is already full, we can't touch GC */
            (txn->tw.dirtyroom < txn->mt_dbs[FREE_DBI].md_depth &&
             !(txn->mt_dbistate[FREE_DBI] & DBI_DIRTY))))
      flags &= ~(MDBX_ALLOC_GC | MDBX_COALESCE);
  }

  if (likely(num == 1 && (flags & MDBX_ALLOC_CACHE) != 0)) {
    /* If there are any loose pages, just use them */
    mdbx_assert(env, (flags & MDBX_ALLOC_SLOT) == 0);
    if (likely(txn->tw.loose_pages)) {
#if MDBX_ENABLE_REFUND
      if (txn->tw.loose_refund_wl > txn->mt_next_pgno) {
        mdbx_refund(txn);
        if (unlikely(!txn->tw.loose_pages))
          goto no_loose;
      }
#endif /* MDBX_ENABLE_REFUND */

      ret.page = txn->tw.loose_pages;
      txn->tw.loose_pages = ret.page->mp_next;
      txn->tw.loose_count--;
      mdbx_debug_extra("db %d use loose page %" PRIaPGNO, DDBI(mc),
                       ret.page->mp_pgno);
      mdbx_tassert(txn, ret.page->mp_pgno < txn->mt_next_pgno);
      mdbx_ensure(env, ret.page->mp_pgno >= NUM_METAS);
      VALGRIND_MAKE_MEM_UNDEFINED(page_data(ret.page), page_space(txn->mt_env));
      MDBX_ASAN_UNPOISON_MEMORY_REGION(page_data(ret.page),
                                       page_space(txn->mt_env));
      ret.page->mp_txnid = txn->mt_front;
      ret.err = MDBX_SUCCESS;
      return ret;
    }
  }
#if MDBX_ENABLE_REFUND
no_loose:
#endif /* MDBX_ENABLE_REFUND */

  mdbx_tassert(txn,
               mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                     txn->mt_next_pgno - MDBX_ENABLE_REFUND));
  pgno_t pgno, *re_list = txn->tw.reclaimed_pglist;
  unsigned range_begin = 0, re_len = MDBX_PNL_SIZE(re_list);
  txnid_t oldest = 0, last = 0;

  while (true) { /* hsr-kick retry loop */
    MDBX_cursor_couple recur;
    for (MDBX_cursor_op op = MDBX_FIRST;;
         op = (flags & MDBX_LIFORECLAIM) ? MDBX_PREV : MDBX_NEXT) {
      MDBX_val key, data;

      /* Seek a big enough contiguous page range.
       * Prefer pages with lower pgno. */
      mdbx_tassert(txn, mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                              txn->mt_next_pgno));
      if ((flags & (MDBX_COALESCE | MDBX_ALLOC_CACHE)) == MDBX_ALLOC_CACHE &&
          re_len >= num) {
        mdbx_tassert(txn, MDBX_PNL_LAST(re_list) < txn->mt_next_pgno &&
                              MDBX_PNL_FIRST(re_list) < txn->mt_next_pgno);
        range_begin = MDBX_PNL_ASCENDING ? 1 : re_len;
        pgno = MDBX_PNL_LEAST(re_list);
        if (likely(num == 1))
          goto done;

        const unsigned wanna_range = num - 1;
#if MDBX_PNL_ASCENDING
        mdbx_tassert(txn, pgno == re_list[1] && range_begin == 1);
        while (true) {
          unsigned range_end = range_begin + wanna_range;
          if (re_list[range_end] - pgno == wanna_range)
            goto done;
          if (range_end == re_len)
            break;
          pgno = re_list[++range_begin];
        }
#else
        mdbx_tassert(txn, pgno == re_list[re_len] && range_begin == re_len);
        while (true) {
          if (re_list[range_begin - wanna_range] - pgno == wanna_range)
            goto done;
          if (range_begin == wanna_range)
            break;
          pgno = re_list[--range_begin];
        }
#endif /* MDBX_PNL sort-order */
      }

      if (op == MDBX_FIRST) { /* 1st iteration, setup cursor, etc */
        if (unlikely(!(flags & MDBX_ALLOC_GC)))
          break /* reclaiming is prohibited for now */;

        /* Prepare to fetch more and coalesce */
        oldest = (flags & MDBX_LIFORECLAIM)
                     ? mdbx_find_oldest(txn)
                     : atomic_load64(&env->me_lck->mti_oldest_reader,
                                     mo_AcquireRelease);
        ret.err = mdbx_cursor_init(&recur.outer, txn, FREE_DBI);
        if (unlikely(ret.err != MDBX_SUCCESS))
          goto fail;
        if (flags & MDBX_LIFORECLAIM) {
          /* Begin from oldest reader if any */
          if (oldest > MIN_TXNID) {
            last = oldest - 1;
            op = MDBX_SET_RANGE;
          }
        } else if (txn->tw.last_reclaimed) {
          /* Continue lookup from txn->tw.last_reclaimed to oldest reader */
          last = txn->tw.last_reclaimed;
          op = MDBX_SET_RANGE;
        }

        key.iov_base = &last;
        key.iov_len = sizeof(last);
      }

      if (!(flags & MDBX_LIFORECLAIM)) {
        /* Do not try fetch more if the record will be too recent */
        if (op != MDBX_FIRST && ++last >= oldest) {
          oldest = mdbx_find_oldest(txn);
          if (oldest <= last)
            break;
        }
      }

      ret.err = mdbx_cursor_get(&recur.outer, &key, NULL, op);
      if (ret.err == MDBX_NOTFOUND && (flags & MDBX_LIFORECLAIM)) {
        if (op == MDBX_SET_RANGE)
          continue;
        txnid_t snap = mdbx_find_oldest(txn);
        if (oldest < snap) {
          oldest = snap;
          last = oldest - 1;
          key.iov_base = &last;
          key.iov_len = sizeof(last);
          op = MDBX_SET_RANGE;
          ret.err = mdbx_cursor_get(&recur.outer, &key, NULL, op);
        }
      }
      if (unlikely(ret.err)) {
        if (ret.err == MDBX_NOTFOUND)
          break;
        goto fail;
      }

      if (!MDBX_DISABLE_PAGECHECKS &&
          unlikely(key.iov_len != sizeof(txnid_t))) {
        ret.err = MDBX_CORRUPTED;
        goto fail;
      }
      last = unaligned_peek_u64(4, key.iov_base);
      if (!MDBX_DISABLE_PAGECHECKS &&
          unlikely(last < MIN_TXNID || last > MAX_TXNID)) {
        ret.err = MDBX_CORRUPTED;
        goto fail;
      }
      if (oldest <= last) {
        oldest = mdbx_find_oldest(txn);
        if (oldest <= last) {
          if (flags & MDBX_LIFORECLAIM)
            continue;
          break;
        }
      }

      if (flags & MDBX_LIFORECLAIM) {
        /* skip IDs of records that already reclaimed */
        if (txn->tw.lifo_reclaimed) {
          size_t i;
          for (i = (size_t)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed); i > 0; --i)
            if (txn->tw.lifo_reclaimed[i] == last)
              break;
          if (i)
            continue;
        }
      }

      /* Reading next GC record */
      MDBX_page *const mp = recur.outer.mc_pg[recur.outer.mc_top];
      if (unlikely((ret.err = mdbx_node_read(
                        &recur.outer,
                        page_node(mp, recur.outer.mc_ki[recur.outer.mc_top]),
                        &data, pp_txnid4chk(mp, txn))) != MDBX_SUCCESS))
        goto fail;

      if ((flags & MDBX_LIFORECLAIM) && !txn->tw.lifo_reclaimed) {
        txn->tw.lifo_reclaimed = mdbx_txl_alloc();
        if (unlikely(!txn->tw.lifo_reclaimed)) {
          ret.err = MDBX_ENOMEM;
          goto fail;
        }
      }

      /* Append PNL from GC record to tw.reclaimed_pglist */
      mdbx_cassert(mc, (mc->mc_flags & C_GCFREEZE) == 0);
      pgno_t *gc_pnl = (pgno_t *)data.iov_base;
      mdbx_tassert(txn, data.iov_len >= MDBX_PNL_SIZEOF(gc_pnl));
      if (unlikely(data.iov_len < MDBX_PNL_SIZEOF(gc_pnl) ||
                   !mdbx_pnl_check(gc_pnl, txn->mt_next_pgno))) {
        ret.err = MDBX_CORRUPTED;
        goto fail;
      }
      const unsigned gc_len = MDBX_PNL_SIZE(gc_pnl);
      if (unlikely(/* list is too long already */ MDBX_PNL_SIZE(
                       txn->tw.reclaimed_pglist) >=
                   env->me_options.rp_augment_limit) &&
          ((/* not a slot-request from gc-update */
            (flags & MDBX_ALLOC_SLOT) == 0 &&
            /* have enough unallocated space */ txn->mt_geo.upper >=
                pgno_add(txn->mt_next_pgno, num)) ||
           gc_len + MDBX_PNL_SIZE(txn->tw.reclaimed_pglist) >=
               MDBX_PGL_LIMIT)) {
        /* Stop reclaiming to avoid overflow the page list.
         * This is a rare case while search for a continuously multi-page region
         * in a large database. https://github.com/erthink/libmdbx/issues/123 */
        mdbx_notice("stop reclaiming to avoid PNL overflow: %u (current) + %u "
                    "(chunk) -> %u",
                    MDBX_PNL_SIZE(txn->tw.reclaimed_pglist), gc_len,
                    gc_len + MDBX_PNL_SIZE(txn->tw.reclaimed_pglist));
        flags &= ~(MDBX_ALLOC_GC | MDBX_COALESCE);
        break;
      }
      ret.err = mdbx_pnl_need(&txn->tw.reclaimed_pglist, gc_len);
      if (unlikely(ret.err != MDBX_SUCCESS))
        goto fail;
      re_list = txn->tw.reclaimed_pglist;

      /* Remember ID of GC record */
      if (flags & MDBX_LIFORECLAIM) {
        ret.err = mdbx_txl_append(&txn->tw.lifo_reclaimed, last);
        if (unlikely(ret.err != MDBX_SUCCESS))
          goto fail;
      }
      txn->tw.last_reclaimed = last;

      if (mdbx_log_enabled(MDBX_LOG_EXTRA)) {
        mdbx_debug_extra("PNL read txn %" PRIaTXN " root %" PRIaPGNO
                         " num %u, PNL",
                         last, txn->mt_dbs[FREE_DBI].md_root, gc_len);
        for (unsigned i = gc_len; i; i--)
          mdbx_debug_extra_print(" %" PRIaPGNO, gc_pnl[i]);
        mdbx_debug_extra_print("%s\n", ".");
      }

      /* Merge in descending sorted order */
      const unsigned prev_re_len = MDBX_PNL_SIZE(re_list);
      mdbx_pnl_xmerge(re_list, gc_pnl);
      /* re-check to avoid duplicates */
      if (!MDBX_DISABLE_PAGECHECKS &&
          unlikely(!mdbx_pnl_check(re_list, txn->mt_next_pgno))) {
        ret.err = MDBX_CORRUPTED;
        goto fail;
      }
      mdbx_tassert(txn, mdbx_dirtylist_check(txn));

      re_len = MDBX_PNL_SIZE(re_list);
      mdbx_tassert(txn, re_len == 0 || re_list[re_len] < txn->mt_next_pgno);
      if (MDBX_ENABLE_REFUND && re_len &&
          unlikely(MDBX_PNL_MOST(re_list) == txn->mt_next_pgno - 1)) {
        /* Refund suitable pages into "unallocated" space */
        mdbx_refund(txn);
        re_list = txn->tw.reclaimed_pglist;
        re_len = MDBX_PNL_SIZE(re_list);
      }

      /* Done for a kick-reclaim mode, actually no page needed */
      if (unlikely(num == 0)) {
        mdbx_debug("early-return NULL-page for %s mode", "MDBX_ALLOC_SLOT");
        mdbx_assert(env, flags & MDBX_ALLOC_SLOT);
        ret.err = MDBX_SUCCESS;
        ret.page = NULL;
        return ret;
      }

      /* Don't try to coalesce too much. */
      if (flags & MDBX_COALESCE) {
        if (re_len /* current size */ > coalesce_threshold ||
            (re_len > prev_re_len &&
             re_len - prev_re_len /* delta from prev */ >=
                 coalesce_threshold / 2)) {
          mdbx_trace("clear %s %s", "MDBX_COALESCE", "since got threshold");
          flags &= ~MDBX_COALESCE;
        }
      }
    }

    if (F_ISSET(flags, MDBX_COALESCE | MDBX_ALLOC_CACHE)) {
      mdbx_debug_extra("clear %s and continue", "MDBX_COALESCE");
      flags &= ~MDBX_COALESCE;
      continue;
    }

    /* There is no suitable pages in the GC and to be able to allocate
     * we should CHOICE one of:
     *  - make a new steady checkpoint if reclaiming was stopped by
     *    the last steady-sync, or wipe it in the MDBX_UTTERLY_NOSYNC mode;
     *  - kick lagging reader(s) if reclaiming was stopped by ones of it.
     *  - extend the database file. */

    /* Will use new pages from the map if nothing is suitable in the GC. */
    range_begin = 0;
    pgno = txn->mt_next_pgno;
    const pgno_t next = pgno_add(pgno, num);

    if (flags & MDBX_ALLOC_GC) {
      const MDBX_meta *const head = mdbx_meta_head(env);
      MDBX_meta *const steady = mdbx_meta_steady(env);
      /* does reclaiming stopped at the last steady point? */
      if (head != steady && META_IS_STEADY(steady) &&
          oldest == mdbx_meta_txnid_stable(env, steady)) {
        mdbx_debug("gc-kick-steady: head %" PRIaTXN "-%s, tail %" PRIaTXN
                   "-%s, oldest %" PRIaTXN,
                   mdbx_meta_txnid_stable(env, head), mdbx_durable_str(head),
                   mdbx_meta_txnid_stable(env, steady),
                   mdbx_durable_str(steady), oldest);
        ret.err = MDBX_RESULT_TRUE;
        const pgno_t autosync_threshold =
            atomic_load32(&env->me_lck->mti_autosync_threshold, mo_Relaxed);
        const uint64_t autosync_period =
            atomic_load64(&env->me_lck->mti_autosync_period, mo_Relaxed);
        /* wipe the last steady-point if one of:
         *  - UTTERLY_NOSYNC mode AND auto-sync threshold is NOT specified
         *  - UTTERLY_NOSYNC mode AND free space at steady-point is exhausted
         * otherwise, make a new steady-point if one of:
         *  - auto-sync threshold is specified and reached;
         *  - upper limit of database size is reached;
         *  - database is full (with the current file size)
         *       AND auto-sync threshold it NOT specified */
        if (F_ISSET(env->me_flags, MDBX_UTTERLY_NOSYNC) &&
            ((autosync_threshold | autosync_period) == 0 ||
             next >= steady->mm_geo.now)) {
          /* wipe steady checkpoint in MDBX_UTTERLY_NOSYNC mode
           * without any auto-sync threshold(s). */
          ret.err = mdbx_wipe_steady(env, oldest);
          mdbx_debug("gc-wipe-steady, rc %d", ret.err);
          mdbx_assert(env, steady != mdbx_meta_steady(env));
        } else if ((flags & MDBX_ALLOC_NEW) == 0 ||
                   (autosync_threshold &&
                    atomic_load32(&env->me_lck->mti_unsynced_pages,
                                  mo_Relaxed) >= autosync_threshold) ||
                   (autosync_period &&
                    mdbx_osal_monotime() -
                            atomic_load64(&env->me_lck->mti_sync_timestamp,
                                          mo_Relaxed) >=
                        autosync_period) ||
                   next >= txn->mt_geo.upper ||
                   (next >= txn->mt_end_pgno &&
                    (autosync_threshold | autosync_period) == 0)) {
          /* make steady checkpoint. */
          MDBX_meta meta = *head;
          ret.err = mdbx_sync_locked(env, env->me_flags & MDBX_WRITEMAP, &meta);
          mdbx_debug("gc-make-steady, rc %d", ret.err);
          mdbx_assert(env, steady != mdbx_meta_steady(env));
        }
        if (ret.err == MDBX_SUCCESS) {
          if (mdbx_find_oldest(txn) > oldest)
            continue;
          /* it is reasonable check/kick lagging reader(s) here,
           * since we made a new steady point or wipe the last. */
          if (oldest < txn->mt_txnid - xMDBX_TXNID_STEP &&
              mdbx_kick_longlived_readers(env, oldest) > oldest)
            continue;
        } else if (unlikely(ret.err != MDBX_RESULT_TRUE))
          goto fail;
      }
    }

    /* don't kick lagging reader(s) if is enough unallocated space
     * at the end of database file. */
    if ((flags & MDBX_ALLOC_NEW) && next <= txn->mt_end_pgno)
      goto done;
    if ((flags & MDBX_ALLOC_GC) && oldest < txn->mt_txnid - xMDBX_TXNID_STEP &&
        mdbx_kick_longlived_readers(env, oldest) > oldest)
      continue;

    ret.err = MDBX_NOTFOUND;
    if (flags & MDBX_ALLOC_NEW) {
      ret.err = MDBX_MAP_FULL;
      if (next <= txn->mt_geo.upper && txn->mt_geo.grow_pv) {
        mdbx_assert(env, next > txn->mt_end_pgno);
        const pgno_t grow_step = pv2pages(txn->mt_geo.grow_pv);
        pgno_t aligned = pgno_align2os_pgno(
            env, pgno_add(next, grow_step - next % grow_step));

        if (aligned > txn->mt_geo.upper)
          aligned = txn->mt_geo.upper;
        mdbx_assert(env, aligned > txn->mt_end_pgno);

        mdbx_verbose("try growth datafile to %" PRIaPGNO " pages (+%" PRIaPGNO
                     ")",
                     aligned, aligned - txn->mt_end_pgno);
        ret.err = mdbx_mapresize_implicit(env, txn->mt_next_pgno, aligned,
                                          txn->mt_geo.upper);
        if (ret.err == MDBX_SUCCESS) {
          env->me_txn->mt_end_pgno = aligned;
          goto done;
        }

        mdbx_error("unable growth datafile to %" PRIaPGNO " pages (+%" PRIaPGNO
                   "), errcode %d",
                   aligned, aligned - txn->mt_end_pgno, ret.err);
      } else {
        mdbx_debug("gc-alloc: next %u > upper %u", next, txn->mt_geo.upper);
      }
    }

  fail:
    mdbx_tassert(txn,
                 mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                       txn->mt_next_pgno - MDBX_ENABLE_REFUND));
    if (likely(!(flags & MDBX_ALLOC_SLOT)))
      txn->mt_flags |= MDBX_TXN_ERROR;
    if (num != 1 || ret.err != MDBX_NOTFOUND)
      mdbx_notice("alloc %u pages failed, flags 0x%x, errcode %d", num, flags,
                  ret.err);
    else
      mdbx_trace("alloc %u pages failed, flags 0x%x, errcode %d", num, flags,
                 ret.err);
    mdbx_assert(env, ret.err != MDBX_SUCCESS);
    ret.page = NULL;
    return ret;
  }

done:
  ret.page = NULL;
  if (unlikely(flags & MDBX_ALLOC_SLOT)) {
    mdbx_debug("return NULL-page for %s mode", "MDBX_ALLOC_SLOT");
    ret.err = MDBX_SUCCESS;
    return ret;
  }

  mdbx_ensure(env, pgno >= NUM_METAS);
  if (env->me_flags & MDBX_WRITEMAP) {
    ret.page = pgno2page(env, pgno);
    /* LY: reset no-access flag from mdbx_page_loose() */
    VALGRIND_MAKE_MEM_UNDEFINED(ret.page, pgno2bytes(env, num));
    MDBX_ASAN_UNPOISON_MEMORY_REGION(ret.page, pgno2bytes(env, num));
  } else {
    if (unlikely(!(ret.page = mdbx_page_malloc(txn, num)))) {
      ret.err = MDBX_ENOMEM;
      goto fail;
    }
  }

  if (range_begin) {
    mdbx_cassert(mc, (mc->mc_flags & C_GCFREEZE) == 0);
    mdbx_tassert(txn, pgno < txn->mt_next_pgno);
    mdbx_tassert(txn, pgno == re_list[range_begin]);
    /* Cutoff allocated pages from tw.reclaimed_pglist */
#if MDBX_PNL_ASCENDING
    for (unsigned i = range_begin + num; i <= re_len;)
      re_list[range_begin++] = re_list[i++];
    MDBX_PNL_SIZE(re_list) = re_len = range_begin - 1;
#else
    MDBX_PNL_SIZE(re_list) = re_len -= num;
    for (unsigned i = range_begin - num; i < re_len;)
      re_list[++i] = re_list[++range_begin];
#endif
    mdbx_tassert(txn,
                 mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                       txn->mt_next_pgno - MDBX_ENABLE_REFUND));
  } else {
    txn->mt_next_pgno = pgno + num;
    mdbx_assert(env, txn->mt_next_pgno <= txn->mt_end_pgno);
  }

  if (unlikely(env->me_flags & MDBX_PAGEPERTURB))
    memset(ret.page, -1, pgno2bytes(env, num));
  VALGRIND_MAKE_MEM_UNDEFINED(ret.page, pgno2bytes(env, num));

  ret.page->mp_pgno = pgno;
  ret.page->mp_leaf2_ksize = 0;
  ret.page->mp_flags = 0;
  if ((mdbx_assert_enabled() || mdbx_audit_enabled()) && num > 1) {
    ret.page->mp_pages = num;
    ret.page->mp_flags = P_OVERFLOW;
  }
  ret.err = mdbx_page_dirty(txn, ret.page, num);
  if (unlikely(ret.err != MDBX_SUCCESS))
    goto fail;

  mdbx_tassert(txn,
               mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                     txn->mt_next_pgno - MDBX_ENABLE_REFUND));
  return ret;
}

/* Copy the used portions of a non-overflow page. */
__hot static void mdbx_page_copy(MDBX_page *dst, const MDBX_page *src,
                                 size_t psize) {
  STATIC_ASSERT(UINT16_MAX > MAX_PAGESIZE - PAGEHDRSZ);
  STATIC_ASSERT(MIN_PAGESIZE > PAGEHDRSZ + NODESIZE * 4);
  if ((src->mp_flags & (P_LEAF2 | P_OVERFLOW)) == 0) {
    size_t upper = src->mp_upper, lower = src->mp_lower, unused = upper - lower;

    /* If page isn't full, just copy the used portion. Adjust
     * alignment so memcpy may copy words instead of bytes. */
    if (unused >= MDBX_CACHELINE_SIZE * 2) {
      lower = ceil_powerof2(lower + PAGEHDRSZ, sizeof(void *));
      upper = floor_powerof2(upper + PAGEHDRSZ, sizeof(void *));
      memcpy(dst, src, lower);
      dst = (void *)((char *)dst + upper);
      src = (void *)((char *)src + upper);
      psize -= upper;
    }
  }
  memcpy(dst, src, psize);
}

/* Pull a page off the txn's spill list, if present.
 *
 * If a page being referenced was spilled to disk in this txn, bring
 * it back and make it dirty/writable again. */
static struct page_result __must_check_result
mdbx_page_unspill(MDBX_txn *const txn, const MDBX_page *const mp) {
  mdbx_verbose("unspill page %" PRIaPGNO, mp->mp_pgno);
  mdbx_tassert(txn, (txn->mt_flags & MDBX_WRITEMAP) == 0);
  mdbx_tassert(txn, IS_SPILLED(txn, mp));
  const pgno_t spilled_pgno = mp->mp_pgno << 1;
  const MDBX_txn *scan = txn;
  struct page_result ret;
  do {
    mdbx_tassert(txn, (scan->mt_flags & MDBX_TXN_SPILLS) != 0);
    if (!scan->tw.spill_pages)
      continue;
    const unsigned si = mdbx_pnl_exist(scan->tw.spill_pages, spilled_pgno);
    if (!si)
      continue;
    const unsigned npages = IS_OVERFLOW(mp) ? mp->mp_pages : 1;
    ret.page = mdbx_page_malloc(txn, npages);
    if (unlikely(!ret.page)) {
      ret.err = MDBX_ENOMEM;
      return ret;
    }
    mdbx_page_copy(ret.page, mp, pgno2bytes(txn->mt_env, npages));
    if (scan == txn) {
      /* If in current txn, this page is no longer spilled.
       * If it happens to be the last page, truncate the spill list.
       * Otherwise mark it as deleted by setting the LSB. */
      mdbx_spill_remove(txn, si, npages);
    } /* otherwise, if belonging to a parent txn, the
       * page remains spilled until child commits */

    ret.err = mdbx_page_dirty(txn, ret.page, npages);
    if (unlikely(ret.err != MDBX_SUCCESS))
      return ret;
#if MDBX_ENABLE_PGOP_STAT
    txn->mt_env->me_lck->mti_pgop_stat.unspill.weak += npages;
#endif /* MDBX_ENABLE_PGOP_STAT */
    ret.page->mp_flags |= (scan == txn) ? 0 : P_SPILLED;
    ret.err = MDBX_SUCCESS;
    return ret;
  } while (likely((scan = scan->mt_parent) != nullptr &&
                  (scan->mt_flags & MDBX_TXN_SPILLS) != 0));
  mdbx_error("Page %" PRIaPGNO " mod-txnid %" PRIaTXN
             " not found in the spill-list(s), current txn %" PRIaTXN
             " front %" PRIaTXN ", root txn %" PRIaTXN " front %" PRIaTXN,
             mp->mp_pgno, mp->mp_txnid, txn->mt_txnid, txn->mt_front,
             txn->mt_env->me_txn0->mt_txnid, txn->mt_env->me_txn0->mt_front);
  ret.err = MDBX_PROBLEM;
  ret.page = NULL;
  return ret;
}

/* Touch a page: make it dirty and re-insert into tree with updated pgno.
 * Set MDBX_TXN_ERROR on failure.
 *
 * [in] mc  cursor pointing to the page to be touched
 *
 * Returns 0 on success, non-zero on failure. */
__hot static int mdbx_page_touch(MDBX_cursor *mc) {
  const MDBX_page *const mp = mc->mc_pg[mc->mc_top];
  MDBX_page *np;
  MDBX_txn *txn = mc->mc_txn;
  int rc;

  if (mdbx_assert_enabled()) {
    if (mc->mc_flags & C_SUB) {
      MDBX_xcursor *mx = container_of(mc->mc_db, MDBX_xcursor, mx_db);
      MDBX_cursor_couple *couple = container_of(mx, MDBX_cursor_couple, inner);
      mdbx_tassert(txn, mc->mc_db == &couple->outer.mc_xcursor->mx_db);
      mdbx_tassert(txn, mc->mc_dbx == &couple->outer.mc_xcursor->mx_dbx);
      mdbx_tassert(txn, *couple->outer.mc_dbistate & DBI_DIRTY);
    } else {
      mdbx_tassert(txn, *mc->mc_dbistate & DBI_DIRTY);
    }
    mdbx_tassert(txn, mc->mc_txn->mt_flags & MDBX_TXN_DIRTY);
    mdbx_tassert(txn, !IS_OVERFLOW(mp));
    mdbx_tassert(txn, mdbx_dirtylist_check(txn));
  }

  if (IS_MODIFIABLE(txn, mp) || IS_SUBP(mp))
    return MDBX_SUCCESS;

  if (IS_FROZEN(txn, mp)) {
    /* CoW the page */
    rc = mdbx_pnl_need(&txn->tw.retired_pages, 1);
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;
    const struct page_result par = mdbx_page_alloc(mc, 1, MDBX_ALLOC_ALL);
    rc = par.err;
    np = par.page;
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;

    const pgno_t pgno = np->mp_pgno;
    mdbx_debug("touched db %d page %" PRIaPGNO " -> %" PRIaPGNO, DDBI(mc),
               mp->mp_pgno, pgno);
    mdbx_tassert(txn, mp->mp_pgno != pgno);
    mdbx_pnl_xappend(txn->tw.retired_pages, mp->mp_pgno);
    /* Update the parent page, if any, to point to the new page */
    if (mc->mc_top) {
      MDBX_page *parent = mc->mc_pg[mc->mc_top - 1];
      MDBX_node *node = page_node(parent, mc->mc_ki[mc->mc_top - 1]);
      node_set_pgno(node, pgno);
    } else {
      mc->mc_db->md_root = pgno;
    }

#if MDBX_ENABLE_PGOP_STAT
    txn->mt_env->me_lck->mti_pgop_stat.cow.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
    mdbx_page_copy(np, mp, txn->mt_env->me_psize);
    np->mp_pgno = pgno;
    np->mp_txnid = txn->mt_front;
  } else if (IS_SPILLED(txn, mp)) {
    struct page_result pur = mdbx_page_unspill(txn, mp);
    np = pur.page;
    rc = pur.err;
    if (likely(rc == MDBX_SUCCESS)) {
      mdbx_tassert(txn, np != nullptr);
      goto done;
    }
    goto fail;
  } else {
    if (unlikely(!txn->mt_parent)) {
      mdbx_error("Unexpected not frozen/modifiable/spilled but shadowed %s "
                 "page %" PRIaPGNO " mod-txnid %" PRIaTXN ","
                 " without parent transaction, current txn %" PRIaTXN
                 " front %" PRIaTXN,
                 IS_BRANCH(mp) ? "branch" : "leaf", mp->mp_pgno, mp->mp_txnid,
                 mc->mc_txn->mt_txnid, mc->mc_txn->mt_front);
      rc = MDBX_PROBLEM;
      goto fail;
    }

    mdbx_debug("clone db %d page %" PRIaPGNO, DDBI(mc), mp->mp_pgno);
    mdbx_tassert(txn, txn->tw.dirtylist->length <=
                          MDBX_PGL_LIMIT + MDBX_PNL_GRANULATE);
    /* No - copy it */
    np = mdbx_page_malloc(txn, 1);
    if (unlikely(!np)) {
      rc = MDBX_ENOMEM;
      goto fail;
    }
    mdbx_page_copy(np, mp, txn->mt_env->me_psize);

    /* insert a clone of parent's dirty page, so don't touch dirtyroom */
    rc = mdbx_page_dirty(txn, np, 1);
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;

#if MDBX_ENABLE_PGOP_STAT
    txn->mt_env->me_lck->mti_pgop_stat.clone.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
  }

done:
  /* Adjust cursors pointing to mp */
  mc->mc_pg[mc->mc_top] = np;
  MDBX_cursor *m2 = txn->tw.cursors[mc->mc_dbi];
  if (mc->mc_flags & C_SUB) {
    for (; m2; m2 = m2->mc_next) {
      MDBX_cursor *m3 = &m2->mc_xcursor->mx_cursor;
      if (m3->mc_snum < mc->mc_snum)
        continue;
      if (m3->mc_pg[mc->mc_top] == mp)
        m3->mc_pg[mc->mc_top] = np;
    }
  } else {
    for (; m2; m2 = m2->mc_next) {
      if (m2->mc_snum < mc->mc_snum)
        continue;
      if (m2 == mc)
        continue;
      if (m2->mc_pg[mc->mc_top] == mp) {
        m2->mc_pg[mc->mc_top] = np;
        if (XCURSOR_INITED(m2) && IS_LEAF(np))
          XCURSOR_REFRESH(m2, np, m2->mc_ki[mc->mc_top]);
      }
    }
  }
  return MDBX_SUCCESS;

fail:
  txn->mt_flags |= MDBX_TXN_ERROR;
  return rc;
}

__cold static int mdbx_env_sync_internal(MDBX_env *env, bool force,
                                         bool nonblock) {
  bool locked = false;
  int rc = MDBX_RESULT_TRUE /* means "nothing to sync" */;

retry:;
  unsigned flags = env->me_flags & ~(MDBX_NOMETASYNC | MDBX_SHRINK_ALLOWED);
  if (unlikely((flags & (MDBX_RDONLY | MDBX_FATAL_ERROR | MDBX_ENV_ACTIVE)) !=
               MDBX_ENV_ACTIVE)) {
    rc = MDBX_EACCESS;
    if (!(flags & MDBX_ENV_ACTIVE))
      rc = MDBX_EPERM;
    if (flags & MDBX_FATAL_ERROR)
      rc = MDBX_PANIC;
    goto bailout;
  }

  const pgno_t unsynced_pages =
      atomic_load32(&env->me_lck->mti_unsynced_pages, mo_Relaxed);
  const MDBX_meta *head = mdbx_meta_head(env);
  const txnid_t head_txnid = mdbx_meta_txnid_fluid(env, head);
  const uint32_t synched_meta_txnid_u32 =
      atomic_load32(&env->me_lck->mti_meta_sync_txnid, mo_Relaxed);
  if (unsynced_pages == 0 && synched_meta_txnid_u32 == (uint32_t)head_txnid &&
      META_IS_STEADY(head))
    goto bailout;

  const pgno_t autosync_threshold =
      atomic_load32(&env->me_lck->mti_autosync_threshold, mo_Relaxed);
  const uint64_t autosync_period =
      atomic_load64(&env->me_lck->mti_autosync_period, mo_Relaxed);
  if (force || (autosync_threshold && unsynced_pages >= autosync_threshold) ||
      (autosync_period &&
       mdbx_osal_monotime() -
               atomic_load64(&env->me_lck->mti_sync_timestamp, mo_Relaxed) >=
           autosync_period))
    flags &= MDBX_WRITEMAP /* clear flags for full steady sync */;

  const bool inside_txn = (env->me_txn0->mt_owner == mdbx_thread_self());
  if (!inside_txn) {
    if (!locked) {
      int err;
      unsigned wops = 0;
      /* pre-sync to avoid latency for writer */
      if (unsynced_pages > /* FIXME: define threshold */ 16 &&
          (flags & MDBX_SAFE_NOSYNC) == 0) {
        mdbx_assert(env, ((flags ^ env->me_flags) & MDBX_WRITEMAP) == 0);
        if (flags & MDBX_WRITEMAP) {
          /* Acquire guard to avoid collision with remap */
#if defined(_WIN32) || defined(_WIN64)
          mdbx_srwlock_AcquireShared(&env->me_remap_guard);
#else
          err = mdbx_fastmutex_acquire(&env->me_remap_guard);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
#endif
          const size_t usedbytes = pgno_align2os_bytes(env, head->mm_geo.next);
          err = mdbx_msync(&env->me_dxb_mmap, 0, usedbytes, MDBX_SYNC_DATA);
#if defined(_WIN32) || defined(_WIN64)
          mdbx_srwlock_ReleaseShared(&env->me_remap_guard);
#else
          int unlock_err = mdbx_fastmutex_release(&env->me_remap_guard);
          if (unlikely(unlock_err != MDBX_SUCCESS) && err == MDBX_SUCCESS)
            err = unlock_err;
#endif
        } else
          err = mdbx_fsync(env->me_lazy_fd, MDBX_SYNC_DATA);

        if (unlikely(err != MDBX_SUCCESS))
          return err;

        /* pre-sync done */
        wops = 1;
        rc = MDBX_SUCCESS /* means "some data was synced" */;
      }

      err = mdbx_txn_lock(env, nonblock);
      if (unlikely(err != MDBX_SUCCESS))
        return err;

      locked = true;
#if MDBX_ENABLE_PGOP_STAT
      env->me_lck->mti_pgop_stat.wops.weak += wops;
#endif /* MDBX_ENABLE_PGOP_STAT */
      goto retry;
    }
    env->me_txn0->mt_txnid = head_txnid;
    mdbx_assert(env, head_txnid == meta_txnid(env, head, false));
    mdbx_assert(env, head_txnid == mdbx_recent_committed_txnid(env));
    mdbx_find_oldest(env->me_txn0);
    flags |= MDBX_SHRINK_ALLOWED;
  }

  mdbx_assert(env, inside_txn || locked);
  mdbx_assert(env, !inside_txn || (flags & MDBX_SHRINK_ALLOWED) == 0);

  if (!META_IS_STEADY(head) ||
      ((flags & MDBX_SAFE_NOSYNC) == 0 && unsynced_pages)) {
    mdbx_debug("meta-head %" PRIaPGNO ", %s, sync_pending %" PRIaPGNO,
               data_page(head)->mp_pgno, mdbx_durable_str(head),
               unsynced_pages);
    MDBX_meta meta = *head;
    rc = mdbx_sync_locked(env, flags, &meta);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;
  }

  /* LY: sync meta-pages if MDBX_NOMETASYNC enabled
   *     and someone was not synced above. */
  if (atomic_load32(&env->me_lck->mti_meta_sync_txnid, mo_Relaxed) !=
      (uint32_t)head_txnid) {
#if MDBX_ENABLE_PGOP_STAT
    env->me_lck->mti_pgop_stat.wops.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
    rc = (flags & MDBX_WRITEMAP)
             ? mdbx_msync(&env->me_dxb_mmap, 0,
                          pgno_align2os_bytes(env, NUM_METAS),
                          MDBX_SYNC_DATA | MDBX_SYNC_IODQ)
             : mdbx_fsync(env->me_lazy_fd, MDBX_SYNC_DATA | MDBX_SYNC_IODQ);
    if (likely(rc == MDBX_SUCCESS))
      atomic_store32(&env->me_lck->mti_meta_sync_txnid, (uint32_t)head_txnid,
                     mo_Relaxed);
  }

bailout:
  if (locked)
    mdbx_txn_unlock(env);
  return rc;
}

static __inline int check_env(const MDBX_env *env, const bool wanna_active) {
  if (unlikely(!env))
    return MDBX_EINVAL;

  if (unlikely(env->me_signature.weak != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

#if MDBX_ENV_CHECKPID
  if (unlikely(env->me_pid != mdbx_getpid())) {
    ((MDBX_env *)env)->me_flags |= MDBX_FATAL_ERROR;
    return MDBX_PANIC;
  }
#endif /* MDBX_ENV_CHECKPID */

  if (unlikely(env->me_flags & MDBX_FATAL_ERROR))
    return MDBX_PANIC;

  if (wanna_active) {
    if (unlikely((env->me_flags & MDBX_ENV_ACTIVE) == 0))
      return MDBX_EPERM;
    mdbx_assert(env, env->me_map != nullptr);
  }

  return MDBX_SUCCESS;
}

__cold int mdbx_env_sync_ex(MDBX_env *env, bool force, bool nonblock) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_env_sync_internal(env, force, nonblock);
}

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
__cold int mdbx_env_sync(MDBX_env *env) { return __inline_mdbx_env_sync(env); }

__cold int mdbx_env_sync_poll(MDBX_env *env) {
  return __inline_mdbx_env_sync_poll(env);
}
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

/* Back up parent txn's cursors, then grab the originals for tracking */
static int mdbx_cursor_shadow(MDBX_txn *parent, MDBX_txn *nested) {
  for (int i = parent->mt_numdbs; --i >= 0;) {
    nested->tw.cursors[i] = NULL;
    MDBX_cursor *mc = parent->tw.cursors[i];
    if (mc != NULL) {
      size_t size = mc->mc_xcursor ? sizeof(MDBX_cursor) + sizeof(MDBX_xcursor)
                                   : sizeof(MDBX_cursor);
      for (MDBX_cursor *bk; mc; mc = bk->mc_next) {
        bk = mc;
        if (mc->mc_signature != MDBX_MC_LIVE)
          continue;
        bk = mdbx_malloc(size);
        if (unlikely(!bk))
          return MDBX_ENOMEM;
        *bk = *mc;
        mc->mc_backup = bk;
        /* Kill pointers into src to reduce abuse: The
         * user may not use mc until dst ends. But we need a valid
         * txn pointer here for cursor fixups to keep working. */
        mc->mc_txn = nested;
        mc->mc_db = &nested->mt_dbs[i];
        mc->mc_dbistate = &nested->mt_dbistate[i];
        MDBX_xcursor *mx = mc->mc_xcursor;
        if (mx != NULL) {
          *(MDBX_xcursor *)(bk + 1) = *mx;
          mx->mx_cursor.mc_txn = nested;
        }
        mc->mc_next = nested->tw.cursors[i];
        nested->tw.cursors[i] = mc;
      }
    }
  }
  return MDBX_SUCCESS;
}

/* Close this write txn's cursors, give parent txn's cursors back to parent.
 *
 * [in] txn     the transaction handle.
 * [in] merge   true to keep changes to parent cursors, false to revert.
 *
 * Returns 0 on success, non-zero on failure. */
static void mdbx_cursors_eot(MDBX_txn *txn, const bool merge) {
  mdbx_tassert(txn, (txn->mt_flags & MDBX_TXN_RDONLY) == 0);
  for (int i = txn->mt_numdbs; --i >= 0;) {
    MDBX_cursor *next, *mc = txn->tw.cursors[i];
    if (!mc)
      continue;
    txn->tw.cursors[i] = NULL;
    do {
      const unsigned stage = mc->mc_signature;
      MDBX_cursor *bk = mc->mc_backup;
      next = mc->mc_next;
      mdbx_ensure(txn->mt_env,
                  stage == MDBX_MC_LIVE || (stage == MDBX_MC_WAIT4EOT && bk));
      mdbx_cassert(mc, mc->mc_dbi == (unsigned)i);
      if (bk) {
        MDBX_xcursor *mx = mc->mc_xcursor;
        mdbx_cassert(mc, mx == bk->mc_xcursor);
        mdbx_tassert(txn, txn->mt_parent != NULL);
        mdbx_ensure(txn->mt_env, bk->mc_signature == MDBX_MC_LIVE);
        if (stage == MDBX_MC_WAIT4EOT /* Cursor was closed by user */)
          mc->mc_signature = stage /* Promote closed state to parent txn */;
        else if (merge) {
          /* Restore pointers to parent txn */
          mc->mc_next = bk->mc_next;
          mc->mc_backup = bk->mc_backup;
          mc->mc_txn = bk->mc_txn;
          mc->mc_db = bk->mc_db;
          mc->mc_dbistate = bk->mc_dbistate;
          if (mx) {
            if (mx != bk->mc_xcursor) {
              *bk->mc_xcursor = *mx;
              mx = bk->mc_xcursor;
            }
            mx->mx_cursor.mc_txn = bk->mc_txn;
          }
        } else {
          /* Restore from backup, i.e. rollback/abort nested txn */
          *mc = *bk;
          if (mx)
            *mx = *(MDBX_xcursor *)(bk + 1);
        }
        bk->mc_signature = 0;
        mdbx_free(bk);
      } else {
        mdbx_ensure(txn->mt_env, stage == MDBX_MC_LIVE);
        mc->mc_signature = MDBX_MC_READY4CLOSE /* Cursor may be reused */;
        mc->mc_flags = 0 /* reset C_UNTRACK */;
      }
    } while ((mc = next) != NULL);
  }
}

#if defined(MDBX_USE_VALGRIND) || defined(__SANITIZE_ADDRESS__)
/* Find largest mvcc-snapshot still referenced by this process. */
static pgno_t mdbx_find_largest_this(MDBX_env *env, pgno_t largest) {
  MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
  if (likely(lck != NULL /* exclusive mode */)) {
    const unsigned snap_nreaders =
        atomic_load32(&lck->mti_numreaders, mo_AcquireRelease);
    for (unsigned i = 0; i < snap_nreaders; ++i) {
    retry:
      if (atomic_load32(&lck->mti_readers[i].mr_pid, mo_AcquireRelease) ==
          env->me_pid) {
        /* mdbx_jitter4testing(true); */
        const pgno_t snap_pages = atomic_load32(
            &lck->mti_readers[i].mr_snapshot_pages_used, mo_Relaxed);
        const txnid_t snap_txnid = safe64_read(&lck->mti_readers[i].mr_txnid);
        if (unlikely(
                snap_pages !=
                    atomic_load32(&lck->mti_readers[i].mr_snapshot_pages_used,
                                  mo_AcquireRelease) ||
                snap_txnid != safe64_read(&lck->mti_readers[i].mr_txnid)))
          goto retry;
        if (largest < snap_pages &&
            atomic_load64(&lck->mti_oldest_reader, mo_AcquireRelease) <=
                /* ignore pending updates */ snap_txnid &&
            snap_txnid <= MAX_TXNID)
          largest = snap_pages;
      }
    }
  }
  return largest;
}

static void mdbx_txn_valgrind(MDBX_env *env, MDBX_txn *txn) {
#if !defined(__SANITIZE_ADDRESS__)
  if (!RUNNING_ON_VALGRIND)
    return;
#endif

  if (txn) { /* transaction start */
    if (env->me_poison_edge < txn->mt_next_pgno)
      env->me_poison_edge = txn->mt_next_pgno;
    VALGRIND_MAKE_MEM_DEFINED(env->me_map, pgno2bytes(env, txn->mt_next_pgno));
    MDBX_ASAN_UNPOISON_MEMORY_REGION(env->me_map,
                                     pgno2bytes(env, txn->mt_next_pgno));
    /* don't touch more, it should be already poisoned */
  } else { /* transaction end */
    bool should_unlock = false;
    pgno_t last = MAX_PAGENO;
    if (env->me_txn0 && env->me_txn0->mt_owner == mdbx_thread_self()) {
      /* inside write-txn */
      MDBX_meta *head = mdbx_meta_head(env);
      last = head->mm_geo.next;
    } else if (env->me_flags & MDBX_RDONLY) {
      /* read-only mode, no write-txn, no wlock mutex */
      last = NUM_METAS;
    } else if (mdbx_txn_lock(env, true) == MDBX_SUCCESS) {
      /* no write-txn */
      last = NUM_METAS;
      should_unlock = true;
    } else {
      /* write txn is running, therefore shouldn't poison any memory range */
      return;
    }

    last = mdbx_find_largest_this(env, last);
    const pgno_t edge = env->me_poison_edge;
    if (edge > last) {
      mdbx_assert(env, last >= NUM_METAS);
      env->me_poison_edge = last;
      VALGRIND_MAKE_MEM_NOACCESS(env->me_map + pgno2bytes(env, last),
                                 pgno2bytes(env, edge - last));
      MDBX_ASAN_POISON_MEMORY_REGION(env->me_map + pgno2bytes(env, last),
                                     pgno2bytes(env, edge - last));
    }
    if (should_unlock)
      mdbx_txn_unlock(env);
  }
}
#endif /* MDBX_USE_VALGRIND || __SANITIZE_ADDRESS__ */

typedef struct {
  int err;
  MDBX_reader *rslot;
} bind_rslot_result;

static bind_rslot_result bind_rslot(MDBX_env *env, const uintptr_t tid) {
  mdbx_assert(env, env->me_lck_mmap.lck);
  mdbx_assert(env, env->me_lck->mti_magic_and_version == MDBX_LOCK_MAGIC);
  mdbx_assert(env, env->me_lck->mti_os_and_format == MDBX_LOCK_FORMAT);

  bind_rslot_result result = {mdbx_rdt_lock(env), nullptr};
  if (unlikely(MDBX_IS_ERROR(result.err)))
    return result;
  if (unlikely(env->me_flags & MDBX_FATAL_ERROR)) {
    mdbx_rdt_unlock(env);
    result.err = MDBX_PANIC;
    return result;
  }
  if (unlikely(!env->me_map)) {
    mdbx_rdt_unlock(env);
    result.err = MDBX_EPERM;
    return result;
  }

  if (unlikely(env->me_live_reader != env->me_pid)) {
    result.err = mdbx_rpid_set(env);
    if (unlikely(result.err != MDBX_SUCCESS)) {
      mdbx_rdt_unlock(env);
      return result;
    }
    env->me_live_reader = env->me_pid;
  }

  result.err = MDBX_SUCCESS;
  unsigned slot, nreaders;
  while (1) {
    nreaders = atomic_load32(&env->me_lck->mti_numreaders, mo_Relaxed);
    for (slot = 0; slot < nreaders; slot++)
      if (atomic_load32(&env->me_lck->mti_readers[slot].mr_pid, mo_Relaxed) ==
          0)
        break;

    if (likely(slot < env->me_maxreaders))
      break;

    result.err = mdbx_cleanup_dead_readers(env, true, NULL);
    if (result.err != MDBX_RESULT_TRUE) {
      mdbx_rdt_unlock(env);
      result.err =
          (result.err == MDBX_SUCCESS) ? MDBX_READERS_FULL : result.err;
      return result;
    }
  }

  result.rslot = &env->me_lck->mti_readers[slot];
  /* Claim the reader slot, carefully since other code
   * uses the reader table un-mutexed: First reset the
   * slot, next publish it in lck->mti_numreaders.  After
   * that, it is safe for mdbx_env_close() to touch it.
   * When it will be closed, we can finally claim it. */
  atomic_store32(&result.rslot->mr_pid, 0, mo_Relaxed);
  safe64_reset(&result.rslot->mr_txnid, true);
  if (slot == nreaders)
    atomic_store32(&env->me_lck->mti_numreaders, ++nreaders, mo_Relaxed);
  atomic_store64(&result.rslot->mr_tid, (env->me_flags & MDBX_NOTLS) ? 0 : tid,
                 mo_Relaxed);
  atomic_store32(&result.rslot->mr_pid, env->me_pid, mo_Relaxed);
  mdbx_rdt_unlock(env);

  if (likely(env->me_flags & MDBX_ENV_TXKEY)) {
    mdbx_assert(env, env->me_live_reader == env->me_pid);
    thread_rthc_set(env->me_txkey, result.rslot);
  }
  return result;
}

__cold int mdbx_thread_register(const MDBX_env *env) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!env->me_lck_mmap.lck))
    return (env->me_flags & MDBX_EXCLUSIVE) ? MDBX_EINVAL : MDBX_EPERM;

  if (unlikely((env->me_flags & MDBX_ENV_TXKEY) == 0)) {
    mdbx_assert(env, !env->me_lck_mmap.lck || (env->me_flags & MDBX_NOTLS));
    return MDBX_EINVAL /* MDBX_NOTLS mode */;
  }

  mdbx_assert(env, (env->me_flags & (MDBX_NOTLS | MDBX_ENV_TXKEY |
                                     MDBX_EXCLUSIVE)) == MDBX_ENV_TXKEY);
  MDBX_reader *r = thread_rthc_get(env->me_txkey);
  if (unlikely(r != NULL)) {
    mdbx_assert(env, r->mr_pid.weak == env->me_pid);
    mdbx_assert(env, r->mr_tid.weak == mdbx_thread_self());
    if (unlikely(r->mr_pid.weak != env->me_pid))
      return MDBX_BAD_RSLOT;
    return MDBX_RESULT_TRUE /* already registered */;
  }

  const uintptr_t tid = mdbx_thread_self();
  if (env->me_txn0 && unlikely(env->me_txn0->mt_owner == tid))
    return MDBX_TXN_OVERLAPPING;
  return bind_rslot((MDBX_env *)env, tid).err;
}

__cold int mdbx_thread_unregister(const MDBX_env *env) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!env->me_lck_mmap.lck))
    return MDBX_RESULT_TRUE;

  if (unlikely((env->me_flags & MDBX_ENV_TXKEY) == 0)) {
    mdbx_assert(env, !env->me_lck_mmap.lck || (env->me_flags & MDBX_NOTLS));
    return MDBX_RESULT_TRUE /* MDBX_NOTLS mode */;
  }

  mdbx_assert(env, (env->me_flags & (MDBX_NOTLS | MDBX_ENV_TXKEY |
                                     MDBX_EXCLUSIVE)) == MDBX_ENV_TXKEY);
  MDBX_reader *r = thread_rthc_get(env->me_txkey);
  if (unlikely(r == NULL))
    return MDBX_RESULT_TRUE /* not registered */;

  mdbx_assert(env, r->mr_pid.weak == env->me_pid);
  mdbx_assert(env, r->mr_tid.weak == mdbx_thread_self());
  if (unlikely(r->mr_pid.weak != env->me_pid ||
               r->mr_tid.weak != mdbx_thread_self()))
    return MDBX_BAD_RSLOT;

  if (unlikely(r->mr_txnid.weak < SAFE64_INVALID_THRESHOLD))
    return MDBX_BUSY /* transaction is still active */;

  atomic_store32(&r->mr_pid, 0, mo_Relaxed);
  atomic_store32(&env->me_lck->mti_readers_refresh_flag, true,
                 mo_AcquireRelease);
  thread_rthc_set(env->me_txkey, nullptr);
  return MDBX_SUCCESS;
}

/* Common code for mdbx_txn_begin() and mdbx_txn_renew(). */
static int mdbx_txn_renew0(MDBX_txn *txn, const unsigned flags) {
  MDBX_env *env = txn->mt_env;
  int rc;

#if MDBX_ENV_CHECKPID
  if (unlikely(env->me_pid != mdbx_getpid())) {
    env->me_flags |= MDBX_FATAL_ERROR;
    return MDBX_PANIC;
  }
#endif /* MDBX_ENV_CHECKPID */

  STATIC_ASSERT(sizeof(MDBX_reader) == 32);
#if MDBX_LOCKING > 0
  STATIC_ASSERT(offsetof(MDBX_lockinfo, mti_wlock) % MDBX_CACHELINE_SIZE == 0);
  STATIC_ASSERT(offsetof(MDBX_lockinfo, mti_rlock) % MDBX_CACHELINE_SIZE == 0);
#else
  STATIC_ASSERT(
      offsetof(MDBX_lockinfo, mti_oldest_reader) % MDBX_CACHELINE_SIZE == 0);
  STATIC_ASSERT(offsetof(MDBX_lockinfo, mti_numreaders) % MDBX_CACHELINE_SIZE ==
                0);
#endif /* MDBX_LOCKING */
  STATIC_ASSERT(offsetof(MDBX_lockinfo, mti_readers) % MDBX_CACHELINE_SIZE ==
                0);

  const uintptr_t tid = mdbx_thread_self();
  if (flags & MDBX_TXN_RDONLY) {
    mdbx_assert(env, (flags & ~(MDBX_TXN_RO_BEGIN_FLAGS | MDBX_WRITEMAP)) == 0);
    txn->mt_flags =
        MDBX_TXN_RDONLY | (env->me_flags & (MDBX_NOTLS | MDBX_WRITEMAP));
    MDBX_reader *r = txn->to.reader;
    STATIC_ASSERT(sizeof(uintptr_t) <= sizeof(r->mr_tid));
    if (likely(env->me_flags & MDBX_ENV_TXKEY)) {
      mdbx_assert(env, !(env->me_flags & MDBX_NOTLS));
      r = thread_rthc_get(env->me_txkey);
      if (likely(r)) {
        if (unlikely(!r->mr_pid.weak) &&
            (mdbx_runtime_flags & MDBX_DBG_LEGACY_MULTIOPEN)) {
          thread_rthc_set(env->me_txkey, nullptr);
          r = nullptr;
        } else {
          mdbx_assert(env, r->mr_pid.weak == env->me_pid);
          mdbx_assert(env, r->mr_tid.weak == mdbx_thread_self());
        }
      }
    } else {
      mdbx_assert(env, !env->me_lck_mmap.lck || (env->me_flags & MDBX_NOTLS));
    }

    if (likely(r)) {
      if (unlikely(r->mr_pid.weak != env->me_pid ||
                   r->mr_txnid.weak < SAFE64_INVALID_THRESHOLD))
        return MDBX_BAD_RSLOT;
    } else if (env->me_lck_mmap.lck) {
      bind_rslot_result brs = bind_rslot(env, tid);
      if (unlikely(brs.err != MDBX_SUCCESS))
        return brs.err;
      r = brs.rslot;
    }
    txn->to.reader = r;
    if (flags & (MDBX_TXN_RDONLY_PREPARE - MDBX_TXN_RDONLY)) {
      mdbx_assert(env, txn->mt_txnid == 0);
      mdbx_assert(env, txn->mt_owner == 0);
      mdbx_assert(env, txn->mt_numdbs == 0);
      if (likely(r)) {
        mdbx_assert(env, r->mr_snapshot_pages_used.weak == 0);
        mdbx_assert(env, r->mr_txnid.weak >= SAFE64_INVALID_THRESHOLD);
        atomic_store32(&r->mr_snapshot_pages_used, 0, mo_Relaxed);
      }
      txn->mt_flags = MDBX_TXN_RDONLY | MDBX_TXN_FINISHED;
      return MDBX_SUCCESS;
    }

    /* Seek & fetch the last meta */
    if (likely(/* not recovery mode */ env->me_stuck_meta < 0)) {
      while (1) {
        MDBX_meta *const meta = mdbx_meta_head(env);
        mdbx_jitter4testing(false);
        const txnid_t snap = mdbx_meta_txnid_fluid(env, meta);
        mdbx_jitter4testing(false);
        if (likely(r)) {
          safe64_reset(&r->mr_txnid, false);
          atomic_store32(&r->mr_snapshot_pages_used, meta->mm_geo.next,
                         mo_Relaxed);
          atomic_store64(&r->mr_snapshot_pages_retired,
                         unaligned_peek_u64(4, meta->mm_pages_retired),
                         mo_Relaxed);
          safe64_write(&r->mr_txnid, snap);
          mdbx_jitter4testing(false);
          mdbx_assert(env, r->mr_pid.weak == mdbx_getpid());
          mdbx_assert(
              env, r->mr_tid.weak ==
                       ((env->me_flags & MDBX_NOTLS) ? 0 : mdbx_thread_self()));
          mdbx_assert(env, r->mr_txnid.weak == snap);
          atomic_store32(&env->me_lck->mti_readers_refresh_flag, true,
                         mo_AcquireRelease);
        }
        mdbx_jitter4testing(true);

        /* Snap the state from current meta-head */
        txn->mt_txnid = snap;
        txn->mt_geo = meta->mm_geo;
        memcpy(txn->mt_dbs, meta->mm_dbs, CORE_DBS * sizeof(MDBX_db));
        txn->mt_canary = meta->mm_canary;

        /* LY: Retry on a race, ITS#7970. */
        if (likely(meta == mdbx_meta_head(env) &&
                   snap == mdbx_meta_txnid_fluid(env, meta) &&
                   snap >= atomic_load64(&env->me_lck->mti_oldest_reader,
                                         mo_AcquireRelease))) {
          mdbx_jitter4testing(false);
          break;
        }
      }
    } else {
      /* r/o recovery mode */
      MDBX_meta *const meta = METAPAGE(env, env->me_stuck_meta);
      txn->mt_txnid = mdbx_meta_txnid_stable(env, meta);
      txn->mt_geo = meta->mm_geo;
      memcpy(txn->mt_dbs, meta->mm_dbs, CORE_DBS * sizeof(MDBX_db));
      txn->mt_canary = meta->mm_canary;
      if (likely(r)) {
        atomic_store32(&r->mr_snapshot_pages_used, meta->mm_geo.next,
                       mo_Relaxed);
        atomic_store64(&r->mr_snapshot_pages_retired,
                       unaligned_peek_u64(4, meta->mm_pages_retired),
                       mo_Relaxed);
        atomic_store64(&r->mr_txnid, txn->mt_txnid, mo_Relaxed);
        mdbx_jitter4testing(false);
        mdbx_assert(env, r->mr_pid.weak == mdbx_getpid());
        mdbx_assert(
            env, r->mr_tid.weak ==
                     ((env->me_flags & MDBX_NOTLS) ? 0 : mdbx_thread_self()));
        mdbx_assert(env, r->mr_txnid.weak == txn->mt_txnid);
        atomic_store32(&env->me_lck->mti_readers_refresh_flag, true,
                       mo_Relaxed);
      }
    }

    if (unlikely(txn->mt_txnid < MIN_TXNID || txn->mt_txnid > MAX_TXNID)) {
      mdbx_error("%s", "environment corrupted by died writer, must shutdown!");
      rc = MDBX_CORRUPTED;
      goto bailout;
    }
    mdbx_assert(env, txn->mt_txnid >= env->me_lck->mti_oldest_reader.weak);
    txn->mt_dbxs = env->me_dbxs; /* mostly static anyway */
    mdbx_ensure(env, txn->mt_txnid >=
                         /* paranoia is appropriate here */ env->me_lck
                             ->mti_oldest_reader.weak);
    txn->mt_numdbs = env->me_numdbs;
  } else {
    mdbx_assert(env, (flags & ~(MDBX_TXN_RW_BEGIN_FLAGS | MDBX_TXN_SPILLS |
                                MDBX_WRITEMAP)) == 0);
    if (unlikely(txn->mt_owner == tid ||
                 /* not recovery mode */ env->me_stuck_meta >= 0))
      return MDBX_BUSY;
    MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
    if (lck && (env->me_flags & MDBX_NOTLS) == 0 &&
        (mdbx_runtime_flags & MDBX_DBG_LEGACY_OVERLAP) == 0) {
      const unsigned snap_nreaders =
          atomic_load32(&lck->mti_numreaders, mo_AcquireRelease);
      for (unsigned i = 0; i < snap_nreaders; ++i) {
        if (atomic_load32(&lck->mti_readers[i].mr_pid, mo_Relaxed) ==
                env->me_pid &&
            unlikely(atomic_load64(&lck->mti_readers[i].mr_tid, mo_Relaxed) ==
                     tid)) {
          const txnid_t txnid = safe64_read(&lck->mti_readers[i].mr_txnid);
          if (txnid >= MIN_TXNID && txnid <= MAX_TXNID)
            return MDBX_TXN_OVERLAPPING;
        }
      }
    }

    /* Not yet touching txn == env->me_txn0, it may be active */
    mdbx_jitter4testing(false);
    rc = mdbx_txn_lock(env, F_ISSET(flags, MDBX_TXN_TRY));
    if (unlikely(rc))
      return rc;
    if (unlikely(env->me_flags & MDBX_FATAL_ERROR)) {
      mdbx_txn_unlock(env);
      return MDBX_PANIC;
    }
#if defined(_WIN32) || defined(_WIN64)
    if (unlikely(!env->me_map)) {
      mdbx_txn_unlock(env);
      return MDBX_EPERM;
    }
#endif /* Windows */

    mdbx_jitter4testing(false);
    MDBX_meta *meta = mdbx_meta_head(env);
    mdbx_jitter4testing(false);
    txn->mt_canary = meta->mm_canary;
    const txnid_t snap = mdbx_meta_txnid_stable(env, meta);
    txn->mt_txnid = safe64_txnid_next(snap);
    if (unlikely(txn->mt_txnid > MAX_TXNID)) {
      rc = MDBX_TXN_FULL;
      mdbx_error("txnid overflow, raise %d", rc);
      goto bailout;
    }

    txn->mt_flags = flags;
    txn->mt_child = NULL;
    txn->tw.loose_pages = NULL;
    txn->tw.loose_count = 0;
#if MDBX_ENABLE_REFUND
    txn->tw.loose_refund_wl = 0;
#endif /* MDBX_ENABLE_REFUND */
    MDBX_PNL_SIZE(txn->tw.retired_pages) = 0;
    txn->tw.spill_pages = NULL;
    txn->tw.spill_least_removed = 0;
    txn->tw.last_reclaimed = 0;
    if (txn->tw.lifo_reclaimed)
      MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) = 0;
    env->me_txn = txn;
    txn->mt_numdbs = env->me_numdbs;
    memcpy(txn->mt_dbiseqs, env->me_dbiseqs, txn->mt_numdbs * sizeof(unsigned));
    /* Copy the DB info and flags */
    memcpy(txn->mt_dbs, meta->mm_dbs, CORE_DBS * sizeof(MDBX_db));
    /* Moved to here to avoid a data race in read TXNs */
    txn->mt_geo = meta->mm_geo;

    rc = mdbx_dpl_alloc(txn);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;
    txn->tw.dirtyroom = txn->mt_env->me_options.dp_limit;
    txn->tw.dirtylru = MDBX_DEBUG ? ~42u : 0;
  }

  /* Setup db info */
  mdbx_compiler_barrier();
  for (unsigned i = CORE_DBS; i < txn->mt_numdbs; i++) {
    const unsigned db_flags = env->me_dbflags[i];
    txn->mt_dbs[i].md_flags = db_flags & DB_PERSISTENT_FLAGS;
    txn->mt_dbistate[i] =
        (db_flags & DB_VALID) ? DBI_VALID | DBI_USRVALID | DBI_STALE : 0;
  }
  txn->mt_dbistate[MAIN_DBI] = DBI_VALID | DBI_USRVALID;
  txn->mt_dbistate[FREE_DBI] = DBI_VALID;
  txn->mt_front =
      txn->mt_txnid + ((flags & (MDBX_WRITEMAP | MDBX_RDONLY)) == 0);

  if (unlikely(env->me_flags & MDBX_FATAL_ERROR)) {
    mdbx_warning("%s", "environment had fatal error, must shutdown!");
    rc = MDBX_PANIC;
  } else {
    const size_t size =
        pgno2bytes(env, (txn->mt_flags & MDBX_TXN_RDONLY) ? txn->mt_next_pgno
                                                          : txn->mt_end_pgno);
    if (unlikely(size > env->me_dxb_mmap.limit)) {
      if (txn->mt_geo.upper > MAX_PAGENO ||
          bytes2pgno(env, pgno2bytes(env, txn->mt_geo.upper)) !=
              txn->mt_geo.upper) {
        rc = MDBX_UNABLE_EXTEND_MAPSIZE;
        goto bailout;
      }
      rc = mdbx_mapresize(env, txn->mt_next_pgno, txn->mt_end_pgno,
                          txn->mt_geo.upper,
                          (txn->mt_flags & MDBX_TXN_RDONLY) ? true : false);
      if (rc != MDBX_SUCCESS)
        goto bailout;
    }
    if (txn->mt_flags & MDBX_TXN_RDONLY) {
#if defined(_WIN32) || defined(_WIN64)
      if (((size > env->me_dbgeo.lower && env->me_dbgeo.shrink) ||
           (mdbx_RunningUnderWine() &&
            /* under Wine acquisition of remap_guard is always required,
             * since Wine don't support section extending,
             * i.e. in both cases unmap+map are required. */
            size < env->me_dbgeo.upper && env->me_dbgeo.grow)) &&
          /* avoid recursive use SRW */ (txn->mt_flags & MDBX_NOTLS) == 0) {
        txn->mt_flags |= MDBX_SHRINK_ALLOWED;
        mdbx_srwlock_AcquireShared(&env->me_remap_guard);
      }
#endif /* Windows */
    } else {
      env->me_dxb_mmap.current = size;
      env->me_dxb_mmap.filesize =
          (env->me_dxb_mmap.filesize < size) ? size : env->me_dxb_mmap.filesize;
    }
#if defined(MDBX_USE_VALGRIND) || defined(__SANITIZE_ADDRESS__)
    mdbx_txn_valgrind(env, txn);
#endif
    txn->mt_owner = tid;
    return MDBX_SUCCESS;
  }
bailout:
  mdbx_tassert(txn, rc != MDBX_SUCCESS);
  mdbx_txn_end(txn, MDBX_END_SLOT | MDBX_END_FAIL_BEGIN);
  return rc;
}

static __always_inline int check_txn(const MDBX_txn *txn, int bad_bits) {
  if (unlikely(!txn))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_signature != MDBX_MT_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(txn->mt_flags & bad_bits))
    return MDBX_BAD_TXN;

#if MDBX_TXN_CHECKOWNER
  if ((txn->mt_flags & MDBX_NOTLS) == 0 &&
      unlikely(txn->mt_owner != mdbx_thread_self()))
    return txn->mt_owner ? MDBX_THREAD_MISMATCH : MDBX_BAD_TXN;
#endif /* MDBX_TXN_CHECKOWNER */

  if (unlikely(!txn->mt_env->me_map))
    return MDBX_EPERM;

  return MDBX_SUCCESS;
}

static __always_inline int check_txn_rw(const MDBX_txn *txn, int bad_bits) {
  int err = check_txn(txn, bad_bits);
  if (unlikely(err))
    return err;

  if (unlikely(F_ISSET(txn->mt_flags, MDBX_TXN_RDONLY)))
    return MDBX_EACCESS;

  return MDBX_SUCCESS;
}

int mdbx_txn_renew(MDBX_txn *txn) {
  if (unlikely(!txn))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_signature != MDBX_MT_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely((txn->mt_flags & MDBX_TXN_RDONLY) == 0))
    return MDBX_EINVAL;

  int rc;
  if (unlikely(txn->mt_owner != 0 || !(txn->mt_flags & MDBX_TXN_FINISHED))) {
    rc = mdbx_txn_reset(txn);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  rc = mdbx_txn_renew0(txn, MDBX_TXN_RDONLY);
  if (rc == MDBX_SUCCESS) {
    txn->mt_owner = mdbx_thread_self();
    mdbx_debug("renew txn %" PRIaTXN "%c %p on env %p, root page %" PRIaPGNO
               "/%" PRIaPGNO,
               txn->mt_txnid, (txn->mt_flags & MDBX_TXN_RDONLY) ? 'r' : 'w',
               (void *)txn, (void *)txn->mt_env, txn->mt_dbs[MAIN_DBI].md_root,
               txn->mt_dbs[FREE_DBI].md_root);
  }
  return rc;
}

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
int mdbx_txn_begin(MDBX_env *env, MDBX_txn *parent, MDBX_txn_flags_t flags,
                   MDBX_txn **ret) {
  return __inline_mdbx_txn_begin(env, parent, flags, ret);
}
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

int mdbx_txn_set_userctx(MDBX_txn *txn, void *ctx) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED - MDBX_TXN_HAS_CHILD);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  txn->mt_userctx = ctx;
  return MDBX_SUCCESS;
}

void *mdbx_txn_get_userctx(const MDBX_txn *txn) {
  return check_txn(txn, MDBX_TXN_BLOCKED - MDBX_TXN_HAS_CHILD)
             ? nullptr
             : txn->mt_userctx;
}

int mdbx_txn_begin_ex(MDBX_env *env, MDBX_txn *parent, MDBX_txn_flags_t flags,
                      MDBX_txn **ret, void *context) {
  MDBX_txn *txn;
  unsigned size, tsize;

  if (unlikely(!ret))
    return MDBX_EINVAL;
  *ret = NULL;

  if (unlikely((flags & ~MDBX_TXN_RW_BEGIN_FLAGS) &&
               (flags & ~MDBX_TXN_RO_BEGIN_FLAGS)))
    return MDBX_EINVAL;

  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(env->me_flags & MDBX_RDONLY &
               ~flags)) /* write txn in RDONLY env */
    return MDBX_EACCESS;

  flags |= env->me_flags & MDBX_WRITEMAP;

  if (parent) {
    /* Nested transactions: Max 1 child, write txns only, no writemap */
    rc = check_txn_rw(parent,
                      MDBX_TXN_RDONLY | MDBX_WRITEMAP | MDBX_TXN_BLOCKED);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;

    if (env->me_options.spill_parent4child_denominator) {
      /* Spill dirty-pages of parent to provide dirtyroom for child txn */
      rc = mdbx_txn_spill(parent, nullptr,
                          parent->tw.dirtylist->length /
                              env->me_options.spill_parent4child_denominator);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    }
    mdbx_tassert(parent, mdbx_audit_ex(parent, 0, false) == 0);

    flags |= parent->mt_flags & (MDBX_TXN_RW_BEGIN_FLAGS | MDBX_TXN_SPILLS);
    /* Child txns save MDBX_pgstate and use own copy of cursors */
    size = env->me_maxdbs * (sizeof(MDBX_db) + sizeof(MDBX_cursor *) + 1);
    size += tsize = sizeof(MDBX_txn);
  } else if (flags & MDBX_TXN_RDONLY) {
    if (env->me_txn0 &&
        unlikely(env->me_txn0->mt_owner == mdbx_thread_self()) &&
        (mdbx_runtime_flags & MDBX_DBG_LEGACY_OVERLAP) == 0)
      return MDBX_TXN_OVERLAPPING;
    size = env->me_maxdbs * (sizeof(MDBX_db) + 1);
    size += tsize = sizeof(MDBX_txn);
  } else {
    /* Reuse preallocated write txn. However, do not touch it until
     * mdbx_txn_renew0() succeeds, since it currently may be active. */
    txn = env->me_txn0;
    goto renew;
  }
  if (unlikely((txn = mdbx_malloc(size)) == NULL)) {
    mdbx_debug("calloc: %s", "failed");
    return MDBX_ENOMEM;
  }
  memset(txn, 0, tsize);
  txn->mt_dbxs = env->me_dbxs; /* static */
  txn->mt_dbs = (MDBX_db *)((char *)txn + tsize);
  txn->mt_dbistate = (uint8_t *)txn + size - env->me_maxdbs;
  txn->mt_flags = flags;
  txn->mt_env = env;

  if (parent) {
    mdbx_tassert(parent, mdbx_dirtylist_check(parent));
    txn->tw.cursors = (MDBX_cursor **)(txn->mt_dbs + env->me_maxdbs);
    txn->mt_dbiseqs = parent->mt_dbiseqs;
    txn->mt_geo = parent->mt_geo;
    rc = mdbx_dpl_alloc(txn);
    if (likely(rc == MDBX_SUCCESS)) {
      const unsigned len =
          MDBX_PNL_SIZE(parent->tw.reclaimed_pglist) + parent->tw.loose_count;
      txn->tw.reclaimed_pglist =
          mdbx_pnl_alloc((len > MDBX_PNL_INITIAL) ? len : MDBX_PNL_INITIAL);
      if (unlikely(!txn->tw.reclaimed_pglist))
        rc = MDBX_ENOMEM;
    }
    if (unlikely(rc != MDBX_SUCCESS)) {
    nested_failed:
      mdbx_pnl_free(txn->tw.reclaimed_pglist);
      mdbx_dpl_free(txn);
      mdbx_free(txn);
      return rc;
    }

    /* Move loose pages to reclaimed list */
    if (parent->tw.loose_count) {
      do {
        MDBX_page *lp = parent->tw.loose_pages;
        const unsigned di = mdbx_dpl_exist(parent, lp->mp_pgno);
        mdbx_tassert(parent, di && parent->tw.dirtylist->items[di].ptr == lp);
        mdbx_tassert(parent, lp->mp_flags == P_LOOSE);
        rc =
            mdbx_pnl_insert_range(&parent->tw.reclaimed_pglist, lp->mp_pgno, 1);
        if (unlikely(rc != MDBX_SUCCESS))
          goto nested_failed;
        parent->tw.loose_pages = lp->mp_next;
        /* Remove from dirty list */
        mdbx_page_wash(parent, di, lp, 1);
      } while (parent->tw.loose_pages);
      parent->tw.loose_count = 0;
#if MDBX_ENABLE_REFUND
      parent->tw.loose_refund_wl = 0;
#endif /* MDBX_ENABLE_REFUND */
      mdbx_tassert(parent, mdbx_dirtylist_check(parent));
    }
    txn->tw.dirtyroom = parent->tw.dirtyroom;
    txn->tw.dirtylru = parent->tw.dirtylru;

    mdbx_dpl_sort(parent);
    if (parent->tw.spill_pages)
      mdbx_spill_purge(parent);

    mdbx_tassert(txn, MDBX_PNL_ALLOCLEN(txn->tw.reclaimed_pglist) >=
                          MDBX_PNL_SIZE(parent->tw.reclaimed_pglist));
    memcpy(txn->tw.reclaimed_pglist, parent->tw.reclaimed_pglist,
           MDBX_PNL_SIZEOF(parent->tw.reclaimed_pglist));
    mdbx_assert(env, mdbx_pnl_check4assert(
                         txn->tw.reclaimed_pglist,
                         (txn->mt_next_pgno /* LY: intentional assignment here,
                                                   only for assertion */
                          = parent->mt_next_pgno) -
                             MDBX_ENABLE_REFUND));

    txn->tw.last_reclaimed = parent->tw.last_reclaimed;
    if (parent->tw.lifo_reclaimed) {
      txn->tw.lifo_reclaimed = parent->tw.lifo_reclaimed;
      parent->tw.lifo_reclaimed =
          (void *)(intptr_t)MDBX_PNL_SIZE(parent->tw.lifo_reclaimed);
    }

    txn->tw.retired_pages = parent->tw.retired_pages;
    parent->tw.retired_pages =
        (void *)(intptr_t)MDBX_PNL_SIZE(parent->tw.retired_pages);

    txn->mt_txnid = parent->mt_txnid;
    txn->mt_front = parent->mt_front + 1;
#if MDBX_ENABLE_REFUND
    txn->tw.loose_refund_wl = 0;
#endif /* MDBX_ENABLE_REFUND */
    txn->mt_canary = parent->mt_canary;
    parent->mt_flags |= MDBX_TXN_HAS_CHILD;
    parent->mt_child = txn;
    txn->mt_parent = parent;
    txn->mt_numdbs = parent->mt_numdbs;
    txn->mt_owner = parent->mt_owner;
    memcpy(txn->mt_dbs, parent->mt_dbs, txn->mt_numdbs * sizeof(MDBX_db));
    /* Copy parent's mt_dbistate, but clear DB_NEW */
    for (unsigned i = 0; i < txn->mt_numdbs; i++)
      txn->mt_dbistate[i] =
          parent->mt_dbistate[i] & ~(DBI_FRESH | DBI_CREAT | DBI_DIRTY);
    mdbx_tassert(parent,
                 parent->tw.dirtyroom + parent->tw.dirtylist->length ==
                     (parent->mt_parent ? parent->mt_parent->tw.dirtyroom
                                        : parent->mt_env->me_options.dp_limit));
    mdbx_tassert(txn, txn->tw.dirtyroom + txn->tw.dirtylist->length ==
                          (txn->mt_parent ? txn->mt_parent->tw.dirtyroom
                                          : txn->mt_env->me_options.dp_limit));
    env->me_txn = txn;
    rc = mdbx_cursor_shadow(parent, txn);
    if (mdbx_audit_enabled() && mdbx_assert_enabled()) {
      txn->mt_signature = MDBX_MT_SIGNATURE;
      mdbx_tassert(txn, mdbx_audit_ex(txn, 0, false) == 0);
    }
    if (unlikely(rc != MDBX_SUCCESS))
      mdbx_txn_end(txn, MDBX_END_FAIL_BEGINCHILD);
  } else { /* MDBX_TXN_RDONLY */
    txn->mt_dbiseqs = env->me_dbiseqs;
  renew:
    rc = mdbx_txn_renew0(txn, flags);
  }

  if (unlikely(rc != MDBX_SUCCESS)) {
    if (txn != env->me_txn0)
      mdbx_free(txn);
  } else {
    if (flags & (MDBX_TXN_RDONLY_PREPARE - MDBX_TXN_RDONLY))
      mdbx_assert(env, txn->mt_flags == (MDBX_TXN_RDONLY | MDBX_TXN_FINISHED));
    else if (flags & MDBX_TXN_RDONLY)
      mdbx_assert(env, (txn->mt_flags &
                        ~(MDBX_NOTLS | MDBX_TXN_RDONLY | MDBX_WRITEMAP |
                          /* Win32: SRWL flag */ MDBX_SHRINK_ALLOWED)) == 0);
    else {
      mdbx_assert(env, (txn->mt_flags & ~(MDBX_WRITEMAP | MDBX_SHRINK_ALLOWED |
                                          MDBX_NOMETASYNC | MDBX_SAFE_NOSYNC |
                                          MDBX_TXN_SPILLS)) == 0);
      assert(!txn->tw.spill_pages && !txn->tw.spill_least_removed);
    }
    txn->mt_signature = MDBX_MT_SIGNATURE;
    txn->mt_userctx = context;
    *ret = txn;
    mdbx_debug("begin txn %" PRIaTXN "%c %p on env %p, root page %" PRIaPGNO
               "/%" PRIaPGNO,
               txn->mt_txnid, (flags & MDBX_TXN_RDONLY) ? 'r' : 'w',
               (void *)txn, (void *)env, txn->mt_dbs[MAIN_DBI].md_root,
               txn->mt_dbs[FREE_DBI].md_root);
  }

  return rc;
}

int mdbx_txn_info(const MDBX_txn *txn, MDBX_txn_info *info, bool scan_rlt) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED - MDBX_TXN_HAS_CHILD);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!info))
    return MDBX_EINVAL;

  MDBX_env *const env = txn->mt_env;
#if MDBX_ENV_CHECKPID
  if (unlikely(env->me_pid != mdbx_getpid())) {
    env->me_flags |= MDBX_FATAL_ERROR;
    return MDBX_PANIC;
  }
#endif /* MDBX_ENV_CHECKPID */

  info->txn_id = txn->mt_txnid;
  info->txn_space_used = pgno2bytes(env, txn->mt_geo.next);

  if (txn->mt_flags & MDBX_TXN_RDONLY) {
    const MDBX_meta *head_meta;
    txnid_t head_txnid;
    uint64_t head_retired;
    do {
      /* fetch info from volatile head */
      head_meta = mdbx_meta_head(env);
      head_txnid = mdbx_meta_txnid_fluid(env, head_meta);
      head_retired = unaligned_peek_u64(4, head_meta->mm_pages_retired);
      info->txn_space_limit_soft = pgno2bytes(env, head_meta->mm_geo.now);
      info->txn_space_limit_hard = pgno2bytes(env, head_meta->mm_geo.upper);
      info->txn_space_leftover =
          pgno2bytes(env, head_meta->mm_geo.now - head_meta->mm_geo.next);
      mdbx_compiler_barrier();
    } while (unlikely(head_meta != mdbx_meta_head(env) ||
                      head_txnid != mdbx_meta_txnid_fluid(env, head_meta)));

    info->txn_reader_lag = head_txnid - info->txn_id;
    info->txn_space_dirty = info->txn_space_retired = 0;
    uint64_t reader_snapshot_pages_retired;
    if (txn->to.reader &&
        head_retired >
            (reader_snapshot_pages_retired = atomic_load64(
                 &txn->to.reader->mr_snapshot_pages_retired, mo_Relaxed))) {
      info->txn_space_dirty = info->txn_space_retired = pgno2bytes(
          env, (pgno_t)(head_retired - reader_snapshot_pages_retired));

      size_t retired_next_reader = 0;
      MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
      if (scan_rlt && info->txn_reader_lag > 1 && lck) {
        /* find next more recent reader */
        txnid_t next_reader = head_txnid;
        const unsigned snap_nreaders =
            atomic_load32(&lck->mti_numreaders, mo_AcquireRelease);
        for (unsigned i = 0; i < snap_nreaders; ++i) {
        retry:
          if (atomic_load32(&lck->mti_readers[i].mr_pid, mo_AcquireRelease)) {
            mdbx_jitter4testing(true);
            const txnid_t snap_txnid =
                safe64_read(&lck->mti_readers[i].mr_txnid);
            const uint64_t snap_retired =
                atomic_load64(&lck->mti_readers[i].mr_snapshot_pages_retired,
                              mo_AcquireRelease);
            if (unlikely(snap_retired !=
                         atomic_load64(
                             &lck->mti_readers[i].mr_snapshot_pages_retired,
                             mo_Relaxed)) ||
                snap_txnid != safe64_read(&lck->mti_readers[i].mr_txnid))
              goto retry;
            if (snap_txnid <= txn->mt_txnid) {
              retired_next_reader = 0;
              break;
            }
            if (snap_txnid < next_reader) {
              next_reader = snap_txnid;
              retired_next_reader = pgno2bytes(
                  env, (pgno_t)(snap_retired -
                                atomic_load64(
                                    &txn->to.reader->mr_snapshot_pages_retired,
                                    mo_Relaxed)));
            }
          }
        }
      }
      info->txn_space_dirty = retired_next_reader;
    }
  } else {
    info->txn_space_limit_soft = pgno2bytes(env, txn->mt_geo.now);
    info->txn_space_limit_hard = pgno2bytes(env, txn->mt_geo.upper);
    info->txn_space_retired = pgno2bytes(
        env, txn->mt_child ? (unsigned)(uintptr_t)txn->tw.retired_pages
                           : MDBX_PNL_SIZE(txn->tw.retired_pages));
    info->txn_space_leftover = pgno2bytes(env, txn->tw.dirtyroom);
    info->txn_space_dirty =
        pgno2bytes(env, txn->mt_env->me_options.dp_limit - txn->tw.dirtyroom);
    info->txn_reader_lag = INT64_MAX;
    MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
    if (scan_rlt && lck) {
      txnid_t oldest_snapshot = txn->mt_txnid;
      const unsigned snap_nreaders =
          atomic_load32(&lck->mti_numreaders, mo_AcquireRelease);
      if (snap_nreaders) {
        oldest_snapshot = mdbx_find_oldest(txn);
        if (oldest_snapshot == txn->mt_txnid - 1) {
          /* check if there is at least one reader */
          bool exists = false;
          for (unsigned i = 0; i < snap_nreaders; ++i) {
            if (atomic_load32(&lck->mti_readers[i].mr_pid, mo_Relaxed) &&
                txn->mt_txnid > safe64_read(&lck->mti_readers[i].mr_txnid)) {
              exists = true;
              break;
            }
          }
          oldest_snapshot += !exists;
        }
      }
      info->txn_reader_lag = txn->mt_txnid - oldest_snapshot;
    }
  }

  return MDBX_SUCCESS;
}

MDBX_env *mdbx_txn_env(const MDBX_txn *txn) {
  if (unlikely(!txn || txn->mt_signature != MDBX_MT_SIGNATURE ||
               txn->mt_env->me_signature.weak != MDBX_ME_SIGNATURE))
    return NULL;
  return txn->mt_env;
}

uint64_t mdbx_txn_id(const MDBX_txn *txn) {
  if (unlikely(!txn || txn->mt_signature != MDBX_MT_SIGNATURE))
    return 0;
  return txn->mt_txnid;
}

int mdbx_txn_flags(const MDBX_txn *txn) {
  if (unlikely(!txn || txn->mt_signature != MDBX_MT_SIGNATURE))
    return -1;
  return txn->mt_flags;
}

/* Check for misused dbi handles */
#define TXN_DBI_CHANGED(txn, dbi)                                              \
  ((txn)->mt_dbiseqs[dbi] != (txn)->mt_env->me_dbiseqs[dbi])

static void dbi_import_locked(MDBX_txn *txn) {
  MDBX_env *const env = txn->mt_env;
  const unsigned n = env->me_numdbs;
  for (unsigned i = CORE_DBS; i < n; ++i) {
    if (i >= txn->mt_numdbs) {
      txn->mt_dbistate[i] = 0;
      if (!(txn->mt_flags & MDBX_TXN_RDONLY))
        txn->tw.cursors[i] = NULL;
    }
    if ((env->me_dbflags[i] & DB_VALID) &&
        !(txn->mt_dbistate[i] & DBI_USRVALID)) {
      txn->mt_dbiseqs[i] = env->me_dbiseqs[i];
      txn->mt_dbs[i].md_flags = env->me_dbflags[i] & DB_PERSISTENT_FLAGS;
      txn->mt_dbistate[i] = DBI_VALID | DBI_USRVALID | DBI_STALE;
      mdbx_tassert(txn, txn->mt_dbxs[i].md_cmp != NULL);
      mdbx_tassert(txn, txn->mt_dbxs[i].md_name.iov_base != NULL);
    }
  }
  txn->mt_numdbs = n;
}

/* Import DBI which opened after txn started into context */
__cold static bool dbi_import(MDBX_txn *txn, MDBX_dbi dbi) {
  if (dbi < CORE_DBS || dbi >= txn->mt_env->me_numdbs)
    return false;

  mdbx_ensure(txn->mt_env, mdbx_fastmutex_acquire(&txn->mt_env->me_dbi_lock) ==
                               MDBX_SUCCESS);
  dbi_import_locked(txn);
  mdbx_ensure(txn->mt_env, mdbx_fastmutex_release(&txn->mt_env->me_dbi_lock) ==
                               MDBX_SUCCESS);
  return txn->mt_dbistate[dbi] & DBI_USRVALID;
}

/* Export or close DBI handles opened in this txn. */
static void dbi_update(MDBX_txn *txn, int keep) {
  mdbx_tassert(txn, !txn->mt_parent && txn == txn->mt_env->me_txn0);
  MDBX_dbi n = txn->mt_numdbs;
  if (n) {
    bool locked = false;
    MDBX_env *const env = txn->mt_env;

    for (unsigned i = n; --i >= CORE_DBS;) {
      if (likely((txn->mt_dbistate[i] & DBI_CREAT) == 0))
        continue;
      if (!locked) {
        mdbx_ensure(env,
                    mdbx_fastmutex_acquire(&env->me_dbi_lock) == MDBX_SUCCESS);
        locked = true;
      }
      if (env->me_numdbs <= i || txn->mt_dbiseqs[i] != env->me_dbiseqs[i])
        continue /* dbi explicitly closed and/or then re-opened by other txn */;
      if (keep) {
        env->me_dbflags[i] = txn->mt_dbs[i].md_flags | DB_VALID;
      } else {
        char *ptr = env->me_dbxs[i].md_name.iov_base;
        if (ptr) {
          env->me_dbxs[i].md_name.iov_len = 0;
          mdbx_memory_fence(mo_AcquireRelease, true);
          mdbx_assert(env, env->me_dbflags[i] == 0);
          env->me_dbiseqs[i]++;
          env->me_dbxs[i].md_name.iov_base = NULL;
          mdbx_free(ptr);
        }
      }
    }

    n = env->me_numdbs;
    if (n > CORE_DBS && unlikely(!(env->me_dbflags[n - 1] & DB_VALID))) {
      if (!locked) {
        mdbx_ensure(env,
                    mdbx_fastmutex_acquire(&env->me_dbi_lock) == MDBX_SUCCESS);
        locked = true;
      }

      n = env->me_numdbs;
      while (n > CORE_DBS && !(env->me_dbflags[n - 1] & DB_VALID))
        --n;
      env->me_numdbs = n;
    }

    if (unlikely(locked))
      mdbx_ensure(env,
                  mdbx_fastmutex_release(&env->me_dbi_lock) == MDBX_SUCCESS);
  }
}

/* Filter-out pgno list from transaction's dirty-page list */
static void mdbx_dpl_sift(MDBX_txn *const txn, MDBX_PNL pl,
                          const bool spilled) {
  if (MDBX_PNL_SIZE(pl) && txn->tw.dirtylist->length) {
    mdbx_tassert(
        txn, mdbx_pnl_check4assert(pl, (size_t)txn->mt_next_pgno << spilled));
    MDBX_dpl *dl = mdbx_dpl_sort(txn);

    /* Scanning in ascend order */
    const int step = MDBX_PNL_ASCENDING ? 1 : -1;
    const int begin = MDBX_PNL_ASCENDING ? 1 : MDBX_PNL_SIZE(pl);
    const int end = MDBX_PNL_ASCENDING ? MDBX_PNL_SIZE(pl) + 1 : 0;
    mdbx_tassert(txn, pl[begin] <= pl[end - step]);

    unsigned r = mdbx_dpl_search(txn, pl[begin] >> spilled);
    mdbx_tassert(txn, dl->sorted == dl->length);
    for (int i = begin; r <= dl->length;) { /* scan loop */
      assert(i != end);
      mdbx_tassert(txn, !spilled || (pl[i] & 1) == 0);
      pgno_t pl_pgno = pl[i] >> spilled;
      pgno_t dp_pgno = dl->items[r].pgno;
      if (likely(dp_pgno != pl_pgno)) {
        const bool cmp = dp_pgno < pl_pgno;
        r += cmp;
        i += cmp ? 0 : step;
        if (likely(i != end))
          continue;
        return;
      }

      /* update loop */
      unsigned w = r;
    remove_dl:
      if ((txn->mt_env->me_flags & MDBX_WRITEMAP) == 0) {
        MDBX_page *dp = dl->items[r].ptr;
        mdbx_dpage_free(txn->mt_env, dp, dpl_npages(dl, r));
      }
      ++r;
    next_i:
      i += step;
      if (unlikely(i == end)) {
        while (r <= dl->length)
          dl->items[w++] = dl->items[r++];
      } else {
        while (r <= dl->length) {
          assert(i != end);
          mdbx_tassert(txn, !spilled || (pl[i] & 1) == 0);
          pl_pgno = pl[i] >> spilled;
          dp_pgno = dl->items[r].pgno;
          if (dp_pgno < pl_pgno)
            dl->items[w++] = dl->items[r++];
          else if (dp_pgno > pl_pgno)
            goto next_i;
          else
            goto remove_dl;
        }
      }
      dl->sorted = dpl_setlen(dl, w - 1);
      txn->tw.dirtyroom += r - w;
      mdbx_tassert(txn,
                   txn->tw.dirtyroom + txn->tw.dirtylist->length ==
                       (txn->mt_parent ? txn->mt_parent->tw.dirtyroom
                                       : txn->mt_env->me_options.dp_limit));
      return;
    }
  }
}

/* End a transaction, except successful commit of a nested transaction.
 * May be called twice for readonly txns: First reset it, then abort.
 * [in] txn   the transaction handle to end
 * [in] mode  why and how to end the transaction */
static int mdbx_txn_end(MDBX_txn *txn, const unsigned mode) {
  MDBX_env *env = txn->mt_env;
  static const char *const names[] = MDBX_END_NAMES;

#if MDBX_ENV_CHECKPID
  if (unlikely(txn->mt_env->me_pid != mdbx_getpid())) {
    env->me_flags |= MDBX_FATAL_ERROR;
    return MDBX_PANIC;
  }
#endif /* MDBX_ENV_CHECKPID */

  mdbx_debug("%s txn %" PRIaTXN "%c %p on mdbenv %p, root page %" PRIaPGNO
             "/%" PRIaPGNO,
             names[mode & MDBX_END_OPMASK], txn->mt_txnid,
             (txn->mt_flags & MDBX_TXN_RDONLY) ? 'r' : 'w', (void *)txn,
             (void *)env, txn->mt_dbs[MAIN_DBI].md_root,
             txn->mt_dbs[FREE_DBI].md_root);

  mdbx_ensure(env, txn->mt_txnid >=
                       /* paranoia is appropriate here */ env->me_lck
                           ->mti_oldest_reader.weak);

  int rc = MDBX_SUCCESS;
  if (F_ISSET(txn->mt_flags, MDBX_TXN_RDONLY)) {
    if (txn->to.reader) {
      MDBX_reader *slot = txn->to.reader;
      mdbx_assert(env, slot->mr_pid.weak == env->me_pid);
      if (likely(!F_ISSET(txn->mt_flags, MDBX_TXN_FINISHED))) {
        mdbx_assert(env, txn->mt_txnid == slot->mr_txnid.weak &&
                             slot->mr_txnid.weak >=
                                 env->me_lck->mti_oldest_reader.weak);
#if defined(MDBX_USE_VALGRIND) || defined(__SANITIZE_ADDRESS__)
        mdbx_txn_valgrind(env, nullptr);
#endif
        atomic_store32(&slot->mr_snapshot_pages_used, 0, mo_Relaxed);
        safe64_reset(&slot->mr_txnid, false);
        atomic_store32(&env->me_lck->mti_readers_refresh_flag, true,
                       mo_Relaxed);
      } else {
        mdbx_assert(env, slot->mr_pid.weak == env->me_pid);
        mdbx_assert(env, slot->mr_txnid.weak >= SAFE64_INVALID_THRESHOLD);
      }
      if (mode & MDBX_END_SLOT) {
        if ((env->me_flags & MDBX_ENV_TXKEY) == 0)
          atomic_store32(&slot->mr_pid, 0, mo_Relaxed);
        txn->to.reader = NULL;
      }
    }
#if defined(_WIN32) || defined(_WIN64)
    if (txn->mt_flags & MDBX_SHRINK_ALLOWED)
      mdbx_srwlock_ReleaseShared(&env->me_remap_guard);
#endif
    txn->mt_numdbs = 0; /* prevent further DBI activity */
    txn->mt_flags = MDBX_TXN_RDONLY | MDBX_TXN_FINISHED;
    txn->mt_owner = 0;
  } else if (!F_ISSET(txn->mt_flags, MDBX_TXN_FINISHED)) {
#if defined(MDBX_USE_VALGRIND) || defined(__SANITIZE_ADDRESS__)
    if (txn == env->me_txn0)
      mdbx_txn_valgrind(env, nullptr);
#endif
    if (!(mode & MDBX_END_EOTDONE)) /* !(already closed cursors) */
      mdbx_cursors_eot(txn, false);

    txn->mt_flags = MDBX_TXN_FINISHED;
    txn->mt_owner = 0;
    env->me_txn = txn->mt_parent;
    mdbx_pnl_free(txn->tw.spill_pages);
    txn->tw.spill_pages = nullptr;
    if (txn == env->me_txn0) {
      mdbx_assert(env, txn->mt_parent == NULL);
      /* Export or close DBI handles created in this txn */
      dbi_update(txn, mode & MDBX_END_UPDATE);
      mdbx_pnl_shrink(&txn->tw.retired_pages);
      mdbx_pnl_shrink(&txn->tw.reclaimed_pglist);
      if (!(env->me_flags & MDBX_WRITEMAP))
        mdbx_dlist_free(txn);
      /* The writer mutex was locked in mdbx_txn_begin. */
      mdbx_txn_unlock(env);
    } else {
      mdbx_assert(env, txn->mt_parent != NULL);
      MDBX_txn *const parent = txn->mt_parent;
      mdbx_assert(env, parent->mt_signature == MDBX_MT_SIGNATURE);
      mdbx_assert(env, parent->mt_child == txn &&
                           (parent->mt_flags & MDBX_TXN_HAS_CHILD) != 0);
      mdbx_assert(
          env, mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                     txn->mt_next_pgno - MDBX_ENABLE_REFUND));

      if (txn->tw.lifo_reclaimed) {
        mdbx_assert(env, MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) >=
                             (unsigned)(uintptr_t)parent->tw.lifo_reclaimed);
        MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) =
            (unsigned)(uintptr_t)parent->tw.lifo_reclaimed;
        parent->tw.lifo_reclaimed = txn->tw.lifo_reclaimed;
      }

      if (txn->tw.retired_pages) {
        mdbx_assert(env, MDBX_PNL_SIZE(txn->tw.retired_pages) >=
                             (unsigned)(uintptr_t)parent->tw.retired_pages);
        MDBX_PNL_SIZE(txn->tw.retired_pages) =
            (unsigned)(uintptr_t)parent->tw.retired_pages;
        parent->tw.retired_pages = txn->tw.retired_pages;
      }

      parent->mt_child = nullptr;
      parent->mt_flags &= ~MDBX_TXN_HAS_CHILD;
      parent->tw.dirtylru = txn->tw.dirtylru;
      mdbx_tassert(parent, mdbx_dirtylist_check(parent));
      mdbx_tassert(parent, mdbx_audit_ex(parent, 0, false) == 0);
      if (!(env->me_flags & MDBX_WRITEMAP))
        mdbx_dlist_free(txn);
      mdbx_dpl_free(txn);
      mdbx_pnl_free(txn->tw.reclaimed_pglist);

      if (parent->mt_geo.upper != txn->mt_geo.upper ||
          parent->mt_geo.now != txn->mt_geo.now) {
        /* undo resize performed by child txn */
        rc = mdbx_mapresize_implicit(env, parent->mt_next_pgno,
                                     parent->mt_geo.now, parent->mt_geo.upper);
        if (rc == MDBX_RESULT_TRUE) {
          /* unable undo resize (it is regular for Windows),
           * therefore promote size changes from child to the parent txn */
          mdbx_warning("unable undo resize performed by child txn, promote to "
                       "the parent (%u->%u, %u->%u)",
                       txn->mt_geo.now, parent->mt_geo.now, txn->mt_geo.upper,
                       parent->mt_geo.upper);
          parent->mt_geo.now = txn->mt_geo.now;
          parent->mt_geo.upper = txn->mt_geo.upper;
          rc = MDBX_SUCCESS;
        } else if (unlikely(rc != MDBX_SUCCESS)) {
          mdbx_error("error %d while undo resize performed by child txn, fail "
                     "the parent",
                     rc);
          parent->mt_flags |= MDBX_TXN_ERROR;
          if (!env->me_dxb_mmap.address)
            env->me_flags |= MDBX_FATAL_ERROR;
        }
      }
    }
  }

  mdbx_assert(env, txn == env->me_txn0 || txn->mt_owner == 0);
  if ((mode & MDBX_END_FREE) != 0 && txn != env->me_txn0) {
    txn->mt_signature = 0;
    mdbx_free(txn);
  }

  return rc;
}

int mdbx_txn_reset(MDBX_txn *txn) {
  int rc = check_txn(txn, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  /* This call is only valid for read-only txns */
  if (unlikely((txn->mt_flags & MDBX_TXN_RDONLY) == 0))
    return MDBX_EINVAL;

  /* LY: don't close DBI-handles */
  rc = mdbx_txn_end(txn, MDBX_END_RESET | MDBX_END_UPDATE);
  if (rc == MDBX_SUCCESS) {
    mdbx_tassert(txn, txn->mt_signature == MDBX_MT_SIGNATURE);
    mdbx_tassert(txn, txn->mt_owner == 0);
  }
  return rc;
}

int mdbx_txn_break(MDBX_txn *txn) {
  do {
    int rc = check_txn(txn, 0);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    txn->mt_flags |= MDBX_TXN_ERROR;
    if (txn->mt_flags & MDBX_TXN_RDONLY)
      break;
    txn = txn->mt_child;
  } while (txn);
  return MDBX_SUCCESS;
}

int mdbx_txn_abort(MDBX_txn *txn) {
  int rc = check_txn(txn, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (F_ISSET(txn->mt_flags, MDBX_TXN_RDONLY))
    /* LY: don't close DBI-handles */
    return mdbx_txn_end(txn, MDBX_END_ABORT | MDBX_END_UPDATE | MDBX_END_SLOT |
                                 MDBX_END_FREE);

  if (txn->mt_child)
    mdbx_txn_abort(txn->mt_child);

  mdbx_tassert(txn, mdbx_dirtylist_check(txn));
  return mdbx_txn_end(txn, MDBX_END_ABORT | MDBX_END_SLOT | MDBX_END_FREE);
}

/* Count all the pages in each DB and in the GC and make sure
 * it matches the actual number of pages being used. */
__cold static int mdbx_audit_ex(MDBX_txn *txn, unsigned retired_stored,
                                bool dont_filter_gc) {
  pgno_t pending = 0;
  if ((txn->mt_flags & MDBX_TXN_RDONLY) == 0) {
    pending = txn->tw.loose_count + MDBX_PNL_SIZE(txn->tw.reclaimed_pglist) +
              (MDBX_PNL_SIZE(txn->tw.retired_pages) - retired_stored);
  }

  MDBX_cursor_couple cx;
  int rc = mdbx_cursor_init(&cx.outer, txn, FREE_DBI);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  pgno_t gc = 0;
  MDBX_val key, data;
  while ((rc = mdbx_cursor_get(&cx.outer, &key, &data, MDBX_NEXT)) == 0) {
    if (!dont_filter_gc) {
      if (unlikely(key.iov_len != sizeof(txnid_t)))
        return MDBX_CORRUPTED;
      txnid_t id = unaligned_peek_u64(4, key.iov_base);
      if (txn->tw.lifo_reclaimed) {
        for (unsigned i = 1; i <= MDBX_PNL_SIZE(txn->tw.lifo_reclaimed); ++i)
          if (id == txn->tw.lifo_reclaimed[i])
            goto skip;
      } else if (id <= txn->tw.last_reclaimed)
        goto skip;
    }

    gc += *(pgno_t *)data.iov_base;
  skip:;
  }
  mdbx_tassert(txn, rc == MDBX_NOTFOUND);

  for (MDBX_dbi i = FREE_DBI; i < txn->mt_numdbs; i++)
    txn->mt_dbistate[i] &= ~DBI_AUDITED;

  pgno_t used = NUM_METAS;
  for (MDBX_dbi i = FREE_DBI; i <= MAIN_DBI; i++) {
    if (!(txn->mt_dbistate[i] & DBI_VALID))
      continue;
    rc = mdbx_cursor_init(&cx.outer, txn, i);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    txn->mt_dbistate[i] |= DBI_AUDITED;
    if (txn->mt_dbs[i].md_root == P_INVALID)
      continue;
    used += txn->mt_dbs[i].md_branch_pages + txn->mt_dbs[i].md_leaf_pages +
            txn->mt_dbs[i].md_overflow_pages;

    if (i != MAIN_DBI)
      continue;
    rc = mdbx_page_search(&cx.outer, NULL, MDBX_PS_FIRST);
    while (rc == MDBX_SUCCESS) {
      MDBX_page *mp = cx.outer.mc_pg[cx.outer.mc_top];
      for (unsigned j = 0; j < page_numkeys(mp); j++) {
        MDBX_node *node = page_node(mp, j);
        if (node_flags(node) == F_SUBDATA) {
          if (unlikely(node_ds(node) != sizeof(MDBX_db)))
            return MDBX_CORRUPTED;
          MDBX_db db_copy, *db;
          memcpy(db = &db_copy, node_data(node), sizeof(db_copy));
          if ((txn->mt_flags & MDBX_TXN_RDONLY) == 0) {
            for (MDBX_dbi k = txn->mt_numdbs; --k > MAIN_DBI;) {
              if ((txn->mt_dbistate[k] & DBI_VALID) &&
                  /* txn->mt_dbxs[k].md_name.iov_len > 0 && */
                  node_ks(node) == txn->mt_dbxs[k].md_name.iov_len &&
                  memcmp(node_key(node), txn->mt_dbxs[k].md_name.iov_base,
                         node_ks(node)) == 0) {
                txn->mt_dbistate[k] |= DBI_AUDITED;
                if (!(txn->mt_dbistate[k] & MDBX_DBI_STALE))
                  db = txn->mt_dbs + k;
                break;
              }
            }
          }
          used +=
              db->md_branch_pages + db->md_leaf_pages + db->md_overflow_pages;
        }
      }
      rc = mdbx_cursor_sibling(&cx.outer, SIBLING_RIGHT);
    }
    mdbx_tassert(txn, rc == MDBX_NOTFOUND);
  }

  for (MDBX_dbi i = FREE_DBI; i < txn->mt_numdbs; i++) {
    if ((txn->mt_dbistate[i] & (DBI_VALID | DBI_AUDITED | DBI_STALE)) !=
        DBI_VALID)
      continue;
    for (MDBX_txn *t = txn; t; t = t->mt_parent)
      if (F_ISSET(t->mt_dbistate[i], DBI_DIRTY | DBI_CREAT)) {
        used += t->mt_dbs[i].md_branch_pages + t->mt_dbs[i].md_leaf_pages +
                t->mt_dbs[i].md_overflow_pages;
        txn->mt_dbistate[i] |= DBI_AUDITED;
        break;
      }
    if (!(txn->mt_dbistate[i] & DBI_AUDITED)) {
      mdbx_warning("audit %s@%" PRIaTXN
                   ": unable account dbi %d / \"%*s\", state 0x%02x",
                   txn->mt_parent ? "nested-" : "", txn->mt_txnid, i,
                   (int)txn->mt_dbxs[i].md_name.iov_len,
                   (const char *)txn->mt_dbxs[i].md_name.iov_base,
                   txn->mt_dbistate[i]);
    }
  }

  if (pending + gc + used == txn->mt_next_pgno)
    return MDBX_SUCCESS;

  if ((txn->mt_flags & MDBX_TXN_RDONLY) == 0)
    mdbx_error("audit @%" PRIaTXN ": %u(pending) = %u(loose) + "
               "%u(reclaimed) + %u(retired-pending) - %u(retired-stored)",
               txn->mt_txnid, pending, txn->tw.loose_count,
               MDBX_PNL_SIZE(txn->tw.reclaimed_pglist),
               txn->tw.retired_pages ? MDBX_PNL_SIZE(txn->tw.retired_pages) : 0,
               retired_stored);
  mdbx_error("audit @%" PRIaTXN ": %" PRIaPGNO "(pending) + %" PRIaPGNO
             "(gc) + %" PRIaPGNO "(count) = %" PRIaPGNO "(total) <> %" PRIaPGNO
             "(allocated)",
             txn->mt_txnid, pending, gc, used, pending + gc + used,
             txn->mt_next_pgno);
  return MDBX_PROBLEM;
}

static __always_inline unsigned backlog_size(MDBX_txn *txn) {
  return MDBX_PNL_SIZE(txn->tw.reclaimed_pglist) + txn->tw.loose_count;
}

/* LY: Prepare a backlog of pages to modify GC itself,
 * while reclaiming is prohibited. It should be enough to prevent search
 * in mdbx_page_alloc() during a deleting, when GC tree is unbalanced. */
static int mdbx_prep_backlog(MDBX_txn *txn, MDBX_cursor *gc_cursor,
                             const size_t pnl_bytes, unsigned *retired_stored) {
  const unsigned linear4list = number_of_ovpages(txn->mt_env, pnl_bytes);
  const unsigned backlog4cow = txn->mt_dbs[FREE_DBI].md_depth;
  const unsigned backlog4rebalance = backlog4cow + 1;

  if (likely(linear4list == 1 &&
             backlog_size(txn) > (pnl_bytes
                                      ? backlog4rebalance
                                      : (backlog4cow + backlog4rebalance))))
    return MDBX_SUCCESS;

  mdbx_trace(">> pnl_bytes %zu, backlog %u, 4list %u, 4cow %u, 4rebalance %u",
             pnl_bytes, backlog_size(txn), linear4list, backlog4cow,
             backlog4rebalance);

  MDBX_val gc_key, fake_val;
  int err;
  if (linear4list < 2) {
    gc_key.iov_base = fake_val.iov_base = nullptr;
    gc_key.iov_len = sizeof(txnid_t);
    fake_val.iov_len = pnl_bytes;
    err = mdbx_cursor_spill(gc_cursor, &gc_key, &fake_val);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }

  gc_cursor->mc_flags &= ~C_RECLAIMING;
  err = mdbx_cursor_touch(gc_cursor);
  mdbx_trace("== after-touch, backlog %u, err %d", backlog_size(txn), err);

  if (linear4list > 1 && err == MDBX_SUCCESS) {
    if (retired_stored) {
      gc_key.iov_base = &txn->mt_txnid;
      gc_key.iov_len = sizeof(txn->mt_txnid);
      const struct cursor_set_result csr =
          mdbx_cursor_set(gc_cursor, &gc_key, &fake_val, MDBX_SET);
      if (csr.err == MDBX_SUCCESS && csr.exact) {
        *retired_stored = 0;
        err = mdbx_cursor_del(gc_cursor, 0);
        mdbx_trace("== clear-4linear, backlog %u, err %d", backlog_size(txn),
                   err);
      }
    }
    err =
        mdbx_page_alloc(gc_cursor, linear4list, MDBX_ALLOC_GC | MDBX_ALLOC_SLOT)
            .err;
    mdbx_trace("== after-4linear, backlog %u, err %d", backlog_size(txn), err);
    mdbx_cassert(gc_cursor,
                 backlog_size(txn) >= linear4list || err != MDBX_SUCCESS);
  }

  while (backlog_size(txn) < backlog4cow + linear4list && err == MDBX_SUCCESS)
    err = mdbx_page_alloc(gc_cursor, 1, MDBX_ALLOC_GC | MDBX_ALLOC_SLOT).err;

  gc_cursor->mc_flags |= C_RECLAIMING;
  mdbx_trace("<< backlog %u, err %d", backlog_size(txn), err);
  return (err != MDBX_NOTFOUND) ? err : MDBX_SUCCESS;
}

static __inline void clean_reserved_gc_pnl(MDBX_env *env, MDBX_val pnl) {
  /* PNL is initially empty, zero out at least the length */
  memset(pnl.iov_base, 0, sizeof(pgno_t));
  if ((env->me_flags & (MDBX_WRITEMAP | MDBX_NOMEMINIT)) == 0)
    /* zero out to avoid leaking values from uninitialized malloc'ed memory
     * to the file in non-writemap mode if length of the saving page-list
     * was changed during space reservation. */
    memset(pnl.iov_base, 0, pnl.iov_len);
}

/* Cleanup reclaimed GC records, than save the retired-list as of this
 * transaction to GC (aka freeDB). This recursive changes the reclaimed-list
 * loose-list and retired-list. Keep trying until it stabilizes. */
static int mdbx_update_gc(MDBX_txn *txn) {
  /* txn->tw.reclaimed_pglist[] can grow and shrink during this call.
   * txn->tw.last_reclaimed and txn->tw.retired_pages[] can only grow.
   * Page numbers cannot disappear from txn->tw.retired_pages[]. */
  MDBX_env *const env = txn->mt_env;
  const bool lifo = (env->me_flags & MDBX_LIFORECLAIM) != 0;
  const char *dbg_prefix_mode = lifo ? "    lifo" : "    fifo";
  (void)dbg_prefix_mode;
  mdbx_trace("\n>>> @%" PRIaTXN, txn->mt_txnid);

  unsigned retired_stored = 0, loop = 0;
  MDBX_cursor_couple couple;
  int rc = mdbx_cursor_init(&couple.outer, txn, FREE_DBI);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout_notracking;

  couple.outer.mc_flags |= C_RECLAIMING;
  couple.outer.mc_next = txn->tw.cursors[FREE_DBI];
  txn->tw.cursors[FREE_DBI] = &couple.outer;
  bool dense_gc = false;

retry:
  ++loop;
  mdbx_trace("%s", " >> restart");
  mdbx_tassert(txn,
               mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                     txn->mt_next_pgno - MDBX_ENABLE_REFUND));
  mdbx_tassert(txn, mdbx_dirtylist_check(txn));
  if (unlikely(/* paranoia */ loop > ((MDBX_DEBUG > 0) ? 12 : 42))) {
    mdbx_error("too more loops %u, bailout", loop);
    rc = MDBX_PROBLEM;
    goto bailout;
  }

  if (unlikely(dense_gc) && retired_stored) {
    rc = mdbx_prep_backlog(txn, &couple.outer,
                           MDBX_PNL_SIZEOF(txn->tw.retired_pages),
                           &retired_stored);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;
  }

  unsigned settled = 0, cleaned_gc_slot = 0, reused_gc_slot = 0,
           filled_gc_slot = ~0u;
  txnid_t cleaned_gc_id = 0, gc_rid = txn->tw.last_reclaimed;
  while (true) {
    /* Come back here after each Put() in case retired-list changed */
    MDBX_val key, data;
    mdbx_trace("%s", " >> continue");

    if (retired_stored != MDBX_PNL_SIZE(txn->tw.retired_pages) &&
        MDBX_PNL_SIZE(txn->tw.retired_pages) > env->me_maxgc_ov1page) {
      rc = mdbx_prep_backlog(txn, &couple.outer,
                             MDBX_PNL_SIZEOF(txn->tw.retired_pages),
                             &retired_stored);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
    }

    mdbx_tassert(txn,
                 mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                       txn->mt_next_pgno - MDBX_ENABLE_REFUND));
    if (lifo) {
      if (cleaned_gc_slot < (txn->tw.lifo_reclaimed
                                 ? MDBX_PNL_SIZE(txn->tw.lifo_reclaimed)
                                 : 0)) {
        settled = 0;
        cleaned_gc_slot = 0;
        reused_gc_slot = 0;
        filled_gc_slot = ~0u;
        /* LY: cleanup reclaimed records. */
        do {
          cleaned_gc_id = txn->tw.lifo_reclaimed[++cleaned_gc_slot];
          mdbx_tassert(txn,
                       cleaned_gc_slot > 0 &&
                           cleaned_gc_id < env->me_lck->mti_oldest_reader.weak);
          key.iov_base = &cleaned_gc_id;
          key.iov_len = sizeof(cleaned_gc_id);
          rc = mdbx_cursor_get(&couple.outer, &key, NULL, MDBX_SET);
          if (rc == MDBX_NOTFOUND)
            continue;
          if (unlikely(rc != MDBX_SUCCESS))
            goto bailout;
          if (likely(!dense_gc)) {
            rc = mdbx_prep_backlog(txn, &couple.outer, 0, nullptr);
            if (unlikely(rc != MDBX_SUCCESS))
              goto bailout;
          }
          mdbx_tassert(txn,
                       cleaned_gc_id < env->me_lck->mti_oldest_reader.weak);
          mdbx_trace("%s: cleanup-reclaimed-id [%u]%" PRIaTXN, dbg_prefix_mode,
                     cleaned_gc_slot, cleaned_gc_id);
          mdbx_tassert(txn, *txn->tw.cursors == &couple.outer);
          rc = mdbx_cursor_del(&couple.outer, 0);
          if (unlikely(rc != MDBX_SUCCESS))
            goto bailout;
        } while (cleaned_gc_slot < MDBX_PNL_SIZE(txn->tw.lifo_reclaimed));
        mdbx_txl_sort(txn->tw.lifo_reclaimed);
      }
    } else {
      /* If using records from GC which we have not yet deleted,
       * now delete them and any we reserved for tw.reclaimed_pglist. */
      while (cleaned_gc_id <= txn->tw.last_reclaimed) {
        rc = mdbx_cursor_first(&couple.outer, &key, NULL);
        if (unlikely(rc != MDBX_SUCCESS)) {
          if (rc == MDBX_NOTFOUND)
            break;
          goto bailout;
        }
        if (!MDBX_DISABLE_PAGECHECKS &&
            unlikely(key.iov_len != sizeof(txnid_t))) {
          rc = MDBX_CORRUPTED;
          goto bailout;
        }
        gc_rid = cleaned_gc_id;
        settled = 0;
        reused_gc_slot = 0;
        cleaned_gc_id = unaligned_peek_u64(4, key.iov_base);
        if (!MDBX_DISABLE_PAGECHECKS &&
            unlikely(cleaned_gc_id < MIN_TXNID || cleaned_gc_id > MAX_TXNID)) {
          rc = MDBX_CORRUPTED;
          goto bailout;
        }
        if (cleaned_gc_id > txn->tw.last_reclaimed)
          break;
        if (likely(!dense_gc) && cleaned_gc_id < txn->tw.last_reclaimed) {
          rc = mdbx_prep_backlog(txn, &couple.outer, 0, nullptr);
          if (unlikely(rc != MDBX_SUCCESS))
            goto bailout;
        }
        mdbx_tassert(txn, cleaned_gc_id <= txn->tw.last_reclaimed);
        mdbx_tassert(txn, cleaned_gc_id < env->me_lck->mti_oldest_reader.weak);
        mdbx_trace("%s: cleanup-reclaimed-id %" PRIaTXN, dbg_prefix_mode,
                   cleaned_gc_id);
        mdbx_tassert(txn, *txn->tw.cursors == &couple.outer);
        rc = mdbx_cursor_del(&couple.outer, 0);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      }
    }

    mdbx_tassert(txn,
                 mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                       txn->mt_next_pgno - MDBX_ENABLE_REFUND));
    mdbx_tassert(txn, mdbx_dirtylist_check(txn));
    if (mdbx_audit_enabled()) {
      rc = mdbx_audit_ex(txn, retired_stored, false);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
    }

    /* return suitable into unallocated space */
    if (mdbx_refund(txn)) {
      mdbx_tassert(
          txn, mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                     txn->mt_next_pgno - MDBX_ENABLE_REFUND));
      if (mdbx_audit_enabled()) {
        rc = mdbx_audit_ex(txn, retired_stored, false);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      }
    }

    /* handle loose pages - put ones into the reclaimed- or retired-list */
    if (txn->tw.loose_pages) {
      /* Return loose page numbers to tw.reclaimed_pglist,
       * though usually none are left at this point.
       * The pages themselves remain in dirtylist. */
      if (unlikely(!txn->tw.lifo_reclaimed && txn->tw.last_reclaimed < 1)) {
        if (txn->tw.loose_count > 0) {
          /* Put loose page numbers in tw.retired_pages,
           * since unable to return them to tw.reclaimed_pglist. */
          if (unlikely((rc = mdbx_pnl_need(&txn->tw.retired_pages,
                                           txn->tw.loose_count)) != 0))
            goto bailout;
          for (MDBX_page *mp = txn->tw.loose_pages; mp; mp = mp->mp_next)
            mdbx_pnl_xappend(txn->tw.retired_pages, mp->mp_pgno);
          mdbx_trace("%s: append %u loose-pages to retired-pages",
                     dbg_prefix_mode, txn->tw.loose_count);
        }
      } else {
        /* Room for loose pages + temp PNL with same */
        rc = mdbx_pnl_need(&txn->tw.reclaimed_pglist,
                           2 * txn->tw.loose_count + 2);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
        MDBX_PNL loose = txn->tw.reclaimed_pglist +
                         MDBX_PNL_ALLOCLEN(txn->tw.reclaimed_pglist) -
                         txn->tw.loose_count - 1;
        unsigned count = 0;
        for (MDBX_page *mp = txn->tw.loose_pages; mp; mp = mp->mp_next) {
          mdbx_tassert(txn, mp->mp_flags == P_LOOSE);
          loose[++count] = mp->mp_pgno;
        }
        mdbx_tassert(txn, count == txn->tw.loose_count);
        MDBX_PNL_SIZE(loose) = count;
        mdbx_pnl_sort(loose, txn->mt_next_pgno);
        mdbx_pnl_xmerge(txn->tw.reclaimed_pglist, loose);
        mdbx_trace("%s: append %u loose-pages to reclaimed-pages",
                   dbg_prefix_mode, txn->tw.loose_count);
      }

      /* filter-out list of dirty-pages from loose-pages */
      MDBX_dpl *const dl = txn->tw.dirtylist;
      unsigned w = 0;
      for (unsigned r = w; ++r <= dl->length;) {
        MDBX_page *dp = dl->items[r].ptr;
        mdbx_tassert(txn, dp->mp_flags == P_LOOSE || IS_MODIFIABLE(txn, dp));
        mdbx_tassert(txn, dpl_endpgno(dl, r) <= txn->mt_next_pgno);
        if ((dp->mp_flags & P_LOOSE) == 0) {
          if (++w != r)
            dl->items[w] = dl->items[r];
        } else {
          mdbx_tassert(txn, dp->mp_flags == P_LOOSE);
          if ((env->me_flags & MDBX_WRITEMAP) == 0)
            mdbx_dpage_free(env, dp, 1);
        }
      }
      mdbx_trace("%s: filtered-out loose-pages from %u -> %u dirty-pages",
                 dbg_prefix_mode, dl->length, w);
      mdbx_tassert(txn, txn->tw.loose_count == dl->length - w);
      dpl_setlen(dl, w);
      dl->sorted = 0;
      txn->tw.dirtyroom += txn->tw.loose_count;
      mdbx_tassert(txn,
                   txn->tw.dirtyroom + txn->tw.dirtylist->length ==
                       (txn->mt_parent ? txn->mt_parent->tw.dirtyroom
                                       : txn->mt_env->me_options.dp_limit));
      txn->tw.loose_pages = NULL;
      txn->tw.loose_count = 0;
#if MDBX_ENABLE_REFUND
      txn->tw.loose_refund_wl = 0;
#endif /* MDBX_ENABLE_REFUND */
    }

    const unsigned amount = (unsigned)MDBX_PNL_SIZE(txn->tw.reclaimed_pglist);
    /* handle retired-list - store ones into single gc-record */
    if (retired_stored < MDBX_PNL_SIZE(txn->tw.retired_pages)) {
      if (unlikely(!retired_stored)) {
        /* Make sure last page of GC is touched and on retired-list */
        couple.outer.mc_flags &= ~C_RECLAIMING;
        rc = mdbx_page_search(&couple.outer, NULL,
                              MDBX_PS_LAST | MDBX_PS_MODIFY);
        couple.outer.mc_flags |= C_RECLAIMING;
        if (unlikely(rc != MDBX_SUCCESS) && rc != MDBX_NOTFOUND)
          goto bailout;
      }
      /* Write to last page of GC */
      key.iov_len = sizeof(txn->mt_txnid);
      key.iov_base = &txn->mt_txnid;
      do {
        data.iov_len = MDBX_PNL_SIZEOF(txn->tw.retired_pages);
        mdbx_prep_backlog(txn, &couple.outer, data.iov_len, &retired_stored);
        rc = mdbx_cursor_put(&couple.outer, &key, &data, MDBX_RESERVE);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
        /* Retry if tw.retired_pages[] grew during the Put() */
      } while (data.iov_len < MDBX_PNL_SIZEOF(txn->tw.retired_pages));

      retired_stored = (unsigned)MDBX_PNL_SIZE(txn->tw.retired_pages);
      mdbx_pnl_sort(txn->tw.retired_pages, txn->mt_next_pgno);
      mdbx_assert(env, data.iov_len == MDBX_PNL_SIZEOF(txn->tw.retired_pages));
      memcpy(data.iov_base, txn->tw.retired_pages, data.iov_len);

      mdbx_trace("%s: put-retired #%u @ %" PRIaTXN, dbg_prefix_mode,
                 retired_stored, txn->mt_txnid);

      if (mdbx_log_enabled(MDBX_LOG_EXTRA)) {
        unsigned i = retired_stored;
        mdbx_debug_extra("PNL write txn %" PRIaTXN " root %" PRIaPGNO
                         " num %u, PNL",
                         txn->mt_txnid, txn->mt_dbs[FREE_DBI].md_root, i);
        for (; i; i--)
          mdbx_debug_extra_print(" %" PRIaPGNO, txn->tw.retired_pages[i]);
        mdbx_debug_extra_print("%s\n", ".");
      }
      if (unlikely(amount != MDBX_PNL_SIZE(txn->tw.reclaimed_pglist) &&
                   settled)) {
        mdbx_trace("%s: reclaimed-list changed %u -> %u, retry",
                   dbg_prefix_mode, amount,
                   (unsigned)MDBX_PNL_SIZE(txn->tw.reclaimed_pglist));
        goto retry /* rare case, but avoids GC fragmentation
                                and one cycle. */
            ;
      }
      continue;
    }

    /* handle reclaimed and lost pages - merge and store both into gc */
    mdbx_tassert(txn,
                 mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                       txn->mt_next_pgno - MDBX_ENABLE_REFUND));
    mdbx_tassert(txn, txn->tw.loose_count == 0);

    mdbx_trace("%s", " >> reserving");
    if (mdbx_audit_enabled()) {
      rc = mdbx_audit_ex(txn, retired_stored, false);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
    }
    const unsigned left = amount - settled;
    mdbx_trace("%s: amount %u, settled %d, left %d, lifo-reclaimed-slots %u, "
               "reused-gc-slots %u",
               dbg_prefix_mode, amount, settled, (int)left,
               txn->tw.lifo_reclaimed
                   ? (unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed)
                   : 0,
               reused_gc_slot);
    if (0 >= (int)left)
      break;

    const unsigned prefer_max_scatter = 257;
    txnid_t reservation_gc_id;
    if (lifo) {
      if (txn->tw.lifo_reclaimed == nullptr) {
        txn->tw.lifo_reclaimed = mdbx_txl_alloc();
        if (unlikely(!txn->tw.lifo_reclaimed)) {
          rc = MDBX_ENOMEM;
          goto bailout;
        }
      }
      if ((unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) <
              prefer_max_scatter &&
          left > ((unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) -
                  reused_gc_slot) *
                     env->me_maxgc_ov1page &&
          !dense_gc) {
        /* LY: need just a txn-id for save page list. */
        bool need_cleanup = false;
        txnid_t snap_oldest;
      retry_rid:
        couple.outer.mc_flags &= ~C_RECLAIMING;
        do {
          snap_oldest = mdbx_find_oldest(txn);
          rc =
              mdbx_page_alloc(&couple.outer, 0, MDBX_ALLOC_GC | MDBX_ALLOC_SLOT)
                  .err;
          if (likely(rc == MDBX_SUCCESS)) {
            mdbx_trace("%s: took @%" PRIaTXN " from GC", dbg_prefix_mode,
                       MDBX_PNL_LAST(txn->tw.lifo_reclaimed));
            need_cleanup = true;
          }
        } while (rc == MDBX_SUCCESS &&
                 (unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) <
                     prefer_max_scatter &&
                 left > ((unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) -
                         reused_gc_slot) *
                            env->me_maxgc_ov1page);
        couple.outer.mc_flags |= C_RECLAIMING;

        if (likely(rc == MDBX_SUCCESS)) {
          mdbx_trace("%s: got enough from GC.", dbg_prefix_mode);
          continue;
        } else if (unlikely(rc != MDBX_NOTFOUND))
          /* LY: some troubles... */
          goto bailout;

        if (MDBX_PNL_SIZE(txn->tw.lifo_reclaimed)) {
          if (need_cleanup) {
            mdbx_txl_sort(txn->tw.lifo_reclaimed);
            cleaned_gc_slot = 0;
          }
          gc_rid = MDBX_PNL_LAST(txn->tw.lifo_reclaimed);
        } else {
          mdbx_tassert(txn, txn->tw.last_reclaimed == 0);
          if (unlikely(mdbx_find_oldest(txn) != snap_oldest))
            /* should retry mdbx_page_alloc(MDBX_ALLOC_GC)
             * if the oldest reader changes since the last attempt */
            goto retry_rid;
          /* no reclaimable GC entries,
           * therefore no entries with ID < mdbx_find_oldest(txn) */
          txn->tw.last_reclaimed = gc_rid = snap_oldest - 1;
          mdbx_trace("%s: none recycled yet, set rid to @%" PRIaTXN,
                     dbg_prefix_mode, gc_rid);
        }

        /* LY: GC is empty, will look any free txn-id in high2low order. */
        while (MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) < prefer_max_scatter &&
               left > ((unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) -
                       reused_gc_slot) *
                          env->me_maxgc_ov1page) {
          if (unlikely(gc_rid <= MIN_TXNID)) {
            if (unlikely(MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) <=
                         reused_gc_slot)) {
              mdbx_notice("** restart: reserve depleted (reused_gc_slot %u >= "
                          "lifo_reclaimed %u" PRIaTXN,
                          reused_gc_slot,
                          (unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed));
              goto retry;
            }
            break;
          }

          mdbx_tassert(txn, gc_rid >= MIN_TXNID && gc_rid <= MAX_TXNID);
          --gc_rid;
          key.iov_base = &gc_rid;
          key.iov_len = sizeof(gc_rid);
          rc = mdbx_cursor_get(&couple.outer, &key, &data, MDBX_SET_KEY);
          if (unlikely(rc == MDBX_SUCCESS)) {
            mdbx_debug("%s: GC's id %" PRIaTXN
                       " is used, continue bottom-up search",
                       dbg_prefix_mode, gc_rid);
            ++gc_rid;
            rc = mdbx_cursor_get(&couple.outer, &key, &data, MDBX_FIRST);
            if (rc == MDBX_NOTFOUND) {
              mdbx_debug("%s: GC is empty (going dense-mode)", dbg_prefix_mode);
              dense_gc = true;
              break;
            }
            if (unlikely(rc != MDBX_SUCCESS ||
                         key.iov_len != sizeof(mdbx_tid_t))) {
              rc = MDBX_CORRUPTED;
              goto bailout;
            }
            txnid_t gc_first = unaligned_peek_u64(4, key.iov_base);
            if (!MDBX_DISABLE_PAGECHECKS &&
                unlikely(gc_first < MIN_TXNID || gc_first > MAX_TXNID)) {
              rc = MDBX_CORRUPTED;
              goto bailout;
            }
            if (gc_first <= MIN_TXNID) {
              mdbx_debug("%s: no free GC's id(s) less than %" PRIaTXN
                         " (going dense-mode)",
                         dbg_prefix_mode, gc_rid);
              dense_gc = true;
              break;
            }
            gc_rid = gc_first - 1;
          }

          mdbx_assert(env, !dense_gc);
          rc = mdbx_txl_append(&txn->tw.lifo_reclaimed, gc_rid);
          if (unlikely(rc != MDBX_SUCCESS))
            goto bailout;

          if (reused_gc_slot)
            /* rare case, but it is better to clear and re-create GC entries
             * with less fragmentation. */
            need_cleanup = true;
          else
            cleaned_gc_slot +=
                1 /* mark cleanup is not needed for added slot. */;

          mdbx_trace("%s: append @%" PRIaTXN
                     " to lifo-reclaimed, cleaned-gc-slot = %u",
                     dbg_prefix_mode, gc_rid, cleaned_gc_slot);
        }

        if (need_cleanup || dense_gc) {
          if (cleaned_gc_slot)
            mdbx_trace(
                "%s: restart inner-loop to clear and re-create GC entries",
                dbg_prefix_mode);
          cleaned_gc_slot = 0;
          continue;
        }
      }

      const unsigned i =
          (unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) - reused_gc_slot;
      mdbx_tassert(txn, i > 0 && i <= MDBX_PNL_SIZE(txn->tw.lifo_reclaimed));
      reservation_gc_id = txn->tw.lifo_reclaimed[i];
      mdbx_trace("%s: take @%" PRIaTXN " from lifo-reclaimed[%u]",
                 dbg_prefix_mode, reservation_gc_id, i);
    } else {
      mdbx_tassert(txn, txn->tw.lifo_reclaimed == NULL);
      if (unlikely(gc_rid == 0)) {
        gc_rid = mdbx_find_oldest(txn) - 1;
        rc = mdbx_cursor_get(&couple.outer, &key, NULL, MDBX_FIRST);
        if (rc == MDBX_SUCCESS) {
          if (!MDBX_DISABLE_PAGECHECKS &&
              unlikely(key.iov_len != sizeof(txnid_t))) {
            rc = MDBX_CORRUPTED;
            goto bailout;
          }
          txnid_t gc_first = unaligned_peek_u64(4, key.iov_base);
          if (!MDBX_DISABLE_PAGECHECKS &&
              unlikely(gc_first < MIN_TXNID || gc_first > MAX_TXNID)) {
            rc = MDBX_CORRUPTED;
            goto bailout;
          }
          if (gc_rid >= gc_first)
            gc_rid = gc_first - 1;
          if (unlikely(gc_rid == 0)) {
            mdbx_error("%s", "** no GC tail-space to store (going dense-mode)");
            dense_gc = true;
            goto retry;
          }
        } else if (rc != MDBX_NOTFOUND)
          goto bailout;
        txn->tw.last_reclaimed = gc_rid;
        cleaned_gc_id = gc_rid + 1;
      }
      reservation_gc_id = gc_rid--;
      mdbx_trace("%s: take @%" PRIaTXN " from head-gc-id", dbg_prefix_mode,
                 reservation_gc_id);
    }
    ++reused_gc_slot;

    unsigned chunk = left;
    if (unlikely(chunk > env->me_maxgc_ov1page)) {
      const unsigned avail_gc_slots =
          txn->tw.lifo_reclaimed
              ? (unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) -
                    reused_gc_slot + 1
          : (gc_rid < INT16_MAX) ? (unsigned)gc_rid
                                 : INT16_MAX;
      if (avail_gc_slots > 1) {
        if (chunk < env->me_maxgc_ov1page * 2)
          chunk /= 2;
        else {
          const unsigned threshold =
              env->me_maxgc_ov1page * ((avail_gc_slots < prefer_max_scatter)
                                           ? avail_gc_slots
                                           : prefer_max_scatter);
          if (left < threshold)
            chunk = env->me_maxgc_ov1page;
          else {
            const unsigned tail = left - threshold + env->me_maxgc_ov1page + 1;
            unsigned span = 1;
            unsigned avail = (unsigned)((pgno2bytes(env, span) - PAGEHDRSZ) /
                                        sizeof(pgno_t)) /* - 1 + span */;
            if (tail > avail) {
              for (unsigned i = amount - span; i > 0; --i) {
                if (MDBX_PNL_ASCENDING
                        ? (txn->tw.reclaimed_pglist[i] + span)
                        : (txn->tw.reclaimed_pglist[i] - span) ==
                              txn->tw.reclaimed_pglist[i + span]) {
                  span += 1;
                  avail = (unsigned)((pgno2bytes(env, span) - PAGEHDRSZ) /
                                     sizeof(pgno_t)) -
                          1 + span;
                  if (avail >= tail)
                    break;
                }
              }
            }

            chunk = (avail >= tail) ? tail - span
                    : (avail_gc_slots > 3 &&
                       reused_gc_slot < prefer_max_scatter - 3)
                        ? avail - span
                        : tail;
          }
        }
      }
    }
    mdbx_tassert(txn, chunk > 0);

    mdbx_trace("%s: gc_rid %" PRIaTXN ", reused_gc_slot %u, reservation-id "
               "%" PRIaTXN,
               dbg_prefix_mode, gc_rid, reused_gc_slot, reservation_gc_id);

    mdbx_trace("%s: chunk %u, gc-per-ovpage %u", dbg_prefix_mode, chunk,
               env->me_maxgc_ov1page);

    mdbx_tassert(txn, reservation_gc_id < env->me_lck->mti_oldest_reader.weak);
    if (unlikely(
            reservation_gc_id < MIN_TXNID ||
            reservation_gc_id >=
                atomic_load64(&env->me_lck->mti_oldest_reader, mo_Relaxed))) {
      mdbx_error("** internal error (reservation_gc_id %" PRIaTXN ")",
                 reservation_gc_id);
      rc = MDBX_PROBLEM;
      goto bailout;
    }

    key.iov_len = sizeof(reservation_gc_id);
    key.iov_base = &reservation_gc_id;
    data.iov_len = (chunk + 1) * sizeof(pgno_t);
    mdbx_trace("%s: reserve %u [%u...%u) @%" PRIaTXN, dbg_prefix_mode, chunk,
               settled + 1, settled + chunk + 1, reservation_gc_id);
    mdbx_prep_backlog(txn, &couple.outer, data.iov_len, nullptr);
    rc = mdbx_cursor_put(&couple.outer, &key, &data,
                         MDBX_RESERVE | MDBX_NOOVERWRITE);
    mdbx_tassert(txn,
                 mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                       txn->mt_next_pgno - MDBX_ENABLE_REFUND));
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;

    clean_reserved_gc_pnl(env, data);
    settled += chunk;
    mdbx_trace("%s: settled %u (+%u), continue", dbg_prefix_mode, settled,
               chunk);

    if (txn->tw.lifo_reclaimed &&
        unlikely(amount < MDBX_PNL_SIZE(txn->tw.reclaimed_pglist)) &&
        (loop < 5 || MDBX_PNL_SIZE(txn->tw.reclaimed_pglist) - amount >
                         env->me_maxgc_ov1page)) {
      mdbx_notice("** restart: reclaimed-list growth %u -> %u", amount,
                  (unsigned)MDBX_PNL_SIZE(txn->tw.reclaimed_pglist));
      goto retry;
    }

    continue;
  }

  mdbx_tassert(
      txn,
      cleaned_gc_slot ==
          (txn->tw.lifo_reclaimed ? MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) : 0));

  mdbx_trace("%s", " >> filling");
  /* Fill in the reserved records */
  filled_gc_slot =
      txn->tw.lifo_reclaimed
          ? (unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed) - reused_gc_slot
          : reused_gc_slot;
  rc = MDBX_SUCCESS;
  mdbx_tassert(txn,
               mdbx_pnl_check4assert(txn->tw.reclaimed_pglist,
                                     txn->mt_next_pgno - MDBX_ENABLE_REFUND));
  mdbx_tassert(txn, mdbx_dirtylist_check(txn));
  if (MDBX_PNL_SIZE(txn->tw.reclaimed_pglist)) {
    MDBX_val key, data;
    key.iov_len = data.iov_len = 0; /* avoid MSVC warning */
    key.iov_base = data.iov_base = NULL;

    const unsigned amount = MDBX_PNL_SIZE(txn->tw.reclaimed_pglist);
    unsigned left = amount;
    if (txn->tw.lifo_reclaimed == nullptr) {
      mdbx_tassert(txn, lifo == 0);
      rc = mdbx_cursor_first(&couple.outer, &key, &data);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
    } else {
      mdbx_tassert(txn, lifo != 0);
    }

    while (true) {
      txnid_t fill_gc_id;
      mdbx_trace("%s: left %u of %u", dbg_prefix_mode, left,
                 (unsigned)MDBX_PNL_SIZE(txn->tw.reclaimed_pglist));
      if (txn->tw.lifo_reclaimed == nullptr) {
        mdbx_tassert(txn, lifo == 0);
        fill_gc_id = unaligned_peek_u64(4, key.iov_base);
        if (filled_gc_slot-- == 0 || fill_gc_id > txn->tw.last_reclaimed) {
          mdbx_notice(
              "** restart: reserve depleted (filled_slot %u, fill_id %" PRIaTXN
              " > last_reclaimed %" PRIaTXN,
              filled_gc_slot, fill_gc_id, txn->tw.last_reclaimed);
          goto retry;
        }
      } else {
        mdbx_tassert(txn, lifo != 0);
        if (++filled_gc_slot >
            (unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed)) {
          mdbx_notice("** restart: reserve depleted (filled_gc_slot %u > "
                      "lifo_reclaimed %u" PRIaTXN,
                      filled_gc_slot,
                      (unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed));
          goto retry;
        }
        fill_gc_id = txn->tw.lifo_reclaimed[filled_gc_slot];
        mdbx_trace("%s: seek-reservation @%" PRIaTXN " at lifo_reclaimed[%u]",
                   dbg_prefix_mode, fill_gc_id, filled_gc_slot);
        key.iov_base = &fill_gc_id;
        key.iov_len = sizeof(fill_gc_id);
        rc = mdbx_cursor_get(&couple.outer, &key, &data, MDBX_SET_KEY);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      }
      mdbx_tassert(txn, cleaned_gc_slot ==
                            (txn->tw.lifo_reclaimed
                                 ? MDBX_PNL_SIZE(txn->tw.lifo_reclaimed)
                                 : 0));
      mdbx_tassert(txn, fill_gc_id > 0 &&
                            fill_gc_id < env->me_lck->mti_oldest_reader.weak);
      key.iov_base = &fill_gc_id;
      key.iov_len = sizeof(fill_gc_id);

      mdbx_tassert(txn, data.iov_len >= sizeof(pgno_t) * 2);
      couple.outer.mc_flags |= C_GCFREEZE;
      unsigned chunk = (unsigned)(data.iov_len / sizeof(pgno_t)) - 1;
      if (unlikely(chunk > left)) {
        mdbx_trace("%s: chunk %u > left %u, @%" PRIaTXN, dbg_prefix_mode, chunk,
                   left, fill_gc_id);
        if ((loop < 5 && chunk - left > loop / 2) ||
            chunk - left > env->me_maxgc_ov1page) {
          data.iov_len = (left + 1) * sizeof(pgno_t);
          if (loop < 7)
            couple.outer.mc_flags &= ~C_GCFREEZE;
        }
        chunk = left;
      }
      rc = mdbx_cursor_put(&couple.outer, &key, &data,
                           MDBX_CURRENT | MDBX_RESERVE);
      couple.outer.mc_flags &= ~C_GCFREEZE;
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
      clean_reserved_gc_pnl(env, data);

      if (unlikely(txn->tw.loose_count ||
                   amount != MDBX_PNL_SIZE(txn->tw.reclaimed_pglist))) {
        mdbx_notice("** restart: reclaimed-list growth (%u -> %u, loose +%u)",
                    amount, MDBX_PNL_SIZE(txn->tw.reclaimed_pglist),
                    txn->tw.loose_count);
        goto retry;
      }
      if (unlikely(txn->tw.lifo_reclaimed
                       ? cleaned_gc_slot < MDBX_PNL_SIZE(txn->tw.lifo_reclaimed)
                       : cleaned_gc_id < txn->tw.last_reclaimed)) {
        mdbx_notice("%s", "** restart: reclaimed-slots changed");
        goto retry;
      }
      if (unlikely(retired_stored != MDBX_PNL_SIZE(txn->tw.retired_pages))) {
        mdbx_tassert(txn,
                     retired_stored < MDBX_PNL_SIZE(txn->tw.retired_pages));
        mdbx_notice("** restart: retired-list growth (%u -> %u)",
                    retired_stored, MDBX_PNL_SIZE(txn->tw.retired_pages));
        goto retry;
      }

      pgno_t *dst = data.iov_base;
      *dst++ = chunk;
      pgno_t *src = MDBX_PNL_BEGIN(txn->tw.reclaimed_pglist) + left - chunk;
      memcpy(dst, src, chunk * sizeof(pgno_t));
      pgno_t *from = src, *to = src + chunk;
      mdbx_trace("%s: fill %u [ %u:%" PRIaPGNO "...%u:%" PRIaPGNO
                 "] @%" PRIaTXN,
                 dbg_prefix_mode, chunk,
                 (unsigned)(from - txn->tw.reclaimed_pglist), from[0],
                 (unsigned)(to - txn->tw.reclaimed_pglist), to[-1], fill_gc_id);

      left -= chunk;
      if (mdbx_audit_enabled()) {
        rc = mdbx_audit_ex(txn, retired_stored + amount - left, true);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      }
      if (left == 0) {
        rc = MDBX_SUCCESS;
        break;
      }

      if (txn->tw.lifo_reclaimed == nullptr) {
        mdbx_tassert(txn, lifo == 0);
        rc = mdbx_cursor_next(&couple.outer, &key, &data, MDBX_NEXT);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      } else {
        mdbx_tassert(txn, lifo != 0);
      }
    }
  }

  mdbx_tassert(txn, rc == MDBX_SUCCESS);
  if (unlikely(txn->tw.loose_count != 0)) {
    mdbx_notice("** restart: got %u loose pages", txn->tw.loose_count);
    goto retry;
  }
  if (unlikely(filled_gc_slot !=
               (txn->tw.lifo_reclaimed
                    ? (unsigned)MDBX_PNL_SIZE(txn->tw.lifo_reclaimed)
                    : 0))) {

    const bool will_retry = loop < 9;
    mdbx_notice("** %s: reserve excess (filled-slot %u, loop %u)",
                will_retry ? "restart" : "ignore", filled_gc_slot, loop);
    if (will_retry)
      goto retry;
  }

  mdbx_tassert(txn,
               txn->tw.lifo_reclaimed == NULL ||
                   cleaned_gc_slot == MDBX_PNL_SIZE(txn->tw.lifo_reclaimed));

bailout:
  txn->tw.cursors[FREE_DBI] = couple.outer.mc_next;

bailout_notracking:
  MDBX_PNL_SIZE(txn->tw.reclaimed_pglist) = 0;
  mdbx_trace("<<< %u loops, rc = %d", loop, rc);
  return rc;
}

static int mdbx_txn_write(MDBX_txn *txn, struct mdbx_iov_ctx *ctx) {
  MDBX_dpl *const dl =
      (txn->mt_flags & MDBX_WRITEMAP) ? txn->tw.dirtylist : mdbx_dpl_sort(txn);
  int rc = MDBX_SUCCESS;
  unsigned r, w;
  for (w = 0, r = 1; r <= dl->length; ++r) {
    MDBX_page *dp = dl->items[r].ptr;
    if (dp->mp_flags & P_LOOSE) {
      dl->items[++w] = dl->items[r];
      continue;
    }
    unsigned npages = dpl_npages(dl, r);
    rc = iov_page(txn, ctx, dp, npages);
    if (unlikely(rc != MDBX_SUCCESS))
      break;
  }

  if (ctx->iov_items)
    rc = mdbx_iov_write(txn, ctx);

  while (r <= dl->length)
    dl->items[++w] = dl->items[r++];

  dl->sorted = dpl_setlen(dl, w);
  txn->tw.dirtyroom += r - 1 - w;
  mdbx_tassert(txn, txn->tw.dirtyroom + txn->tw.dirtylist->length ==
                        (txn->mt_parent ? txn->mt_parent->tw.dirtyroom
                                        : txn->mt_env->me_options.dp_limit));
  return rc;
}

/* Check txn and dbi arguments to a function */
static __always_inline bool check_dbi(MDBX_txn *txn, MDBX_dbi dbi,
                                      unsigned validity) {
  if (likely(dbi < txn->mt_numdbs))
    return likely((txn->mt_dbistate[dbi] & validity) &&
                  !TXN_DBI_CHANGED(txn, dbi) &&
                  (txn->mt_dbxs[dbi].md_name.iov_base || dbi < CORE_DBS));

  return dbi_import(txn, dbi);
}

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
int mdbx_txn_commit(MDBX_txn *txn) { return __inline_mdbx_txn_commit(txn); }
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

/* Merge child txn into parent */
static __inline void mdbx_txn_merge(MDBX_txn *const parent, MDBX_txn *const txn,
                                    const unsigned parent_retired_len) {
  MDBX_dpl *const src = mdbx_dpl_sort(txn);

  /* Remove refunded pages from parent's dirty list */
  MDBX_dpl *const dst = mdbx_dpl_sort(parent);
  if (MDBX_ENABLE_REFUND) {
    unsigned n = dst->length;
    while (n && dst->items[n].pgno >= parent->mt_next_pgno) {
      if (!(txn->mt_env->me_flags & MDBX_WRITEMAP)) {
        MDBX_page *dp = dst->items[n].ptr;
        mdbx_dpage_free(txn->mt_env, dp, dpl_npages(dst, n));
      }
      --n;
    }
    parent->tw.dirtyroom += dst->sorted - n;
    dst->sorted = dpl_setlen(dst, n);
    mdbx_tassert(parent,
                 parent->tw.dirtyroom + parent->tw.dirtylist->length ==
                     (parent->mt_parent ? parent->mt_parent->tw.dirtyroom
                                        : parent->mt_env->me_options.dp_limit));
  }

  /* Remove reclaimed pages from parent's dirty list */
  const MDBX_PNL reclaimed_list = parent->tw.reclaimed_pglist;
  mdbx_dpl_sift(parent, reclaimed_list, false);

  /* Move retired pages from parent's dirty & spilled list to reclaimed */
  unsigned r, w, d, s, l;
  for (r = w = parent_retired_len;
       ++r <= MDBX_PNL_SIZE(parent->tw.retired_pages);) {
    const pgno_t pgno = parent->tw.retired_pages[r];
    const unsigned di = mdbx_dpl_exist(parent, pgno);
    const unsigned si = (!di && unlikely(parent->tw.spill_pages))
                            ? mdbx_pnl_exist(parent->tw.spill_pages, pgno << 1)
                            : 0;
    unsigned npages;
    const char *kind;
    if (di) {
      MDBX_page *dp = dst->items[di].ptr;
      mdbx_tassert(parent, (dp->mp_flags & ~(P_LEAF | P_LEAF2 | P_BRANCH |
                                             P_OVERFLOW | P_SPILLED)) == 0);
      npages = dpl_npages(dst, di);
      mdbx_page_wash(parent, di, dp, npages);
      kind = "dirty";
      l = 1;
      if (unlikely(npages > l)) {
        /* OVERFLOW-страница могла быть переиспользована по частям. Тогда
         * в retired-списке может быть только начало последовательности,
         * а остаток растащен по dirty, spilled и reclaimed спискам. Поэтому
         * переносим в reclaimed с проверкой на обрыв последовательности.
         * В любом случае, все осколки будут учтены и отфильтрованы, т.е. если
         * страница была разбита на части, то важно удалить dirty-элемент,
         * а все осколки будут учтены отдельно. */

        /* Список retired страниц не сортирован, но для ускорения сортировки
         * дополняется в соответствии с MDBX_PNL_ASCENDING */
#if MDBX_PNL_ASCENDING
        const unsigned len = MDBX_PNL_SIZE(parent->tw.retired_pages);
        while (r < len && parent->tw.retired_pages[r + 1] == pgno + l) {
          ++r;
          if (++l == npages)
            break;
        }
#else
        while (w > parent_retired_len &&
               parent->tw.retired_pages[w - 1] == pgno + l) {
          --w;
          if (++l == npages)
            break;
        }
#endif
      }
    } else if (unlikely(si)) {
      l = npages = 1;
      mdbx_spill_remove(parent, si, 1);
      kind = "spilled";
    } else {
      parent->tw.retired_pages[++w] = pgno;
      continue;
    }

    mdbx_debug("reclaim retired parent's %u->%u %s page %" PRIaPGNO, npages, l,
               kind, pgno);
    int err = mdbx_pnl_insert_range(&parent->tw.reclaimed_pglist, pgno, l);
    mdbx_ensure(txn->mt_env, err == MDBX_SUCCESS);
  }
  MDBX_PNL_SIZE(parent->tw.retired_pages) = w;

  /* Filter-out parent spill list */
  if (parent->tw.spill_pages && MDBX_PNL_SIZE(parent->tw.spill_pages) > 0) {
    const MDBX_PNL sl = mdbx_spill_purge(parent);
    unsigned len = MDBX_PNL_SIZE(sl);
    if (len) {
      /* Remove refunded pages from parent's spill list */
      if (MDBX_ENABLE_REFUND &&
          MDBX_PNL_MOST(sl) >= (parent->mt_next_pgno << 1)) {
#if MDBX_PNL_ASCENDING
        unsigned i = MDBX_PNL_SIZE(sl);
        assert(MDBX_PNL_MOST(sl) == MDBX_PNL_LAST(sl));
        do {
          if ((sl[i] & 1) == 0)
            mdbx_debug("refund parent's spilled page %" PRIaPGNO, sl[i] >> 1);
          i -= 1;
        } while (i && sl[i] >= (parent->mt_next_pgno << 1));
        MDBX_PNL_SIZE(sl) = i;
#else
        assert(MDBX_PNL_MOST(sl) == MDBX_PNL_FIRST(sl));
        unsigned i = 0;
        do {
          ++i;
          if ((sl[i] & 1) == 0)
            mdbx_debug("refund parent's spilled page %" PRIaPGNO, sl[i] >> 1);
        } while (i < len && sl[i + 1] >= (parent->mt_next_pgno << 1));
        MDBX_PNL_SIZE(sl) = len -= i;
        memmove(sl + 1, sl + 1 + i, len * sizeof(sl[0]));
#endif
      }
      mdbx_tassert(
          txn, mdbx_pnl_check4assert(sl, (size_t)parent->mt_next_pgno << 1));

      /* Remove reclaimed pages from parent's spill list */
      s = MDBX_PNL_SIZE(sl), r = MDBX_PNL_SIZE(reclaimed_list);
      /* Scanning from end to begin */
      while (s && r) {
        if (sl[s] & 1) {
          --s;
          continue;
        }
        const pgno_t spilled_pgno = sl[s] >> 1;
        const pgno_t reclaimed_pgno = reclaimed_list[r];
        if (reclaimed_pgno != spilled_pgno) {
          const bool cmp = MDBX_PNL_ORDERED(spilled_pgno, reclaimed_pgno);
          s -= !cmp;
          r -= cmp;
        } else {
          mdbx_debug("remove reclaimed parent's spilled page %" PRIaPGNO,
                     reclaimed_pgno);
          mdbx_spill_remove(parent, s, 1);
          --s;
          --r;
        }
      }

      /* Remove anything in our dirty list from parent's spill list */
      /* Scanning spill list in descend order */
      const int step = MDBX_PNL_ASCENDING ? -1 : 1;
      s = MDBX_PNL_ASCENDING ? MDBX_PNL_SIZE(sl) : 1;
      d = src->length;
      while (d && (MDBX_PNL_ASCENDING ? s > 0 : s <= MDBX_PNL_SIZE(sl))) {
        if (sl[s] & 1) {
          s += step;
          continue;
        }
        const pgno_t spilled_pgno = sl[s] >> 1;
        const pgno_t dirty_pgno_form = src->items[d].pgno;
        const unsigned npages = dpl_npages(src, d);
        const pgno_t dirty_pgno_to = dirty_pgno_form + npages;
        if (dirty_pgno_form > spilled_pgno) {
          --d;
          continue;
        }
        if (dirty_pgno_to <= spilled_pgno) {
          s += step;
          continue;
        }

        mdbx_debug("remove dirtied parent's spilled %u page %" PRIaPGNO, npages,
                   dirty_pgno_form);
        mdbx_spill_remove(parent, s, 1);
        s += step;
      }

      /* Squash deleted pagenums if we deleted any */
      mdbx_spill_purge(parent);
    }
  }

  /* Remove anything in our spill list from parent's dirty list */
  if (txn->tw.spill_pages) {
    mdbx_tassert(txn, mdbx_pnl_check4assert(txn->tw.spill_pages,
                                            (size_t)parent->mt_next_pgno << 1));
    mdbx_dpl_sift(parent, txn->tw.spill_pages, true);
    mdbx_tassert(parent,
                 parent->tw.dirtyroom + parent->tw.dirtylist->length ==
                     (parent->mt_parent ? parent->mt_parent->tw.dirtyroom
                                        : parent->mt_env->me_options.dp_limit));
  }

  /* Find length of merging our dirty list with parent's and release
   * filter-out pages */
  for (l = 0, d = dst->length, s = src->length; d > 0 && s > 0;) {
    MDBX_page *sp = src->items[s].ptr;
    mdbx_tassert(parent,
                 (sp->mp_flags & ~(P_LEAF | P_LEAF2 | P_BRANCH | P_OVERFLOW |
                                   P_LOOSE | P_SPILLED)) == 0);
    const unsigned s_npages = dpl_npages(src, s);
    const pgno_t s_pgno = src->items[s].pgno;

    MDBX_page *dp = dst->items[d].ptr;
    mdbx_tassert(parent, (dp->mp_flags & ~(P_LEAF | P_LEAF2 | P_BRANCH |
                                           P_OVERFLOW | P_SPILLED)) == 0);
    const unsigned d_npages = dpl_npages(dst, d);
    const pgno_t d_pgno = dst->items[d].pgno;

    if (d_pgno >= s_pgno + s_npages) {
      --d;
      ++l;
    } else if (d_pgno + d_npages <= s_pgno) {
      if (sp->mp_flags != P_LOOSE) {
        sp->mp_txnid = parent->mt_front;
        sp->mp_flags &= ~P_SPILLED;
      }
      --s;
      ++l;
    } else {
      dst->items[d--].ptr = nullptr;
      if ((txn->mt_flags & MDBX_WRITEMAP) == 0)
        mdbx_dpage_free(txn->mt_env, dp, d_npages);
    }
  }
  assert(dst->sorted == dst->length);
  mdbx_tassert(parent, dst->detent >= l + d + s);
  dst->sorted = l + d + s; /* the merged length */

  while (s > 0) {
    MDBX_page *sp = src->items[s].ptr;
    mdbx_tassert(parent,
                 (sp->mp_flags & ~(P_LEAF | P_LEAF2 | P_BRANCH | P_OVERFLOW |
                                   P_LOOSE | P_SPILLED)) == 0);
    if (sp->mp_flags != P_LOOSE) {
      sp->mp_txnid = parent->mt_front;
      sp->mp_flags &= ~P_SPILLED;
    }
    --s;
  }

  /* Merge our dirty list into parent's, i.e. merge(dst, src) -> dst */
  if (dst->sorted >= dst->length) {
    /* from end to begin with dst extending */
    for (l = dst->sorted, s = src->length, d = dst->length; s > 0 && d > 0;) {
      if (unlikely(l <= d)) {
        /* squash to get a gap of free space for merge */
        for (r = w = 1; r <= d; ++r)
          if (dst->items[r].ptr) {
            if (w != r) {
              dst->items[w] = dst->items[r];
              dst->items[r].ptr = nullptr;
            }
            ++w;
          }
        mdbx_notice("squash to begin for extending-merge %u -> %u", d, w - 1);
        d = w - 1;
        continue;
      }
      assert(l > d);
      if (dst->items[d].ptr) {
        dst->items[l--] = (dst->items[d].pgno > src->items[s].pgno)
                              ? dst->items[d--]
                              : src->items[s--];
      } else
        --d;
    }
    if (s > 0) {
      assert(l == s);
      while (d > 0) {
        assert(dst->items[d].ptr == nullptr);
        --d;
      }
      do {
        assert(l > 0);
        dst->items[l--] = src->items[s--];
      } while (s > 0);
    } else {
      assert(l == d);
      while (l > 0) {
        assert(dst->items[l].ptr != nullptr);
        --l;
      }
    }
  } else {
    /* from begin to end with dst shrinking (a lot of new overflow pages) */
    for (l = s = d = 1; s <= src->length && d <= dst->length;) {
      if (unlikely(l >= d)) {
        /* squash to get a gap of free space for merge */
        for (r = w = dst->length; r >= d; --r)
          if (dst->items[r].ptr) {
            if (w != r) {
              dst->items[w] = dst->items[r];
              dst->items[r].ptr = nullptr;
            }
            --w;
          }
        mdbx_notice("squash to end for shrinking-merge %u -> %u", d, w + 1);
        d = w + 1;
        continue;
      }
      assert(l < d);
      if (dst->items[d].ptr) {
        dst->items[l++] = (dst->items[d].pgno < src->items[s].pgno)
                              ? dst->items[d++]
                              : src->items[s++];
      } else
        ++d;
    }
    if (s <= src->length) {
      assert(dst->sorted - l == src->length - s);
      while (d <= dst->length) {
        assert(dst->items[d].ptr == nullptr);
        --d;
      }
      do {
        assert(l <= dst->sorted);
        dst->items[l++] = src->items[s++];
      } while (s <= src->length);
    } else {
      assert(dst->sorted - l == dst->length - d);
      while (l <= dst->sorted) {
        assert(l <= d && d <= dst->length && dst->items[d].ptr);
        dst->items[l++] = dst->items[d++];
      }
    }
  }
  parent->tw.dirtyroom -= dst->sorted - dst->length;
  assert(parent->tw.dirtyroom <= parent->mt_env->me_options.dp_limit);
  dpl_setlen(dst, dst->sorted);
  parent->tw.dirtylru = txn->tw.dirtylru;
  mdbx_tassert(parent, mdbx_dirtylist_check(parent));
  mdbx_dpl_free(txn);

  if (txn->tw.spill_pages) {
    if (parent->tw.spill_pages) {
      /* Must not fail since space was preserved above. */
      mdbx_pnl_xmerge(parent->tw.spill_pages, txn->tw.spill_pages);
      mdbx_pnl_free(txn->tw.spill_pages);
    } else {
      parent->tw.spill_pages = txn->tw.spill_pages;
      parent->tw.spill_least_removed = txn->tw.spill_least_removed;
    }
    mdbx_tassert(parent, mdbx_dirtylist_check(parent));
  }

  parent->mt_flags &= ~MDBX_TXN_HAS_CHILD;
  if (parent->tw.spill_pages) {
    assert(mdbx_pnl_check4assert(parent->tw.spill_pages,
                                 (size_t)parent->mt_next_pgno << 1));
    if (MDBX_PNL_SIZE(parent->tw.spill_pages))
      parent->mt_flags |= MDBX_TXN_SPILLS;
  }
}

int mdbx_txn_commit_ex(MDBX_txn *txn, MDBX_commit_latency *latency) {
  STATIC_ASSERT(MDBX_TXN_FINISHED ==
                MDBX_TXN_BLOCKED - MDBX_TXN_HAS_CHILD - MDBX_TXN_ERROR);
  const uint64_t ts_0 = latency ? mdbx_osal_monotime() : 0;
  uint64_t ts_1 = 0, ts_2 = 0, ts_3 = 0, ts_4 = 0;
  uint32_t audit_duration = 0;

  int rc = check_txn(txn, MDBX_TXN_FINISHED);
  if (unlikely(rc != MDBX_SUCCESS))
    goto provide_latency;

  if (unlikely(txn->mt_flags & MDBX_TXN_ERROR)) {
    rc = MDBX_RESULT_TRUE;
    goto fail;
  }

  MDBX_env *env = txn->mt_env;
#if MDBX_ENV_CHECKPID
  if (unlikely(env->me_pid != mdbx_getpid())) {
    env->me_flags |= MDBX_FATAL_ERROR;
    rc = MDBX_PANIC;
    goto provide_latency;
  }
#endif /* MDBX_ENV_CHECKPID */

  /* mdbx_txn_end() mode for a commit which writes nothing */
  unsigned end_mode =
      MDBX_END_PURE_COMMIT | MDBX_END_UPDATE | MDBX_END_SLOT | MDBX_END_FREE;
  if (unlikely(F_ISSET(txn->mt_flags, MDBX_TXN_RDONLY)))
    goto done;

  if (txn->mt_child) {
    rc = mdbx_txn_commit_ex(txn->mt_child, NULL);
    mdbx_tassert(txn, txn->mt_child == NULL);
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;
  }

  if (unlikely(txn != env->me_txn)) {
    mdbx_debug("%s", "attempt to commit unknown transaction");
    rc = MDBX_EINVAL;
    goto fail;
  }

  if (txn->mt_parent) {
    mdbx_tassert(txn, mdbx_audit_ex(txn, 0, false) == 0);
    mdbx_assert(env, txn != env->me_txn0);
    MDBX_txn *const parent = txn->mt_parent;
    mdbx_assert(env, parent->mt_signature == MDBX_MT_SIGNATURE);
    mdbx_assert(env, parent->mt_child == txn &&
                         (parent->mt_flags & MDBX_TXN_HAS_CHILD) != 0);
    mdbx_assert(env, mdbx_dirtylist_check(txn));

    if (txn->tw.dirtylist->length == 0 && !(txn->mt_flags & MDBX_TXN_DIRTY) &&
        parent->mt_numdbs == txn->mt_numdbs) {
      for (int i = txn->mt_numdbs; --i >= 0;) {
        mdbx_tassert(txn, (txn->mt_dbistate[i] & DBI_DIRTY) == 0);
        if ((txn->mt_dbistate[i] & DBI_STALE) &&
            !(parent->mt_dbistate[i] & DBI_STALE))
          mdbx_tassert(txn, memcmp(&parent->mt_dbs[i], &txn->mt_dbs[i],
                                   sizeof(MDBX_db)) == 0);
      }

      mdbx_tassert(txn, memcmp(&parent->mt_geo, &txn->mt_geo,
                               sizeof(parent->mt_geo)) == 0);
      mdbx_tassert(txn, memcmp(&parent->mt_canary, &txn->mt_canary,
                               sizeof(parent->mt_canary)) == 0);
      mdbx_tassert(txn, !txn->tw.spill_pages ||
                            MDBX_PNL_SIZE(txn->tw.spill_pages) == 0);
      mdbx_tassert(txn, txn->tw.loose_count == 0);

      /* fast completion of pure nested transaction */
      end_mode = MDBX_END_PURE_COMMIT | MDBX_END_SLOT | MDBX_END_FREE;
      goto done;
    }

    /* Preserve space for spill list to avoid parent's state corruption
     * if allocation fails. */
    const unsigned parent_retired_len =
        (unsigned)(uintptr_t)parent->tw.retired_pages;
    mdbx_tassert(txn,
                 parent_retired_len <= MDBX_PNL_SIZE(txn->tw.retired_pages));
    const unsigned retired_delta =
        MDBX_PNL_SIZE(txn->tw.retired_pages) - parent_retired_len;
    if (retired_delta) {
      rc = mdbx_pnl_need(&txn->tw.reclaimed_pglist, retired_delta);
      if (unlikely(rc != MDBX_SUCCESS))
        goto fail;
    }

    if (txn->tw.spill_pages) {
      if (parent->tw.spill_pages) {
        rc = mdbx_pnl_need(&parent->tw.spill_pages,
                           MDBX_PNL_SIZE(txn->tw.spill_pages));
        if (unlikely(rc != MDBX_SUCCESS))
          goto fail;
      }
      mdbx_spill_purge(txn);
    }

    if (unlikely(txn->tw.dirtylist->length + parent->tw.dirtylist->length >
                     parent->tw.dirtylist->detent &&
                 !mdbx_dpl_reserve(parent, txn->tw.dirtylist->length +
                                               parent->tw.dirtylist->length))) {
      rc = MDBX_ENOMEM;
      goto fail;
    }

    //-------------------------------------------------------------------------

    parent->tw.lifo_reclaimed = txn->tw.lifo_reclaimed;
    txn->tw.lifo_reclaimed = NULL;

    parent->tw.retired_pages = txn->tw.retired_pages;
    txn->tw.retired_pages = NULL;

    mdbx_pnl_free(parent->tw.reclaimed_pglist);
    parent->tw.reclaimed_pglist = txn->tw.reclaimed_pglist;
    txn->tw.reclaimed_pglist = NULL;
    parent->tw.last_reclaimed = txn->tw.last_reclaimed;

    parent->mt_geo = txn->mt_geo;
    parent->mt_canary = txn->mt_canary;
    parent->mt_flags |= txn->mt_flags & MDBX_TXN_DIRTY;

    /* Move loose pages to parent */
#if MDBX_ENABLE_REFUND
    parent->tw.loose_refund_wl = txn->tw.loose_refund_wl;
#endif /* MDBX_ENABLE_REFUND */
    parent->tw.loose_count = txn->tw.loose_count;
    parent->tw.loose_pages = txn->tw.loose_pages;

    /* Merge our cursors into parent's and close them */
    mdbx_cursors_eot(txn, true);
    end_mode |= MDBX_END_EOTDONE;

    /* Update parent's DBs array */
    memcpy(parent->mt_dbs, txn->mt_dbs, txn->mt_numdbs * sizeof(MDBX_db));
    parent->mt_numdbs = txn->mt_numdbs;
    parent->mt_dbistate[FREE_DBI] = txn->mt_dbistate[FREE_DBI];
    parent->mt_dbistate[MAIN_DBI] = txn->mt_dbistate[MAIN_DBI];
    for (unsigned i = CORE_DBS; i < txn->mt_numdbs; i++) {
      /* preserve parent's status */
      const uint8_t state =
          txn->mt_dbistate[i] |
          (parent->mt_dbistate[i] & (DBI_CREAT | DBI_FRESH | DBI_DIRTY));
      mdbx_debug("db %u dbi-state %s 0x%02x -> 0x%02x", i,
                 (parent->mt_dbistate[i] != state) ? "update" : "still",
                 parent->mt_dbistate[i], state);
      parent->mt_dbistate[i] = state;
    }

    ts_1 = latency ? mdbx_osal_monotime() : 0;
    mdbx_txn_merge(parent, txn, parent_retired_len);
    ts_2 = latency ? mdbx_osal_monotime() : 0;
    env->me_txn = parent;
    parent->mt_child = NULL;
    mdbx_tassert(parent, mdbx_dirtylist_check(parent));

#if MDBX_ENABLE_REFUND
    mdbx_refund(parent);
    if (mdbx_assert_enabled()) {
      /* Check parent's loose pages not suitable for refund */
      for (MDBX_page *lp = parent->tw.loose_pages; lp; lp = lp->mp_next)
        mdbx_tassert(parent, lp->mp_pgno < parent->tw.loose_refund_wl &&
                                 lp->mp_pgno + 1 < parent->mt_next_pgno);
      /* Check parent's reclaimed pages not suitable for refund */
      if (MDBX_PNL_SIZE(parent->tw.reclaimed_pglist))
        mdbx_tassert(parent, MDBX_PNL_MOST(parent->tw.reclaimed_pglist) + 1 <
                                 parent->mt_next_pgno);
    }
#endif /* MDBX_ENABLE_REFUND */

    ts_4 = ts_3 = latency ? mdbx_osal_monotime() : 0;
    txn->mt_signature = 0;
    mdbx_free(txn);
    mdbx_tassert(parent, mdbx_audit_ex(parent, 0, false) == 0);
    rc = MDBX_SUCCESS;
    goto provide_latency;
  }

  mdbx_tassert(txn, txn->tw.dirtyroom + txn->tw.dirtylist->length ==
                        (txn->mt_parent ? txn->mt_parent->tw.dirtyroom
                                        : txn->mt_env->me_options.dp_limit));
  mdbx_cursors_eot(txn, false);
  end_mode |= MDBX_END_EOTDONE;

  if (txn->tw.dirtylist->length == 0 &&
      (txn->mt_flags & (MDBX_TXN_DIRTY | MDBX_TXN_SPILLS)) == 0) {
    for (int i = txn->mt_numdbs; --i >= 0;)
      mdbx_tassert(txn, (txn->mt_dbistate[i] & DBI_DIRTY) == 0);
    rc = MDBX_SUCCESS;
    goto done;
  }

  mdbx_debug("committing txn %" PRIaTXN " %p on mdbenv %p, root page %" PRIaPGNO
             "/%" PRIaPGNO,
             txn->mt_txnid, (void *)txn, (void *)env,
             txn->mt_dbs[MAIN_DBI].md_root, txn->mt_dbs[FREE_DBI].md_root);

  /* Update DB root pointers */
  if (txn->mt_numdbs > CORE_DBS) {
    MDBX_cursor_couple couple;
    MDBX_val data;
    data.iov_len = sizeof(MDBX_db);

    rc = mdbx_cursor_init(&couple.outer, txn, MAIN_DBI);
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;
    for (MDBX_dbi i = CORE_DBS; i < txn->mt_numdbs; i++) {
      if (txn->mt_dbistate[i] & DBI_DIRTY) {
        if (unlikely(TXN_DBI_CHANGED(txn, i))) {
          rc = MDBX_BAD_DBI;
          goto fail;
        }
        MDBX_db *db = &txn->mt_dbs[i];
        mdbx_debug("update main's entry for sub-db %u, mod_txnid %" PRIaTXN
                   " -> %" PRIaTXN,
                   i, db->md_mod_txnid, txn->mt_txnid);
        db->md_mod_txnid = txn->mt_txnid;
        data.iov_base = db;
        WITH_CURSOR_TRACKING(couple.outer,
                             rc = mdbx_cursor_put(&couple.outer,
                                                  &txn->mt_dbxs[i].md_name,
                                                  &data, F_SUBDATA));
        if (unlikely(rc != MDBX_SUCCESS))
          goto fail;
      }
    }
  }

  ts_1 = latency ? mdbx_osal_monotime() : 0;
  rc = mdbx_update_gc(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    goto fail;

  ts_2 = latency ? mdbx_osal_monotime() : 0;
  if (mdbx_audit_enabled()) {
    rc = mdbx_audit_ex(txn, MDBX_PNL_SIZE(txn->tw.retired_pages), true);
    const uint64_t audit_end = mdbx_osal_monotime();
    audit_duration = mdbx_osal_monotime_to_16dot16(audit_end - ts_2);
    ts_2 = audit_end;
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;
  }

  struct mdbx_iov_ctx ctx;
  mdbx_iov_init(txn, &ctx);
  rc = mdbx_txn_write(txn, &ctx);
  if (likely(rc == MDBX_SUCCESS))
    mdbx_iov_done(txn, &ctx);
  /* TODO: use ctx.flush_begin & ctx.flush_end for range-sync */
  ts_3 = latency ? mdbx_osal_monotime() : 0;

  if (likely(rc == MDBX_SUCCESS)) {

    MDBX_meta meta, *head = mdbx_meta_head(env);
    memcpy(meta.mm_magic_and_version, head->mm_magic_and_version, 8);
    meta.mm_extra_flags = head->mm_extra_flags;
    meta.mm_validator_id = head->mm_validator_id;
    meta.mm_extra_pagehdr = head->mm_extra_pagehdr;
    unaligned_poke_u64(4, meta.mm_pages_retired,
                       unaligned_peek_u64(4, head->mm_pages_retired) +
                           MDBX_PNL_SIZE(txn->tw.retired_pages));

    meta.mm_geo = txn->mt_geo;
    meta.mm_dbs[FREE_DBI] = txn->mt_dbs[FREE_DBI];
    meta.mm_dbs[FREE_DBI].md_mod_txnid =
        (txn->mt_dbistate[FREE_DBI] & DBI_DIRTY)
            ? txn->mt_txnid
            : txn->mt_dbs[FREE_DBI].md_mod_txnid;
    meta.mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
    meta.mm_dbs[MAIN_DBI].md_mod_txnid =
        (txn->mt_dbistate[MAIN_DBI] & DBI_DIRTY)
            ? txn->mt_txnid
            : txn->mt_dbs[MAIN_DBI].md_mod_txnid;
    meta.mm_canary = txn->mt_canary;
    mdbx_meta_set_txnid(env, &meta, txn->mt_txnid);

    rc = mdbx_sync_locked(
        env, env->me_flags | txn->mt_flags | MDBX_SHRINK_ALLOWED, &meta);
  }
  ts_4 = latency ? mdbx_osal_monotime() : 0;
  if (unlikely(rc != MDBX_SUCCESS)) {
    env->me_flags |= MDBX_FATAL_ERROR;
    goto fail;
  }

  end_mode = MDBX_END_COMMITTED | MDBX_END_UPDATE | MDBX_END_EOTDONE;

done:
  rc = mdbx_txn_end(txn, end_mode);

provide_latency:
  if (latency) {
    latency->audit = audit_duration;
    latency->preparation =
        ts_1 ? mdbx_osal_monotime_to_16dot16(ts_1 - ts_0) : 0;
    latency->gc =
        (ts_1 && ts_2) ? mdbx_osal_monotime_to_16dot16(ts_2 - ts_1) : 0;
    latency->write =
        (ts_2 && ts_3) ? mdbx_osal_monotime_to_16dot16(ts_3 - ts_2) : 0;
    latency->sync =
        (ts_3 && ts_4) ? mdbx_osal_monotime_to_16dot16(ts_4 - ts_3) : 0;
    const uint64_t ts_5 = mdbx_osal_monotime();
    latency->ending = ts_4 ? mdbx_osal_monotime_to_16dot16(ts_5 - ts_4) : 0;
    latency->whole = mdbx_osal_monotime_to_16dot16(ts_5 - ts_0);
  }
  return rc;

fail:
  mdbx_txn_abort(txn);
  goto provide_latency;
}

static int mdbx_validate_meta(MDBX_env *env, MDBX_meta *const meta,
                              const MDBX_page *const page,
                              const unsigned meta_number,
                              unsigned *guess_pagesize) {
  const uint64_t magic_and_version =
      unaligned_peek_u64(4, &meta->mm_magic_and_version);
  if (unlikely(magic_and_version != MDBX_DATA_MAGIC &&
               magic_and_version != MDBX_DATA_MAGIC_LEGACY_COMPAT &&
               magic_and_version != MDBX_DATA_MAGIC_LEGACY_DEVEL)) {
    mdbx_error("meta[%u] has invalid magic/version %" PRIx64, meta_number,
               magic_and_version);
    return ((magic_and_version >> 8) != MDBX_MAGIC) ? MDBX_INVALID
                                                    : MDBX_VERSION_MISMATCH;
  }

  if (unlikely(page->mp_pgno != meta_number)) {
    mdbx_error("meta[%u] has invalid pageno %" PRIaPGNO, meta_number,
               page->mp_pgno);
    return MDBX_INVALID;
  }

  if (unlikely(page->mp_flags != P_META)) {
    mdbx_error("page #%u not a meta-page", meta_number);
    return MDBX_INVALID;
  }

  /* LY: check pagesize */
  if (unlikely(!is_powerof2(meta->mm_psize) || meta->mm_psize < MIN_PAGESIZE ||
               meta->mm_psize > MAX_PAGESIZE)) {
    mdbx_warning("meta[%u] has invalid pagesize (%u), skip it", meta_number,
                 meta->mm_psize);
    return is_powerof2(meta->mm_psize) ? MDBX_VERSION_MISMATCH : MDBX_INVALID;
  }

  if (guess_pagesize && *guess_pagesize != meta->mm_psize) {
    *guess_pagesize = meta->mm_psize;
    mdbx_verbose("meta[%u] took pagesize %u", meta_number, meta->mm_psize);
  }

  const txnid_t txnid = unaligned_peek_u64(4, &meta->mm_txnid_a);
  if (unlikely(txnid != unaligned_peek_u64(4, &meta->mm_txnid_b))) {
    mdbx_warning("meta[%u] not completely updated, skip it", meta_number);
    return MDBX_RESULT_TRUE;
  }

  /* LY: check signature as a checksum */
  if (META_IS_STEADY(meta) &&
      unlikely(unaligned_peek_u64(4, &meta->mm_datasync_sign) !=
               mdbx_meta_sign(meta))) {
    mdbx_warning("meta[%u] has invalid steady-checksum (0x%" PRIx64
                 " != 0x%" PRIx64 "), skip it",
                 meta_number, unaligned_peek_u64(4, &meta->mm_datasync_sign),
                 mdbx_meta_sign(meta));
    return MDBX_RESULT_TRUE;
  }

  mdbx_debug("checking meta%" PRIaPGNO " = root %" PRIaPGNO "/%" PRIaPGNO
             ", geo %" PRIaPGNO "/%" PRIaPGNO "-%" PRIaPGNO "/%" PRIaPGNO
             " +%u -%u, txn_id %" PRIaTXN ", %s",
             page->mp_pgno, meta->mm_dbs[MAIN_DBI].md_root,
             meta->mm_dbs[FREE_DBI].md_root, meta->mm_geo.lower,
             meta->mm_geo.next, meta->mm_geo.now, meta->mm_geo.upper,
             pv2pages(meta->mm_geo.grow_pv), pv2pages(meta->mm_geo.shrink_pv),
             txnid, mdbx_durable_str(meta));

  if (unlikely(txnid < MIN_TXNID || txnid > MAX_TXNID)) {
    mdbx_warning("meta[%u] has invalid txnid %" PRIaTXN ", skip it",
                 meta_number, txnid);
    return MDBX_RESULT_TRUE;
  }

  /* LY: check min-pages value */
  if (unlikely(meta->mm_geo.lower < MIN_PAGENO ||
               meta->mm_geo.lower > MAX_PAGENO)) {
    mdbx_warning("meta[%u] has invalid min-pages (%" PRIaPGNO "), skip it",
                 meta_number, meta->mm_geo.lower);
    return MDBX_INVALID;
  }

  /* LY: check max-pages value */
  if (unlikely(meta->mm_geo.upper < MIN_PAGENO ||
               meta->mm_geo.upper > MAX_PAGENO ||
               meta->mm_geo.upper < meta->mm_geo.lower)) {
    mdbx_warning("meta[%u] has invalid max-pages (%" PRIaPGNO "), skip it",
                 meta_number, meta->mm_geo.upper);
    return MDBX_INVALID;
  }

  /* LY: check last_pgno */
  if (unlikely(meta->mm_geo.next < MIN_PAGENO ||
               meta->mm_geo.next - 1 > MAX_PAGENO)) {
    mdbx_warning("meta[%u] has invalid next-pageno (%" PRIaPGNO "), skip it",
                 meta_number, meta->mm_geo.next);
    return MDBX_CORRUPTED;
  }

  /* LY: check filesize & used_bytes */
  const uint64_t used_bytes = meta->mm_geo.next * (uint64_t)meta->mm_psize;
  if (unlikely(used_bytes > env->me_dxb_mmap.filesize)) {
    /* Here could be a race with DB-shrinking performed by other process */
    int err = mdbx_filesize(env->me_lazy_fd, &env->me_dxb_mmap.filesize);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if (unlikely(used_bytes > env->me_dxb_mmap.filesize)) {
      mdbx_warning("meta[%u] used-bytes (%" PRIu64 ") beyond filesize (%" PRIu64
                   "), skip it",
                   meta_number, used_bytes, env->me_dxb_mmap.filesize);
      return MDBX_CORRUPTED;
    }
  }
  if (unlikely(meta->mm_geo.next - 1 > MAX_PAGENO ||
               used_bytes > MAX_MAPSIZE)) {
    mdbx_warning("meta[%u] has too large used-space (%" PRIu64 "), skip it",
                 meta_number, used_bytes);
    return MDBX_TOO_LARGE;
  }

  /* LY: check mapsize limits */
  pgno_t geo_lower = meta->mm_geo.lower;
  uint64_t mapsize_min = geo_lower * (uint64_t)meta->mm_psize;
  STATIC_ASSERT(MAX_MAPSIZE < PTRDIFF_MAX - MAX_PAGESIZE);
  STATIC_ASSERT(MIN_MAPSIZE < MAX_MAPSIZE);
  if (unlikely(mapsize_min < MIN_MAPSIZE || mapsize_min > MAX_MAPSIZE)) {
    if (MAX_MAPSIZE != MAX_MAPSIZE64 && mapsize_min > MAX_MAPSIZE &&
        mapsize_min <= MAX_MAPSIZE64) {
      mdbx_assert(env, meta->mm_geo.next - 1 <= MAX_PAGENO &&
                           used_bytes <= MAX_MAPSIZE);
      mdbx_warning("meta[%u] has too large min-mapsize (%" PRIu64 "), "
                   "but size of used space still acceptable (%" PRIu64 ")",
                   meta_number, mapsize_min, used_bytes);
      geo_lower = (pgno_t)(mapsize_min = MAX_MAPSIZE / meta->mm_psize);
      mdbx_warning("meta[%u] consider get-%s pageno is %" PRIaPGNO
                   " instead of wrong %" PRIaPGNO
                   ", will be corrected on next commit(s)",
                   meta_number, "lower", geo_lower, meta->mm_geo.lower);
      meta->mm_geo.lower = geo_lower;
    } else {
      mdbx_warning("meta[%u] has invalid min-mapsize (%" PRIu64 "), skip it",
                   meta_number, mapsize_min);
      return MDBX_VERSION_MISMATCH;
    }
  }

  pgno_t geo_upper = meta->mm_geo.upper;
  uint64_t mapsize_max = geo_upper * (uint64_t)meta->mm_psize;
  STATIC_ASSERT(MIN_MAPSIZE < MAX_MAPSIZE);
  if (unlikely(mapsize_max > MAX_MAPSIZE ||
               MAX_PAGENO <
                   ceil_powerof2((size_t)mapsize_max, env->me_os_psize) /
                       (size_t)meta->mm_psize)) {
    if (mapsize_max > MAX_MAPSIZE64) {
      mdbx_warning("meta[%u] has invalid max-mapsize (%" PRIu64 "), skip it",
                   meta_number, mapsize_max);
      return MDBX_VERSION_MISMATCH;
    }
    /* allow to open large DB from a 32-bit environment */
    mdbx_assert(env, meta->mm_geo.next - 1 <= MAX_PAGENO &&
                         used_bytes <= MAX_MAPSIZE);
    mdbx_warning("meta[%u] has too large max-mapsize (%" PRIu64 "), "
                 "but size of used space still acceptable (%" PRIu64 ")",
                 meta_number, mapsize_max, used_bytes);
    geo_upper = (pgno_t)(mapsize_max = MAX_MAPSIZE / meta->mm_psize);
    mdbx_warning("meta[%u] consider get-%s pageno is %" PRIaPGNO
                 " instead of wrong %" PRIaPGNO
                 ", will be corrected on next commit(s)",
                 meta_number, "upper", geo_upper, meta->mm_geo.upper);
    meta->mm_geo.upper = geo_upper;
  }

  /* LY: check and silently put mm_geo.now into [geo.lower...geo.upper].
   *
   * Copy-with-compaction by previous version of libmdbx could produce DB-file
   * less than meta.geo.lower bound, in case actual filling is low or no data
   * at all. This is not a problem as there is no damage or loss of data.
   * Therefore it is better not to consider such situation as an error, but
   * silently correct it. */
  pgno_t geo_now = meta->mm_geo.now;
  if (geo_now < geo_lower)
    geo_now = geo_lower;
  if (geo_now > geo_upper && meta->mm_geo.next <= geo_upper)
    geo_now = geo_upper;

  if (unlikely(meta->mm_geo.next > geo_now)) {
    mdbx_warning("meta[%u] next-pageno (%" PRIaPGNO
                 ") is beyond end-pgno (%" PRIaPGNO "), skip it",
                 meta_number, meta->mm_geo.next, geo_now);
    return MDBX_CORRUPTED;
  }
  if (meta->mm_geo.now != geo_now) {
    mdbx_warning("meta[%u] consider geo-%s pageno is %" PRIaPGNO
                 " instead of wrong %" PRIaPGNO
                 ", will be corrected on next commit(s)",
                 meta_number, "now", geo_now, meta->mm_geo.now);
    meta->mm_geo.now = geo_now;
  }

  /* GC */
  if (meta->mm_dbs[FREE_DBI].md_root == P_INVALID) {
    if (unlikely(meta->mm_dbs[FREE_DBI].md_branch_pages ||
                 meta->mm_dbs[FREE_DBI].md_depth ||
                 meta->mm_dbs[FREE_DBI].md_entries ||
                 meta->mm_dbs[FREE_DBI].md_leaf_pages ||
                 meta->mm_dbs[FREE_DBI].md_overflow_pages)) {
      mdbx_warning("meta[%u] has false-empty GC, skip it", meta_number);
      return MDBX_CORRUPTED;
    }
  } else if (unlikely(meta->mm_dbs[FREE_DBI].md_root >= meta->mm_geo.next)) {
    mdbx_warning("meta[%u] has invalid GC-root %" PRIaPGNO ", skip it",
                 meta_number, meta->mm_dbs[FREE_DBI].md_root);
    return MDBX_CORRUPTED;
  }

  /* MainDB */
  if (meta->mm_dbs[MAIN_DBI].md_root == P_INVALID) {
    if (unlikely(meta->mm_dbs[MAIN_DBI].md_branch_pages ||
                 meta->mm_dbs[MAIN_DBI].md_depth ||
                 meta->mm_dbs[MAIN_DBI].md_entries ||
                 meta->mm_dbs[MAIN_DBI].md_leaf_pages ||
                 meta->mm_dbs[MAIN_DBI].md_overflow_pages)) {
      mdbx_warning("meta[%u] has false-empty maindb", meta_number);
      return MDBX_CORRUPTED;
    }
  } else if (unlikely(meta->mm_dbs[MAIN_DBI].md_root >= meta->mm_geo.next)) {
    mdbx_warning("meta[%u] has invalid maindb-root %" PRIaPGNO ", skip it",
                 meta_number, meta->mm_dbs[MAIN_DBI].md_root);
    return MDBX_CORRUPTED;
  }

  return MDBX_SUCCESS;
}

static int mdbx_validate_meta_copy(MDBX_env *env, const MDBX_meta *meta,
                                   MDBX_meta *dest) {
  *dest = *meta;
  return mdbx_validate_meta(env, dest, data_page(meta),
                            bytes2pgno(env, (uint8_t *)meta - env->me_map),
                            nullptr);
}

/* Read the environment parameters of a DB environment
 * before mapping it into memory. */
__cold static int mdbx_read_header(MDBX_env *env, MDBX_meta *dest,
                                   const int lck_exclusive,
                                   const mdbx_mode_t mode_bits) {
  int rc = mdbx_filesize(env->me_lazy_fd, &env->me_dxb_mmap.filesize);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  memset(dest, 0, sizeof(MDBX_meta));
  unaligned_poke_u64(4, dest->mm_datasync_sign, MDBX_DATASIGN_WEAK);
  rc = MDBX_CORRUPTED;

  /* Read twice all meta pages so we can find the latest one. */
  unsigned loop_limit = NUM_METAS * 2;
  /* We don't know the page size on first time. So, just guess it. */
  unsigned guess_pagesize = 0;
  for (unsigned loop_count = 0; loop_count < loop_limit; ++loop_count) {
    const unsigned meta_number = loop_count % NUM_METAS;
    const unsigned offset = (guess_pagesize             ? guess_pagesize
                             : (loop_count > NUM_METAS) ? env->me_psize
                                                        : env->me_os_psize) *
                            meta_number;

    char buffer[MIN_PAGESIZE];
    unsigned retryleft = 42;
    while (1) {
      mdbx_trace("reading meta[%d]: offset %u, bytes %u, retry-left %u",
                 meta_number, offset, MIN_PAGESIZE, retryleft);
      int err = mdbx_pread(env->me_lazy_fd, buffer, MIN_PAGESIZE, offset);
      if (err != MDBX_SUCCESS) {
        if (err == MDBX_ENODATA && offset == 0 && loop_count == 0 &&
            env->me_dxb_mmap.filesize == 0 &&
            mode_bits /* non-zero for DB creation */ != 0)
          mdbx_notice("read meta: empty file (%d, %s)", err,
                      mdbx_strerror(err));
        else
          mdbx_error("read meta[%u,%u]: %i, %s", offset, MIN_PAGESIZE, err,
                     mdbx_strerror(err));
        return err;
      }

      char again[MIN_PAGESIZE];
      err = mdbx_pread(env->me_lazy_fd, again, MIN_PAGESIZE, offset);
      if (err != MDBX_SUCCESS) {
        mdbx_error("read meta[%u,%u]: %i, %s", offset, MIN_PAGESIZE, err,
                   mdbx_strerror(err));
        return err;
      }

      if (memcmp(buffer, again, MIN_PAGESIZE) == 0 || --retryleft == 0)
        break;

      mdbx_verbose("meta[%u] was updated, re-read it", meta_number);
    }

    if (!retryleft) {
      mdbx_error("meta[%u] is too volatile, skip it", meta_number);
      continue;
    }

    MDBX_page *const page = (MDBX_page *)buffer;
    MDBX_meta *const meta = page_meta(page);
    rc = mdbx_validate_meta(env, meta, page, meta_number, &guess_pagesize);
    if (rc != MDBX_SUCCESS)
      continue;

    if ((env->me_stuck_meta < 0)
            ? mdbx_meta_ot(meta_bootid_match(meta) ? prefer_last
                                                   : prefer_steady,
                           env, dest, meta)
            : (meta_number == (unsigned)env->me_stuck_meta)) {
      *dest = *meta;
      if (!lck_exclusive && !META_IS_STEADY(dest))
        loop_limit += 1; /* LY: should re-read to hush race with update */
      mdbx_verbose("latch meta[%u]", meta_number);
    }
  }

  if (dest->mm_psize == 0 ||
      (env->me_stuck_meta < 0 &&
       !(META_IS_STEADY(dest) ||
         meta_weak_acceptable(env, dest, lck_exclusive)))) {
    mdbx_error("%s", "no usable meta-pages, database is corrupted");
    if (rc == MDBX_SUCCESS) {
      /* TODO: try to restore the database by fully checking b-tree structure
       * for the each meta page, if the corresponding option was given */
      return MDBX_CORRUPTED;
    }
    return rc;
  }

  return MDBX_SUCCESS;
}

__cold static MDBX_page *mdbx_meta_model(const MDBX_env *env, MDBX_page *model,
                                         unsigned num) {
  mdbx_ensure(env, is_powerof2(env->me_psize));
  mdbx_ensure(env, env->me_psize >= MIN_PAGESIZE);
  mdbx_ensure(env, env->me_psize <= MAX_PAGESIZE);
  mdbx_ensure(env, env->me_dbgeo.lower >= MIN_MAPSIZE);
  mdbx_ensure(env, env->me_dbgeo.upper <= MAX_MAPSIZE);
  mdbx_ensure(env, env->me_dbgeo.now >= env->me_dbgeo.lower);
  mdbx_ensure(env, env->me_dbgeo.now <= env->me_dbgeo.upper);

  memset(model, 0, env->me_psize);
  model->mp_pgno = num;
  model->mp_flags = P_META;
  MDBX_meta *const model_meta = page_meta(model);
  unaligned_poke_u64(4, model_meta->mm_magic_and_version, MDBX_DATA_MAGIC);

  model_meta->mm_geo.lower = bytes2pgno(env, env->me_dbgeo.lower);
  model_meta->mm_geo.upper = bytes2pgno(env, env->me_dbgeo.upper);
  model_meta->mm_geo.grow_pv = pages2pv(bytes2pgno(env, env->me_dbgeo.grow));
  model_meta->mm_geo.shrink_pv =
      pages2pv(bytes2pgno(env, env->me_dbgeo.shrink));
  model_meta->mm_geo.now = bytes2pgno(env, env->me_dbgeo.now);
  model_meta->mm_geo.next = NUM_METAS;

  mdbx_ensure(env, model_meta->mm_geo.lower >= MIN_PAGENO);
  mdbx_ensure(env, model_meta->mm_geo.upper <= MAX_PAGENO);
  mdbx_ensure(env, model_meta->mm_geo.now >= model_meta->mm_geo.lower);
  mdbx_ensure(env, model_meta->mm_geo.now <= model_meta->mm_geo.upper);
  mdbx_ensure(env, model_meta->mm_geo.next >= MIN_PAGENO);
  mdbx_ensure(env, model_meta->mm_geo.next <= model_meta->mm_geo.now);
  mdbx_ensure(env, model_meta->mm_geo.grow_pv ==
                       pages2pv(pv2pages(model_meta->mm_geo.grow_pv)));
  mdbx_ensure(env, model_meta->mm_geo.shrink_pv ==
                       pages2pv(pv2pages(model_meta->mm_geo.shrink_pv)));

  model_meta->mm_psize = env->me_psize;
  model_meta->mm_dbs[FREE_DBI].md_flags = MDBX_INTEGERKEY;
  model_meta->mm_dbs[FREE_DBI].md_root = P_INVALID;
  model_meta->mm_dbs[MAIN_DBI].md_root = P_INVALID;
  mdbx_meta_set_txnid(env, model_meta, MIN_TXNID + num);
  unaligned_poke_u64(4, model_meta->mm_datasync_sign,
                     mdbx_meta_sign(model_meta));
  return (MDBX_page *)((uint8_t *)model + env->me_psize);
}

/* Fill in most of the zeroed meta-pages for an empty database environment.
 * Return pointer to recently (head) meta-page. */
__cold static MDBX_meta *mdbx_init_metas(const MDBX_env *env, void *buffer) {
  MDBX_page *page0 = (MDBX_page *)buffer;
  MDBX_page *page1 = mdbx_meta_model(env, page0, 0);
  MDBX_page *page2 = mdbx_meta_model(env, page1, 1);
  mdbx_meta_model(env, page2, 2);
  mdbx_assert(env, !mdbx_meta_eq(env, page_meta(page0), page_meta(page1)));
  mdbx_assert(env, !mdbx_meta_eq(env, page_meta(page1), page_meta(page2)));
  mdbx_assert(env, !mdbx_meta_eq(env, page_meta(page2), page_meta(page0)));
  return page_meta(page2);
}

#if MDBX_ENABLE_MADVISE && !(defined(_WIN32) || defined(_WIN64))
static size_t mdbx_madvise_threshold(const MDBX_env *env,
                                     const size_t largest_bytes) {
  /* TODO: use options */
  const unsigned factor = 9;
  const size_t threshold = (largest_bytes < (65536ul << factor))
                               ? 65536 /* minimal threshold */
                           : (largest_bytes > (MEGABYTE * 4 << factor))
                               ? MEGABYTE * 4 /* maximal threshold */
                               : largest_bytes >> factor;
  return bytes_align2os_bytes(env, threshold);
}
#endif /* MDBX_ENABLE_MADVISE */

static int mdbx_sync_locked(MDBX_env *env, unsigned flags,
                            MDBX_meta *const pending) {
  mdbx_assert(env, ((env->me_flags ^ flags) & MDBX_WRITEMAP) == 0);
  MDBX_meta *const meta0 = METAPAGE(env, 0);
  MDBX_meta *const meta1 = METAPAGE(env, 1);
  MDBX_meta *const meta2 = METAPAGE(env, 2);
  MDBX_meta *const head = mdbx_meta_head(env);
  int rc;

  mdbx_assert(env, mdbx_meta_eq_mask(env) == 0);
  mdbx_assert(env,
              pending < METAPAGE(env, 0) || pending > METAPAGE(env, NUM_METAS));
  mdbx_assert(env, (env->me_flags & (MDBX_RDONLY | MDBX_FATAL_ERROR)) == 0);
  mdbx_assert(env, pending->mm_geo.next <= pending->mm_geo.now);

  if (flags & MDBX_SAFE_NOSYNC) {
    /* Check auto-sync conditions */
    const pgno_t autosync_threshold =
        atomic_load32(&env->me_lck->mti_autosync_threshold, mo_Relaxed);
    const uint64_t autosync_period =
        atomic_load64(&env->me_lck->mti_autosync_period, mo_Relaxed);
    if ((autosync_threshold &&
         atomic_load32(&env->me_lck->mti_unsynced_pages, mo_Relaxed) >=
             autosync_threshold) ||
        (autosync_period &&
         mdbx_osal_monotime() -
                 atomic_load64(&env->me_lck->mti_sync_timestamp, mo_Relaxed) >=
             autosync_period))
      flags &= MDBX_WRITEMAP | MDBX_SHRINK_ALLOWED; /* force steady */
  }

  pgno_t shrink = 0;
  if (flags & MDBX_SHRINK_ALLOWED) {
    /* LY: check conditions to discard unused pages */
    const pgno_t largest_pgno = mdbx_find_largest(
        env, (head->mm_geo.next > pending->mm_geo.next) ? head->mm_geo.next
                                                        : pending->mm_geo.next);
    mdbx_assert(env, largest_pgno >= NUM_METAS);
#if defined(MDBX_USE_VALGRIND) || defined(__SANITIZE_ADDRESS__)
    const pgno_t edge = env->me_poison_edge;
    if (edge > largest_pgno) {
      env->me_poison_edge = largest_pgno;
      VALGRIND_MAKE_MEM_NOACCESS(env->me_map + pgno2bytes(env, largest_pgno),
                                 pgno2bytes(env, edge - largest_pgno));
      MDBX_ASAN_POISON_MEMORY_REGION(env->me_map +
                                         pgno2bytes(env, largest_pgno),
                                     pgno2bytes(env, edge - largest_pgno));
    }
#endif /* MDBX_USE_VALGRIND || __SANITIZE_ADDRESS__ */
#if MDBX_ENABLE_MADVISE &&                                                     \
    (defined(MADV_DONTNEED) || defined(POSIX_MADV_DONTNEED))
    const size_t largest_bytes = pgno2bytes(env, largest_pgno);
    /* threshold to avoid unreasonable frequent madvise() calls */
    const size_t madvise_threshold = mdbx_madvise_threshold(env, largest_bytes);
    const size_t discard_edge_bytes = bytes_align2os_bytes(
        env, ((MDBX_RDONLY &
               (env->me_lck_mmap.lck ? env->me_lck_mmap.lck->mti_envmode.weak
                                     : env->me_flags))
                  ? largest_bytes
                  : largest_bytes + madvise_threshold));
    const pgno_t discard_edge_pgno = bytes2pgno(env, discard_edge_bytes);
    const pgno_t prev_discarded_pgno =
        atomic_load32(&env->me_lck->mti_discarded_tail, mo_Relaxed);
    if (prev_discarded_pgno >=
        discard_edge_pgno + bytes2pgno(env, madvise_threshold)) {
      mdbx_notice("open-MADV_%s %u..%u", "DONTNEED", largest_pgno,
                  prev_discarded_pgno);
      atomic_store32(&env->me_lck->mti_discarded_tail, discard_edge_pgno,
                     mo_Relaxed);
      const size_t prev_discarded_bytes =
          ceil_powerof2(pgno2bytes(env, prev_discarded_pgno), env->me_os_psize);
      mdbx_ensure(env, prev_discarded_bytes > discard_edge_bytes);
#if defined(MADV_DONTNEED)
      int advise = MADV_DONTNEED;
#if defined(MADV_FREE) &&                                                      \
    0 /* MADV_FREE works for only anonymous vma at the moment */
      if ((env->me_flags & MDBX_WRITEMAP) &&
          mdbx_linux_kernel_version > 0x04050000)
        advise = MADV_FREE;
#endif /* MADV_FREE */
      int err = madvise(env->me_map + discard_edge_bytes,
                        prev_discarded_bytes - discard_edge_bytes, advise)
                    ? ignore_enosys(errno)
                    : MDBX_SUCCESS;
#else
      int err = ignore_enosys(posix_madvise(
          env->me_map + discard_edge_bytes,
          prev_discarded_bytes - discard_edge_bytes, POSIX_MADV_DONTNEED));
#endif
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
    }
#endif /* MDBX_ENABLE_MADVISE && (MADV_DONTNEED || POSIX_MADV_DONTNEED) */

    /* LY: check conditions to shrink datafile */
    const pgno_t backlog_gap = 3 + pending->mm_dbs[FREE_DBI].md_depth * 3;
    pgno_t shrink_step = 0;
    if (pending->mm_geo.shrink_pv &&
        pending->mm_geo.now - pending->mm_geo.next >
            (shrink_step = pv2pages(pending->mm_geo.shrink_pv)) + backlog_gap) {
      if (pending->mm_geo.now > largest_pgno &&
          pending->mm_geo.now - largest_pgno > shrink_step + backlog_gap) {
        pgno_t grow_step = 0;
        const pgno_t aligner =
            pending->mm_geo.grow_pv
                ? (grow_step = pv2pages(pending->mm_geo.grow_pv))
                : shrink_step;
        const pgno_t with_backlog_gap = largest_pgno + backlog_gap;
        const pgno_t aligned = pgno_align2os_pgno(
            env, with_backlog_gap + aligner - with_backlog_gap % aligner);
        const pgno_t bottom =
            (aligned > pending->mm_geo.lower) ? aligned : pending->mm_geo.lower;
        if (pending->mm_geo.now > bottom) {
          if (META_IS_STEADY(mdbx_meta_steady(env)))
            /* force steady, but only if steady-checkpoint is present */
            flags &= MDBX_WRITEMAP | MDBX_SHRINK_ALLOWED;
          shrink = pending->mm_geo.now - bottom;
          pending->mm_geo.now = bottom;
          if (unlikely(mdbx_meta_txnid_stable(env, head) ==
                       unaligned_peek_u64(4, pending->mm_txnid_a))) {
            const txnid_t txnid =
                safe64_txnid_next(unaligned_peek_u64(4, pending->mm_txnid_a));
            mdbx_notice("force-forward pending-txn %" PRIaTXN " -> %" PRIaTXN,
                        unaligned_peek_u64(4, pending->mm_txnid_a), txnid);
            mdbx_ensure(env, env->me_txn0->mt_owner != mdbx_thread_self() &&
                                 !env->me_txn);
            if (unlikely(txnid > MAX_TXNID)) {
              rc = MDBX_TXN_FULL;
              mdbx_error("txnid overflow, raise %d", rc);
              goto fail;
            }
            mdbx_meta_set_txnid(env, pending, txnid);
          }
        }
      }
    }
  }

  /* LY: step#1 - sync previously written/updated data-pages */
  rc = MDBX_RESULT_FALSE /* carry steady */;
  if (atomic_load32(&env->me_lck->mti_unsynced_pages, mo_Relaxed)) {
    mdbx_assert(env, ((flags ^ env->me_flags) & MDBX_WRITEMAP) == 0);
    enum mdbx_syncmode_bits mode_bits = MDBX_SYNC_NONE;
    if ((flags & MDBX_SAFE_NOSYNC) == 0) {
      mode_bits = MDBX_SYNC_DATA;
      if (pending->mm_geo.next > mdbx_meta_steady(env)->mm_geo.now)
        mode_bits |= MDBX_SYNC_SIZE;
      if (flags & MDBX_NOMETASYNC)
        mode_bits |= MDBX_SYNC_IODQ;
    }
#if MDBX_ENABLE_PGOP_STAT
    env->me_lck->mti_pgop_stat.wops.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
    if (flags & MDBX_WRITEMAP)
      rc =
          mdbx_msync(&env->me_dxb_mmap, 0,
                     pgno_align2os_bytes(env, pending->mm_geo.next), mode_bits);
    else
      rc = mdbx_fsync(env->me_lazy_fd, mode_bits);
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;
    rc = (flags & MDBX_SAFE_NOSYNC) ? MDBX_RESULT_TRUE /* carry non-steady */
                                    : MDBX_RESULT_FALSE /* carry steady */;
  }

  /* Steady or Weak */
  if (rc == MDBX_RESULT_FALSE /* carry steady */) {
    atomic_store64(&env->me_lck->mti_sync_timestamp, mdbx_osal_monotime(),
                   mo_Relaxed);
    unaligned_poke_u64(4, pending->mm_datasync_sign, mdbx_meta_sign(pending));
    atomic_store32(&env->me_lck->mti_unsynced_pages, 0, mo_Relaxed);
  } else {
    assert(rc == MDBX_RESULT_TRUE /* carry non-steady */);
    unaligned_poke_u64(4, pending->mm_datasync_sign, MDBX_DATASIGN_WEAK);
  }

  MDBX_meta *target = nullptr;
  if (mdbx_meta_txnid_stable(env, head) ==
      unaligned_peek_u64(4, pending->mm_txnid_a)) {
    mdbx_assert(env, memcmp(&head->mm_dbs, &pending->mm_dbs,
                            sizeof(head->mm_dbs)) == 0);
    mdbx_assert(env, memcmp(&head->mm_canary, &pending->mm_canary,
                            sizeof(head->mm_canary)) == 0);
    mdbx_assert(env, memcmp(&head->mm_geo, &pending->mm_geo,
                            sizeof(pending->mm_geo)) == 0);
    if (!META_IS_STEADY(head) && META_IS_STEADY(pending))
      target = head;
    else {
      mdbx_ensure(env, mdbx_meta_eq(env, head, pending));
      mdbx_debug("%s", "skip update meta");
      return MDBX_SUCCESS;
    }
  } else if (head == meta0)
    target = mdbx_meta_ancient(prefer_steady, env, meta1, meta2);
  else if (head == meta1)
    target = mdbx_meta_ancient(prefer_steady, env, meta0, meta2);
  else {
    mdbx_assert(env, head == meta2);
    target = mdbx_meta_ancient(prefer_steady, env, meta0, meta1);
  }

  /* LY: step#2 - update meta-page. */
  mdbx_debug(
      "writing meta%" PRIaPGNO " = root %" PRIaPGNO "/%" PRIaPGNO
      ", geo %" PRIaPGNO "/%" PRIaPGNO "-%" PRIaPGNO "/%" PRIaPGNO
      " +%u -%u, txn_id %" PRIaTXN ", %s",
      data_page(target)->mp_pgno, pending->mm_dbs[MAIN_DBI].md_root,
      pending->mm_dbs[FREE_DBI].md_root, pending->mm_geo.lower,
      pending->mm_geo.next, pending->mm_geo.now, pending->mm_geo.upper,
      pv2pages(pending->mm_geo.grow_pv), pv2pages(pending->mm_geo.shrink_pv),
      unaligned_peek_u64(4, pending->mm_txnid_a), mdbx_durable_str(pending));

  mdbx_debug("meta0: %s, %s, txn_id %" PRIaTXN ", root %" PRIaPGNO
             "/%" PRIaPGNO,
             (meta0 == head)     ? "head"
             : (meta0 == target) ? "tail"
                                 : "stay",
             mdbx_durable_str(meta0), mdbx_meta_txnid_fluid(env, meta0),
             meta0->mm_dbs[MAIN_DBI].md_root, meta0->mm_dbs[FREE_DBI].md_root);
  mdbx_debug("meta1: %s, %s, txn_id %" PRIaTXN ", root %" PRIaPGNO
             "/%" PRIaPGNO,
             (meta1 == head)     ? "head"
             : (meta1 == target) ? "tail"
                                 : "stay",
             mdbx_durable_str(meta1), mdbx_meta_txnid_fluid(env, meta1),
             meta1->mm_dbs[MAIN_DBI].md_root, meta1->mm_dbs[FREE_DBI].md_root);
  mdbx_debug("meta2: %s, %s, txn_id %" PRIaTXN ", root %" PRIaPGNO
             "/%" PRIaPGNO,
             (meta2 == head)     ? "head"
             : (meta2 == target) ? "tail"
                                 : "stay",
             mdbx_durable_str(meta2), mdbx_meta_txnid_fluid(env, meta2),
             meta2->mm_dbs[MAIN_DBI].md_root, meta2->mm_dbs[FREE_DBI].md_root);

  mdbx_assert(env, !mdbx_meta_eq(env, pending, meta0));
  mdbx_assert(env, !mdbx_meta_eq(env, pending, meta1));
  mdbx_assert(env, !mdbx_meta_eq(env, pending, meta2));

  mdbx_assert(env, ((env->me_flags ^ flags) & MDBX_WRITEMAP) == 0);
  mdbx_ensure(env,
              target == head || mdbx_meta_txnid_stable(env, target) <
                                    unaligned_peek_u64(4, pending->mm_txnid_a));
#if MDBX_ENABLE_PGOP_STAT
  env->me_lck->mti_pgop_stat.wops.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
  if (flags & MDBX_WRITEMAP) {
    mdbx_jitter4testing(true);
    if (likely(target != head)) {
      /* LY: 'invalidate' the meta. */
      mdbx_meta_update_begin(env, target,
                             unaligned_peek_u64(4, pending->mm_txnid_a));
      unaligned_poke_u64(4, target->mm_datasync_sign, MDBX_DATASIGN_WEAK);
#ifndef NDEBUG
      /* debug: provoke failure to catch a violators, but don't touch mm_psize
       * to allow readers catch actual pagesize. */
      uint8_t *provoke_begin = (uint8_t *)&target->mm_dbs[FREE_DBI].md_root;
      uint8_t *provoke_end = (uint8_t *)&target->mm_datasync_sign;
      memset(provoke_begin, 0xCC, provoke_end - provoke_begin);
      mdbx_jitter4testing(false);
#endif

      /* LY: update info */
      target->mm_geo = pending->mm_geo;
      target->mm_dbs[FREE_DBI] = pending->mm_dbs[FREE_DBI];
      target->mm_dbs[MAIN_DBI] = pending->mm_dbs[MAIN_DBI];
      target->mm_canary = pending->mm_canary;
      memcpy(target->mm_pages_retired, pending->mm_pages_retired, 8);
      mdbx_jitter4testing(true);

      /* LY: 'commit' the meta */
      mdbx_meta_update_end(env, target,
                           unaligned_peek_u64(4, pending->mm_txnid_b));
      mdbx_jitter4testing(true);
    } else {
      /* dangerous case (target == head), only mm_datasync_sign could
       * me updated, check assertions once again */
      mdbx_ensure(env, mdbx_meta_txnid_stable(env, head) ==
                               unaligned_peek_u64(4, pending->mm_txnid_a) &&
                           !META_IS_STEADY(head) && META_IS_STEADY(pending));
      mdbx_ensure(env, memcmp(&head->mm_geo, &pending->mm_geo,
                              sizeof(head->mm_geo)) == 0);
      mdbx_ensure(env, memcmp(&head->mm_dbs, &pending->mm_dbs,
                              sizeof(head->mm_dbs)) == 0);
      mdbx_ensure(env, memcmp(&head->mm_canary, &pending->mm_canary,
                              sizeof(head->mm_canary)) == 0);
    }
    memcpy(target->mm_datasync_sign, pending->mm_datasync_sign, 8);
    mdbx_flush_incoherent_cpu_writeback();
    mdbx_jitter4testing(true);
    /* sync meta-pages */
    rc =
        mdbx_msync(&env->me_dxb_mmap, 0, pgno_align2os_bytes(env, NUM_METAS),
                   (flags & MDBX_NOMETASYNC) ? MDBX_SYNC_NONE
                                             : MDBX_SYNC_DATA | MDBX_SYNC_IODQ);
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;
  } else {
    const MDBX_meta undo_meta = *target;
    const mdbx_filehandle_t fd = (env->me_dsync_fd != INVALID_HANDLE_VALUE)
                                     ? env->me_dsync_fd
                                     : env->me_lazy_fd;
#if MDBX_ENABLE_PGOP_STAT
    env->me_lck->mti_pgop_stat.wops.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
    rc = mdbx_pwrite(fd, pending, sizeof(MDBX_meta),
                     (uint8_t *)target - env->me_map);
    if (unlikely(rc != MDBX_SUCCESS)) {
    undo:
      mdbx_debug("%s", "write failed, disk error?");
      /* On a failure, the pagecache still contains the new data.
       * Try write some old data back, to prevent it from being used. */
      mdbx_pwrite(fd, &undo_meta, sizeof(MDBX_meta),
                  (uint8_t *)target - env->me_map);
      goto fail;
    }
    mdbx_flush_incoherent_mmap(target, sizeof(MDBX_meta), env->me_os_psize);
    /* sync meta-pages */
    if ((flags & MDBX_NOMETASYNC) == 0 && fd == env->me_lazy_fd) {
      rc = mdbx_fsync(env->me_lazy_fd, MDBX_SYNC_DATA | MDBX_SYNC_IODQ);
      if (rc != MDBX_SUCCESS)
        goto undo;
    }
  }
  env->me_lck->mti_meta_sync_txnid.weak =
      (uint32_t)unaligned_peek_u64(4, pending->mm_txnid_a) -
      ((flags & MDBX_NOMETASYNC) ? UINT32_MAX / 3 : 0);

  /* LY: shrink datafile if needed */
  if (unlikely(shrink)) {
    mdbx_verbose("shrink to %" PRIaPGNO " pages (-%" PRIaPGNO ")",
                 pending->mm_geo.now, shrink);
    rc = mdbx_mapresize_implicit(env, pending->mm_geo.next, pending->mm_geo.now,
                                 pending->mm_geo.upper);
    if (MDBX_IS_ERROR(rc))
      goto fail;
  }

  MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
  if (likely(lck))
    /* toggle oldest refresh */
    atomic_store32(&lck->mti_readers_refresh_flag, false, mo_Relaxed);

  return MDBX_SUCCESS;

fail:
  env->me_flags |= MDBX_FATAL_ERROR;
  return rc;
}

static void recalculate_merge_threshold(MDBX_env *env) {
  const unsigned bytes = page_space(env);
  env->me_merge_threshold =
      (uint16_t)(bytes -
                 (bytes * env->me_options.merge_threshold_16dot16_percent >>
                  16));
  env->me_merge_threshold_gc =
      (uint16_t)(bytes -
                 ((env->me_options.merge_threshold_16dot16_percent > 19005)
                      ? bytes / 3 /* 33 % */
                      : bytes / 4 /* 25 % */));
}

__cold static void mdbx_setup_pagesize(MDBX_env *env, const size_t pagesize) {
  STATIC_ASSERT(PTRDIFF_MAX > MAX_MAPSIZE);
  STATIC_ASSERT(MIN_PAGESIZE > sizeof(MDBX_page) + sizeof(MDBX_meta));
  mdbx_ensure(env, is_powerof2(pagesize));
  mdbx_ensure(env, pagesize >= MIN_PAGESIZE);
  mdbx_ensure(env, pagesize <= MAX_PAGESIZE);
  env->me_psize = (unsigned)pagesize;

  STATIC_ASSERT(MAX_GC1OVPAGE(MIN_PAGESIZE) > 4);
  STATIC_ASSERT(MAX_GC1OVPAGE(MAX_PAGESIZE) < MDBX_PGL_LIMIT / 4);
  const intptr_t maxgc_ov1page = (pagesize - PAGEHDRSZ) / sizeof(pgno_t) - 1;
  mdbx_ensure(env, maxgc_ov1page > 42 &&
                       maxgc_ov1page < (intptr_t)MDBX_PGL_LIMIT / 4);
  env->me_maxgc_ov1page = (unsigned)maxgc_ov1page;

  STATIC_ASSERT(LEAF_NODE_MAX(MIN_PAGESIZE) > sizeof(MDBX_db) + NODESIZE + 42);
  STATIC_ASSERT(LEAF_NODE_MAX(MAX_PAGESIZE) < UINT16_MAX);
  STATIC_ASSERT(LEAF_NODE_MAX(MIN_PAGESIZE) >= BRANCH_NODE_MAX(MIN_PAGESIZE));
  STATIC_ASSERT(BRANCH_NODE_MAX(MAX_PAGESIZE) > NODESIZE + 42);
  STATIC_ASSERT(BRANCH_NODE_MAX(MAX_PAGESIZE) < UINT16_MAX);
  const intptr_t branch_nodemax = BRANCH_NODE_MAX(pagesize);
  const intptr_t leaf_nodemax = LEAF_NODE_MAX(pagesize);
  mdbx_ensure(env,
              branch_nodemax > (intptr_t)(NODESIZE + 42) &&
                  branch_nodemax % 2 == 0 &&
                  leaf_nodemax > (intptr_t)(sizeof(MDBX_db) + NODESIZE + 42) &&
                  leaf_nodemax >= branch_nodemax &&
                  leaf_nodemax < (int)UINT16_MAX && leaf_nodemax % 2 == 0);
  env->me_leaf_nodemax = (unsigned)leaf_nodemax;
  env->me_psize2log = (uint8_t)log2n_powerof2(pagesize);
  mdbx_assert(env, pgno2bytes(env, 1) == pagesize);
  mdbx_assert(env, bytes2pgno(env, pagesize + pagesize) == 2);
  recalculate_merge_threshold(env);

  const pgno_t max_pgno = bytes2pgno(env, MAX_MAPSIZE);
  if (!env->me_options.flags.non_auto.dp_limit) {
    /* auto-setup dp_limit by "The42" ;-) */
    intptr_t total_ram_pages, avail_ram_pages;
    int err = mdbx_get_sysraminfo(nullptr, &total_ram_pages, &avail_ram_pages);
    if (unlikely(err != MDBX_SUCCESS))
      mdbx_error("mdbx_get_sysraminfo(), rc %d", err);
    else {
      size_t reasonable_dpl_limit =
          (size_t)(total_ram_pages + avail_ram_pages) / 42;
      if (pagesize > env->me_os_psize)
        reasonable_dpl_limit /= pagesize / env->me_os_psize;
      else if (pagesize < env->me_os_psize)
        reasonable_dpl_limit *= env->me_os_psize / pagesize;
      reasonable_dpl_limit = (reasonable_dpl_limit < MDBX_PGL_LIMIT)
                                 ? reasonable_dpl_limit
                                 : MDBX_PGL_LIMIT;
      reasonable_dpl_limit = (reasonable_dpl_limit > CURSOR_STACK * 4)
                                 ? reasonable_dpl_limit
                                 : CURSOR_STACK * 4;
      env->me_options.dp_limit = (unsigned)reasonable_dpl_limit;
    }
  }
  if (env->me_options.dp_limit > max_pgno - NUM_METAS)
    env->me_options.dp_limit = max_pgno - NUM_METAS;
  if (env->me_options.dp_initial > env->me_options.dp_limit)
    env->me_options.dp_initial = env->me_options.dp_limit;
}

static __inline MDBX_CONST_FUNCTION MDBX_lockinfo *
lckless_stub(const MDBX_env *env) {
  uintptr_t stub = (uintptr_t)&env->x_lckless_stub;
  /* align to avoid false-positive alarm from UndefinedBehaviorSanitizer */
  stub = (stub + MDBX_CACHELINE_SIZE - 1) & ~(MDBX_CACHELINE_SIZE - 1);
  return (MDBX_lockinfo *)stub;
}

__cold int mdbx_env_create(MDBX_env **penv) {
  MDBX_env *env = mdbx_calloc(1, sizeof(MDBX_env));
  if (unlikely(!env))
    return MDBX_ENOMEM;

  env->me_maxreaders = DEFAULT_READERS;
  env->me_maxdbs = env->me_numdbs = CORE_DBS;
  env->me_lazy_fd = INVALID_HANDLE_VALUE;
  env->me_dsync_fd = INVALID_HANDLE_VALUE;
  env->me_lfd = INVALID_HANDLE_VALUE;
  env->me_pid = mdbx_getpid();
  env->me_stuck_meta = -1;

  env->me_options.dp_reserve_limit = 1024;
  env->me_options.rp_augment_limit = 256 * 1024;
  env->me_options.dp_limit = 64 * 1024;
  if (env->me_options.dp_limit > MAX_PAGENO - NUM_METAS)
    env->me_options.dp_limit = MAX_PAGENO - NUM_METAS;
  env->me_options.dp_initial = MDBX_PNL_INITIAL;
  if (env->me_options.dp_initial > env->me_options.dp_limit)
    env->me_options.dp_initial = env->me_options.dp_limit;
  env->me_options.spill_max_denominator = 8;
  env->me_options.spill_min_denominator = 8;
  env->me_options.spill_parent4child_denominator = 0;
  env->me_options.dp_loose_limit = 64;
  env->me_options.merge_threshold_16dot16_percent = 65536 / 4 /* 25% */;

  int rc;
  const size_t os_psize = mdbx_syspagesize();
  if (unlikely(!is_powerof2(os_psize) || os_psize < MIN_PAGESIZE)) {
    mdbx_error("unsuitable system pagesize %" PRIuPTR, os_psize);
    rc = MDBX_INCOMPATIBLE;
    goto bailout;
  }
  env->me_os_psize = (unsigned)os_psize;
  mdbx_setup_pagesize(env, (env->me_os_psize < MAX_PAGESIZE) ? env->me_os_psize
                                                             : MAX_PAGESIZE);

  rc = mdbx_fastmutex_init(&env->me_dbi_lock);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

#if defined(_WIN32) || defined(_WIN64)
  mdbx_srwlock_Init(&env->me_remap_guard);
  InitializeCriticalSection(&env->me_windowsbug_lock);
#else
  rc = mdbx_fastmutex_init(&env->me_remap_guard);
  if (unlikely(rc != MDBX_SUCCESS)) {
    mdbx_fastmutex_destroy(&env->me_dbi_lock);
    goto bailout;
  }

#if MDBX_LOCKING > MDBX_LOCKING_SYSV
  MDBX_lockinfo *const stub = lckless_stub(env);
  rc = mdbx_ipclock_stub(&stub->mti_wlock);
#endif /* MDBX_LOCKING */
  if (unlikely(rc != MDBX_SUCCESS)) {
    mdbx_fastmutex_destroy(&env->me_remap_guard);
    mdbx_fastmutex_destroy(&env->me_dbi_lock);
    goto bailout;
  }
#endif /* Windows */

  VALGRIND_CREATE_MEMPOOL(env, 0, 0);
  env->me_signature.weak = MDBX_ME_SIGNATURE;
  *penv = env;
  return MDBX_SUCCESS;

bailout:
  mdbx_free(env);
  *penv = nullptr;
  return rc;
}

__cold static intptr_t get_reasonable_db_maxsize(intptr_t *cached_result) {
  if (*cached_result == 0) {
    intptr_t pagesize, total_ram_pages;
    if (unlikely(mdbx_get_sysraminfo(&pagesize, &total_ram_pages, nullptr) !=
                 MDBX_SUCCESS))
      return MAX_MAPSIZE32 /* the 32-bit limit is good enough for fallback */;

    if (unlikely((size_t)total_ram_pages * 2 > MAX_MAPSIZE / (size_t)pagesize))
      return MAX_MAPSIZE;
    assert(MAX_MAPSIZE >= (size_t)(total_ram_pages * pagesize * 2));

    /* Suggesting should not be more than golden ratio of the size of RAM. */
    *cached_result = (intptr_t)((size_t)total_ram_pages * 207 >> 7) * pagesize;

    /* Round to the nearest human-readable granulation. */
    for (size_t unit = MEGABYTE; unit; unit <<= 5) {
      const size_t floor = floor_powerof2(*cached_result, unit);
      const size_t ceil = ceil_powerof2(*cached_result, unit);
      const size_t threshold = (size_t)*cached_result >> 4;
      const bool down =
          *cached_result - floor < ceil - *cached_result || ceil > MAX_MAPSIZE;
      if (threshold < (down ? *cached_result - floor : ceil - *cached_result))
        break;
      *cached_result = down ? floor : ceil;
    }
  }
  return *cached_result;
}

__cold LIBMDBX_API int
mdbx_env_set_geometry(MDBX_env *env, intptr_t size_lower, intptr_t size_now,
                      intptr_t size_upper, intptr_t growth_step,
                      intptr_t shrink_threshold, intptr_t pagesize) {
  int rc = check_env(env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const bool inside_txn =
      (env->me_txn0 && env->me_txn0->mt_owner == mdbx_thread_self());

#if MDBX_DEBUG
  if (growth_step < 0) {
    growth_step = 1;
    if (shrink_threshold < 0)
      shrink_threshold = 1;
  }
#endif /* MDBX_DEBUG */

  intptr_t reasonable_maxsize = 0;
  bool need_unlock = false;
  if (env->me_map) {
    /* env already mapped */
    if (unlikely(env->me_flags & MDBX_RDONLY))
      return MDBX_EACCESS;

    if (!inside_txn) {
      int err = mdbx_txn_lock(env, false);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      need_unlock = true;
    }
    MDBX_meta *head = mdbx_meta_head(env);
    if (!inside_txn) {
      env->me_txn0->mt_txnid = meta_txnid(env, head, false);
      mdbx_find_oldest(env->me_txn0);
    }

    /* get untouched params from DB */
    if (pagesize <= 0 || pagesize >= INT_MAX)
      pagesize = env->me_psize;
    if (size_lower < 0)
      size_lower = pgno2bytes(env, head->mm_geo.lower);
    if (size_now < 0)
      size_now = pgno2bytes(env, head->mm_geo.now);
    if (size_upper < 0)
      size_upper = pgno2bytes(env, head->mm_geo.upper);
    if (growth_step < 0)
      growth_step = pgno2bytes(env, pv2pages(head->mm_geo.grow_pv));
    if (shrink_threshold < 0)
      shrink_threshold = pgno2bytes(env, pv2pages(head->mm_geo.shrink_pv));

    if (pagesize != (intptr_t)env->me_psize) {
      rc = MDBX_EINVAL;
      goto bailout;
    }
    const size_t usedbytes =
        pgno2bytes(env, mdbx_find_largest(env, head->mm_geo.next));
    if ((size_t)size_upper < usedbytes) {
      rc = MDBX_MAP_FULL;
      goto bailout;
    }
    if ((size_t)size_now < usedbytes)
      size_now = usedbytes;
  } else {
    /* env NOT yet mapped */
    if (unlikely(inside_txn))
      return MDBX_PANIC;

    /* is requested some auto-value for pagesize ? */
    if (pagesize >= INT_MAX /* maximal */)
      pagesize = MAX_PAGESIZE;
    else if (pagesize <= 0) {
      if (pagesize < 0 /* default */) {
        pagesize = env->me_os_psize;
        if ((uintptr_t)pagesize > MAX_PAGESIZE)
          pagesize = MAX_PAGESIZE;
        mdbx_assert(env, (uintptr_t)pagesize >= MIN_PAGESIZE);
      } else if (pagesize == 0 /* minimal */)
        pagesize = MIN_PAGESIZE;

      /* choose pagesize */
      intptr_t max_size = (size_now > size_lower) ? size_now : size_lower;
      max_size = (size_upper > max_size) ? size_upper : max_size;
      if (max_size < 0 /* default */)
        max_size = DEFAULT_MAPSIZE;
      else if (max_size == 0 /* minimal */)
        max_size = MIN_MAPSIZE;
      else if (max_size >= (intptr_t)MAX_MAPSIZE /* maximal */)
        max_size = get_reasonable_db_maxsize(&reasonable_maxsize);

      while (max_size > pagesize * (int64_t)MAX_PAGENO &&
             pagesize < MAX_PAGESIZE)
        pagesize <<= 1;
    }
  }

  if (pagesize < (intptr_t)MIN_PAGESIZE || pagesize > (intptr_t)MAX_PAGESIZE ||
      !is_powerof2(pagesize)) {
    rc = MDBX_EINVAL;
    goto bailout;
  }

  if (size_lower <= 0) {
    size_lower = MIN_MAPSIZE;
    if (MIN_MAPSIZE / pagesize < MIN_PAGENO)
      size_lower = MIN_PAGENO * pagesize;
  }
  if (size_lower >= INTPTR_MAX) {
    size_lower = get_reasonable_db_maxsize(&reasonable_maxsize);
    if ((size_t)size_lower / pagesize > MAX_PAGENO)
      size_lower = pagesize * MAX_PAGENO;
  }

  if (size_now <= 0) {
    size_now = size_lower;
    if (size_upper >= size_lower && size_now > size_upper)
      size_now = size_upper;
  }
  if (size_now >= INTPTR_MAX) {
    size_now = get_reasonable_db_maxsize(&reasonable_maxsize);
    if ((size_t)size_now / pagesize > MAX_PAGENO)
      size_now = pagesize * MAX_PAGENO;
  }

  if (size_upper <= 0) {
    if (size_now >= get_reasonable_db_maxsize(&reasonable_maxsize) / 2)
      size_upper = get_reasonable_db_maxsize(&reasonable_maxsize);
    else if (MAX_MAPSIZE != MAX_MAPSIZE32 &&
             (size_t)size_now >= MAX_MAPSIZE32 / 2 &&
             (size_t)size_now <= MAX_MAPSIZE32 / 4 * 3)
      size_upper = MAX_MAPSIZE32;
    else {
      size_upper = size_now + size_now;
      if ((size_t)size_upper < DEFAULT_MAPSIZE * 2)
        size_upper = DEFAULT_MAPSIZE * 2;
    }
    if ((size_t)size_upper / pagesize > MAX_PAGENO)
      size_upper = pagesize * MAX_PAGENO;
  } else if (size_upper >= INTPTR_MAX) {
    size_upper = get_reasonable_db_maxsize(&reasonable_maxsize);
    if ((size_t)size_upper / pagesize > MAX_PAGENO)
      size_upper = pagesize * MAX_PAGENO;
  }

  if (unlikely(size_lower < (intptr_t)MIN_MAPSIZE || size_lower > size_upper)) {
    rc = MDBX_EINVAL;
    goto bailout;
  }

  if ((uint64_t)size_lower / pagesize < MIN_PAGENO) {
    rc = MDBX_EINVAL;
    goto bailout;
  }

  if (unlikely((size_t)size_upper > MAX_MAPSIZE ||
               (uint64_t)size_upper / pagesize > MAX_PAGENO)) {
    rc = MDBX_TOO_LARGE;
    goto bailout;
  }

  const size_t unit = (env->me_os_psize > (size_t)pagesize) ? env->me_os_psize
                                                            : (size_t)pagesize;
  size_lower = ceil_powerof2(size_lower, unit);
  size_upper = ceil_powerof2(size_upper, unit);
  size_now = ceil_powerof2(size_now, unit);

  /* LY: подбираем значение size_upper:
   *  - кратное размеру страницы
   *  - без нарушения MAX_MAPSIZE и MAX_PAGENO */
  while (unlikely((size_t)size_upper > MAX_MAPSIZE ||
                  (uint64_t)size_upper / pagesize > MAX_PAGENO)) {
    if ((size_t)size_upper < unit + MIN_MAPSIZE ||
        (size_t)size_upper < (size_t)pagesize * (MIN_PAGENO + 1)) {
      /* паранойа на случай переполнения при невероятных значениях */
      rc = MDBX_EINVAL;
      goto bailout;
    }
    size_upper -= unit;
    if ((size_t)size_upper < (size_t)size_lower)
      size_lower = size_upper;
  }
  mdbx_assert(env, (size_upper - size_lower) % env->me_os_psize == 0);

  if (size_now < size_lower)
    size_now = size_lower;
  if (size_now > size_upper)
    size_now = size_upper;

  if (growth_step < 0) {
    growth_step = ((size_t)(size_upper - size_lower)) / 42;
    if (growth_step > size_lower && size_lower < (intptr_t)MEGABYTE)
      growth_step = size_lower;
    if (growth_step < 65536)
      growth_step = 65536;
    if ((size_t)growth_step > MAX_MAPSIZE / 64)
      growth_step = MAX_MAPSIZE / 64;
  }
  if (growth_step == 0 && shrink_threshold > 0)
    growth_step = 1;
  growth_step = ceil_powerof2(growth_step, unit);

  if (shrink_threshold < 0)
    shrink_threshold = growth_step + growth_step;
  shrink_threshold = ceil_powerof2(shrink_threshold, unit);

  //----------------------------------------------------------------------------

  if (!env->me_map) {
    /* save user's geo-params for future open/create */
    if (pagesize != (intptr_t)env->me_psize)
      mdbx_setup_pagesize(env, pagesize);
    env->me_dbgeo.lower = size_lower;
    env->me_dbgeo.now = size_now;
    env->me_dbgeo.upper = size_upper;
    env->me_dbgeo.grow =
        pgno2bytes(env, pv2pages(pages2pv(bytes2pgno(env, growth_step))));
    env->me_dbgeo.shrink =
        pgno2bytes(env, pv2pages(pages2pv(bytes2pgno(env, shrink_threshold))));

    mdbx_ensure(env, env->me_dbgeo.lower >= MIN_MAPSIZE);
    mdbx_ensure(env, env->me_dbgeo.lower / (unsigned)pagesize >= MIN_PAGENO);
    mdbx_ensure(env, env->me_dbgeo.lower % (unsigned)pagesize == 0);
    mdbx_ensure(env, env->me_dbgeo.lower % env->me_os_psize == 0);

    mdbx_ensure(env, env->me_dbgeo.upper <= MAX_MAPSIZE);
    mdbx_ensure(env, env->me_dbgeo.upper / (unsigned)pagesize <= MAX_PAGENO);
    mdbx_ensure(env, env->me_dbgeo.upper % (unsigned)pagesize == 0);
    mdbx_ensure(env, env->me_dbgeo.upper % env->me_os_psize == 0);

    mdbx_ensure(env, env->me_dbgeo.now >= env->me_dbgeo.lower);
    mdbx_ensure(env, env->me_dbgeo.now <= env->me_dbgeo.upper);
    mdbx_ensure(env, env->me_dbgeo.now % (unsigned)pagesize == 0);
    mdbx_ensure(env, env->me_dbgeo.now % env->me_os_psize == 0);

    mdbx_ensure(env, env->me_dbgeo.grow % (unsigned)pagesize == 0);
    mdbx_ensure(env, env->me_dbgeo.grow % env->me_os_psize == 0);
    mdbx_ensure(env, env->me_dbgeo.shrink % (unsigned)pagesize == 0);
    mdbx_ensure(env, env->me_dbgeo.shrink % env->me_os_psize == 0);

    rc = MDBX_SUCCESS;
  } else {
    /* apply new params to opened environment */
    mdbx_ensure(env, pagesize == (intptr_t)env->me_psize);
    MDBX_meta meta;
    MDBX_meta *head = nullptr;
    const MDBX_geo *current_geo;
    if (inside_txn) {
      current_geo = &env->me_txn->mt_geo;
    } else {
      head = mdbx_meta_head(env);
      meta = *head;
      current_geo = &meta.mm_geo;
    }

    MDBX_geo new_geo;
    new_geo.lower = bytes2pgno(env, size_lower);
    new_geo.now = bytes2pgno(env, size_now);
    new_geo.upper = bytes2pgno(env, size_upper);
    new_geo.grow_pv = pages2pv(bytes2pgno(env, growth_step));
    new_geo.shrink_pv = pages2pv(bytes2pgno(env, shrink_threshold));
    new_geo.next = current_geo->next;

    mdbx_ensure(env,
                pgno_align2os_bytes(env, new_geo.lower) == (size_t)size_lower);
    mdbx_ensure(env,
                pgno_align2os_bytes(env, new_geo.upper) == (size_t)size_upper);
    mdbx_ensure(env, pgno_align2os_bytes(env, new_geo.now) == (size_t)size_now);
    mdbx_ensure(env, new_geo.grow_pv == pages2pv(pv2pages(new_geo.grow_pv)));
    mdbx_ensure(env,
                new_geo.shrink_pv == pages2pv(pv2pages(new_geo.shrink_pv)));

    mdbx_ensure(env, (size_t)size_lower >= MIN_MAPSIZE);
    mdbx_ensure(env, new_geo.lower >= MIN_PAGENO);
    mdbx_ensure(env, (size_t)size_upper <= MAX_MAPSIZE);
    mdbx_ensure(env, new_geo.upper <= MAX_PAGENO);
    mdbx_ensure(env, new_geo.now >= new_geo.next);
    mdbx_ensure(env, new_geo.upper >= new_geo.now);
    mdbx_ensure(env, new_geo.now >= new_geo.lower);

    if (memcmp(current_geo, &new_geo, sizeof(MDBX_geo)) != 0) {
#if defined(_WIN32) || defined(_WIN64)
      /* Was DB shrinking disabled before and now it will be enabled? */
      if (new_geo.lower < new_geo.upper && new_geo.shrink_pv &&
          !(current_geo->lower < current_geo->upper &&
            current_geo->shrink_pv)) {
        if (!env->me_lck_mmap.lck) {
          rc = MDBX_EPERM;
          goto bailout;
        }
        int err = mdbx_rdt_lock(env);
        if (unlikely(MDBX_IS_ERROR(err))) {
          rc = err;
          goto bailout;
        }

        /* Check if there are any reading threads that do not use the SRWL */
        const size_t CurrentTid = GetCurrentThreadId();
        const MDBX_reader *const begin = env->me_lck_mmap.lck->mti_readers;
        const MDBX_reader *const end =
            begin + atomic_load32(&env->me_lck_mmap.lck->mti_numreaders,
                                  mo_AcquireRelease);
        for (const MDBX_reader *reader = begin; reader < end; ++reader) {
          if (reader->mr_pid.weak == env->me_pid && reader->mr_tid.weak &&
              reader->mr_tid.weak != CurrentTid) {
            /* At least one thread may don't use SRWL */
            rc = MDBX_EPERM;
            break;
          }
        }

        mdbx_rdt_unlock(env);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      }
#endif

      if (new_geo.now != current_geo->now ||
          new_geo.upper != current_geo->upper) {
        rc = mdbx_mapresize(env, current_geo->next, new_geo.now, new_geo.upper,
                            false);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
        mdbx_assert(env, (head == nullptr) == inside_txn);
        if (head)
          head = /* base address could be changed */ mdbx_meta_head(env);
      }
      if (inside_txn) {
        env->me_txn->mt_geo = new_geo;
        env->me_txn->mt_flags |= MDBX_TXN_DIRTY;
      } else {
        meta.mm_geo = new_geo;
        const txnid_t txnid =
            safe64_txnid_next(mdbx_meta_txnid_stable(env, head));
        if (unlikely(txnid > MAX_TXNID)) {
          rc = MDBX_TXN_FULL;
          mdbx_error("txnid overflow, raise %d", rc);
        } else {
          mdbx_meta_set_txnid(env, &meta, txnid);
          rc = mdbx_sync_locked(env, env->me_flags, &meta);
        }
      }

      if (likely(rc == MDBX_SUCCESS)) {
        /* store new geo to env to avoid influences */
        env->me_dbgeo.now = pgno2bytes(env, new_geo.now);
        env->me_dbgeo.lower = pgno2bytes(env, new_geo.lower);
        env->me_dbgeo.upper = pgno2bytes(env, new_geo.upper);
        env->me_dbgeo.grow = pgno2bytes(env, pv2pages(new_geo.grow_pv));
        env->me_dbgeo.shrink = pgno2bytes(env, pv2pages(new_geo.shrink_pv));
      }
    }
  }

bailout:
  if (need_unlock)
    mdbx_txn_unlock(env);
  return rc;
}

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
__cold int mdbx_env_set_mapsize(MDBX_env *env, size_t size) {
  return __inline_mdbx_env_set_mapsize(env, size);
}

__cold int mdbx_env_set_maxdbs(MDBX_env *env, MDBX_dbi dbs) {
  return __inline_mdbx_env_set_maxdbs(env, dbs);
}

__cold int mdbx_env_get_maxdbs(const MDBX_env *env, MDBX_dbi *dbs) {
  return __inline_mdbx_env_get_maxdbs(env, dbs);
}

__cold int mdbx_env_set_maxreaders(MDBX_env *env, unsigned readers) {
  return __inline_mdbx_env_set_maxreaders(env, readers);
}

__cold int mdbx_env_get_maxreaders(const MDBX_env *env, unsigned *readers) {
  return __inline_mdbx_env_get_maxreaders(env, readers);
}
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

__cold static int alloc_page_buf(MDBX_env *env) {
  return env->me_pbuf
             ? MDBX_SUCCESS
             : mdbx_memalign_alloc(env->me_os_psize, env->me_psize * NUM_METAS,
                                   &env->me_pbuf);
}

/* Further setup required for opening an MDBX environment */
__cold static int mdbx_setup_dxb(MDBX_env *env, const int lck_rc,
                                 const mdbx_mode_t mode_bits) {
  MDBX_meta meta;
  int rc = MDBX_RESULT_FALSE;
  int err = mdbx_read_header(env, &meta, lck_rc, mode_bits);
  if (unlikely(err != MDBX_SUCCESS)) {
    if (lck_rc != /* lck exclusive */ MDBX_RESULT_TRUE || err != MDBX_ENODATA ||
        (env->me_flags & MDBX_RDONLY) != 0 ||
        /* recovery mode */ env->me_stuck_meta >= 0)
      return err;

    mdbx_debug("%s", "create new database");
    rc = /* new database */ MDBX_RESULT_TRUE;

    if (!env->me_dbgeo.now) {
      /* set defaults if not configured */
      err = mdbx_env_set_geometry(env, 0, -1, DEFAULT_MAPSIZE, -1, -1, -1);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }

    err = alloc_page_buf(env);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    meta = *mdbx_init_metas(env, env->me_pbuf);
    err = mdbx_pwrite(env->me_lazy_fd, env->me_pbuf, env->me_psize * NUM_METAS,
                      0);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    err = mdbx_ftruncate(env->me_lazy_fd,
                         env->me_dxb_mmap.filesize = env->me_dbgeo.now);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

#ifndef NDEBUG /* just for checking */
    err = mdbx_read_header(env, &meta, lck_rc, mode_bits);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
#endif
  }

  mdbx_verbose(
      "header: root %" PRIaPGNO "/%" PRIaPGNO ", geo %" PRIaPGNO "/%" PRIaPGNO
      "-%" PRIaPGNO "/%" PRIaPGNO " +%u -%u, txn_id %" PRIaTXN ", %s",
      meta.mm_dbs[MAIN_DBI].md_root, meta.mm_dbs[FREE_DBI].md_root,
      meta.mm_geo.lower, meta.mm_geo.next, meta.mm_geo.now, meta.mm_geo.upper,
      pv2pages(meta.mm_geo.grow_pv), pv2pages(meta.mm_geo.shrink_pv),
      unaligned_peek_u64(4, meta.mm_txnid_a), mdbx_durable_str(&meta));

  mdbx_setup_pagesize(env, meta.mm_psize);
  const size_t used_bytes = pgno2bytes(env, meta.mm_geo.next);
  const size_t used_aligned2os_bytes =
      ceil_powerof2(used_bytes, env->me_os_psize);
  if ((env->me_flags & MDBX_RDONLY) /* readonly */
      || lck_rc != MDBX_RESULT_TRUE /* not exclusive */
      || /* recovery mode */ env->me_stuck_meta >= 0) {
    /* use present params from db */
    const size_t pagesize = meta.mm_psize;
    err = mdbx_env_set_geometry(
        env, meta.mm_geo.lower * pagesize, meta.mm_geo.now * pagesize,
        meta.mm_geo.upper * pagesize, pv2pages(meta.mm_geo.grow_pv) * pagesize,
        pv2pages(meta.mm_geo.shrink_pv) * pagesize, meta.mm_psize);
    if (unlikely(err != MDBX_SUCCESS)) {
      mdbx_error("%s: err %d", "could not apply preconfigured geometry from db",
                 err);
      return (err == MDBX_EINVAL) ? MDBX_INCOMPATIBLE : err;
    }
  } else if (env->me_dbgeo.now) {
    /* silently growth to last used page */
    if (env->me_dbgeo.now < used_aligned2os_bytes)
      env->me_dbgeo.now = used_aligned2os_bytes;
    if (env->me_dbgeo.upper < used_aligned2os_bytes)
      env->me_dbgeo.upper = used_aligned2os_bytes;

    /* apply preconfigured params, but only if substantial changes:
     *  - upper or lower limit changes
     *  - shrink threshold or growth step
     * But ignore change just a 'now/current' size. */
    if (bytes_align2os_bytes(env, env->me_dbgeo.upper) !=
            pgno2bytes(env, meta.mm_geo.upper) ||
        bytes_align2os_bytes(env, env->me_dbgeo.lower) !=
            pgno2bytes(env, meta.mm_geo.lower) ||
        bytes_align2os_bytes(env, env->me_dbgeo.shrink) !=
            pgno2bytes(env, pv2pages(meta.mm_geo.shrink_pv)) ||
        bytes_align2os_bytes(env, env->me_dbgeo.grow) !=
            pgno2bytes(env, pv2pages(meta.mm_geo.grow_pv))) {

      if (env->me_dbgeo.shrink && env->me_dbgeo.now > used_bytes)
        /* pre-shrink if enabled */
        env->me_dbgeo.now = used_bytes + env->me_dbgeo.shrink -
                            used_bytes % env->me_dbgeo.shrink;

      err = mdbx_env_set_geometry(env, env->me_dbgeo.lower, env->me_dbgeo.now,
                                  env->me_dbgeo.upper, env->me_dbgeo.grow,
                                  env->me_dbgeo.shrink, meta.mm_psize);
      if (unlikely(err != MDBX_SUCCESS)) {
        mdbx_error("%s: err %d", "could not apply preconfigured db-geometry",
                   err);
        return (err == MDBX_EINVAL) ? MDBX_INCOMPATIBLE : err;
      }

      /* update meta fields */
      meta.mm_geo.now = bytes2pgno(env, env->me_dbgeo.now);
      meta.mm_geo.lower = bytes2pgno(env, env->me_dbgeo.lower);
      meta.mm_geo.upper = bytes2pgno(env, env->me_dbgeo.upper);
      meta.mm_geo.grow_pv = pages2pv(bytes2pgno(env, env->me_dbgeo.grow));
      meta.mm_geo.shrink_pv = pages2pv(bytes2pgno(env, env->me_dbgeo.shrink));

      mdbx_verbose("amended: root %" PRIaPGNO "/%" PRIaPGNO ", geo %" PRIaPGNO
                   "/%" PRIaPGNO "-%" PRIaPGNO "/%" PRIaPGNO
                   " +%u -%u, txn_id %" PRIaTXN ", %s",
                   meta.mm_dbs[MAIN_DBI].md_root, meta.mm_dbs[FREE_DBI].md_root,
                   meta.mm_geo.lower, meta.mm_geo.next, meta.mm_geo.now,
                   meta.mm_geo.upper, pv2pages(meta.mm_geo.grow_pv),
                   pv2pages(meta.mm_geo.shrink_pv),
                   unaligned_peek_u64(4, meta.mm_txnid_a),
                   mdbx_durable_str(&meta));
    } else {
      /* fetch back 'now/current' size, since it was ignored during comparison
       * and may differ. */
      env->me_dbgeo.now = pgno_align2os_bytes(env, meta.mm_geo.now);
    }
    mdbx_ensure(env, meta.mm_geo.now >= meta.mm_geo.next);
  } else {
    /* geo-params are not pre-configured by user,
     * get current values from the meta. */
    env->me_dbgeo.now = pgno2bytes(env, meta.mm_geo.now);
    env->me_dbgeo.lower = pgno2bytes(env, meta.mm_geo.lower);
    env->me_dbgeo.upper = pgno2bytes(env, meta.mm_geo.upper);
    env->me_dbgeo.grow = pgno2bytes(env, pv2pages(meta.mm_geo.grow_pv));
    env->me_dbgeo.shrink = pgno2bytes(env, pv2pages(meta.mm_geo.shrink_pv));
  }

  mdbx_ensure(env,
              pgno_align2os_bytes(env, meta.mm_geo.now) == env->me_dbgeo.now);
  mdbx_ensure(env, env->me_dbgeo.now >= used_bytes);
  const uint64_t filesize_before = env->me_dxb_mmap.filesize;
  if (unlikely(filesize_before != env->me_dbgeo.now)) {
    if (lck_rc != /* lck exclusive */ MDBX_RESULT_TRUE) {
      mdbx_verbose("filesize mismatch (expect %" PRIuPTR "b/%" PRIaPGNO
                   "p, have %" PRIu64 "b/%" PRIaPGNO "p), "
                   "assume other process working",
                   env->me_dbgeo.now, bytes2pgno(env, env->me_dbgeo.now),
                   filesize_before, bytes2pgno(env, (size_t)filesize_before));
    } else {
      mdbx_warning("filesize mismatch (expect %" PRIuSIZE "b/%" PRIaPGNO
                   "p, have %" PRIu64 "b/%" PRIaPGNO "p)",
                   env->me_dbgeo.now, bytes2pgno(env, env->me_dbgeo.now),
                   filesize_before, bytes2pgno(env, (size_t)filesize_before));
      if (filesize_before < used_bytes) {
        mdbx_error("last-page beyond end-of-file (last %" PRIaPGNO
                   ", have %" PRIaPGNO ")",
                   meta.mm_geo.next, bytes2pgno(env, (size_t)filesize_before));
        return MDBX_CORRUPTED;
      }

      if (env->me_flags & MDBX_RDONLY) {
        if (filesize_before & (env->me_os_psize - 1)) {
          mdbx_error("%s", "filesize should be rounded-up to system page");
          return MDBX_WANNA_RECOVERY;
        }
        mdbx_warning("%s", "ignore filesize mismatch in readonly-mode");
      } else {
        mdbx_verbose("will resize datafile to %" PRIuSIZE " bytes, %" PRIaPGNO
                     " pages",
                     env->me_dbgeo.now, bytes2pgno(env, env->me_dbgeo.now));
      }
    }
  }

  mdbx_verbose("current boot-id %" PRIx64 "-%" PRIx64 " (%savailable)",
               bootid.x, bootid.y, (bootid.x | bootid.y) ? "" : "not-");

#if MDBX_ENABLE_MADVISE
  /* calculate readahead hint before mmap with zero redundant pages */
  const bool readahead =
      !(env->me_flags & MDBX_NORDAHEAD) &&
      mdbx_is_readahead_reasonable(used_bytes, 0) == MDBX_RESULT_TRUE;
#endif /* MDBX_ENABLE_MADVISE */

  err = mdbx_mmap(env->me_flags, &env->me_dxb_mmap, env->me_dbgeo.now,
                  env->me_dbgeo.upper, lck_rc ? MMAP_OPTION_TRUNCATE : 0);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

#if MDBX_ENABLE_MADVISE
#if defined(MADV_DONTDUMP)
  err = madvise(env->me_map, env->me_dxb_mmap.limit, MADV_DONTDUMP)
            ? ignore_enosys(errno)
            : MDBX_SUCCESS;
  if (unlikely(MDBX_IS_ERROR(err)))
    return err;
#endif /* MADV_DONTDUMP */
#if defined(MADV_DODUMP)
  if (mdbx_runtime_flags & MDBX_DBG_DUMP) {
    const size_t meta_length_aligned2os = pgno_align2os_bytes(env, NUM_METAS);
    err = madvise(env->me_map, meta_length_aligned2os, MADV_DODUMP)
              ? ignore_enosys(errno)
              : MDBX_SUCCESS;
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
  }
#endif /* MADV_DODUMP */
#endif /* MDBX_ENABLE_MADVISE */

#ifdef MDBX_USE_VALGRIND
  env->me_valgrind_handle =
      VALGRIND_CREATE_BLOCK(env->me_map, env->me_dxb_mmap.limit, "mdbx");
#endif /* MDBX_USE_VALGRIND */

  mdbx_assert(env, used_bytes >= pgno2bytes(env, NUM_METAS) &&
                       used_bytes <= env->me_dxb_mmap.limit);
#if defined(MDBX_USE_VALGRIND) || defined(__SANITIZE_ADDRESS__)
  if (env->me_dxb_mmap.filesize > used_bytes &&
      env->me_dxb_mmap.filesize < env->me_dxb_mmap.limit) {
    VALGRIND_MAKE_MEM_NOACCESS(env->me_map + used_bytes,
                               env->me_dxb_mmap.filesize - used_bytes);
    MDBX_ASAN_POISON_MEMORY_REGION(env->me_map + used_bytes,
                                   env->me_dxb_mmap.filesize - used_bytes);
  }
  env->me_poison_edge =
      bytes2pgno(env, (env->me_dxb_mmap.filesize < env->me_dxb_mmap.limit)
                          ? env->me_dxb_mmap.filesize
                          : env->me_dxb_mmap.limit);
#endif /* MDBX_USE_VALGRIND || __SANITIZE_ADDRESS__ */

  //-------------------------------- validate/rollback head & steady meta-pages
  if (unlikely(env->me_stuck_meta >= 0)) {
    /* recovery mode */
    MDBX_meta clone;
    MDBX_meta const *const target = METAPAGE(env, env->me_stuck_meta);
    err = mdbx_validate_meta_copy(env, target, &clone);
    if (unlikely(err != MDBX_SUCCESS)) {
      mdbx_error("target meta[%u] is corrupted",
                 bytes2pgno(env, (uint8_t *)data_page(target) - env->me_map));
      return MDBX_CORRUPTED;
    }
  } else /* not recovery mode */
    while (1) {
      const unsigned meta_clash_mask = mdbx_meta_eq_mask(env);
      if (unlikely(meta_clash_mask)) {
        mdbx_error("meta-pages are clashed: mask 0x%d", meta_clash_mask);
        return MDBX_CORRUPTED;
      }

      if (lck_rc != /* lck exclusive */ MDBX_RESULT_TRUE) {
        /* non-exclusive mode,
         * meta-pages should be validated by a first process opened the DB */
        MDBX_meta *const head = mdbx_meta_head(env);
        MDBX_meta *const steady = mdbx_meta_steady(env);
        const txnid_t head_txnid = mdbx_meta_txnid_fluid(env, head);
        const txnid_t steady_txnid = mdbx_meta_txnid_fluid(env, steady);
        if (head_txnid == steady_txnid)
          break;

        if (!env->me_lck_mmap.lck) {
          /* LY: without-lck (read-only) mode, so it is impossible that other
           * process made weak checkpoint. */
          mdbx_error("%s", "without-lck, unable recovery/rollback");
          return MDBX_WANNA_RECOVERY;
        }

        /* LY: assume just have a collision with other running process,
         *     or someone make a weak checkpoint */
        mdbx_verbose("%s", "assume collision or online weak checkpoint");
        break;
      }
      mdbx_assert(env, lck_rc == MDBX_RESULT_TRUE);
      /* exclusive mode */

      MDBX_meta clone;
      MDBX_meta const *const steady = mdbx_meta_steady(env);
      MDBX_meta const *const head = mdbx_meta_head(env);
      const txnid_t steady_txnid = mdbx_meta_txnid_fluid(env, steady);
      if (META_IS_STEADY(steady)) {
        err = mdbx_validate_meta_copy(env, steady, &clone);
        if (unlikely(err != MDBX_SUCCESS)) {
          mdbx_error("meta[%u] with %s txnid %" PRIaTXN
                     " is corrupted, %s needed",
                     bytes2pgno(env, (uint8_t *)steady - env->me_map), "steady",
                     steady_txnid, "manual recovery");
          return MDBX_CORRUPTED;
        }
        if (steady == head)
          break;
      }

      const pgno_t pgno = bytes2pgno(env, (uint8_t *)head - env->me_map);
      const txnid_t head_txnid = mdbx_meta_txnid_fluid(env, head);
      const bool head_valid =
          mdbx_validate_meta_copy(env, head, &clone) == MDBX_SUCCESS;
      mdbx_assert(env, !META_IS_STEADY(steady) || head_txnid != steady_txnid);
      if (unlikely(!head_valid)) {
        if (unlikely(!META_IS_STEADY(steady))) {
          mdbx_error("%s for open or automatic rollback, %s",
                     "there are no suitable meta-pages",
                     "manual recovery is required");
          return MDBX_CORRUPTED;
        }
        mdbx_warning("meta[%u] with last txnid %" PRIaTXN
                     " is corrupted, rollback needed",
                     pgno, head_txnid);
        goto purge_meta_head;
      }

      if (meta_bootid_match(head)) {
        if (env->me_flags & MDBX_RDONLY) {
          mdbx_error("%s, but boot-id(%016" PRIx64 "-%016" PRIx64 ") is MATCH: "
                     "rollback NOT needed, steady-sync NEEDED%s",
                     "opening after an unclean shutdown", bootid.x, bootid.y,
                     ", but unable in read-only mode");
          return MDBX_WANNA_RECOVERY;
        }
        mdbx_warning("%s, but boot-id(%016" PRIx64 "-%016" PRIx64 ") is MATCH: "
                     "rollback NOT needed, steady-sync NEEDED%s",
                     "opening after an unclean shutdown", bootid.x, bootid.y,
                     "");
        meta = clone;
        atomic_store32(&env->me_lck->mti_unsynced_pages, meta.mm_geo.next,
                       mo_Relaxed);
        break;
      }
      if (unlikely(!META_IS_STEADY(steady))) {
        mdbx_error("%s, but %s for automatic rollback: %s",
                   "opening after an unclean shutdown",
                   "there are no suitable meta-pages",
                   "manual recovery is required");
        return MDBX_CORRUPTED;
      }
      if (env->me_flags & MDBX_RDONLY) {
        mdbx_error("%s and rollback needed: (from head %" PRIaTXN
                   " to steady %" PRIaTXN ")%s",
                   "opening after an unclean shutdown", head_txnid,
                   steady_txnid, ", but unable in read-only mode");
        return MDBX_WANNA_RECOVERY;
      }

    purge_meta_head:
      mdbx_notice("%s and doing automatic rollback: "
                  "purge%s meta[%u] with%s txnid %" PRIaTXN,
                  "opening after an unclean shutdown",
                  head_valid ? "" : " invalid", pgno, head_valid ? " weak" : "",
                  head_txnid);
      mdbx_ensure(env, META_IS_STEADY(steady));
      err = mdbx_override_meta(env, pgno, 0, head_valid ? head : steady);
      if (err) {
        mdbx_error("rollback: overwrite meta[%u] with txnid %" PRIaTXN
                   ", error %d",
                   pgno, head_txnid, err);
        return err;
      }
      mdbx_ensure(env, 0 == mdbx_meta_txnid_fluid(env, head));
      mdbx_ensure(env, 0 == mdbx_meta_eq_mask(env));
    }

  if (lck_rc == /* lck exclusive */ MDBX_RESULT_TRUE) {
    //-------------------------------------------------- shrink DB & update geo
    const MDBX_meta *head = mdbx_meta_head(env);
    /* re-check size after mmap */
    if ((env->me_dxb_mmap.current & (env->me_os_psize - 1)) != 0 ||
        env->me_dxb_mmap.current < used_bytes) {
      mdbx_error("unacceptable/unexpected datafile size %" PRIuPTR,
                 env->me_dxb_mmap.current);
      return MDBX_PROBLEM;
    }
    if (env->me_dxb_mmap.current != env->me_dbgeo.now) {
      meta.mm_geo.now = bytes2pgno(env, env->me_dxb_mmap.current);
      mdbx_notice("need update meta-geo to filesize %" PRIuPTR
                  " bytes, %" PRIaPGNO " pages",
                  env->me_dxb_mmap.current, meta.mm_geo.now);
    }

    if (memcmp(&meta.mm_geo, &head->mm_geo, sizeof(meta.mm_geo))) {
      if ((env->me_flags & MDBX_RDONLY) != 0 ||
          /* recovery mode */ env->me_stuck_meta >= 0) {
        mdbx_warning(
            "skipped update meta.geo in %s mode: from l%" PRIaPGNO
            "-n%" PRIaPGNO "-u%" PRIaPGNO "/s%u-g%u, to l%" PRIaPGNO
            "-n%" PRIaPGNO "-u%" PRIaPGNO "/s%u-g%u",
            (env->me_stuck_meta < 0) ? "read-only" : "recovery",
            head->mm_geo.lower, head->mm_geo.now, head->mm_geo.upper,
            pv2pages(head->mm_geo.shrink_pv), pv2pages(head->mm_geo.grow_pv),
            meta.mm_geo.lower, meta.mm_geo.now, meta.mm_geo.upper,
            pv2pages(meta.mm_geo.shrink_pv), pv2pages(meta.mm_geo.grow_pv));
      } else {
        const txnid_t txnid = mdbx_meta_txnid_stable(env, head);
        const txnid_t next_txnid = safe64_txnid_next(txnid);
        if (unlikely(txnid > MAX_TXNID)) {
          mdbx_error("txnid overflow, raise %d", MDBX_TXN_FULL);
          return MDBX_TXN_FULL;
        }
        mdbx_notice("updating meta.geo: "
                    "from l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO
                    "/s%u-g%u (txn#%" PRIaTXN "), "
                    "to l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO
                    "/s%u-g%u (txn#%" PRIaTXN ")",
                    head->mm_geo.lower, head->mm_geo.now, head->mm_geo.upper,
                    pv2pages(head->mm_geo.shrink_pv),
                    pv2pages(head->mm_geo.grow_pv), txnid, meta.mm_geo.lower,
                    meta.mm_geo.now, meta.mm_geo.upper,
                    pv2pages(meta.mm_geo.shrink_pv),
                    pv2pages(meta.mm_geo.grow_pv), next_txnid);

        mdbx_ensure(env, mdbx_meta_eq(env, &meta, head));
        mdbx_meta_set_txnid(env, &meta, next_txnid);
        err = mdbx_sync_locked(env, env->me_flags | MDBX_SHRINK_ALLOWED, &meta);
        if (err) {
          mdbx_error("error %d, while updating meta.geo: "
                     "from l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO
                     "/s%u-g%u (txn#%" PRIaTXN "), "
                     "to l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO
                     "/s%u-g%u (txn#%" PRIaTXN ")",
                     err, head->mm_geo.lower, head->mm_geo.now,
                     head->mm_geo.upper, pv2pages(head->mm_geo.shrink_pv),
                     pv2pages(head->mm_geo.grow_pv), txnid, meta.mm_geo.lower,
                     meta.mm_geo.now, meta.mm_geo.upper,
                     pv2pages(meta.mm_geo.shrink_pv),
                     pv2pages(meta.mm_geo.grow_pv), next_txnid);
          return err;
        }
      }
    }

    atomic_store32(&env->me_lck->mti_discarded_tail,
                   bytes2pgno(env, used_aligned2os_bytes), mo_Relaxed);

    if ((env->me_flags & MDBX_RDONLY) == 0 && env->me_stuck_meta < 0) {
      for (int n = 0; n < NUM_METAS; ++n) {
        MDBX_meta *const pmeta = METAPAGE(env, n);
        if (unlikely(unaligned_peek_u64(4, &pmeta->mm_magic_and_version) !=
                     MDBX_DATA_MAGIC)) {
          const txnid_t txnid = mdbx_meta_txnid_fluid(env, pmeta);
          mdbx_notice("%s %s"
                      "meta[%u], txnid %" PRIaTXN,
                      "updating db-format signature for",
                      META_IS_STEADY(pmeta) ? "stead-" : "weak-", n, txnid);
          err = mdbx_override_meta(env, n, txnid, pmeta);
          if (unlikely(err != MDBX_SUCCESS) &&
              /* Just ignore the MDBX_PROBLEM error, since here it is
               * returned only in case of the attempt to upgrade an obsolete
               * meta-page that is invalid for current state of a DB,
               * e.g. after shrinking DB file */
              err != MDBX_PROBLEM) {
            mdbx_error("%s meta[%u], txnid %" PRIaTXN ", error %d",
                       "updating db-format signature for", n, txnid, err);
            return err;
          }
        }
      }
    }
  } /* lck exclusive, lck_rc == MDBX_RESULT_TRUE */

  //---------------------------------------------------- setup madvise/readahead
#if MDBX_ENABLE_MADVISE
  if (used_aligned2os_bytes < env->me_dxb_mmap.current) {
#if defined(MADV_REMOVE)
    if (lck_rc && (env->me_flags & MDBX_WRITEMAP) != 0 &&
        /* not recovery mode */ env->me_stuck_meta < 0) {
      mdbx_notice("open-MADV_%s %u..%u", "REMOVE (deallocate file space)",
                  env->me_lck->mti_discarded_tail.weak,
                  bytes2pgno(env, env->me_dxb_mmap.current));
      err =
          madvise(env->me_map + used_aligned2os_bytes,
                  env->me_dxb_mmap.current - used_aligned2os_bytes, MADV_REMOVE)
              ? ignore_enosys(errno)
              : MDBX_SUCCESS;
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
    }
#endif /* MADV_REMOVE */
#if defined(MADV_DONTNEED)
    mdbx_notice("open-MADV_%s %u..%u", "DONTNEED",
                env->me_lck->mti_discarded_tail.weak,
                bytes2pgno(env, env->me_dxb_mmap.current));
    err =
        madvise(env->me_map + used_aligned2os_bytes,
                env->me_dxb_mmap.current - used_aligned2os_bytes, MADV_DONTNEED)
            ? ignore_enosys(errno)
            : MDBX_SUCCESS;
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(POSIX_MADV_DONTNEED)
    err = ignore_enosys(posix_madvise(
        env->me_map + used_aligned2os_bytes,
        env->me_dxb_mmap.current - used_aligned2os_bytes, POSIX_MADV_DONTNEED));
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(POSIX_FADV_DONTNEED)
    err = ignore_enosys(posix_fadvise(
        env->me_lazy_fd, used_aligned2os_bytes,
        env->me_dxb_mmap.current - used_aligned2os_bytes, POSIX_FADV_DONTNEED));
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#endif /* MADV_DONTNEED */
  }

  err = mdbx_set_readahead(env, bytes2pgno(env, used_bytes), readahead, true);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
#endif /* MDBX_ENABLE_MADVISE */

  return rc;
}

/******************************************************************************/

/* Open and/or initialize the lock region for the environment. */
__cold static int mdbx_setup_lck(MDBX_env *env, char *lck_pathname,
                                 mdbx_mode_t mode) {
  mdbx_assert(env, env->me_lazy_fd != INVALID_HANDLE_VALUE);
  mdbx_assert(env, env->me_lfd == INVALID_HANDLE_VALUE);

  int err = mdbx_openfile(MDBX_OPEN_LCK, env, lck_pathname, &env->me_lfd, mode);
  if (err != MDBX_SUCCESS) {
    if (!(err == MDBX_ENOFILE && (env->me_flags & MDBX_EXCLUSIVE)) &&
        !((err == MDBX_EROFS || err == MDBX_EACCESS || err == MDBX_EPERM) &&
          (env->me_flags & MDBX_RDONLY)))
      return err;

    /* ensure the file system is read-only */
    err = mdbx_check_fs_rdonly(env->me_lazy_fd, lck_pathname, err);
    if (err != MDBX_SUCCESS &&
        /* ignore ERROR_NOT_SUPPORTED for exclusive mode */
        !(err == MDBX_ENOSYS && (env->me_flags & MDBX_EXCLUSIVE)))
      return err;

    /* LY: without-lck mode (e.g. exclusive or on read-only filesystem) */
    /* beginning of a locked section ---------------------------------------- */
    lcklist_lock();
    mdbx_assert(env, env->me_lcklist_next == nullptr);
    env->me_lfd = INVALID_HANDLE_VALUE;
    const int rc = mdbx_lck_seize(env);
    if (MDBX_IS_ERROR(rc)) {
      /* Calling lcklist_detach_locked() is required to restore POSIX-filelock
       * and this job will be done by mdbx_env_close0(). */
      lcklist_unlock();
      return rc;
    }
    /* insert into inprocess lck-list */
    env->me_lcklist_next = inprocess_lcklist_head;
    inprocess_lcklist_head = env;
    lcklist_unlock();
    /* end of a locked section ---------------------------------------------- */

    env->me_lck = lckless_stub(env);
    env->me_maxreaders = UINT_MAX;
    mdbx_debug("lck-setup:%s%s%s", " lck-less",
               (env->me_flags & MDBX_RDONLY) ? " readonly" : "",
               (rc == MDBX_RESULT_TRUE) ? " exclusive" : " cooperative");
    return rc;
  }

  /* beginning of a locked section ------------------------------------------ */
  lcklist_lock();
  mdbx_assert(env, env->me_lcklist_next == nullptr);

  /* Try to get exclusive lock. If we succeed, then
   * nobody is using the lock region and we should initialize it. */
  err = mdbx_lck_seize(env);
  if (MDBX_IS_ERROR(err)) {
  bailout:
    /* Calling lcklist_detach_locked() is required to restore POSIX-filelock
     * and this job will be done by mdbx_env_close0(). */
    lcklist_unlock();
    return err;
  }

  MDBX_env *inprocess_neighbor = nullptr;
  if (err == MDBX_RESULT_TRUE) {
    err = uniq_check(&env->me_lck_mmap, &inprocess_neighbor);
    if (MDBX_IS_ERROR(err))
      goto bailout;
    if (inprocess_neighbor &&
        ((mdbx_runtime_flags & MDBX_DBG_LEGACY_MULTIOPEN) == 0 ||
         (inprocess_neighbor->me_flags & MDBX_EXCLUSIVE) != 0)) {
      err = MDBX_BUSY;
      goto bailout;
    }
  }
  const int lck_seize_rc = err;

  mdbx_debug("lck-setup:%s%s%s", " with-lck",
             (env->me_flags & MDBX_RDONLY) ? " readonly" : "",
             (lck_seize_rc == MDBX_RESULT_TRUE) ? " exclusive"
                                                : " cooperative");

  uint64_t size = 0;
  err = mdbx_filesize(env->me_lfd, &size);
  if (unlikely(err != MDBX_SUCCESS))
    goto bailout;

  if (lck_seize_rc == MDBX_RESULT_TRUE) {
    size = ceil_powerof2(env->me_maxreaders * sizeof(MDBX_reader) +
                             sizeof(MDBX_lockinfo),
                         env->me_os_psize);
    mdbx_jitter4testing(false);
  } else {
    if (env->me_flags & MDBX_EXCLUSIVE) {
      err = MDBX_BUSY;
      goto bailout;
    }
    if (size > INT_MAX || (size & (env->me_os_psize - 1)) != 0 ||
        size < env->me_os_psize) {
      mdbx_error("lck-file has invalid size %" PRIu64 " bytes", size);
      err = MDBX_PROBLEM;
      goto bailout;
    }
  }

  const size_t maxreaders =
      ((size_t)size - sizeof(MDBX_lockinfo)) / sizeof(MDBX_reader);
  if (maxreaders < 4) {
    mdbx_error("lck-size too small (up to %" PRIuPTR " readers)", maxreaders);
    err = MDBX_PROBLEM;
    goto bailout;
  }
  env->me_maxreaders = (maxreaders <= MDBX_READERS_LIMIT)
                           ? (unsigned)maxreaders
                           : (unsigned)MDBX_READERS_LIMIT;

  err = mdbx_mmap((env->me_flags & MDBX_EXCLUSIVE) | MDBX_WRITEMAP,
                  &env->me_lck_mmap, (size_t)size, (size_t)size,
                  lck_seize_rc ? MMAP_OPTION_TRUNCATE | MMAP_OPTION_SEMAPHORE
                               : MMAP_OPTION_SEMAPHORE);
  if (unlikely(err != MDBX_SUCCESS))
    goto bailout;

#if MDBX_ENABLE_MADVISE
#ifdef MADV_DODUMP
  err = madvise(env->me_lck_mmap.lck, size, MADV_DODUMP) ? ignore_enosys(errno)
                                                         : MDBX_SUCCESS;
  if (unlikely(MDBX_IS_ERROR(err)))
    goto bailout;
#endif /* MADV_DODUMP */

#ifdef MADV_WILLNEED
  err = madvise(env->me_lck_mmap.lck, size, MADV_WILLNEED)
            ? ignore_enosys(errno)
            : MDBX_SUCCESS;
  if (unlikely(MDBX_IS_ERROR(err)))
    goto bailout;
#elif defined(POSIX_MADV_WILLNEED)
  err = ignore_enosys(
      posix_madvise(env->me_lck_mmap.lck, size, POSIX_MADV_WILLNEED));
  if (unlikely(MDBX_IS_ERROR(err)))
    goto bailout;
#endif /* MADV_WILLNEED */
#endif /* MDBX_ENABLE_MADVISE */

  struct MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
  if (lck_seize_rc == MDBX_RESULT_TRUE) {
    /* LY: exclusive mode, check and reset lck content */
    memset(lck, 0, (size_t)size);
    mdbx_jitter4testing(false);
    lck->mti_magic_and_version = MDBX_LOCK_MAGIC;
    lck->mti_os_and_format = MDBX_LOCK_FORMAT;
#if MDBX_ENABLE_PGOP_STAT
    lck->mti_pgop_stat.wops.weak = 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
    err = mdbx_msync(&env->me_lck_mmap, 0, (size_t)size, MDBX_SYNC_NONE);
    if (unlikely(err != MDBX_SUCCESS)) {
      mdbx_error("initial-%s for lck-file failed", "msync");
      goto bailout;
    }
    err = mdbx_fsync(env->me_lck_mmap.fd, MDBX_SYNC_SIZE);
    if (unlikely(err != MDBX_SUCCESS)) {
      mdbx_error("initial-%s for lck-file failed", "fsync");
      goto bailout;
    }
  } else {
    if (lck->mti_magic_and_version != MDBX_LOCK_MAGIC) {
      mdbx_error("%s", "lock region has invalid magic/version");
      err = ((lck->mti_magic_and_version >> 8) != MDBX_MAGIC)
                ? MDBX_INVALID
                : MDBX_VERSION_MISMATCH;
      goto bailout;
    }
    if (lck->mti_os_and_format != MDBX_LOCK_FORMAT) {
      mdbx_error("lock region has os/format 0x%" PRIx32 ", expected 0x%" PRIx32,
                 lck->mti_os_and_format, MDBX_LOCK_FORMAT);
      err = MDBX_VERSION_MISMATCH;
      goto bailout;
    }
  }

  err = mdbx_lck_init(env, inprocess_neighbor, lck_seize_rc);
  if (MDBX_IS_ERROR(err))
    goto bailout;

  mdbx_ensure(env, env->me_lcklist_next == nullptr);
  /* insert into inprocess lck-list */
  env->me_lcklist_next = inprocess_lcklist_head;
  inprocess_lcklist_head = env;
  lcklist_unlock();
  /* end of a locked section ------------------------------------------------ */

  mdbx_assert(env, !MDBX_IS_ERROR(lck_seize_rc));
  env->me_lck = lck;
  return lck_seize_rc;
}

__cold int mdbx_is_readahead_reasonable(size_t volume, intptr_t redundancy) {
  if (volume <= 1024 * 1024 * 4ul)
    return MDBX_RESULT_TRUE;

  intptr_t pagesize, total_ram_pages;
  int err = mdbx_get_sysraminfo(&pagesize, &total_ram_pages, nullptr);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  const int log2page = log2n_powerof2(pagesize);
  const intptr_t volume_pages = (volume + pagesize - 1) >> log2page;
  const intptr_t redundancy_pages =
      (redundancy < 0) ? -(intptr_t)((-redundancy + pagesize - 1) >> log2page)
                       : (intptr_t)(redundancy + pagesize - 1) >> log2page;
  if (volume_pages >= total_ram_pages ||
      volume_pages + redundancy_pages >= total_ram_pages)
    return MDBX_RESULT_FALSE;

  intptr_t avail_ram_pages;
  err = mdbx_get_sysraminfo(nullptr, nullptr, &avail_ram_pages);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  return (volume_pages + redundancy_pages >= avail_ram_pages)
             ? MDBX_RESULT_FALSE
             : MDBX_RESULT_TRUE;
}

/* Merge sync flags */
static uint32_t merge_sync_flags(const uint32_t a, const uint32_t b) {
  uint32_t r = a | b;

  /* avoid false MDBX_UTTERLY_NOSYNC */
  if (F_ISSET(r, MDBX_UTTERLY_NOSYNC) && !F_ISSET(a, MDBX_UTTERLY_NOSYNC) &&
      !F_ISSET(b, MDBX_UTTERLY_NOSYNC))
    r = (r - MDBX_UTTERLY_NOSYNC) | MDBX_SAFE_NOSYNC;

  /* convert MDBX_DEPRECATED_MAPASYNC to MDBX_SAFE_NOSYNC */
  if ((r & (MDBX_WRITEMAP | MDBX_DEPRECATED_MAPASYNC)) ==
          (MDBX_WRITEMAP | MDBX_DEPRECATED_MAPASYNC) &&
      !F_ISSET(r, MDBX_UTTERLY_NOSYNC))
    r = (r - MDBX_DEPRECATED_MAPASYNC) | MDBX_SAFE_NOSYNC;

  /* force MDBX_NOMETASYNC if MDBX_SAFE_NOSYNC enabled */
  if (r & MDBX_SAFE_NOSYNC)
    r |= MDBX_NOMETASYNC;

  assert(!(F_ISSET(r, MDBX_UTTERLY_NOSYNC) &&
           !F_ISSET(a, MDBX_UTTERLY_NOSYNC) &&
           !F_ISSET(b, MDBX_UTTERLY_NOSYNC)));
  return r;
}

__cold static int __must_check_result mdbx_override_meta(
    MDBX_env *env, unsigned target, txnid_t txnid, const MDBX_meta *shape) {
  int rc = alloc_page_buf(env);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  MDBX_page *const page = env->me_pbuf;
  mdbx_meta_model(env, page, target);
  MDBX_meta *const model = page_meta(page);
  mdbx_meta_set_txnid(env, model, txnid);
  if (shape) {
    model->mm_extra_flags = shape->mm_extra_flags;
    model->mm_validator_id = shape->mm_validator_id;
    model->mm_extra_pagehdr = shape->mm_extra_pagehdr;
    memcpy(&model->mm_geo, &shape->mm_geo, sizeof(model->mm_geo));
    memcpy(&model->mm_dbs, &shape->mm_dbs, sizeof(model->mm_dbs));
    memcpy(&model->mm_canary, &shape->mm_canary, sizeof(model->mm_canary));
    memcpy(&model->mm_pages_retired, &shape->mm_pages_retired,
           sizeof(model->mm_pages_retired));
  }
  unaligned_poke_u64(4, model->mm_datasync_sign, mdbx_meta_sign(model));
  rc = mdbx_validate_meta(env, model, page, target, nullptr);
  if (unlikely(MDBX_IS_ERROR(rc)))
    return MDBX_PROBLEM;

#if MDBX_ENABLE_PGOP_STAT
  env->me_lck->mti_pgop_stat.wops.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
  if (env->me_flags & MDBX_WRITEMAP) {
    rc = mdbx_msync(&env->me_dxb_mmap, 0,
                    pgno_align2os_bytes(env, model->mm_geo.next),
                    MDBX_SYNC_DATA | MDBX_SYNC_IODQ);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    /* mdbx_override_meta() called only while current process have exclusive
     * lock of a DB file. So meta-page could be updated directly without
     * clearing consistency flag by mdbx_meta_update_begin() */
    memcpy(pgno2page(env, target), page, env->me_psize);
    mdbx_flush_incoherent_cpu_writeback();
    rc = mdbx_msync(&env->me_dxb_mmap, 0, pgno_align2os_bytes(env, target + 1),
                    MDBX_SYNC_DATA | MDBX_SYNC_IODQ);
  } else {
    const mdbx_filehandle_t fd = (env->me_dsync_fd != INVALID_HANDLE_VALUE)
                                     ? env->me_dsync_fd
                                     : env->me_lazy_fd;
    rc = mdbx_pwrite(fd, page, env->me_psize, pgno2bytes(env, target));
    if (rc == MDBX_SUCCESS && fd == env->me_lazy_fd)
      rc = mdbx_fsync(env->me_lazy_fd, MDBX_SYNC_DATA | MDBX_SYNC_IODQ);
  }
  mdbx_flush_incoherent_mmap(env->me_map, pgno2bytes(env, NUM_METAS),
                             env->me_os_psize);
  return rc;
}

__cold int mdbx_env_turn_for_recovery(MDBX_env *env, unsigned target) {
  if (unlikely(target >= NUM_METAS))
    return MDBX_EINVAL;
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely((env->me_flags & (MDBX_EXCLUSIVE | MDBX_RDONLY)) !=
               MDBX_EXCLUSIVE))
    return MDBX_EPERM;

  const MDBX_meta *target_meta = METAPAGE(env, target);
  txnid_t new_txnid =
      safe64_txnid_next(mdbx_meta_txnid_stable(env, target_meta));
  for (unsigned n = 0; n < NUM_METAS; ++n) {
    MDBX_page *page = pgno2page(env, n);
    MDBX_meta meta = *page_meta(page);
    if (n == target)
      continue;
    if (mdbx_validate_meta(env, &meta, page, n, nullptr) != MDBX_SUCCESS) {
      int err = mdbx_override_meta(env, n, 0, nullptr);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    } else {
      txnid_t txnid = mdbx_meta_txnid_stable(env, &meta);
      if (new_txnid <= txnid)
        new_txnid = safe64_txnid_next(txnid);
    }
  }

  if (unlikely(new_txnid > MAX_TXNID)) {
    mdbx_error("txnid overflow, raise %d", MDBX_TXN_FULL);
    return MDBX_TXN_FULL;
  }
  return mdbx_override_meta(env, target, new_txnid, target_meta);
}

__cold int mdbx_env_open_for_recovery(MDBX_env *env, const char *pathname,
                                      unsigned target_meta, bool writeable) {
  if (unlikely(target_meta >= NUM_METAS))
    return MDBX_EINVAL;
  int rc = check_env(env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  if (unlikely(env->me_map))
    return MDBX_EPERM;

  env->me_stuck_meta = (int8_t)target_meta;
  return mdbx_env_open(
      env, pathname, writeable ? MDBX_EXCLUSIVE : MDBX_EXCLUSIVE | MDBX_RDONLY,
      0);
}

typedef struct {
  void *buffer_for_free;
  char *lck, *dxb;
  size_t ent_len;
} MDBX_handle_env_pathname;

__cold static int mdbx_handle_env_pathname(MDBX_handle_env_pathname *ctx,
                                           const char *pathname,
                                           MDBX_env_flags_t *flags,
                                           const mdbx_mode_t mode) {
  int rc;
  memset(ctx, 0, sizeof(*ctx));
  if (unlikely(!pathname))
    return MDBX_EINVAL;

#if defined(_WIN32) || defined(_WIN64)
  const size_t wlen = mbstowcs(nullptr, pathname, INT_MAX);
  if (wlen < 1 || wlen > /* MAX_PATH */ INT16_MAX)
    return ERROR_INVALID_NAME;
  wchar_t *const pathnameW = _alloca((wlen + 1) * sizeof(wchar_t));
  if (wlen != mbstowcs(pathnameW, pathname, wlen + 1))
    return ERROR_INVALID_NAME;

  const DWORD dwAttrib = GetFileAttributesW(pathnameW);
  if (dwAttrib == INVALID_FILE_ATTRIBUTES) {
    rc = GetLastError();
    if (rc != MDBX_ENOFILE)
      return rc;
    if (mode == 0 || (*flags & MDBX_RDONLY) != 0)
      /* can't open existing */
      return rc;

    /* auto-create directory if requested */
    if ((*flags & MDBX_NOSUBDIR) == 0 &&
        !CreateDirectoryW(pathnameW, nullptr)) {
      rc = GetLastError();
      if (rc != ERROR_ALREADY_EXISTS)
        return rc;
    }
  } else {
    /* ignore passed MDBX_NOSUBDIR flag and set it automatically */
    *flags |= MDBX_NOSUBDIR;
    if (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)
      *flags -= MDBX_NOSUBDIR;
  }
#else
  struct stat st;
  if (stat(pathname, &st)) {
    rc = errno;
    if (rc != MDBX_ENOFILE)
      return rc;
    if (mode == 0 || (*flags & MDBX_RDONLY) != 0)
      /* can't open existing */
      return rc;

    /* auto-create directory if requested */
    const mdbx_mode_t dir_mode =
        (/* inherit read/write permissions for group and others */ mode &
         (S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) |
        /* always add read/write/search for owner */ S_IRWXU |
        ((mode & S_IRGRP) ? /* +search if readable by group */ S_IXGRP : 0) |
        ((mode & S_IROTH) ? /* +search if readable by others */ S_IXOTH : 0);
    if ((*flags & MDBX_NOSUBDIR) == 0 && mkdir(pathname, dir_mode)) {
      rc = errno;
      if (rc != EEXIST)
        return rc;
    }
  } else {
    /* ignore passed MDBX_NOSUBDIR flag and set it automatically */
    *flags |= MDBX_NOSUBDIR;
    if (S_ISDIR(st.st_mode))
      *flags -= MDBX_NOSUBDIR;
  }
#endif

  static const char dxb_name[] = MDBX_DATANAME;
  static const size_t dxb_name_len = sizeof(dxb_name) - 1;
  static const char lck_name[] = MDBX_LOCKNAME;
  static const char lock_suffix[] = MDBX_LOCK_SUFFIX;

  ctx->ent_len = strlen(pathname);
  if ((*flags & MDBX_NOSUBDIR) && ctx->ent_len >= dxb_name_len &&
      !memcmp(dxb_name, pathname + ctx->ent_len - dxb_name_len, dxb_name_len)) {
    *flags -= MDBX_NOSUBDIR;
    ctx->ent_len -= dxb_name_len;
  }

  const size_t bytes_needed =
      ctx->ent_len * 2 + ((*flags & MDBX_NOSUBDIR)
                              ? sizeof(lock_suffix) + 1
                              : sizeof(lck_name) + sizeof(dxb_name));
  ctx->buffer_for_free = mdbx_malloc(bytes_needed);
  if (!ctx->buffer_for_free)
    return MDBX_ENOMEM;

  ctx->lck = ctx->buffer_for_free;
  if (*flags & MDBX_NOSUBDIR) {
    ctx->dxb = ctx->lck + ctx->ent_len + sizeof(lock_suffix);
    sprintf(ctx->lck, "%s%s", pathname, lock_suffix);
    strcpy(ctx->dxb, pathname);
  } else {
    ctx->dxb = ctx->lck + ctx->ent_len + sizeof(lck_name);
    sprintf(ctx->lck, "%.*s%s", (int)ctx->ent_len, pathname, lck_name);
    sprintf(ctx->dxb, "%.*s%s", (int)ctx->ent_len, pathname, dxb_name);
  }

  return MDBX_SUCCESS;
}

__cold int mdbx_env_delete(const char *pathname, MDBX_env_delete_mode_t mode) {
  switch (mode) {
  default:
    return MDBX_EINVAL;
  case MDBX_ENV_JUST_DELETE:
  case MDBX_ENV_ENSURE_UNUSED:
  case MDBX_ENV_WAIT_FOR_UNUSED:
    break;
  }

#ifdef __e2k__ /* https://bugs.mcst.ru/bugzilla/show_bug.cgi?id=6011 */
  MDBX_env *const dummy_env = alloca(sizeof(MDBX_env));
#else
  MDBX_env dummy_env_silo, *const dummy_env = &dummy_env_silo;
#endif
  memset(dummy_env, 0, sizeof(*dummy_env));
  dummy_env->me_flags =
      (mode == MDBX_ENV_ENSURE_UNUSED) ? MDBX_EXCLUSIVE : MDBX_ENV_DEFAULTS;
  dummy_env->me_os_psize = (unsigned)mdbx_syspagesize();
  dummy_env->me_psize = (unsigned)mdbx_default_pagesize();
  dummy_env->me_pathname = (char *)pathname;

  MDBX_handle_env_pathname env_pathname;
  STATIC_ASSERT(sizeof(dummy_env->me_flags) == sizeof(MDBX_env_flags_t));
  int rc = MDBX_RESULT_TRUE,
      err = mdbx_handle_env_pathname(
          &env_pathname, pathname, (MDBX_env_flags_t *)&dummy_env->me_flags, 0);
  if (likely(err == MDBX_SUCCESS)) {
    mdbx_filehandle_t clk_handle = INVALID_HANDLE_VALUE,
                      dxb_handle = INVALID_HANDLE_VALUE;
    if (mode > MDBX_ENV_JUST_DELETE) {
      err = mdbx_openfile(MDBX_OPEN_DELETE, dummy_env, env_pathname.dxb,
                          &dxb_handle, 0);
      err = (err == MDBX_ENOFILE) ? MDBX_SUCCESS : err;
      if (err == MDBX_SUCCESS) {
        err = mdbx_openfile(MDBX_OPEN_DELETE, dummy_env, env_pathname.lck,
                            &clk_handle, 0);
        err = (err == MDBX_ENOFILE) ? MDBX_SUCCESS : err;
      }
      if (err == MDBX_SUCCESS && clk_handle != INVALID_HANDLE_VALUE)
        err = mdbx_lockfile(clk_handle, mode == MDBX_ENV_WAIT_FOR_UNUSED);
      if (err == MDBX_SUCCESS && dxb_handle != INVALID_HANDLE_VALUE)
        err = mdbx_lockfile(dxb_handle, mode == MDBX_ENV_WAIT_FOR_UNUSED);
    }

    if (err == MDBX_SUCCESS) {
      err = mdbx_removefile(env_pathname.dxb);
      if (err == MDBX_SUCCESS)
        rc = MDBX_SUCCESS;
      else if (err == MDBX_ENOFILE)
        err = MDBX_SUCCESS;
    }

    if (err == MDBX_SUCCESS) {
      err = mdbx_removefile(env_pathname.lck);
      if (err == MDBX_SUCCESS)
        rc = MDBX_SUCCESS;
      else if (err == MDBX_ENOFILE)
        err = MDBX_SUCCESS;
    }

    if (err == MDBX_SUCCESS && !(dummy_env->me_flags & MDBX_NOSUBDIR)) {
      err = mdbx_removedirectory(pathname);
      if (err == MDBX_SUCCESS)
        rc = MDBX_SUCCESS;
      else if (err == MDBX_ENOFILE)
        err = MDBX_SUCCESS;
    }

    if (dxb_handle != INVALID_HANDLE_VALUE)
      mdbx_closefile(dxb_handle);
    if (clk_handle != INVALID_HANDLE_VALUE)
      mdbx_closefile(clk_handle);
  } else if (err == MDBX_ENOFILE)
    err = MDBX_SUCCESS;

  mdbx_free(env_pathname.buffer_for_free);
  return (err == MDBX_SUCCESS) ? rc : err;
}

__cold int mdbx_env_open(MDBX_env *env, const char *pathname,
                         MDBX_env_flags_t flags, mdbx_mode_t mode) {
  int rc = check_env(env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(flags & ~ENV_USABLE_FLAGS))
    return MDBX_EINVAL;

  if (flags & MDBX_RDONLY)
    mode = 0;

  if (env->me_lazy_fd != INVALID_HANDLE_VALUE ||
      (env->me_flags & MDBX_ENV_ACTIVE) != 0 || env->me_map)
    return MDBX_EPERM;

  /* pickup previously mdbx_env_set_flags(),
   * but avoid MDBX_UTTERLY_NOSYNC by disjunction */
  const uint32_t saved_me_flags = env->me_flags;
  flags = merge_sync_flags(flags, env->me_flags);

  MDBX_handle_env_pathname env_pathname;
  rc = mdbx_handle_env_pathname(&env_pathname, pathname, &flags, mode);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  if (flags & MDBX_RDONLY) {
    /* LY: silently ignore irrelevant flags when
     * we're only getting read access */
    flags &= ~(MDBX_WRITEMAP | MDBX_DEPRECATED_MAPASYNC | MDBX_SAFE_NOSYNC |
               MDBX_NOMETASYNC | MDBX_COALESCE | MDBX_LIFORECLAIM |
               MDBX_NOMEMINIT | MDBX_ACCEDE);
  } else {
#if MDBX_MMAP_INCOHERENT_FILE_WRITE
    /* Temporary `workaround` for OpenBSD kernel's flaw.
     * See https://github.com/erthink/libmdbx/issues/67 */
    if ((flags & MDBX_WRITEMAP) == 0) {
      if (flags & MDBX_ACCEDE)
        flags |= MDBX_WRITEMAP;
      else {
        mdbx_debug_log(MDBX_LOG_ERROR, __func__, __LINE__,
                       "System (i.e. OpenBSD) requires MDBX_WRITEMAP because "
                       "of an internal flaw(s) in a file/buffer/page cache.\n");
        rc = 42 /* ENOPROTOOPT */;
        goto bailout;
      }
    }
#endif /* MDBX_MMAP_INCOHERENT_FILE_WRITE */
  }

  env->me_flags = (flags & ~MDBX_FATAL_ERROR) | MDBX_ENV_ACTIVE;
  env->me_pathname = mdbx_calloc(env_pathname.ent_len + 1, 1);
  env->me_dbxs = mdbx_calloc(env->me_maxdbs, sizeof(MDBX_dbx));
  env->me_dbflags = mdbx_calloc(env->me_maxdbs, sizeof(env->me_dbflags[0]));
  env->me_dbiseqs = mdbx_calloc(env->me_maxdbs, sizeof(env->me_dbiseqs[0]));
  if (!(env->me_dbxs && env->me_pathname && env->me_dbflags &&
        env->me_dbiseqs)) {
    rc = MDBX_ENOMEM;
    goto bailout;
  }
  memcpy(env->me_pathname, env_pathname.dxb, env_pathname.ent_len);
  env->me_dbxs[FREE_DBI].md_cmp = cmp_int_align4; /* aligned MDBX_INTEGERKEY */
  env->me_dbxs[FREE_DBI].md_dcmp = cmp_lenfast;

  rc = mdbx_openfile(F_ISSET(flags, MDBX_RDONLY) ? MDBX_OPEN_DXB_READ
                                                 : MDBX_OPEN_DXB_LAZY,
                     env, env_pathname.dxb, &env->me_lazy_fd, mode);
  if (rc != MDBX_SUCCESS)
    goto bailout;

  mdbx_assert(env, env->me_dsync_fd == INVALID_HANDLE_VALUE);
  if ((flags & (MDBX_RDONLY | MDBX_SAFE_NOSYNC | MDBX_NOMETASYNC)) == 0) {
    rc = mdbx_openfile(MDBX_OPEN_DXB_DSYNC, env, env_pathname.dxb,
                       &env->me_dsync_fd, 0);
    mdbx_ensure(env, (rc != MDBX_SUCCESS) ==
                         (env->me_dsync_fd == INVALID_HANDLE_VALUE));
  }

#if MDBX_LOCKING == MDBX_LOCKING_SYSV
  env->me_sysv_ipc.key = ftok(env_pathname.dxb, 42);
  if (env->me_sysv_ipc.key == -1) {
    rc = errno;
    goto bailout;
  }
#endif /* MDBX_LOCKING */

#if !(defined(_WIN32) || defined(_WIN64))
  if (mode == 0) {
    /* pickup mode for lck-file */
    struct stat st;
    if (fstat(env->me_lazy_fd, &st)) {
      rc = errno;
      goto bailout;
    }
    mode = st.st_mode;
  }
  mode = (/* inherit read permissions for group and others */ mode &
          (S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) |
         /* always add read/write/search for owner */ S_IRUSR | S_IWUSR |
         ((mode & S_IRGRP) ? /* +write if readable by group */ S_IWGRP : 0) |
         ((mode & S_IROTH) ? /* +write if readable by others */ S_IWOTH : 0);
#endif /* !Windows */
  const int lck_rc = mdbx_setup_lck(env, env_pathname.lck, mode);
  if (MDBX_IS_ERROR(lck_rc)) {
    rc = lck_rc;
    goto bailout;
  }

  /* Set the position in files outside of the data to avoid corruption
   * due to erroneous use of file descriptors in the application code. */
  mdbx_fseek(env->me_lfd, UINT64_C(1) << 63);
  mdbx_fseek(env->me_lazy_fd, UINT64_C(1) << 63);
  if (env->me_dsync_fd != INVALID_HANDLE_VALUE)
    mdbx_fseek(env->me_dsync_fd, UINT64_C(1) << 63);

  const MDBX_env_flags_t rigorous_flags =
      MDBX_SAFE_NOSYNC | MDBX_DEPRECATED_MAPASYNC;
  const MDBX_env_flags_t mode_flags = rigorous_flags | MDBX_NOMETASYNC |
                                      MDBX_LIFORECLAIM | MDBX_COALESCE |
                                      MDBX_NORDAHEAD;

  MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
  if (lck && lck_rc != MDBX_RESULT_TRUE && (env->me_flags & MDBX_RDONLY) == 0) {
    while (atomic_load32(&lck->mti_envmode, mo_AcquireRelease) == MDBX_RDONLY) {
      if (atomic_cas32(&lck->mti_envmode, MDBX_RDONLY,
                       env->me_flags & mode_flags)) {
        /* The case:
         *  - let's assume that for some reason the DB file is smaller
         *    than it should be according to the geometry,
         *    but not smaller than the last page used;
         *  - the first process that opens the database (lc_rc = true)
         *    does this in readonly mode and therefore cannot bring
         *    the file size back to normal;
         *  - some next process (lc_rc = false) opens the DB in read-write
         *    mode and now is here.
         *
         * FIXME: Should we re-check and set the size of DB-file right here? */
        break;
      }
      atomic_yield();
    }

    if (env->me_flags & MDBX_ACCEDE) {
      /* pickup current mode-flags, including MDBX_LIFORECLAIM |
       * MDBX_COALESCE | MDBX_NORDAHEAD */
      const unsigned diff =
          (lck->mti_envmode.weak ^ env->me_flags) & mode_flags;
      mdbx_notice("accede mode-flags: 0x%X, 0x%X -> 0x%X", diff, env->me_flags,
                  env->me_flags ^ diff);
      env->me_flags ^= diff;
    }

    if ((lck->mti_envmode.weak ^ env->me_flags) & rigorous_flags) {
      mdbx_error("%s", "current mode/flags incompatible with requested");
      rc = MDBX_INCOMPATIBLE;
      goto bailout;
    }
  }

  const int dxb_rc = mdbx_setup_dxb(env, lck_rc, mode);
  if (MDBX_IS_ERROR(dxb_rc)) {
    rc = dxb_rc;
    goto bailout;
  }

  if (unlikely(/* recovery mode */ env->me_stuck_meta >= 0) &&
      (lck_rc != /* exclusive */ MDBX_RESULT_TRUE ||
       (flags & MDBX_EXCLUSIVE) == 0)) {
    mdbx_error("%s", "recovery requires exclusive mode");
    rc = MDBX_BUSY;
    goto bailout;
  }

  mdbx_debug("opened dbenv %p", (void *)env);
  if (lck) {
    if (lck_rc == MDBX_RESULT_TRUE) {
      lck->mti_envmode.weak = env->me_flags & (mode_flags | MDBX_RDONLY);
      rc = mdbx_lck_downgrade(env);
      mdbx_debug("lck-downgrade-%s: rc %i",
                 (env->me_flags & MDBX_EXCLUSIVE) ? "partial" : "full", rc);
      if (rc != MDBX_SUCCESS)
        goto bailout;
    } else {
      rc = mdbx_cleanup_dead_readers(env, false, NULL);
      if (MDBX_IS_ERROR(rc))
        goto bailout;
    }

    if ((env->me_flags & MDBX_NOTLS) == 0) {
      rc = mdbx_rthc_alloc(&env->me_txkey, &lck->mti_readers[0],
                           &lck->mti_readers[env->me_maxreaders]);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
      env->me_flags |= MDBX_ENV_TXKEY;
    }
  }

  if ((flags & MDBX_RDONLY) == 0) {
    const size_t tsize = sizeof(MDBX_txn),
                 size = tsize + env->me_maxdbs *
                                    (sizeof(MDBX_db) + sizeof(MDBX_cursor *) +
                                     sizeof(unsigned) + 1);
    rc = alloc_page_buf(env);
    if (rc == MDBX_SUCCESS) {
      memset(env->me_pbuf, -1, env->me_psize * 2);
      MDBX_txn *txn = mdbx_calloc(1, size);
      if (txn) {
        txn->mt_dbs = (MDBX_db *)((char *)txn + tsize);
        txn->tw.cursors = (MDBX_cursor **)(txn->mt_dbs + env->me_maxdbs);
        txn->mt_dbiseqs = (unsigned *)(txn->tw.cursors + env->me_maxdbs);
        txn->mt_dbistate = (uint8_t *)(txn->mt_dbiseqs + env->me_maxdbs);
        txn->mt_env = env;
        txn->mt_dbxs = env->me_dbxs;
        txn->mt_flags = MDBX_TXN_FINISHED;
        env->me_txn0 = txn;
        txn->tw.retired_pages = mdbx_pnl_alloc(MDBX_PNL_INITIAL);
        txn->tw.reclaimed_pglist = mdbx_pnl_alloc(MDBX_PNL_INITIAL);
        if (unlikely(!txn->tw.retired_pages || !txn->tw.reclaimed_pglist))
          rc = MDBX_ENOMEM;
      } else
        rc = MDBX_ENOMEM;
    }
  }

#if MDBX_DEBUG
  if (rc == MDBX_SUCCESS) {
    MDBX_meta *meta = mdbx_meta_head(env);
    MDBX_db *db = &meta->mm_dbs[MAIN_DBI];

    mdbx_debug("opened database version %u, pagesize %u",
               (uint8_t)unaligned_peek_u64(4, meta->mm_magic_and_version),
               env->me_psize);
    mdbx_debug("using meta page %" PRIaPGNO ", txn %" PRIaTXN,
               data_page(meta)->mp_pgno, mdbx_meta_txnid_fluid(env, meta));
    mdbx_debug("depth: %u", db->md_depth);
    mdbx_debug("entries: %" PRIu64, db->md_entries);
    mdbx_debug("branch pages: %" PRIaPGNO, db->md_branch_pages);
    mdbx_debug("leaf pages: %" PRIaPGNO, db->md_leaf_pages);
    mdbx_debug("overflow pages: %" PRIaPGNO, db->md_overflow_pages);
    mdbx_debug("root: %" PRIaPGNO, db->md_root);
    mdbx_debug("schema_altered: %" PRIaTXN, db->md_mod_txnid);
  }
#endif

bailout:
  if (rc != MDBX_SUCCESS) {
    rc = mdbx_env_close0(env) ? MDBX_PANIC : rc;
    env->me_flags =
        saved_me_flags | ((rc != MDBX_PANIC) ? 0 : MDBX_FATAL_ERROR);
  } else {
#if defined(MDBX_USE_VALGRIND) || defined(__SANITIZE_ADDRESS__)
    mdbx_txn_valgrind(env, nullptr);
#endif
  }
  mdbx_free(env_pathname.buffer_for_free);
  return rc;
}

/* Destroy resources from mdbx_env_open(), clear our readers & DBIs */
__cold static int mdbx_env_close0(MDBX_env *env) {
  env->me_stuck_meta = -1;
  if (!(env->me_flags & MDBX_ENV_ACTIVE)) {
    mdbx_ensure(env, env->me_lcklist_next == nullptr);
    return MDBX_SUCCESS;
  }

  env->me_flags &= ~MDBX_ENV_ACTIVE;
  env->me_lck = nullptr;
  if (env->me_flags & MDBX_ENV_TXKEY)
    mdbx_rthc_remove(env->me_txkey);

  lcklist_lock();
  const int rc = lcklist_detach_locked(env);
  lcklist_unlock();

  if (env->me_map) {
    mdbx_munmap(&env->me_dxb_mmap);
#ifdef MDBX_USE_VALGRIND
    VALGRIND_DISCARD(env->me_valgrind_handle);
    env->me_valgrind_handle = -1;
#endif
  }

  if (env->me_dsync_fd != INVALID_HANDLE_VALUE) {
    (void)mdbx_closefile(env->me_dsync_fd);
    env->me_dsync_fd = INVALID_HANDLE_VALUE;
  }

  if (env->me_lazy_fd != INVALID_HANDLE_VALUE) {
    (void)mdbx_closefile(env->me_lazy_fd);
    env->me_lazy_fd = INVALID_HANDLE_VALUE;
  }

  if (env->me_lck_mmap.lck)
    mdbx_munmap(&env->me_lck_mmap);

  if (env->me_lfd != INVALID_HANDLE_VALUE) {
    (void)mdbx_closefile(env->me_lfd);
    env->me_lfd = INVALID_HANDLE_VALUE;
  }

  if (env->me_dbxs) {
    for (unsigned i = env->me_numdbs; --i >= CORE_DBS;)
      mdbx_free(env->me_dbxs[i].md_name.iov_base);
    mdbx_free(env->me_dbxs);
  }
  mdbx_memalign_free(env->me_pbuf);
  mdbx_free(env->me_dbiseqs);
  mdbx_free(env->me_dbflags);
  mdbx_free(env->me_pathname);
  if (env->me_txn0) {
    mdbx_dpl_free(env->me_txn0);
    mdbx_txl_free(env->me_txn0->tw.lifo_reclaimed);
    mdbx_pnl_free(env->me_txn0->tw.retired_pages);
    mdbx_pnl_free(env->me_txn0->tw.spill_pages);
    mdbx_pnl_free(env->me_txn0->tw.reclaimed_pglist);
    mdbx_free(env->me_txn0);
  }
  env->me_flags = 0;
  return rc;
}

__cold int mdbx_env_close_ex(MDBX_env *env, bool dont_sync) {
  MDBX_page *dp;
  int rc = MDBX_SUCCESS;

  if (unlikely(!env))
    return MDBX_EINVAL;

  if (unlikely(env->me_signature.weak != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

#if MDBX_ENV_CHECKPID || !(defined(_WIN32) || defined(_WIN64))
  /* Check the PID even if MDBX_ENV_CHECKPID=0 on non-Windows
   * platforms (i.e. where fork() is available).
   * This is required to legitimize a call after fork()
   * from a child process, that should be allowed to free resources. */
  if (unlikely(env->me_pid != mdbx_getpid()))
    env->me_flags |= MDBX_FATAL_ERROR;
#endif /* MDBX_ENV_CHECKPID */

  if (env->me_map && (env->me_flags & (MDBX_RDONLY | MDBX_FATAL_ERROR)) == 0 &&
      env->me_txn0) {
    if (env->me_txn0->mt_owner && env->me_txn0->mt_owner != mdbx_thread_self())
      return MDBX_BUSY;
  } else
    dont_sync = true;

  if (!atomic_cas32(&env->me_signature, MDBX_ME_SIGNATURE, 0))
    return MDBX_EBADSIGN;

  if (!dont_sync) {
#if defined(_WIN32) || defined(_WIN64)
    /* On windows, without blocking is impossible to determine whether another
     * process is running a writing transaction or not.
     * Because in the "owner died" condition kernel don't release
     * file lock immediately. */
    rc = mdbx_env_sync_internal(env, true, false);
    rc = (rc == MDBX_RESULT_TRUE) ? MDBX_SUCCESS : rc;
#else
    struct stat st;
    if (unlikely(fstat(env->me_lazy_fd, &st)))
      rc = errno;
    else if (st.st_nlink > 0 /* don't sync deleted files */) {
      rc = mdbx_env_sync_internal(env, true, true);
      rc = (rc == MDBX_BUSY || rc == EAGAIN || rc == EACCES || rc == EBUSY ||
            rc == EWOULDBLOCK || rc == MDBX_RESULT_TRUE)
               ? MDBX_SUCCESS
               : rc;
    }
#endif
  }

  mdbx_assert(env, env->me_signature.weak == 0);
  rc = mdbx_env_close0(env) ? MDBX_PANIC : rc;
  mdbx_ensure(env, mdbx_fastmutex_destroy(&env->me_dbi_lock) == MDBX_SUCCESS);
#if defined(_WIN32) || defined(_WIN64)
  /* me_remap_guard don't have destructor (Slim Reader/Writer Lock) */
  DeleteCriticalSection(&env->me_windowsbug_lock);
#else
  mdbx_ensure(env,
              mdbx_fastmutex_destroy(&env->me_remap_guard) == MDBX_SUCCESS);
#endif /* Windows */

#if MDBX_LOCKING > MDBX_LOCKING_SYSV
  MDBX_lockinfo *const stub = lckless_stub(env);
  mdbx_ensure(env, mdbx_ipclock_destroy(&stub->mti_wlock) == 0);
#endif /* MDBX_LOCKING */

  while ((dp = env->me_dp_reserve) != NULL) {
    MDBX_ASAN_UNPOISON_MEMORY_REGION(dp, env->me_psize);
    VALGRIND_MAKE_MEM_DEFINED(&dp->mp_next, sizeof(dp->mp_next));
    env->me_dp_reserve = dp->mp_next;
    mdbx_free(dp);
  }
  VALGRIND_DESTROY_MEMPOOL(env);
  mdbx_ensure(env, env->me_lcklist_next == nullptr);
  env->me_pid = 0;
  mdbx_free(env);

  return rc;
}

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
__cold int mdbx_env_close(MDBX_env *env) {
  return __inline_mdbx_env_close(env);
}
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

/* Compare two items pointing at aligned unsigned int's. */
static int __hot cmp_int_align4(const MDBX_val *a, const MDBX_val *b) {
  mdbx_assert(NULL, a->iov_len == b->iov_len);
  switch (a->iov_len) {
  case 4:
    return CMP2INT(unaligned_peek_u32(4, a->iov_base),
                   unaligned_peek_u32(4, b->iov_base));
  case 8:
    return CMP2INT(unaligned_peek_u64(4, a->iov_base),
                   unaligned_peek_u64(4, b->iov_base));
  default:
    mdbx_assert_fail(NULL, "invalid size for INTEGERKEY/INTEGERDUP", __func__,
                     __LINE__);
    return 0;
  }
}

/* Compare two items pointing at 2-byte aligned unsigned int's. */
static int __hot cmp_int_align2(const MDBX_val *a, const MDBX_val *b) {
  mdbx_assert(NULL, a->iov_len == b->iov_len);
  switch (a->iov_len) {
  case 4:
    return CMP2INT(unaligned_peek_u32(2, a->iov_base),
                   unaligned_peek_u32(2, b->iov_base));
  case 8:
    return CMP2INT(unaligned_peek_u64(2, a->iov_base),
                   unaligned_peek_u64(2, b->iov_base));
  default:
    mdbx_assert_fail(NULL, "invalid size for INTEGERKEY/INTEGERDUP", __func__,
                     __LINE__);
    return 0;
  }
}

/* Compare two items pointing at unsigned values with unknown alignment.
 *
 * This is also set as MDBX_INTEGERDUP|MDBX_DUPFIXED's MDBX_dbx.md_dcmp. */
static int __hot cmp_int_unaligned(const MDBX_val *a, const MDBX_val *b) {
  mdbx_assert(NULL, a->iov_len == b->iov_len);
  switch (a->iov_len) {
  case 4:
    return CMP2INT(unaligned_peek_u32(1, a->iov_base),
                   unaligned_peek_u32(1, b->iov_base));
  case 8:
    return CMP2INT(unaligned_peek_u64(1, a->iov_base),
                   unaligned_peek_u64(1, b->iov_base));
  default:
    mdbx_assert_fail(NULL, "invalid size for INTEGERKEY/INTEGERDUP", __func__,
                     __LINE__);
    return 0;
  }
}

/* Compare two items lexically */
static int __hot cmp_lexical(const MDBX_val *a, const MDBX_val *b) {
  if (a->iov_len == b->iov_len)
    return a->iov_len ? memcmp(a->iov_base, b->iov_base, a->iov_len) : 0;

  const int diff_len = (a->iov_len < b->iov_len) ? -1 : 1;
  const size_t shortest = (a->iov_len < b->iov_len) ? a->iov_len : b->iov_len;
  int diff_data = shortest ? memcmp(a->iov_base, b->iov_base, shortest) : 0;
  return likely(diff_data) ? diff_data : diff_len;
}

/* Compare two items in reverse byte order */
static int __hot cmp_reverse(const MDBX_val *a, const MDBX_val *b) {
  const size_t shortest = (a->iov_len < b->iov_len) ? a->iov_len : b->iov_len;
  if (likely(shortest)) {
    const uint8_t *pa = (const uint8_t *)a->iov_base + a->iov_len;
    const uint8_t *pb = (const uint8_t *)b->iov_base + b->iov_len;
    const uint8_t *const end = pa - shortest;
    do {
      int diff = *--pa - *--pb;
      if (likely(diff))
        return diff;
    } while (pa != end);
  }
  return CMP2INT(a->iov_len, b->iov_len);
}

/* Fast non-lexically comparator */
static int __hot cmp_lenfast(const MDBX_val *a, const MDBX_val *b) {
  int diff = CMP2INT(a->iov_len, b->iov_len);
  return likely(diff || a->iov_len == 0)
             ? diff
             : memcmp(a->iov_base, b->iov_base, a->iov_len);
}

static bool unsure_equal(MDBX_cmp_func cmp, const MDBX_val *a,
                         const MDBX_val *b) {
  /* checking for the use of a known good comparator
   * or/otherwise for a full byte-to-byte match */
  return cmp == cmp_lenfast || cmp == cmp_lexical || cmp == cmp_reverse ||
         cmp == cmp_int_unaligned || cmp_lenfast(a, b) == 0;
}

/* Search for key within a page, using binary search.
 * Returns the smallest entry larger or equal to the key.
 * Updates the cursor index with the index of the found entry.
 * If no entry larger or equal to the key is found, returns NULL. */
static struct node_result __hot mdbx_node_search(MDBX_cursor *mc,
                                                 const MDBX_val *key) {
  MDBX_page *mp = mc->mc_pg[mc->mc_top];
  const int nkeys = page_numkeys(mp);
  DKBUF_DEBUG;

  mdbx_debug("searching %u keys in %s %spage %" PRIaPGNO, nkeys,
             IS_LEAF(mp) ? "leaf" : "branch", IS_SUBP(mp) ? "sub-" : "",
             mp->mp_pgno);

  struct node_result ret;
  ret.exact = false;
  STATIC_ASSERT(P_BRANCH == 1);
  int low = mp->mp_flags & P_BRANCH;
  int high = nkeys - 1;
  if (unlikely(high < low)) {
    mc->mc_ki[mc->mc_top] = 0;
    ret.node = NULL;
    return ret;
  }

  int cr = 0, i = 0;
  MDBX_cmp_func *cmp = mc->mc_dbx->md_cmp;
  MDBX_val nodekey;
  if (unlikely(IS_LEAF2(mp))) {
    mdbx_cassert(mc, mp->mp_leaf2_ksize == mc->mc_db->md_xsize);
    nodekey.iov_len = mp->mp_leaf2_ksize;
    do {
      i = (low + high) >> 1;
      nodekey.iov_base = page_leaf2key(mp, i, nodekey.iov_len);
      mdbx_cassert(mc, (char *)mp + mc->mc_txn->mt_env->me_psize >=
                           (char *)nodekey.iov_base + nodekey.iov_len);
      cr = cmp(key, &nodekey);
      mdbx_debug("found leaf index %u [%s], rc = %i", i, DKEY_DEBUG(&nodekey),
                 cr);
      if (unlikely(cr == 0)) {
        ret.exact = true;
        break;
      }
      low = (cr < 0) ? low : i + 1;
      high = (cr < 0) ? i - 1 : high;
    } while (likely(low <= high));

    /* Found entry is less than the key. */
    /* Skip to get the smallest entry larger than key. */
    i += cr > 0;

    /* store the key index */
    mc->mc_ki[mc->mc_top] = (indx_t)i;
    ret.node = (i < nkeys)
                   ? /* fake for LEAF2 */ (MDBX_node *)(intptr_t)-1
                   : /* There is no entry larger or equal to the key. */ NULL;
    return ret;
  }

  if (IS_BRANCH(mp) && cmp == cmp_int_align2)
    /* Branch pages have no data, so if using integer keys,
     * alignment is guaranteed. Use faster cmp_int_align4(). */
    cmp = cmp_int_align4;

  MDBX_node *node;
  do {
    i = (low + high) >> 1;

    node = page_node(mp, i);
    nodekey.iov_len = node_ks(node);
    nodekey.iov_base = node_key(node);
    mdbx_cassert(mc, (char *)mp + mc->mc_txn->mt_env->me_psize >=
                         (char *)nodekey.iov_base + nodekey.iov_len);

    cr = cmp(key, &nodekey);
    if (IS_LEAF(mp))
      mdbx_debug("found leaf index %u [%s], rc = %i", i, DKEY_DEBUG(&nodekey),
                 cr);
    else
      mdbx_debug("found branch index %u [%s -> %" PRIaPGNO "], rc = %i", i,
                 DKEY_DEBUG(&nodekey), node_pgno(node), cr);
    if (unlikely(cr == 0)) {
      ret.exact = true;
      break;
    }
    low = (cr < 0) ? low : i + 1;
    high = (cr < 0) ? i - 1 : high;
  } while (likely(low <= high));

  /* Found entry is less than the key. */
  /* Skip to get the smallest entry larger than key. */
  i += cr > 0;

  /* store the key index */
  mc->mc_ki[mc->mc_top] = (indx_t)i;
  ret.node = (i < nkeys)
                 ? page_node(mp, i)
                 : /* There is no entry larger or equal to the key. */ NULL;
  return ret;
}

/* Pop a page off the top of the cursor's stack. */
static __inline void mdbx_cursor_pop(MDBX_cursor *mc) {
  if (mc->mc_snum) {
    mdbx_debug("popped page %" PRIaPGNO " off db %d cursor %p",
               mc->mc_pg[mc->mc_top]->mp_pgno, DDBI(mc), (void *)mc);
    if (--mc->mc_snum) {
      mc->mc_top--;
    } else {
      mc->mc_flags &= ~C_INITIALIZED;
    }
  }
}

/* Push a page onto the top of the cursor's stack.
 * Set MDBX_TXN_ERROR on failure. */
static __inline int mdbx_cursor_push(MDBX_cursor *mc, MDBX_page *mp) {
  mdbx_debug("pushing page %" PRIaPGNO " on db %d cursor %p", mp->mp_pgno,
             DDBI(mc), (void *)mc);

  if (unlikely(mc->mc_snum >= CURSOR_STACK)) {
    mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
    return MDBX_CURSOR_FULL;
  }

  mdbx_cassert(mc, mc->mc_snum < UINT16_MAX);
  mc->mc_top = mc->mc_snum++;
  mc->mc_pg[mc->mc_top] = mp;
  mc->mc_ki[mc->mc_top] = 0;

  return MDBX_SUCCESS;
}

__hot static struct page_result
mdbx_page_get_ex(MDBX_cursor *const mc, const pgno_t pgno,
                 /* TODO: use parent-page ptr */ txnid_t front) {
  struct page_result ret;
  MDBX_txn *const txn = mc->mc_txn;
  mdbx_tassert(txn, front <= txn->mt_front);
  if (unlikely(pgno >= txn->mt_next_pgno)) {
    mdbx_error("page #%" PRIaPGNO " beyond next-pgno", pgno);
  notfound:
    ret.page = nullptr;
    ret.err = MDBX_PAGE_NOTFOUND;
  bailout:
    mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
    return ret;
  }

  MDBX_env *const env = txn->mt_env;
  mdbx_assert(env, ((txn->mt_flags ^ env->me_flags) & MDBX_WRITEMAP) == 0);
  if (unlikely((txn->mt_flags & (MDBX_TXN_RDONLY | MDBX_WRITEMAP)) == 0)) {
    const MDBX_txn *spiller = txn;
    do {
      /* Spilled pages were dirtied in this txn and flushed
       * because the dirty list got full. Bring this page
       * back in from the map (but don't unspill it here,
       * leave that unless page_touch happens again). */
      if (unlikely(spiller->mt_flags & MDBX_TXN_SPILLS) &&
          spiller->tw.spill_pages &&
          mdbx_pnl_exist(spiller->tw.spill_pages, pgno << 1)) {
        goto spilled;
      }

      const unsigned i = mdbx_dpl_search(spiller, pgno);
      assert((int)i > 0);
      if (spiller->tw.dirtylist->items[i].pgno == pgno) {
        ret.page = spiller->tw.dirtylist->items[i].ptr;
        spiller->tw.dirtylist->items[i].lru = txn->tw.dirtylru++;
        goto dirty;
      }

      spiller = spiller->mt_parent;
    } while (spiller != NULL);
  }

spilled:
  ret.page = pgno2page(env, pgno);

dirty:
  if (unlikely(ret.page->mp_pgno != pgno)) {
    bad_page(ret.page,
             "mismatch actual pgno (%" PRIaPGNO ") != expected (%" PRIaPGNO
             ")\n",
             ret.page->mp_pgno, pgno);
    goto notfound;
  }

#if !MDBX_DISABLE_PAGECHECKS
  if (unlikely(ret.page->mp_flags & P_ILL_BITS)) {
    ret.err =
        bad_page(ret.page, "invalid page's flags (%u)\n", ret.page->mp_flags);
    goto bailout;
  }

  if (unlikely(ret.page->mp_txnid > front) &&
      unlikely(ret.page->mp_txnid > txn->mt_front || front < txn->mt_txnid)) {
    ret.err = bad_page(
        ret.page,
        "invalid page txnid (%" PRIaTXN ") for %s' txnid (%" PRIaTXN ")\n",
        ret.page->mp_txnid,
        (front == txn->mt_front && front != txn->mt_txnid) ? "front-txn"
                                                           : "parent-page",
        front);
    goto bailout;
  }

  if (unlikely((ret.page->mp_upper < ret.page->mp_lower ||
                ((ret.page->mp_lower | ret.page->mp_upper) & 1) ||
                PAGEHDRSZ + ret.page->mp_upper > env->me_psize) &&
               !IS_OVERFLOW(ret.page))) {
    ret.err =
        bad_page(ret.page, "invalid page lower(%u)/upper(%u) with limit (%u)\n",
                 ret.page->mp_lower, ret.page->mp_upper, page_space(env));
    goto bailout;
  }
#endif /* !MDBX_DISABLE_PAGECHECKS */

  ret.err = MDBX_SUCCESS;
  if (mdbx_audit_enabled())
    ret.err = mdbx_page_check(mc, ret.page, C_UPDATING);
  return ret;
}

/* Finish mdbx_page_search() / mdbx_page_search_lowest().
 * The cursor is at the root page, set up the rest of it. */
__hot static int mdbx_page_search_root(MDBX_cursor *mc, const MDBX_val *key,
                                       int flags) {
  MDBX_page *mp = mc->mc_pg[mc->mc_top];
  int rc;
  DKBUF_DEBUG;

  while (IS_BRANCH(mp)) {
    MDBX_node *node;
    int i;

    mdbx_debug("branch page %" PRIaPGNO " has %u keys", mp->mp_pgno,
               page_numkeys(mp));
    /* Don't assert on branch pages in the GC. We can get here
     * while in the process of rebalancing a GC branch page; we must
     * let that proceed. ITS#8336 */
    mdbx_cassert(mc, !mc->mc_dbi || page_numkeys(mp) > 1);
    mdbx_debug("found index 0 to page %" PRIaPGNO, node_pgno(page_node(mp, 0)));

    if (flags & (MDBX_PS_FIRST | MDBX_PS_LAST)) {
      i = 0;
      if (flags & MDBX_PS_LAST) {
        i = page_numkeys(mp) - 1;
        /* if already init'd, see if we're already in right place */
        if (mc->mc_flags & C_INITIALIZED) {
          if (mc->mc_ki[mc->mc_top] == i) {
            mc->mc_top = mc->mc_snum++;
            mp = mc->mc_pg[mc->mc_top];
            goto ready;
          }
        }
      }
    } else {
      const struct node_result nsr = mdbx_node_search(mc, key);
      if (nsr.node)
        i = mc->mc_ki[mc->mc_top] + nsr.exact - 1;
      else
        i = page_numkeys(mp) - 1;
      mdbx_debug("following index %u for key [%s]", i, DKEY_DEBUG(key));
    }

    mdbx_cassert(mc, i >= 0 && i < (int)page_numkeys(mp));
    node = page_node(mp, i);

    if (unlikely((rc = mdbx_page_get(mc, node_pgno(node), &mp,
                                     pp_txnid4chk(mp, mc->mc_txn))) != 0))
      return rc;

    mc->mc_ki[mc->mc_top] = (indx_t)i;
    if (unlikely(rc = mdbx_cursor_push(mc, mp)))
      return rc;

  ready:
    if (flags & MDBX_PS_MODIFY) {
      if (unlikely((rc = mdbx_page_touch(mc)) != 0))
        return rc;
      mp = mc->mc_pg[mc->mc_top];
    }
  }

#if !MDBX_DISABLE_PAGECHECKS
  if (unlikely(!IS_LEAF(mp))) {
    mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
    return bad_page(mp, "index points to a page with 0x%02x flags\n",
                    mp->mp_flags);
  }
#endif /* !MDBX_DISABLE_PAGECHECKS */

  mdbx_debug("found leaf page %" PRIaPGNO " for key [%s]", mp->mp_pgno,
             DKEY_DEBUG(key));
  mc->mc_flags |= C_INITIALIZED;
  mc->mc_flags &= ~C_EOF;

  return MDBX_SUCCESS;
}

static int mdbx_setup_dbx(MDBX_dbx *const dbx, const MDBX_db *const db,
                          const unsigned pagesize) {
  if (unlikely(!dbx->md_cmp)) {
    dbx->md_cmp = get_default_keycmp(db->md_flags);
    dbx->md_dcmp = get_default_datacmp(db->md_flags);
  }

  dbx->md_klen_min =
      (db->md_flags & MDBX_INTEGERKEY) ? 4 /* sizeof(uint32_t) */ : 0;
  dbx->md_klen_max = keysize_max(pagesize, db->md_flags);
  assert(dbx->md_klen_max != (unsigned)-1);

  dbx->md_vlen_min = (db->md_flags & MDBX_INTEGERDUP)
                         ? 4 /* sizeof(uint32_t) */
                         : ((db->md_flags & MDBX_DUPFIXED) ? 1 : 0);
  dbx->md_vlen_max = valsize_max(pagesize, db->md_flags);
  assert(dbx->md_vlen_max != (unsigned)-1);

  if ((db->md_flags & (MDBX_DUPFIXED | MDBX_INTEGERDUP)) != 0 && db->md_xsize) {
    if (!MDBX_DISABLE_PAGECHECKS && unlikely(db->md_xsize < dbx->md_vlen_min ||
                                             db->md_xsize > dbx->md_vlen_max)) {
      mdbx_error("db.md_xsize (%u) <> min/max value-length (%zu/%zu)",
                 db->md_xsize, dbx->md_vlen_min, dbx->md_vlen_max);
      return MDBX_CORRUPTED;
    }
    dbx->md_vlen_min = dbx->md_vlen_max = db->md_xsize;
  }
  return MDBX_SUCCESS;
}

static int mdbx_fetch_sdb(MDBX_txn *txn, MDBX_dbi dbi) {
  MDBX_cursor_couple couple;
  if (unlikely(TXN_DBI_CHANGED(txn, dbi)))
    return MDBX_BAD_DBI;
  int rc = mdbx_cursor_init(&couple.outer, txn, MAIN_DBI);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  MDBX_dbx *const dbx = &txn->mt_dbxs[dbi];
  rc = mdbx_page_search(&couple.outer, &dbx->md_name, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return (rc == MDBX_NOTFOUND) ? MDBX_BAD_DBI : rc;

  MDBX_val data;
  struct node_result nsr = mdbx_node_search(&couple.outer, &dbx->md_name);
  if (unlikely(!nsr.exact))
    return MDBX_BAD_DBI;
  if (unlikely((node_flags(nsr.node) & (F_DUPDATA | F_SUBDATA)) != F_SUBDATA))
    return MDBX_INCOMPATIBLE; /* not a named DB */

  const txnid_t pp_txnid =
      pp_txnid4chk(couple.outer.mc_pg[couple.outer.mc_top], txn);
  rc = mdbx_node_read(&couple.outer, nsr.node, &data, pp_txnid);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(data.iov_len != sizeof(MDBX_db)))
    return MDBX_INCOMPATIBLE; /* not a named DB */

  uint16_t md_flags = UNALIGNED_PEEK_16(data.iov_base, MDBX_db, md_flags);
  /* The txn may not know this DBI, or another process may
   * have dropped and recreated the DB with other flags. */
  MDBX_db *const db = &txn->mt_dbs[dbi];
  if (unlikely((db->md_flags & DB_PERSISTENT_FLAGS) != md_flags))
    return MDBX_INCOMPATIBLE;

  memcpy(db, data.iov_base, sizeof(MDBX_db));
#if !MDBX_DISABLE_PAGECHECKS
  mdbx_tassert(txn, txn->mt_front >= pp_txnid);
  if (unlikely(db->md_mod_txnid > pp_txnid)) {
    mdbx_error("db.md_mod_txnid (%" PRIaTXN ") > page-txnid (%" PRIaTXN ")",
               db->md_mod_txnid, pp_txnid);
    return MDBX_CORRUPTED;
  }
#endif /* !MDBX_DISABLE_PAGECHECKS */
  rc = mdbx_setup_dbx(dbx, db, txn->mt_env->me_psize);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  txn->mt_dbistate[dbi] &= ~DBI_STALE;
  return MDBX_SUCCESS;
}

/* Search for the lowest key under the current branch page.
 * This just bypasses a numkeys check in the current page
 * before calling mdbx_page_search_root(), because the callers
 * are all in situations where the current page is known to
 * be underfilled. */
__hot static int mdbx_page_search_lowest(MDBX_cursor *mc) {
  MDBX_page *mp = mc->mc_pg[mc->mc_top];
  mdbx_cassert(mc, IS_BRANCH(mp));
  MDBX_node *node = page_node(mp, 0);
  int rc;

  if (unlikely((rc = mdbx_page_get(mc, node_pgno(node), &mp,
                                   pp_txnid4chk(mp, mc->mc_txn))) != 0))
    return rc;

  mc->mc_ki[mc->mc_top] = 0;
  if (unlikely(rc = mdbx_cursor_push(mc, mp)))
    return rc;
  return mdbx_page_search_root(mc, NULL, MDBX_PS_FIRST);
}

/* Search for the page a given key should be in.
 * Push it and its parent pages on the cursor stack.
 *
 * [in,out] mc  the cursor for this operation.
 * [in] key     the key to search for, or NULL for first/last page.
 * [in] flags   If MDBX_PS_MODIFY is set, visited pages in the DB
 *              are touched (updated with new page numbers).
 *              If MDBX_PS_FIRST or MDBX_PS_LAST is set, find first or last
 * leaf.
 *              This is used by mdbx_cursor_first() and mdbx_cursor_last().
 *              If MDBX_PS_ROOTONLY set, just fetch root node, no further
 *              lookups.
 *
 * Returns 0 on success, non-zero on failure. */
__hot static int mdbx_page_search(MDBX_cursor *mc, const MDBX_val *key,
                                  int flags) {
  int rc;
  pgno_t root;

  /* Make sure the txn is still viable, then find the root from
   * the txn's db table and set it as the root of the cursor's stack. */
  if (unlikely(mc->mc_txn->mt_flags & MDBX_TXN_BLOCKED)) {
    mdbx_debug("%s", "transaction has failed, must abort");
    return MDBX_BAD_TXN;
  }

  /* Make sure we're using an up-to-date root */
  if (unlikely(*mc->mc_dbistate & DBI_STALE)) {
    rc = mdbx_fetch_sdb(mc->mc_txn, mc->mc_dbi);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }
  root = mc->mc_db->md_root;

  if (unlikely(root == P_INVALID)) { /* Tree is empty. */
    mdbx_debug("%s", "tree is empty");
    return MDBX_NOTFOUND;
  }

  mdbx_cassert(mc, root >= NUM_METAS);
  if (!mc->mc_pg[0] || mc->mc_pg[0]->mp_pgno != root) {
    txnid_t pp_txnid = mc->mc_db->md_mod_txnid;
    pp_txnid = /* mc->mc_db->md_mod_txnid maybe zero in a legacy DB */ pp_txnid
                   ? pp_txnid
                   : mc->mc_txn->mt_txnid;
    MDBX_txn *scan = mc->mc_txn;
    do
      if ((scan->mt_flags & MDBX_TXN_DIRTY) &&
          (mc->mc_dbi == MAIN_DBI ||
           (scan->mt_dbistate[mc->mc_dbi] & DBI_DIRTY))) {
        pp_txnid = scan->mt_front;
        break;
      }
    while (unlikely((scan = scan->mt_parent) != nullptr));
    if (unlikely((rc = mdbx_page_get(mc, root, &mc->mc_pg[0], pp_txnid)) != 0))
      return rc;
  }

  mc->mc_snum = 1;
  mc->mc_top = 0;

  mdbx_debug("db %d root page %" PRIaPGNO " has flags 0x%X", DDBI(mc), root,
             mc->mc_pg[0]->mp_flags);

  if (flags & MDBX_PS_MODIFY) {
    if (!(*mc->mc_dbistate & DBI_DIRTY) && unlikely(rc = mdbx_touch_dbi(mc)))
      return rc;
    if (unlikely(rc = mdbx_page_touch(mc)))
      return rc;
  }

  if (flags & MDBX_PS_ROOTONLY)
    return MDBX_SUCCESS;

  return mdbx_page_search_root(mc, key, flags);
}

/* Return the data associated with a given node.
 *
 * [in] mc      The cursor for this operation.
 * [in] leaf    The node being read.
 * [out] data   Updated to point to the node's data.
 *
 * Returns 0 on success, non-zero on failure. */
static __always_inline int mdbx_node_read(MDBX_cursor *mc,
                                          const MDBX_node *node, MDBX_val *data,
                                          const txnid_t front) {
  data->iov_len = node_ds(node);
  data->iov_base = node_data(node);
  if (unlikely(F_ISSET(node_flags(node), F_BIGDATA))) {
    /* Read overflow data. */
    MDBX_page *omp; /* overflow page */
    int rc = mdbx_page_get(mc, node_largedata_pgno(node), &omp, front);
    if (unlikely((rc != MDBX_SUCCESS))) {
      mdbx_debug("read overflow page %" PRIaPGNO " failed",
                 node_largedata_pgno(node));
      return rc;
    }
    data->iov_base = page_data(omp);
  }
  return MDBX_SUCCESS;
}

int mdbx_get(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key, MDBX_val *data) {
  DKBUF_DEBUG;
  mdbx_debug("===> get db %u key [%s]", dbi, DKEY_DEBUG(key));

  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key || !data))
    return MDBX_EINVAL;

  if (unlikely(!check_dbi(txn, dbi, DBI_USRVALID)))
    return MDBX_BAD_DBI;

  MDBX_cursor_couple cx;
  rc = mdbx_cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_cursor_set(&cx.outer, (MDBX_val *)key, data, MDBX_SET).err;
}

int mdbx_get_equal_or_great(MDBX_txn *txn, MDBX_dbi dbi, MDBX_val *key,
                            MDBX_val *data) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key || !data))
    return MDBX_EINVAL;

  if (unlikely(!check_dbi(txn, dbi, DBI_USRVALID)))
    return MDBX_BAD_DBI;

  if (unlikely(txn->mt_flags & MDBX_TXN_BLOCKED))
    return MDBX_BAD_TXN;

  MDBX_cursor_couple cx;
  rc = mdbx_cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_cursor_get(&cx.outer, key, data, MDBX_SET_LOWERBOUND);
}

int mdbx_get_ex(MDBX_txn *txn, MDBX_dbi dbi, MDBX_val *key, MDBX_val *data,
                size_t *values_count) {
  DKBUF_DEBUG;
  mdbx_debug("===> get db %u key [%s]", dbi, DKEY_DEBUG(key));

  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key || !data))
    return MDBX_EINVAL;

  if (unlikely(!check_dbi(txn, dbi, DBI_USRVALID)))
    return MDBX_BAD_DBI;

  MDBX_cursor_couple cx;
  rc = mdbx_cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = mdbx_cursor_set(&cx.outer, key, data, MDBX_SET_KEY).err;
  if (unlikely(rc != MDBX_SUCCESS)) {
    if (rc == MDBX_NOTFOUND && values_count)
      *values_count = 0;
    return rc;
  }

  if (values_count) {
    *values_count = 1;
    if (cx.outer.mc_xcursor != NULL) {
      MDBX_node *node = page_node(cx.outer.mc_pg[cx.outer.mc_top],
                                  cx.outer.mc_ki[cx.outer.mc_top]);
      if (F_ISSET(node_flags(node), F_DUPDATA)) {
        // coverity[uninit_use : FALSE]
        mdbx_tassert(txn, cx.outer.mc_xcursor == &cx.inner &&
                              (cx.inner.mx_cursor.mc_flags & C_INITIALIZED));
        // coverity[uninit_use : FALSE]
        *values_count =
            (sizeof(*values_count) >= sizeof(cx.inner.mx_db.md_entries) ||
             cx.inner.mx_db.md_entries <= PTRDIFF_MAX)
                ? (size_t)cx.inner.mx_db.md_entries
                : PTRDIFF_MAX;
      }
    }
  }
  return MDBX_SUCCESS;
}

/* Find a sibling for a page.
 * Replaces the page at the top of the cursor's stack with the specified
 * sibling, if one exists.
 *
 * [in] mc    The cursor for this operation.
 * [in] dir   SIBLING_LEFT or SIBLING_RIGHT.
 *
 * Returns 0 on success, non-zero on failure. */
static int mdbx_cursor_sibling(MDBX_cursor *mc, int dir) {
  int rc;
  MDBX_node *node;
  MDBX_page *mp;
  assert(dir == SIBLING_LEFT || dir == SIBLING_RIGHT);

  if (unlikely(mc->mc_snum < 2))
    return MDBX_NOTFOUND; /* root has no siblings */

  mdbx_cursor_pop(mc);
  mdbx_debug("parent page is page %" PRIaPGNO ", index %u",
             mc->mc_pg[mc->mc_top]->mp_pgno, mc->mc_ki[mc->mc_top]);

  if ((dir == SIBLING_RIGHT)
          ? (mc->mc_ki[mc->mc_top] + 1u >= page_numkeys(mc->mc_pg[mc->mc_top]))
          : (mc->mc_ki[mc->mc_top] == 0)) {
    mdbx_debug("no more keys aside, moving to next %s sibling",
               dir ? "right" : "left");
    if (unlikely((rc = mdbx_cursor_sibling(mc, dir)) != MDBX_SUCCESS)) {
      /* undo cursor_pop before returning */
      mc->mc_top++;
      mc->mc_snum++;
      return rc;
    }
  } else {
    assert((dir - 1) == -1 || (dir - 1) == 1);
    mc->mc_ki[mc->mc_top] += (indx_t)(dir - 1);
    mdbx_debug("just moving to %s index key %u",
               (dir == SIBLING_RIGHT) ? "right" : "left",
               mc->mc_ki[mc->mc_top]);
  }
  mdbx_cassert(mc, IS_BRANCH(mc->mc_pg[mc->mc_top]));

  node = page_node(mp = mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
  if (unlikely((rc = mdbx_page_get(mc, node_pgno(node), &mp,
                                   pp_txnid4chk(mp, mc->mc_txn))) != 0)) {
    /* mc will be inconsistent if caller does mc_snum++ as above */
    mc->mc_flags &= ~(C_INITIALIZED | C_EOF);
    return rc;
  }

  rc = mdbx_cursor_push(mc, mp);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  mc->mc_ki[mc->mc_top] =
      (indx_t)((dir == SIBLING_LEFT) ? page_numkeys(mp) - 1 : 0);
  return MDBX_SUCCESS;
}

/* Move the cursor to the next data item. */
static int mdbx_cursor_next(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data,
                            MDBX_cursor_op op) {
  MDBX_page *mp;
  MDBX_node *node;
  int rc;

  if (unlikely(mc->mc_flags & C_DEL) && op == MDBX_NEXT_DUP)
    return MDBX_NOTFOUND;

  if (unlikely(!(mc->mc_flags & C_INITIALIZED)))
    return mdbx_cursor_first(mc, key, data);

  mp = mc->mc_pg[mc->mc_top];
  if (unlikely(mc->mc_flags & C_EOF)) {
    if (mc->mc_ki[mc->mc_top] + 1u >= page_numkeys(mp))
      return MDBX_NOTFOUND;
    mc->mc_flags ^= C_EOF;
  }

  if (mc->mc_db->md_flags & MDBX_DUPSORT) {
    node = page_node(mp, mc->mc_ki[mc->mc_top]);
    if (F_ISSET(node_flags(node), F_DUPDATA)) {
      if (op == MDBX_NEXT || op == MDBX_NEXT_DUP) {
        rc =
            mdbx_cursor_next(&mc->mc_xcursor->mx_cursor, data, NULL, MDBX_NEXT);
        if (op != MDBX_NEXT || rc != MDBX_NOTFOUND) {
          if (likely(rc == MDBX_SUCCESS))
            get_key_optional(node, key);
          return rc;
        }
      }
    } else {
      mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED | C_EOF);
      if (op == MDBX_NEXT_DUP)
        return MDBX_NOTFOUND;
    }
  }

  mdbx_debug("cursor_next: top page is %" PRIaPGNO " in cursor %p", mp->mp_pgno,
             (void *)mc);
  if (mc->mc_flags & C_DEL) {
    mc->mc_flags ^= C_DEL;
    goto skip;
  }

  int ki = mc->mc_ki[mc->mc_top];
  mc->mc_ki[mc->mc_top] = (indx_t)++ki;
  const int numkeys = page_numkeys(mp);
  if (unlikely(ki >= numkeys)) {
    mdbx_debug("%s", "=====> move to next sibling page");
    mc->mc_ki[mc->mc_top] = (indx_t)(numkeys - 1);
    if (unlikely((rc = mdbx_cursor_sibling(mc, SIBLING_RIGHT)) !=
                 MDBX_SUCCESS)) {
      mc->mc_flags |= C_EOF;
      return rc;
    }
    mp = mc->mc_pg[mc->mc_top];
    mdbx_debug("next page is %" PRIaPGNO ", key index %u", mp->mp_pgno,
               mc->mc_ki[mc->mc_top]);
  }

skip:
  mdbx_debug("==> cursor points to page %" PRIaPGNO
             " with %u keys, key index %u",
             mp->mp_pgno, page_numkeys(mp), mc->mc_ki[mc->mc_top]);

  if (!MDBX_DISABLE_PAGECHECKS && unlikely(!IS_LEAF(mp)))
    return MDBX_CORRUPTED;

  if (IS_LEAF2(mp)) {
    if (!MDBX_DISABLE_PAGECHECKS && unlikely((mc->mc_flags & C_SUB) == 0)) {
      mdbx_error("unexpected LEAF2-page %" PRIaPGNO "for non-dupsort cursor",
                 mp->mp_pgno);
      return MDBX_CORRUPTED;
    } else if (likely(key)) {
      key->iov_len = mc->mc_db->md_xsize;
      key->iov_base = page_leaf2key(mp, mc->mc_ki[mc->mc_top], key->iov_len);
    }
    return MDBX_SUCCESS;
  }

  node = page_node(mp, mc->mc_ki[mc->mc_top]);
  if (F_ISSET(node_flags(node), F_DUPDATA)) {
    rc = mdbx_xcursor_init1(mc, node, mp);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    rc = mdbx_cursor_first(&mc->mc_xcursor->mx_cursor, data, NULL);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  } else if (likely(data)) {
    if (unlikely((rc = mdbx_node_read(mc, node, data,
                                      pp_txnid4chk(mp, mc->mc_txn))) !=
                 MDBX_SUCCESS))
      return rc;
  }

  get_key_optional(node, key);
  return MDBX_SUCCESS;
}

/* Move the cursor to the previous data item. */
static int mdbx_cursor_prev(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data,
                            MDBX_cursor_op op) {
  MDBX_page *mp;
  MDBX_node *node;
  int rc;

  if (unlikely(mc->mc_flags & C_DEL) && op == MDBX_PREV_DUP)
    return MDBX_NOTFOUND;

  if (unlikely(!(mc->mc_flags & C_INITIALIZED))) {
    rc = mdbx_cursor_last(mc, key, data);
    if (unlikely(rc))
      return rc;
    mc->mc_ki[mc->mc_top]++;
  }

  mp = mc->mc_pg[mc->mc_top];
  if ((mc->mc_db->md_flags & MDBX_DUPSORT) &&
      mc->mc_ki[mc->mc_top] < page_numkeys(mp)) {
    node = page_node(mp, mc->mc_ki[mc->mc_top]);
    if (F_ISSET(node_flags(node), F_DUPDATA)) {
      if (op == MDBX_PREV || op == MDBX_PREV_DUP) {
        rc =
            mdbx_cursor_prev(&mc->mc_xcursor->mx_cursor, data, NULL, MDBX_PREV);
        if (op != MDBX_PREV || rc != MDBX_NOTFOUND) {
          if (likely(rc == MDBX_SUCCESS)) {
            get_key_optional(node, key);
            mc->mc_flags &= ~C_EOF;
          }
          return rc;
        }
      }
    } else {
      mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED | C_EOF);
      if (op == MDBX_PREV_DUP)
        return MDBX_NOTFOUND;
    }
  }

  mdbx_debug("cursor_prev: top page is %" PRIaPGNO " in cursor %p", mp->mp_pgno,
             (void *)mc);

  mc->mc_flags &= ~(C_EOF | C_DEL);

  int ki = mc->mc_ki[mc->mc_top];
  mc->mc_ki[mc->mc_top] = (indx_t)--ki;
  if (unlikely(ki < 0)) {
    mc->mc_ki[mc->mc_top] = 0;
    mdbx_debug("%s", "=====> move to prev sibling page");
    if ((rc = mdbx_cursor_sibling(mc, SIBLING_LEFT)) != MDBX_SUCCESS)
      return rc;
    mp = mc->mc_pg[mc->mc_top];
    mdbx_debug("prev page is %" PRIaPGNO ", key index %u", mp->mp_pgno,
               mc->mc_ki[mc->mc_top]);
  }
  mdbx_debug("==> cursor points to page %" PRIaPGNO
             " with %u keys, key index %u",
             mp->mp_pgno, page_numkeys(mp), mc->mc_ki[mc->mc_top]);

  if (!MDBX_DISABLE_PAGECHECKS && unlikely(!IS_LEAF(mp)))
    return MDBX_CORRUPTED;

  if (IS_LEAF2(mp)) {
    if (!MDBX_DISABLE_PAGECHECKS && unlikely((mc->mc_flags & C_SUB) == 0)) {
      mdbx_error("unexpected LEAF2-page %" PRIaPGNO "for non-dupsort cursor",
                 mp->mp_pgno);
      return MDBX_CORRUPTED;
    } else if (likely(key)) {
      key->iov_len = mc->mc_db->md_xsize;
      key->iov_base = page_leaf2key(mp, mc->mc_ki[mc->mc_top], key->iov_len);
    }
    return MDBX_SUCCESS;
  }

  node = page_node(mp, mc->mc_ki[mc->mc_top]);

  if (F_ISSET(node_flags(node), F_DUPDATA)) {
    rc = mdbx_xcursor_init1(mc, node, mp);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    rc = mdbx_cursor_last(&mc->mc_xcursor->mx_cursor, data, NULL);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  } else if (likely(data)) {
    if (unlikely((rc = mdbx_node_read(mc, node, data,
                                      pp_txnid4chk(mp, mc->mc_txn))) !=
                 MDBX_SUCCESS))
      return rc;
  }

  get_key_optional(node, key);
  return MDBX_SUCCESS;
}

/* Set the cursor on a specific data item. */
static struct cursor_set_result mdbx_cursor_set(MDBX_cursor *mc, MDBX_val *key,
                                                MDBX_val *data,
                                                MDBX_cursor_op op) {
  MDBX_page *mp;
  MDBX_node *node = NULL;
  DKBUF_DEBUG;

  struct cursor_set_result ret;
  ret.exact = false;
  if (unlikely(key->iov_len < mc->mc_dbx->md_klen_min ||
               key->iov_len > mc->mc_dbx->md_klen_max)) {
    mdbx_cassert(mc, !"Invalid key-size");
    ret.err = MDBX_BAD_VALSIZE;
    return ret;
  }

  MDBX_val aligned_key = *key;
  uint64_t aligned_keybytes;
  if (mc->mc_db->md_flags & MDBX_INTEGERKEY) {
    switch (aligned_key.iov_len) {
    default:
      mdbx_cassert(mc, !"key-size is invalid for MDBX_INTEGERKEY");
      ret.err = MDBX_BAD_VALSIZE;
      return ret;
    case 4:
      if (unlikely(3 & (uintptr_t)aligned_key.iov_base))
        /* copy instead of return error to avoid break compatibility */
        aligned_key.iov_base =
            memcpy(&aligned_keybytes, aligned_key.iov_base, 4);
      break;
    case 8:
      if (unlikely(7 & (uintptr_t)aligned_key.iov_base))
        /* copy instead of return error to avoid break compatibility */
        aligned_key.iov_base =
            memcpy(&aligned_keybytes, aligned_key.iov_base, 8);
      break;
    }
  }

  if (mc->mc_xcursor)
    mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED | C_EOF);

  /* See if we're already on the right page */
  if (mc->mc_flags & C_INITIALIZED) {
    MDBX_val nodekey;

    mdbx_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]));
    mp = mc->mc_pg[mc->mc_top];
    if (unlikely(!page_numkeys(mp))) {
      mc->mc_ki[mc->mc_top] = 0;
      mc->mc_flags |= C_EOF;
      ret.err = MDBX_NOTFOUND;
      return ret;
    }
    if (IS_LEAF2(mp)) {
      nodekey.iov_len = mc->mc_db->md_xsize;
      nodekey.iov_base = page_leaf2key(mp, 0, nodekey.iov_len);
    } else {
      node = page_node(mp, 0);
      get_key(node, &nodekey);
    }
    int cmp = mc->mc_dbx->md_cmp(&aligned_key, &nodekey);
    if (unlikely(cmp == 0)) {
      /* Probably happens rarely, but first node on the page
       * was the one we wanted. */
      mc->mc_ki[mc->mc_top] = 0;
      ret.exact = true;
      mdbx_cassert(mc, mc->mc_ki[mc->mc_top] <
                               page_numkeys(mc->mc_pg[mc->mc_top]) ||
                           (mc->mc_flags & C_EOF));
      goto got_node;
    }
    if (cmp > 0) {
      const unsigned nkeys = page_numkeys(mp);
      if (nkeys > 1) {
        if (IS_LEAF2(mp)) {
          nodekey.iov_base = page_leaf2key(mp, nkeys - 1, nodekey.iov_len);
        } else {
          node = page_node(mp, nkeys - 1);
          get_key(node, &nodekey);
        }
        cmp = mc->mc_dbx->md_cmp(&aligned_key, &nodekey);
        if (cmp == 0) {
          /* last node was the one we wanted */
          mdbx_cassert(mc, nkeys >= 1 && nkeys <= UINT16_MAX + 1);
          mc->mc_ki[mc->mc_top] = (indx_t)(nkeys - 1);
          ret.exact = true;
          mdbx_cassert(mc, mc->mc_ki[mc->mc_top] <
                                   page_numkeys(mc->mc_pg[mc->mc_top]) ||
                               (mc->mc_flags & C_EOF));
          goto got_node;
        }
        if (cmp < 0) {
          if (mc->mc_ki[mc->mc_top] < page_numkeys(mp)) {
            /* This is definitely the right page, skip search_page */
            if (IS_LEAF2(mp)) {
              nodekey.iov_base =
                  page_leaf2key(mp, mc->mc_ki[mc->mc_top], nodekey.iov_len);
            } else {
              node = page_node(mp, mc->mc_ki[mc->mc_top]);
              get_key(node, &nodekey);
            }
            cmp = mc->mc_dbx->md_cmp(&aligned_key, &nodekey);
            if (cmp == 0) {
              /* current node was the one we wanted */
              ret.exact = true;
              mdbx_cassert(mc, mc->mc_ki[mc->mc_top] <
                                       page_numkeys(mc->mc_pg[mc->mc_top]) ||
                                   (mc->mc_flags & C_EOF));
              goto got_node;
            }
          }
          mc->mc_flags &= ~C_EOF;
          goto search_node;
        }
      }
      /* If any parents have right-sibs, search.
       * Otherwise, there's nothing further. */
      unsigned i;
      for (i = 0; i < mc->mc_top; i++)
        if (mc->mc_ki[i] < page_numkeys(mc->mc_pg[i]) - 1)
          break;
      if (i == mc->mc_top) {
        /* There are no other pages */
        mdbx_cassert(mc, nkeys <= UINT16_MAX);
        mc->mc_ki[mc->mc_top] = (uint16_t)nkeys;
        mc->mc_flags |= C_EOF;
        ret.err = MDBX_NOTFOUND;
        return ret;
      }
    }
    if (!mc->mc_top) {
      /* There are no other pages */
      mc->mc_ki[mc->mc_top] = 0;
      if (op == MDBX_SET_RANGE)
        goto got_node;

      mdbx_cassert(mc, mc->mc_ki[mc->mc_top] <
                               page_numkeys(mc->mc_pg[mc->mc_top]) ||
                           (mc->mc_flags & C_EOF));
      ret.err = MDBX_NOTFOUND;
      return ret;
    }
  } else {
    mc->mc_pg[0] = 0;
  }

  ret.err = mdbx_page_search(mc, &aligned_key, 0);
  if (unlikely(ret.err != MDBX_SUCCESS))
    return ret;

  mp = mc->mc_pg[mc->mc_top];
  mdbx_cassert(mc, IS_LEAF(mp));

search_node:;
  struct node_result nsr = mdbx_node_search(mc, &aligned_key);
  node = nsr.node;
  ret.exact = nsr.exact;
  if (!ret.exact) {
    if (op != MDBX_SET_RANGE) {
      /* MDBX_SET specified and not an exact match. */
      if (unlikely(mc->mc_ki[mc->mc_top] >=
                   page_numkeys(mc->mc_pg[mc->mc_top])))
        mc->mc_flags |= C_EOF;
      ret.err = MDBX_NOTFOUND;
      return ret;
    }

    if (node == NULL) {
      mdbx_debug("%s", "===> inexact leaf not found, goto sibling");
      ret.err = mdbx_cursor_sibling(mc, SIBLING_RIGHT);
      if (unlikely(ret.err != MDBX_SUCCESS)) {
        mc->mc_flags |= C_EOF;
        return ret; /* no entries matched */
      }
      mp = mc->mc_pg[mc->mc_top];
      mdbx_cassert(mc, IS_LEAF(mp));
      if (!IS_LEAF2(mp))
        node = page_node(mp, 0);
    }
  }
  mdbx_cassert(mc,
               mc->mc_ki[mc->mc_top] < page_numkeys(mc->mc_pg[mc->mc_top]) ||
                   (mc->mc_flags & C_EOF));

got_node:
  mc->mc_flags |= C_INITIALIZED;
  mc->mc_flags &= ~C_EOF;

  if (IS_LEAF2(mp)) {
    if (!MDBX_DISABLE_PAGECHECKS && unlikely((mc->mc_flags & C_SUB) == 0)) {
      mdbx_error("unexpected LEAF2-page %" PRIaPGNO "for non-dupsort cursor",
                 mp->mp_pgno);
      ret.err = MDBX_CORRUPTED;
    } else {
      if (op == MDBX_SET_RANGE || op == MDBX_SET_KEY) {
        key->iov_len = mc->mc_db->md_xsize;
        key->iov_base = page_leaf2key(mp, mc->mc_ki[mc->mc_top], key->iov_len);
      }
      ret.err = MDBX_SUCCESS;
    }
    return ret;
  }

  if (F_ISSET(node_flags(node), F_DUPDATA)) {
    ret.err = mdbx_xcursor_init1(mc, node, mp);
    if (unlikely(ret.err != MDBX_SUCCESS))
      return ret;
    if (op == MDBX_SET || op == MDBX_SET_KEY || op == MDBX_SET_RANGE) {
      ret.err = mdbx_cursor_first(&mc->mc_xcursor->mx_cursor, data, NULL);
      if (unlikely(ret.err != MDBX_SUCCESS))
        return ret;
    } else {
      ret = mdbx_cursor_set(&mc->mc_xcursor->mx_cursor, data, NULL,
                            MDBX_SET_RANGE);
      if (unlikely(ret.err != MDBX_SUCCESS))
        return ret;
      if (op == MDBX_GET_BOTH && !ret.exact) {
        ret.err = MDBX_NOTFOUND;
        return ret;
      }
    }
  } else if (likely(data)) {
    if (op == MDBX_GET_BOTH || op == MDBX_GET_BOTH_RANGE) {
      if (unlikely(data->iov_len < mc->mc_dbx->md_vlen_min ||
                   data->iov_len > mc->mc_dbx->md_vlen_max)) {
        mdbx_cassert(mc, !"Invalid data-size");
        ret.err = MDBX_BAD_VALSIZE;
        return ret;
      }
      MDBX_val aligned_data = *data;
      uint64_t aligned_databytes;
      if (mc->mc_db->md_flags & MDBX_INTEGERDUP) {
        switch (aligned_data.iov_len) {
        default:
          mdbx_cassert(mc, !"data-size is invalid for MDBX_INTEGERDUP");
          ret.err = MDBX_BAD_VALSIZE;
          return ret;
        case 4:
          if (unlikely(3 & (uintptr_t)aligned_data.iov_base))
            /* copy instead of return error to avoid break compatibility */
            aligned_data.iov_base =
                memcpy(&aligned_databytes, aligned_data.iov_base, 4);
          break;
        case 8:
          if (unlikely(7 & (uintptr_t)aligned_data.iov_base))
            /* copy instead of return error to avoid break compatibility */
            aligned_data.iov_base =
                memcpy(&aligned_databytes, aligned_data.iov_base, 8);
          break;
        }
      }
      MDBX_val actual_data;
      ret.err = mdbx_node_read(mc, node, &actual_data,
                               pp_txnid4chk(mc->mc_pg[mc->mc_top], mc->mc_txn));
      if (unlikely(ret.err != MDBX_SUCCESS))
        return ret;
      const int cmp = mc->mc_dbx->md_dcmp(&aligned_data, &actual_data);
      if (cmp) {
        mdbx_cassert(mc, mc->mc_ki[mc->mc_top] <
                                 page_numkeys(mc->mc_pg[mc->mc_top]) ||
                             (mc->mc_flags & C_EOF));
        if (op != MDBX_GET_BOTH_RANGE || cmp > 0) {
          ret.err = MDBX_NOTFOUND;
          return ret;
        }
      }
      *data = actual_data;
    } else {
      ret.err = mdbx_node_read(mc, node, data,
                               pp_txnid4chk(mc->mc_pg[mc->mc_top], mc->mc_txn));
      if (unlikely(ret.err != MDBX_SUCCESS))
        return ret;
    }
  }

  /* The key already matches in all other cases */
  if (op == MDBX_SET_RANGE || op == MDBX_SET_KEY)
    get_key_optional(node, key);

  mdbx_debug("==> cursor placed on key [%s], data [%s]", DKEY_DEBUG(key),
             DVAL_DEBUG(data));
  ret.err = MDBX_SUCCESS;
  return ret;
}

/* Move the cursor to the first item in the database. */
static int mdbx_cursor_first(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data) {
  int rc;

  if (mc->mc_xcursor)
    mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED | C_EOF);

  if (!(mc->mc_flags & C_INITIALIZED) || mc->mc_top) {
    rc = mdbx_page_search(mc, NULL, MDBX_PS_FIRST);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  if (!MDBX_DISABLE_PAGECHECKS && unlikely(!IS_LEAF(mc->mc_pg[mc->mc_top])))
    return MDBX_CORRUPTED;

  mc->mc_flags |= C_INITIALIZED;
  mc->mc_flags &= ~C_EOF;
  mc->mc_ki[mc->mc_top] = 0;

  if (IS_LEAF2(mc->mc_pg[mc->mc_top])) {
    if (!MDBX_DISABLE_PAGECHECKS && unlikely((mc->mc_flags & C_SUB) == 0)) {
      mdbx_error("unexpected LEAF2-page %" PRIaPGNO "for non-dupsort cursor",
                 mc->mc_pg[mc->mc_top]->mp_pgno);
      return MDBX_CORRUPTED;
    } else if (likely(key)) {
      key->iov_len = mc->mc_db->md_xsize;
      key->iov_base = page_leaf2key(mc->mc_pg[mc->mc_top], 0, key->iov_len);
    }
    return MDBX_SUCCESS;
  }

  MDBX_node *node = page_node(mc->mc_pg[mc->mc_top], 0);
  if (F_ISSET(node_flags(node), F_DUPDATA)) {
    rc = mdbx_xcursor_init1(mc, node, mc->mc_pg[mc->mc_top]);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    rc = mdbx_cursor_first(&mc->mc_xcursor->mx_cursor, data, NULL);
    if (unlikely(rc))
      return rc;
  } else if (likely(data)) {
    if (unlikely((rc = mdbx_node_read(
                      mc, node, data,
                      pp_txnid4chk(mc->mc_pg[mc->mc_top], mc->mc_txn))) !=
                 MDBX_SUCCESS))
      return rc;
  }

  get_key_optional(node, key);
  return MDBX_SUCCESS;
}

/* Move the cursor to the last item in the database. */
static int mdbx_cursor_last(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data) {
  int rc;

  if (mc->mc_xcursor)
    mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED | C_EOF);

  if (!(mc->mc_flags & C_INITIALIZED) || mc->mc_top) {
    rc = mdbx_page_search(mc, NULL, MDBX_PS_LAST);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  if (!MDBX_DISABLE_PAGECHECKS && unlikely(!IS_LEAF(mc->mc_pg[mc->mc_top])))
    return MDBX_CORRUPTED;

  mc->mc_ki[mc->mc_top] = (indx_t)page_numkeys(mc->mc_pg[mc->mc_top]) - 1;
  mc->mc_flags |= C_INITIALIZED | C_EOF;

  if (IS_LEAF2(mc->mc_pg[mc->mc_top])) {
    if (!MDBX_DISABLE_PAGECHECKS && unlikely((mc->mc_flags & C_SUB) == 0)) {
      mdbx_error("unexpected LEAF2-page %" PRIaPGNO "for non-dupsort cursor",
                 mc->mc_pg[mc->mc_top]->mp_pgno);
      return MDBX_CORRUPTED;
    } else if (likely(key)) {
      key->iov_len = mc->mc_db->md_xsize;
      key->iov_base = page_leaf2key(mc->mc_pg[mc->mc_top],
                                    mc->mc_ki[mc->mc_top], key->iov_len);
    }
    return MDBX_SUCCESS;
  }

  MDBX_node *node = page_node(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
  if (F_ISSET(node_flags(node), F_DUPDATA)) {
    rc = mdbx_xcursor_init1(mc, node, mc->mc_pg[mc->mc_top]);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    rc = mdbx_cursor_last(&mc->mc_xcursor->mx_cursor, data, NULL);
    if (unlikely(rc))
      return rc;
  } else if (likely(data)) {
    if (unlikely((rc = mdbx_node_read(
                      mc, node, data,
                      pp_txnid4chk(mc->mc_pg[mc->mc_top], mc->mc_txn))) !=
                 MDBX_SUCCESS))
      return rc;
  }

  get_key_optional(node, key);
  return MDBX_SUCCESS;
}

int mdbx_cursor_get(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data,
                    MDBX_cursor_op op) {
  if (unlikely(mc == NULL))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_signature != MDBX_MC_LIVE))
    return (mc->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                     : MDBX_EBADSIGN;

  int rc = check_txn(mc->mc_txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  int (*mfunc)(MDBX_cursor * mc, MDBX_val * key, MDBX_val * data);
  switch (op) {
  case MDBX_GET_CURRENT: {
    if (unlikely(!(mc->mc_flags & C_INITIALIZED)))
      return MDBX_ENODATA;
    MDBX_page *mp = mc->mc_pg[mc->mc_top];
    const unsigned nkeys = page_numkeys(mp);
    if (unlikely(mc->mc_ki[mc->mc_top] >= nkeys)) {
      mdbx_cassert(mc, nkeys <= UINT16_MAX);
      if (mc->mc_flags & C_EOF)
        return MDBX_ENODATA;
      mc->mc_ki[mc->mc_top] = (uint16_t)nkeys;
      mc->mc_flags |= C_EOF;
      return MDBX_NOTFOUND;
    }
    mdbx_cassert(mc, nkeys > 0);

    rc = MDBX_SUCCESS;
    if (IS_LEAF2(mp)) {
      if (!MDBX_DISABLE_PAGECHECKS && unlikely((mc->mc_flags & C_SUB) == 0)) {
        mdbx_error("unexpected LEAF2-page %" PRIaPGNO "for non-dupsort cursor",
                   mp->mp_pgno);
        return MDBX_CORRUPTED;
      }
      key->iov_len = mc->mc_db->md_xsize;
      key->iov_base = page_leaf2key(mp, mc->mc_ki[mc->mc_top], key->iov_len);
    } else {
      MDBX_node *node = page_node(mp, mc->mc_ki[mc->mc_top]);
      get_key_optional(node, key);
      if (data) {
        if (F_ISSET(node_flags(node), F_DUPDATA)) {
          if (unlikely(!(mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED))) {
            rc = mdbx_xcursor_init1(mc, node, mp);
            if (unlikely(rc != MDBX_SUCCESS))
              return rc;
            rc = mdbx_cursor_first(&mc->mc_xcursor->mx_cursor, data, NULL);
            if (unlikely(rc))
              return rc;
          } else {
            rc = mdbx_cursor_get(&mc->mc_xcursor->mx_cursor, data, NULL,
                                 MDBX_GET_CURRENT);
            if (unlikely(rc))
              return rc;
          }
        } else {
          rc = mdbx_node_read(mc, node, data, pp_txnid4chk(mp, mc->mc_txn));
          if (unlikely(rc))
            return rc;
        }
      }
    }
    break;
  }
  case MDBX_GET_BOTH:
  case MDBX_GET_BOTH_RANGE:
    if (unlikely(data == NULL))
      return MDBX_EINVAL;
    if (unlikely(mc->mc_xcursor == NULL))
      return MDBX_INCOMPATIBLE;
    /* fall through */
    __fallthrough;
  case MDBX_SET:
  case MDBX_SET_KEY:
  case MDBX_SET_RANGE:
    if (unlikely(key == NULL))
      return MDBX_EINVAL;
    rc = mdbx_cursor_set(mc, key, data, op).err;
    if (mc->mc_flags & C_INITIALIZED) {
      mdbx_cassert(mc, mc->mc_snum > 0 && mc->mc_top < mc->mc_snum);
      mdbx_cassert(mc, mc->mc_ki[mc->mc_top] <
                               page_numkeys(mc->mc_pg[mc->mc_top]) ||
                           (mc->mc_flags & C_EOF));
    }
    break;
  case MDBX_GET_MULTIPLE:
    if (unlikely(data == NULL || !(mc->mc_flags & C_INITIALIZED)))
      return MDBX_EINVAL;
    if (unlikely(!(mc->mc_db->md_flags & MDBX_DUPFIXED)))
      return MDBX_INCOMPATIBLE;
    rc = MDBX_SUCCESS;
    if ((mc->mc_xcursor->mx_cursor.mc_flags & (C_INITIALIZED | C_EOF)) !=
        C_INITIALIZED)
      break;
    goto fetchm;
  case MDBX_NEXT_MULTIPLE:
    if (unlikely(data == NULL))
      return MDBX_EINVAL;
    if (unlikely(!(mc->mc_db->md_flags & MDBX_DUPFIXED)))
      return MDBX_INCOMPATIBLE;
    rc = mdbx_cursor_next(mc, key, data, MDBX_NEXT_DUP);
    if (rc == MDBX_SUCCESS) {
      if (mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED) {
        MDBX_cursor *mx;
      fetchm:
        mx = &mc->mc_xcursor->mx_cursor;
        data->iov_len =
            page_numkeys(mx->mc_pg[mx->mc_top]) * mx->mc_db->md_xsize;
        data->iov_base = page_data(mx->mc_pg[mx->mc_top]);
        mx->mc_ki[mx->mc_top] = (indx_t)page_numkeys(mx->mc_pg[mx->mc_top]) - 1;
      } else {
        rc = MDBX_NOTFOUND;
      }
    }
    break;
  case MDBX_PREV_MULTIPLE:
    if (data == NULL)
      return MDBX_EINVAL;
    if (!(mc->mc_db->md_flags & MDBX_DUPFIXED))
      return MDBX_INCOMPATIBLE;
    rc = MDBX_SUCCESS;
    if (!(mc->mc_flags & C_INITIALIZED))
      rc = mdbx_cursor_last(mc, key, data);
    if (rc == MDBX_SUCCESS) {
      MDBX_cursor *mx = &mc->mc_xcursor->mx_cursor;
      if (mx->mc_flags & C_INITIALIZED) {
        rc = mdbx_cursor_sibling(mx, SIBLING_LEFT);
        if (rc == MDBX_SUCCESS)
          goto fetchm;
      } else {
        rc = MDBX_NOTFOUND;
      }
    }
    break;
  case MDBX_NEXT:
  case MDBX_NEXT_DUP:
  case MDBX_NEXT_NODUP:
    rc = mdbx_cursor_next(mc, key, data, op);
    break;
  case MDBX_PREV:
  case MDBX_PREV_DUP:
  case MDBX_PREV_NODUP:
    rc = mdbx_cursor_prev(mc, key, data, op);
    break;
  case MDBX_FIRST:
    rc = mdbx_cursor_first(mc, key, data);
    break;
  case MDBX_FIRST_DUP:
    mfunc = mdbx_cursor_first;
  move:
    if (unlikely(data == NULL || !(mc->mc_flags & C_INITIALIZED)))
      return MDBX_EINVAL;
    if (unlikely(mc->mc_xcursor == NULL))
      return MDBX_INCOMPATIBLE;
    if (mc->mc_ki[mc->mc_top] >= page_numkeys(mc->mc_pg[mc->mc_top])) {
      mc->mc_ki[mc->mc_top] = (indx_t)page_numkeys(mc->mc_pg[mc->mc_top]);
      mc->mc_flags |= C_EOF;
      return MDBX_NOTFOUND;
    }
    {
      MDBX_node *node = page_node(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
      if (!F_ISSET(node_flags(node), F_DUPDATA)) {
        get_key_optional(node, key);
        rc = mdbx_node_read(mc, node, data,
                            pp_txnid4chk(mc->mc_pg[mc->mc_top], mc->mc_txn));
        break;
      }
    }
    if (unlikely(!(mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED)))
      return MDBX_EINVAL;
    rc = mfunc(&mc->mc_xcursor->mx_cursor, data, NULL);
    break;
  case MDBX_LAST:
    rc = mdbx_cursor_last(mc, key, data);
    break;
  case MDBX_LAST_DUP:
    mfunc = mdbx_cursor_last;
    goto move;
  case MDBX_SET_UPPERBOUND: /* mostly same as MDBX_SET_LOWERBOUND */
  case MDBX_SET_LOWERBOUND: {
    if (unlikely(key == NULL || data == NULL))
      return MDBX_EINVAL;
    MDBX_val save_data = *data;
    struct cursor_set_result csr =
        mdbx_cursor_set(mc, key, data, MDBX_SET_RANGE);
    rc = csr.err;
    if (rc == MDBX_SUCCESS && csr.exact && mc->mc_xcursor) {
      mc->mc_flags &= ~C_DEL;
      csr.exact = false;
      if (!save_data.iov_base && (mc->mc_db->md_flags & MDBX_DUPFIXED)) {
        /* Avoiding search nested dupfixed hive if no data provided.
         * This is changes the semantic of MDBX_SET_LOWERBOUND but avoid
         * returning MDBX_BAD_VALSIZE. */
      } else if (mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED) {
        *data = save_data;
        csr = mdbx_cursor_set(&mc->mc_xcursor->mx_cursor, data, NULL,
                              MDBX_SET_RANGE);
        rc = csr.err;
        if (rc == MDBX_NOTFOUND) {
          mdbx_cassert(mc, !csr.exact);
          rc = mdbx_cursor_next(mc, key, data, MDBX_NEXT_NODUP);
        }
      } else {
        int cmp = mc->mc_dbx->md_dcmp(&save_data, data);
        csr.exact = (cmp == 0);
        if (cmp > 0)
          rc = mdbx_cursor_next(mc, key, data, MDBX_NEXT_NODUP);
      }
    }
    if (rc == MDBX_SUCCESS && !csr.exact)
      rc = MDBX_RESULT_TRUE;
    if (unlikely(op == MDBX_SET_UPPERBOUND)) {
      /* minor fixups for MDBX_SET_UPPERBOUND */
      if (rc == MDBX_RESULT_TRUE)
        /* already at great-than by MDBX_SET_LOWERBOUND */
        rc = MDBX_SUCCESS;
      else if (rc == MDBX_SUCCESS)
        /* exactly match, going next */
        rc = mdbx_cursor_next(mc, key, data, MDBX_NEXT);
    }
    break;
  }
  default:
    mdbx_debug("unhandled/unimplemented cursor operation %u", op);
    return MDBX_EINVAL;
  }

  mc->mc_flags &= ~C_DEL;
  return rc;
}

static int cursor_first_batch(MDBX_cursor *mc) {
  if (!(mc->mc_flags & C_INITIALIZED) || mc->mc_top) {
    int err = mdbx_page_search(mc, NULL, MDBX_PS_FIRST);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }
  mdbx_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]));

  mc->mc_flags |= C_INITIALIZED;
  mc->mc_flags &= ~C_EOF;
  mc->mc_ki[mc->mc_top] = 0;
  return MDBX_SUCCESS;
}

static int cursor_next_batch(MDBX_cursor *mc) {
  if (unlikely(!(mc->mc_flags & C_INITIALIZED)))
    return cursor_first_batch(mc);

  MDBX_page *mp = mc->mc_pg[mc->mc_top];
  if (unlikely(mc->mc_flags & C_EOF)) {
    if ((unsigned)mc->mc_ki[mc->mc_top] + 1 >= page_numkeys(mp))
      return MDBX_NOTFOUND;
    mc->mc_flags ^= C_EOF;
  }

  int ki = mc->mc_ki[mc->mc_top];
  mc->mc_ki[mc->mc_top] = (indx_t)++ki;
  const int numkeys = page_numkeys(mp);
  if (likely(ki >= numkeys)) {
    mdbx_debug("%s", "=====> move to next sibling page");
    mc->mc_ki[mc->mc_top] = (indx_t)(numkeys - 1);
    int err = mdbx_cursor_sibling(mc, SIBLING_RIGHT);
    if (unlikely(err != MDBX_SUCCESS)) {
      mc->mc_flags |= C_EOF;
      return err;
    }
    mp = mc->mc_pg[mc->mc_top];
    mdbx_debug("next page is %" PRIaPGNO ", key index %u", mp->mp_pgno,
               mc->mc_ki[mc->mc_top]);
  }
  return MDBX_SUCCESS;
}

int mdbx_cursor_get_batch(MDBX_cursor *mc, size_t *count, MDBX_val *pairs,
                          size_t limit, MDBX_cursor_op op) {
  if (unlikely(mc == NULL || count == NULL || limit < 4))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_signature != MDBX_MC_LIVE))
    return (mc->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                     : MDBX_EBADSIGN;

  int rc = check_txn(mc->mc_txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(mc->mc_db->md_flags & MDBX_DUPSORT))
    return MDBX_INCOMPATIBLE /* must be a non-dupsort subDB */;

  switch (op) {
  case MDBX_FIRST:
    rc = cursor_first_batch(mc);
    break;
  case MDBX_NEXT:
    rc = cursor_next_batch(mc);
    break;
  case MDBX_GET_CURRENT:
    rc = likely(mc->mc_flags & C_INITIALIZED) ? MDBX_SUCCESS : MDBX_ENODATA;
    break;
  default:
    mdbx_debug("unhandled/unimplemented cursor operation %u", op);
    rc = EINVAL;
    break;
  }

  if (unlikely(rc != MDBX_SUCCESS)) {
    *count = 0;
    return rc;
  }

  const MDBX_page *const page = mc->mc_pg[mc->mc_top];
  const unsigned nkeys = page_numkeys(page);
  unsigned i = mc->mc_ki[mc->mc_top], n = 0;
  if (unlikely(i >= nkeys)) {
    mdbx_cassert(mc, op == MDBX_GET_CURRENT);
    mdbx_cassert(mc, mdbx_cursor_on_last(mc) == MDBX_RESULT_TRUE);
    *count = 0;
    if (mc->mc_flags & C_EOF) {
      mdbx_cassert(mc, mdbx_cursor_on_last(mc) == MDBX_RESULT_TRUE);
      return MDBX_ENODATA;
    }
    if (mdbx_cursor_on_last(mc) != MDBX_RESULT_TRUE)
      return MDBX_EINVAL /* again MDBX_GET_CURRENT after MDBX_GET_CURRENT */;
    mc->mc_flags |= C_EOF;
    return MDBX_NOTFOUND;
  }

  const txnid_t pp_txnid = pp_txnid4chk(page, mc->mc_txn);
  do {
    if (unlikely(n + 2 > limit)) {
      rc = MDBX_RESULT_TRUE;
      break;
    }
    const MDBX_node *leaf = page_node(page, i);
    get_key(leaf, &pairs[n]);
    rc = mdbx_node_read(mc, leaf, &pairs[n + 1], pp_txnid);
    if (unlikely(rc != MDBX_SUCCESS))
      break;
    n += 2;
  } while (++i < nkeys);

  mc->mc_ki[mc->mc_top] = (indx_t)i;
  *count = n;
  return rc;
}

static int mdbx_touch_dbi(MDBX_cursor *mc) {
  mdbx_cassert(mc, (*mc->mc_dbistate & DBI_DIRTY) == 0);
  *mc->mc_dbistate |= DBI_DIRTY;
  mc->mc_txn->mt_flags |= MDBX_TXN_DIRTY;
  if (mc->mc_dbi >= CORE_DBS) {
    mdbx_cassert(mc, (mc->mc_flags & C_RECLAIMING) == 0);
    /* Touch DB record of named DB */
    MDBX_cursor_couple cx;
    int rc = mdbx_cursor_init(&cx.outer, mc->mc_txn, MAIN_DBI);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    mc->mc_txn->mt_dbistate[MAIN_DBI] |= DBI_DIRTY;
    rc = mdbx_page_search(&cx.outer, &mc->mc_dbx->md_name, MDBX_PS_MODIFY);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }
  return MDBX_SUCCESS;
}

/* Touch all the pages in the cursor stack. Set mc_top.
 * Makes sure all the pages are writable, before attempting a write operation.
 * [in] mc The cursor to operate on. */
static int mdbx_cursor_touch(MDBX_cursor *mc) {
  int rc = MDBX_SUCCESS;
  if (unlikely((*mc->mc_dbistate & DBI_DIRTY) == 0)) {
    rc = mdbx_touch_dbi(mc);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }
  if (likely(mc->mc_snum)) {
    mc->mc_top = 0;
    do {
      rc = mdbx_page_touch(mc);
    } while (!rc && ++(mc->mc_top) < mc->mc_snum);
    mc->mc_top = mc->mc_snum - 1;
  }
  return rc;
}

int mdbx_cursor_put(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data,
                    unsigned flags) {
  MDBX_env *env;
  MDBX_page *sub_root = NULL;
  MDBX_val xdata, *rdata, dkey, olddata;
  MDBX_db nested_dupdb;
  int err;
  DKBUF_DEBUG;

  if (unlikely(mc == NULL || key == NULL || data == NULL))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_signature != MDBX_MC_LIVE))
    return (mc->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                     : MDBX_EBADSIGN;

  int rc = check_txn_rw(mc->mc_txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(TXN_DBI_CHANGED(mc->mc_txn, mc->mc_dbi)))
    return MDBX_BAD_DBI;

  mdbx_cassert(mc, cursor_is_tracked(mc));
  env = mc->mc_txn->mt_env;

  /* Check this first so counter will always be zero on any early failures. */
  size_t mcount = 0, dcount = 0;
  if (unlikely(flags & MDBX_MULTIPLE)) {
    if (unlikely(flags & MDBX_RESERVE))
      return MDBX_EINVAL;
    if (unlikely(!F_ISSET(mc->mc_db->md_flags, MDBX_DUPFIXED)))
      return MDBX_INCOMPATIBLE;
    dcount = data[1].iov_len;
    if (unlikely(dcount < 2 || data->iov_len == 0))
      return MDBX_BAD_VALSIZE;
    if (unlikely(mc->mc_db->md_xsize != data->iov_len) && mc->mc_db->md_xsize)
      return MDBX_BAD_VALSIZE;
    if (unlikely(dcount > MAX_MAPSIZE / 2 /
                              (BRANCH_NODE_MAX(MAX_PAGESIZE) - NODESIZE))) {
      /* checking for multiplication overflow */
      if (unlikely(dcount > MAX_MAPSIZE / 2 / data->iov_len))
        return MDBX_TOO_LARGE;
    }
    data[1].iov_len = 0 /* reset done item counter */;
  }

  if (flags & MDBX_RESERVE) {
    if (unlikely(mc->mc_db->md_flags & (MDBX_DUPSORT | MDBX_REVERSEDUP |
                                        MDBX_INTEGERDUP | MDBX_DUPFIXED)))
      return MDBX_INCOMPATIBLE;
    data->iov_base = nullptr;
  }

  const unsigned nospill = flags & MDBX_NOSPILL;
  flags -= nospill;

  if (unlikely(mc->mc_txn->mt_flags & (MDBX_TXN_RDONLY | MDBX_TXN_BLOCKED)))
    return (mc->mc_txn->mt_flags & MDBX_TXN_RDONLY) ? MDBX_EACCESS
                                                    : MDBX_BAD_TXN;

  uint64_t aligned_keybytes, aligned_databytes;
  MDBX_val aligned_key, aligned_data;
  if (likely((mc->mc_flags & C_SUB) == 0)) {
    if (unlikely(key->iov_len < mc->mc_dbx->md_klen_min ||
                 key->iov_len > mc->mc_dbx->md_klen_max)) {
      mdbx_cassert(mc, !"Invalid key-size");
      return MDBX_BAD_VALSIZE;
    }
    if (unlikely(data->iov_len < mc->mc_dbx->md_vlen_min ||
                 data->iov_len > mc->mc_dbx->md_vlen_max)) {
      mdbx_cassert(mc, !"Invalid data-size");
      return MDBX_BAD_VALSIZE;
    }

    if (mc->mc_db->md_flags & MDBX_INTEGERKEY) {
      switch (key->iov_len) {
      default:
        mdbx_cassert(mc, !"key-size is invalid for MDBX_INTEGERKEY");
        return MDBX_BAD_VALSIZE;
      case 4:
        if (unlikely(3 & (uintptr_t)key->iov_base)) {
          /* copy instead of return error to avoid break compatibility */
          aligned_key.iov_base =
              memcpy(&aligned_keybytes, key->iov_base, aligned_key.iov_len = 4);
          key = &aligned_key;
        }
        break;
      case 8:
        if (unlikely(7 & (uintptr_t)key->iov_base)) {
          /* copy instead of return error to avoid break compatibility */
          aligned_key.iov_base =
              memcpy(&aligned_keybytes, key->iov_base, aligned_key.iov_len = 8);
          key = &aligned_key;
        }
        break;
      }
    }
    if (mc->mc_db->md_flags & MDBX_INTEGERDUP) {
      switch (data->iov_len) {
      default:
        mdbx_cassert(mc, !"data-size is invalid for MDBX_INTEGERKEY");
        return MDBX_BAD_VALSIZE;
      case 4:
        if (unlikely(3 & (uintptr_t)data->iov_base)) {
          if (unlikely(flags & MDBX_MULTIPLE))
            return MDBX_BAD_VALSIZE;
          /* copy instead of return error to avoid break compatibility */
          aligned_data.iov_base = memcpy(&aligned_databytes, data->iov_base,
                                         aligned_data.iov_len = 4);
          data = &aligned_data;
        }
        break;
      case 8:
        if (unlikely(7 & (uintptr_t)data->iov_base)) {
          if (unlikely(flags & MDBX_MULTIPLE))
            return MDBX_BAD_VALSIZE;
          /* copy instead of return error to avoid break compatibility */
          aligned_data.iov_base = memcpy(&aligned_databytes, data->iov_base,
                                         aligned_data.iov_len = 8);
          data = &aligned_data;
        }
        break;
      }
    }
  }

  mdbx_debug(
      "==> put db %d key [%s], size %" PRIuPTR ", data [%s] size %" PRIuPTR,
      DDBI(mc), DKEY_DEBUG(key), key->iov_len,
      DVAL_DEBUG((flags & MDBX_RESERVE) ? nullptr : data), data->iov_len);

  int dupdata_flag = 0;
  if ((flags & MDBX_CURRENT) != 0 && (mc->mc_flags & C_SUB) == 0) {
    if (unlikely(flags & (MDBX_APPEND | MDBX_NOOVERWRITE)))
      return MDBX_EINVAL;
    /* Опция MDBX_CURRENT означает, что запрошено обновление текущей записи,
     * на которой сейчас стоит курсор. Проверяем что переданный ключ совпадает
     * со значением в текущей позиции курсора.
     * Здесь проще вызвать mdbx_cursor_get(), так как для обслуживания таблиц
     * с MDBX_DUPSORT также требуется текущий размер данных. */
    MDBX_val current_key, current_data;
    rc = mdbx_cursor_get(mc, &current_key, &current_data, MDBX_GET_CURRENT);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    if (mc->mc_dbx->md_cmp(key, &current_key) != 0)
      return MDBX_EKEYMISMATCH;

    if (unlikely((flags & MDBX_MULTIPLE)))
      goto drop_current;

    if (F_ISSET(mc->mc_db->md_flags, MDBX_DUPSORT)) {
      MDBX_node *node = page_node(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
      if (F_ISSET(node_flags(node), F_DUPDATA)) {
        mdbx_cassert(mc,
                     mc->mc_xcursor != NULL &&
                         (mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED));
        /* Если за ключом более одного значения, либо если размер данных
         * отличается, то вместо обновления требуется удаление и
         * последующая вставка. */
        if (mc->mc_xcursor->mx_db.md_entries > 1 ||
            current_data.iov_len != data->iov_len) {
        drop_current:
          rc = mdbx_cursor_del(mc, flags & MDBX_ALLDUPS);
          if (unlikely(rc != MDBX_SUCCESS))
            return rc;
          flags -= MDBX_CURRENT;
          goto skip_check_samedata;
        }
      } else if (unlikely(node_size(key, data) > env->me_leaf_nodemax)) {
        rc = mdbx_cursor_del(mc, 0);
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;
        flags -= MDBX_CURRENT;
        goto skip_check_samedata;
      }
    }
    if (!(flags & MDBX_RESERVE) &&
        unlikely(cmp_lenfast(&current_data, data) == 0))
      return MDBX_SUCCESS /* the same data, nothing to update */;
  skip_check_samedata:;
  }

  if (mc->mc_db->md_root == P_INVALID) {
    /* new database, cursor has nothing to point to */
    mc->mc_snum = 0;
    mc->mc_top = 0;
    mc->mc_flags &= ~C_INITIALIZED;
    rc = MDBX_NO_ROOT;
  } else if ((flags & MDBX_CURRENT) == 0) {
    bool exact = false;
    if ((flags & MDBX_APPEND) && mc->mc_db->md_entries > 0) {
      rc = mdbx_cursor_last(mc, &dkey, &olddata);
      if (likely(rc == MDBX_SUCCESS)) {
        rc = mc->mc_dbx->md_cmp(key, &dkey);
        if (likely(rc > 0)) {
          mc->mc_ki[mc->mc_top]++; /* step forward for appending */
          rc = MDBX_NOTFOUND;
        } else {
          if (unlikely(rc != MDBX_SUCCESS || !(flags & MDBX_APPENDDUP)))
            /* new-key < last-key
             * or new-key == last-key without MDBX_APPENDDUP */
            return MDBX_EKEYMISMATCH;
          exact = true;
        }
      }
    } else {
      struct cursor_set_result csr =
          /* olddata may not be updated in case LEAF2-page of dupfixed-subDB */
          mdbx_cursor_set(mc, (MDBX_val *)key, &olddata, MDBX_SET);
      rc = csr.err;
      exact = csr.exact;
    }
    if (likely(rc == MDBX_SUCCESS)) {
      if (exact) {
        if (unlikely(flags & MDBX_NOOVERWRITE)) {
          mdbx_debug("duplicate key [%s]", DKEY_DEBUG(key));
          *data = olddata;
          return MDBX_KEYEXIST;
        }
        if (unlikely(mc->mc_flags & C_SUB)) {
          /* nested subtree of DUPSORT-database with the same key,
           * nothing to update */
          mdbx_assert(env, data->iov_len == 0 &&
                               (olddata.iov_len == 0 ||
                                /* olddata may not be updated in case LEAF2-page
                                   of dupfixed-subDB */
                                (mc->mc_db->md_flags & MDBX_DUPFIXED)));
          return MDBX_SUCCESS;
        }
        if (unlikely(flags & MDBX_ALLDUPS) && mc->mc_xcursor &&
            (mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED)) {
          rc = mdbx_cursor_del(mc, MDBX_ALLDUPS);
          if (unlikely(rc != MDBX_SUCCESS))
            return rc;
          flags -= MDBX_ALLDUPS;
          rc = MDBX_NOTFOUND;
          exact = false;
        } else /* checking for early exit without dirtying pages */
          if (!(flags & (MDBX_RESERVE | MDBX_MULTIPLE)) &&
              unlikely(mc->mc_dbx->md_dcmp(data, &olddata) == 0)) {
            if (!mc->mc_xcursor)
              /* the same data, nothing to update */
              return MDBX_SUCCESS;
            if (flags & MDBX_NODUPDATA)
              return MDBX_KEYEXIST;
            if (flags & MDBX_APPENDDUP)
              return MDBX_EKEYMISMATCH;
            if (likely(unsure_equal(mc->mc_dbx->md_dcmp, data, &olddata)))
              /* data is match exactly byte-to-byte, nothing to update */
              return MDBX_SUCCESS;
            else {
              /* The data has differences, but the user-provided comparator
               * considers them equal. So continue update since called without.
               * Continue to update since was called without MDBX_NODUPDATA. */
            }
          }
      }
    } else if (unlikely(rc != MDBX_NOTFOUND))
      return rc;
  }

  mc->mc_flags &= ~C_DEL;

  /* Cursor is positioned, check for room in the dirty list */
  if (!nospill) {
    rdata = data;
    if (unlikely(flags & MDBX_MULTIPLE)) {
      rdata = &xdata;
      xdata.iov_len = data->iov_len * dcount;
    }
    if (unlikely(err = mdbx_cursor_spill(mc, key, rdata)))
      return err;
  }

  if (unlikely(rc == MDBX_NO_ROOT)) {
    /* new database, write a root leaf page */
    mdbx_debug("%s", "allocating new root leaf page");
    if (unlikely((*mc->mc_dbistate & DBI_DIRTY) == 0)) {
      err = mdbx_touch_dbi(mc);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
    struct page_result npr = mdbx_page_new(mc, P_LEAF, 1);
    if (unlikely(npr.err != MDBX_SUCCESS))
      return npr.err;
    npr.err = mdbx_cursor_push(mc, npr.page);
    if (unlikely(npr.err != MDBX_SUCCESS))
      return npr.err;
    mc->mc_db->md_root = npr.page->mp_pgno;
    mc->mc_db->md_depth++;
    if (mc->mc_db->md_flags & MDBX_INTEGERKEY) {
      assert(key->iov_len >= mc->mc_dbx->md_klen_min &&
             key->iov_len <= mc->mc_dbx->md_klen_max);
      mc->mc_dbx->md_klen_min = mc->mc_dbx->md_klen_max = key->iov_len;
    }
    if (mc->mc_db->md_flags & (MDBX_INTEGERDUP | MDBX_DUPFIXED)) {
      assert(data->iov_len >= mc->mc_dbx->md_vlen_min &&
             data->iov_len <= mc->mc_dbx->md_vlen_max);
      assert(mc->mc_xcursor != NULL);
      mc->mc_db->md_xsize = mc->mc_xcursor->mx_db.md_xsize =
          (unsigned)(mc->mc_dbx->md_vlen_min = mc->mc_dbx->md_vlen_max =
                         mc->mc_xcursor->mx_dbx.md_klen_min =
                             mc->mc_xcursor->mx_dbx.md_klen_max =
                                 data->iov_len);
    }
    if ((mc->mc_db->md_flags & (MDBX_DUPSORT | MDBX_DUPFIXED)) == MDBX_DUPFIXED)
      npr.page->mp_flags |= P_LEAF2;
    mc->mc_flags |= C_INITIALIZED;
  } else {
    /* make sure all cursor pages are writable */
    err = mdbx_cursor_touch(mc);
    if (unlikely(err))
      return err;
  }

  bool insert_key, insert_data, do_sub = false;
  insert_key = insert_data = (rc != MDBX_SUCCESS);
  uint16_t fp_flags = P_LEAF;
  MDBX_page *fp = env->me_pbuf;
  fp->mp_txnid = mc->mc_txn->mt_front;
  if (insert_key) {
    /* The key does not exist */
    mdbx_debug("inserting key at index %i", mc->mc_ki[mc->mc_top]);
    if ((mc->mc_db->md_flags & MDBX_DUPSORT) &&
        node_size(key, data) > env->me_leaf_nodemax) {
      /* Too big for a node, insert in sub-DB.  Set up an empty
       * "old sub-page" for prep_subDB to expand to a full page. */
      fp->mp_leaf2_ksize =
          (mc->mc_db->md_flags & MDBX_DUPFIXED) ? (uint16_t)data->iov_len : 0;
      fp->mp_lower = fp->mp_upper = 0;
      olddata.iov_len = PAGEHDRSZ;
      goto prep_subDB;
    }
  } else {
    /* there's only a key anyway, so this is a no-op */
    if (IS_LEAF2(mc->mc_pg[mc->mc_top])) {
      char *ptr;
      unsigned ksize = mc->mc_db->md_xsize;
      if (unlikely(key->iov_len != ksize))
        return MDBX_BAD_VALSIZE;
      ptr = page_leaf2key(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top], ksize);
      memcpy(ptr, key->iov_base, ksize);
    fix_parent:
      /* if overwriting slot 0 of leaf, need to
       * update branch key if there is a parent page */
      if (mc->mc_top && !mc->mc_ki[mc->mc_top]) {
        unsigned dtop = 1;
        mc->mc_top--;
        /* slot 0 is always an empty key, find real slot */
        while (mc->mc_top && !mc->mc_ki[mc->mc_top]) {
          mc->mc_top--;
          dtop++;
        }
        err = MDBX_SUCCESS;
        if (mc->mc_ki[mc->mc_top])
          err = mdbx_update_key(mc, key);
        mdbx_cassert(mc, mc->mc_top + dtop < UINT16_MAX);
        mc->mc_top += (uint16_t)dtop;
        if (unlikely(err != MDBX_SUCCESS))
          return err;
      }

      if (mdbx_audit_enabled()) {
        err = mdbx_cursor_check(mc, 0);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
      }
      return MDBX_SUCCESS;
    }

  more:;
    if (mdbx_audit_enabled()) {
      err = mdbx_cursor_check(mc, 0);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
    MDBX_node *node = page_node(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);

    /* Large/Overflow page overwrites need special handling */
    if (unlikely(F_ISSET(node_flags(node), F_BIGDATA))) {
      int dpages = (node_size(key, data) > env->me_leaf_nodemax)
                       ? number_of_ovpages(env, data->iov_len)
                       : 0;

      const pgno_t pgno = node_largedata_pgno(node);
      struct page_result pgr = mdbx_page_get_ex(
          mc, pgno, pp_txnid4chk(mc->mc_pg[mc->mc_top], mc->mc_txn));
      if (unlikely(pgr.err != MDBX_SUCCESS))
        return pgr.err;
      if (unlikely(!IS_OVERFLOW(pgr.page)))
        return MDBX_CORRUPTED;

      /* Is the ov page from this txn (or a parent) and big enough? */
      int ovpages = pgr.page->mp_pages;
      if (!IS_FROZEN(mc->mc_txn, pgr.page) &&
          (unlikely(mc->mc_flags & C_GCFREEZE)
               ? (ovpages >= dpages)
               : (ovpages ==
                  /* LY: add configurable threshold to keep reserve space */
                  dpages))) {
        /* yes, overwrite it. */
        if (!IS_MODIFIABLE(mc->mc_txn, pgr.page)) {
          if (IS_SPILLED(mc->mc_txn, pgr.page)) {
            pgr = /* TODO: avoid search and get txn & spill-index from
                     page_result */
                mdbx_page_unspill(mc->mc_txn, pgr.page);
            if (unlikely(pgr.err))
              return pgr.err;
          } else {
            if (unlikely(!mc->mc_txn->mt_parent)) {
              mdbx_error(
                  "Unexpected not frozen/modifiable/spilled but shadowed %s "
                  "page %" PRIaPGNO " mod-txnid %" PRIaTXN ","
                  " without parent transaction, current txn %" PRIaTXN
                  " front %" PRIaTXN,
                  "overflow/large", pgno, pgr.page->mp_txnid,
                  mc->mc_txn->mt_txnid, mc->mc_txn->mt_front);
              return MDBX_PROBLEM;
            }

            /* It is writable only in a parent txn */
            MDBX_page *np = mdbx_page_malloc(mc->mc_txn, ovpages);
            if (unlikely(!np))
              return MDBX_ENOMEM;

            memcpy(np, pgr.page, PAGEHDRSZ); /* Copy header of page */
            err = mdbx_page_dirty(mc->mc_txn, pgr.page = np, ovpages);
            if (unlikely(err != MDBX_SUCCESS))
              return err;

#if MDBX_ENABLE_PGOP_STAT
            mc->mc_txn->mt_env->me_lck->mti_pgop_stat.clone.weak += ovpages;
#endif /* MDBX_ENABLE_PGOP_STAT */
            mdbx_cassert(mc, mdbx_dirtylist_check(mc->mc_txn));
          }
        }
        node_set_ds(node, data->iov_len);
        if (F_ISSET(flags, MDBX_RESERVE))
          data->iov_base = page_data(pgr.page);
        else
          memcpy(page_data(pgr.page), data->iov_base, data->iov_len);

        if (mdbx_audit_enabled()) {
          err = mdbx_cursor_check(mc, 0);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
        }
        return MDBX_SUCCESS;
      }

      if ((err = mdbx_page_retire(mc, pgr.page)) != MDBX_SUCCESS)
        return err;
    } else {
      olddata.iov_len = node_ds(node);
      olddata.iov_base = node_data(node);
      mdbx_cassert(mc, (char *)olddata.iov_base + olddata.iov_len <=
                           (char *)(mc->mc_pg[mc->mc_top]) + env->me_psize);

      /* DB has dups? */
      if (F_ISSET(mc->mc_db->md_flags, MDBX_DUPSORT)) {
        /* Prepare (sub-)page/sub-DB to accept the new item, if needed.
         * fp: old sub-page or a header faking it.
         * mp: new (sub-)page.  offset: growth in page size.
         * xdata: node data with new page or DB. */
        unsigned i;
        size_t offset = 0;
        MDBX_page *mp = fp = xdata.iov_base = env->me_pbuf;
        mp->mp_pgno = mc->mc_pg[mc->mc_top]->mp_pgno;

        /* Was a single item before, must convert now */
        if (!F_ISSET(node_flags(node), F_DUPDATA)) {

          /* does data match? */
          const int cmp = mc->mc_dbx->md_dcmp(data, &olddata);
          if ((flags & MDBX_APPENDDUP) && unlikely(cmp <= 0))
            return MDBX_EKEYMISMATCH;
          if (cmp == 0) {
            if (flags & MDBX_NODUPDATA)
              return MDBX_KEYEXIST;
            if (likely(unsure_equal(mc->mc_dbx->md_dcmp, data, &olddata))) {
              /* data is match exactly byte-to-byte, nothing to update */
              if (unlikely(flags & MDBX_MULTIPLE)) {
                rc = MDBX_SUCCESS;
                goto continue_multiple;
              }
              return MDBX_SUCCESS;
            } else {
              /* The data has differences, but the user-provided comparator
               * considers them equal. So continue update since called without.
               * Continue to update since was called without MDBX_NODUPDATA. */
            }
            mdbx_cassert(mc, node_size(key, data) <= env->me_leaf_nodemax);
            goto current;
          }

          /* Just overwrite the current item */
          if (flags & MDBX_CURRENT) {
            mdbx_cassert(mc, node_size(key, data) <= env->me_leaf_nodemax);
            goto current;
          }

          /* Back up original data item */
          memcpy(dkey.iov_base = fp + 1, olddata.iov_base,
                 dkey.iov_len = olddata.iov_len);
          dupdata_flag = 1;

          /* Make sub-page header for the dup items, with dummy body */
          fp->mp_flags = P_LEAF | P_SUBP;
          fp->mp_lower = 0;
          xdata.iov_len = PAGEHDRSZ + dkey.iov_len + data->iov_len;
          if (mc->mc_db->md_flags & MDBX_DUPFIXED) {
            fp->mp_flags |= P_LEAF2;
            fp->mp_leaf2_ksize = (uint16_t)data->iov_len;
            xdata.iov_len += 2 * data->iov_len; /* leave space for 2 more */
            mdbx_cassert(mc, xdata.iov_len <= env->me_psize);
          } else {
            xdata.iov_len += 2 * (sizeof(indx_t) + NODESIZE) +
                             (dkey.iov_len & 1) + (data->iov_len & 1);
            mdbx_cassert(mc, xdata.iov_len <= env->me_psize);
          }
          fp->mp_upper = (uint16_t)(xdata.iov_len - PAGEHDRSZ);
          olddata.iov_len = xdata.iov_len; /* pretend olddata is fp */
        } else if (node_flags(node) & F_SUBDATA) {
          /* Data is on sub-DB, just store it */
          flags |= F_DUPDATA | F_SUBDATA;
          goto put_sub;
        } else {
          /* Data is on sub-page */
          fp = olddata.iov_base;
          switch (flags) {
          default:
            if (!(mc->mc_db->md_flags & MDBX_DUPFIXED)) {
              offset = node_size(data, nullptr) + sizeof(indx_t);
              break;
            }
            offset = fp->mp_leaf2_ksize;
            if (page_room(fp) < offset) {
              offset *= 4; /* space for 4 more */
              break;
            }
            /* FALLTHRU: Big enough MDBX_DUPFIXED sub-page */
            __fallthrough;
          case MDBX_CURRENT | MDBX_NODUPDATA:
          case MDBX_CURRENT:
            fp->mp_txnid = mc->mc_txn->mt_front;
            fp->mp_pgno = mp->mp_pgno;
            mc->mc_xcursor->mx_cursor.mc_pg[0] = fp;
            flags |= F_DUPDATA;
            goto put_sub;
          }
          xdata.iov_len = olddata.iov_len + offset;
        }

        fp_flags = fp->mp_flags;
        if (node_size_len(node_ks(node), xdata.iov_len) >
            env->me_leaf_nodemax) {
          /* Too big for a sub-page, convert to sub-DB */
          fp_flags &= ~P_SUBP;
        prep_subDB:
          nested_dupdb.md_xsize = 0;
          nested_dupdb.md_flags = flags_db2sub(mc->mc_db->md_flags);
          if (mc->mc_db->md_flags & MDBX_DUPFIXED) {
            fp_flags |= P_LEAF2;
            nested_dupdb.md_xsize = fp->mp_leaf2_ksize;
          }
          nested_dupdb.md_depth = 1;
          nested_dupdb.md_branch_pages = 0;
          nested_dupdb.md_leaf_pages = 1;
          nested_dupdb.md_overflow_pages = 0;
          nested_dupdb.md_entries = page_numkeys(fp);
          xdata.iov_len = sizeof(nested_dupdb);
          xdata.iov_base = &nested_dupdb;
          const struct page_result par = mdbx_page_alloc(mc, 1, MDBX_ALLOC_ALL);
          mp = par.page;
          if (unlikely(par.err != MDBX_SUCCESS))
            return par.err;
          mc->mc_db->md_leaf_pages += 1;
          mdbx_cassert(mc, env->me_psize > olddata.iov_len);
          offset = env->me_psize - (unsigned)olddata.iov_len;
          flags |= F_DUPDATA | F_SUBDATA;
          nested_dupdb.md_root = mp->mp_pgno;
          nested_dupdb.md_seq = 0;
          nested_dupdb.md_mod_txnid = mc->mc_txn->mt_txnid;
          sub_root = mp;
        }
        if (mp != fp) {
          mp->mp_flags = fp_flags;
          mp->mp_txnid = mc->mc_txn->mt_front;
          mp->mp_leaf2_ksize = fp->mp_leaf2_ksize;
          mp->mp_lower = fp->mp_lower;
          mdbx_cassert(mc, fp->mp_upper + offset <= UINT16_MAX);
          mp->mp_upper = (indx_t)(fp->mp_upper + offset);
          if (unlikely(fp_flags & P_LEAF2)) {
            memcpy(page_data(mp), page_data(fp),
                   page_numkeys(fp) * fp->mp_leaf2_ksize);
          } else {
            memcpy((char *)mp + mp->mp_upper + PAGEHDRSZ,
                   (char *)fp + fp->mp_upper + PAGEHDRSZ,
                   olddata.iov_len - fp->mp_upper - PAGEHDRSZ);
            memcpy((char *)(&mp->mp_ptrs), (char *)(&fp->mp_ptrs),
                   page_numkeys(fp) * sizeof(mp->mp_ptrs[0]));
            for (i = 0; i < page_numkeys(fp); i++) {
              mdbx_cassert(mc, mp->mp_ptrs[i] + offset <= UINT16_MAX);
              mp->mp_ptrs[i] += (indx_t)offset;
            }
          }
        }

        rdata = &xdata;
        flags |= F_DUPDATA;
        do_sub = true;
        if (!insert_key)
          mdbx_node_del(mc, 0);
        goto new_sub;
      }

      /* MDBX passes F_SUBDATA in 'flags' to write a DB record */
      if (unlikely((node_flags(node) ^ flags) & F_SUBDATA))
        return MDBX_INCOMPATIBLE;

    current:
      if (data->iov_len == olddata.iov_len) {
        mdbx_cassert(mc, EVEN(key->iov_len) == EVEN(node_ks(node)));
        /* same size, just replace it. Note that we could
         * also reuse this node if the new data is smaller,
         * but instead we opt to shrink the node in that case. */
        if (F_ISSET(flags, MDBX_RESERVE))
          data->iov_base = olddata.iov_base;
        else if (!(mc->mc_flags & C_SUB))
          memcpy(olddata.iov_base, data->iov_base, data->iov_len);
        else {
          mdbx_cassert(mc, page_numkeys(mc->mc_pg[mc->mc_top]) == 1);
          mdbx_cassert(mc, PAGETYPE(mc->mc_pg[mc->mc_top]) == P_LEAF);
          mdbx_cassert(mc, node_ds(node) == 0);
          mdbx_cassert(mc, node_flags(node) == 0);
          mdbx_cassert(mc, key->iov_len < UINT16_MAX);
          node_set_ks(node, key->iov_len);
          memcpy(node_key(node), key->iov_base, key->iov_len);
          mdbx_cassert(mc, (char *)node_key(node) + node_ds(node) <
                               (char *)(mc->mc_pg[mc->mc_top]) + env->me_psize);
          goto fix_parent;
        }

        if (mdbx_audit_enabled()) {
          err = mdbx_cursor_check(mc, 0);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
        }
        return MDBX_SUCCESS;
      }
    }
    mdbx_node_del(mc, 0);
  }

  rdata = data;

new_sub:;
  unsigned nflags = flags & NODE_ADD_FLAGS;
  size_t nsize = IS_LEAF2(mc->mc_pg[mc->mc_top]) ? key->iov_len
                                                 : leaf_size(env, key, rdata);
  if (page_room(mc->mc_pg[mc->mc_top]) < nsize) {
    if (!insert_key)
      nflags |= MDBX_SPLIT_REPLACE;
    rc = mdbx_page_split(mc, key, rdata, P_INVALID, nflags);
    if (rc == MDBX_SUCCESS && mdbx_audit_enabled())
      rc = mdbx_cursor_check(mc, 0);
  } else {
    /* There is room already in this leaf page. */
    if (IS_LEAF2(mc->mc_pg[mc->mc_top])) {
      mdbx_cassert(mc, (nflags & (F_BIGDATA | F_SUBDATA | F_DUPDATA)) == 0 &&
                           rdata->iov_len == 0);
      rc = mdbx_node_add_leaf2(mc, mc->mc_ki[mc->mc_top], key);
    } else
      rc = mdbx_node_add_leaf(mc, mc->mc_ki[mc->mc_top], key, rdata, nflags);
    if (likely(rc == 0)) {
      /* Adjust other cursors pointing to mp */
      const MDBX_dbi dbi = mc->mc_dbi;
      const unsigned i = mc->mc_top;
      MDBX_page *const mp = mc->mc_pg[i];
      for (MDBX_cursor *m2 = mc->mc_txn->tw.cursors[dbi]; m2;
           m2 = m2->mc_next) {
        MDBX_cursor *m3 =
            (mc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
        if (m3 == mc || m3->mc_snum < mc->mc_snum || m3->mc_pg[i] != mp)
          continue;
        if (m3->mc_ki[i] >= mc->mc_ki[i])
          m3->mc_ki[i] += insert_key;
        if (XCURSOR_INITED(m3))
          XCURSOR_REFRESH(m3, mp, m3->mc_ki[i]);
      }
    }
  }

  if (likely(rc == MDBX_SUCCESS)) {
    /* Now store the actual data in the child DB. Note that we're
     * storing the user data in the keys field, so there are strict
     * size limits on dupdata. The actual data fields of the child
     * DB are all zero size. */
    if (do_sub) {
      int xflags;
      size_t ecount;
    put_sub:
      xdata.iov_len = 0;
      xdata.iov_base = nullptr;
      MDBX_node *node = page_node(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
#define SHIFT_MDBX_NODUPDATA_TO_MDBX_NOOVERWRITE 1
      STATIC_ASSERT(
          (MDBX_NODUPDATA >> SHIFT_MDBX_NODUPDATA_TO_MDBX_NOOVERWRITE) ==
          MDBX_NOOVERWRITE);
      xflags = MDBX_CURRENT | MDBX_NOSPILL |
               ((flags & MDBX_NODUPDATA) >>
                SHIFT_MDBX_NODUPDATA_TO_MDBX_NOOVERWRITE);
      if ((flags & MDBX_CURRENT) == 0) {
        xflags -= MDBX_CURRENT;
        err = mdbx_xcursor_init1(mc, node, mc->mc_pg[mc->mc_top]);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
      }
      if (sub_root)
        mc->mc_xcursor->mx_cursor.mc_pg[0] = sub_root;
      /* converted, write the original data first */
      if (dupdata_flag) {
        rc = mdbx_cursor_put(&mc->mc_xcursor->mx_cursor, &dkey, &xdata, xflags);
        if (unlikely(rc))
          goto bad_sub;
        /* we've done our job */
        dkey.iov_len = 0;
      }
      if (!(node_flags(node) & F_SUBDATA) || sub_root) {
        /* Adjust other cursors pointing to mp */
        MDBX_cursor *m2;
        MDBX_xcursor *mx = mc->mc_xcursor;
        unsigned i = mc->mc_top;
        MDBX_page *mp = mc->mc_pg[i];
        const int nkeys = page_numkeys(mp);

        for (m2 = mc->mc_txn->tw.cursors[mc->mc_dbi]; m2; m2 = m2->mc_next) {
          if (m2 == mc || m2->mc_snum < mc->mc_snum)
            continue;
          if (!(m2->mc_flags & C_INITIALIZED))
            continue;
          if (m2->mc_pg[i] == mp) {
            if (m2->mc_ki[i] == mc->mc_ki[i]) {
              err = mdbx_xcursor_init2(m2, mx, dupdata_flag);
              if (unlikely(err != MDBX_SUCCESS))
                return err;
            } else if (!insert_key && m2->mc_ki[i] < nkeys) {
              XCURSOR_REFRESH(m2, mp, m2->mc_ki[i]);
            }
          }
        }
      }
      mdbx_cassert(mc, mc->mc_xcursor->mx_db.md_entries < PTRDIFF_MAX);
      ecount = (size_t)mc->mc_xcursor->mx_db.md_entries;
#define SHIFT_MDBX_APPENDDUP_TO_MDBX_APPEND 1
      STATIC_ASSERT((MDBX_APPENDDUP >> SHIFT_MDBX_APPENDDUP_TO_MDBX_APPEND) ==
                    MDBX_APPEND);
      xflags |= (flags & MDBX_APPENDDUP) >> SHIFT_MDBX_APPENDDUP_TO_MDBX_APPEND;
      rc = mdbx_cursor_put(&mc->mc_xcursor->mx_cursor, data, &xdata, xflags);
      if (flags & F_SUBDATA) {
        void *db = node_data(node);
        mc->mc_xcursor->mx_db.md_mod_txnid = mc->mc_txn->mt_txnid;
        memcpy(db, &mc->mc_xcursor->mx_db, sizeof(MDBX_db));
      }
      insert_data = (ecount != (size_t)mc->mc_xcursor->mx_db.md_entries);
    }
    /* Increment count unless we just replaced an existing item. */
    if (insert_data)
      mc->mc_db->md_entries++;
    if (insert_key) {
      /* Invalidate txn if we created an empty sub-DB */
      if (unlikely(rc))
        goto bad_sub;
      /* If we succeeded and the key didn't exist before,
       * make sure the cursor is marked valid. */
      mc->mc_flags |= C_INITIALIZED;
    }
    if (unlikely(flags & MDBX_MULTIPLE)) {
      if (likely(rc == MDBX_SUCCESS)) {
      continue_multiple:
        mcount++;
        /* let caller know how many succeeded, if any */
        data[1].iov_len = mcount;
        if (mcount < dcount) {
          data[0].iov_base = (char *)data[0].iov_base + data[0].iov_len;
          insert_key = insert_data = false;
          goto more;
        }
      }
    }
    if (rc == MDBX_SUCCESS && mdbx_audit_enabled())
      rc = mdbx_cursor_check(mc, 0);
    return rc;
  bad_sub:
    if (unlikely(rc == MDBX_KEYEXIST)) {
      /* should not happen, we deleted that item */
      mdbx_error("Unexpected %i error while put to nested dupsort's hive", rc);
      rc = MDBX_PROBLEM;
    }
  }
  mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
  return rc;
}

int mdbx_cursor_del(MDBX_cursor *mc, MDBX_put_flags_t flags) {
  if (unlikely(!mc))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_signature != MDBX_MC_LIVE))
    return (mc->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                     : MDBX_EBADSIGN;

  int rc = check_txn_rw(mc->mc_txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(TXN_DBI_CHANGED(mc->mc_txn, mc->mc_dbi)))
    return MDBX_BAD_DBI;

  if (unlikely(!(mc->mc_flags & C_INITIALIZED)))
    return MDBX_ENODATA;

  if (unlikely(mc->mc_ki[mc->mc_top] >= page_numkeys(mc->mc_pg[mc->mc_top])))
    return MDBX_NOTFOUND;

  if (likely((flags & MDBX_NOSPILL) == 0) &&
      unlikely(rc = mdbx_cursor_spill(mc, NULL, NULL)))
    return rc;

  rc = mdbx_cursor_touch(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  MDBX_page *mp = mc->mc_pg[mc->mc_top];
  if (!MDBX_DISABLE_PAGECHECKS && unlikely(!IS_LEAF(mp)))
    return MDBX_CORRUPTED;
  if (IS_LEAF2(mp)) {
    if (!MDBX_DISABLE_PAGECHECKS && unlikely((mc->mc_flags & C_SUB) == 0)) {
      mdbx_error("unexpected LEAF2-page %" PRIaPGNO "for non-dupsort cursor",
                 mp->mp_pgno);
      return MDBX_CORRUPTED;
    }
    goto del_key;
  }

  MDBX_node *node = page_node(mp, mc->mc_ki[mc->mc_top]);
  if (F_ISSET(node_flags(node), F_DUPDATA)) {
    if (flags & (MDBX_ALLDUPS | /* for compatibility */ MDBX_NODUPDATA)) {
      /* mdbx_cursor_del0() will subtract the final entry */
      mc->mc_db->md_entries -= mc->mc_xcursor->mx_db.md_entries - 1;
      mc->mc_xcursor->mx_cursor.mc_flags &= ~C_INITIALIZED;
    } else {
      if (!F_ISSET(node_flags(node), F_SUBDATA))
        mc->mc_xcursor->mx_cursor.mc_pg[0] = node_data(node);
      rc = mdbx_cursor_del(&mc->mc_xcursor->mx_cursor, MDBX_NOSPILL);
      if (unlikely(rc))
        return rc;
      /* If sub-DB still has entries, we're done */
      if (mc->mc_xcursor->mx_db.md_entries) {
        if (node_flags(node) & F_SUBDATA) {
          /* update subDB info */
          void *db = node_data(node);
          mc->mc_xcursor->mx_db.md_mod_txnid = mc->mc_txn->mt_txnid;
          memcpy(db, &mc->mc_xcursor->mx_db, sizeof(MDBX_db));
        } else {
          MDBX_cursor *m2;
          /* shrink fake page */
          mdbx_node_shrink(mp, mc->mc_ki[mc->mc_top]);
          node = page_node(mp, mc->mc_ki[mc->mc_top]);
          mc->mc_xcursor->mx_cursor.mc_pg[0] = node_data(node);
          /* fix other sub-DB cursors pointed at fake pages on this page */
          for (m2 = mc->mc_txn->tw.cursors[mc->mc_dbi]; m2; m2 = m2->mc_next) {
            if (m2 == mc || m2->mc_snum < mc->mc_snum)
              continue;
            if (!(m2->mc_flags & C_INITIALIZED))
              continue;
            if (m2->mc_pg[mc->mc_top] == mp) {
              MDBX_node *inner = node;
              if (m2->mc_ki[mc->mc_top] >= page_numkeys(mp))
                continue;
              if (m2->mc_ki[mc->mc_top] != mc->mc_ki[mc->mc_top]) {
                inner = page_node(mp, m2->mc_ki[mc->mc_top]);
                if (node_flags(inner) & F_SUBDATA)
                  continue;
              }
              m2->mc_xcursor->mx_cursor.mc_pg[0] = node_data(inner);
            }
          }
        }
        mc->mc_db->md_entries--;
        mdbx_cassert(mc, mc->mc_db->md_entries > 0 && mc->mc_db->md_depth > 0 &&
                             mc->mc_db->md_root != P_INVALID);
        return rc;
      } else {
        mc->mc_xcursor->mx_cursor.mc_flags &= ~C_INITIALIZED;
      }
      /* otherwise fall thru and delete the sub-DB */
    }

    if (node_flags(node) & F_SUBDATA) {
      /* add all the child DB's pages to the free list */
      rc = mdbx_drop_tree(&mc->mc_xcursor->mx_cursor, false);
      if (unlikely(rc))
        goto fail;
    }
  }
  /* MDBX passes F_SUBDATA in 'flags' to delete a DB record */
  else if (unlikely((node_flags(node) ^ flags) & F_SUBDATA))
    return MDBX_INCOMPATIBLE;

  /* add overflow pages to free list */
  if (F_ISSET(node_flags(node), F_BIGDATA)) {
    MDBX_page *omp;
    if (unlikely((rc = mdbx_page_get(mc, node_largedata_pgno(node), &omp,
                                     pp_txnid4chk(mp, mc->mc_txn))) ||
                 (rc = mdbx_page_retire(mc, omp))))
      goto fail;
  }

del_key:
  return mdbx_cursor_del0(mc);

fail:
  mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
  return rc;
}

/* Allocate and initialize new pages for a database.
 * Set MDBX_TXN_ERROR on failure.
 *
 * [in] mc a  cursor on the database being added to.
 * [in] flags flags defining what type of page is being allocated.
 * [in] num   the number of pages to allocate. This is usually 1,
 *            unless allocating overflow pages for a large record.
 * [out] mp   Address of a page, or NULL on failure.
 *
 * Returns 0 on success, non-zero on failure. */
static struct page_result mdbx_page_new(MDBX_cursor *mc, const unsigned flags,
                                        const unsigned npages) {
  struct page_result ret = mdbx_page_alloc(mc, npages, MDBX_ALLOC_ALL);
  if (unlikely(ret.err != MDBX_SUCCESS))
    return ret;

  mdbx_debug("db %u allocated new page %" PRIaPGNO ", num %u", mc->mc_dbi,
             ret.page->mp_pgno, npages);
  ret.page->mp_flags = (uint16_t)flags;
  ret.page->mp_txnid = mc->mc_txn->mt_front;
  mdbx_cassert(mc, *mc->mc_dbistate & DBI_DIRTY);
  mdbx_cassert(mc, mc->mc_txn->mt_flags & MDBX_TXN_DIRTY);
#if MDBX_ENABLE_PGOP_STAT
  mc->mc_txn->mt_env->me_lck->mti_pgop_stat.newly.weak += npages;
#endif /* MDBX_ENABLE_PGOP_STAT */

  if (likely((flags & P_OVERFLOW) == 0)) {
    STATIC_ASSERT(P_BRANCH == 1);
    const bool is_branch = flags & P_BRANCH;
    ret.page->mp_lower = 0;
    ret.page->mp_upper = (indx_t)(mc->mc_txn->mt_env->me_psize - PAGEHDRSZ);
    mc->mc_db->md_branch_pages += is_branch;
    mc->mc_db->md_leaf_pages += 1 - is_branch;
    if (unlikely(mc->mc_flags & C_SUB)) {
      MDBX_db *outer = mdbx_outer_db(mc);
      outer->md_branch_pages += is_branch;
      outer->md_leaf_pages += 1 - is_branch;
    }
  } else {
    mc->mc_db->md_overflow_pages += npages;
    ret.page->mp_pages = npages;
    mdbx_cassert(mc, !(mc->mc_flags & C_SUB));
  }

  return ret;
}

static int __must_check_result mdbx_node_add_leaf2(MDBX_cursor *mc,
                                                   unsigned indx,
                                                   const MDBX_val *key) {
  MDBX_page *mp = mc->mc_pg[mc->mc_top];
  DKBUF_DEBUG;
  mdbx_debug("add to leaf2-%spage %" PRIaPGNO " index %i, "
             " key size %" PRIuPTR " [%s]",
             IS_SUBP(mp) ? "sub-" : "", mp->mp_pgno, indx,
             key ? key->iov_len : 0, DKEY_DEBUG(key));

  mdbx_cassert(mc, key);
  mdbx_cassert(mc, PAGETYPE(mp) == (P_LEAF | P_LEAF2));
  const unsigned ksize = mc->mc_db->md_xsize;
  mdbx_cassert(mc, ksize == key->iov_len);
  const unsigned nkeys = page_numkeys(mp);

  /* Just using these for counting */
  const intptr_t lower = mp->mp_lower + sizeof(indx_t);
  const intptr_t upper = mp->mp_upper - (ksize - sizeof(indx_t));
  if (unlikely(lower > upper)) {
    mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
    return MDBX_PAGE_FULL;
  }
  mp->mp_lower = (indx_t)lower;
  mp->mp_upper = (indx_t)upper;

  char *const ptr = page_leaf2key(mp, indx, ksize);
  mdbx_cassert(mc, nkeys >= indx);
  const unsigned diff = nkeys - indx;
  if (likely(diff > 0))
    /* Move higher keys up one slot. */
    memmove(ptr + ksize, ptr, diff * ksize);
  /* insert new key */
  memcpy(ptr, key->iov_base, ksize);
  return MDBX_SUCCESS;
}

static int __must_check_result mdbx_node_add_branch(MDBX_cursor *mc,
                                                    unsigned indx,
                                                    const MDBX_val *key,
                                                    pgno_t pgno) {
  MDBX_page *mp = mc->mc_pg[mc->mc_top];
  DKBUF_DEBUG;
  mdbx_debug("add to branch-%spage %" PRIaPGNO " index %i, node-pgno %" PRIaPGNO
             " key size %" PRIuPTR " [%s]",
             IS_SUBP(mp) ? "sub-" : "", mp->mp_pgno, indx, pgno,
             key ? key->iov_len : 0, DKEY_DEBUG(key));

  mdbx_cassert(mc, PAGETYPE(mp) == P_BRANCH);
  STATIC_ASSERT(NODESIZE % 2 == 0);

  /* Move higher pointers up one slot. */
  const unsigned nkeys = page_numkeys(mp);
  mdbx_cassert(mc, nkeys >= indx);
  for (unsigned i = nkeys; i > indx; --i)
    mp->mp_ptrs[i] = mp->mp_ptrs[i - 1];

  /* Adjust free space offsets. */
  const size_t branch_bytes = branch_size(mc->mc_txn->mt_env, key);
  const intptr_t lower = mp->mp_lower + sizeof(indx_t);
  const intptr_t upper = mp->mp_upper - (branch_bytes - sizeof(indx_t));
  if (unlikely(lower > upper)) {
    mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
    return MDBX_PAGE_FULL;
  }
  mp->mp_lower = (indx_t)lower;
  mp->mp_ptrs[indx] = mp->mp_upper = (indx_t)upper;

  /* Write the node data. */
  MDBX_node *node = page_node(mp, indx);
  node_set_pgno(node, pgno);
  node_set_flags(node, 0);
  UNALIGNED_POKE_8(node, MDBX_node, mn_extra, 0);
  node_set_ks(node, 0);
  if (likely(key != NULL)) {
    node_set_ks(node, key->iov_len);
    memcpy(node_key(node), key->iov_base, key->iov_len);
  }
  return MDBX_SUCCESS;
}

static int __must_check_result mdbx_node_add_leaf(MDBX_cursor *mc,
                                                  unsigned indx,
                                                  const MDBX_val *key,
                                                  MDBX_val *data,
                                                  unsigned flags) {
  MDBX_page *mp = mc->mc_pg[mc->mc_top];
  DKBUF_DEBUG;
  mdbx_debug("add to leaf-%spage %" PRIaPGNO " index %i, data size %" PRIuPTR
             " key size %" PRIuPTR " [%s]",
             IS_SUBP(mp) ? "sub-" : "", mp->mp_pgno, indx,
             data ? data->iov_len : 0, key ? key->iov_len : 0, DKEY_DEBUG(key));
  mdbx_cassert(mc, key != NULL && data != NULL);
  mdbx_cassert(mc, PAGETYPE(mp) == P_LEAF);
  mdbx_cassert(mc, page_room(mp) >= leaf_size(mc->mc_txn->mt_env, key, data));
  MDBX_page *largepage = NULL;

  size_t node_bytes;
  if (unlikely(flags & F_BIGDATA)) {
    /* Data already on overflow page. */
    STATIC_ASSERT(sizeof(pgno_t) % 2 == 0);
    node_bytes =
        node_size_len(key->iov_len, 0) + sizeof(pgno_t) + sizeof(indx_t);
  } else if (unlikely(node_size(key, data) >
                      mc->mc_txn->mt_env->me_leaf_nodemax)) {
    /* Put data on overflow page. */
    if (unlikely(mc->mc_db->md_flags & MDBX_DUPSORT)) {
      mdbx_error("Unexpected target %s flags 0x%x for large data-item",
                 "dupsort-db", mc->mc_db->md_flags);
      return MDBX_PROBLEM;
    }
    if (unlikely(flags & (F_DUPDATA | F_SUBDATA))) {
      mdbx_error("Unexpected target %s flags 0x%x for large data-item", "node",
                 flags);
      return MDBX_PROBLEM;
    }
    const pgno_t ovpages = number_of_ovpages(mc->mc_txn->mt_env, data->iov_len);
    const struct page_result npr = mdbx_page_new(mc, P_OVERFLOW, ovpages);
    if (unlikely(npr.err != MDBX_SUCCESS))
      return npr.err;
    largepage = npr.page;
    mdbx_debug("allocated %u overflow page(s) %" PRIaPGNO "for %" PRIuPTR
               " data bytes",
               largepage->mp_pages, largepage->mp_pgno, data->iov_len);
    flags |= F_BIGDATA;
    node_bytes =
        node_size_len(key->iov_len, 0) + sizeof(pgno_t) + sizeof(indx_t);
  } else {
    node_bytes = node_size(key, data) + sizeof(indx_t);
  }
  mdbx_cassert(mc, node_bytes == leaf_size(mc->mc_txn->mt_env, key, data));

  /* Move higher pointers up one slot. */
  const unsigned nkeys = page_numkeys(mp);
  mdbx_cassert(mc, nkeys >= indx);
  for (unsigned i = nkeys; i > indx; --i)
    mp->mp_ptrs[i] = mp->mp_ptrs[i - 1];

  /* Adjust free space offsets. */
  const intptr_t lower = mp->mp_lower + sizeof(indx_t);
  const intptr_t upper = mp->mp_upper - (node_bytes - sizeof(indx_t));
  if (unlikely(lower > upper)) {
    mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
    return MDBX_PAGE_FULL;
  }
  mp->mp_lower = (indx_t)lower;
  mp->mp_ptrs[indx] = mp->mp_upper = (indx_t)upper;

  /* Write the node data. */
  MDBX_node *node = page_node(mp, indx);
  node_set_ks(node, key->iov_len);
  node_set_flags(node, (uint8_t)flags);
  UNALIGNED_POKE_8(node, MDBX_node, mn_extra, 0);
  node_set_ds(node, data->iov_len);
  memcpy(node_key(node), key->iov_base, key->iov_len);

  void *nodedata = node_data(node);
  if (likely(largepage == NULL)) {
    if (unlikely(flags & F_BIGDATA))
      memcpy(nodedata, data->iov_base, sizeof(pgno_t));
    else if (unlikely(flags & MDBX_RESERVE))
      data->iov_base = nodedata;
    else if (likely(nodedata != data->iov_base &&
                    data->iov_len /* to avoid UBSAN traps*/ != 0))
      memcpy(nodedata, data->iov_base, data->iov_len);
  } else {
    poke_pgno(nodedata, largepage->mp_pgno);
    nodedata = page_data(largepage);
    if (unlikely(flags & MDBX_RESERVE))
      data->iov_base = nodedata;
    else if (likely(nodedata != data->iov_base &&
                    data->iov_len /* to avoid UBSAN traps*/ != 0))
      memcpy(nodedata, data->iov_base, data->iov_len);
  }
  return MDBX_SUCCESS;
}

/* Delete the specified node from a page.
 * [in] mc Cursor pointing to the node to delete.
 * [in] ksize The size of a node. Only used if the page is
 * part of a MDBX_DUPFIXED database. */
static void mdbx_node_del(MDBX_cursor *mc, size_t ksize) {
  MDBX_page *mp = mc->mc_pg[mc->mc_top];
  int indx = mc->mc_ki[mc->mc_top];
  int i, j, nkeys, ptr;
  MDBX_node *node;
  char *base;

  mdbx_debug("delete node %u on %s page %" PRIaPGNO, indx,
             IS_LEAF(mp) ? "leaf" : "branch", mp->mp_pgno);
  nkeys = page_numkeys(mp);
  mdbx_cassert(mc, indx < nkeys);

  if (IS_LEAF2(mp)) {
    mdbx_cassert(mc, ksize >= sizeof(indx_t));
    unsigned diff = nkeys - 1 - indx;
    base = page_leaf2key(mp, indx, ksize);
    if (diff)
      memmove(base, base + ksize, diff * ksize);
    mdbx_cassert(mc, mp->mp_lower >= sizeof(indx_t));
    mp->mp_lower -= sizeof(indx_t);
    mdbx_cassert(mc,
                 (size_t)UINT16_MAX - mp->mp_upper >= ksize - sizeof(indx_t));
    mp->mp_upper += (indx_t)(ksize - sizeof(indx_t));
    return;
  }

  node = page_node(mp, indx);
  mdbx_cassert(mc, !IS_BRANCH(mp) || indx || node_ks(node) == 0);
  size_t sz = NODESIZE + node_ks(node);
  if (IS_LEAF(mp)) {
    if (F_ISSET(node_flags(node), F_BIGDATA))
      sz += sizeof(pgno_t);
    else
      sz += node_ds(node);
  }
  sz = EVEN(sz);

  ptr = mp->mp_ptrs[indx];
  for (i = j = 0; i < nkeys; i++) {
    if (i != indx) {
      mp->mp_ptrs[j] = mp->mp_ptrs[i];
      if (mp->mp_ptrs[i] < ptr) {
        mdbx_cassert(mc, (size_t)UINT16_MAX - mp->mp_ptrs[j] >= sz);
        mp->mp_ptrs[j] += (indx_t)sz;
      }
      j++;
    }
  }

  base = (char *)mp + mp->mp_upper + PAGEHDRSZ;
  memmove(base + sz, base, ptr - mp->mp_upper);

  mdbx_cassert(mc, mp->mp_lower >= sizeof(indx_t));
  mp->mp_lower -= sizeof(indx_t);
  mdbx_cassert(mc, (size_t)UINT16_MAX - mp->mp_upper >= sz);
  mp->mp_upper += (indx_t)sz;

#if MDBX_DEBUG > 0
  if (mdbx_audit_enabled()) {
    int page_check_err = mdbx_page_check(mc, mp, C_UPDATING);
    mdbx_cassert(mc, page_check_err == MDBX_SUCCESS);
  }
#endif
}

/* Compact the main page after deleting a node on a subpage.
 * [in] mp The main page to operate on.
 * [in] indx The index of the subpage on the main page. */
static void mdbx_node_shrink(MDBX_page *mp, unsigned indx) {
  MDBX_node *node;
  MDBX_page *sp, *xp;
  char *base;
  size_t nsize, delta, len, ptr;
  int i;

  node = page_node(mp, indx);
  sp = (MDBX_page *)node_data(node);
  delta = page_room(sp);
  assert(delta > 0);

  /* Prepare to shift upward, set len = length(subpage part to shift) */
  if (IS_LEAF2(sp)) {
    delta &= /* do not make the node uneven-sized */ ~(size_t)1;
    if (unlikely(delta) == 0)
      return;
    nsize = node_ds(node) - delta;
    assert(nsize % 1 == 0);
    len = nsize;
  } else {
    xp = (MDBX_page *)((char *)sp + delta); /* destination subpage */
    for (i = page_numkeys(sp); --i >= 0;) {
      assert(sp->mp_ptrs[i] >= delta);
      xp->mp_ptrs[i] = (indx_t)(sp->mp_ptrs[i] - delta);
    }
    nsize = node_ds(node) - delta;
    len = PAGEHDRSZ;
  }
  sp->mp_upper = sp->mp_lower;
  sp->mp_pgno = mp->mp_pgno;
  node_set_ds(node, nsize);

  /* Shift <lower nodes...initial part of subpage> upward */
  base = (char *)mp + mp->mp_upper + PAGEHDRSZ;
  memmove(base + delta, base, (char *)sp + len - base);

  ptr = mp->mp_ptrs[indx];
  for (i = page_numkeys(mp); --i >= 0;) {
    if (mp->mp_ptrs[i] <= ptr) {
      assert((size_t)UINT16_MAX - mp->mp_ptrs[i] >= delta);
      mp->mp_ptrs[i] += (indx_t)delta;
    }
  }
  assert((size_t)UINT16_MAX - mp->mp_upper >= delta);
  mp->mp_upper += (indx_t)delta;
}

/* Initial setup of a sorted-dups cursor.
 *
 * Sorted duplicates are implemented as a sub-database for the given key.
 * The duplicate data items are actually keys of the sub-database.
 * Operations on the duplicate data items are performed using a sub-cursor
 * initialized when the sub-database is first accessed. This function does
 * the preliminary setup of the sub-cursor, filling in the fields that
 * depend only on the parent DB.
 *
 * [in] mc The main cursor whose sorted-dups cursor is to be initialized. */
static int mdbx_xcursor_init0(MDBX_cursor *mc) {
  MDBX_xcursor *mx = mc->mc_xcursor;
  if (!MDBX_DISABLE_PAGECHECKS && unlikely(mx == nullptr)) {
    mdbx_error("unexpected dupsort-page for non-dupsort db/cursor (dbi %u)",
               mc->mc_dbi);
    return MDBX_CORRUPTED;
  }

  mx->mx_cursor.mc_xcursor = NULL;
  mx->mx_cursor.mc_next = NULL;
  mx->mx_cursor.mc_txn = mc->mc_txn;
  mx->mx_cursor.mc_db = &mx->mx_db;
  mx->mx_cursor.mc_dbx = &mx->mx_dbx;
  mx->mx_cursor.mc_dbi = mc->mc_dbi;
  mx->mx_cursor.mc_dbistate = mc->mc_dbistate;
  mx->mx_cursor.mc_snum = 0;
  mx->mx_cursor.mc_top = 0;
  mx->mx_cursor.mc_flags = C_SUB | (mc->mc_flags & (C_COPYING | C_SKIPORD));
  mx->mx_dbx.md_name.iov_len = 0;
  mx->mx_dbx.md_name.iov_base = NULL;
  mx->mx_dbx.md_cmp = mc->mc_dbx->md_dcmp;
  mx->mx_dbx.md_dcmp = NULL;
  mx->mx_dbx.md_klen_min = INT_MAX;
  mx->mx_dbx.md_vlen_min = mx->mx_dbx.md_klen_max = mx->mx_dbx.md_vlen_max = 0;
  return MDBX_SUCCESS;
}

/* Final setup of a sorted-dups cursor.
 * Sets up the fields that depend on the data from the main cursor.
 * [in] mc The main cursor whose sorted-dups cursor is to be initialized.
 * [in] node The data containing the MDBX_db record for the sorted-dup database.
 */
static int mdbx_xcursor_init1(MDBX_cursor *mc, MDBX_node *node,
                              const MDBX_page *mp) {
  MDBX_xcursor *mx = mc->mc_xcursor;
  if (!MDBX_DISABLE_PAGECHECKS && unlikely(mx == nullptr)) {
    mdbx_error("unexpected dupsort-page for non-dupsort db/cursor (dbi %u)",
               mc->mc_dbi);
    return MDBX_CORRUPTED;
  }

  const uint8_t flags = node_flags(node);
  switch (flags) {
  default:
    mdbx_error("invalid node flags %u", flags);
    return MDBX_CORRUPTED;
  case F_DUPDATA | F_SUBDATA:
    if (!MDBX_DISABLE_PAGECHECKS &&
        unlikely(node_ds(node) != sizeof(MDBX_db))) {
      mdbx_error("invalid nested-db record size %zu", node_ds(node));
      return MDBX_CORRUPTED;
    }
    memcpy(&mx->mx_db, node_data(node), sizeof(MDBX_db));
    const txnid_t pp_txnid = mp->mp_txnid;
    if (!MDBX_DISABLE_PAGECHECKS &&
        unlikely(mx->mx_db.md_mod_txnid > pp_txnid)) {
      mdbx_error("nested-db.md_mod_txnid (%" PRIaTXN ") > page-txnid (%" PRIaTXN
                 ")",
                 mx->mx_db.md_mod_txnid, pp_txnid);
      return MDBX_CORRUPTED;
    }
    mx->mx_cursor.mc_pg[0] = 0;
    mx->mx_cursor.mc_snum = 0;
    mx->mx_cursor.mc_top = 0;
    mx->mx_cursor.mc_flags = C_SUB | (mc->mc_flags & (C_COPYING | C_SKIPORD));
    break;
  case F_DUPDATA:
    if (!MDBX_DISABLE_PAGECHECKS && unlikely(node_ds(node) <= PAGEHDRSZ)) {
      mdbx_error("invalid nested-page size %zu", node_ds(node));
      return MDBX_CORRUPTED;
    }
    MDBX_page *fp = node_data(node);
    mx->mx_db.md_depth = 1;
    mx->mx_db.md_branch_pages = 0;
    mx->mx_db.md_leaf_pages = 1;
    mx->mx_db.md_overflow_pages = 0;
    mx->mx_db.md_entries = page_numkeys(fp);
    mx->mx_db.md_root = fp->mp_pgno;
    mx->mx_db.md_mod_txnid = mp->mp_txnid;
    mx->mx_cursor.mc_snum = 1;
    mx->mx_cursor.mc_top = 0;
    mx->mx_cursor.mc_flags =
        C_INITIALIZED | C_SUB | (mc->mc_flags & (C_COPYING | C_SKIPORD));
    mx->mx_cursor.mc_pg[0] = fp;
    mx->mx_cursor.mc_ki[0] = 0;
    mx->mx_db.md_flags = flags_db2sub(mc->mc_db->md_flags);
    mx->mx_db.md_xsize =
        (mc->mc_db->md_flags & MDBX_DUPFIXED) ? fp->mp_leaf2_ksize : 0;
    break;
  }

  if (unlikely(mx->mx_db.md_xsize != mc->mc_db->md_xsize)) {
    if (!MDBX_DISABLE_PAGECHECKS && unlikely(mc->mc_db->md_xsize != 0)) {
      mdbx_error("cursor mismatched nested-db md_xsize %u",
                 mc->mc_db->md_xsize);
      return MDBX_CORRUPTED;
    }
    if (!MDBX_DISABLE_PAGECHECKS &&
        unlikely((mc->mc_db->md_flags & MDBX_DUPFIXED) == 0)) {
      mdbx_error("mismatched nested-db md_flags %u", mc->mc_db->md_flags);
      return MDBX_CORRUPTED;
    }
    if (!MDBX_DISABLE_PAGECHECKS &&
        unlikely(mx->mx_db.md_xsize < mc->mc_dbx->md_vlen_min ||
                 mx->mx_db.md_xsize > mc->mc_dbx->md_vlen_max)) {
      mdbx_error("mismatched nested-db.md_xsize (%u) <> min/max value-length "
                 "(%zu/%zu)",
                 mx->mx_db.md_xsize, mc->mc_dbx->md_vlen_min,
                 mc->mc_dbx->md_vlen_max);
      return MDBX_CORRUPTED;
    }
    mc->mc_db->md_xsize = mx->mx_db.md_xsize;
    mc->mc_dbx->md_vlen_min = mc->mc_dbx->md_vlen_max = mx->mx_db.md_xsize;
  }
  mx->mx_dbx.md_klen_min = mc->mc_dbx->md_vlen_min;
  mx->mx_dbx.md_klen_max = mc->mc_dbx->md_vlen_max;

  mdbx_debug("Sub-db -%u root page %" PRIaPGNO, mx->mx_cursor.mc_dbi,
             mx->mx_db.md_root);
  return MDBX_SUCCESS;
}

/* Fixup a sorted-dups cursor due to underlying update.
 * Sets up some fields that depend on the data from the main cursor.
 * Almost the same as init1, but skips initialization steps if the
 * xcursor had already been used.
 * [in] mc The main cursor whose sorted-dups cursor is to be fixed up.
 * [in] src_mx The xcursor of an up-to-date cursor.
 * [in] new_dupdata True if converting from a non-F_DUPDATA item. */
static int mdbx_xcursor_init2(MDBX_cursor *mc, MDBX_xcursor *src_mx,
                              bool new_dupdata) {
  MDBX_xcursor *mx = mc->mc_xcursor;
  if (!MDBX_DISABLE_PAGECHECKS && unlikely(mx == nullptr)) {
    mdbx_error("unexpected dupsort-page for non-dupsort db/cursor (dbi %u)",
               mc->mc_dbi);
    return MDBX_CORRUPTED;
  }

  if (new_dupdata) {
    mx->mx_cursor.mc_snum = 1;
    mx->mx_cursor.mc_top = 0;
    mx->mx_cursor.mc_flags |= C_INITIALIZED;
    mx->mx_cursor.mc_ki[0] = 0;
  }

  mx->mx_dbx.md_klen_min = src_mx->mx_dbx.md_klen_min;
  mx->mx_dbx.md_klen_max = src_mx->mx_dbx.md_klen_max;
  mx->mx_dbx.md_cmp = src_mx->mx_dbx.md_cmp;
  mx->mx_db = src_mx->mx_db;
  mx->mx_cursor.mc_pg[0] = src_mx->mx_cursor.mc_pg[0];
  if (mx->mx_cursor.mc_flags & C_INITIALIZED) {
    mdbx_debug("Sub-db -%u root page %" PRIaPGNO, mx->mx_cursor.mc_dbi,
               mx->mx_db.md_root);
  }
  return MDBX_SUCCESS;
}

static __inline int mdbx_couple_init(MDBX_cursor_couple *couple,
                                     const MDBX_dbi dbi, MDBX_txn *const txn,
                                     MDBX_db *const db, MDBX_dbx *const dbx,
                                     uint8_t *const dbstate) {
  couple->outer.mc_signature = MDBX_MC_LIVE;
  couple->outer.mc_next = NULL;
  couple->outer.mc_backup = NULL;
  couple->outer.mc_dbi = dbi;
  couple->outer.mc_txn = txn;
  couple->outer.mc_db = db;
  couple->outer.mc_dbx = dbx;
  couple->outer.mc_dbistate = dbstate;
  couple->outer.mc_snum = 0;
  couple->outer.mc_top = 0;
  couple->outer.mc_pg[0] = 0;
  couple->outer.mc_flags = 0;
  couple->outer.mc_ki[0] = 0;
  couple->outer.mc_xcursor = NULL;

  int rc = MDBX_SUCCESS;
  if (unlikely(*couple->outer.mc_dbistate & DBI_STALE)) {
    rc = mdbx_page_search(&couple->outer, NULL, MDBX_PS_ROOTONLY);
    rc = (rc != MDBX_NOTFOUND) ? rc : MDBX_SUCCESS;
  } else if (unlikely(couple->outer.mc_dbx->md_klen_max == 0)) {
    rc = mdbx_setup_dbx(couple->outer.mc_dbx, couple->outer.mc_db,
                        txn->mt_env->me_psize);
  }

  if (couple->outer.mc_db->md_flags & MDBX_DUPSORT) {
    couple->inner.mx_cursor.mc_signature = MDBX_MC_LIVE;
    couple->outer.mc_xcursor = &couple->inner;
    rc = mdbx_xcursor_init0(&couple->outer);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    couple->inner.mx_dbx.md_klen_min = couple->outer.mc_dbx->md_vlen_min;
    couple->inner.mx_dbx.md_klen_max = couple->outer.mc_dbx->md_vlen_max;
  }
  return rc;
}

/* Initialize a cursor for a given transaction and database. */
static int mdbx_cursor_init(MDBX_cursor *mc, MDBX_txn *txn, MDBX_dbi dbi) {
  STATIC_ASSERT(offsetof(MDBX_cursor_couple, outer) == 0);
  if (unlikely(TXN_DBI_CHANGED(txn, dbi)))
    return MDBX_BAD_DBI;

  return mdbx_couple_init(container_of(mc, MDBX_cursor_couple, outer), dbi, txn,
                          &txn->mt_dbs[dbi], &txn->mt_dbxs[dbi],
                          &txn->mt_dbistate[dbi]);
}

MDBX_cursor *mdbx_cursor_create(void *context) {
  MDBX_cursor_couple *couple = mdbx_calloc(1, sizeof(MDBX_cursor_couple));
  if (unlikely(!couple))
    return nullptr;

  couple->outer.mc_signature = MDBX_MC_READY4CLOSE;
  couple->outer.mc_dbi = UINT_MAX;
  couple->mc_userctx = context;
  return &couple->outer;
}

int mdbx_cursor_set_userctx(MDBX_cursor *mc, void *ctx) {
  if (unlikely(!mc))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_signature != MDBX_MC_READY4CLOSE &&
               mc->mc_signature != MDBX_MC_LIVE))
    return MDBX_EBADSIGN;

  MDBX_cursor_couple *couple = container_of(mc, MDBX_cursor_couple, outer);
  couple->mc_userctx = ctx;
  return MDBX_SUCCESS;
}

void *mdbx_cursor_get_userctx(const MDBX_cursor *mc) {
  if (unlikely(!mc))
    return nullptr;

  if (unlikely(mc->mc_signature != MDBX_MC_READY4CLOSE &&
               mc->mc_signature != MDBX_MC_LIVE))
    return nullptr;

  MDBX_cursor_couple *couple = container_of(mc, MDBX_cursor_couple, outer);
  return couple->mc_userctx;
}

int mdbx_cursor_bind(MDBX_txn *txn, MDBX_cursor *mc, MDBX_dbi dbi) {
  if (unlikely(!mc))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_signature != MDBX_MC_READY4CLOSE &&
               mc->mc_signature != MDBX_MC_LIVE))
    return MDBX_EBADSIGN;

  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!check_dbi(txn, dbi, DBI_VALID)))
    return MDBX_BAD_DBI;

  if (unlikely(dbi == FREE_DBI && !F_ISSET(txn->mt_flags, MDBX_TXN_RDONLY)))
    return MDBX_EACCESS;

  if (unlikely(mc->mc_backup)) /* Cursor from parent transaction */ {
    mdbx_cassert(mc, mc->mc_signature == MDBX_MC_LIVE);
    if (unlikely(mc->mc_dbi != dbi ||
                 /* paranoia */ mc->mc_signature != MDBX_MC_LIVE ||
                 mc->mc_txn != txn))
      return MDBX_EINVAL;

    assert(mc->mc_db == &txn->mt_dbs[dbi]);
    assert(mc->mc_dbx == &txn->mt_dbxs[dbi]);
    assert(mc->mc_dbi == dbi);
    assert(mc->mc_dbistate == &txn->mt_dbistate[dbi]);
    return likely(mc->mc_dbi == dbi &&
                  /* paranoia */ mc->mc_signature == MDBX_MC_LIVE &&
                  mc->mc_txn == txn)
               ? MDBX_SUCCESS
               : MDBX_EINVAL /* Disallow change DBI in nested transactions */;
  }

  if (mc->mc_signature == MDBX_MC_LIVE) {
    if (unlikely(!mc->mc_txn ||
                 mc->mc_txn->mt_signature != MDBX_MT_SIGNATURE)) {
      mdbx_error("Wrong cursor's transaction %p 0x%x",
                 __Wpedantic_format_voidptr(mc->mc_txn),
                 mc->mc_txn ? mc->mc_txn->mt_signature : 0);
      return MDBX_PROBLEM;
    }
    if (mc->mc_flags & C_UNTRACK) {
      mdbx_cassert(mc, !(mc->mc_txn->mt_flags & MDBX_TXN_RDONLY));
      MDBX_cursor **prev = &mc->mc_txn->tw.cursors[mc->mc_dbi];
      while (*prev && *prev != mc)
        prev = &(*prev)->mc_next;
      mdbx_cassert(mc, *prev == mc);
      *prev = mc->mc_next;
    }
    mc->mc_signature = MDBX_MC_READY4CLOSE;
    mc->mc_flags = 0;
    mc->mc_dbi = UINT_MAX;
    mc->mc_next = NULL;
    mc->mc_db = NULL;
    mc->mc_dbx = NULL;
    mc->mc_dbistate = NULL;
  }
  mdbx_cassert(mc, !(mc->mc_flags & C_UNTRACK));

  rc = mdbx_cursor_init(mc, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (!(txn->mt_flags & MDBX_TXN_RDONLY)) {
    mc->mc_next = txn->tw.cursors[dbi];
    txn->tw.cursors[dbi] = mc;
    mc->mc_flags |= C_UNTRACK;
  }

  return MDBX_SUCCESS;
}

int mdbx_cursor_open(MDBX_txn *txn, MDBX_dbi dbi, MDBX_cursor **ret) {
  if (unlikely(!ret))
    return MDBX_EINVAL;
  *ret = NULL;

  MDBX_cursor *const mc = mdbx_cursor_create(nullptr);
  if (unlikely(!mc))
    return MDBX_ENOMEM;

  int rc = mdbx_cursor_bind(txn, mc, dbi);
  if (unlikely(rc != MDBX_SUCCESS)) {
    mdbx_cursor_close(mc);
    return rc;
  }

  *ret = mc;
  return MDBX_SUCCESS;
}

int mdbx_cursor_renew(MDBX_txn *txn, MDBX_cursor *mc) {
  return likely(mc) ? mdbx_cursor_bind(txn, mc, mc->mc_dbi) : MDBX_EINVAL;
}

int mdbx_cursor_copy(const MDBX_cursor *src, MDBX_cursor *dest) {
  if (unlikely(!src))
    return MDBX_EINVAL;
  if (unlikely(src->mc_signature != MDBX_MC_LIVE))
    return (src->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                      : MDBX_EBADSIGN;

  int rc = mdbx_cursor_bind(src->mc_txn, dest, src->mc_dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  assert(dest->mc_db == src->mc_db);
  assert(dest->mc_dbi == src->mc_dbi);
  assert(dest->mc_dbx == src->mc_dbx);
  assert(dest->mc_dbistate == src->mc_dbistate);
again:
  assert(dest->mc_txn == src->mc_txn);
  dest->mc_flags ^= (dest->mc_flags ^ src->mc_flags) & ~C_UNTRACK;
  dest->mc_top = src->mc_top;
  dest->mc_snum = src->mc_snum;
  for (unsigned i = 0; i < src->mc_snum; ++i) {
    dest->mc_ki[i] = src->mc_ki[i];
    dest->mc_pg[i] = src->mc_pg[i];
  }

  if (src->mc_xcursor) {
    dest->mc_xcursor->mx_db = src->mc_xcursor->mx_db;
    dest->mc_xcursor->mx_dbx = src->mc_xcursor->mx_dbx;
    src = &src->mc_xcursor->mx_cursor;
    dest = &dest->mc_xcursor->mx_cursor;
    goto again;
  }

  return MDBX_SUCCESS;
}

void mdbx_cursor_close(MDBX_cursor *mc) {
  if (likely(mc)) {
    mdbx_ensure(NULL, mc->mc_signature == MDBX_MC_LIVE ||
                          mc->mc_signature == MDBX_MC_READY4CLOSE);
    MDBX_txn *const txn = mc->mc_txn;
    if (!mc->mc_backup) {
      mc->mc_txn = NULL;
      /* Remove from txn, if tracked.
       * A read-only txn (!C_UNTRACK) may have been freed already,
       * so do not peek inside it.  Only write txns track cursors. */
      if (mc->mc_flags & C_UNTRACK) {
        mdbx_ensure(txn->mt_env, check_txn_rw(txn, 0) == MDBX_SUCCESS);
        MDBX_cursor **prev = &txn->tw.cursors[mc->mc_dbi];
        while (*prev && *prev != mc)
          prev = &(*prev)->mc_next;
        mdbx_tassert(txn, *prev == mc);
        *prev = mc->mc_next;
      }
      mc->mc_signature = 0;
      mc->mc_next = mc;
      mdbx_free(mc);
    } else {
      /* Cursor closed before nested txn ends */
      mdbx_tassert(txn, mc->mc_signature == MDBX_MC_LIVE);
      mdbx_ensure(txn->mt_env, check_txn_rw(txn, 0) == MDBX_SUCCESS);
      mc->mc_signature = MDBX_MC_WAIT4EOT;
    }
  }
}

MDBX_txn *mdbx_cursor_txn(const MDBX_cursor *mc) {
  if (unlikely(!mc || mc->mc_signature != MDBX_MC_LIVE))
    return NULL;
  MDBX_txn *txn = mc->mc_txn;
  if (unlikely(!txn || txn->mt_signature != MDBX_MT_SIGNATURE))
    return NULL;
  if (unlikely(txn->mt_flags & MDBX_TXN_FINISHED))
    return NULL;
  return txn;
}

MDBX_dbi mdbx_cursor_dbi(const MDBX_cursor *mc) {
  if (unlikely(!mc || mc->mc_signature != MDBX_MC_LIVE))
    return UINT_MAX;
  return mc->mc_dbi;
}

/* Return the count of duplicate data items for the current key */
int mdbx_cursor_count(const MDBX_cursor *mc, size_t *countp) {
  if (unlikely(mc == NULL))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_signature != MDBX_MC_LIVE))
    return (mc->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                     : MDBX_EBADSIGN;

  int rc = check_txn(mc->mc_txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(countp == NULL || !(mc->mc_flags & C_INITIALIZED)))
    return MDBX_EINVAL;

  if (!mc->mc_snum) {
    *countp = 0;
    return MDBX_NOTFOUND;
  }

  MDBX_page *mp = mc->mc_pg[mc->mc_top];
  if ((mc->mc_flags & C_EOF) && mc->mc_ki[mc->mc_top] >= page_numkeys(mp)) {
    *countp = 0;
    return MDBX_NOTFOUND;
  }

  *countp = 1;
  if (mc->mc_xcursor != NULL) {
    MDBX_node *node = page_node(mp, mc->mc_ki[mc->mc_top]);
    if (F_ISSET(node_flags(node), F_DUPDATA)) {
      mdbx_cassert(mc, mc->mc_xcursor && (mc->mc_xcursor->mx_cursor.mc_flags &
                                          C_INITIALIZED));
      *countp = unlikely(mc->mc_xcursor->mx_db.md_entries > PTRDIFF_MAX)
                    ? PTRDIFF_MAX
                    : (size_t)mc->mc_xcursor->mx_db.md_entries;
    }
  }
  return MDBX_SUCCESS;
}

/* Replace the key for a branch node with a new key.
 * Set MDBX_TXN_ERROR on failure.
 * [in] mc Cursor pointing to the node to operate on.
 * [in] key The new key to use.
 * Returns 0 on success, non-zero on failure. */
static int mdbx_update_key(MDBX_cursor *mc, const MDBX_val *key) {
  MDBX_page *mp;
  MDBX_node *node;
  char *base;
  size_t len;
  int delta, ksize, oksize;
  int ptr, i, nkeys, indx;
  DKBUF_DEBUG;

  mdbx_cassert(mc, cursor_is_tracked(mc));
  indx = mc->mc_ki[mc->mc_top];
  mp = mc->mc_pg[mc->mc_top];
  node = page_node(mp, indx);
  ptr = mp->mp_ptrs[indx];
#if MDBX_DEBUG
  MDBX_val k2;
  k2.iov_base = node_key(node);
  k2.iov_len = node_ks(node);
  mdbx_debug("update key %u (offset %u) [%s] to [%s] on page %" PRIaPGNO, indx,
             ptr, DVAL_DEBUG(&k2), DKEY_DEBUG(key), mp->mp_pgno);
#endif /* MDBX_DEBUG */

  /* Sizes must be 2-byte aligned. */
  ksize = EVEN(key->iov_len);
  oksize = EVEN(node_ks(node));
  delta = ksize - oksize;

  /* Shift node contents if EVEN(key length) changed. */
  if (delta) {
    if (delta > (int)page_room(mp)) {
      /* not enough space left, do a delete and split */
      mdbx_debug("Not enough room, delta = %d, splitting...", delta);
      pgno_t pgno = node_pgno(node);
      mdbx_node_del(mc, 0);
      int rc = mdbx_page_split(mc, key, NULL, pgno, MDBX_SPLIT_REPLACE);
      if (rc == MDBX_SUCCESS && mdbx_audit_enabled())
        rc = mdbx_cursor_check(mc, C_UPDATING);
      return rc;
    }

    nkeys = page_numkeys(mp);
    for (i = 0; i < nkeys; i++) {
      if (mp->mp_ptrs[i] <= ptr) {
        mdbx_cassert(mc, mp->mp_ptrs[i] >= delta);
        mp->mp_ptrs[i] -= (indx_t)delta;
      }
    }

    base = (char *)mp + mp->mp_upper + PAGEHDRSZ;
    len = ptr - mp->mp_upper + NODESIZE;
    memmove(base - delta, base, len);
    mdbx_cassert(mc, mp->mp_upper >= delta);
    mp->mp_upper -= (indx_t)delta;

    node = page_node(mp, indx);
  }

  /* But even if no shift was needed, update ksize */
  node_set_ks(node, key->iov_len);

  if (likely(key->iov_len /* to avoid UBSAN traps*/ != 0))
    memcpy(node_key(node), key->iov_base, key->iov_len);
  return MDBX_SUCCESS;
}

/* Move a node from csrc to cdst. */
static int mdbx_node_move(MDBX_cursor *csrc, MDBX_cursor *cdst, bool fromleft) {
  int rc;
  DKBUF_DEBUG;

  MDBX_page *psrc = csrc->mc_pg[csrc->mc_top];
  MDBX_page *pdst = cdst->mc_pg[cdst->mc_top];
  mdbx_cassert(csrc, PAGETYPE(psrc) == PAGETYPE(pdst));
  mdbx_cassert(csrc, csrc->mc_dbi == cdst->mc_dbi);
  mdbx_cassert(csrc, csrc->mc_top == cdst->mc_top);
  if (unlikely(PAGETYPE(psrc) != PAGETYPE(pdst))) {
  bailout:
    mdbx_error("Wrong or mismatch pages's types (src %d, dst %d) to move node",
               PAGETYPE(psrc), PAGETYPE(pdst));
    csrc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
    return MDBX_PROBLEM;
  }

  MDBX_val key4move;
  switch (PAGETYPE(psrc)) {
  case P_BRANCH: {
    const MDBX_node *srcnode = page_node(psrc, csrc->mc_ki[csrc->mc_top]);
    mdbx_cassert(csrc, node_flags(srcnode) == 0);
    const pgno_t srcpg = node_pgno(srcnode);
    key4move.iov_len = node_ks(srcnode);
    key4move.iov_base = node_key(srcnode);

    if (csrc->mc_ki[csrc->mc_top] == 0) {
      const unsigned snum = csrc->mc_snum;
      mdbx_cassert(csrc, snum > 0);
      /* must find the lowest key below src */
      rc = mdbx_page_search_lowest(csrc);
      MDBX_page *lowest_page = csrc->mc_pg[csrc->mc_top];
      if (unlikely(rc))
        return rc;
      mdbx_cassert(csrc, IS_LEAF(lowest_page));
      if (unlikely(!IS_LEAF(lowest_page)))
        goto bailout;
      if (IS_LEAF2(lowest_page)) {
        key4move.iov_len = csrc->mc_db->md_xsize;
        key4move.iov_base = page_leaf2key(lowest_page, 0, key4move.iov_len);
      } else {
        const MDBX_node *lowest_node = page_node(lowest_page, 0);
        key4move.iov_len = node_ks(lowest_node);
        key4move.iov_base = node_key(lowest_node);
      }

      /* restore cursor after mdbx_page_search_lowest() */
      csrc->mc_snum = snum;
      csrc->mc_top = snum - 1;
      csrc->mc_ki[csrc->mc_top] = 0;

      /* paranoia */
      mdbx_cassert(csrc, psrc == csrc->mc_pg[csrc->mc_top]);
      mdbx_cassert(csrc, IS_BRANCH(psrc));
      if (unlikely(!IS_BRANCH(psrc)))
        goto bailout;
    }

    if (cdst->mc_ki[cdst->mc_top] == 0) {
      const unsigned snum = cdst->mc_snum;
      mdbx_cassert(csrc, snum > 0);
      MDBX_cursor mn;
      cursor_copy(cdst, &mn);
      /* must find the lowest key below dst */
      rc = mdbx_page_search_lowest(&mn);
      if (unlikely(rc))
        return rc;
      MDBX_page *const lowest_page = mn.mc_pg[mn.mc_top];
      mdbx_cassert(cdst, IS_LEAF(lowest_page));
      if (unlikely(!IS_LEAF(lowest_page)))
        goto bailout;
      MDBX_val key;
      if (IS_LEAF2(lowest_page)) {
        key.iov_len = mn.mc_db->md_xsize;
        key.iov_base = page_leaf2key(lowest_page, 0, key.iov_len);
      } else {
        MDBX_node *lowest_node = page_node(lowest_page, 0);
        key.iov_len = node_ks(lowest_node);
        key.iov_base = node_key(lowest_node);
      }

      /* restore cursor after mdbx_page_search_lowest() */
      mn.mc_snum = snum;
      mn.mc_top = snum - 1;
      mn.mc_ki[mn.mc_top] = 0;

      const intptr_t delta =
          EVEN(key.iov_len) - EVEN(node_ks(page_node(mn.mc_pg[mn.mc_top], 0)));
      const intptr_t needed =
          branch_size(cdst->mc_txn->mt_env, &key4move) + delta;
      const intptr_t have = page_room(pdst);
      if (unlikely(needed > have))
        return MDBX_RESULT_TRUE;

      if (unlikely((rc = mdbx_page_touch(csrc)) ||
                   (rc = mdbx_page_touch(cdst))))
        return rc;
      psrc = csrc->mc_pg[csrc->mc_top];
      pdst = cdst->mc_pg[cdst->mc_top];

      WITH_CURSOR_TRACKING(mn, rc = mdbx_update_key(&mn, &key));
      if (unlikely(rc))
        return rc;
    } else {
      const size_t needed = branch_size(cdst->mc_txn->mt_env, &key4move);
      const size_t have = page_room(pdst);
      if (unlikely(needed > have))
        return MDBX_RESULT_TRUE;

      if (unlikely((rc = mdbx_page_touch(csrc)) ||
                   (rc = mdbx_page_touch(cdst))))
        return rc;
      psrc = csrc->mc_pg[csrc->mc_top];
      pdst = cdst->mc_pg[cdst->mc_top];
    }

    mdbx_debug("moving %s-node %u [%s] on page %" PRIaPGNO
               " to node %u on page %" PRIaPGNO,
               "branch", csrc->mc_ki[csrc->mc_top], DKEY_DEBUG(&key4move),
               psrc->mp_pgno, cdst->mc_ki[cdst->mc_top], pdst->mp_pgno);
    /* Add the node to the destination page. */
    rc =
        mdbx_node_add_branch(cdst, cdst->mc_ki[cdst->mc_top], &key4move, srcpg);
  } break;

  case P_LEAF: {
    /* Mark src and dst as dirty. */
    if (unlikely((rc = mdbx_page_touch(csrc)) || (rc = mdbx_page_touch(cdst))))
      return rc;
    psrc = csrc->mc_pg[csrc->mc_top];
    pdst = cdst->mc_pg[cdst->mc_top];
    const MDBX_node *srcnode = page_node(psrc, csrc->mc_ki[csrc->mc_top]);
    MDBX_val data;
    data.iov_len = node_ds(srcnode);
    data.iov_base = node_data(srcnode);
    key4move.iov_len = node_ks(srcnode);
    key4move.iov_base = node_key(srcnode);
    mdbx_debug("moving %s-node %u [%s] on page %" PRIaPGNO
               " to node %u on page %" PRIaPGNO,
               "leaf", csrc->mc_ki[csrc->mc_top], DKEY_DEBUG(&key4move),
               psrc->mp_pgno, cdst->mc_ki[cdst->mc_top], pdst->mp_pgno);
    /* Add the node to the destination page. */
    rc = mdbx_node_add_leaf(cdst, cdst->mc_ki[cdst->mc_top], &key4move, &data,
                            node_flags(srcnode));
  } break;

  case P_LEAF | P_LEAF2: {
    /* Mark src and dst as dirty. */
    if (unlikely((rc = mdbx_page_touch(csrc)) || (rc = mdbx_page_touch(cdst))))
      return rc;
    psrc = csrc->mc_pg[csrc->mc_top];
    pdst = cdst->mc_pg[cdst->mc_top];
    key4move.iov_len = csrc->mc_db->md_xsize;
    key4move.iov_base =
        page_leaf2key(psrc, csrc->mc_ki[csrc->mc_top], key4move.iov_len);
    mdbx_debug("moving %s-node %u [%s] on page %" PRIaPGNO
               " to node %u on page %" PRIaPGNO,
               "leaf2", csrc->mc_ki[csrc->mc_top], DKEY_DEBUG(&key4move),
               psrc->mp_pgno, cdst->mc_ki[cdst->mc_top], pdst->mp_pgno);
    /* Add the node to the destination page. */
    rc = mdbx_node_add_leaf2(cdst, cdst->mc_ki[cdst->mc_top], &key4move);
  } break;

  default:
    goto bailout;
  }

  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  /* Delete the node from the source page. */
  mdbx_node_del(csrc, key4move.iov_len);

  mdbx_cassert(csrc, psrc == csrc->mc_pg[csrc->mc_top]);
  mdbx_cassert(cdst, pdst == cdst->mc_pg[cdst->mc_top]);
  mdbx_cassert(csrc, PAGETYPE(psrc) == PAGETYPE(pdst));

  {
    /* Adjust other cursors pointing to mp */
    MDBX_cursor *m2, *m3;
    const MDBX_dbi dbi = csrc->mc_dbi;
    mdbx_cassert(csrc, csrc->mc_top == cdst->mc_top);
    if (fromleft) {
      /* If we're adding on the left, bump others up */
      for (m2 = csrc->mc_txn->tw.cursors[dbi]; m2; m2 = m2->mc_next) {
        m3 = (csrc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
        if (!(m3->mc_flags & C_INITIALIZED) || m3->mc_top < csrc->mc_top)
          continue;
        if (m3 != cdst && m3->mc_pg[csrc->mc_top] == pdst &&
            m3->mc_ki[csrc->mc_top] >= cdst->mc_ki[csrc->mc_top]) {
          m3->mc_ki[csrc->mc_top]++;
        }
        if (m3 != csrc && m3->mc_pg[csrc->mc_top] == psrc &&
            m3->mc_ki[csrc->mc_top] == csrc->mc_ki[csrc->mc_top]) {
          m3->mc_pg[csrc->mc_top] = pdst;
          m3->mc_ki[csrc->mc_top] = cdst->mc_ki[cdst->mc_top];
          mdbx_cassert(csrc, csrc->mc_top > 0);
          m3->mc_ki[csrc->mc_top - 1]++;
        }
        if (XCURSOR_INITED(m3) && IS_LEAF(psrc))
          XCURSOR_REFRESH(m3, m3->mc_pg[csrc->mc_top], m3->mc_ki[csrc->mc_top]);
      }
    } else {
      /* Adding on the right, bump others down */
      for (m2 = csrc->mc_txn->tw.cursors[dbi]; m2; m2 = m2->mc_next) {
        m3 = (csrc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
        if (m3 == csrc)
          continue;
        if (!(m3->mc_flags & C_INITIALIZED) || m3->mc_top < csrc->mc_top)
          continue;
        if (m3->mc_pg[csrc->mc_top] == psrc) {
          if (!m3->mc_ki[csrc->mc_top]) {
            m3->mc_pg[csrc->mc_top] = pdst;
            m3->mc_ki[csrc->mc_top] = cdst->mc_ki[cdst->mc_top];
            mdbx_cassert(csrc, csrc->mc_top > 0);
            m3->mc_ki[csrc->mc_top - 1]--;
          } else {
            m3->mc_ki[csrc->mc_top]--;
          }
          if (XCURSOR_INITED(m3) && IS_LEAF(psrc))
            XCURSOR_REFRESH(m3, m3->mc_pg[csrc->mc_top],
                            m3->mc_ki[csrc->mc_top]);
        }
      }
    }
  }

  /* Update the parent separators. */
  if (csrc->mc_ki[csrc->mc_top] == 0) {
    mdbx_cassert(csrc, csrc->mc_top > 0);
    if (csrc->mc_ki[csrc->mc_top - 1] != 0) {
      MDBX_val key;
      if (IS_LEAF2(psrc)) {
        key.iov_len = psrc->mp_leaf2_ksize;
        key.iov_base = page_leaf2key(psrc, 0, key.iov_len);
      } else {
        MDBX_node *srcnode = page_node(psrc, 0);
        key.iov_len = node_ks(srcnode);
        key.iov_base = node_key(srcnode);
      }
      mdbx_debug("update separator for source page %" PRIaPGNO " to [%s]",
                 psrc->mp_pgno, DKEY_DEBUG(&key));
      MDBX_cursor mn;
      cursor_copy(csrc, &mn);
      mdbx_cassert(csrc, mn.mc_snum > 0);
      mn.mc_snum--;
      mn.mc_top--;
      /* We want mdbx_rebalance to find mn when doing fixups */
      WITH_CURSOR_TRACKING(mn, rc = mdbx_update_key(&mn, &key));
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    }
    if (IS_BRANCH(psrc)) {
      const MDBX_val nullkey = {0, 0};
      const indx_t ix = csrc->mc_ki[csrc->mc_top];
      csrc->mc_ki[csrc->mc_top] = 0;
      rc = mdbx_update_key(csrc, &nullkey);
      csrc->mc_ki[csrc->mc_top] = ix;
      mdbx_cassert(csrc, rc == MDBX_SUCCESS);
    }
  }

  if (cdst->mc_ki[cdst->mc_top] == 0) {
    mdbx_cassert(cdst, cdst->mc_top > 0);
    if (cdst->mc_ki[cdst->mc_top - 1] != 0) {
      MDBX_val key;
      if (IS_LEAF2(pdst)) {
        key.iov_len = pdst->mp_leaf2_ksize;
        key.iov_base = page_leaf2key(pdst, 0, key.iov_len);
      } else {
        MDBX_node *srcnode = page_node(pdst, 0);
        key.iov_len = node_ks(srcnode);
        key.iov_base = node_key(srcnode);
      }
      mdbx_debug("update separator for destination page %" PRIaPGNO " to [%s]",
                 pdst->mp_pgno, DKEY_DEBUG(&key));
      MDBX_cursor mn;
      cursor_copy(cdst, &mn);
      mdbx_cassert(cdst, mn.mc_snum > 0);
      mn.mc_snum--;
      mn.mc_top--;
      /* We want mdbx_rebalance to find mn when doing fixups */
      WITH_CURSOR_TRACKING(mn, rc = mdbx_update_key(&mn, &key));
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    }
    if (IS_BRANCH(pdst)) {
      const MDBX_val nullkey = {0, 0};
      const indx_t ix = cdst->mc_ki[cdst->mc_top];
      cdst->mc_ki[cdst->mc_top] = 0;
      rc = mdbx_update_key(cdst, &nullkey);
      cdst->mc_ki[cdst->mc_top] = ix;
      mdbx_cassert(cdst, rc == MDBX_SUCCESS);
    }
  }

  return MDBX_SUCCESS;
}

/* Merge one page into another.
 *
 * The nodes from the page pointed to by csrc will be copied to the page
 * pointed to by cdst and then the csrc page will be freed.
 *
 * [in] csrc Cursor pointing to the source page.
 * [in] cdst Cursor pointing to the destination page.
 *
 * Returns 0 on success, non-zero on failure. */
static int mdbx_page_merge(MDBX_cursor *csrc, MDBX_cursor *cdst) {
  MDBX_val key;
  int rc;

  mdbx_cassert(csrc, csrc != cdst);
  mdbx_cassert(csrc, cursor_is_tracked(csrc));
  mdbx_cassert(cdst, cursor_is_tracked(cdst));
  const MDBX_page *const psrc = csrc->mc_pg[csrc->mc_top];
  MDBX_page *pdst = cdst->mc_pg[cdst->mc_top];
  mdbx_debug("merging page %" PRIaPGNO " into %" PRIaPGNO, psrc->mp_pgno,
             pdst->mp_pgno);

  mdbx_cassert(csrc, PAGETYPE(psrc) == PAGETYPE(pdst));
  mdbx_cassert(csrc,
               csrc->mc_dbi == cdst->mc_dbi && csrc->mc_db == cdst->mc_db);
  mdbx_cassert(csrc, csrc->mc_snum > 1); /* can't merge root page */
  mdbx_cassert(cdst, cdst->mc_snum > 1);
  mdbx_cassert(cdst, cdst->mc_snum < cdst->mc_db->md_depth ||
                         IS_LEAF(cdst->mc_pg[cdst->mc_db->md_depth - 1]));
  mdbx_cassert(csrc, csrc->mc_snum < csrc->mc_db->md_depth ||
                         IS_LEAF(csrc->mc_pg[csrc->mc_db->md_depth - 1]));
  mdbx_cassert(cdst, page_room(pdst) >= page_used(cdst->mc_txn->mt_env, psrc));
  const int pagetype = PAGETYPE(psrc);

  /* Move all nodes from src to dst */
  const unsigned dst_nkeys = page_numkeys(pdst);
  const unsigned src_nkeys = page_numkeys(psrc);
  mdbx_cassert(cdst, dst_nkeys + src_nkeys >= (IS_LEAF(psrc) ? 1u : 2u));
  if (likely(src_nkeys)) {
    unsigned j = dst_nkeys;
    if (unlikely(pagetype & P_LEAF2)) {
      /* Mark dst as dirty. */
      if (unlikely(rc = mdbx_page_touch(cdst)))
        return rc;

      key.iov_len = csrc->mc_db->md_xsize;
      key.iov_base = page_data(psrc);
      unsigned i = 0;
      do {
        rc = mdbx_node_add_leaf2(cdst, j++, &key);
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;
        key.iov_base = (char *)key.iov_base + key.iov_len;
      } while (++i != src_nkeys);
    } else {
      MDBX_node *srcnode = page_node(psrc, 0);
      key.iov_len = node_ks(srcnode);
      key.iov_base = node_key(srcnode);
      if (pagetype & P_BRANCH) {
        MDBX_cursor mn;
        cursor_copy(csrc, &mn);
        /* must find the lowest key below src */
        rc = mdbx_page_search_lowest(&mn);
        if (unlikely(rc))
          return rc;

        const MDBX_page *mp = mn.mc_pg[mn.mc_top];
        if (likely(!IS_LEAF2(mp))) {
          mdbx_cassert(&mn, IS_LEAF(mp));
          const MDBX_node *lowest = page_node(mp, 0);
          key.iov_len = node_ks(lowest);
          key.iov_base = node_key(lowest);
        } else {
          mdbx_cassert(&mn, mn.mc_top > csrc->mc_top);
          key.iov_len = mp->mp_leaf2_ksize;
          key.iov_base = page_leaf2key(mp, mn.mc_ki[mn.mc_top], key.iov_len);
        }
        mdbx_cassert(&mn, key.iov_len >= csrc->mc_dbx->md_klen_min);
        mdbx_cassert(&mn, key.iov_len <= csrc->mc_dbx->md_klen_max);

        const size_t dst_room = page_room(pdst);
        const size_t src_used = page_used(cdst->mc_txn->mt_env, psrc);
        const size_t space_needed = src_used - node_ks(srcnode) + key.iov_len;
        if (unlikely(space_needed > dst_room))
          return MDBX_RESULT_TRUE;
      }

      /* Mark dst as dirty. */
      if (unlikely(rc = mdbx_page_touch(cdst)))
        return rc;

      unsigned i = 0;
      while (true) {
        if (pagetype & P_LEAF) {
          MDBX_val data;
          data.iov_len = node_ds(srcnode);
          data.iov_base = node_data(srcnode);
          rc = mdbx_node_add_leaf(cdst, j++, &key, &data, node_flags(srcnode));
        } else {
          mdbx_cassert(csrc, node_flags(srcnode) == 0);
          rc = mdbx_node_add_branch(cdst, j++, &key, node_pgno(srcnode));
        }
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;

        if (++i == src_nkeys)
          break;
        srcnode = page_node(psrc, i);
        key.iov_len = node_ks(srcnode);
        key.iov_base = node_key(srcnode);
      }
    }

    pdst = cdst->mc_pg[cdst->mc_top];
    mdbx_debug("dst page %" PRIaPGNO " now has %u keys (%.1f%% filled)",
               pdst->mp_pgno, page_numkeys(pdst),
               page_fill(cdst->mc_txn->mt_env, pdst));

    mdbx_cassert(csrc, psrc == csrc->mc_pg[csrc->mc_top]);
    mdbx_cassert(cdst, pdst == cdst->mc_pg[cdst->mc_top]);
  }

  /* Unlink the src page from parent and add to free list. */
  csrc->mc_top--;
  mdbx_node_del(csrc, 0);
  if (csrc->mc_ki[csrc->mc_top] == 0) {
    const MDBX_val nullkey = {0, 0};
    rc = mdbx_update_key(csrc, &nullkey);
    if (unlikely(rc)) {
      csrc->mc_top++;
      return rc;
    }
  }
  csrc->mc_top++;

  mdbx_cassert(csrc, psrc == csrc->mc_pg[csrc->mc_top]);
  mdbx_cassert(cdst, pdst == cdst->mc_pg[cdst->mc_top]);

  {
    /* Adjust other cursors pointing to mp */
    MDBX_cursor *m2, *m3;
    const MDBX_dbi dbi = csrc->mc_dbi;
    const unsigned top = csrc->mc_top;

    for (m2 = csrc->mc_txn->tw.cursors[dbi]; m2; m2 = m2->mc_next) {
      m3 = (csrc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
      if (m3 == csrc || top >= m3->mc_snum)
        continue;
      if (m3->mc_pg[top] == psrc) {
        m3->mc_pg[top] = pdst;
        mdbx_cassert(m3, dst_nkeys + m3->mc_ki[top] <= UINT16_MAX);
        m3->mc_ki[top] += (indx_t)dst_nkeys;
        m3->mc_ki[top - 1] = cdst->mc_ki[top - 1];
      } else if (m3->mc_pg[top - 1] == csrc->mc_pg[top - 1] &&
                 m3->mc_ki[top - 1] > csrc->mc_ki[top - 1]) {
        m3->mc_ki[top - 1]--;
      }
      if (XCURSOR_INITED(m3) && IS_LEAF(psrc))
        XCURSOR_REFRESH(m3, m3->mc_pg[top], m3->mc_ki[top]);
    }
  }

  /* If not operating on GC, allow this page to be reused
   * in this txn. Otherwise just add to free list. */
  rc = mdbx_page_retire(csrc, (MDBX_page *)psrc);
  if (unlikely(rc))
    return rc;

  mdbx_cassert(cdst, cdst->mc_db->md_entries > 0);
  mdbx_cassert(cdst, cdst->mc_snum <= cdst->mc_db->md_depth);
  mdbx_cassert(cdst, cdst->mc_top > 0);
  mdbx_cassert(cdst, cdst->mc_snum == cdst->mc_top + 1);
  MDBX_page *const top_page = cdst->mc_pg[cdst->mc_top];
  const indx_t top_indx = cdst->mc_ki[cdst->mc_top];
  const unsigned save_snum = cdst->mc_snum;
  const uint16_t save_depth = cdst->mc_db->md_depth;
  mdbx_cursor_pop(cdst);
  rc = mdbx_rebalance(cdst);
  if (unlikely(rc))
    return rc;

  mdbx_cassert(cdst, cdst->mc_db->md_entries > 0);
  mdbx_cassert(cdst, cdst->mc_snum <= cdst->mc_db->md_depth);
  mdbx_cassert(cdst, cdst->mc_snum == cdst->mc_top + 1);

#if MDBX_ENABLE_PGOP_STAT
  cdst->mc_txn->mt_env->me_lck->mti_pgop_stat.merge.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */

  if (IS_LEAF(cdst->mc_pg[cdst->mc_top])) {
    /* LY: don't touch cursor if top-page is a LEAF */
    mdbx_cassert(cdst, IS_LEAF(cdst->mc_pg[cdst->mc_top]) ||
                           PAGETYPE(cdst->mc_pg[cdst->mc_top]) == pagetype);
    return MDBX_SUCCESS;
  }

  mdbx_cassert(cdst, page_numkeys(top_page) == dst_nkeys + src_nkeys);

  if (unlikely(pagetype != PAGETYPE(top_page))) {
    /* LY: LEAF-page becomes BRANCH, unable restore cursor's stack */
    goto bailout;
  }

  if (top_page == cdst->mc_pg[cdst->mc_top]) {
    /* LY: don't touch cursor if prev top-page already on the top */
    mdbx_cassert(cdst, cdst->mc_ki[cdst->mc_top] == top_indx);
    mdbx_cassert(cdst, IS_LEAF(cdst->mc_pg[cdst->mc_top]) ||
                           PAGETYPE(cdst->mc_pg[cdst->mc_top]) == pagetype);
    return MDBX_SUCCESS;
  }

  const int new_snum = save_snum - save_depth + cdst->mc_db->md_depth;
  if (unlikely(new_snum < 1 || new_snum > cdst->mc_db->md_depth)) {
    /* LY: out of range, unable restore cursor's stack */
    goto bailout;
  }

  if (top_page == cdst->mc_pg[new_snum - 1]) {
    mdbx_cassert(cdst, cdst->mc_ki[new_snum - 1] == top_indx);
    /* LY: restore cursor stack */
    cdst->mc_snum = (uint16_t)new_snum;
    cdst->mc_top = (uint16_t)new_snum - 1;
    mdbx_cassert(cdst, cdst->mc_snum < cdst->mc_db->md_depth ||
                           IS_LEAF(cdst->mc_pg[cdst->mc_db->md_depth - 1]));
    mdbx_cassert(cdst, IS_LEAF(cdst->mc_pg[cdst->mc_top]) ||
                           PAGETYPE(cdst->mc_pg[cdst->mc_top]) == pagetype);
    return MDBX_SUCCESS;
  }

  MDBX_page *const stub_page = (MDBX_page *)(~(uintptr_t)top_page);
  const indx_t stub_indx = top_indx;
  if (save_depth > cdst->mc_db->md_depth &&
      ((cdst->mc_pg[save_snum - 1] == top_page &&
        cdst->mc_ki[save_snum - 1] == top_indx) ||
       (cdst->mc_pg[save_snum - 1] == stub_page &&
        cdst->mc_ki[save_snum - 1] == stub_indx))) {
    /* LY: restore cursor stack */
    cdst->mc_pg[new_snum - 1] = top_page;
    cdst->mc_ki[new_snum - 1] = top_indx;
    cdst->mc_pg[new_snum] = (MDBX_page *)(~(uintptr_t)cdst->mc_pg[new_snum]);
    cdst->mc_ki[new_snum] = ~cdst->mc_ki[new_snum];
    cdst->mc_snum = (uint16_t)new_snum;
    cdst->mc_top = (uint16_t)new_snum - 1;
    mdbx_cassert(cdst, cdst->mc_snum < cdst->mc_db->md_depth ||
                           IS_LEAF(cdst->mc_pg[cdst->mc_db->md_depth - 1]));
    mdbx_cassert(cdst, IS_LEAF(cdst->mc_pg[cdst->mc_top]) ||
                           PAGETYPE(cdst->mc_pg[cdst->mc_top]) == pagetype);
    return MDBX_SUCCESS;
  }

bailout:
  /* LY: unable restore cursor's stack */
  cdst->mc_flags &= ~C_INITIALIZED;
  return MDBX_CURSOR_FULL;
}

static void cursor_restore(const MDBX_cursor *csrc, MDBX_cursor *cdst) {
  mdbx_cassert(cdst, cdst->mc_dbi == csrc->mc_dbi);
  mdbx_cassert(cdst, cdst->mc_txn == csrc->mc_txn);
  mdbx_cassert(cdst, cdst->mc_db == csrc->mc_db);
  mdbx_cassert(cdst, cdst->mc_dbx == csrc->mc_dbx);
  mdbx_cassert(cdst, cdst->mc_dbistate == csrc->mc_dbistate);
  cdst->mc_snum = csrc->mc_snum;
  cdst->mc_top = csrc->mc_top;
  cdst->mc_flags = csrc->mc_flags;

  for (unsigned i = 0; i < csrc->mc_snum; i++) {
    cdst->mc_pg[i] = csrc->mc_pg[i];
    cdst->mc_ki[i] = csrc->mc_ki[i];
  }
}

/* Copy the contents of a cursor.
 * [in] csrc The cursor to copy from.
 * [out] cdst The cursor to copy to. */
static void cursor_copy(const MDBX_cursor *csrc, MDBX_cursor *cdst) {
  mdbx_cassert(csrc, csrc->mc_txn->mt_txnid >=
                         csrc->mc_txn->mt_env->me_lck->mti_oldest_reader.weak);
  cdst->mc_dbi = csrc->mc_dbi;
  cdst->mc_next = NULL;
  cdst->mc_backup = NULL;
  cdst->mc_xcursor = NULL;
  cdst->mc_txn = csrc->mc_txn;
  cdst->mc_db = csrc->mc_db;
  cdst->mc_dbx = csrc->mc_dbx;
  cdst->mc_dbistate = csrc->mc_dbistate;
  cursor_restore(csrc, cdst);
}

/* Rebalance the tree after a delete operation.
 * [in] mc Cursor pointing to the page where rebalancing should begin.
 * Returns 0 on success, non-zero on failure. */
static int mdbx_rebalance(MDBX_cursor *mc) {
  mdbx_cassert(mc, cursor_is_tracked(mc));
  mdbx_cassert(mc, mc->mc_snum > 0);
  mdbx_cassert(mc, mc->mc_snum < mc->mc_db->md_depth ||
                       IS_LEAF(mc->mc_pg[mc->mc_db->md_depth - 1]));
  const int pagetype = PAGETYPE(mc->mc_pg[mc->mc_top]);

  STATIC_ASSERT(P_BRANCH == 1);
  const unsigned minkeys = (pagetype & P_BRANCH) + 1;

  /* Pages emptier than this are candidates for merging. */
  unsigned room_threshold = likely(mc->mc_dbi != FREE_DBI)
                                ? mc->mc_txn->mt_env->me_merge_threshold
                                : mc->mc_txn->mt_env->me_merge_threshold_gc;

  const MDBX_page *const tp = mc->mc_pg[mc->mc_top];
  const unsigned numkeys = page_numkeys(tp);
  const unsigned room = page_room(tp);
  mdbx_debug("rebalancing %s page %" PRIaPGNO
             " (has %u keys, full %.1f%%, used %u, room %u bytes )",
             (pagetype & P_LEAF) ? "leaf" : "branch", tp->mp_pgno, numkeys,
             page_fill(mc->mc_txn->mt_env, tp),
             page_used(mc->mc_txn->mt_env, tp), room);

  if (unlikely(numkeys < minkeys)) {
    mdbx_debug("page %" PRIaPGNO " must be merged due keys < %u threshold",
               tp->mp_pgno, minkeys);
  } else if (unlikely(room > room_threshold)) {
    mdbx_debug("page %" PRIaPGNO " should be merged due room %u > %u threshold",
               tp->mp_pgno, room, room_threshold);
  } else {
    mdbx_debug("no need to rebalance page %" PRIaPGNO
               ", room %u < %u threshold",
               tp->mp_pgno, room, room_threshold);
    mdbx_cassert(mc, mc->mc_db->md_entries > 0);
    return MDBX_SUCCESS;
  }

  int rc;
  if (mc->mc_snum < 2) {
    MDBX_page *const mp = mc->mc_pg[0];
    const unsigned nkeys = page_numkeys(mp);
    mdbx_cassert(mc, (mc->mc_db->md_entries == 0) == (nkeys == 0));
    if (IS_SUBP(mp)) {
      mdbx_debug("%s", "Can't rebalance a subpage, ignoring");
      mdbx_cassert(mc, pagetype & P_LEAF);
      return MDBX_SUCCESS;
    }
    if (nkeys == 0) {
      mdbx_cassert(mc, IS_LEAF(mp));
      mdbx_debug("%s", "tree is completely empty");
      mc->mc_db->md_root = P_INVALID;
      mc->mc_db->md_depth = 0;
      mdbx_cassert(mc, mc->mc_db->md_branch_pages == 0 &&
                           mc->mc_db->md_overflow_pages == 0 &&
                           mc->mc_db->md_leaf_pages == 1);
      /* Adjust cursors pointing to mp */
      for (MDBX_cursor *m2 = mc->mc_txn->tw.cursors[mc->mc_dbi]; m2;
           m2 = m2->mc_next) {
        MDBX_cursor *m3 =
            (mc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
        if (m3 == mc || !(m3->mc_flags & C_INITIALIZED))
          continue;
        if (m3->mc_pg[0] == mp) {
          m3->mc_snum = 0;
          m3->mc_top = 0;
          m3->mc_flags &= ~C_INITIALIZED;
        }
      }
      mc->mc_snum = 0;
      mc->mc_top = 0;
      mc->mc_flags &= ~C_INITIALIZED;

      rc = mdbx_page_retire(mc, mp);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    } else if (IS_BRANCH(mp) && nkeys == 1) {
      mdbx_debug("%s", "collapsing root page!");
      mc->mc_db->md_root = node_pgno(page_node(mp, 0));
      rc = mdbx_page_get(mc, mc->mc_db->md_root, &mc->mc_pg[0],
                         pp_txnid4chk(mp, mc->mc_txn));
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      mc->mc_db->md_depth--;
      mc->mc_ki[0] = mc->mc_ki[1];
      for (int i = 1; i < mc->mc_db->md_depth; i++) {
        mc->mc_pg[i] = mc->mc_pg[i + 1];
        mc->mc_ki[i] = mc->mc_ki[i + 1];
      }

      /* Adjust other cursors pointing to mp */
      for (MDBX_cursor *m2 = mc->mc_txn->tw.cursors[mc->mc_dbi]; m2;
           m2 = m2->mc_next) {
        MDBX_cursor *m3 =
            (mc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
        if (m3 == mc || !(m3->mc_flags & C_INITIALIZED))
          continue;
        if (m3->mc_pg[0] == mp) {
          for (int i = 0; i < mc->mc_db->md_depth; i++) {
            m3->mc_pg[i] = m3->mc_pg[i + 1];
            m3->mc_ki[i] = m3->mc_ki[i + 1];
          }
          m3->mc_snum--;
          m3->mc_top--;
        }
      }
      mdbx_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]) ||
                           PAGETYPE(mc->mc_pg[mc->mc_top]) == pagetype);
      mdbx_cassert(mc, mc->mc_snum < mc->mc_db->md_depth ||
                           IS_LEAF(mc->mc_pg[mc->mc_db->md_depth - 1]));

      rc = mdbx_page_retire(mc, mp);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    } else {
      mdbx_debug("root page %" PRIaPGNO
                 " doesn't need rebalancing (flags 0x%x)",
                 mp->mp_pgno, mp->mp_flags);
    }
    return MDBX_SUCCESS;
  }

  /* The parent (branch page) must have at least 2 pointers,
   * otherwise the tree is invalid. */
  const unsigned pre_top = mc->mc_top - 1;
  mdbx_cassert(mc, IS_BRANCH(mc->mc_pg[pre_top]));
  mdbx_cassert(mc, !IS_SUBP(mc->mc_pg[0]));
  mdbx_cassert(mc, page_numkeys(mc->mc_pg[pre_top]) > 1);

  /* Leaf page fill factor is below the threshold.
   * Try to move keys from left or right neighbor, or
   * merge with a neighbor page. */

  /* Find neighbors. */
  MDBX_cursor mn;
  cursor_copy(mc, &mn);

  MDBX_page *left = nullptr, *right = nullptr;
  if (mn.mc_ki[pre_top] > 0) {
    rc = mdbx_page_get(
        &mn, node_pgno(page_node(mn.mc_pg[pre_top], mn.mc_ki[pre_top] - 1)),
        &left, pp_txnid4chk(mn.mc_pg[pre_top], mc->mc_txn));
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    mdbx_cassert(mc, PAGETYPE(left) == PAGETYPE(mc->mc_pg[mc->mc_top]));
  }
  if (mn.mc_ki[pre_top] + 1u < page_numkeys(mn.mc_pg[pre_top])) {
    rc = mdbx_page_get(
        &mn, node_pgno(page_node(mn.mc_pg[pre_top], mn.mc_ki[pre_top] + 1)),
        &right, pp_txnid4chk(mn.mc_pg[pre_top], mc->mc_txn));
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    mdbx_cassert(mc, PAGETYPE(right) == PAGETYPE(mc->mc_pg[mc->mc_top]));
  }
  mdbx_cassert(mc, left || right);

  const unsigned ki_top = mc->mc_ki[mc->mc_top];
  const unsigned ki_pre_top = mn.mc_ki[pre_top];
  const unsigned nkeys = page_numkeys(mn.mc_pg[mn.mc_top]);

  const unsigned left_room = left ? page_room(left) : 0;
  const unsigned right_room = right ? page_room(right) : 0;
  const unsigned left_nkeys = left ? page_numkeys(left) : 0;
  const unsigned right_nkeys = right ? page_numkeys(right) : 0;
retry:
  if (left_room > room_threshold && left_room >= right_room) {
    /* try merge with left */
    mdbx_cassert(mc, left_nkeys >= minkeys);
    mn.mc_pg[mn.mc_top] = left;
    mn.mc_ki[mn.mc_top - 1] = (indx_t)(ki_pre_top - 1);
    mn.mc_ki[mn.mc_top] = (indx_t)(left_nkeys - 1);
    mc->mc_ki[mc->mc_top] = 0;
    const unsigned new_ki = ki_top + left_nkeys;
    mn.mc_ki[mn.mc_top] += mc->mc_ki[mn.mc_top] + 1;
    /* We want mdbx_rebalance to find mn when doing fixups */
    WITH_CURSOR_TRACKING(mn, rc = mdbx_page_merge(mc, &mn));
    if (likely(rc != MDBX_RESULT_TRUE)) {
      cursor_restore(&mn, mc);
      mc->mc_ki[mc->mc_top] = (indx_t)new_ki;
      mdbx_cassert(mc, rc || page_numkeys(mc->mc_pg[mc->mc_top]) >= minkeys);
      return rc;
    }
  }
  if (right_room > room_threshold) {
    /* try merge with right */
    mdbx_cassert(mc, right_nkeys >= minkeys);
    mn.mc_pg[mn.mc_top] = right;
    mn.mc_ki[mn.mc_top - 1] = (indx_t)(ki_pre_top + 1);
    mn.mc_ki[mn.mc_top] = 0;
    mc->mc_ki[mc->mc_top] = (indx_t)nkeys;
    WITH_CURSOR_TRACKING(mn, rc = mdbx_page_merge(&mn, mc));
    if (likely(rc != MDBX_RESULT_TRUE)) {
      mc->mc_ki[mc->mc_top] = (indx_t)ki_top;
      mdbx_cassert(mc, rc || page_numkeys(mc->mc_pg[mc->mc_top]) >= minkeys);
      return rc;
    }
  }

  if (left_nkeys > minkeys &&
      (right_nkeys <= left_nkeys || right_room >= left_room)) {
    /* try move from left */
    mn.mc_pg[mn.mc_top] = left;
    mn.mc_ki[mn.mc_top - 1] = (indx_t)(ki_pre_top - 1);
    mn.mc_ki[mn.mc_top] = (indx_t)(left_nkeys - 1);
    mc->mc_ki[mc->mc_top] = 0;
    WITH_CURSOR_TRACKING(mn, rc = mdbx_node_move(&mn, mc, true));
    if (likely(rc != MDBX_RESULT_TRUE)) {
      mc->mc_ki[mc->mc_top] = (indx_t)(ki_top + 1);
      mdbx_cassert(mc, rc || page_numkeys(mc->mc_pg[mc->mc_top]) >= minkeys);
      return rc;
    }
  }
  if (right_nkeys > minkeys) {
    /* try move from right */
    mn.mc_pg[mn.mc_top] = right;
    mn.mc_ki[mn.mc_top - 1] = (indx_t)(ki_pre_top + 1);
    mn.mc_ki[mn.mc_top] = 0;
    mc->mc_ki[mc->mc_top] = (indx_t)nkeys;
    WITH_CURSOR_TRACKING(mn, rc = mdbx_node_move(&mn, mc, false));
    if (likely(rc != MDBX_RESULT_TRUE)) {
      mc->mc_ki[mc->mc_top] = (indx_t)ki_top;
      mdbx_cassert(mc, rc || page_numkeys(mc->mc_pg[mc->mc_top]) >= minkeys);
      return rc;
    }
  }

  if (nkeys >= minkeys) {
    mc->mc_ki[mc->mc_top] = (indx_t)ki_top;
    if (!mdbx_audit_enabled())
      return MDBX_SUCCESS;
    return mdbx_cursor_check(mc, C_UPDATING);
  }

  if (likely(room_threshold > 0)) {
    room_threshold = 0;
    goto retry;
  }
  mdbx_error("Unable to merge/rebalance %s page %" PRIaPGNO
             " (has %u keys, full %.1f%%, used %u, room %u bytes )",
             (pagetype & P_LEAF) ? "leaf" : "branch", tp->mp_pgno, numkeys,
             page_fill(mc->mc_txn->mt_env, tp),
             page_used(mc->mc_txn->mt_env, tp), room);
  return MDBX_PROBLEM;
}

__cold static int mdbx_page_check(MDBX_cursor *const mc,
                                  const MDBX_page *const mp, unsigned options) {
  DKBUF;
  options |= mc->mc_flags;
  MDBX_env *const env = mc->mc_txn->mt_env;
  const unsigned nkeys = page_numkeys(mp);
  char *const end_of_page = (char *)mp + env->me_psize;
  if (unlikely(mp->mp_pgno < MIN_PAGENO || mp->mp_pgno > MAX_PAGENO))
    return bad_page(mp, "invalid pgno (%u)\n", mp->mp_pgno);
  if (IS_OVERFLOW(mp)) {
    if (unlikely(mp->mp_pages < 1 && mp->mp_pages >= MAX_PAGENO / 2))
      return bad_page(mp, "invalid overflow n-pages (%u)\n", mp->mp_pages);
    if (unlikely(mp->mp_pgno + mp->mp_pages > mc->mc_txn->mt_next_pgno))
      return bad_page(mp, "overflow page beyond (%u) next-pgno\n",
                      mp->mp_pgno + mp->mp_pages);
    if (unlikely((options & (C_SUB | C_COPYING)) == C_SUB))
      return bad_page(mp,
                      "unexpected overflow-page for dupsort db (flags 0x%x)\n",
                      mc->mc_db->md_flags);
    return MDBX_SUCCESS;
  }

  int rc = MDBX_SUCCESS;
  if ((options & C_UPDATING) == 0 || !IS_MODIFIABLE(mc->mc_txn, mp)) {
    if (unlikely(nkeys < 2 && IS_BRANCH(mp)))
      rc = bad_page(mp, "branch-page nkey (%u) < 2\n", nkeys);
  }
  if (IS_LEAF2(mp) && unlikely((options & (C_SUB | C_COPYING)) == 0))
    rc = bad_page(mp, "unexpected leaf2-page (db flags 0x%x)\n",
                  mc->mc_db->md_flags);

  MDBX_val here, prev = {0, 0};
  for (unsigned i = 0; i < nkeys; ++i) {
    if (IS_LEAF2(mp)) {
      const size_t ksize = mp->mp_leaf2_ksize;
      char *const key = page_leaf2key(mp, i, ksize);
      if (unlikely(end_of_page < key + ksize)) {
        rc = bad_page(mp, "leaf2-key beyond (%zu) page-end\n",
                      key + ksize - end_of_page);
        continue;
      }

      if ((options & C_COPYING) == 0) {
        if (unlikely(ksize != mc->mc_dbx->md_klen_min)) {
          if (unlikely(ksize < mc->mc_dbx->md_klen_min ||
                       ksize > mc->mc_dbx->md_klen_max))
            rc = bad_page(
                mp, "leaf2-key size (%zu) <> min/max key-length (%zu/%zu)\n",
                ksize, mc->mc_dbx->md_klen_min, mc->mc_dbx->md_klen_max);
          else
            mc->mc_dbx->md_klen_min = mc->mc_dbx->md_klen_max = ksize;
        }
        if ((options & C_SKIPORD) == 0) {
          here.iov_len = ksize;
          here.iov_base = key;
          if (prev.iov_base && unlikely(mc->mc_dbx->md_cmp(&prev, &here) >= 0))
            rc = bad_page(mp, "leaf2-key #%u wrong order (%s >= %s)\n", i,
                          DKEY(&prev), DVAL(&here));
          prev = here;
        }
      }
    } else {
      const MDBX_node *const node = page_node(mp, i);
      const char *node_end = (char *)node + NODESIZE;
      if (unlikely(node_end > end_of_page)) {
        rc = bad_page(mp, "node[%u] (%zu) beyond page-end\n", i,
                      node_end - end_of_page);
        continue;
      }
      size_t ksize = node_ks(node);
      char *key = node_key(node);
      if (unlikely(end_of_page < key + ksize)) {
        rc = bad_page(mp, "node[%u] key (%zu) beyond page-end\n", i,
                      key + ksize - end_of_page);
        continue;
      }
      if ((IS_LEAF(mp) || i > 0) && (options & C_COPYING) == 0) {
        if (unlikely(ksize < mc->mc_dbx->md_klen_min ||
                     ksize > mc->mc_dbx->md_klen_max))
          rc = bad_page(
              mp, "node[%u] key size (%zu) <> min/max key-length (%zu/%zu)\n",
              i, ksize, mc->mc_dbx->md_klen_min, mc->mc_dbx->md_klen_max);
        if ((options & C_SKIPORD) == 0) {
          here.iov_base = key;
          here.iov_len = ksize;
          if (prev.iov_base && unlikely(mc->mc_dbx->md_cmp(&prev, &here) >= 0))
            rc = bad_page(mp, "node[%u] key wrong order (%s >= %s)\n", i,
                          DKEY(&prev), DVAL(&here));
          prev = here;
        }
      }
      if (IS_BRANCH(mp)) {
        if ((options & C_UPDATING) == 0 && i == 0 && unlikely(ksize != 0))
          rc = bad_page(mp, "branch-node[%u] wrong 0-node key-length (%zu)\n",
                        i, ksize);
        if ((options & C_RETIRING) == 0) {
          const pgno_t ref = node_pgno(node);
          if (unlikely(ref < MIN_PAGENO || ref >= mc->mc_txn->mt_next_pgno))
            rc = bad_page(mp, "branch-node[%u] wrong pgno (%u)\n", i, ref);
        }
        if (unlikely(node_flags(node)))
          rc = bad_page(mp, "branch-node[%u] wrong flags (%u)\n", i,
                        node_flags(node));
        continue;
      }

      switch (node_flags(node)) {
      default:
        rc = bad_page(mp, "invalid node[%u] flags (%u)\n", i, node_flags(node));
        break;
      case F_BIGDATA /* data on large-page */:
      case 0 /* usual */:
      case F_SUBDATA /* sub-db */:
      case F_SUBDATA | F_DUPDATA /* dupsorted sub-tree */:
      case F_DUPDATA /* short sub-page */:
        break;
      }

      const size_t dsize = node_ds(node);
      const char *const data = node_data(node);
      if (node_flags(node) & F_BIGDATA) {
        if (unlikely(end_of_page < data + sizeof(pgno_t))) {
          rc = bad_page(
              mp, "node-%s(%u of %u, %zu bytes) beyond (%zu) page-end\n",
              "bigdata-pgno", i, nkeys, dsize, data + dsize - end_of_page);
          continue;
        }
        if ((options & C_COPYING) == 0) {
          if (unlikely(dsize <= mc->mc_dbx->md_vlen_min ||
                       dsize > mc->mc_dbx->md_vlen_max))
            rc = bad_page(
                mp,
                "big-node data size (%zu) <> min/max value-length (%zu/%zu)\n",
                dsize, mc->mc_dbx->md_vlen_min, mc->mc_dbx->md_vlen_max);
        }
        if ((options & C_RETIRING) == 0) {
          MDBX_page *lp;
          int err = mdbx_page_get(mc, node_largedata_pgno(node), &lp,
                                  pp_txnid4chk(mp, mc->mc_txn));
          if (unlikely(err != MDBX_SUCCESS))
            return err;
          if (unlikely(!IS_OVERFLOW(lp))) {
            rc = bad_page(mp, "big-node refs to non-overflow page (%u)\n",
                          lp->mp_pgno);
            continue;
          }
          if (unlikely(number_of_ovpages(env, dsize) > lp->mp_pages))
            rc =
                bad_page(mp, "big-node size (%zu) mismatch n-pages size (%u)\n",
                         dsize, lp->mp_pages);
        }
        continue;
      }

      if (unlikely(end_of_page < data + dsize)) {
        rc =
            bad_page(mp, "node-%s(%u of %u, %zu bytes) beyond (%zu) page-end\n",
                     "data", i, nkeys, dsize, data + dsize - end_of_page);
        continue;
      }

      switch (node_flags(node)) {
      default:
        /* wrong, but already handled */
        continue;
      case 0 /* usual */:
        if ((options & C_COPYING) == 0) {
          if (unlikely(dsize < mc->mc_dbx->md_vlen_min ||
                       dsize > mc->mc_dbx->md_vlen_max)) {
            rc = bad_page(
                mp, "node-data size (%zu) <> min/max value-length (%zu/%zu)\n",
                dsize, mc->mc_dbx->md_vlen_min, mc->mc_dbx->md_vlen_max);
            continue;
          }
        }
        break;
      case F_SUBDATA /* sub-db */:
        if (unlikely(dsize != sizeof(MDBX_db))) {
          rc = bad_page(mp, "invalid sub-db record size (%zu)\n", dsize);
          continue;
        }
        break;
      case F_SUBDATA | F_DUPDATA /* dupsorted sub-tree */:
        if (unlikely(dsize != sizeof(MDBX_db))) {
          rc = bad_page(mp, "invalid nested-db record size (%zu)\n", dsize);
          continue;
        }
        break;
      case F_DUPDATA /* short sub-page */:
        if (unlikely(dsize <= PAGEHDRSZ)) {
          rc = bad_page(mp, "invalid nested/sub-page record size (%zu)\n",
                        dsize);
          continue;
        } else {
          const MDBX_page *const sp = (MDBX_page *)data;
          const char *const end_of_subpage = data + dsize;
          const int nsubkeys = page_numkeys(sp);
          switch (sp->mp_flags & /* ignore legacy P_DIRTY flag */ ~0x10) {
          case P_LEAF | P_SUBP:
          case P_LEAF | P_LEAF2 | P_SUBP:
            break;
          default:
            rc = bad_page(mp, "invalid nested/sub-page flags (0x%02x)\n",
                          sp->mp_flags);
            continue;
          }

          MDBX_val sub_here, sub_prev = {0, 0};
          for (int j = 0; j < nsubkeys; j++) {
            if (IS_LEAF2(sp)) {
              /* LEAF2 pages have no mp_ptrs[] or node headers */
              size_t sub_ksize = sp->mp_leaf2_ksize;
              char *sub_key = page_leaf2key(sp, j, sub_ksize);
              if (unlikely(end_of_subpage < sub_key + sub_ksize)) {
                rc = bad_page(mp, "nested-leaf2-key beyond (%zu) nested-page\n",
                              sub_key + sub_ksize - end_of_subpage);
                continue;
              }

              if ((options & C_COPYING) == 0) {
                if (unlikely(sub_ksize != mc->mc_dbx->md_vlen_min)) {
                  if (unlikely(sub_ksize < mc->mc_dbx->md_vlen_min ||
                               sub_ksize > mc->mc_dbx->md_vlen_max)) {
                    rc = bad_page(mp,
                                  "nested-leaf2-key size (%zu) <> min/max "
                                  "value-length (%zu/%zu)\n",
                                  sub_ksize, mc->mc_dbx->md_vlen_min,
                                  mc->mc_dbx->md_vlen_max);
                    continue;
                  }
                  mc->mc_dbx->md_vlen_min = mc->mc_dbx->md_vlen_max = sub_ksize;
                }
                if ((options & C_SKIPORD) == 0) {
                  sub_here.iov_len = sub_ksize;
                  sub_here.iov_base = sub_key;
                  if (sub_prev.iov_base &&
                      unlikely(mc->mc_dbx->md_dcmp(&sub_prev, &sub_here) >= 0))
                    rc = bad_page(
                        mp, "nested-leaf2-key #%u wrong order (%s >= %s)\n", j,
                        DKEY(&sub_prev), DVAL(&sub_here));
                  sub_prev = sub_here;
                }
              }
            } else {
              const MDBX_node *const sub_node = page_node(sp, j);
              const char *sub_node_end = (char *)sub_node + NODESIZE;
              if (unlikely(sub_node_end > end_of_subpage)) {
                rc = bad_page(mp, "nested-node beyond (%zu) nested-page\n",
                              end_of_subpage - sub_node_end);
                continue;
              }
              if (unlikely(node_flags(sub_node) != 0))
                rc = bad_page(mp, "nested-node invalid flags (%u)\n",
                              node_flags(sub_node));

              size_t sub_ksize = node_ks(sub_node);
              char *sub_key = node_key(sub_node);
              size_t sub_dsize = node_ds(sub_node);
              /* char *sub_data = node_data(sub_node); */

              if ((options & C_COPYING) == 0) {
                if (unlikely(sub_ksize < mc->mc_dbx->md_vlen_min ||
                             sub_ksize > mc->mc_dbx->md_vlen_max))
                  rc = bad_page(mp,
                                "nested-node-key size (%zu) <> min/max "
                                "value-length (%zu/%zu)\n",
                                sub_ksize, mc->mc_dbx->md_vlen_min,
                                mc->mc_dbx->md_vlen_max);

                if ((options & C_SKIPORD) == 0) {
                  sub_here.iov_len = sub_ksize;
                  sub_here.iov_base = sub_key;
                  if (sub_prev.iov_base &&
                      unlikely(mc->mc_dbx->md_dcmp(&sub_prev, &sub_here) >= 0))
                    rc = bad_page(
                        mp, "nested-node-key #%u wrong order (%s >= %s)\n", j,
                        DKEY(&sub_prev), DVAL(&sub_here));
                  sub_prev = sub_here;
                }
              }
              if (unlikely(sub_dsize != 0))
                rc = bad_page(mp, "nested-node non-empty data size (%zu)\n",
                              sub_dsize);
              if (unlikely(end_of_subpage < sub_key + sub_ksize))
                rc = bad_page(mp, "nested-node-key beyond (%zu) nested-page\n",
                              sub_key + sub_ksize - end_of_subpage);
            }
          }
        }
        break;
      }
    }
  }
  return rc;
}

__cold static int mdbx_cursor_check(MDBX_cursor *mc, unsigned options) {
  mdbx_cassert(mc,
               mc->mc_txn->tw.dirtyroom + mc->mc_txn->tw.dirtylist->length ==
                   (mc->mc_txn->mt_parent
                        ? mc->mc_txn->mt_parent->tw.dirtyroom
                        : mc->mc_txn->mt_env->me_options.dp_limit));
  mdbx_cassert(mc, mc->mc_top == mc->mc_snum - 1 || (options & C_UPDATING));
  if (unlikely(mc->mc_top != mc->mc_snum - 1) && (options & C_UPDATING) == 0)
    return MDBX_CURSOR_FULL;
  mdbx_cassert(mc, (options & C_UPDATING) ? mc->mc_snum <= mc->mc_db->md_depth
                                          : mc->mc_snum == mc->mc_db->md_depth);
  if (unlikely((options & C_UPDATING) ? mc->mc_snum > mc->mc_db->md_depth
                                      : mc->mc_snum != mc->mc_db->md_depth))
    return MDBX_CURSOR_FULL;

  for (int n = 0; n < (int)mc->mc_snum; ++n) {
    MDBX_page *mp = mc->mc_pg[n];
    const unsigned nkeys = page_numkeys(mp);
    const bool expect_branch = (n < mc->mc_db->md_depth - 1) ? true : false;
    const bool expect_nested_leaf =
        (n + 1 == mc->mc_db->md_depth - 1) ? true : false;
    const bool branch = IS_BRANCH(mp) ? true : false;
    mdbx_cassert(mc, branch == expect_branch);
    if (unlikely(branch != expect_branch))
      return MDBX_CURSOR_FULL;
    if ((options & C_UPDATING) == 0) {
      mdbx_cassert(mc,
                   nkeys > mc->mc_ki[n] || (!branch && nkeys == mc->mc_ki[n] &&
                                            (mc->mc_flags & C_EOF) != 0));
      if (unlikely(nkeys <= mc->mc_ki[n] &&
                   !(!branch && nkeys == mc->mc_ki[n] &&
                     (mc->mc_flags & C_EOF) != 0)))
        return MDBX_CURSOR_FULL;
    } else {
      mdbx_cassert(mc, nkeys + 1 >= mc->mc_ki[n]);
      if (unlikely(nkeys + 1 < mc->mc_ki[n]))
        return MDBX_CURSOR_FULL;
    }

    int err = mdbx_page_check(mc, mp, options);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    for (unsigned i = 0; i < nkeys; ++i) {
      if (branch) {
        MDBX_node *node = page_node(mp, i);
        mdbx_cassert(mc, node_flags(node) == 0);
        if (unlikely(node_flags(node) != 0))
          return MDBX_CURSOR_FULL;
        pgno_t pgno = node_pgno(node);
        MDBX_page *np;
        int rc = mdbx_page_get(mc, pgno, &np, pp_txnid4chk(mp, mc->mc_txn));
        mdbx_cassert(mc, rc == MDBX_SUCCESS);
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;
        const bool nested_leaf = IS_LEAF(np) ? true : false;
        mdbx_cassert(mc, nested_leaf == expect_nested_leaf);
        if (unlikely(nested_leaf != expect_nested_leaf))
          return MDBX_CURSOR_FULL;
        err = mdbx_page_check(mc, np, options);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
      }
    }
  }
  return MDBX_SUCCESS;
}

/* Complete a delete operation started by mdbx_cursor_del(). */
static int mdbx_cursor_del0(MDBX_cursor *mc) {
  int rc;
  MDBX_page *mp;
  indx_t ki;
  unsigned nkeys;
  MDBX_dbi dbi = mc->mc_dbi;

  mdbx_cassert(mc, cursor_is_tracked(mc));
  mdbx_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]));
  ki = mc->mc_ki[mc->mc_top];
  mp = mc->mc_pg[mc->mc_top];
  mdbx_node_del(mc, mc->mc_db->md_xsize);
  mc->mc_db->md_entries--;

  /* Adjust other cursors pointing to mp */
  for (MDBX_cursor *m2 = mc->mc_txn->tw.cursors[dbi]; m2; m2 = m2->mc_next) {
    MDBX_cursor *m3 = (mc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
    if (m3 == mc || !(m2->mc_flags & m3->mc_flags & C_INITIALIZED))
      continue;
    if (m3->mc_snum < mc->mc_snum)
      continue;
    if (m3->mc_pg[mc->mc_top] == mp) {
      if (m3->mc_ki[mc->mc_top] == ki) {
        m3->mc_flags |= C_DEL;
        if (mc->mc_db->md_flags & MDBX_DUPSORT) {
          /* Sub-cursor referred into dataset which is gone */
          m3->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED | C_EOF);
        }
        continue;
      } else if (m3->mc_ki[mc->mc_top] > ki) {
        m3->mc_ki[mc->mc_top]--;
      }
      if (XCURSOR_INITED(m3))
        XCURSOR_REFRESH(m3, m3->mc_pg[mc->mc_top], m3->mc_ki[mc->mc_top]);
    }
  }

  rc = mdbx_rebalance(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  if (unlikely(!mc->mc_snum)) {
    /* DB is totally empty now, just bail out.
     * Other cursors adjustments were already done
     * by mdbx_rebalance and aren't needed here. */
    mdbx_cassert(mc, mc->mc_db->md_entries == 0 && mc->mc_db->md_depth == 0 &&
                         mc->mc_db->md_root == P_INVALID);
    mc->mc_flags |= C_EOF;
    return MDBX_SUCCESS;
  }

  ki = mc->mc_ki[mc->mc_top];
  mp = mc->mc_pg[mc->mc_top];
  mdbx_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]));
  nkeys = page_numkeys(mp);
  mdbx_cassert(mc, (mc->mc_db->md_entries > 0 && nkeys > 0) ||
                       ((mc->mc_flags & C_SUB) && mc->mc_db->md_entries == 0 &&
                        nkeys == 0));

  /* Adjust this and other cursors pointing to mp */
  for (MDBX_cursor *m2 = mc->mc_txn->tw.cursors[dbi]; m2; m2 = m2->mc_next) {
    MDBX_cursor *m3 = (mc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
    if (!(m2->mc_flags & m3->mc_flags & C_INITIALIZED))
      continue;
    if (m3->mc_snum < mc->mc_snum)
      continue;
    if (m3->mc_pg[mc->mc_top] == mp) {
      /* if m3 points past last node in page, find next sibling */
      if (m3->mc_ki[mc->mc_top] >= nkeys) {
        rc = mdbx_cursor_sibling(m3, SIBLING_RIGHT);
        if (rc == MDBX_NOTFOUND) {
          m3->mc_flags |= C_EOF;
          rc = MDBX_SUCCESS;
          continue;
        }
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      }
      if (m3->mc_ki[mc->mc_top] >= ki ||
          /* moved to right sibling */ m3->mc_pg[mc->mc_top] != mp) {
        if (m3->mc_xcursor && !(m3->mc_flags & C_EOF)) {
          MDBX_node *node =
              page_node(m3->mc_pg[m3->mc_top], m3->mc_ki[m3->mc_top]);
          /* If this node has dupdata, it may need to be reinited
           * because its data has moved.
           * If the xcursor was not inited it must be reinited.
           * Else if node points to a subDB, nothing is needed. */
          if (node_flags(node) & F_DUPDATA) {
            if (m3->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED) {
              if (!(node_flags(node) & F_SUBDATA))
                m3->mc_xcursor->mx_cursor.mc_pg[0] = node_data(node);
            } else {
              rc = mdbx_xcursor_init1(m3, node, m3->mc_pg[m3->mc_top]);
              if (unlikely(rc != MDBX_SUCCESS))
                goto bailout;
              rc = mdbx_cursor_first(&m3->mc_xcursor->mx_cursor, NULL, NULL);
              if (unlikely(rc != MDBX_SUCCESS))
                goto bailout;
            }
          }
          m3->mc_xcursor->mx_cursor.mc_flags |= C_DEL;
        }
        m3->mc_flags |= C_DEL;
      }
    }
  }

  mdbx_cassert(mc, rc == MDBX_SUCCESS);
  if (mdbx_audit_enabled())
    rc = mdbx_cursor_check(mc, 0);
  return rc;

bailout:
  mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
  return rc;
}

int mdbx_del(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key,
             const MDBX_val *data) {
  int rc = check_txn_rw(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key))
    return MDBX_EINVAL;

  if (unlikely(!check_dbi(txn, dbi, DBI_USRVALID)))
    return MDBX_BAD_DBI;

  if (unlikely(txn->mt_flags & (MDBX_TXN_RDONLY | MDBX_TXN_BLOCKED)))
    return (txn->mt_flags & MDBX_TXN_RDONLY) ? MDBX_EACCESS : MDBX_BAD_TXN;

  return mdbx_del0(txn, dbi, key, data, 0);
}

static int mdbx_del0(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key,
                     const MDBX_val *data, unsigned flags) {
  MDBX_cursor_couple cx;
  MDBX_cursor_op op;
  MDBX_val rdata;
  int rc;
  DKBUF_DEBUG;

  mdbx_debug("====> delete db %u key [%s], data [%s]", dbi, DKEY_DEBUG(key),
             DVAL_DEBUG(data));

  rc = mdbx_cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (data) {
    op = MDBX_GET_BOTH;
    rdata = *data;
    data = &rdata;
  } else {
    op = MDBX_SET;
    flags |= MDBX_ALLDUPS;
  }
  rc = mdbx_cursor_set(&cx.outer, (MDBX_val *)key, (MDBX_val *)data, op).err;
  if (likely(rc == MDBX_SUCCESS)) {
    /* let mdbx_page_split know about this cursor if needed:
     * delete will trigger a rebalance; if it needs to move
     * a node from one page to another, it will have to
     * update the parent's separator key(s). If the new sepkey
     * is larger than the current one, the parent page may
     * run out of space, triggering a split. We need this
     * cursor to be consistent until the end of the rebalance. */
    cx.outer.mc_next = txn->tw.cursors[dbi];
    txn->tw.cursors[dbi] = &cx.outer;
    rc = mdbx_cursor_del(&cx.outer, flags);
    txn->tw.cursors[dbi] = cx.outer.mc_next;
  }
  return rc;
}

/* Split a page and insert a new node.
 * Set MDBX_TXN_ERROR on failure.
 * [in,out] mc Cursor pointing to the page and desired insertion index.
 * The cursor will be updated to point to the actual page and index where
 * the node got inserted after the split.
 * [in] newkey The key for the newly inserted node.
 * [in] newdata The data for the newly inserted node.
 * [in] newpgno The page number, if the new node is a branch node.
 * [in] nflags The NODE_ADD_FLAGS for the new node.
 * Returns 0 on success, non-zero on failure. */
static int mdbx_page_split(MDBX_cursor *mc, const MDBX_val *const newkey,
                           MDBX_val *const newdata, pgno_t newpgno,
                           unsigned nflags) {
  unsigned flags;
  int rc = MDBX_SUCCESS, foliage = 0;
  unsigned i, ptop;
  MDBX_env *const env = mc->mc_txn->mt_env;
  MDBX_val sepkey, rkey, xdata;
  MDBX_page *tmp_ki_copy = NULL;
  DKBUF;

  MDBX_page *const mp = mc->mc_pg[mc->mc_top];
  const unsigned newindx = mc->mc_ki[mc->mc_top];
  unsigned nkeys = page_numkeys(mp);
  if (mdbx_audit_enabled()) {
    rc = mdbx_cursor_check(mc, C_UPDATING);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }
  STATIC_ASSERT(P_BRANCH == 1);
  const unsigned minkeys = (mp->mp_flags & P_BRANCH) + 1;

  mdbx_debug(">> splitting %s-page %" PRIaPGNO
             " and adding %zu+%zu [%s] at %i, nkeys %i",
             IS_LEAF(mp) ? "leaf" : "branch", mp->mp_pgno, newkey->iov_len,
             newdata ? newdata->iov_len : 0, DKEY_DEBUG(newkey),
             mc->mc_ki[mc->mc_top], nkeys);
  mdbx_cassert(mc, nkeys + 1 >= minkeys * 2);

  /* Create a new sibling page. */
  struct page_result npr = mdbx_page_new(mc, mp->mp_flags, 1);
  if (unlikely(npr.err != MDBX_SUCCESS))
    return npr.err;
  MDBX_page *const sister = npr.page;
  sister->mp_leaf2_ksize = mp->mp_leaf2_ksize;
  mdbx_debug("new sibling: page %" PRIaPGNO, sister->mp_pgno);

  /* Usually when splitting the root page, the cursor
   * height is 1. But when called from mdbx_update_key,
   * the cursor height may be greater because it walks
   * up the stack while finding the branch slot to update. */
  if (mc->mc_top < 1) {
    npr = mdbx_page_new(mc, P_BRANCH, 1);
    rc = npr.err;
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;
    MDBX_page *const pp = npr.page;
    /* shift current top to make room for new parent */
    mdbx_cassert(mc, mc->mc_snum < 2 && mc->mc_db->md_depth > 0);
#if MDBX_DEBUG
    memset(mc->mc_pg + 3, 0, sizeof(mc->mc_pg) - sizeof(mc->mc_pg[0]) * 3);
    memset(mc->mc_ki + 3, -1, sizeof(mc->mc_ki) - sizeof(mc->mc_ki[0]) * 3);
#endif
    mc->mc_pg[2] = mc->mc_pg[1];
    mc->mc_ki[2] = mc->mc_ki[1];
    mc->mc_pg[1] = mc->mc_pg[0];
    mc->mc_ki[1] = mc->mc_ki[0];
    mc->mc_pg[0] = pp;
    mc->mc_ki[0] = 0;
    mc->mc_db->md_root = pp->mp_pgno;
    mdbx_debug("root split! new root = %" PRIaPGNO, pp->mp_pgno);
    foliage = mc->mc_db->md_depth++;

    /* Add left (implicit) pointer. */
    rc = mdbx_node_add_branch(mc, 0, NULL, mp->mp_pgno);
    if (unlikely(rc != MDBX_SUCCESS)) {
      /* undo the pre-push */
      mc->mc_pg[0] = mc->mc_pg[1];
      mc->mc_ki[0] = mc->mc_ki[1];
      mc->mc_db->md_root = mp->mp_pgno;
      mc->mc_db->md_depth--;
      goto done;
    }
    mc->mc_snum++;
    mc->mc_top++;
    ptop = 0;
    if (mdbx_audit_enabled()) {
      rc = mdbx_cursor_check(mc, C_UPDATING);
      if (unlikely(rc != MDBX_SUCCESS))
        goto done;
    }
  } else {
    ptop = mc->mc_top - 1;
    mdbx_debug("parent branch page is %" PRIaPGNO, mc->mc_pg[ptop]->mp_pgno);
  }

  MDBX_cursor mn;
  cursor_copy(mc, &mn);
  mn.mc_pg[mn.mc_top] = sister;
  mn.mc_ki[mn.mc_top] = 0;
  mn.mc_ki[ptop] = mc->mc_ki[ptop] + 1;

  unsigned split_indx =
      (newindx < nkeys)
          ? /* split at the middle */ (nkeys + 1) / 2
          : /* split at the end (i.e. like append-mode ) */ nkeys - minkeys + 1;

  mdbx_cassert(mc, !IS_BRANCH(mp) || newindx > 0);
  /* It is reasonable and possible to split the page at the begin */
  if (unlikely(newindx < minkeys)) {
    split_indx = minkeys;
    if (newindx == 0 && foliage == 0 && !(nflags & MDBX_SPLIT_REPLACE)) {
      split_indx = 0;
      /* Checking for ability of splitting by the left-side insertion
       * of a pure page with the new key */
      for (i = 0; i < mc->mc_top; ++i)
        if (mc->mc_ki[i]) {
          get_key(page_node(mc->mc_pg[i], mc->mc_ki[i]), &sepkey);
          if (mc->mc_dbx->md_cmp(newkey, &sepkey) >= 0)
            split_indx = minkeys;
          break;
        }
      if (split_indx == 0) {
        /* Save the current first key which was omitted on the parent branch
         * page and should be updated if the new first entry will be added */
        if (IS_LEAF2(mp)) {
          sepkey.iov_len = mp->mp_leaf2_ksize;
          sepkey.iov_base = page_leaf2key(mp, 0, sepkey.iov_len);
        } else
          get_key(page_node(mp, 0), &sepkey);
        mdbx_cassert(mc, mc->mc_dbx->md_cmp(newkey, &sepkey) < 0);
        /* Avoiding rare complex cases of split the parent page */
        if (page_room(mn.mc_pg[ptop]) < branch_size(env, &sepkey))
          split_indx = minkeys;
      }
    }
  }

  const bool pure_right = split_indx == nkeys;
  const bool pure_left = split_indx == 0;
  if (unlikely(pure_right)) {
    /* newindx == split_indx == nkeys */
    mdbx_trace("no-split, but add new pure page at the %s", "right/after");
    mdbx_cassert(mc, newindx == nkeys && split_indx == nkeys && minkeys == 1);
    sepkey = *newkey;
  } else if (unlikely(pure_left)) {
    /* newindx == split_indx == 0 */
    mdbx_trace("no-split, but add new pure page at the %s", "left/before");
    mdbx_cassert(mc, newindx == 0 && split_indx == 0 && minkeys == 1);
    mdbx_trace("old-first-key is %s", DKEY_DEBUG(&sepkey));
  } else {
    if (IS_LEAF2(sister)) {
      char *split, *ins;
      unsigned lsize, rsize, ksize;
      /* Move half of the keys to the right sibling */
      const int x = mc->mc_ki[mc->mc_top] - split_indx;
      ksize = mc->mc_db->md_xsize;
      split = page_leaf2key(mp, split_indx, ksize);
      rsize = (nkeys - split_indx) * ksize;
      lsize = (nkeys - split_indx) * sizeof(indx_t);
      mdbx_cassert(mc, mp->mp_lower >= lsize);
      mp->mp_lower -= (indx_t)lsize;
      mdbx_cassert(mc, sister->mp_lower + lsize <= UINT16_MAX);
      sister->mp_lower += (indx_t)lsize;
      mdbx_cassert(mc, mp->mp_upper + rsize - lsize <= UINT16_MAX);
      mp->mp_upper += (indx_t)(rsize - lsize);
      mdbx_cassert(mc, sister->mp_upper >= rsize - lsize);
      sister->mp_upper -= (indx_t)(rsize - lsize);
      sepkey.iov_len = ksize;
      sepkey.iov_base = (newindx != split_indx) ? split : newkey->iov_base;
      if (x < 0) {
        mdbx_cassert(mc, ksize >= sizeof(indx_t));
        ins = page_leaf2key(mp, mc->mc_ki[mc->mc_top], ksize);
        memcpy(sister->mp_ptrs, split, rsize);
        sepkey.iov_base = sister->mp_ptrs;
        memmove(ins + ksize, ins, (split_indx - mc->mc_ki[mc->mc_top]) * ksize);
        memcpy(ins, newkey->iov_base, ksize);
        mdbx_cassert(mc, UINT16_MAX - mp->mp_lower >= (int)sizeof(indx_t));
        mp->mp_lower += sizeof(indx_t);
        mdbx_cassert(mc, mp->mp_upper >= ksize - sizeof(indx_t));
        mp->mp_upper -= (indx_t)(ksize - sizeof(indx_t));
      } else {
        memcpy(sister->mp_ptrs, split, x * ksize);
        ins = page_leaf2key(sister, x, ksize);
        memcpy(ins, newkey->iov_base, ksize);
        memcpy(ins + ksize, split + x * ksize, rsize - x * ksize);
        mdbx_cassert(mc, UINT16_MAX - sister->mp_lower >= (int)sizeof(indx_t));
        sister->mp_lower += sizeof(indx_t);
        mdbx_cassert(mc, sister->mp_upper >= ksize - sizeof(indx_t));
        sister->mp_upper -= (indx_t)(ksize - sizeof(indx_t));
        mdbx_cassert(mc, x <= (int)UINT16_MAX);
        mc->mc_ki[mc->mc_top] = (indx_t)x;
      }

      if (mdbx_audit_enabled()) {
        rc = mdbx_cursor_check(mc, C_UPDATING);
        if (unlikely(rc != MDBX_SUCCESS))
          goto done;
        rc = mdbx_cursor_check(&mn, C_UPDATING);
        if (unlikely(rc != MDBX_SUCCESS))
          goto done;
      }
    } else {
      /* Maximum free space in an empty page */
      const unsigned max_space = page_space(env);
      const size_t new_size = IS_LEAF(mp) ? leaf_size(env, newkey, newdata)
                                          : branch_size(env, newkey);

      /* grab a page to hold a temporary copy */
      tmp_ki_copy = mdbx_page_malloc(mc->mc_txn, 1);
      if (unlikely(tmp_ki_copy == NULL)) {
        rc = MDBX_ENOMEM;
        goto done;
      }

      /* prepare to insert */
      for (unsigned j = i = 0; i < nkeys; ++i, ++j) {
        tmp_ki_copy->mp_ptrs[j] = 0;
        j += (i == newindx);
        tmp_ki_copy->mp_ptrs[j] = mp->mp_ptrs[i];
      }
      tmp_ki_copy->mp_pgno = mp->mp_pgno;
      tmp_ki_copy->mp_flags = mp->mp_flags;
      tmp_ki_copy->mp_txnid = INVALID_TXNID;
      tmp_ki_copy->mp_lower = 0;
      tmp_ki_copy->mp_upper = (indx_t)max_space;

      /* When items are relatively large the split point needs
       * to be checked, because being off-by-one will make the
       * difference between success or failure in mdbx_node_add.
       *
       * It's also relevant if a page happens to be laid out
       * such that one half of its nodes are all "small" and
       * the other half of its nodes are "large". If the new
       * item is also "large" and falls on the half with
       * "large" nodes, it also may not fit.
       *
       * As a final tweak, if the new item goes on the last
       * spot on the page (and thus, onto the new page), bias
       * the split so the new page is emptier than the old page.
       * This yields better packing during sequential inserts. */

      if (nkeys < 32 || new_size > max_space / 16) {
        /* Find split point */
        int dir;
        if (newindx <= split_indx) {
          i = 0;
          dir = 1;
        } else {
          i = nkeys;
          dir = -1;
        }
        size_t before = 0, after = new_size + page_used(env, mp);
        int best = split_indx;
        int best_offset = nkeys + 1;

        mdbx_trace("seek separator from %u, step %i, default %u, new-idx %u, "
                   "new-size %zu",
                   i, dir, split_indx, newindx, new_size);
        do {
          mdbx_cassert(mc, i <= nkeys);
          size_t size = new_size;
          if (i != newindx) {
            MDBX_node *node =
                (MDBX_node *)((char *)mp + tmp_ki_copy->mp_ptrs[i] + PAGEHDRSZ);
            size = NODESIZE + node_ks(node) + sizeof(indx_t);
            if (IS_LEAF(mp))
              size += F_ISSET(node_flags(node), F_BIGDATA) ? sizeof(pgno_t)
                                                           : node_ds(node);
            size = EVEN(size);
          }

          before += size;
          after -= size;
          mdbx_trace("step %u, size %zu, before %zu, after %zu, max %u", i,
                     size, before, after, max_space);

          if (before <= max_space && after <= max_space) {
            int offset = branchless_abs(split_indx - i);
            if (offset >= best_offset)
              break;
            best_offset = offset;
            best = i;
          }
          i += dir;
        } while (i < nkeys);

        split_indx = best + (dir > 0);
        split_indx = (split_indx <= nkeys - minkeys + 1) ? split_indx
                                                         : nkeys - minkeys + 1;
        split_indx = (split_indx >= minkeys) ? split_indx : minkeys;
        mdbx_trace("chosen %u", split_indx);
      }

      sepkey.iov_len = newkey->iov_len;
      sepkey.iov_base = newkey->iov_base;
      if (split_indx != newindx) {
        MDBX_node *node =
            (MDBX_node *)((char *)mp + tmp_ki_copy->mp_ptrs[split_indx] +
                          PAGEHDRSZ);
        sepkey.iov_len = node_ks(node);
        sepkey.iov_base = node_key(node);
      }
    }
  }
  mdbx_debug("separator is %d [%s]", split_indx, DKEY_DEBUG(&sepkey));

  bool did_split_parent = false;
  /* Copy separator key to the parent. */
  if (page_room(mn.mc_pg[ptop]) < branch_size(env, &sepkey)) {
    mdbx_trace("need split parent branch-page for key %s", DKEY_DEBUG(&sepkey));
    mdbx_cassert(mc, page_numkeys(mn.mc_pg[ptop]) > 2);
    mdbx_cassert(mc, !pure_left);
    const int snum = mc->mc_snum;
    const int depth = mc->mc_db->md_depth;
    mn.mc_snum--;
    mn.mc_top--;
    did_split_parent = true;
    /* We want other splits to find mn when doing fixups */
    WITH_CURSOR_TRACKING(
        mn, rc = mdbx_page_split(&mn, &sepkey, NULL, sister->mp_pgno, 0));
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;
    mdbx_cassert(mc, (int)mc->mc_snum - snum == mc->mc_db->md_depth - depth);
    if (mdbx_audit_enabled()) {
      rc = mdbx_cursor_check(mc, C_UPDATING);
      if (unlikely(rc != MDBX_SUCCESS))
        goto done;
    }

    /* root split? */
    ptop += mc->mc_snum - snum;

    /* Right page might now have changed parent.
     * Check if left page also changed parent. */
    if (mn.mc_pg[ptop] != mc->mc_pg[ptop] &&
        mc->mc_ki[ptop] >= page_numkeys(mc->mc_pg[ptop])) {
      for (i = 0; i < ptop; i++) {
        mc->mc_pg[i] = mn.mc_pg[i];
        mc->mc_ki[i] = mn.mc_ki[i];
      }
      mc->mc_pg[ptop] = mn.mc_pg[ptop];
      if (mn.mc_ki[ptop]) {
        mc->mc_ki[ptop] = mn.mc_ki[ptop] - 1;
      } else {
        /* find right page's left sibling */
        mc->mc_ki[ptop] = mn.mc_ki[ptop];
        rc = mdbx_cursor_sibling(mc, SIBLING_LEFT);
        if (unlikely(rc != MDBX_SUCCESS)) {
          if (rc == MDBX_NOTFOUND) /* improper mdbx_cursor_sibling() result */ {
            mdbx_error("unexpected %i error going left sibling", rc);
            rc = MDBX_PROBLEM;
          }
          goto done;
        }
      }
    }
  } else if (unlikely(pure_left)) {
    MDBX_page *ptop_page = mc->mc_pg[ptop];
    mdbx_debug("adding to parent page %u node[%u] left-leaf page #%u key %s",
               ptop_page->mp_pgno, mc->mc_ki[ptop], sister->mp_pgno,
               DKEY(mc->mc_ki[ptop] ? newkey : NULL));
    mc->mc_top--;
    rc = mdbx_node_add_branch(mc, mc->mc_ki[ptop],
                              mc->mc_ki[ptop] ? newkey : NULL, sister->mp_pgno);
    mdbx_cassert(mc, mp == mc->mc_pg[ptop + 1] &&
                         newindx == mc->mc_ki[ptop + 1] && ptop == mc->mc_top);

    if (likely(rc == MDBX_SUCCESS) && mc->mc_ki[ptop] == 0) {
      mdbx_debug("update prev-first key on parent %s", DKEY(&sepkey));
      MDBX_node *node = page_node(mc->mc_pg[ptop], 1);
      mdbx_cassert(mc, node_ks(node) == 0 && node_pgno(node) == mp->mp_pgno);
      mdbx_cassert(mc, mc->mc_top == ptop && mc->mc_ki[ptop] == 0);
      mc->mc_ki[ptop] = 1;
      rc = mdbx_update_key(mc, &sepkey);
      mdbx_cassert(mc, mc->mc_top == ptop && mc->mc_ki[ptop] == 1);
      mdbx_cassert(mc,
                   mp == mc->mc_pg[ptop + 1] && newindx == mc->mc_ki[ptop + 1]);
      mc->mc_ki[ptop] = 0;
    }

    mc->mc_top++;
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;

    MDBX_node *node = page_node(mc->mc_pg[ptop], mc->mc_ki[ptop] + 1);
    mdbx_cassert(mc, node_pgno(node) == mp->mp_pgno &&
                         mc->mc_pg[ptop] == ptop_page);
  } else {
    mn.mc_top--;
    mdbx_trace("add-to-parent the right-entry[%u] for new sibling-page",
               mn.mc_ki[ptop]);
    rc = mdbx_node_add_branch(&mn, mn.mc_ki[ptop], &sepkey, sister->mp_pgno);
    mn.mc_top++;
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;
  }

  if (unlikely(pure_left | pure_right)) {
    mc->mc_pg[mc->mc_top] = sister;
    mc->mc_ki[mc->mc_top] = 0;
    switch (PAGETYPE(sister)) {
    case P_LEAF: {
      mdbx_cassert(mc, newpgno == 0 || newpgno == P_INVALID);
      rc = mdbx_node_add_leaf(mc, 0, newkey, newdata, nflags);
    } break;
    case P_LEAF | P_LEAF2: {
      mdbx_cassert(mc, (nflags & (F_BIGDATA | F_SUBDATA | F_DUPDATA)) == 0);
      mdbx_cassert(mc, newpgno == 0 || newpgno == P_INVALID);
      rc = mdbx_node_add_leaf2(mc, 0, newkey);
    } break;
    default:
      rc = bad_page(sister, "wrong page-type %u\n", PAGETYPE(sister));
    }
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;

    if (pure_right) {
      for (i = 0; i < mc->mc_top; i++)
        mc->mc_ki[i] = mn.mc_ki[i];
    } else if (mc->mc_ki[mc->mc_top - 1] == 0) {
      for (i = 2; i <= mc->mc_top; ++i)
        if (mc->mc_ki[mc->mc_top - i]) {
          get_key(
              page_node(mc->mc_pg[mc->mc_top - i], mc->mc_ki[mc->mc_top - i]),
              &sepkey);
          if (mc->mc_dbx->md_cmp(newkey, &sepkey) < 0) {
            mc->mc_top -= i;
            mdbx_debug("update new-first on parent [%i] page %u key %s",
                       mc->mc_ki[mc->mc_top], mc->mc_pg[mc->mc_top]->mp_pgno,
                       DKEY(newkey));
            rc = mdbx_update_key(mc, newkey);
            mc->mc_top += i;
            if (unlikely(rc != MDBX_SUCCESS))
              goto done;
          }
          break;
        }
    }
  } else if (!IS_LEAF2(mp)) {
    /* Move nodes */
    mc->mc_pg[mc->mc_top] = sister;
    i = split_indx;
    unsigned n = 0;
    pgno_t pgno = 0;
    do {
      mdbx_trace("i %u, nkeys %u => n %u, rp #%u", i, nkeys, n,
                 sister->mp_pgno);
      MDBX_val *rdata = NULL;
      if (i == newindx) {
        rkey.iov_base = newkey->iov_base;
        rkey.iov_len = newkey->iov_len;
        if (IS_LEAF(mp))
          rdata = newdata;
        else
          pgno = newpgno;
        flags = nflags;
        /* Update index for the new key. */
        mc->mc_ki[mc->mc_top] = (indx_t)n;
      } else {
        MDBX_node *node =
            (MDBX_node *)((char *)mp + tmp_ki_copy->mp_ptrs[i] + PAGEHDRSZ);
        rkey.iov_base = node_key(node);
        rkey.iov_len = node_ks(node);
        if (IS_LEAF(mp)) {
          xdata.iov_base = node_data(node);
          xdata.iov_len = node_ds(node);
          rdata = &xdata;
        } else
          pgno = node_pgno(node);
        flags = node_flags(node);
      }

      switch (PAGETYPE(sister)) {
      case P_BRANCH: {
        mdbx_cassert(mc, 0 == (uint16_t)flags);
        /* First branch index doesn't need key data. */
        rc = mdbx_node_add_branch(mc, n, n ? &rkey : NULL, pgno);
      } break;
      case P_LEAF: {
        mdbx_cassert(mc, pgno == 0);
        mdbx_cassert(mc, rdata != NULL);
        rc = mdbx_node_add_leaf(mc, n, &rkey, rdata, flags);
      } break;
      /* case P_LEAF | P_LEAF2: {
        mdbx_cassert(mc, (nflags & (F_BIGDATA | F_SUBDATA | F_DUPDATA)) == 0);
        mdbx_cassert(mc, gno == 0);
        rc = mdbx_node_add_leaf2(mc, n, &rkey);
      } break; */
      default:
        rc = bad_page(sister, "wrong page-type %u\n", PAGETYPE(sister));
      }
      if (unlikely(rc != MDBX_SUCCESS))
        goto done;

      ++n;
      if (++i > nkeys) {
        i = 0;
        n = 0;
        mc->mc_pg[mc->mc_top] = tmp_ki_copy;
        mdbx_trace("switch to mp #%u", tmp_ki_copy->mp_pgno);
      }
    } while (i != split_indx);

    mdbx_trace("i %u, nkeys %u, n %u, pgno #%u", i, nkeys, n,
               mc->mc_pg[mc->mc_top]->mp_pgno);

    nkeys = page_numkeys(tmp_ki_copy);
    for (i = 0; i < nkeys; i++)
      mp->mp_ptrs[i] = tmp_ki_copy->mp_ptrs[i];
    mp->mp_lower = tmp_ki_copy->mp_lower;
    mp->mp_upper = tmp_ki_copy->mp_upper;
    memcpy(page_node(mp, nkeys - 1), page_node(tmp_ki_copy, nkeys - 1),
           env->me_psize - tmp_ki_copy->mp_upper - PAGEHDRSZ);

    /* reset back to original page */
    if (newindx < split_indx) {
      mc->mc_pg[mc->mc_top] = mp;
    } else {
      mc->mc_pg[mc->mc_top] = sister;
      mc->mc_ki[ptop]++;
      /* Make sure mc_ki is still valid. */
      if (mn.mc_pg[ptop] != mc->mc_pg[ptop] &&
          mc->mc_ki[ptop] >= page_numkeys(mc->mc_pg[ptop])) {
        for (i = 0; i <= ptop; i++) {
          mc->mc_pg[i] = mn.mc_pg[i];
          mc->mc_ki[i] = mn.mc_ki[i];
        }
      }
    }
  } else if (newindx >= split_indx) {
    mc->mc_pg[mc->mc_top] = sister;
    mc->mc_ki[ptop]++;
    /* Make sure mc_ki is still valid. */
    if (mn.mc_pg[ptop] != mc->mc_pg[ptop] &&
        mc->mc_ki[ptop] >= page_numkeys(mc->mc_pg[ptop])) {
      for (i = 0; i <= ptop; i++) {
        mc->mc_pg[i] = mn.mc_pg[i];
        mc->mc_ki[i] = mn.mc_ki[i];
      }
    }
  }

  /* Adjust other cursors pointing to mp and/or to parent page */
  nkeys = page_numkeys(mp);
  for (MDBX_cursor *m2 = mc->mc_txn->tw.cursors[mc->mc_dbi]; m2;
       m2 = m2->mc_next) {
    MDBX_cursor *m3 = (mc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
    if (m3 == mc)
      continue;
    if (!(m2->mc_flags & m3->mc_flags & C_INITIALIZED))
      continue;
    if (foliage) {
      /* sub cursors may be on different DB */
      if (m3->mc_pg[0] != mp)
        continue;
      /* root split */
      for (int k = foliage; k >= 0; k--) {
        m3->mc_ki[k + 1] = m3->mc_ki[k];
        m3->mc_pg[k + 1] = m3->mc_pg[k];
      }
      m3->mc_ki[0] = (m3->mc_ki[0] >= nkeys) ? 1 : 0;
      m3->mc_pg[0] = mc->mc_pg[0];
      m3->mc_snum++;
      m3->mc_top++;
    }

    if (m3->mc_top >= mc->mc_top && m3->mc_pg[mc->mc_top] == mp && !pure_left) {
      if (m3->mc_ki[mc->mc_top] >= newindx && !(nflags & MDBX_SPLIT_REPLACE))
        m3->mc_ki[mc->mc_top]++;
      if (m3->mc_ki[mc->mc_top] >= nkeys) {
        m3->mc_pg[mc->mc_top] = sister;
        mdbx_cassert(mc, m3->mc_ki[mc->mc_top] >= nkeys);
        m3->mc_ki[mc->mc_top] -= (indx_t)nkeys;
        for (i = 0; i < mc->mc_top; i++) {
          m3->mc_ki[i] = mn.mc_ki[i];
          m3->mc_pg[i] = mn.mc_pg[i];
        }
      }
    } else if (!did_split_parent && m3->mc_top >= ptop &&
               m3->mc_pg[ptop] == mc->mc_pg[ptop] &&
               m3->mc_ki[ptop] >= mc->mc_ki[ptop]) {
      m3->mc_ki[ptop]++; /* also for the `pure-left` case */
    }
    if (XCURSOR_INITED(m3) && IS_LEAF(mp))
      XCURSOR_REFRESH(m3, m3->mc_pg[mc->mc_top], m3->mc_ki[mc->mc_top]);
  }
  mdbx_trace("mp #%u left: %d, sister #%u left: %d", mp->mp_pgno, page_room(mp),
             sister->mp_pgno, page_room(sister));

done:
  if (tmp_ki_copy)
    mdbx_dpage_free(env, tmp_ki_copy, 1);

  if (unlikely(rc != MDBX_SUCCESS))
    mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
  else {
    if (mdbx_audit_enabled())
      rc = mdbx_cursor_check(mc, C_UPDATING);
    if (unlikely(nflags & MDBX_RESERVE)) {
      MDBX_node *node = page_node(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
      if (!(node_flags(node) & F_BIGDATA))
        newdata->iov_base = node_data(node);
    }
#if MDBX_ENABLE_PGOP_STAT
    env->me_lck->mti_pgop_stat.split.weak += 1;
#endif /* MDBX_ENABLE_PGOP_STAT */
  }

  mdbx_debug("<< mp #%u, rc %d", mp->mp_pgno, rc);
  return rc;
}

int mdbx_put(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key, MDBX_val *data,
             unsigned flags) {
  int rc = check_txn_rw(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key || !data))
    return MDBX_EINVAL;

  if (unlikely(!check_dbi(txn, dbi, DBI_USRVALID)))
    return MDBX_BAD_DBI;

  if (unlikely(flags & ~(MDBX_NOOVERWRITE | MDBX_NODUPDATA | MDBX_ALLDUPS |
                         MDBX_ALLDUPS | MDBX_RESERVE | MDBX_APPEND |
                         MDBX_APPENDDUP | MDBX_CURRENT | MDBX_MULTIPLE)))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_flags & (MDBX_TXN_RDONLY | MDBX_TXN_BLOCKED)))
    return (txn->mt_flags & MDBX_TXN_RDONLY) ? MDBX_EACCESS : MDBX_BAD_TXN;

  MDBX_cursor_couple cx;
  rc = mdbx_cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  cx.outer.mc_next = txn->tw.cursors[dbi];
  txn->tw.cursors[dbi] = &cx.outer;

  /* LY: support for update (explicit overwrite) */
  if (flags & MDBX_CURRENT) {
    rc = mdbx_cursor_get(&cx.outer, (MDBX_val *)key, NULL, MDBX_SET);
    if (likely(rc == MDBX_SUCCESS) &&
        (txn->mt_dbs[dbi].md_flags & MDBX_DUPSORT) &&
        (flags & MDBX_ALLDUPS) == 0) {
      /* LY: allows update (explicit overwrite) only for unique keys */
      MDBX_node *node = page_node(cx.outer.mc_pg[cx.outer.mc_top],
                                  cx.outer.mc_ki[cx.outer.mc_top]);
      if (F_ISSET(node_flags(node), F_DUPDATA)) {
        mdbx_tassert(txn, XCURSOR_INITED(&cx.outer) &&
                              cx.outer.mc_xcursor->mx_db.md_entries > 1);
        rc = MDBX_EMULTIVAL;
      }
    }
  }

  if (likely(rc == MDBX_SUCCESS))
    rc = mdbx_cursor_put(&cx.outer, key, data, flags);
  txn->tw.cursors[dbi] = cx.outer.mc_next;

  return rc;
}

/**** COPYING *****************************************************************/

/* State needed for a double-buffering compacting copy. */
typedef struct mdbx_copy {
  MDBX_env *mc_env;
  MDBX_txn *mc_txn;
  mdbx_condpair_t mc_condpair;
  uint8_t *mc_wbuf[2];
  uint8_t *mc_over[2];
  size_t mc_wlen[2];
  size_t mc_olen[2];
  mdbx_filehandle_t mc_fd;
  /* Error code.  Never cleared if set.  Both threads can set nonzero
   * to fail the copy.  Not mutex-protected, MDBX expects atomic int. */
  volatile int mc_error;
  pgno_t mc_next_pgno;
  volatile unsigned mc_head;
  volatile unsigned mc_tail;
} mdbx_copy;

/* Dedicated writer thread for compacting copy. */
__cold static THREAD_RESULT THREAD_CALL mdbx_env_copythr(void *arg) {
  mdbx_copy *my = arg;

#if defined(EPIPE) && !(defined(_WIN32) || defined(_WIN64))
  sigset_t sigset;
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGPIPE);
  my->mc_error = pthread_sigmask(SIG_BLOCK, &sigset, NULL);
#endif /* EPIPE */

  mdbx_condpair_lock(&my->mc_condpair);
  while (!my->mc_error) {
    while (my->mc_tail == my->mc_head && !my->mc_error) {
      int err = mdbx_condpair_wait(&my->mc_condpair, true);
      if (err != MDBX_SUCCESS) {
        my->mc_error = err;
        goto bailout;
      }
    }
    const unsigned toggle = my->mc_tail & 1;
    size_t wsize = my->mc_wlen[toggle];
    if (wsize == 0) {
      my->mc_tail += 1;
      break /* EOF */;
    }
    my->mc_wlen[toggle] = 0;
    uint8_t *ptr = my->mc_wbuf[toggle];
  again:
    if (!my->mc_error) {
      int err = mdbx_write(my->mc_fd, ptr, wsize);
      if (err != MDBX_SUCCESS) {
#if defined(EPIPE) && !(defined(_WIN32) || defined(_WIN64))
        if (err == EPIPE) {
          /* Collect the pending SIGPIPE,
           * otherwise at least OS X gives it to the process on thread-exit. */
          int unused;
          sigwait(&sigset, &unused);
        }
#endif /* EPIPE */
        my->mc_error = err;
        goto bailout;
      }
    }

    /* If there's an overflow page tail, write it too */
    wsize = my->mc_olen[toggle];
    if (wsize) {
      my->mc_olen[toggle] = 0;
      ptr = my->mc_over[toggle];
      goto again;
    }
    my->mc_tail += 1;
    mdbx_condpair_signal(&my->mc_condpair, false);
  }
bailout:
  mdbx_condpair_unlock(&my->mc_condpair);
  return (THREAD_RESULT)0;
}

/* Give buffer and/or MDBX_EOF to writer thread, await unused buffer. */
__cold static int mdbx_env_cthr_toggle(mdbx_copy *my) {
  mdbx_condpair_lock(&my->mc_condpair);
  mdbx_assert(my->mc_env, my->mc_head - my->mc_tail < 2 || my->mc_error);
  my->mc_head += 1;
  mdbx_condpair_signal(&my->mc_condpair, true);
  while (!my->mc_error &&
         my->mc_head - my->mc_tail == 2 /* both buffers in use */) {
    int err = mdbx_condpair_wait(&my->mc_condpair, false);
    if (err != MDBX_SUCCESS)
      my->mc_error = err;
  }
  mdbx_condpair_unlock(&my->mc_condpair);
  return my->mc_error;
}

/* Depth-first tree traversal for compacting copy.
 * [in] my control structure.
 * [in,out] pg database root.
 * [in] flags includes F_DUPDATA if it is a sorted-duplicate sub-DB. */
__cold static int mdbx_env_cwalk(mdbx_copy *my, pgno_t *pg, int flags) {
  MDBX_cursor_couple couple;
  MDBX_page *mo, *mp, *leaf;
  char *buf, *ptr;
  int rc;
  unsigned i;

  /* Empty DB, nothing to do */
  if (*pg == P_INVALID)
    return MDBX_SUCCESS;

  memset(&couple, 0, sizeof(couple));
  couple.outer.mc_snum = 1;
  couple.outer.mc_txn = my->mc_txn;
  couple.outer.mc_flags = couple.inner.mx_cursor.mc_flags =
      C_COPYING | C_SKIPORD;

  rc = mdbx_page_get(&couple.outer, *pg, &couple.outer.mc_pg[0],
                     my->mc_txn->mt_txnid);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  rc = mdbx_page_search_root(&couple.outer, NULL, MDBX_PS_FIRST);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  /* Make cursor pages writable */
  buf = ptr = mdbx_malloc(pgno2bytes(my->mc_env, couple.outer.mc_snum));
  if (buf == NULL)
    return MDBX_ENOMEM;

  for (i = 0; i < couple.outer.mc_top; i++) {
    mdbx_page_copy((MDBX_page *)ptr, couple.outer.mc_pg[i],
                   my->mc_env->me_psize);
    couple.outer.mc_pg[i] = (MDBX_page *)ptr;
    ptr += my->mc_env->me_psize;
  }

  /* This is writable space for a leaf page. Usually not needed. */
  leaf = (MDBX_page *)ptr;

  while (couple.outer.mc_snum > 0) {
    mp = couple.outer.mc_pg[couple.outer.mc_top];
    unsigned n = page_numkeys(mp);

    if (IS_LEAF(mp)) {
      if (!IS_LEAF2(mp) && !(flags & F_DUPDATA)) {
        for (i = 0; i < n; i++) {
          MDBX_node *node = page_node(mp, i);
          if (node_flags(node) & F_BIGDATA) {
            MDBX_page *omp;

            /* Need writable leaf */
            if (mp != leaf) {
              couple.outer.mc_pg[couple.outer.mc_top] = leaf;
              mdbx_page_copy(leaf, mp, my->mc_env->me_psize);
              mp = leaf;
              node = page_node(mp, i);
            }

            const pgno_t pgno = node_largedata_pgno(node);
            poke_pgno(node_data(node), my->mc_next_pgno);
            rc = mdbx_page_get(&couple.outer, pgno, &omp,
                               pp_txnid4chk(mp, my->mc_txn));
            if (unlikely(rc != MDBX_SUCCESS))
              goto done;
            unsigned toggle = my->mc_head & 1;
            if (my->mc_wlen[toggle] + my->mc_env->me_psize >
                ((size_t)(MDBX_ENVCOPY_WRITEBUF))) {
              rc = mdbx_env_cthr_toggle(my);
              if (unlikely(rc != MDBX_SUCCESS))
                goto done;
              toggle = my->mc_head & 1;
            }
            mo = (MDBX_page *)(my->mc_wbuf[toggle] + my->mc_wlen[toggle]);
            memcpy(mo, omp, my->mc_env->me_psize);
            mo->mp_pgno = my->mc_next_pgno;
            my->mc_next_pgno += omp->mp_pages;
            my->mc_wlen[toggle] += my->mc_env->me_psize;
            if (omp->mp_pages > 1) {
              my->mc_olen[toggle] = pgno2bytes(my->mc_env, omp->mp_pages - 1);
              my->mc_over[toggle] = (uint8_t *)omp + my->mc_env->me_psize;
              rc = mdbx_env_cthr_toggle(my);
              if (unlikely(rc != MDBX_SUCCESS))
                goto done;
              toggle = my->mc_head & 1;
            }
          } else if (node_flags(node) & F_SUBDATA) {
            if (!MDBX_DISABLE_PAGECHECKS &&
                unlikely(node_ds(node) != sizeof(MDBX_db))) {
              rc = MDBX_CORRUPTED;
              goto done;
            }

            /* Need writable leaf */
            if (mp != leaf) {
              couple.outer.mc_pg[couple.outer.mc_top] = leaf;
              mdbx_page_copy(leaf, mp, my->mc_env->me_psize);
              mp = leaf;
              node = page_node(mp, i);
            }

            MDBX_db db;
            memcpy(&db, node_data(node), sizeof(MDBX_db));
            rc = mdbx_env_cwalk(my, &db.md_root, node_flags(node) & F_DUPDATA);
            if (rc)
              goto done;
            memcpy(node_data(node), &db, sizeof(MDBX_db));
          }
        }
      }
    } else {
      couple.outer.mc_ki[couple.outer.mc_top]++;
      if (couple.outer.mc_ki[couple.outer.mc_top] < n) {
      again:
        rc = mdbx_page_get(
            &couple.outer,
            node_pgno(page_node(mp, couple.outer.mc_ki[couple.outer.mc_top])),
            &mp, pp_txnid4chk(mp, my->mc_txn));
        if (unlikely(rc != MDBX_SUCCESS))
          goto done;
        couple.outer.mc_top++;
        couple.outer.mc_snum++;
        couple.outer.mc_ki[couple.outer.mc_top] = 0;
        if (IS_BRANCH(mp)) {
          /* Whenever we advance to a sibling branch page,
           * we must proceed all the way down to its first leaf. */
          mdbx_page_copy(couple.outer.mc_pg[couple.outer.mc_top], mp,
                         my->mc_env->me_psize);
          goto again;
        } else
          couple.outer.mc_pg[couple.outer.mc_top] = mp;
        continue;
      }
    }
    unsigned toggle = my->mc_head & 1;
    if (my->mc_wlen[toggle] + my->mc_wlen[toggle] >
        ((size_t)(MDBX_ENVCOPY_WRITEBUF))) {
      rc = mdbx_env_cthr_toggle(my);
      if (unlikely(rc != MDBX_SUCCESS))
        goto done;
      toggle = my->mc_head & 1;
    }
    mo = (MDBX_page *)(my->mc_wbuf[toggle] + my->mc_wlen[toggle]);
    mdbx_page_copy(mo, mp, my->mc_env->me_psize);
    mo->mp_pgno = my->mc_next_pgno++;
    my->mc_wlen[toggle] += my->mc_env->me_psize;
    if (couple.outer.mc_top) {
      /* Update parent if there is one */
      node_set_pgno(page_node(couple.outer.mc_pg[couple.outer.mc_top - 1],
                              couple.outer.mc_ki[couple.outer.mc_top - 1]),
                    mo->mp_pgno);
      mdbx_cursor_pop(&couple.outer);
    } else {
      /* Otherwise we're done */
      *pg = mo->mp_pgno;
      break;
    }
  }
done:
  mdbx_free(buf);
  return rc;
}

__cold static void compact_fixup_meta(MDBX_env *env, MDBX_meta *meta) {
  /* Calculate filesize taking in account shrink/growing thresholds */
  if (meta->mm_geo.next != meta->mm_geo.now) {
    meta->mm_geo.now = meta->mm_geo.next;
    const pgno_t aligner = pv2pages(
        meta->mm_geo.grow_pv ? meta->mm_geo.grow_pv : meta->mm_geo.shrink_pv);
    if (aligner) {
      const pgno_t aligned = pgno_align2os_pgno(
          env, meta->mm_geo.next + aligner - meta->mm_geo.next % aligner);
      meta->mm_geo.now = aligned;
    }
  }

  if (meta->mm_geo.now < meta->mm_geo.lower)
    meta->mm_geo.now = meta->mm_geo.lower;
  if (meta->mm_geo.now > meta->mm_geo.upper)
    meta->mm_geo.now = meta->mm_geo.upper;

  /* Update signature */
  assert(meta->mm_geo.now >= meta->mm_geo.next);
  unaligned_poke_u64(4, meta->mm_datasync_sign, mdbx_meta_sign(meta));
}

/* Make resizeable */
__cold static void make_sizeable(MDBX_meta *meta) {
  meta->mm_geo.lower = MIN_PAGENO;
  if (meta->mm_geo.grow_pv == 0) {
    const pgno_t step = 1 + (meta->mm_geo.upper - meta->mm_geo.lower) / 42;
    meta->mm_geo.grow_pv = pages2pv(step);
  }
  if (meta->mm_geo.shrink_pv == 0) {
    const pgno_t step = pv2pages(meta->mm_geo.grow_pv) << 1;
    meta->mm_geo.shrink_pv = pages2pv(step);
  }
}

/* Copy environment with compaction. */
__cold static int mdbx_env_compact(MDBX_env *env, MDBX_txn *read_txn,
                                   mdbx_filehandle_t fd, uint8_t *buffer,
                                   const bool dest_is_pipe, const int flags) {
  const size_t meta_bytes = pgno2bytes(env, NUM_METAS);
  uint8_t *const data_buffer =
      buffer + ceil_powerof2(meta_bytes, env->me_os_psize);
  MDBX_meta *const meta = mdbx_init_metas(env, buffer);
  mdbx_meta_set_txnid(env, meta, read_txn->mt_txnid);

  if (flags & MDBX_CP_FORCE_DYNAMIC_SIZE)
    make_sizeable(meta);

  /* copy canary sequences if present */
  if (read_txn->mt_canary.v) {
    meta->mm_canary = read_txn->mt_canary;
    meta->mm_canary.v = mdbx_meta_txnid_stable(env, meta);
  }

  /* Set metapage 1 with current main DB */
  pgno_t new_root, root = read_txn->mt_dbs[MAIN_DBI].md_root;
  if ((new_root = root) == P_INVALID) {
    /* When the DB is empty, handle it specially to
     * fix any breakage like page leaks from ITS#8174. */
    meta->mm_dbs[MAIN_DBI].md_flags = read_txn->mt_dbs[MAIN_DBI].md_flags;
    compact_fixup_meta(env, meta);
    if (dest_is_pipe) {
      int rc = mdbx_write(fd, buffer, meta_bytes);
      if (rc != MDBX_SUCCESS)
        return rc;
    }
  } else {
    /* Count free pages + GC pages.  Subtract from last_pg
     * to find the new last_pg, which also becomes the new root. */
    pgno_t freecount = 0;
    MDBX_cursor_couple couple;
    MDBX_val key, data;

    int rc = mdbx_cursor_init(&couple.outer, read_txn, FREE_DBI);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    while ((rc = mdbx_cursor_get(&couple.outer, &key, &data, MDBX_NEXT)) == 0)
      freecount += *(pgno_t *)data.iov_base;
    if (unlikely(rc != MDBX_NOTFOUND))
      return rc;

    freecount += read_txn->mt_dbs[FREE_DBI].md_branch_pages +
                 read_txn->mt_dbs[FREE_DBI].md_leaf_pages +
                 read_txn->mt_dbs[FREE_DBI].md_overflow_pages;

    new_root = read_txn->mt_next_pgno - 1 - freecount;
    meta->mm_geo.next = new_root + 1;
    meta->mm_dbs[MAIN_DBI] = read_txn->mt_dbs[MAIN_DBI];
    meta->mm_dbs[MAIN_DBI].md_root = new_root;

    mdbx_copy ctx;
    memset(&ctx, 0, sizeof(ctx));
    rc = mdbx_condpair_init(&ctx.mc_condpair);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;

    memset(data_buffer, 0, ((size_t)(MDBX_ENVCOPY_WRITEBUF)) * 2);
    ctx.mc_wbuf[0] = data_buffer;
    ctx.mc_wbuf[1] = data_buffer + ((size_t)(MDBX_ENVCOPY_WRITEBUF));
    ctx.mc_next_pgno = NUM_METAS;
    ctx.mc_env = env;
    ctx.mc_fd = fd;
    ctx.mc_txn = read_txn;

    mdbx_thread_t thread;
    int thread_err = mdbx_thread_create(&thread, mdbx_env_copythr, &ctx);
    if (likely(thread_err == MDBX_SUCCESS)) {
      if (dest_is_pipe) {
        compact_fixup_meta(env, meta);
        rc = mdbx_write(fd, buffer, meta_bytes);
      }
      if (rc == MDBX_SUCCESS)
        rc = mdbx_env_cwalk(&ctx, &root, 0);
      mdbx_env_cthr_toggle(&ctx);
      mdbx_env_cthr_toggle(&ctx);
      thread_err = mdbx_thread_join(thread);
      mdbx_assert(env, (ctx.mc_tail == ctx.mc_head &&
                        ctx.mc_wlen[ctx.mc_head & 1] == 0) ||
                           ctx.mc_error);
      mdbx_condpair_destroy(&ctx.mc_condpair);
    }
    if (unlikely(thread_err != MDBX_SUCCESS))
      return thread_err;
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    if (unlikely(ctx.mc_error != MDBX_SUCCESS))
      return ctx.mc_error;

    if (dest_is_pipe) {
      if (unlikely(root != new_root)) {
        mdbx_error("post-compactification root %" PRIaPGNO
                   " NE expected %" PRIaPGNO
                   " (source DB corrupted or has a page leak(s))",
                   root, new_root);
        return MDBX_CORRUPTED; /* page leak or corrupt DB */
      }
    } else {
      if (unlikely(root > new_root)) {
        mdbx_error("post-compactification root %" PRIaPGNO
                   " GT expected %" PRIaPGNO " (source DB corrupted)",
                   root, new_root);
        return MDBX_CORRUPTED; /* page leak or corrupt DB */
      }
      if (unlikely(root < new_root)) {
        mdbx_warning("post-compactification root %" PRIaPGNO
                     " LT expected %" PRIaPGNO " (page leak(s) in source DB)",
                     root, new_root);
        /* fixup meta */
        meta->mm_dbs[MAIN_DBI].md_root = root;
        meta->mm_geo.next = root + 1;
      }
      compact_fixup_meta(env, meta);
    }
  }

  /* Extend file if required */
  if (meta->mm_geo.now != meta->mm_geo.next) {
    const size_t whole_size = pgno2bytes(env, meta->mm_geo.now);
    if (!dest_is_pipe)
      return mdbx_ftruncate(fd, whole_size);

    const size_t used_size = pgno2bytes(env, meta->mm_geo.next);
    memset(data_buffer, 0, ((size_t)(MDBX_ENVCOPY_WRITEBUF)));
    for (size_t offset = used_size; offset < whole_size;) {
      const size_t chunk =
          (((size_t)(MDBX_ENVCOPY_WRITEBUF)) < whole_size - offset)
              ? ((size_t)(MDBX_ENVCOPY_WRITEBUF))
              : whole_size - offset;
      /* copy to avoid EFAULT in case swapped-out */
      int rc = mdbx_write(fd, data_buffer, chunk);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      offset += chunk;
    }
  }
  return MDBX_SUCCESS;
}

/* Copy environment as-is. */
__cold static int mdbx_env_copy_asis(MDBX_env *env, MDBX_txn *read_txn,
                                     mdbx_filehandle_t fd, uint8_t *buffer,
                                     const bool dest_is_pipe, const int flags) {
  /* We must start the actual read txn after blocking writers */
  int rc = mdbx_txn_end(read_txn, MDBX_END_RESET_TMP);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  /* Temporarily block writers until we snapshot the meta pages */
  rc = mdbx_txn_lock(env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = mdbx_txn_renew0(read_txn, MDBX_TXN_RDONLY);
  if (unlikely(rc != MDBX_SUCCESS)) {
    mdbx_txn_unlock(env);
    return rc;
  }

  mdbx_jitter4testing(false);
  const size_t meta_bytes = pgno2bytes(env, NUM_METAS);
  /* Make a snapshot of meta-pages,
   * but writing ones after the data was flushed */
  memcpy(buffer, env->me_map, meta_bytes);
  MDBX_meta *const headcopy = /* LY: get pointer to the snapshot copy */
      (MDBX_meta *)(buffer + ((uint8_t *)mdbx_meta_head(env) - env->me_map));
  mdbx_txn_unlock(env);

  if (flags & MDBX_CP_FORCE_DYNAMIC_SIZE)
    make_sizeable(headcopy);
  /* Update signature to steady */
  unaligned_poke_u64(4, headcopy->mm_datasync_sign, mdbx_meta_sign(headcopy));

  /* Copy the data */
  const size_t whole_size = pgno_align2os_bytes(env, read_txn->mt_end_pgno);
  const size_t used_size = pgno2bytes(env, read_txn->mt_next_pgno);
  mdbx_jitter4testing(false);

  if (dest_is_pipe)
    rc = mdbx_write(fd, buffer, meta_bytes);

  uint8_t *const data_buffer =
      buffer + ceil_powerof2(meta_bytes, env->me_os_psize);
#if MDBX_USE_COPYFILERANGE
  static bool copyfilerange_unavailable;
  bool not_the_same_filesystem = false;
#endif /* MDBX_USE_COPYFILERANGE */
  for (size_t offset = meta_bytes; rc == MDBX_SUCCESS && offset < used_size;) {
#if MDBX_USE_SENDFILE
    static bool sendfile_unavailable;
    if (dest_is_pipe && likely(!sendfile_unavailable)) {
      off_t in_offset = offset;
      const ssize_t written =
          sendfile(fd, env->me_lazy_fd, &in_offset, used_size - offset);
      if (likely(written > 0)) {
        offset = in_offset;
        continue;
      }
      rc = MDBX_ENODATA;
      if (written == 0 || ignore_enosys(rc = errno) != MDBX_RESULT_TRUE)
        break;
      sendfile_unavailable = true;
    }
#endif /* MDBX_USE_SENDFILE */

#if MDBX_USE_COPYFILERANGE
    if (!dest_is_pipe && !not_the_same_filesystem &&
        likely(!copyfilerange_unavailable)) {
      off_t in_offset = offset, out_offset = offset;
      ssize_t bytes_copied = copy_file_range(
          env->me_lazy_fd, &in_offset, fd, &out_offset, used_size - offset, 0);
      if (likely(bytes_copied > 0)) {
        offset = in_offset;
        continue;
      }
      rc = MDBX_ENODATA;
      if (bytes_copied == 0)
        break;
      rc = errno;
      if (rc == EXDEV)
        not_the_same_filesystem = true;
      else if (ignore_enosys(rc) == MDBX_RESULT_TRUE)
        copyfilerange_unavailable = true;
      else
        break;
    }
#endif /* MDBX_USE_COPYFILERANGE */

    /* fallback to portable */
    const size_t chunk =
        (((size_t)(MDBX_ENVCOPY_WRITEBUF)) < used_size - offset)
            ? ((size_t)(MDBX_ENVCOPY_WRITEBUF))
            : used_size - offset;
    /* copy to avoid EFAULT in case swapped-out */
    memcpy(data_buffer, env->me_map + offset, chunk);
    rc = mdbx_write(fd, data_buffer, chunk);
    offset += chunk;
  }

  /* Extend file if required */
  if (likely(rc == MDBX_SUCCESS) && whole_size != used_size) {
    if (!dest_is_pipe)
      rc = mdbx_ftruncate(fd, whole_size);
    else {
      memset(data_buffer, 0, ((size_t)(MDBX_ENVCOPY_WRITEBUF)));
      for (size_t offset = used_size;
           rc == MDBX_SUCCESS && offset < whole_size;) {
        const size_t chunk =
            (((size_t)(MDBX_ENVCOPY_WRITEBUF)) < whole_size - offset)
                ? ((size_t)(MDBX_ENVCOPY_WRITEBUF))
                : whole_size - offset;
        /* copy to avoid EFAULT in case swapped-out */
        rc = mdbx_write(fd, data_buffer, chunk);
        offset += chunk;
      }
    }
  }

  return rc;
}

__cold int mdbx_env_copy2fd(MDBX_env *env, mdbx_filehandle_t fd,
                            unsigned flags) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const int dest_is_pipe = mdbx_is_pipe(fd);
  if (MDBX_IS_ERROR(dest_is_pipe))
    return dest_is_pipe;

  if (!dest_is_pipe) {
    rc = mdbx_fseek(fd, 0);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  const size_t buffer_size =
      pgno_align2os_bytes(env, NUM_METAS) +
      ceil_powerof2(((flags & MDBX_CP_COMPACT)
                         ? ((size_t)(MDBX_ENVCOPY_WRITEBUF)) * 2
                         : ((size_t)(MDBX_ENVCOPY_WRITEBUF))),
                    env->me_os_psize);

  uint8_t *buffer = NULL;
  rc = mdbx_memalign_alloc(env->me_os_psize, buffer_size, (void **)&buffer);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  MDBX_txn *read_txn = NULL;
  /* Do the lock/unlock of the reader mutex before starting the
   * write txn. Otherwise other read txns could block writers. */
  rc = mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &read_txn);
  if (unlikely(rc != MDBX_SUCCESS)) {
    mdbx_memalign_free(buffer);
    return rc;
  }

  if (!dest_is_pipe) {
    /* Firstly write a stub to meta-pages.
     * Now we sure to incomplete copy will not be used. */
    memset(buffer, -1, pgno2bytes(env, NUM_METAS));
    rc = mdbx_write(fd, buffer, pgno2bytes(env, NUM_METAS));
  }

  if (likely(rc == MDBX_SUCCESS)) {
    memset(buffer, 0, pgno2bytes(env, NUM_METAS));
    rc = ((flags & MDBX_CP_COMPACT) ? mdbx_env_compact : mdbx_env_copy_asis)(
        env, read_txn, fd, buffer, dest_is_pipe, flags);
  }
  mdbx_txn_abort(read_txn);

  if (!dest_is_pipe) {
    if (likely(rc == MDBX_SUCCESS))
      rc = mdbx_fsync(fd, MDBX_SYNC_DATA | MDBX_SYNC_SIZE);

    /* Write actual meta */
    if (likely(rc == MDBX_SUCCESS))
      rc = mdbx_pwrite(fd, buffer, pgno2bytes(env, NUM_METAS), 0);

    if (likely(rc == MDBX_SUCCESS))
      rc = mdbx_fsync(fd, MDBX_SYNC_DATA | MDBX_SYNC_IODQ);
  }

  mdbx_memalign_free(buffer);
  return rc;
}

__cold int mdbx_env_copy(MDBX_env *env, const char *dest_path,
                         MDBX_copy_flags_t flags) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!dest_path))
    return MDBX_EINVAL;

  /* The destination path must exist, but the destination file must not.
   * We don't want the OS to cache the writes, since the source data is
   * already in the OS cache. */
  mdbx_filehandle_t newfd;
  rc = mdbx_openfile(MDBX_OPEN_COPY, env, dest_path, &newfd,
#if defined(_WIN32) || defined(_WIN64)
                     (mdbx_mode_t)-1
#else
                     S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP
#endif
  );

  if (rc == MDBX_SUCCESS) {
#if defined(_WIN32) || defined(_WIN64)
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    if (!LockFileEx(newfd, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 0, INT32_MAX, &ov))
      rc = GetLastError();
#else
    struct flock lock_op;
    memset(&lock_op, 0, sizeof(lock_op));
    lock_op.l_type = F_WRLCK;
    lock_op.l_whence = SEEK_SET;
    lock_op.l_start = 0;
    lock_op.l_len =
        (sizeof(lock_op.l_len) > 4 ? INT64_MAX : INT32_MAX) & ~(size_t)0xffff;
    if (fcntl(newfd, F_SETLK, &lock_op)
#if (defined(__linux__) || defined(__gnu_linux__)) && defined(LOCK_EX) &&      \
    (!defined(__ANDROID_API__) || __ANDROID_API__ >= 24)
        || flock(newfd, LOCK_EX | LOCK_NB)
#endif /* Linux */
    )
      rc = errno;
#endif /* Windows / POSIX */
  }

  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_copy2fd(env, newfd, flags);

  if (newfd != INVALID_HANDLE_VALUE) {
    int err = mdbx_closefile(newfd);
    if (rc == MDBX_SUCCESS && err != rc)
      rc = err;
    if (rc != MDBX_SUCCESS)
      (void)mdbx_removefile(dest_path);
  }

  return rc;
}

/******************************************************************************/

__cold int mdbx_env_set_flags(MDBX_env *env, MDBX_env_flags_t flags,
                              bool onoff) {
  int rc = check_env(env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(flags &
               ((env->me_flags & MDBX_ENV_ACTIVE) ? ~ENV_CHANGEABLE_FLAGS
                                                  : ~ENV_USABLE_FLAGS)))
    return MDBX_EPERM;

  if (unlikely(env->me_flags & MDBX_RDONLY))
    return MDBX_EACCESS;

  if ((env->me_flags & MDBX_ENV_ACTIVE) &&
      unlikely(env->me_txn0->mt_owner == mdbx_thread_self()))
    return MDBX_BUSY;

  const bool lock_needed = (env->me_flags & MDBX_ENV_ACTIVE) &&
                           env->me_txn0->mt_owner != mdbx_thread_self();
  bool should_unlock = false;
  if (lock_needed) {
    rc = mdbx_txn_lock(env, false);
    if (unlikely(rc))
      return rc;
    should_unlock = true;
  }

  if (onoff)
    env->me_flags = merge_sync_flags(env->me_flags, flags);
  else
    env->me_flags &= ~flags;

  if (should_unlock)
    mdbx_txn_unlock(env);
  return MDBX_SUCCESS;
}

__cold int mdbx_env_get_flags(const MDBX_env *env, unsigned *arg) {
  int rc = check_env(env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!arg))
    return MDBX_EINVAL;

  *arg = env->me_flags & ENV_USABLE_FLAGS;
  return MDBX_SUCCESS;
}

__cold int mdbx_env_set_userctx(MDBX_env *env, void *ctx) {
  int rc = check_env(env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  env->me_userctx = ctx;
  return MDBX_SUCCESS;
}

__cold void *mdbx_env_get_userctx(const MDBX_env *env) {
  return env ? env->me_userctx : NULL;
}

__cold int mdbx_env_set_assert(MDBX_env *env, MDBX_assert_func *func) {
  int rc = check_env(env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

#if MDBX_DEBUG
  env->me_assert_func = func;
  return MDBX_SUCCESS;
#else
  (void)func;
  return MDBX_ENOSYS;
#endif
}

__cold int mdbx_env_get_path(const MDBX_env *env, const char **arg) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!arg))
    return MDBX_EINVAL;

  *arg = env->me_pathname;
  return MDBX_SUCCESS;
}

__cold int mdbx_env_get_fd(const MDBX_env *env, mdbx_filehandle_t *arg) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!arg))
    return MDBX_EINVAL;

  *arg = env->me_lazy_fd;
  return MDBX_SUCCESS;
}

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
__cold int mdbx_env_stat(const MDBX_env *env, MDBX_stat *stat, size_t bytes) {
  return __inline_mdbx_env_stat(env, stat, bytes);
}
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

static void stat_get(const MDBX_db *db, MDBX_stat *st, size_t bytes) {
  st->ms_depth = db->md_depth;
  st->ms_branch_pages = db->md_branch_pages;
  st->ms_leaf_pages = db->md_leaf_pages;
  st->ms_overflow_pages = db->md_overflow_pages;
  st->ms_entries = db->md_entries;
  if (likely(bytes >=
             offsetof(MDBX_stat, ms_mod_txnid) + sizeof(st->ms_mod_txnid)))
    st->ms_mod_txnid = db->md_mod_txnid;
}

static void stat_add(const MDBX_db *db, MDBX_stat *const st,
                     const size_t bytes) {
  st->ms_depth += db->md_depth;
  st->ms_branch_pages += db->md_branch_pages;
  st->ms_leaf_pages += db->md_leaf_pages;
  st->ms_overflow_pages += db->md_overflow_pages;
  st->ms_entries += db->md_entries;
  if (likely(bytes >=
             offsetof(MDBX_stat, ms_mod_txnid) + sizeof(st->ms_mod_txnid)))
    st->ms_mod_txnid = (st->ms_mod_txnid > db->md_mod_txnid) ? st->ms_mod_txnid
                                                             : db->md_mod_txnid;
}

__cold static int stat_acc(const MDBX_txn *txn, MDBX_stat *st, size_t bytes) {
  int err = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  st->ms_psize = txn->mt_env->me_psize;
#if 1
  /* assuming GC is internal and not subject for accounting */
  stat_get(&txn->mt_dbs[MAIN_DBI], st, bytes);
#else
  stat_get(&txn->mt_dbs[FREE_DBI], st, bytes);
  stat_add(&txn->mt_dbs[MAIN_DBI], st, bytes);
#endif

  /* account opened named subDBs */
  for (MDBX_dbi dbi = CORE_DBS; dbi < txn->mt_numdbs; dbi++)
    if ((txn->mt_dbistate[dbi] & (DBI_VALID | DBI_STALE)) == DBI_VALID)
      stat_add(txn->mt_dbs + dbi, st, bytes);

  if (!(txn->mt_dbs[MAIN_DBI].md_flags & (MDBX_DUPSORT | MDBX_INTEGERKEY)) &&
      txn->mt_dbs[MAIN_DBI].md_entries /* TODO: use `md_subs` field */) {
    MDBX_cursor_couple cx;
    err = mdbx_cursor_init(&cx.outer, (MDBX_txn *)txn, MAIN_DBI);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    /* scan and account not opened named subDBs */
    err = mdbx_page_search(&cx.outer, NULL, MDBX_PS_FIRST);
    while (err == MDBX_SUCCESS) {
      const MDBX_page *mp = cx.outer.mc_pg[cx.outer.mc_top];
      for (unsigned i = 0; i < page_numkeys(mp); i++) {
        const MDBX_node *node = page_node(mp, i);
        if (node_flags(node) != F_SUBDATA)
          continue;
        if (unlikely(node_ds(node) != sizeof(MDBX_db)))
          return MDBX_CORRUPTED;

        /* skip opened and already accounted */
        for (MDBX_dbi dbi = CORE_DBS; dbi < txn->mt_numdbs; dbi++)
          if ((txn->mt_dbistate[dbi] & (DBI_VALID | DBI_STALE)) == DBI_VALID &&
              node_ks(node) == txn->mt_dbxs[dbi].md_name.iov_len &&
              memcmp(node_key(node), txn->mt_dbxs[dbi].md_name.iov_base,
                     node_ks(node)) == 0) {
            node = NULL;
            break;
          }

        if (node) {
          MDBX_db db;
          memcpy(&db, node_data(node), sizeof(db));
          stat_add(&db, st, bytes);
        }
      }
      err = mdbx_cursor_sibling(&cx.outer, SIBLING_RIGHT);
    }
    if (unlikely(err != MDBX_NOTFOUND))
      return err;
  }

  return MDBX_SUCCESS;
}

__cold int mdbx_env_stat_ex(const MDBX_env *env, const MDBX_txn *txn,
                            MDBX_stat *dest, size_t bytes) {
  if (unlikely(!dest))
    return MDBX_EINVAL;
  const size_t size_before_modtxnid = offsetof(MDBX_stat, ms_mod_txnid);
  if (unlikely(bytes != sizeof(MDBX_stat)) && bytes != size_before_modtxnid)
    return MDBX_EINVAL;

  if (likely(txn)) {
    if (env && unlikely(txn->mt_env != env))
      return MDBX_EINVAL;
    return stat_acc(txn, dest, bytes);
  }

  int err = check_env(env, true);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  if (env->me_txn0 && env->me_txn0->mt_owner == mdbx_thread_self())
    /* inside write-txn */
    return stat_acc(env->me_txn, dest, bytes);

  MDBX_txn *tmp_txn;
  err = mdbx_txn_begin((MDBX_env *)env, NULL, MDBX_TXN_RDONLY, &tmp_txn);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  const int rc = stat_acc(tmp_txn, dest, bytes);
  err = mdbx_txn_abort(tmp_txn);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  return rc;
}

__cold int mdbx_dbi_dupsort_depthmask(MDBX_txn *txn, MDBX_dbi dbi,
                                      uint32_t *mask) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!mask))
    return MDBX_EINVAL;

  if (unlikely(!check_dbi(txn, dbi, DBI_VALID)))
    return MDBX_BAD_DBI;

  MDBX_cursor_couple cx;
  rc = mdbx_cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  if ((cx.outer.mc_db->md_flags & MDBX_DUPSORT) == 0)
    return MDBX_RESULT_TRUE;

  MDBX_val key, data;
  rc = mdbx_cursor_first(&cx.outer, &key, &data);
  *mask = 0;
  while (rc == MDBX_SUCCESS) {
    const MDBX_node *node = page_node(cx.outer.mc_pg[cx.outer.mc_top],
                                      cx.outer.mc_ki[cx.outer.mc_top]);
    const MDBX_db *db = node_data(node);
    const unsigned flags = node_flags(node);
    switch (flags) {
    case F_BIGDATA:
    case 0:
      /* single-value entry, deep = 0 */
      *mask |= 1 << 0;
      break;
    case F_DUPDATA:
      /* single sub-page, deep = 1 */
      *mask |= 1 << 1;
      break;
    case F_DUPDATA | F_SUBDATA:
      /* sub-tree */
      *mask |= 1 << UNALIGNED_PEEK_16(db, MDBX_db, md_depth);
      break;
    default:
      mdbx_error("wrong node-flags %u", flags);
      return MDBX_CORRUPTED;
    }
    rc = mdbx_cursor_next(&cx.outer, &key, &data, MDBX_NEXT_NODUP);
  }

  return (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
}

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
__cold int mdbx_env_info(const MDBX_env *env, MDBX_envinfo *info,
                         size_t bytes) {
  return __inline_mdbx_env_info(env, info, bytes);
}
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

__cold static int fetch_envinfo_ex(const MDBX_env *env, const MDBX_txn *txn,
                                   MDBX_envinfo *arg, const size_t bytes) {

  const size_t size_before_bootid = offsetof(MDBX_envinfo, mi_bootid);
  const size_t size_before_pgop_stat = offsetof(MDBX_envinfo, mi_pgop_stat);

  /* is the environment open? (https://github.com/erthink/libmdbx/issues/171) */
  if (unlikely(!env->me_map)) {
    /* environment not yet opened */
#if 1
    /* default behavior: returns the available info but zeroed the rest */
    memset(arg, 0, bytes);
    arg->mi_geo.lower = env->me_dbgeo.lower;
    arg->mi_geo.upper = env->me_dbgeo.upper;
    arg->mi_geo.shrink = env->me_dbgeo.shrink;
    arg->mi_geo.grow = env->me_dbgeo.grow;
    arg->mi_geo.current = env->me_dbgeo.now;
    arg->mi_maxreaders = env->me_maxreaders;
    arg->mi_dxb_pagesize = env->me_psize;
    arg->mi_sys_pagesize = env->me_os_psize;
    if (likely(bytes > size_before_bootid)) {
      arg->mi_bootid.current.x = bootid.x;
      arg->mi_bootid.current.y = bootid.y;
    }
    return MDBX_SUCCESS;
#else
    /* some users may prefer this behavior: return appropriate error */
    return MDBX_EPERM;
#endif
  }

  const MDBX_meta *const meta0 = METAPAGE(env, 0);
  const MDBX_meta *const meta1 = METAPAGE(env, 1);
  const MDBX_meta *const meta2 = METAPAGE(env, 2);
  if (unlikely(env->me_flags & MDBX_FATAL_ERROR))
    return MDBX_PANIC;

  const MDBX_meta *const recent_meta = mdbx_meta_head(env);
  arg->mi_recent_txnid = mdbx_meta_txnid_fluid(env, recent_meta);
  arg->mi_meta0_txnid = mdbx_meta_txnid_fluid(env, meta0);
  arg->mi_meta0_sign = unaligned_peek_u64(4, meta0->mm_datasync_sign);
  arg->mi_meta1_txnid = mdbx_meta_txnid_fluid(env, meta1);
  arg->mi_meta1_sign = unaligned_peek_u64(4, meta1->mm_datasync_sign);
  arg->mi_meta2_txnid = mdbx_meta_txnid_fluid(env, meta2);
  arg->mi_meta2_sign = unaligned_peek_u64(4, meta2->mm_datasync_sign);
  if (likely(bytes > size_before_bootid)) {
    memcpy(&arg->mi_bootid.meta0, &meta0->mm_bootid, 16);
    memcpy(&arg->mi_bootid.meta1, &meta1->mm_bootid, 16);
    memcpy(&arg->mi_bootid.meta2, &meta2->mm_bootid, 16);
  }

  const MDBX_meta *txn_meta = recent_meta;
  arg->mi_last_pgno = txn_meta->mm_geo.next - 1;
  arg->mi_geo.current = pgno2bytes(env, txn_meta->mm_geo.now);
  if (txn) {
    arg->mi_last_pgno = txn->mt_next_pgno - 1;
    arg->mi_geo.current = pgno2bytes(env, txn->mt_end_pgno);

    const txnid_t wanna_meta_txnid = (txn->mt_flags & MDBX_TXN_RDONLY)
                                         ? txn->mt_txnid
                                         : txn->mt_txnid - xMDBX_TXNID_STEP;
    txn_meta = (arg->mi_meta0_txnid == wanna_meta_txnid) ? meta0 : txn_meta;
    txn_meta = (arg->mi_meta1_txnid == wanna_meta_txnid) ? meta1 : txn_meta;
    txn_meta = (arg->mi_meta2_txnid == wanna_meta_txnid) ? meta2 : txn_meta;
  }
  arg->mi_geo.lower = pgno2bytes(env, txn_meta->mm_geo.lower);
  arg->mi_geo.upper = pgno2bytes(env, txn_meta->mm_geo.upper);
  arg->mi_geo.shrink = pgno2bytes(env, pv2pages(txn_meta->mm_geo.shrink_pv));
  arg->mi_geo.grow = pgno2bytes(env, pv2pages(txn_meta->mm_geo.grow_pv));
  const pgno_t unsynced_pages =
      atomic_load32(&env->me_lck->mti_unsynced_pages, mo_Relaxed) +
      (atomic_load32(&env->me_lck->mti_meta_sync_txnid, mo_Relaxed) !=
       (uint32_t)arg->mi_recent_txnid);

  arg->mi_mapsize = env->me_dxb_mmap.limit;

  const MDBX_lockinfo *const lck = env->me_lck;
  arg->mi_maxreaders = env->me_maxreaders;
  arg->mi_numreaders = env->me_lck_mmap.lck
                           ? atomic_load32(&lck->mti_numreaders, mo_Relaxed)
                           : INT32_MAX;
  arg->mi_dxb_pagesize = env->me_psize;
  arg->mi_sys_pagesize = env->me_os_psize;

  if (likely(bytes > size_before_bootid)) {
    arg->mi_unsync_volume = pgno2bytes(env, unsynced_pages);
    const uint64_t monotime_now = mdbx_osal_monotime();
    uint64_t ts = atomic_load64(&lck->mti_sync_timestamp, mo_Relaxed);
    arg->mi_since_sync_seconds16dot16 =
        ts ? mdbx_osal_monotime_to_16dot16(monotime_now - ts) : 0;
    ts = atomic_load64(&lck->mti_reader_check_timestamp, mo_Relaxed);
    arg->mi_since_reader_check_seconds16dot16 =
        ts ? mdbx_osal_monotime_to_16dot16(monotime_now - ts) : 0;
    arg->mi_autosync_threshold = pgno2bytes(
        env, atomic_load32(&lck->mti_autosync_threshold, mo_Relaxed));
    arg->mi_autosync_period_seconds16dot16 = mdbx_osal_monotime_to_16dot16(
        atomic_load64(&lck->mti_autosync_period, mo_Relaxed));
    arg->mi_bootid.current.x = bootid.x;
    arg->mi_bootid.current.y = bootid.y;
    arg->mi_mode = env->me_lck_mmap.lck ? lck->mti_envmode.weak : env->me_flags;
  }

  if (likely(bytes > size_before_pgop_stat)) {
#if MDBX_ENABLE_PGOP_STAT
    arg->mi_pgop_stat.newly =
        atomic_load64(&lck->mti_pgop_stat.newly, mo_Relaxed);
    arg->mi_pgop_stat.cow = atomic_load64(&lck->mti_pgop_stat.cow, mo_Relaxed);
    arg->mi_pgop_stat.clone =
        atomic_load64(&lck->mti_pgop_stat.clone, mo_Relaxed);
    arg->mi_pgop_stat.split =
        atomic_load64(&lck->mti_pgop_stat.split, mo_Relaxed);
    arg->mi_pgop_stat.merge =
        atomic_load64(&lck->mti_pgop_stat.merge, mo_Relaxed);
    arg->mi_pgop_stat.spill =
        atomic_load64(&lck->mti_pgop_stat.spill, mo_Relaxed);
    arg->mi_pgop_stat.unspill =
        atomic_load64(&lck->mti_pgop_stat.unspill, mo_Relaxed);
    arg->mi_pgop_stat.wops =
        atomic_load64(&lck->mti_pgop_stat.wops, mo_Relaxed);
#else
    memset(&arg->mi_pgop_stat, 0, sizeof(arg->mi_pgop_stat));
#endif /* MDBX_ENABLE_PGOP_STAT*/
  }

  arg->mi_self_latter_reader_txnid = arg->mi_latter_reader_txnid =
      arg->mi_recent_txnid;
  for (unsigned i = 0; i < arg->mi_numreaders; ++i) {
    const uint32_t pid =
        atomic_load32(&lck->mti_readers[i].mr_pid, mo_AcquireRelease);
    if (pid) {
      const txnid_t txnid = safe64_read(&lck->mti_readers[i].mr_txnid);
      if (arg->mi_latter_reader_txnid > txnid)
        arg->mi_latter_reader_txnid = txnid;
      if (pid == env->me_pid && arg->mi_self_latter_reader_txnid > txnid)
        arg->mi_self_latter_reader_txnid = txnid;
    }
  }

  mdbx_compiler_barrier();
  return MDBX_SUCCESS;
}

__cold int mdbx_env_info_ex(const MDBX_env *env, const MDBX_txn *txn,
                            MDBX_envinfo *arg, size_t bytes) {
  if (unlikely((env == NULL && txn == NULL) || arg == NULL))
    return MDBX_EINVAL;

  if (txn) {
    int err = check_txn(txn, MDBX_TXN_BLOCKED);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }
  if (env) {
    int err = check_env(env, false);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if (txn && unlikely(txn->mt_env != env))
      return MDBX_EINVAL;
  } else {
    env = txn->mt_env;
  }

  const size_t size_before_bootid = offsetof(MDBX_envinfo, mi_bootid);
  const size_t size_before_pgop_stat = offsetof(MDBX_envinfo, mi_pgop_stat);
  if (unlikely(bytes != sizeof(MDBX_envinfo)) && bytes != size_before_bootid &&
      bytes != size_before_pgop_stat)
    return MDBX_EINVAL;

  MDBX_envinfo snap;
  int rc = fetch_envinfo_ex(env, txn, &snap, sizeof(snap));
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  while (1) {
    rc = fetch_envinfo_ex(env, txn, arg, bytes);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    snap.mi_since_sync_seconds16dot16 = arg->mi_since_sync_seconds16dot16;
    snap.mi_since_reader_check_seconds16dot16 =
        arg->mi_since_reader_check_seconds16dot16;
    if (likely(memcmp(&snap, arg, bytes) == 0))
      return MDBX_SUCCESS;
    memcpy(&snap, arg, bytes);
  }
}

static __inline MDBX_cmp_func *get_default_keycmp(unsigned flags) {
  return (flags & MDBX_REVERSEKEY)   ? cmp_reverse
         : (flags & MDBX_INTEGERKEY) ? cmp_int_align2
                                     : cmp_lexical;
}

static __inline MDBX_cmp_func *get_default_datacmp(unsigned flags) {
  return !(flags & MDBX_DUPSORT)
             ? cmp_lenfast
             : ((flags & MDBX_INTEGERDUP)
                    ? cmp_int_unaligned
                    : ((flags & MDBX_REVERSEDUP) ? cmp_reverse : cmp_lexical));
}

static int mdbx_dbi_bind(MDBX_txn *txn, const MDBX_dbi dbi, unsigned user_flags,
                         MDBX_cmp_func *keycmp, MDBX_cmp_func *datacmp) {
  /* LY: so, accepting only three cases for the table's flags:
   * 1) user_flags and both comparators are zero
   *    = assume that a by-default mode/flags is requested for reading;
   * 2) user_flags exactly the same
   *    = assume that the target mode/flags are requested properly;
   * 3) user_flags differs, but table is empty and MDBX_CREATE is provided
   *    = assume that a properly create request with custom flags;
   */
  if ((user_flags ^ txn->mt_dbs[dbi].md_flags) & DB_PERSISTENT_FLAGS) {
    /* flags are differs, check other conditions */
    if ((!user_flags && (!keycmp || keycmp == txn->mt_dbxs[dbi].md_cmp) &&
         (!datacmp || datacmp == txn->mt_dbxs[dbi].md_dcmp)) ||
        user_flags == MDBX_ACCEDE) {
      /* no comparators were provided and flags are zero,
       * seems that is case #1 above */
      user_flags = txn->mt_dbs[dbi].md_flags;
    } else if ((user_flags & MDBX_CREATE) && txn->mt_dbs[dbi].md_entries == 0) {
      if (txn->mt_flags & MDBX_TXN_RDONLY)
        return /* FIXME: return extended info */ MDBX_EACCESS;
      /* make sure flags changes get committed */
      txn->mt_dbs[dbi].md_flags = user_flags & DB_PERSISTENT_FLAGS;
      txn->mt_flags |= MDBX_TXN_DIRTY;
    } else {
      return /* FIXME: return extended info */ MDBX_INCOMPATIBLE;
    }
  }

  if (!keycmp)
    keycmp = txn->mt_dbxs[dbi].md_cmp ? txn->mt_dbxs[dbi].md_cmp
                                      : get_default_keycmp(user_flags);
  if (txn->mt_dbxs[dbi].md_cmp != keycmp) {
    if (txn->mt_dbxs[dbi].md_cmp)
      return MDBX_EINVAL;
    txn->mt_dbxs[dbi].md_cmp = keycmp;
  }

  if (!datacmp)
    datacmp = txn->mt_dbxs[dbi].md_dcmp ? txn->mt_dbxs[dbi].md_dcmp
                                        : get_default_datacmp(user_flags);
  if (txn->mt_dbxs[dbi].md_dcmp != datacmp) {
    if (txn->mt_dbxs[dbi].md_dcmp)
      return MDBX_EINVAL;
    txn->mt_dbxs[dbi].md_dcmp = datacmp;
  }

  return MDBX_SUCCESS;
}

static int dbi_open(MDBX_txn *txn, const char *table_name, unsigned user_flags,
                    MDBX_dbi *dbi, MDBX_cmp_func *keycmp,
                    MDBX_cmp_func *datacmp) {
  int rc = MDBX_EINVAL;
  if (unlikely(!dbi))
    return rc;

  if (unlikely((user_flags & ~DB_USABLE_FLAGS) != 0)) {
  early_bailout:
    *dbi = 0;
    return rc;
  }

  rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    goto early_bailout;

  switch (user_flags & (MDBX_INTEGERDUP | MDBX_DUPFIXED | MDBX_DUPSORT |
                        MDBX_REVERSEDUP | MDBX_ACCEDE)) {
  case MDBX_ACCEDE:
    if ((user_flags & MDBX_CREATE) == 0)
      break;
    __fallthrough /* fall through */;
  default:
    rc = MDBX_EINVAL;
    goto early_bailout;

  case MDBX_DUPSORT:
  case MDBX_DUPSORT | MDBX_REVERSEDUP:
  case MDBX_DUPSORT | MDBX_DUPFIXED:
  case MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_REVERSEDUP:
  case MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP:
  case MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP | MDBX_REVERSEDUP:
  case 0:
    break;
  }

  /* main table? */
  if (!table_name) {
    rc = mdbx_dbi_bind(txn, MAIN_DBI, user_flags, keycmp, datacmp);
    if (unlikely(rc != MDBX_SUCCESS))
      goto early_bailout;
    *dbi = MAIN_DBI;
    return rc;
  }

  MDBX_env *env = txn->mt_env;
  size_t len = strlen(table_name);
  if (len > env->me_leaf_nodemax - NODESIZE - sizeof(MDBX_db))
    return MDBX_EINVAL;

  if (txn->mt_dbxs[MAIN_DBI].md_cmp == NULL) {
    txn->mt_dbxs[MAIN_DBI].md_cmp =
        get_default_keycmp(txn->mt_dbs[MAIN_DBI].md_flags);
    txn->mt_dbxs[MAIN_DBI].md_dcmp =
        get_default_datacmp(txn->mt_dbs[MAIN_DBI].md_flags);
  }

  /* Is the DB already open? */
  MDBX_dbi scan, slot;
  for (slot = scan = txn->mt_numdbs; --scan >= CORE_DBS;) {
    if (!txn->mt_dbxs[scan].md_name.iov_len) {
      /* Remember this free slot */
      slot = scan;
      continue;
    }
    if (len == txn->mt_dbxs[scan].md_name.iov_len &&
        !strncmp(table_name, txn->mt_dbxs[scan].md_name.iov_base, len)) {
      rc = mdbx_dbi_bind(txn, scan, user_flags, keycmp, datacmp);
      if (unlikely(rc != MDBX_SUCCESS))
        goto early_bailout;
      *dbi = scan;
      return rc;
    }
  }

  /* Fail, if no free slot and max hit */
  if (unlikely(slot >= env->me_maxdbs)) {
    rc = MDBX_DBS_FULL;
    goto early_bailout;
  }

  /* Cannot mix named table with some main-table flags */
  if (unlikely(txn->mt_dbs[MAIN_DBI].md_flags &
               (MDBX_DUPSORT | MDBX_INTEGERKEY))) {
    rc = (user_flags & MDBX_CREATE) ? MDBX_INCOMPATIBLE : MDBX_NOTFOUND;
    goto early_bailout;
  }

  /* Find the DB info */
  MDBX_val key, data;
  key.iov_len = len;
  key.iov_base = (void *)table_name;
  MDBX_cursor_couple couple;
  rc = mdbx_cursor_init(&couple.outer, txn, MAIN_DBI);
  if (unlikely(rc != MDBX_SUCCESS))
    goto early_bailout;
  rc = mdbx_cursor_set(&couple.outer, &key, &data, MDBX_SET).err;
  if (unlikely(rc != MDBX_SUCCESS)) {
    if (rc != MDBX_NOTFOUND || !(user_flags & MDBX_CREATE))
      goto early_bailout;
  } else {
    /* make sure this is actually a table */
    MDBX_node *node = page_node(couple.outer.mc_pg[couple.outer.mc_top],
                                couple.outer.mc_ki[couple.outer.mc_top]);
    if (unlikely((node_flags(node) & (F_DUPDATA | F_SUBDATA)) != F_SUBDATA)) {
      rc = MDBX_INCOMPATIBLE;
      goto early_bailout;
    }
    if (!MDBX_DISABLE_PAGECHECKS && unlikely(data.iov_len != sizeof(MDBX_db))) {
      rc = MDBX_CORRUPTED;
      goto early_bailout;
    }
  }

  if (rc != MDBX_SUCCESS && unlikely(txn->mt_flags & MDBX_TXN_RDONLY)) {
    rc = MDBX_EACCESS;
    goto early_bailout;
  }

  /* Done here so we cannot fail after creating a new DB */
  char *namedup = mdbx_strdup(table_name);
  if (unlikely(!namedup)) {
    rc = MDBX_ENOMEM;
    goto early_bailout;
  }

  int err = mdbx_fastmutex_acquire(&env->me_dbi_lock);
  if (unlikely(err != MDBX_SUCCESS)) {
    rc = err;
    mdbx_free(namedup);
    goto early_bailout;
  }

  /* Import handles from env */
  dbi_import_locked(txn);

  /* Rescan after mutex acquisition & import handles */
  for (slot = scan = txn->mt_numdbs; --scan >= CORE_DBS;) {
    if (!txn->mt_dbxs[scan].md_name.iov_len) {
      /* Remember this free slot */
      slot = scan;
      continue;
    }
    if (len == txn->mt_dbxs[scan].md_name.iov_len &&
        !strncmp(table_name, txn->mt_dbxs[scan].md_name.iov_base, len)) {
      rc = mdbx_dbi_bind(txn, scan, user_flags, keycmp, datacmp);
      if (unlikely(rc != MDBX_SUCCESS))
        goto later_bailout;
      *dbi = scan;
      goto later_exit;
    }
  }

  if (unlikely(slot >= env->me_maxdbs)) {
    rc = MDBX_DBS_FULL;
    goto later_bailout;
  }

  unsigned dbiflags = DBI_FRESH | DBI_VALID | DBI_USRVALID;
  MDBX_db db_dummy;
  if (unlikely(rc)) {
    /* MDBX_NOTFOUND and MDBX_CREATE: Create new DB */
    mdbx_tassert(txn, rc == MDBX_NOTFOUND);
    memset(&db_dummy, 0, sizeof(db_dummy));
    db_dummy.md_root = P_INVALID;
    db_dummy.md_mod_txnid = txn->mt_txnid;
    db_dummy.md_flags = user_flags & DB_PERSISTENT_FLAGS;
    data.iov_len = sizeof(db_dummy);
    data.iov_base = &db_dummy;
    WITH_CURSOR_TRACKING(couple.outer,
                         rc = mdbx_cursor_put(&couple.outer, &key, &data,
                                              F_SUBDATA | MDBX_NOOVERWRITE));

    if (unlikely(rc != MDBX_SUCCESS))
      goto later_bailout;

    dbiflags |= DBI_DIRTY | DBI_CREAT;
    txn->mt_flags |= MDBX_TXN_DIRTY;
  }

  /* Got info, register DBI in this txn */
  memset(txn->mt_dbxs + slot, 0, sizeof(MDBX_dbx));
  memcpy(&txn->mt_dbs[slot], data.iov_base, sizeof(MDBX_db));
  env->me_dbflags[slot] = 0;
  rc = mdbx_dbi_bind(txn, slot, user_flags, keycmp, datacmp);
  if (unlikely(rc != MDBX_SUCCESS)) {
    mdbx_tassert(txn, (dbiflags & DBI_CREAT) == 0);
  later_bailout:
    *dbi = 0;
  later_exit:
    mdbx_free(namedup);
  } else {
    txn->mt_dbistate[slot] = (uint8_t)dbiflags;
    txn->mt_dbxs[slot].md_name.iov_base = namedup;
    txn->mt_dbxs[slot].md_name.iov_len = len;
    txn->mt_dbiseqs[slot] = ++env->me_dbiseqs[slot];
    if (!(dbiflags & DBI_CREAT))
      env->me_dbflags[slot] = txn->mt_dbs[slot].md_flags | DB_VALID;
    if (txn->mt_numdbs == slot) {
      mdbx_compiler_barrier();
      txn->mt_numdbs = env->me_numdbs = slot + 1;
      if (!(txn->mt_flags & MDBX_TXN_RDONLY))
        txn->tw.cursors[slot] = NULL;
    }
    mdbx_assert(env, env->me_numdbs > slot);
    *dbi = slot;
  }

  mdbx_ensure(env, mdbx_fastmutex_release(&env->me_dbi_lock) == MDBX_SUCCESS);
  return rc;
}

int mdbx_dbi_open(MDBX_txn *txn, const char *table_name,
                  MDBX_db_flags_t table_flags, MDBX_dbi *dbi) {
  return dbi_open(txn, table_name, table_flags, dbi, nullptr, nullptr);
}

int mdbx_dbi_open_ex(MDBX_txn *txn, const char *table_name,
                     MDBX_db_flags_t table_flags, MDBX_dbi *dbi,
                     MDBX_cmp_func *keycmp, MDBX_cmp_func *datacmp) {
  return dbi_open(txn, table_name, table_flags, dbi, keycmp, datacmp);
}

__cold int mdbx_dbi_stat(MDBX_txn *txn, MDBX_dbi dbi, MDBX_stat *dest,
                         size_t bytes) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!dest))
    return MDBX_EINVAL;

  if (unlikely(!check_dbi(txn, dbi, DBI_VALID)))
    return MDBX_BAD_DBI;

  const size_t size_before_modtxnid = offsetof(MDBX_stat, ms_mod_txnid);
  if (unlikely(bytes != sizeof(MDBX_stat)) && bytes != size_before_modtxnid)
    return MDBX_EINVAL;

  if (unlikely(txn->mt_flags & MDBX_TXN_BLOCKED))
    return MDBX_BAD_TXN;

  if (unlikely(txn->mt_dbistate[dbi] & DBI_STALE)) {
    rc = mdbx_fetch_sdb(txn, dbi);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  dest->ms_psize = txn->mt_env->me_psize;
  stat_get(&txn->mt_dbs[dbi], dest, bytes);
  return MDBX_SUCCESS;
}

static int mdbx_dbi_close_locked(MDBX_env *env, MDBX_dbi dbi) {
  mdbx_assert(env, dbi >= CORE_DBS);
  if (unlikely(dbi >= env->me_numdbs))
    return MDBX_BAD_DBI;

  char *ptr = env->me_dbxs[dbi].md_name.iov_base;
  /* If there was no name, this was already closed */
  if (unlikely(!ptr))
    return MDBX_BAD_DBI;

  env->me_dbflags[dbi] = 0;
  env->me_dbiseqs[dbi]++;
  env->me_dbxs[dbi].md_name.iov_len = 0;
  mdbx_memory_fence(mo_AcquireRelease, true);
  env->me_dbxs[dbi].md_name.iov_base = NULL;
  mdbx_free(ptr);

  if (env->me_numdbs == dbi + 1) {
    unsigned i = env->me_numdbs;
    do
      --i;
    while (i > CORE_DBS && !env->me_dbxs[i - 1].md_name.iov_base);
    env->me_numdbs = i;
  }

  return MDBX_SUCCESS;
}

int mdbx_dbi_close(MDBX_env *env, MDBX_dbi dbi) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(dbi < CORE_DBS || dbi >= env->me_maxdbs))
    return MDBX_BAD_DBI;

  rc = mdbx_fastmutex_acquire(&env->me_dbi_lock);
  if (likely(rc == MDBX_SUCCESS)) {
    rc = (dbi < env->me_maxdbs && (env->me_dbflags[dbi] & DB_VALID))
             ? mdbx_dbi_close_locked(env, dbi)
             : MDBX_BAD_DBI;
    mdbx_ensure(env, mdbx_fastmutex_release(&env->me_dbi_lock) == MDBX_SUCCESS);
  }
  return rc;
}

int mdbx_dbi_flags_ex(MDBX_txn *txn, MDBX_dbi dbi, unsigned *flags,
                      unsigned *state) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!flags || !state))
    return MDBX_EINVAL;

  if (unlikely(!check_dbi(txn, dbi, DBI_VALID)))
    return MDBX_BAD_DBI;

  *flags = txn->mt_dbs[dbi].md_flags & DB_PERSISTENT_FLAGS;
  *state =
      txn->mt_dbistate[dbi] & (DBI_FRESH | DBI_CREAT | DBI_DIRTY | DBI_STALE);

  return MDBX_SUCCESS;
}

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
int mdbx_dbi_flags(MDBX_txn *txn, MDBX_dbi dbi, unsigned *flags) {
  return __inline_mdbx_dbi_flags(txn, dbi, flags);
}
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

static int mdbx_drop_tree(MDBX_cursor *mc, const bool may_have_subDBs) {
  int rc = mdbx_page_search(mc, NULL, MDBX_PS_FIRST);
  if (likely(rc == MDBX_SUCCESS)) {
    MDBX_txn *txn = mc->mc_txn;

    /* DUPSORT sub-DBs have no ovpages/DBs. Omit scanning leaves.
     * This also avoids any P_LEAF2 pages, which have no nodes.
     * Also if the DB doesn't have sub-DBs and has no overflow
     * pages, omit scanning leaves. */
    if (!(may_have_subDBs | mc->mc_db->md_overflow_pages))
      mdbx_cursor_pop(mc);

    rc = mdbx_pnl_need(&txn->tw.retired_pages,
                       mc->mc_db->md_branch_pages + mc->mc_db->md_leaf_pages +
                           mc->mc_db->md_overflow_pages);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;

    MDBX_cursor mx;
    cursor_copy(mc, &mx);
    while (mc->mc_snum > 0) {
      MDBX_page *const mp = mc->mc_pg[mc->mc_top];
      const unsigned nkeys = page_numkeys(mp);
      if (IS_LEAF(mp)) {
        mdbx_cassert(mc, mc->mc_snum == mc->mc_db->md_depth);
        for (unsigned i = 0; i < nkeys; i++) {
          MDBX_node *node = page_node(mp, i);
          if (node_flags(node) & F_BIGDATA) {
            rc = mdbx_page_retire_ex(mc, node_largedata_pgno(node), NULL, 0);
            if (unlikely(rc != MDBX_SUCCESS))
              goto bailout;
            if (!(may_have_subDBs | mc->mc_db->md_overflow_pages))
              goto pop;
          } else if (node_flags(node) & F_SUBDATA) {
            if (unlikely((node_flags(node) & F_DUPDATA) == 0)) {
              rc = /* disallowing implicit subDB deletion */ MDBX_INCOMPATIBLE;
              goto bailout;
            }
            rc = mdbx_xcursor_init1(mc, node, mp);
            if (unlikely(rc != MDBX_SUCCESS))
              goto bailout;
            rc = mdbx_drop_tree(&mc->mc_xcursor->mx_cursor, false);
            if (unlikely(rc != MDBX_SUCCESS))
              goto bailout;
          }
        }
      } else {
        mdbx_cassert(mc, mc->mc_snum < mc->mc_db->md_depth);
        if (mdbx_audit_enabled())
          mc->mc_flags |= C_RETIRING;
        const int pagetype =
            (IS_FROZEN(txn, mp) ? P_FROZEN : 0) +
            ((mc->mc_snum + 1 == mc->mc_db->md_depth) ? P_LEAF : P_BRANCH);
        for (unsigned i = 0; i < nkeys; i++) {
          MDBX_node *node = page_node(mp, i);
          mdbx_tassert(txn, (node_flags(node) &
                             (F_BIGDATA | F_SUBDATA | F_DUPDATA)) == 0);
          const pgno_t pgno = node_pgno(node);
          rc = mdbx_page_retire_ex(mc, pgno, NULL, pagetype);
          if (unlikely(rc != MDBX_SUCCESS))
            goto bailout;
        }
        if (mdbx_audit_enabled())
          mc->mc_flags -= C_RETIRING;
      }
      if (!mc->mc_top)
        break;
      mdbx_cassert(mc, nkeys > 0);
      mc->mc_ki[mc->mc_top] = (indx_t)nkeys;
      rc = mdbx_cursor_sibling(mc, SIBLING_RIGHT);
      if (unlikely(rc != MDBX_SUCCESS)) {
        if (unlikely(rc != MDBX_NOTFOUND))
          goto bailout;
      /* no more siblings, go back to beginning
       * of previous level. */
      pop:
        mdbx_cursor_pop(mc);
        mc->mc_ki[0] = 0;
        for (unsigned i = 1; i < mc->mc_snum; i++) {
          mc->mc_ki[i] = 0;
          mc->mc_pg[i] = mx.mc_pg[i];
        }
      }
    }
    rc = mdbx_page_retire(mc, mc->mc_pg[0]);
  bailout:
    if (unlikely(rc != MDBX_SUCCESS))
      txn->mt_flags |= MDBX_TXN_ERROR;
  } else if (rc == MDBX_NOTFOUND) {
    rc = MDBX_SUCCESS;
  }
  mc->mc_flags &= ~C_INITIALIZED;
  return rc;
}

int mdbx_drop(MDBX_txn *txn, MDBX_dbi dbi, bool del) {
  int rc = check_txn_rw(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  MDBX_cursor *mc;
  rc = mdbx_cursor_open(txn, dbi, &mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = mdbx_drop_tree(mc, dbi == MAIN_DBI ||
                              (mc->mc_db->md_flags & MDBX_DUPSORT) != 0);
  /* Invalidate the dropped DB's cursors */
  for (MDBX_cursor *m2 = txn->tw.cursors[dbi]; m2; m2 = m2->mc_next)
    m2->mc_flags &= ~(C_INITIALIZED | C_EOF);
  if (unlikely(rc))
    goto bailout;

  /* Can't delete the main DB */
  if (del && dbi >= CORE_DBS) {
    rc = mdbx_del0(txn, MAIN_DBI, &mc->mc_dbx->md_name, NULL, F_SUBDATA);
    if (likely(rc == MDBX_SUCCESS)) {
      mdbx_tassert(txn, txn->mt_dbistate[MAIN_DBI] & DBI_DIRTY);
      mdbx_tassert(txn, txn->mt_flags & MDBX_TXN_DIRTY);
      txn->mt_dbistate[dbi] = DBI_STALE;
      MDBX_env *env = txn->mt_env;
      rc = mdbx_fastmutex_acquire(&env->me_dbi_lock);
      if (unlikely(rc != MDBX_SUCCESS)) {
        txn->mt_flags |= MDBX_TXN_ERROR;
        goto bailout;
      }
      mdbx_dbi_close_locked(env, dbi);
      mdbx_ensure(env,
                  mdbx_fastmutex_release(&env->me_dbi_lock) == MDBX_SUCCESS);
    } else {
      txn->mt_flags |= MDBX_TXN_ERROR;
    }
  } else {
    /* reset the DB record, mark it dirty */
    txn->mt_dbistate[dbi] |= DBI_DIRTY;
    txn->mt_dbs[dbi].md_depth = 0;
    txn->mt_dbs[dbi].md_branch_pages = 0;
    txn->mt_dbs[dbi].md_leaf_pages = 0;
    txn->mt_dbs[dbi].md_overflow_pages = 0;
    txn->mt_dbs[dbi].md_entries = 0;
    txn->mt_dbs[dbi].md_root = P_INVALID;
    txn->mt_dbs[dbi].md_seq = 0;
    txn->mt_flags |= MDBX_TXN_DIRTY;
  }

bailout:
  mdbx_cursor_close(mc);
  return rc;
}

int mdbx_set_compare(MDBX_txn *txn, MDBX_dbi dbi, MDBX_cmp_func *cmp) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!check_dbi(txn, dbi, DBI_USRVALID)))
    return MDBX_BAD_DBI;

  txn->mt_dbxs[dbi].md_cmp = cmp;
  return MDBX_SUCCESS;
}

int mdbx_set_dupsort(MDBX_txn *txn, MDBX_dbi dbi, MDBX_cmp_func *cmp) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!check_dbi(txn, dbi, DBI_USRVALID)))
    return MDBX_BAD_DBI;

  txn->mt_dbxs[dbi].md_dcmp = cmp;
  return MDBX_SUCCESS;
}

__cold int mdbx_reader_list(const MDBX_env *env, MDBX_reader_list_func *func,
                            void *ctx) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!func))
    return MDBX_EINVAL;

  rc = MDBX_RESULT_TRUE;
  int serial = 0;
  MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
  if (likely(lck)) {
    const unsigned snap_nreaders =
        atomic_load32(&lck->mti_numreaders, mo_AcquireRelease);
    for (unsigned i = 0; i < snap_nreaders; i++) {
      const MDBX_reader *r = lck->mti_readers + i;
    retry_reader:;
      const uint32_t pid = atomic_load32(&r->mr_pid, mo_AcquireRelease);
      if (!pid)
        continue;
      txnid_t txnid = safe64_read(&r->mr_txnid);
      const uint64_t tid = atomic_load64(&r->mr_tid, mo_Relaxed);
      const pgno_t pages_used =
          atomic_load32(&r->mr_snapshot_pages_used, mo_Relaxed);
      const uint64_t reader_pages_retired =
          atomic_load64(&r->mr_snapshot_pages_retired, mo_Relaxed);
      if (unlikely(
              txnid != safe64_read(&r->mr_txnid) ||
              pid != atomic_load32(&r->mr_pid, mo_AcquireRelease) ||
              tid != atomic_load64(&r->mr_tid, mo_Relaxed) ||
              pages_used !=
                  atomic_load32(&r->mr_snapshot_pages_used, mo_Relaxed) ||
              reader_pages_retired !=
                  atomic_load64(&r->mr_snapshot_pages_retired, mo_Relaxed)))
        goto retry_reader;

      mdbx_assert(env, txnid > 0);
      if (txnid >= SAFE64_INVALID_THRESHOLD)
        txnid = 0;

      size_t bytes_used = 0;
      size_t bytes_retained = 0;
      uint64_t lag = 0;
      if (txnid) {
      retry_header:;
        const MDBX_meta *const recent_meta = mdbx_meta_head(env);
        const uint64_t head_pages_retired =
            unaligned_peek_u64(4, recent_meta->mm_pages_retired);
        const txnid_t head_txnid = mdbx_meta_txnid_fluid(env, recent_meta);
        mdbx_compiler_barrier();
        if (unlikely(
                recent_meta != mdbx_meta_head(env) ||
                head_pages_retired !=
                    unaligned_peek_u64(4, recent_meta->mm_pages_retired)) ||
            head_txnid != mdbx_meta_txnid_fluid(env, recent_meta))
          goto retry_header;

        lag = (head_txnid - txnid) / xMDBX_TXNID_STEP;
        bytes_used = pgno2bytes(env, pages_used);
        bytes_retained = (head_pages_retired > reader_pages_retired)
                             ? pgno2bytes(env, (pgno_t)(head_pages_retired -
                                                        reader_pages_retired))
                             : 0;
      }
      rc = func(ctx, ++serial, i, pid, (mdbx_tid_t)tid, txnid, lag, bytes_used,
                bytes_retained);
      if (unlikely(rc != MDBX_SUCCESS))
        break;
    }
  }

  return rc;
}

/* Insert pid into list if not already present.
 * return -1 if already present. */
__cold static bool mdbx_pid_insert(uint32_t *ids, uint32_t pid) {
  /* binary search of pid in list */
  unsigned base = 0;
  unsigned cursor = 1;
  int val = 0;
  unsigned n = ids[0];

  while (n > 0) {
    unsigned pivot = n >> 1;
    cursor = base + pivot + 1;
    val = pid - ids[cursor];

    if (val < 0) {
      n = pivot;
    } else if (val > 0) {
      base = cursor;
      n -= pivot + 1;
    } else {
      /* found, so it's a duplicate */
      return false;
    }
  }

  if (val > 0)
    ++cursor;

  ids[0]++;
  for (n = ids[0]; n > cursor; n--)
    ids[n] = ids[n - 1];
  ids[n] = pid;
  return true;
}

__cold int mdbx_reader_check(MDBX_env *env, int *dead) {
  if (dead)
    *dead = 0;
  return mdbx_cleanup_dead_readers(env, false, dead);
}

/* Return:
 *  MDBX_RESULT_TRUE - done and mutex recovered
 *  MDBX_SUCCESS     - done
 *  Otherwise errcode. */
__cold MDBX_INTERNAL_FUNC int
mdbx_cleanup_dead_readers(MDBX_env *env, int rdt_locked, int *dead) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  mdbx_assert(env, rdt_locked >= 0);
  MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
  if (unlikely(lck == NULL)) {
    /* exclusive mode */
    if (dead)
      *dead = 0;
    return MDBX_SUCCESS;
  }

  const unsigned snap_nreaders =
      atomic_load32(&lck->mti_numreaders, mo_AcquireRelease);
  uint32_t pidsbuf_onstask[142];
  uint32_t *const pids =
      (snap_nreaders < ARRAY_LENGTH(pidsbuf_onstask))
          ? pidsbuf_onstask
          : mdbx_malloc((snap_nreaders + 1) * sizeof(uint32_t));
  if (unlikely(!pids))
    return MDBX_ENOMEM;

  pids[0] = 0;
  int count = 0;
  for (unsigned i = 0; i < snap_nreaders; i++) {
    const uint32_t pid =
        atomic_load32(&lck->mti_readers[i].mr_pid, mo_AcquireRelease);
    if (pid == 0)
      continue /* skip empty */;
    if (pid == env->me_pid)
      continue /* skip self */;
    if (!mdbx_pid_insert(pids, pid))
      continue /* such pid already processed */;

    int err = mdbx_rpid_check(env, pid);
    if (err == MDBX_RESULT_TRUE)
      continue /* reader is live */;

    if (err != MDBX_SUCCESS) {
      rc = err;
      break /* mdbx_rpid_check() failed */;
    }

    /* stale reader found */
    if (!rdt_locked) {
      err = mdbx_rdt_lock(env);
      if (MDBX_IS_ERROR(err)) {
        rc = err;
        break;
      }

      rdt_locked = -1;
      if (err == MDBX_RESULT_TRUE) {
        /* mutex recovered, the mdbx_ipclock_failed() checked all readers */
        rc = MDBX_RESULT_TRUE;
        break;
      }

      /* a other process may have clean and reused slot, recheck */
      if (lck->mti_readers[i].mr_pid.weak != pid)
        continue;

      err = mdbx_rpid_check(env, pid);
      if (MDBX_IS_ERROR(err)) {
        rc = err;
        break;
      }

      if (err != MDBX_SUCCESS)
        continue /* the race with other process, slot reused */;
    }

    /* clean it */
    for (unsigned j = i; j < snap_nreaders; j++) {
      if (lck->mti_readers[j].mr_pid.weak == pid) {
        mdbx_debug("clear stale reader pid %" PRIuPTR " txn %" PRIaTXN,
                   (size_t)pid, lck->mti_readers[j].mr_txnid.weak);
        atomic_store32(&lck->mti_readers[j].mr_pid, 0, mo_Relaxed);
        atomic_store32(&lck->mti_readers_refresh_flag, true, mo_AcquireRelease);
        count++;
      }
    }
  }

  if (likely(!MDBX_IS_ERROR(rc)))
    atomic_store64(&lck->mti_reader_check_timestamp, mdbx_osal_monotime(),
                   mo_Relaxed);

  if (rdt_locked < 0)
    mdbx_rdt_unlock(env);

  if (pids != pidsbuf_onstask)
    mdbx_free(pids);

  if (dead)
    *dead = count;
  return rc;
}

__cold int mdbx_setup_debug(int loglevel, int flags, MDBX_debug_func *logger) {
  const int rc = mdbx_runtime_flags | (mdbx_loglevel << 16);

  if (loglevel != MDBX_LOG_DONTCHANGE)
    mdbx_loglevel = (uint8_t)loglevel;

  if (flags != MDBX_DBG_DONTCHANGE) {
    flags &=
#if MDBX_DEBUG
        MDBX_DBG_ASSERT | MDBX_DBG_AUDIT | MDBX_DBG_JITTER |
#endif
        MDBX_DBG_DUMP | MDBX_DBG_LEGACY_MULTIOPEN | MDBX_DBG_LEGACY_OVERLAP;
    mdbx_runtime_flags = (uint8_t)flags;
  }

  if (logger != MDBX_LOGGER_DONTCHANGE)
    mdbx_debug_logger = logger;
  return rc;
}

__cold static txnid_t mdbx_kick_longlived_readers(MDBX_env *env,
                                                  const txnid_t laggard) {
  mdbx_debug("DB size maxed out by reading #%" PRIaTXN, laggard);

  int retry;
  for (retry = 0; retry < INT_MAX; ++retry) {
    txnid_t oldest = mdbx_recent_steady_txnid(env);
    mdbx_assert(env, oldest < env->me_txn0->mt_txnid);
    mdbx_assert(env, oldest >= laggard);
    mdbx_assert(env, oldest >= env->me_lck->mti_oldest_reader.weak);
    MDBX_lockinfo *const lck = env->me_lck_mmap.lck;
    if (oldest == laggard || unlikely(!lck /* without-LCK mode */))
      return oldest;

    if (MDBX_IS_ERROR(mdbx_cleanup_dead_readers(env, false, NULL)))
      break;

    MDBX_reader *asleep = nullptr;
    uint64_t oldest_retired = UINT64_MAX;
    const unsigned snap_nreaders =
        atomic_load32(&lck->mti_numreaders, mo_AcquireRelease);
    for (unsigned i = 0; i < snap_nreaders; ++i) {
    retry:
      if (atomic_load32(&lck->mti_readers[i].mr_pid, mo_AcquireRelease)) {
        /* mdbx_jitter4testing(true); */
        const uint64_t snap_retired = atomic_load64(
            &lck->mti_readers[i].mr_snapshot_pages_retired, mo_Relaxed);
        const txnid_t snap_txnid = safe64_read(&lck->mti_readers[i].mr_txnid);
        if (unlikely(snap_retired !=
                         atomic_load64(
                             &lck->mti_readers[i].mr_snapshot_pages_retired,
                             mo_AcquireRelease) ||
                     snap_txnid != safe64_read(&lck->mti_readers[i].mr_txnid)))
          goto retry;
        if (oldest > snap_txnid &&
            laggard <= /* ignore pending updates */ snap_txnid) {
          oldest = snap_txnid;
          oldest_retired = snap_retired;
          asleep = &lck->mti_readers[i];
        }
      }
    }

    if (laggard < oldest || !asleep) {
      if (retry && env->me_hsr_callback) {
        /* LY: notify end of hsr-loop */
        const txnid_t gap = oldest - laggard;
        env->me_hsr_callback(env, env->me_txn, 0, 0, laggard,
                             (gap < UINT_MAX) ? (unsigned)gap : UINT_MAX, 0,
                             -retry);
      }
      mdbx_notice("hsr-kick: update oldest %" PRIaTXN " -> %" PRIaTXN,
                  lck->mti_oldest_reader.weak, oldest);
      mdbx_assert(env, lck->mti_oldest_reader.weak <= oldest);
      return atomic_store64(&lck->mti_oldest_reader, oldest, mo_Relaxed);
    }

    if (!env->me_hsr_callback)
      break;

    uint32_t pid = atomic_load32(&asleep->mr_pid, mo_AcquireRelease);
    uint64_t tid = asleep->mr_tid.weak;
    if (safe64_read(&asleep->mr_txnid) != laggard || pid <= 0)
      continue;

    const MDBX_meta *head_meta = mdbx_meta_head(env);
    const txnid_t gap =
        (mdbx_meta_txnid_stable(env, head_meta) - laggard) / xMDBX_TXNID_STEP;
    const uint64_t head_retired =
        unaligned_peek_u64(4, head_meta->mm_pages_retired);
    const size_t space =
        (oldest_retired > head_retired)
            ? pgno2bytes(env, (pgno_t)(oldest_retired - head_retired))
            : 0;
    int rc = env->me_hsr_callback(
        env, env->me_txn, pid, (mdbx_tid_t)tid, laggard,
        (gap < UINT_MAX) ? (unsigned)gap : UINT_MAX, space, retry);
    if (rc < 0)
      break;

    if (rc > 0) {
      if (rc == 1) {
        safe64_reset_compare(&asleep->mr_txnid, laggard);
      } else {
        safe64_reset(&asleep->mr_txnid, true);
        atomic_store64(&asleep->mr_tid, 0, mo_Relaxed);
        atomic_store32(&asleep->mr_pid, 0, mo_Relaxed);
      }
      atomic_store32(&lck->mti_readers_refresh_flag, true, mo_Relaxed);
    }
  }

  if (retry && env->me_hsr_callback) {
    /* LY: notify end of hsr-loop */
    env->me_hsr_callback(env, env->me_txn, 0, 0, laggard, 0, 0, -retry);
  }
  return mdbx_find_oldest(env->me_txn);
}

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
__cold int mdbx_env_set_syncbytes(MDBX_env *env, size_t threshold) {
  return __inline_mdbx_env_set_syncbytes(env, threshold);
}

__cold int mdbx_env_set_syncperiod(MDBX_env *env, unsigned seconds_16dot16) {
  return __inline_mdbx_env_set_syncperiod(env, seconds_16dot16);
}
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

__cold int mdbx_env_set_hsr(MDBX_env *env, MDBX_hsr_func *hsr) {
  int rc = check_env(env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  env->me_hsr_callback = hsr;
  return MDBX_SUCCESS;
}

__cold MDBX_hsr_func *mdbx_env_get_hsr(const MDBX_env *env) {
  return likely(env && env->me_signature.weak == MDBX_ME_SIGNATURE)
             ? env->me_hsr_callback
             : NULL;
}

#ifdef __SANITIZE_THREAD__
/* LY: avoid tsan-trap by me_txn, mm_last_pg and mt_next_pgno */
__attribute__((__no_sanitize_thread__, __noinline__))
#endif
int mdbx_txn_straggler(const MDBX_txn *txn, int *percent)
{
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return (rc > 0) ? -rc : rc;

  MDBX_env *env = txn->mt_env;
  if (unlikely((txn->mt_flags & MDBX_TXN_RDONLY) == 0)) {
    if (percent)
      *percent =
          (int)((txn->mt_next_pgno * UINT64_C(100) + txn->mt_end_pgno / 2) /
                txn->mt_end_pgno);
    return 0;
  }

  txnid_t recent;
  MDBX_meta *meta;
  do {
    meta = mdbx_meta_head(env);
    recent = mdbx_meta_txnid_fluid(env, meta);
    if (percent) {
      const pgno_t maxpg = meta->mm_geo.now;
      *percent = (int)((meta->mm_geo.next * UINT64_C(100) + maxpg / 2) / maxpg);
    }
  } while (unlikely(recent != mdbx_meta_txnid_fluid(env, meta)));

  txnid_t lag = (recent - txn->mt_txnid) / xMDBX_TXNID_STEP;
  return (lag > INT_MAX) ? INT_MAX : (int)lag;
}

typedef struct mdbx_walk_ctx {
  void *mw_user;
  MDBX_pgvisitor_func *mw_visitor;
  MDBX_txn *mw_txn;
  MDBX_cursor *mw_cursor;
  bool mw_dont_check_keys_ordering;
} mdbx_walk_ctx_t;

__cold static int mdbx_walk_sdb(mdbx_walk_ctx_t *ctx, MDBX_db *const db,
                                const char *name, int deep);

static MDBX_page_type_t walk_page_type(const MDBX_page *mp) {
  if (mp)
    switch (mp->mp_flags) {
    case P_BRANCH:
      return MDBX_page_branch;
    case P_LEAF:
      return MDBX_page_leaf;
    case P_LEAF | P_LEAF2:
      return MDBX_page_dupfixed_leaf;
    case P_OVERFLOW:
      return MDBX_page_large;
    case P_META:
      return MDBX_page_meta;
    }
  return MDBX_page_broken;
}

/* Depth-first tree traversal. */
__cold static int mdbx_walk_tree(mdbx_walk_ctx_t *ctx, const pgno_t pgno,
                                 const char *name, int deep,
                                 txnid_t parent_txnid) {
  assert(pgno != P_INVALID);
  MDBX_page *mp = nullptr;
  int rc, err = mdbx_page_get(ctx->mw_cursor, pgno, &mp, parent_txnid);
  if (err == MDBX_SUCCESS)
    err = mdbx_page_check(ctx->mw_cursor, mp, 0);

  MDBX_page_type_t type = walk_page_type(mp);
  const int nentries = (mp && !IS_OVERFLOW(mp)) ? page_numkeys(mp) : 1;
  unsigned npages = (mp && IS_OVERFLOW(mp)) ? mp->mp_pages : 1;
  size_t pagesize = pgno2bytes(ctx->mw_txn->mt_env, npages);
  size_t header_size = (mp && !IS_LEAF2(mp) && !IS_OVERFLOW(mp))
                           ? PAGEHDRSZ + mp->mp_lower
                           : PAGEHDRSZ;
  size_t payload_size = 0;
  size_t unused_size =
      (mp && !IS_OVERFLOW(mp) ? page_room(mp) : pagesize - header_size) -
      payload_size;
  size_t align_bytes = 0;

  if (err == MDBX_SUCCESS) {
    /* LY: Don't use mask here, e.g bitwise
     * (P_BRANCH|P_LEAF|P_LEAF2|P_META|P_OVERFLOW|P_SUBP).
     * Pages should not me marked dirty/loose or otherwise. */
    switch (mp->mp_flags) {
    default:
      err = MDBX_CORRUPTED;
      break;
    case P_BRANCH:
      if (unlikely(nentries < 2))
        err = MDBX_CORRUPTED;
    case P_LEAF:
    case P_LEAF | P_LEAF2:
      break;
    }
  }

  for (int i = 0; err == MDBX_SUCCESS && i < nentries;
       align_bytes += ((payload_size + align_bytes) & 1), i++) {
    if (type == MDBX_page_dupfixed_leaf) {
      /* LEAF2 pages have no mp_ptrs[] or node headers */
      payload_size += mp->mp_leaf2_ksize;
      continue;
    }

    MDBX_node *node = page_node(mp, i);
    payload_size += NODESIZE + node_ks(node);

    if (type == MDBX_page_branch) {
      assert(i > 0 || node_ks(node) == 0);
      continue;
    }

    assert(type == MDBX_page_leaf);
    switch (node_flags(node)) {
    case 0 /* usual node */:
      payload_size += node_ds(node);
      break;

    case F_BIGDATA /* long data on the large/overflow page */: {
      payload_size += sizeof(pgno_t);
      const pgno_t large_pgno = node_largedata_pgno(node);
      const size_t over_payload = node_ds(node);
      const size_t over_header = PAGEHDRSZ;
      npages = 1;

      MDBX_page *op;
      err = mdbx_page_get(ctx->mw_cursor, large_pgno, &op,
                          pp_txnid4chk(mp, ctx->mw_txn));
      if (err == MDBX_SUCCESS)
        err = mdbx_page_check(ctx->mw_cursor, op, 0);
      if (err == MDBX_SUCCESS) {
        /* LY: Don't use mask here, e.g bitwise
         * (P_BRANCH|P_LEAF|P_LEAF2|P_META|P_OVERFLOW|P_SUBP).
         * Pages should not me marked dirty/loose or otherwise. */
        if (unlikely(P_OVERFLOW != op->mp_flags))
          err = bad_page(mp, "wrong page type %d for large data", op->mp_flags);
        else
          npages = op->mp_pages;
      }

      pagesize = pgno2bytes(ctx->mw_txn->mt_env, npages);
      const size_t over_unused = pagesize - over_payload - over_header;
      rc = ctx->mw_visitor(large_pgno, npages, ctx->mw_user, deep, name,
                           pagesize, MDBX_page_large, err, 1, over_payload,
                           over_header, over_unused);
      if (unlikely(rc != MDBX_SUCCESS))
        return (rc == MDBX_RESULT_TRUE) ? MDBX_SUCCESS : rc;
    } break;

    case F_SUBDATA /* sub-db */: {
      const size_t namelen = node_ks(node);
      payload_size += node_ds(node);
      if (unlikely(namelen == 0 || node_ds(node) != sizeof(MDBX_db)))
        err = MDBX_CORRUPTED;
    } break;

    case F_SUBDATA | F_DUPDATA /* dupsorted sub-tree */:
      payload_size += sizeof(MDBX_db);
      if (unlikely(node_ds(node) != sizeof(MDBX_db)))
        err = MDBX_CORRUPTED;
      break;

    case F_DUPDATA /* short sub-page */: {
      if (unlikely(node_ds(node) <= PAGEHDRSZ)) {
        err = MDBX_CORRUPTED;
        break;
      }

      MDBX_page *sp = node_data(node);
      const int nsubkeys = page_numkeys(sp);
      size_t subheader_size =
          IS_LEAF2(sp) ? PAGEHDRSZ : PAGEHDRSZ + sp->mp_lower;
      size_t subunused_size = page_room(sp);
      size_t subpayload_size = 0;
      size_t subalign_bytes = 0;
      MDBX_page_type_t subtype;

      switch (sp->mp_flags & /* ignore legacy P_DIRTY flag */ ~0x10) {
      case P_LEAF | P_SUBP:
        subtype = MDBX_subpage_leaf;
        break;
      case P_LEAF | P_LEAF2 | P_SUBP:
        subtype = MDBX_subpage_dupfixed_leaf;
        break;
      default:
        subtype = MDBX_subpage_broken;
        err = MDBX_CORRUPTED;
      }

      for (int j = 0; err == MDBX_SUCCESS && j < nsubkeys;
           subalign_bytes += ((subpayload_size + subalign_bytes) & 1), j++) {

        if (subtype == MDBX_subpage_dupfixed_leaf) {
          /* LEAF2 pages have no mp_ptrs[] or node headers */
          subpayload_size += sp->mp_leaf2_ksize;
        } else {
          assert(subtype == MDBX_subpage_leaf);
          MDBX_node *subnode = page_node(sp, j);
          subpayload_size += NODESIZE + node_ks(subnode) + node_ds(subnode);
          if (unlikely(node_flags(subnode) != 0))
            err = MDBX_CORRUPTED;
        }
      }

      rc = ctx->mw_visitor(pgno, 0, ctx->mw_user, deep + 1, name, node_ds(node),
                           subtype, err, nsubkeys, subpayload_size,
                           subheader_size, subunused_size + subalign_bytes);
      if (unlikely(rc != MDBX_SUCCESS))
        return (rc == MDBX_RESULT_TRUE) ? MDBX_SUCCESS : rc;
      header_size += subheader_size;
      unused_size += subunused_size;
      payload_size += subpayload_size;
      align_bytes += subalign_bytes;
    } break;

    default:
      err = MDBX_CORRUPTED;
    }
  }

  rc = ctx->mw_visitor(pgno, 1, ctx->mw_user, deep, name,
                       ctx->mw_txn->mt_env->me_psize, type, err, nentries,
                       payload_size, header_size, unused_size + align_bytes);
  if (unlikely(rc != MDBX_SUCCESS))
    return (rc == MDBX_RESULT_TRUE) ? MDBX_SUCCESS : rc;

  for (int i = 0; err == MDBX_SUCCESS && i < nentries; i++) {
    if (type == MDBX_page_dupfixed_leaf)
      continue;

    MDBX_node *node = page_node(mp, i);
    if (type == MDBX_page_branch) {
      err = mdbx_walk_tree(ctx, node_pgno(node), name, deep + 1,
                           pp_txnid4chk(mp, ctx->mw_txn));
      if (unlikely(err != MDBX_SUCCESS)) {
        if (err == MDBX_RESULT_TRUE)
          break;
        return err;
      }
      continue;
    }

    assert(type == MDBX_page_leaf);
    MDBX_db db;
    switch (node_flags(node)) {
    default:
      continue;

    case F_SUBDATA /* sub-db */: {
      const size_t namelen = node_ks(node);
      if (unlikely(namelen == 0 || node_ds(node) != sizeof(MDBX_db))) {
        err = MDBX_CORRUPTED;
        break;
      }

      char namebuf_onstask[64];
      char *const sub_name = (namelen < sizeof(namebuf_onstask))
                                 ? namebuf_onstask
                                 : mdbx_malloc(namelen + 1);
      if (sub_name) {
        memcpy(sub_name, node_key(node), namelen);
        sub_name[namelen] = 0;
        memcpy(&db, node_data(node), sizeof(db));
        err = mdbx_walk_sdb(ctx, &db, sub_name, deep + 1);
        if (sub_name != namebuf_onstask)
          mdbx_free(sub_name);
      } else {
        err = MDBX_ENOMEM;
      }
    } break;

    case F_SUBDATA | F_DUPDATA /* dupsorted sub-tree */:
      if (unlikely(node_ds(node) != sizeof(MDBX_db) ||
                   ctx->mw_cursor->mc_xcursor == NULL))
        err = MDBX_CORRUPTED;
      else {
        memcpy(&db, node_data(node), sizeof(db));
        assert(ctx->mw_cursor->mc_xcursor ==
               &container_of(ctx->mw_cursor, MDBX_cursor_couple, outer)->inner);
        ctx->mw_cursor = &ctx->mw_cursor->mc_xcursor->mx_cursor;
        err = mdbx_walk_tree(ctx, db.md_root, name, deep + 1,
                             pp_txnid4chk(mp, ctx->mw_txn));
        MDBX_xcursor *inner_xcursor =
            container_of(ctx->mw_cursor, MDBX_xcursor, mx_cursor);
        MDBX_cursor_couple *couple =
            container_of(inner_xcursor, MDBX_cursor_couple, inner);
        ctx->mw_cursor = &couple->outer;
      }
      break;
    }
  }

  return MDBX_SUCCESS;
}

__cold static int mdbx_walk_sdb(mdbx_walk_ctx_t *ctx, MDBX_db *const db,
                                const char *name, int deep) {
  if (unlikely(db->md_root == P_INVALID))
    return MDBX_SUCCESS; /* empty db */

  MDBX_cursor_couple couple;
  MDBX_dbx dbx = {.md_klen_min = INT_MAX};
  uint8_t dbistate = DBI_VALID | DBI_AUDITED;
  int rc = mdbx_couple_init(&couple, ~0u, ctx->mw_txn, db, &dbx, &dbistate);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (ctx->mw_dont_check_keys_ordering) {
    couple.outer.mc_flags |= C_SKIPORD;
    couple.inner.mx_cursor.mc_flags |= C_SKIPORD;
  }
  couple.outer.mc_next = ctx->mw_cursor;
  ctx->mw_cursor = &couple.outer;
  rc = mdbx_walk_tree(ctx, db->md_root, name, deep, ctx->mw_txn->mt_txnid);
  ctx->mw_cursor = couple.outer.mc_next;
  return rc;
}

__cold int mdbx_env_pgwalk(MDBX_txn *txn, MDBX_pgvisitor_func *visitor,
                           void *user, bool dont_check_keys_ordering) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  mdbx_walk_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.mw_txn = txn;
  ctx.mw_user = user;
  ctx.mw_visitor = visitor;
  ctx.mw_dont_check_keys_ordering = dont_check_keys_ordering;

  rc = visitor(0, NUM_METAS, user, 0, MDBX_PGWALK_META,
               pgno2bytes(txn->mt_env, NUM_METAS), MDBX_page_meta, MDBX_SUCCESS,
               NUM_METAS, sizeof(MDBX_meta) * NUM_METAS, PAGEHDRSZ * NUM_METAS,
               (txn->mt_env->me_psize - sizeof(MDBX_meta) - PAGEHDRSZ) *
                   NUM_METAS);
  if (!MDBX_IS_ERROR(rc))
    rc = mdbx_walk_sdb(&ctx, &txn->mt_dbs[FREE_DBI], MDBX_PGWALK_GC, 0);
  if (!MDBX_IS_ERROR(rc))
    rc = mdbx_walk_sdb(&ctx, &txn->mt_dbs[MAIN_DBI], MDBX_PGWALK_MAIN, 0);
  return rc;
}

int mdbx_canary_put(MDBX_txn *txn, const MDBX_canary *canary) {
  int rc = check_txn_rw(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (likely(canary)) {
    if (txn->mt_canary.x == canary->x && txn->mt_canary.y == canary->y &&
        txn->mt_canary.z == canary->z)
      return MDBX_SUCCESS;
    txn->mt_canary.x = canary->x;
    txn->mt_canary.y = canary->y;
    txn->mt_canary.z = canary->z;
  }
  txn->mt_canary.v = txn->mt_txnid;
  txn->mt_flags |= MDBX_TXN_DIRTY;

  return MDBX_SUCCESS;
}

int mdbx_canary_get(const MDBX_txn *txn, MDBX_canary *canary) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(canary == NULL))
    return MDBX_EINVAL;

  *canary = txn->mt_canary;
  return MDBX_SUCCESS;
}

int mdbx_cursor_on_first(const MDBX_cursor *mc) {
  if (unlikely(mc == NULL))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_signature != MDBX_MC_LIVE))
    return (mc->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                     : MDBX_EBADSIGN;

  if (!(mc->mc_flags & C_INITIALIZED))
    return mc->mc_db->md_entries ? MDBX_RESULT_FALSE : MDBX_RESULT_TRUE;

  for (unsigned i = 0; i < mc->mc_snum; ++i) {
    if (mc->mc_ki[i])
      return MDBX_RESULT_FALSE;
  }

  return MDBX_RESULT_TRUE;
}

int mdbx_cursor_on_last(const MDBX_cursor *mc) {
  if (unlikely(mc == NULL))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_signature != MDBX_MC_LIVE))
    return (mc->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                     : MDBX_EBADSIGN;

  if (!(mc->mc_flags & C_INITIALIZED))
    return mc->mc_db->md_entries ? MDBX_RESULT_FALSE : MDBX_RESULT_TRUE;

  for (unsigned i = 0; i < mc->mc_snum; ++i) {
    unsigned nkeys = page_numkeys(mc->mc_pg[i]);
    if (mc->mc_ki[i] < nkeys - 1)
      return MDBX_RESULT_FALSE;
  }

  return MDBX_RESULT_TRUE;
}

int mdbx_cursor_eof(const MDBX_cursor *mc) {
  if (unlikely(mc == NULL))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_signature != MDBX_MC_LIVE))
    return (mc->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                     : MDBX_EBADSIGN;

  return ((mc->mc_flags & (C_INITIALIZED | C_EOF)) == C_INITIALIZED &&
          mc->mc_snum &&
          mc->mc_ki[mc->mc_top] < page_numkeys(mc->mc_pg[mc->mc_top]))
             ? MDBX_RESULT_FALSE
             : MDBX_RESULT_TRUE;
}

//------------------------------------------------------------------------------

struct diff_result {
  ptrdiff_t diff;
  unsigned level;
  int root_nkeys;
};

/* calculates: r = x - y */
__hot static int cursor_diff(const MDBX_cursor *const __restrict x,
                             const MDBX_cursor *const __restrict y,
                             struct diff_result *const __restrict r) {
  r->diff = 0;
  r->level = 0;
  r->root_nkeys = 0;

  if (unlikely(x->mc_signature != MDBX_MC_LIVE))
    return (x->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                    : MDBX_EBADSIGN;

  if (unlikely(y->mc_signature != MDBX_MC_LIVE))
    return (y->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                    : MDBX_EBADSIGN;

  int rc = check_txn(x->mc_txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(x->mc_txn != y->mc_txn))
    return MDBX_BAD_TXN;

  if (unlikely(y->mc_dbi != x->mc_dbi))
    return MDBX_EINVAL;

  if (unlikely(!(y->mc_flags & x->mc_flags & C_INITIALIZED)))
    return MDBX_ENODATA;

  while (likely(r->level < y->mc_snum && r->level < x->mc_snum)) {
    if (unlikely(y->mc_pg[r->level] != x->mc_pg[r->level])) {
      mdbx_error("Mismatch cursors's pages at %u level", r->level);
      return MDBX_PROBLEM;
    }

    int nkeys = page_numkeys(y->mc_pg[r->level]);
    assert(nkeys > 0);
    if (r->level == 0)
      r->root_nkeys = nkeys;

    const int limit_ki = nkeys - 1;
    const int x_ki = x->mc_ki[r->level];
    const int y_ki = y->mc_ki[r->level];
    r->diff = ((x_ki < limit_ki) ? x_ki : limit_ki) -
              ((y_ki < limit_ki) ? y_ki : limit_ki);
    if (r->diff == 0) {
      r->level += 1;
      continue;
    }

    while (unlikely(r->diff == 1) &&
           likely(r->level + 1 < y->mc_snum && r->level + 1 < x->mc_snum)) {
      r->level += 1;
      /*   DB'PAGEs: 0------------------>MAX
       *
       *    CURSORs:       y < x
       *  STACK[i ]:         |
       *  STACK[+1]:  ...y++N|0++x...
       */
      nkeys = page_numkeys(y->mc_pg[r->level]);
      r->diff = (nkeys - y->mc_ki[r->level]) + x->mc_ki[r->level];
      assert(r->diff > 0);
    }

    while (unlikely(r->diff == -1) &&
           likely(r->level + 1 < y->mc_snum && r->level + 1 < x->mc_snum)) {
      r->level += 1;
      /*   DB'PAGEs: 0------------------>MAX
       *
       *    CURSORs:       x < y
       *  STACK[i ]:         |
       *  STACK[+1]:  ...x--N|0--y...
       */
      nkeys = page_numkeys(x->mc_pg[r->level]);
      r->diff = -(nkeys - x->mc_ki[r->level]) - y->mc_ki[r->level];
      assert(r->diff < 0);
    }

    return MDBX_SUCCESS;
  }

  r->diff = CMP2INT(x->mc_flags & C_EOF, y->mc_flags & C_EOF);
  return MDBX_SUCCESS;
}

__hot static ptrdiff_t estimate(const MDBX_db *db,
                                struct diff_result *const __restrict dr) {
  /*        root: branch-page    => scale = leaf-factor * branch-factor^(N-1)
   *     level-1: branch-page(s) => scale = leaf-factor * branch-factor^2
   *     level-2: branch-page(s) => scale = leaf-factor * branch-factor
   *     level-N: branch-page(s) => scale = leaf-factor
   *  leaf-level: leaf-page(s)   => scale = 1
   */
  ptrdiff_t btree_power = (ptrdiff_t)db->md_depth - 2 - (ptrdiff_t)dr->level;
  if (btree_power < 0)
    return dr->diff;

  ptrdiff_t estimated =
      (ptrdiff_t)db->md_entries * dr->diff / (ptrdiff_t)db->md_leaf_pages;
  if (btree_power == 0)
    return estimated;

  if (db->md_depth < 4) {
    assert(dr->level == 0 && btree_power == 1);
    return (ptrdiff_t)db->md_entries * dr->diff / (ptrdiff_t)dr->root_nkeys;
  }

  /* average_branchpage_fillfactor = total(branch_entries) / branch_pages
     total(branch_entries) = leaf_pages + branch_pages - 1 (root page) */
  const size_t log2_fixedpoint = sizeof(size_t) - 1;
  const size_t half = UINT64_C(1) << (log2_fixedpoint - 1);
  const size_t factor =
      ((db->md_leaf_pages + db->md_branch_pages - 1) << log2_fixedpoint) /
      db->md_branch_pages;
  while (1) {
    switch ((size_t)btree_power) {
    default: {
      const size_t square = (factor * factor + half) >> log2_fixedpoint;
      const size_t quad = (square * square + half) >> log2_fixedpoint;
      do {
        estimated = estimated * quad + half;
        estimated >>= log2_fixedpoint;
        btree_power -= 4;
      } while (btree_power >= 4);
      continue;
    }
    case 3:
      estimated = estimated * factor + half;
      estimated >>= log2_fixedpoint;
      __fallthrough /* fall through */;
    case 2:
      estimated = estimated * factor + half;
      estimated >>= log2_fixedpoint;
      __fallthrough /* fall through */;
    case 1:
      estimated = estimated * factor + half;
      estimated >>= log2_fixedpoint;
      __fallthrough /* fall through */;
    case 0:
      if (unlikely(estimated > (ptrdiff_t)db->md_entries))
        return (ptrdiff_t)db->md_entries;
      if (unlikely(estimated < -(ptrdiff_t)db->md_entries))
        return -(ptrdiff_t)db->md_entries;
      return estimated;
    }
  }
}

int mdbx_estimate_distance(const MDBX_cursor *first, const MDBX_cursor *last,
                           ptrdiff_t *distance_items) {
  if (unlikely(first == NULL || last == NULL || distance_items == NULL))
    return MDBX_EINVAL;

  *distance_items = 0;
  struct diff_result dr;
  int rc = cursor_diff(last, first, &dr);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(dr.diff == 0) &&
      F_ISSET(first->mc_db->md_flags & last->mc_db->md_flags,
              MDBX_DUPSORT | C_INITIALIZED)) {
    first = &first->mc_xcursor->mx_cursor;
    last = &last->mc_xcursor->mx_cursor;
    rc = cursor_diff(first, last, &dr);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  if (likely(dr.diff != 0))
    *distance_items = estimate(first->mc_db, &dr);

  return MDBX_SUCCESS;
}

int mdbx_estimate_move(const MDBX_cursor *cursor, MDBX_val *key, MDBX_val *data,
                       MDBX_cursor_op move_op, ptrdiff_t *distance_items) {
  if (unlikely(cursor == NULL || distance_items == NULL ||
               move_op == MDBX_GET_CURRENT || move_op == MDBX_GET_MULTIPLE))
    return MDBX_EINVAL;

  if (unlikely(cursor->mc_signature != MDBX_MC_LIVE))
    return (cursor->mc_signature == MDBX_MC_READY4CLOSE) ? MDBX_EINVAL
                                                         : MDBX_EBADSIGN;

  int rc = check_txn(cursor->mc_txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (!(cursor->mc_flags & C_INITIALIZED))
    return MDBX_ENODATA;

  MDBX_cursor_couple next;
  cursor_copy(cursor, &next.outer);
  if (cursor->mc_db->md_flags & MDBX_DUPSORT) {
    next.outer.mc_xcursor = &next.inner;
    rc = mdbx_xcursor_init0(&next.outer);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    MDBX_xcursor *mx = &container_of(cursor, MDBX_cursor_couple, outer)->inner;
    cursor_copy(&mx->mx_cursor, &next.inner.mx_cursor);
  }

  MDBX_val stub = {0, 0};
  if (data == NULL) {
    const unsigned mask =
        1 << MDBX_GET_BOTH | 1 << MDBX_GET_BOTH_RANGE | 1 << MDBX_SET_KEY;
    if (unlikely(mask & (1 << move_op)))
      return MDBX_EINVAL;
    data = &stub;
  }

  if (key == NULL) {
    const unsigned mask = 1 << MDBX_GET_BOTH | 1 << MDBX_GET_BOTH_RANGE |
                          1 << MDBX_SET_KEY | 1 << MDBX_SET |
                          1 << MDBX_SET_RANGE;
    if (unlikely(mask & (1 << move_op)))
      return MDBX_EINVAL;
    key = &stub;
  }

  next.outer.mc_signature = MDBX_MC_LIVE;
  rc = mdbx_cursor_get(&next.outer, key, data, move_op);
  if (unlikely(rc != MDBX_SUCCESS &&
               (rc != MDBX_NOTFOUND || !(next.outer.mc_flags & C_INITIALIZED))))
    return rc;

  return mdbx_estimate_distance(cursor, &next.outer, distance_items);
}

int mdbx_estimate_range(MDBX_txn *txn, MDBX_dbi dbi, MDBX_val *begin_key,
                        MDBX_val *begin_data, MDBX_val *end_key,
                        MDBX_val *end_data, ptrdiff_t *size_items) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!size_items))
    return MDBX_EINVAL;

  if (unlikely(begin_data && (begin_key == NULL || begin_key == MDBX_EPSILON)))
    return MDBX_EINVAL;

  if (unlikely(end_data && (end_key == NULL || end_key == MDBX_EPSILON)))
    return MDBX_EINVAL;

  if (unlikely(begin_key == MDBX_EPSILON && end_key == MDBX_EPSILON))
    return MDBX_EINVAL;

  if (unlikely(!check_dbi(txn, dbi, DBI_USRVALID)))
    return MDBX_BAD_DBI;

  MDBX_cursor_couple begin;
  /* LY: first, initialize cursor to refresh a DB in case it have DB_STALE */
  rc = mdbx_cursor_init(&begin.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(begin.outer.mc_db->md_entries == 0)) {
    *size_items = 0;
    return MDBX_SUCCESS;
  }

  if (!begin_key) {
    if (unlikely(!end_key)) {
      /* LY: FIRST..LAST case */
      *size_items = (ptrdiff_t)begin.outer.mc_db->md_entries;
      return MDBX_SUCCESS;
    }
    MDBX_val stub = {0, 0};
    rc = mdbx_cursor_first(&begin.outer, &stub, &stub);
    if (unlikely(end_key == MDBX_EPSILON)) {
      /* LY: FIRST..+epsilon case */
      return (rc == MDBX_SUCCESS)
                 ? mdbx_cursor_count(&begin.outer, (size_t *)size_items)
                 : rc;
    }
  } else {
    if (unlikely(begin_key == MDBX_EPSILON)) {
      if (end_key == NULL) {
        /* LY: -epsilon..LAST case */
        MDBX_val stub = {0, 0};
        rc = mdbx_cursor_last(&begin.outer, &stub, &stub);
        return (rc == MDBX_SUCCESS)
                   ? mdbx_cursor_count(&begin.outer, (size_t *)size_items)
                   : rc;
      }
      /* LY: -epsilon..value case */
      assert(end_key != MDBX_EPSILON);
      begin_key = end_key;
    } else if (unlikely(end_key == MDBX_EPSILON)) {
      /* LY: value..+epsilon case */
      assert(begin_key != MDBX_EPSILON);
      end_key = begin_key;
    }
    if (end_key && !begin_data && !end_data &&
        (begin_key == end_key ||
         begin.outer.mc_dbx->md_cmp(begin_key, end_key) == 0)) {
      /* LY: single key case */
      rc = mdbx_cursor_set(&begin.outer, begin_key, NULL, MDBX_SET).err;
      if (unlikely(rc != MDBX_SUCCESS)) {
        *size_items = 0;
        return (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
      }
      *size_items = 1;
      if (begin.outer.mc_xcursor != NULL) {
        MDBX_node *node = page_node(begin.outer.mc_pg[begin.outer.mc_top],
                                    begin.outer.mc_ki[begin.outer.mc_top]);
        if (F_ISSET(node_flags(node), F_DUPDATA)) {
          /* LY: return the number of duplicates for given key */
          mdbx_tassert(txn,
                       begin.outer.mc_xcursor == &begin.inner &&
                           (begin.inner.mx_cursor.mc_flags & C_INITIALIZED));
          *size_items =
              (sizeof(*size_items) >= sizeof(begin.inner.mx_db.md_entries) ||
               begin.inner.mx_db.md_entries <= PTRDIFF_MAX)
                  ? (size_t)begin.inner.mx_db.md_entries
                  : PTRDIFF_MAX;
        }
      }
      return MDBX_SUCCESS;
    } else {
      rc = mdbx_cursor_set(&begin.outer, begin_key, begin_data,
                           begin_data ? MDBX_GET_BOTH_RANGE : MDBX_SET_RANGE)
               .err;
    }
  }

  if (unlikely(rc != MDBX_SUCCESS)) {
    if (rc != MDBX_NOTFOUND || !(begin.outer.mc_flags & C_INITIALIZED))
      return rc;
  }

  MDBX_cursor_couple end;
  rc = mdbx_cursor_init(&end.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  if (!end_key) {
    MDBX_val stub = {0, 0};
    rc = mdbx_cursor_last(&end.outer, &stub, &stub);
  } else {
    rc = mdbx_cursor_set(&end.outer, end_key, end_data,
                         end_data ? MDBX_GET_BOTH_RANGE : MDBX_SET_RANGE)
             .err;
  }
  if (unlikely(rc != MDBX_SUCCESS)) {
    if (rc != MDBX_NOTFOUND || !(end.outer.mc_flags & C_INITIALIZED))
      return rc;
  }

  rc = mdbx_estimate_distance(&begin.outer, &end.outer, size_items);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  assert(*size_items >= -(ptrdiff_t)begin.outer.mc_db->md_entries &&
         *size_items <= (ptrdiff_t)begin.outer.mc_db->md_entries);

#if 0 /* LY: Was decided to returns as-is (i.e. negative) the estimation       \
       * results for an inverted ranges. */

  /* Commit 8ddfd1f34ad7cf7a3c4aa75d2e248ca7e639ed63
     Change-Id: If59eccf7311123ab6384c4b93f9b1fed5a0a10d1 */

  if (*size_items < 0) {
    /* LY: inverted range case */
    *size_items += (ptrdiff_t)begin.outer.mc_db->md_entries;
  } else if (*size_items == 0 && begin_key && end_key) {
    int cmp = begin.outer.mc_dbx->md_cmp(&origin_begin_key, &origin_end_key);
    if (cmp == 0 && (begin.inner.mx_cursor.mc_flags & C_INITIALIZED) &&
        begin_data && end_data)
      cmp = begin.outer.mc_dbx->md_dcmp(&origin_begin_data, &origin_end_data);
    if (cmp > 0) {
      /* LY: inverted range case with empty scope */
      *size_items = (ptrdiff_t)begin.outer.mc_db->md_entries;
    }
  }
  assert(*size_items >= 0 &&
         *size_items <= (ptrdiff_t)begin.outer.mc_db->md_entries);
#endif

  return MDBX_SUCCESS;
}

//------------------------------------------------------------------------------

/* Позволяет обновить или удалить существующую запись с получением
 * в old_data предыдущего значения данных. При этом если new_data равен
 * нулю, то выполняется удаление, иначе обновление/вставка.
 *
 * Текущее значение может находиться в уже измененной (грязной) странице.
 * В этом случае страница будет перезаписана при обновлении, а само старое
 * значение утрачено. Поэтому исходно в old_data должен быть передан
 * дополнительный буфер для копирования старого значения.
 * Если переданный буфер слишком мал, то функция вернет -1, установив
 * old_data->iov_len в соответствующее значение.
 *
 * Для не-уникальных ключей также возможен второй сценарий использования,
 * когда посредством old_data из записей с одинаковым ключом для
 * удаления/обновления выбирается конкретная. Для выбора этого сценария
 * во flags следует одновременно указать MDBX_CURRENT и MDBX_NOOVERWRITE.
 * Именно эта комбинация выбрана, так как она лишена смысла, и этим позволяет
 * идентифицировать запрос такого сценария.
 *
 * Функция может быть замещена соответствующими операциями с курсорами
 * после двух доработок (TODO):
 *  - внешняя аллокация курсоров, в том числе на стеке (без malloc).
 *  - получения dirty-статуса страницы по адресу (знать о MUTABLE/WRITEABLE).
 */

int mdbx_replace_ex(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key,
                    MDBX_val *new_data, MDBX_val *old_data,
                    MDBX_put_flags_t flags, MDBX_preserve_func preserver,
                    void *preserver_context) {
  int rc = check_txn_rw(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key || !old_data || old_data == new_data))
    return MDBX_EINVAL;

  if (unlikely(old_data->iov_base == NULL && old_data->iov_len))
    return MDBX_EINVAL;

  if (unlikely(new_data == NULL &&
               (flags & (MDBX_CURRENT | MDBX_RESERVE)) != MDBX_CURRENT))
    return MDBX_EINVAL;

  if (unlikely(!check_dbi(txn, dbi, DBI_USRVALID)))
    return MDBX_BAD_DBI;

  if (unlikely(flags &
               ~(MDBX_NOOVERWRITE | MDBX_NODUPDATA | MDBX_ALLDUPS |
                 MDBX_RESERVE | MDBX_APPEND | MDBX_APPENDDUP | MDBX_CURRENT)))
    return MDBX_EINVAL;

  MDBX_cursor_couple cx;
  rc = mdbx_cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  cx.outer.mc_next = txn->tw.cursors[dbi];
  txn->tw.cursors[dbi] = &cx.outer;

  MDBX_val present_key = *key;
  if (F_ISSET(flags, MDBX_CURRENT | MDBX_NOOVERWRITE)) {
    /* в old_data значение для выбора конкретного дубликата */
    if (unlikely(!(txn->mt_dbs[dbi].md_flags & MDBX_DUPSORT))) {
      rc = MDBX_EINVAL;
      goto bailout;
    }

    /* убираем лишний бит, он был признаком запрошенного режима */
    flags -= MDBX_NOOVERWRITE;

    rc = mdbx_cursor_get(&cx.outer, &present_key, old_data, MDBX_GET_BOTH);
    if (rc != MDBX_SUCCESS)
      goto bailout;
  } else {
    /* в old_data буфер для сохранения предыдущего значения */
    if (unlikely(new_data && old_data->iov_base == new_data->iov_base))
      return MDBX_EINVAL;
    MDBX_val present_data;
    rc = mdbx_cursor_get(&cx.outer, &present_key, &present_data, MDBX_SET_KEY);
    if (unlikely(rc != MDBX_SUCCESS)) {
      old_data->iov_base = NULL;
      old_data->iov_len = 0;
      if (rc != MDBX_NOTFOUND || (flags & MDBX_CURRENT))
        goto bailout;
    } else if (flags & MDBX_NOOVERWRITE) {
      rc = MDBX_KEYEXIST;
      *old_data = present_data;
      goto bailout;
    } else {
      MDBX_page *page = cx.outer.mc_pg[cx.outer.mc_top];
      if (txn->mt_dbs[dbi].md_flags & MDBX_DUPSORT) {
        if (flags & MDBX_CURRENT) {
          /* disallow update/delete for multi-values */
          MDBX_node *node = page_node(page, cx.outer.mc_ki[cx.outer.mc_top]);
          if (F_ISSET(node_flags(node), F_DUPDATA)) {
            mdbx_tassert(txn, XCURSOR_INITED(&cx.outer) &&
                                  cx.outer.mc_xcursor->mx_db.md_entries > 1);
            if (cx.outer.mc_xcursor->mx_db.md_entries > 1) {
              rc = MDBX_EMULTIVAL;
              goto bailout;
            }
          }
          /* В оригинальной LMDB флажок MDBX_CURRENT здесь приведет
           * к замене данных без учета MDBX_DUPSORT сортировки,
           * но здесь это в любом случае допустимо, так как мы
           * проверили что для ключа есть только одно значение. */
        }
      }

      if (IS_MODIFIABLE(txn, page)) {
        if (new_data && cmp_lenfast(&present_data, new_data) == 0) {
          /* если данные совпадают, то ничего делать не надо */
          *old_data = *new_data;
          goto bailout;
        }
        rc = preserver ? preserver(preserver_context, old_data,
                                   present_data.iov_base, present_data.iov_len)
                       : MDBX_SUCCESS;
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      } else {
        *old_data = present_data;
      }
      flags |= MDBX_CURRENT;
    }
  }

  if (likely(new_data))
    rc = mdbx_cursor_put(&cx.outer, key, new_data, flags);
  else
    rc = mdbx_cursor_del(&cx.outer, flags & MDBX_ALLDUPS);

bailout:
  txn->tw.cursors[dbi] = cx.outer.mc_next;
  return rc;
}

static int default_value_preserver(void *context, MDBX_val *target,
                                   const void *src, size_t bytes) {
  (void)context;
  if (unlikely(target->iov_len < bytes)) {
    target->iov_base = nullptr;
    target->iov_len = bytes;
    return MDBX_RESULT_TRUE;
  }
  memcpy(target->iov_base, src, target->iov_len = bytes);
  return MDBX_SUCCESS;
}

int mdbx_replace(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key,
                 MDBX_val *new_data, MDBX_val *old_data,
                 MDBX_put_flags_t flags) {
  return mdbx_replace_ex(txn, dbi, key, new_data, old_data, flags,
                         default_value_preserver, nullptr);
}

/* Функция сообщает находится ли указанный адрес в "грязной" странице у
 * заданной пишущей транзакции. В конечном счете это позволяет избавиться от
 * лишнего копирования данных из НЕ-грязных страниц.
 *
 * "Грязные" страницы - это те, которые уже были изменены в ходе пишущей
 * транзакции. Соответственно, какие-либо дальнейшие изменения могут привести
 * к перезаписи таких страниц. Поэтому все функции, выполняющие изменения, в
 * качестве аргументов НЕ должны получать указатели на данные в таких
 * страницах. В свою очередь "НЕ грязные" страницы перед модификацией будут
 * скопированы.
 *
 * Другими словами, данные из "грязных" страниц должны быть либо скопированы
 * перед передачей в качестве аргументов для дальнейших модификаций, либо
 * отвергнуты на стадии проверки корректности аргументов.
 *
 * Таким образом, функция позволяет как избавится от лишнего копирования,
 * так и выполнить более полную проверку аргументов.
 *
 * ВАЖНО: Передаваемый указатель должен указывать на начало данных. Только
 * так гарантируется что актуальный заголовок страницы будет физически
 * расположен в той-же странице памяти, в том числе для многостраничных
 * P_OVERFLOW страниц с длинными данными. */
int mdbx_is_dirty(const MDBX_txn *txn, const void *ptr) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const MDBX_env *env = txn->mt_env;
  const ptrdiff_t offset = (uint8_t *)ptr - env->me_map;
  if (offset >= 0) {
    const pgno_t pgno = bytes2pgno(env, offset);
    if (likely(pgno < txn->mt_next_pgno)) {
      const MDBX_page *page = pgno2page(env, pgno);
      if (unlikely(page->mp_pgno != pgno ||
                   (page->mp_flags & P_ILL_BITS) != 0)) {
        /* The ptr pointed into middle of a large page,
         * not to the beginning of a data. */
        return MDBX_EINVAL;
      }
      return ((txn->mt_flags & MDBX_TXN_RDONLY) || !IS_MODIFIABLE(txn, page))
                 ? MDBX_RESULT_FALSE
                 : MDBX_RESULT_TRUE;
    }
    if ((size_t)offset < env->me_dxb_mmap.limit) {
      /* Указатель адресует что-то в пределах mmap, но за границей
       * распределенных страниц. Такое может случится если mdbx_is_dirty()
       * вызывается после операции, в ходе которой грязная страница была
       * возвращена в нераспределенное пространство. */
      return (txn->mt_flags & MDBX_TXN_RDONLY) ? MDBX_EINVAL : MDBX_RESULT_TRUE;
    }
  }

  /* Страница вне используемого mmap-диапазона, т.е. либо в функцию был
   * передан некорректный адрес, либо адрес в теневой странице, которая была
   * выделена посредством malloc().
   *
   * Для режима MDBX_WRITE_MAP режима страница однозначно "не грязная",
   * а для режимов без MDBX_WRITE_MAP однозначно "не чистая". */
  return (txn->mt_flags & (MDBX_WRITEMAP | MDBX_TXN_RDONLY)) ? MDBX_EINVAL
                                                             : MDBX_RESULT_TRUE;
}

int mdbx_dbi_sequence(MDBX_txn *txn, MDBX_dbi dbi, uint64_t *result,
                      uint64_t increment) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!check_dbi(txn, dbi, DBI_USRVALID)))
    return MDBX_BAD_DBI;

  if (unlikely(txn->mt_dbistate[dbi] & DBI_STALE)) {
    rc = mdbx_fetch_sdb(txn, dbi);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  MDBX_db *dbs = &txn->mt_dbs[dbi];
  if (likely(result))
    *result = dbs->md_seq;

  if (likely(increment > 0)) {
    if (unlikely(txn->mt_flags & MDBX_TXN_RDONLY))
      return MDBX_EACCESS;

    uint64_t new = dbs->md_seq + increment;
    if (unlikely(new < increment))
      return MDBX_RESULT_TRUE;

    mdbx_tassert(txn, new > dbs->md_seq);
    dbs->md_seq = new;
    txn->mt_flags |= MDBX_TXN_DIRTY;
    txn->mt_dbistate[dbi] |= DBI_DIRTY;
  }

  return MDBX_SUCCESS;
}

/*----------------------------------------------------------------------------*/

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
__cold MDBX_NOTHROW_CONST_FUNCTION intptr_t mdbx_limits_pgsize_min(void) {
  return __inline_mdbx_limits_pgsize_min();
}

__cold MDBX_NOTHROW_CONST_FUNCTION intptr_t mdbx_limits_pgsize_max(void) {
  return __inline_mdbx_limits_pgsize_max();
}
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

__cold intptr_t mdbx_limits_dbsize_min(intptr_t pagesize) {
  if (pagesize < 1)
    pagesize = (intptr_t)mdbx_default_pagesize();
  else if (unlikely(pagesize < (intptr_t)MIN_PAGESIZE ||
                    pagesize > (intptr_t)MAX_PAGESIZE ||
                    !is_powerof2((size_t)pagesize)))
    return -1;

  return MIN_PAGENO * pagesize;
}

__cold intptr_t mdbx_limits_dbsize_max(intptr_t pagesize) {
  if (pagesize < 1)
    pagesize = (intptr_t)mdbx_default_pagesize();
  else if (unlikely(pagesize < (intptr_t)MIN_PAGESIZE ||
                    pagesize > (intptr_t)MAX_PAGESIZE ||
                    !is_powerof2((size_t)pagesize)))
    return -1;

  STATIC_ASSERT(MAX_MAPSIZE < INTPTR_MAX);
  const uint64_t limit = (1 + (uint64_t)MAX_PAGENO) * pagesize;
  return (limit < (intptr_t)MAX_MAPSIZE) ? (intptr_t)limit
                                         : (intptr_t)MAX_MAPSIZE;
}

__cold intptr_t mdbx_limits_txnsize_max(intptr_t pagesize) {
  if (pagesize < 1)
    pagesize = (intptr_t)mdbx_default_pagesize();
  else if (unlikely(pagesize < (intptr_t)MIN_PAGESIZE ||
                    pagesize > (intptr_t)MAX_PAGESIZE ||
                    !is_powerof2((size_t)pagesize)))
    return -1;

  STATIC_ASSERT(MAX_MAPSIZE < INTPTR_MAX);
  const uint64_t pgl_limit =
      pagesize * (uint64_t)(MDBX_PGL_LIMIT / 1.6180339887498948482);
  const uint64_t map_limit = (uint64_t)(MAX_MAPSIZE / 1.6180339887498948482);
  return (pgl_limit < map_limit) ? (intptr_t)pgl_limit : (intptr_t)map_limit;
}

/*** Key-making functions to avoid custom comparators *************************/

static __always_inline double key2double(const int64_t key) {
  union {
    uint64_t u;
    double f;
  } casting;

  casting.u = (key < 0) ? key + UINT64_C(0x8000000000000000)
                        : UINT64_C(0xffffFFFFffffFFFF) - key;
  return casting.f;
}

static __always_inline uint64_t double2key(const double *const ptr) {
  STATIC_ASSERT(sizeof(double) == sizeof(int64_t));
  const int64_t i = *(const int64_t *)ptr;
  const uint64_t u = (i < 0) ? UINT64_C(0xffffFFFFffffFFFF) - i
                             : i + UINT64_C(0x8000000000000000);
  if (mdbx_assert_enabled()) {
    const double f = key2double(u);
    assert(memcmp(&f, ptr, 8) == 0);
  }
  return u;
}

static __always_inline float key2float(const int32_t key) {
  union {
    uint32_t u;
    float f;
  } casting;

  casting.u =
      (key < 0) ? key + UINT32_C(0x80000000) : UINT32_C(0xffffFFFF) - key;
  return casting.f;
}

static __always_inline uint32_t float2key(const float *const ptr) {
  STATIC_ASSERT(sizeof(float) == sizeof(int32_t));
  const int32_t i = *(const int32_t *)ptr;
  const uint32_t u =
      (i < 0) ? UINT32_C(0xffffFFFF) - i : i + UINT32_C(0x80000000);
  if (mdbx_assert_enabled()) {
    const float f = key2float(u);
    assert(memcmp(&f, ptr, 4) == 0);
  }
  return u;
}

uint64_t mdbx_key_from_double(const double ieee754_64bit) {
  return double2key(&ieee754_64bit);
}

uint64_t mdbx_key_from_ptrdouble(const double *const ieee754_64bit) {
  return double2key(ieee754_64bit);
}

uint32_t mdbx_key_from_float(const float ieee754_32bit) {
  return float2key(&ieee754_32bit);
}

uint32_t mdbx_key_from_ptrfloat(const float *const ieee754_32bit) {
  return float2key(ieee754_32bit);
}

#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API
MDBX_NOTHROW_CONST_FUNCTION uint64_t mdbx_key_from_int64(const int64_t i64) {
  return __inline_mdbx_key_from_int64(i64);
}

MDBX_NOTHROW_CONST_FUNCTION uint32_t mdbx_key_from_int32(const int32_t i32) {
  return __inline_mdbx_key_from_int32(i32);
}
#endif /* LIBMDBX_NO_EXPORTS_LEGACY_API */

#define IEEE754_DOUBLE_MANTISSA_SIZE 52
#define IEEE754_DOUBLE_EXPONENTA_BIAS 0x3FF
#define IEEE754_DOUBLE_EXPONENTA_MAX 0x7FF
#define IEEE754_DOUBLE_IMPLICIT_LEAD UINT64_C(0x0010000000000000)
#define IEEE754_DOUBLE_MANTISSA_MASK UINT64_C(0x000FFFFFFFFFFFFF)
#define IEEE754_DOUBLE_MANTISSA_AMAX UINT64_C(0x001FFFFFFFFFFFFF)

static __inline int clz64(uint64_t value) {
#if __GNUC_PREREQ(4, 1) || __has_builtin(__builtin_clzl)
  if (sizeof(value) == sizeof(int))
    return __builtin_clz(value);
  if (sizeof(value) == sizeof(long))
    return __builtin_clzl(value);
#if (defined(__SIZEOF_LONG_LONG__) && __SIZEOF_LONG_LONG__ == 8) ||            \
    __has_builtin(__builtin_clzll)
  return __builtin_clzll(value);
#endif /* have(long long) && long long == uint64_t */
#endif /* GNU C */

#if defined(_MSC_VER)
  unsigned long index;
#if defined(_M_AMD64) || defined(_M_ARM64) || defined(_M_X64)
  _BitScanReverse64(&index, value);
  return 63 - index;
#else
  if (value > UINT32_MAX) {
    _BitScanReverse(&index, (uint32_t)(value >> 32));
    return 31 - index;
  }
  _BitScanReverse(&index, (uint32_t)value);
  return 63 - index;
#endif
#endif /* MSVC */

  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  value |= value >> 32;
  static const uint8_t debruijn_clz64[64] = {
      63, 16, 62, 7,  15, 36, 61, 3,  6,  14, 22, 26, 35, 47, 60, 2,
      9,  5,  28, 11, 13, 21, 42, 19, 25, 31, 34, 40, 46, 52, 59, 1,
      17, 8,  37, 4,  23, 27, 48, 10, 29, 12, 43, 20, 32, 41, 53, 18,
      38, 24, 49, 30, 44, 33, 54, 39, 50, 45, 55, 51, 56, 57, 58, 0};
  return debruijn_clz64[value * UINT64_C(0x03F79D71B4CB0A89) >> 58];
}

static __inline uint64_t round_mantissa(const uint64_t u64, int shift) {
  assert(shift < 0 && u64 > 0);
  shift = -shift;
  const unsigned half = 1 << (shift - 1);
  const unsigned lsb = 1 & (unsigned)(u64 >> shift);
  const unsigned tie2even = 1 ^ lsb;
  return (u64 + half - tie2even) >> shift;
}

uint64_t mdbx_key_from_jsonInteger(const int64_t json_integer) {
  const uint64_t bias = UINT64_C(0x8000000000000000);
  if (json_integer > 0) {
    const uint64_t u64 = json_integer;
    int shift = clz64(u64) - (64 - IEEE754_DOUBLE_MANTISSA_SIZE - 1);
    uint64_t mantissa = u64 << shift;
    if (unlikely(shift < 0)) {
      mantissa = round_mantissa(u64, shift);
      if (mantissa > IEEE754_DOUBLE_MANTISSA_AMAX)
        mantissa = round_mantissa(u64, --shift);
    }

    assert(mantissa >= IEEE754_DOUBLE_IMPLICIT_LEAD &&
           mantissa <= IEEE754_DOUBLE_MANTISSA_AMAX);
    const uint64_t exponent =
        IEEE754_DOUBLE_EXPONENTA_BIAS + IEEE754_DOUBLE_MANTISSA_SIZE - shift;
    assert(exponent > 0 && exponent <= IEEE754_DOUBLE_EXPONENTA_MAX);
    const uint64_t key = bias + (exponent << IEEE754_DOUBLE_MANTISSA_SIZE) +
                         (mantissa - IEEE754_DOUBLE_IMPLICIT_LEAD);
#if !defined(_MSC_VER) ||                                                      \
    defined(                                                                   \
        _DEBUG) /* Workaround for MSVC error LNK2019: unresolved external      \
                   symbol __except1 referenced in function __ftol3_except */
    assert(key == mdbx_key_from_double((double)json_integer));
#endif /* Workaround for MSVC */
    return key;
  }

  if (json_integer < 0) {
    const uint64_t u64 = -json_integer;
    int shift = clz64(u64) - (64 - IEEE754_DOUBLE_MANTISSA_SIZE - 1);
    uint64_t mantissa = u64 << shift;
    if (unlikely(shift < 0)) {
      mantissa = round_mantissa(u64, shift);
      if (mantissa > IEEE754_DOUBLE_MANTISSA_AMAX)
        mantissa = round_mantissa(u64, --shift);
    }

    assert(mantissa >= IEEE754_DOUBLE_IMPLICIT_LEAD &&
           mantissa <= IEEE754_DOUBLE_MANTISSA_AMAX);
    const uint64_t exponent =
        IEEE754_DOUBLE_EXPONENTA_BIAS + IEEE754_DOUBLE_MANTISSA_SIZE - shift;
    assert(exponent > 0 && exponent <= IEEE754_DOUBLE_EXPONENTA_MAX);
    const uint64_t key = bias - 1 - (exponent << IEEE754_DOUBLE_MANTISSA_SIZE) -
                         (mantissa - IEEE754_DOUBLE_IMPLICIT_LEAD);
#if !defined(_MSC_VER) ||                                                      \
    defined(                                                                   \
        _DEBUG) /* Workaround for MSVC error LNK2019: unresolved external      \
                   symbol __except1 referenced in function __ftol3_except */
    assert(key == mdbx_key_from_double((double)json_integer));
#endif /* Workaround for MSVC */
    return key;
  }

  return bias;
}

int64_t mdbx_jsonInteger_from_key(const MDBX_val v) {
  assert(v.iov_len == 8);
  const uint64_t key = unaligned_peek_u64(2, v.iov_base);
  const uint64_t bias = UINT64_C(0x8000000000000000);
  const uint64_t covalent = (key > bias) ? key - bias : bias - key - 1;
  const int shift = IEEE754_DOUBLE_EXPONENTA_BIAS + 63 -
                    (IEEE754_DOUBLE_EXPONENTA_MAX &
                     (int)(covalent >> IEEE754_DOUBLE_MANTISSA_SIZE));
  if (unlikely(shift < 1))
    return (key < bias) ? INT64_MIN : INT64_MAX;
  if (unlikely(shift > 63))
    return 0;

  const uint64_t unscaled = ((covalent & IEEE754_DOUBLE_MANTISSA_MASK)
                             << (63 - IEEE754_DOUBLE_MANTISSA_SIZE)) +
                            bias;
  const int64_t absolute = unscaled >> shift;
  const int64_t value = (key < bias) ? -absolute : absolute;
  assert(key == mdbx_key_from_jsonInteger(value) ||
         (mdbx_key_from_jsonInteger(value - 1) < key &&
          key < mdbx_key_from_jsonInteger(value + 1)));
  return value;
}

double mdbx_double_from_key(const MDBX_val v) {
  assert(v.iov_len == 8);
  return key2double(unaligned_peek_u64(2, v.iov_base));
}

float mdbx_float_from_key(const MDBX_val v) {
  assert(v.iov_len == 4);
  return key2float(unaligned_peek_u32(2, v.iov_base));
}

int32_t mdbx_int32_from_key(const MDBX_val v) {
  assert(v.iov_len == 4);
  return (int32_t)(unaligned_peek_u32(2, v.iov_base) - UINT32_C(0x80000000));
}

int64_t mdbx_int64_from_key(const MDBX_val v) {
  assert(v.iov_len == 8);
  return (int64_t)(unaligned_peek_u64(2, v.iov_base) -
                   UINT64_C(0x8000000000000000));
}

__cold MDBX_cmp_func *mdbx_get_keycmp(unsigned flags) {
  return get_default_keycmp(flags);
}

__cold MDBX_cmp_func *mdbx_get_datacmp(unsigned flags) {
  return get_default_datacmp(flags);
}

__cold int mdbx_env_set_option(MDBX_env *env, const MDBX_option_t option,
                               uint64_t value) {
  int err = check_env(env, false);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  const bool lock_needed = ((env->me_flags & MDBX_ENV_ACTIVE) && env->me_txn0 &&
                            env->me_txn0->mt_owner != mdbx_thread_self());
  bool should_unlock = false;
  switch (option) {
  case MDBX_opt_sync_bytes:
    if (value == UINT64_MAX)
      value = SIZE_MAX - 65536;
    if (unlikely(env->me_flags & MDBX_RDONLY))
      return MDBX_EACCESS;
    if (unlikely(!(env->me_flags & MDBX_ENV_ACTIVE)))
      return MDBX_EPERM;
    if (unlikely(value > SIZE_MAX - 65536))
      return MDBX_TOO_LARGE;
    if (atomic_store32(&env->me_lck->mti_autosync_threshold,
                       bytes2pgno(env, (size_t)value + env->me_psize - 1),
                       mo_Relaxed) != 0 &&
        (env->me_flags & MDBX_ENV_ACTIVE)) {
      err = mdbx_env_sync_poll(env);
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
      err = MDBX_SUCCESS;
    }
    break;

  case MDBX_opt_sync_period:
    if (value == UINT64_MAX)
      value = UINT32_MAX;
    if (unlikely(env->me_flags & MDBX_RDONLY))
      return MDBX_EACCESS;
    if (unlikely(!(env->me_flags & MDBX_ENV_ACTIVE)))
      return MDBX_EPERM;
    if (unlikely(value > UINT32_MAX))
      return MDBX_TOO_LARGE;
    if (atomic_store64(&env->me_lck->mti_autosync_period,
                       mdbx_osal_16dot16_to_monotime((uint32_t)value),
                       mo_Relaxed) != 0 &&
        (env->me_flags & MDBX_ENV_ACTIVE)) {
      err = mdbx_env_sync_poll(env);
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
      err = MDBX_SUCCESS;
    }
    break;

  case MDBX_opt_max_db:
    if (value == UINT64_MAX)
      value = MDBX_MAX_DBI;
    if (unlikely(value > MDBX_MAX_DBI))
      return MDBX_EINVAL;
    if (unlikely(env->me_map))
      return MDBX_EPERM;
    env->me_maxdbs = (unsigned)value + CORE_DBS;
    break;

  case MDBX_opt_max_readers:
    if (value == UINT64_MAX)
      value = MDBX_READERS_LIMIT;
    if (unlikely(value < 1 || value > MDBX_READERS_LIMIT))
      return MDBX_EINVAL;
    if (unlikely(env->me_map))
      return MDBX_EPERM;
    env->me_maxreaders = (unsigned)value;
    break;

  case MDBX_opt_dp_reserve_limit:
    if (value == UINT64_MAX)
      value = INT_MAX;
    if (unlikely(value > INT_MAX))
      return MDBX_EINVAL;
    if (env->me_options.dp_reserve_limit != (unsigned)value) {
      if (lock_needed) {
        err = mdbx_txn_lock(env, false);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
        should_unlock = true;
      }
      env->me_options.dp_reserve_limit = (unsigned)value;
      while (env->me_dp_reserve_len > env->me_options.dp_reserve_limit) {
        mdbx_assert(env, env->me_dp_reserve != NULL);
        MDBX_page *dp = env->me_dp_reserve;
        MDBX_ASAN_UNPOISON_MEMORY_REGION(dp, env->me_psize);
        VALGRIND_MAKE_MEM_DEFINED(&dp->mp_next, sizeof(dp->mp_next));
        env->me_dp_reserve = dp->mp_next;
        VALGRIND_MEMPOOL_FREE(env, dp);
        mdbx_free(dp);
        env->me_dp_reserve_len -= 1;
      }
    }
    break;

  case MDBX_opt_rp_augment_limit:
    if (value == UINT64_MAX)
      value = MDBX_PGL_LIMIT;
    if (unlikely(value > MDBX_PGL_LIMIT))
      return MDBX_EINVAL;
    env->me_options.rp_augment_limit = (unsigned)value;
    break;

  case MDBX_opt_txn_dp_limit:
  case MDBX_opt_txn_dp_initial:
    if (value == UINT64_MAX)
      value = MDBX_PGL_LIMIT;
    if (unlikely(value > MDBX_PGL_LIMIT || value < CURSOR_STACK * 4))
      return MDBX_EINVAL;
    if (unlikely(env->me_flags & MDBX_RDONLY))
      return MDBX_EACCESS;
    if (lock_needed) {
      err = mdbx_txn_lock(env, false);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      should_unlock = true;
    }
    if (env->me_txn)
      err = MDBX_EPERM /* unable change during transaction */;
    else {
      const pgno_t value32 = (pgno_t)value;
      if (option == MDBX_opt_txn_dp_initial &&
          env->me_options.dp_initial != value32) {
        env->me_options.dp_initial = value32;
        if (env->me_options.dp_limit < value32) {
          env->me_options.dp_limit = value32;
          env->me_options.flags.non_auto.dp_limit = 1;
        }
      }
      if (option == MDBX_opt_txn_dp_limit &&
          env->me_options.dp_limit != value32) {
        env->me_options.dp_limit = value32;
        env->me_options.flags.non_auto.dp_limit = 1;
        if (env->me_options.dp_initial > value32)
          env->me_options.dp_initial = value32;
      }
    }
    break;

  case MDBX_opt_spill_max_denominator:
    if (value == UINT64_MAX)
      value = 255;
    if (unlikely(value > 255))
      return MDBX_EINVAL;
    env->me_options.spill_max_denominator = (uint8_t)value;
    break;
  case MDBX_opt_spill_min_denominator:
    if (unlikely(value > 255))
      return MDBX_EINVAL;
    env->me_options.spill_min_denominator = (uint8_t)value;
    break;
  case MDBX_opt_spill_parent4child_denominator:
    if (unlikely(value > 255))
      return MDBX_EINVAL;
    env->me_options.spill_parent4child_denominator = (uint8_t)value;
    break;

  case MDBX_opt_loose_limit:
    if (value == UINT64_MAX)
      value = 255;
    if (unlikely(value > 255))
      return MDBX_EINVAL;
    env->me_options.dp_loose_limit = (uint8_t)value;
    break;

  case MDBX_opt_merge_threshold_16dot16_percent:
    if (value == UINT64_MAX)
      value = 32768;
    if (unlikely(value < 8192 || value > 32768))
      return MDBX_EINVAL;
    env->me_options.merge_threshold_16dot16_percent = (unsigned)value;
    recalculate_merge_threshold(env);
    break;

  default:
    return MDBX_EINVAL;
  }

  if (should_unlock)
    mdbx_txn_unlock(env);
  return err;
}

__cold int mdbx_env_get_option(const MDBX_env *env, const MDBX_option_t option,
                               uint64_t *pvalue) {
  int err = check_env(env, false);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  if (unlikely(!pvalue))
    return MDBX_EINVAL;

  switch (option) {
  case MDBX_opt_sync_bytes:
    if (unlikely(!(env->me_flags & MDBX_ENV_ACTIVE)))
      return MDBX_EPERM;
    *pvalue = pgno2bytes(
        env, atomic_load32(&env->me_lck->mti_autosync_threshold, mo_Relaxed));
    break;

  case MDBX_opt_sync_period:
    if (unlikely(!(env->me_flags & MDBX_ENV_ACTIVE)))
      return MDBX_EPERM;
    *pvalue = mdbx_osal_monotime_to_16dot16(
        atomic_load64(&env->me_lck->mti_autosync_period, mo_Relaxed));
    break;

  case MDBX_opt_max_db:
    *pvalue = env->me_maxdbs - CORE_DBS;
    break;

  case MDBX_opt_max_readers:
    *pvalue = env->me_maxreaders;
    break;

  case MDBX_opt_dp_reserve_limit:
    *pvalue = env->me_options.dp_reserve_limit;
    break;

  case MDBX_opt_rp_augment_limit:
    *pvalue = env->me_options.rp_augment_limit;
    break;

  case MDBX_opt_txn_dp_limit:
    *pvalue = env->me_options.dp_limit;
    break;
  case MDBX_opt_txn_dp_initial:
    *pvalue = env->me_options.dp_initial;
    break;

  case MDBX_opt_spill_max_denominator:
    *pvalue = env->me_options.spill_max_denominator;
    break;
  case MDBX_opt_spill_min_denominator:
    *pvalue = env->me_options.spill_min_denominator;
    break;
  case MDBX_opt_spill_parent4child_denominator:
    *pvalue = env->me_options.spill_parent4child_denominator;
    break;

  case MDBX_opt_loose_limit:
    *pvalue = env->me_options.dp_loose_limit;
    break;

  case MDBX_opt_merge_threshold_16dot16_percent:
    *pvalue = env->me_options.merge_threshold_16dot16_percent;
    break;

  default:
    return MDBX_EINVAL;
  }

  return MDBX_SUCCESS;
}

/*** Attribute support functions for Nexenta **********************************/
#ifdef MDBX_NEXENTA_ATTRS

static __inline int mdbx_attr_peek(MDBX_val *data, mdbx_attr_t *attrptr) {
  if (unlikely(data->iov_len < sizeof(mdbx_attr_t)))
    return MDBX_INCOMPATIBLE;

  if (likely(attrptr != NULL))
    *attrptr = *(mdbx_attr_t *)data->iov_base;
  data->iov_len -= sizeof(mdbx_attr_t);
  data->iov_base =
      likely(data->iov_len > 0) ? ((mdbx_attr_t *)data->iov_base) + 1 : NULL;

  return MDBX_SUCCESS;
}

static __inline int mdbx_attr_poke(MDBX_val *reserved, MDBX_val *data,
                                   mdbx_attr_t attr, MDBX_put_flags_t flags) {
  mdbx_attr_t *space = reserved->iov_base;
  if (flags & MDBX_RESERVE) {
    if (likely(data != NULL)) {
      data->iov_base = data->iov_len ? space + 1 : NULL;
    }
  } else {
    *space = attr;
    if (likely(data != NULL)) {
      memcpy(space + 1, data->iov_base, data->iov_len);
    }
  }

  return MDBX_SUCCESS;
}

int mdbx_cursor_get_attr(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data,
                         mdbx_attr_t *attrptr, MDBX_cursor_op op) {
  int rc = mdbx_cursor_get(mc, key, data, op);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_attr_peek(data, attrptr);
}

int mdbx_get_attr(MDBX_txn *txn, MDBX_dbi dbi, MDBX_val *key, MDBX_val *data,
                  uint64_t *attrptr) {
  int rc = mdbx_get(txn, dbi, key, data);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_attr_peek(data, attrptr);
}

int mdbx_put_attr(MDBX_txn *txn, MDBX_dbi dbi, MDBX_val *key, MDBX_val *data,
                  mdbx_attr_t attr, MDBX_put_flags_t flags) {
  MDBX_val reserve;
  reserve.iov_base = NULL;
  reserve.iov_len = (data ? data->iov_len : 0) + sizeof(mdbx_attr_t);

  int rc = mdbx_put(txn, dbi, key, &reserve, flags | MDBX_RESERVE);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_attr_poke(&reserve, data, attr, flags);
}

int mdbx_cursor_put_attr(MDBX_cursor *cursor, MDBX_val *key, MDBX_val *data,
                         mdbx_attr_t attr, MDBX_put_flags_t flags) {
  MDBX_val reserve;
  reserve.iov_base = NULL;
  reserve.iov_len = (data ? data->iov_len : 0) + sizeof(mdbx_attr_t);

  int rc = mdbx_cursor_put(cursor, key, &reserve, flags | MDBX_RESERVE);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_attr_poke(&reserve, data, attr, flags);
}

int mdbx_set_attr(MDBX_txn *txn, MDBX_dbi dbi, MDBX_val *key, MDBX_val *data,
                  mdbx_attr_t attr) {
  if (unlikely(!key || !txn))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_signature != MDBX_MT_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(!TXN_DBI_EXIST(txn, dbi, DB_USRVALID)))
    return MDBX_BAD_DBI;

  if (unlikely(txn->mt_flags & (MDBX_TXN_RDONLY | MDBX_TXN_BLOCKED)))
    return (txn->mt_flags & MDBX_TXN_RDONLY) ? MDBX_EACCESS : MDBX_BAD_TXN;

  MDBX_cursor_couple cx;
  MDBX_val old_data;
  int rc = mdbx_cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  rc = mdbx_cursor_set(&cx.outer, key, &old_data, MDBX_SET, NULL);
  if (unlikely(rc != MDBX_SUCCESS)) {
    if (rc == MDBX_NOTFOUND && data) {
      cx.outer.mc_next = txn->tw.cursors[dbi];
      txn->tw.cursors[dbi] = &cx.outer;
      rc = mdbx_cursor_put_attr(&cx.outer, key, data, attr, 0);
      txn->tw.cursors[dbi] = cx.outer.mc_next;
    }
    return rc;
  }

  mdbx_attr_t old_attr = 0;
  rc = mdbx_attr_peek(&old_data, &old_attr);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (old_attr == attr && (!data || (data->iov_len == old_data.iov_len &&
                                     memcmp(data->iov_base, old_data.iov_base,
                                            old_data.iov_len) == 0)))
    return MDBX_SUCCESS;

  cx.outer.mc_next = txn->tw.cursors[dbi];
  txn->tw.cursors[dbi] = &cx.outer;
  rc = mdbx_cursor_put_attr(&cx.outer, key, data ? data : &old_data, attr,
                            MDBX_CURRENT);
  txn->tw.cursors[dbi] = cx.outer.mc_next;
  return rc;
}
#endif /* MDBX_NEXENTA_ATTRS */

/******************************************************************************/
/* *INDENT-OFF* */
/* clang-format off */

__dll_export
#ifdef __attribute_used__
    __attribute_used__
#elif defined(__GNUC__) || __has_attribute(__used__)
    __attribute__((__used__))
#endif
#ifdef __attribute_externally_visible__
        __attribute_externally_visible__
#elif (defined(__GNUC__) && !defined(__clang__)) ||                            \
    __has_attribute(__externally_visible__)
    __attribute__((__externally_visible__))
#endif
    const struct MDBX_build_info mdbx_build = {
#ifdef MDBX_BUILD_TIMESTAMP
    MDBX_BUILD_TIMESTAMP
#else
    "\"" __DATE__ " " __TIME__ "\""
#endif /* MDBX_BUILD_TIMESTAMP */

    ,
#ifdef MDBX_BUILD_TARGET
    MDBX_BUILD_TARGET
#else
  #if defined(__ANDROID_API__)
    "Android" MDBX_STRINGIFY(__ANDROID_API__)
  #elif defined(__linux__) || defined(__gnu_linux__)
    "Linux"
  #elif defined(EMSCRIPTEN) || defined(__EMSCRIPTEN__)
    "webassembly"
  #elif defined(__CYGWIN__)
    "CYGWIN"
  #elif defined(_WIN64) || defined(_WIN32) || defined(__TOS_WIN__) \
      || defined(__WINDOWS__)
    "Windows"
  #elif defined(__APPLE__)
    #if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) \
      || (defined(TARGET_IPHONE_SIMULATOR) && TARGET_IPHONE_SIMULATOR)
      "iOS"
    #else
      "MacOS"
    #endif
  #elif defined(__FreeBSD__)
    "FreeBSD"
  #elif defined(__DragonFly__)
    "DragonFlyBSD"
  #elif defined(__NetBSD__)
    "NetBSD"
  #elif defined(__OpenBSD__)
    "OpenBSD"
  #elif defined(__bsdi__)
    "UnixBSDI"
  #elif defined(__MACH__)
    "MACH"
  #elif (defined(_HPUX_SOURCE) || defined(__hpux) || defined(__HP_aCC))
    "HPUX"
  #elif defined(_AIX)
    "AIX"
  #elif defined(__sun) && defined(__SVR4)
    "Solaris"
  #elif defined(__BSD__) || defined(BSD)
    "UnixBSD"
  #elif defined(__unix__) || defined(UNIX) || defined(__unix) \
      || defined(__UNIX) || defined(__UNIX__)
    "UNIX"
  #elif defined(_POSIX_VERSION)
    "POSIX" MDBX_STRINGIFY(_POSIX_VERSION)
  #else
    "UnknownOS"
  #endif /* Target OS */

    "-"

  #if defined(__amd64__)
    "AMD64"
  #elif defined(__ia32__)
    "IA32"
  #elif defined(__e2k__) || defined(__elbrus__)
    "Elbrus"
  #elif defined(__alpha__) || defined(__alpha) || defined(_M_ALPHA)
    "Alpha"
  #elif defined(__aarch64__) || defined(_M_ARM64)
    "ARM64"
  #elif defined(__arm__) || defined(__thumb__) || defined(__TARGET_ARCH_ARM) \
      || defined(__TARGET_ARCH_THUMB) || defined(_ARM) || defined(_M_ARM) \
      || defined(_M_ARMT) || defined(__arm)
    "ARM"
  #elif defined(__mips64) || defined(__mips64__) || (defined(__mips) && (__mips >= 64))
    "MIPS64"
  #elif defined(__mips__) || defined(__mips) || defined(_R4000) || defined(__MIPS__)
    "MIPS"
  #elif defined(__hppa64__) || defined(__HPPA64__) || defined(__hppa64)
    "PARISC64"
  #elif defined(__hppa__) || defined(__HPPA__) || defined(__hppa)
    "PARISC"
  #elif defined(__ia64__) || defined(__ia64) || defined(_IA64) \
      || defined(__IA64__) || defined(_M_IA64) || defined(__itanium__)
    "Itanium"
  #elif defined(__powerpc64__) || defined(__ppc64__) || defined(__ppc64) \
      || defined(__powerpc64) || defined(_ARCH_PPC64)
    "PowerPC64"
  #elif defined(__powerpc__) || defined(__ppc__) || defined(__powerpc) \
      || defined(__ppc) || defined(_ARCH_PPC) || defined(__PPC__) || defined(__POWERPC__)
    "PowerPC"
  #elif defined(__sparc64__) || defined(__sparc64)
    "SPARC64"
  #elif defined(__sparc__) || defined(__sparc)
    "SPARC"
  #elif defined(__s390__) || defined(__s390) || defined(__zarch__) || defined(__zarch)
    "S390"
  #else
    "UnknownARCH"
  #endif
#endif /* MDBX_BUILD_TARGET */

#ifdef MDBX_BUILD_TYPE
# if defined(_MSC_VER)
#   pragma message("Configuration-depended MDBX_BUILD_TYPE: " MDBX_BUILD_TYPE)
# endif
    "-" MDBX_BUILD_TYPE
#endif /* MDBX_BUILD_TYPE */
    ,
    "MDBX_DEBUG=" MDBX_STRINGIFY(MDBX_DEBUG)
    " MDBX_WORDBITS=" MDBX_STRINGIFY(MDBX_WORDBITS)
    " BYTE_ORDER="
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    "LITTLE_ENDIAN"
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    "BIG_ENDIAN"
#else
    #error "FIXME: Unsupported byte order"
#endif /* __BYTE_ORDER__ */
    " MDBX_ENV_CHECKPID=" MDBX_ENV_CHECKPID_CONFIG
    " MDBX_TXN_CHECKOWNER=" MDBX_TXN_CHECKOWNER_CONFIG
    " MDBX_64BIT_ATOMIC=" MDBX_64BIT_ATOMIC_CONFIG
    " MDBX_64BIT_CAS=" MDBX_64BIT_CAS_CONFIG
    " MDBX_TRUST_RTC=" MDBX_TRUST_RTC_CONFIG
    " MDBX_ENABLE_REFUND=" MDBX_STRINGIFY(MDBX_ENABLE_REFUND)
    " MDBX_ENABLE_MADVISE=" MDBX_STRINGIFY(MDBX_ENABLE_MADVISE)
#if MDBX_DISABLE_PAGECHECKS
    " MDBX_DISABLE_PAGECHECKS=YES"
#endif /* MDBX_DISABLE_PAGECHECKS */
#ifdef __SANITIZE_ADDRESS__
    " SANITIZE_ADDRESS=YES"
#endif /* __SANITIZE_ADDRESS__ */
#ifdef MDBX_USE_VALGRIND
    " MDBX_USE_VALGRIND=YES"
#endif /* MDBX_USE_VALGRIND */
#if MDBX_FORCE_ASSERTIONS
    " MDBX_FORCE_ASSERTIONS=YES"
#endif /* MDBX_FORCE_ASSERTIONS */
#ifdef _GNU_SOURCE
    " _GNU_SOURCE=YES"
#else
    " _GNU_SOURCE=NO"
#endif /* _GNU_SOURCE */
#ifdef __APPLE__
    " MDBX_OSX_SPEED_INSTEADOF_DURABILITY=" MDBX_STRINGIFY(MDBX_OSX_SPEED_INSTEADOF_DURABILITY)
#endif /* MacOS */
#if defined(_WIN32) || defined(_WIN64)
    " MDBX_WITHOUT_MSVC_CRT=" MDBX_STRINGIFY(MDBX_AVOID_CRT)
    " MDBX_BUILD_SHARED_LIBRARY=" MDBX_STRINGIFY(MDBX_BUILD_SHARED_LIBRARY)
#if !MDBX_BUILD_SHARED_LIBRARY
    " MDBX_MANUAL_MODULE_HANDLER=" MDBX_STRINGIFY(MDBX_MANUAL_MODULE_HANDLER)
#endif
    " WINVER=" MDBX_STRINGIFY(WINVER)
#else /* Windows */
    " MDBX_LOCKING=" MDBX_LOCKING_CONFIG
    " MDBX_USE_OFDLOCKS=" MDBX_USE_OFDLOCKS_CONFIG
#endif /* !Windows */
    " MDBX_CACHELINE_SIZE=" MDBX_STRINGIFY(MDBX_CACHELINE_SIZE)
    " MDBX_CPU_WRITEBACK_INCOHERENT=" MDBX_STRINGIFY(MDBX_CPU_WRITEBACK_INCOHERENT)
    " MDBX_MMAP_INCOHERENT_CPU_CACHE=" MDBX_STRINGIFY(MDBX_MMAP_INCOHERENT_CPU_CACHE)
    " MDBX_MMAP_INCOHERENT_FILE_WRITE=" MDBX_STRINGIFY(MDBX_MMAP_INCOHERENT_FILE_WRITE)
    " MDBX_UNALIGNED_OK=" MDBX_STRINGIFY(MDBX_UNALIGNED_OK)
    " MDBX_PNL_ASCENDING=" MDBX_STRINGIFY(MDBX_PNL_ASCENDING)
    ,
#ifdef MDBX_BUILD_COMPILER
    MDBX_BUILD_COMPILER
#else
  #ifdef __INTEL_COMPILER
    "Intel C/C++ " MDBX_STRINGIFY(__INTEL_COMPILER)
  #elif defined(__apple_build_version__)
    "Apple clang " MDBX_STRINGIFY(__apple_build_version__)
  #elif defined(__ibmxl__)
    "IBM clang C " MDBX_STRINGIFY(__ibmxl_version__) "." MDBX_STRINGIFY(__ibmxl_release__)
    "." MDBX_STRINGIFY(__ibmxl_modification__) "." MDBX_STRINGIFY(__ibmxl_ptf_fix_level__)
  #elif defined(__clang__)
    "clang " MDBX_STRINGIFY(__clang_version__)
  #elif defined(__MINGW64__)
    "MINGW-64 " MDBX_STRINGIFY(__MINGW64_MAJOR_VERSION) "." MDBX_STRINGIFY(__MINGW64_MINOR_VERSION)
  #elif defined(__MINGW32__)
    "MINGW-32 " MDBX_STRINGIFY(__MINGW32_MAJOR_VERSION) "." MDBX_STRINGIFY(__MINGW32_MINOR_VERSION)
  #elif defined(__IBMC__)
    "IBM C " MDBX_STRINGIFY(__IBMC__)
  #elif defined(__GNUC__)
    "GNU C/C++ "
    #ifdef __VERSION__
      __VERSION__
    #else
      MDBX_STRINGIFY(__GNUC__) "." MDBX_STRINGIFY(__GNUC_MINOR__) "." MDBX_STRINGIFY(__GNUC_PATCHLEVEL__)
    #endif
  #elif defined(_MSC_VER)
    "MSVC " MDBX_STRINGIFY(_MSC_FULL_VER) "-" MDBX_STRINGIFY(_MSC_BUILD)
  #else
    "Unknown compiler"
  #endif
#endif /* MDBX_BUILD_COMPILER */
    ,
#ifdef MDBX_BUILD_FLAGS_CONFIG
    MDBX_BUILD_FLAGS_CONFIG
#endif /* MDBX_BUILD_FLAGS_CONFIG */
#ifdef MDBX_BUILD_FLAGS
    MDBX_BUILD_FLAGS
#endif /* MDBX_BUILD_FLAGS */
#if !(defined(MDBX_BUILD_FLAGS_CONFIG) || defined(MDBX_BUILD_FLAGS))
    "undefined (please use correct build script)"
#ifdef _MSC_VER
#pragma message("warning: Build flags undefined. Please use correct build script")
#else
#warning "Build flags undefined. Please use correct build script"
#endif // _MSC_VER
#endif
};

#ifdef __SANITIZE_ADDRESS__
LIBMDBX_API __attribute__((__weak__)) const char *__asan_default_options() {
  return "symbolize=1:allow_addr2line=1:"
#if MDBX_DEBUG
         "debug=1:"
         "verbosity=2:"
#endif /* MDBX_DEBUG */
         "log_threads=1:"
         "report_globals=1:"
         "replace_str=1:replace_intrin=1:"
         "malloc_context_size=9:"
         "detect_leaks=1:"
         "check_printf=1:"
         "detect_deadlocks=1:"
#ifndef LTO_ENABLED
         "check_initialization_order=1:"
#endif
         "detect_stack_use_after_return=1:"
         "intercept_tls_get_addr=1:"
         "decorate_proc_maps=1:"
         "abort_on_error=1";
}
#endif /* __SANITIZE_ADDRESS__ */

/* *INDENT-ON* */
/* clang-format on */
