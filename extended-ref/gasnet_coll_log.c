/*   $Source: bitbucket.org:berkeleylab/gasnet.git/extended-ref/gasnet_coll_log.c $
 * Description: PROOF-OF-CONCEPT implementation of collectives using only logrithmic team storage
 * Copyright 2015, Lawrence Berkeley National Laboratory
 * Terms of use are as specified in license.txt
 */

#include <gasnet_internal.h>
#include <gasnet_coll_internal.h>

/* NOTES:
 *
 * This is a PROOF-OF-CONCEPT implementation of collectives using only logrithmic team storage.
 *
 * These issues may need some attention:
 * + The code assumes gasnet_node_t is never larger than uint32_t
 * + The p2p code is using O(P), rather than O(log(P)) storage
 *   This can currently be fixed only at the expense of expanding the p2p API
 * + The scratch code is using O(P) for (at least) team->scratch_segs[].
 *   This can be fixed only if all callers are modified.
 * + Only implemented for SEQ (since endpoints will replace images)
 * + Only implemented as LOCAL (since offset-based addrs will replace SINGLE).
 *   Unlike SEQ-v-PAR, COLL_SINGLE will "just work".
 * + Use of Async long operations is not strictly compliant.
 *   However, it follows the intent of the nb-Long planned for GASNet-EX.
 * + The code totally ignores in vs out of segment.
 *   This is consistent with the plans for GASNet-EX (everything by default).
 *   This is (or can be made to be) acceptible on udp- and mpi-conduits, or with
 *   any conduit via SEGMENT_EVERYTHING.
 * + When using "segmentation" into multiple SUBORDINATE operations, this
 *   implementation makes no effort to bound the number in-flight at a given time.
 */

/*---------------------------------------------------------------------------------*/
/* bit arithmetic helpers */

/* TODO: replace the following bit-counting functions with algorithms from
 *   http://graphics.stanford.edu/~seander/bithacks.html (or similar).
 * However, we'll assume the compiler builtins are good enough when available.
 */

/* Count consectitive zero bits from the right (least-significant) end */
GASNETI_INLINE(gasnete_coll_ctz) GASNETI_CONST
unsigned int gasnete_coll_ctz(const uint32_t v) {
#if HAVE_BUILTIN_CTZ
  return v ? __builtin_ctz(v) : 32;
#elif HAVE_FFS
  return v ? (ffs(v)-1) : 32;
#else
  unsigned int c = 32;
  if (v) {
    for (c=0; !(v&1); ++c) v >>= 1;
  }
  return c;
#endif
}
GASNETI_CONSTP(gasnete_coll_ctz)

/* Returns floor(log_2(v)) and -1 for v=0 */
GASNETI_INLINE(gasnete_coll_log2) GASNETI_CONST
int gasnete_coll_log2(const uint32_t v) {
#if HAVE_BUILTIN_CLZ
  return v ? (31 - __builtin_clz(v)) : -1;
#else
  int c;
  for (c=-1; v; ++c) v >>= 1;
  return c;
#endif
}
GASNETI_CONSTP(gasnete_coll_ctz)

/*---------------------------------------------------------------------------------*/
/* binomial geometry helpers */

/* Size of local binomial subtree, including self */
GASNETI_INLINE(gasnete_coll_binom_subtree_size) GASNETI_PURE
gasnet_node_t gasnete_coll_binom_subtree_size(const int rank, gasnete_coll_team_t const team) {
  const gasnet_node_t remain = team->total_ranks - rank;
  const gasnet_node_t size = (rank & (-rank));
  return (!size || (size > remain)) ? remain : size;
}
GASNETI_PUREP(gasnete_coll_binom_subtree_size)

/* Count of direct children in binomial subtree */
GASNETI_INLINE(gasnete_coll_binom_children) GASNETI_PURE
gasnet_node_t gasnete_coll_binom_children(const int rank, gasnete_coll_team_t const team) {
  return 1 + gasnete_coll_log2(gasnete_coll_binom_subtree_size(rank, team) - 1);
}
GASNETI_PUREP(gasnete_coll_binom_children)

/* Parent in binomial tree, but FATAL to call for rank=0 */
GASNETI_INLINE(gasnete_coll_binom_parent) GASNETI_PURE
gasnet_node_t gasnete_coll_binom_parent(const int rank, gasnete_coll_team_t const team) {
  gasneti_assert(0 != rank); /* ctz(0) = 32, which would index off the end of 'bwd' */
  return team->peers.bwd[gasnete_coll_ctz(rank)];
}
GASNETI_PUREP(gasnete_coll_binom_parent)

/* Rank among siblings */
#define gasnete_coll_binom_child_rank(_rank, _team) gasnete_coll_ctz(_rank)

/* Is this rank a leaf? */
GASNETI_INLINE(gasnete_coll_binom_leaf) GASNETI_PURE
gasnet_node_t gasnete_coll_binom_leaf(const int rank, gasnete_coll_team_t const team) {
  return (rank & 1) || (rank == (team->total_ranks-1));
}
GASNETI_PUREP(gasnete_coll_binom_leaf)

/*---------------------------------------------------------------------------------*/
/* communication helpers */

/* UP half-barrier over the binomial tree.
 * No memory fences. */
GASNETI_INLINE(gasnete_coll_binomial_upsync)
int gasnete_coll_binomial_upsync(gasnete_coll_op_t * const op, const int rank, const int counter) {
  gasnete_coll_generic_data_t * const data = op->data;
  gasnete_coll_team_t const team = op->team;
#if 0 /* counts bits, taking log(P) time */
  const int goal = gasnete_coll_binom_children(rank, team);
  if (gasneti_weakatomic_read(&data->p2p->counter[counter], 0) == goal)
#else /* shifts bits, likely in O(1) */
  const int rcvd = gasneti_weakatomic_read(&data->p2p->counter[counter], 0);
  if ((1 << rcvd) >= gasnete_coll_binom_subtree_size(rank, team))
#endif
  {
    if (rank) {
      gasnete_coll_p2p_advance(op, gasnete_coll_binom_parent(rank, team), counter);
    }
    return 1;
  }
  return 0;
}

/* DOWN half-barrier over the binomial tree.
 * No memory fences. */
GASNETI_INLINE(gasnete_coll_binomial_downsync)
int gasnete_coll_binomial_downsync(gasnete_coll_op_t * const op, const int rank, const int counter) {
  gasnete_coll_generic_data_t * const data = op->data;
  if (!rank || gasneti_weakatomic_read(&data->p2p->counter[counter], 0)) {
    gasnete_coll_team_t const team = op->team;
    gasnet_node_t size = gasnete_coll_binom_subtree_size(rank, team) - 1;
    int idx;
    for (idx = 0; size != 0; ++idx, size >>= 1) {
      gasnete_coll_p2p_advance(op, team->peers.fwd[idx], counter);
    }
    return 1;
  }
  return 0;
}

/*---------------------------------------------------------------------------------*/
/* misc helpers */

typedef struct {
  size_t count;
  gasnet_coll_handle_t handles[1]; /* should be a C99 flexible array */
} gasnete_coll_handle_vec_t;

GASNETI_INLINE(gasnete_coll_alloc_handle_vec)
gasnete_coll_handle_vec_t *gasnete_coll_alloc_handle_vec(size_t count) {
  gasnete_coll_handle_vec_t * result =
    gasneti_calloc(1, sizeof(gasnete_coll_handle_vec_t) + (count - 1) * sizeof(gasnet_coll_handle_t));
  result->count = count;
  return result;
}
