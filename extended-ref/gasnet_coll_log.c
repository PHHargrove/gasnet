/*   $Source: bitbucket.org:berkeleylab/gasnet.git/extended-ref/gasnet_coll_log.c $
 * Description: PROOF-OF-CONCEPT implementation of collectives using only logrithmic team storage
 * Copyright 2015, Lawrence Berkeley National Laboratory
 * Terms of use are as specified in license.txt
 */

#include <gasnet_internal.h>
#include <gasnet_coll_internal.h>
#include <gasnet_coll_trees.h>
#include <gasnet_coll_scratch.h>

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

/*---------------------------------------------------------------------------------*/
/* gasnete_coll_broadcast_nb() */

/* bcast BinomEager */
/* Requires GASNETE_COLL_GENERIC_OPT_P2P on all nodes */
/* Naturally IN_MYSYNC, OUT_MYSYNC */
/* Max size is the eager limit */
static int gasnete_coll_pf_bcast_BinomEager(gasnete_coll_op_t *op GASNETE_THREAD_FARG) {
  gasnete_coll_generic_data_t * const data = op->data;
  gasnete_coll_broadcast_args_t * const args = GASNETE_COLL_GENERIC_ARGS(data, broadcast);
  gasnete_coll_team_t const team = op->team;
  int result = 0;

  int rank = team->myrank - args->srcnode;
  if (rank < 0) rank += team->total_ranks;

  switch (data->state) {
    case 0:     /* Optional IN barrier */
      if (!gasnete_coll_generic_all_threads(data) ||
          ((op->flags & GASNET_COLL_IN_ALLSYNC) &&
           !gasnete_coll_binomial_upsync(op, rank, 0))) {
        break;
      }
      data->state = 1;

    case 1: 	/* Data movement */
      /* Note that we send to children in reverse order (deepest first) */
      if (!rank || data->p2p->state[0]) {
        int idx   = rank ? gasnete_coll_binom_children(rank, team) : team->peers.num;
        void *src = rank ? data->p2p->data : args->src;
        while (idx--) {
          gasnete_coll_p2p_eager_put_tree(op, team->peers.fwd[idx], src, args->nbytes);
        }
        GASNETE_FAST_UNALIGNED_MEMCPY_CHECK(args->dst, src, args->nbytes);
      } else {
        break;	/* Stalled until data arrives */
      }
      data->state = 2;

    case 2:   /* Optional OUT barrier */
      if (!gasnete_coll_generic_outsync(team, data)) {
        break;
      }
      data->state = 3;

    case 3: /*done*/
      gasnete_coll_generic_free(team, data GASNETE_THREAD_PASS);
      result = (GASNETE_COLL_OP_COMPLETE | GASNETE_COLL_OP_INACTIVE);
  }

  return result;
}
GASNETE_COLL_DECLARE_BCAST_ALG(BinomEager)
{
  int options =
  GASNETE_COLL_GENERIC_OPT_OUTSYNC_IF(flags & GASNET_COLL_OUT_ALLSYNC) |
  GASNETE_COLL_GENERIC_OPT_P2P;

  gasneti_assert(nbytes <= gasnet_AMMaxMedium());
  gasneti_assert(nbytes <= gasneti_nodes * gasnete_coll_p2p_eager_scale);

  return gasnete_coll_generic_broadcast_nb(team, dst, srcimage, src, nbytes, flags,
                                           &gasnete_coll_pf_bcast_BinomEager, options,
                                           NULL, NULL, sequence, NULL, 0, NULL
                                           GASNETE_THREAD_PASS);
}


/* bcast BinomLong */
/* Requires GASNETE_COLL_GENERIC_OPT_P2P on all nodes */
/* Naturally IN_MYSYNC, OUT_MYSYNC */
/* max size is MaxLongRequest */
static int gasnete_coll_pf_bcast_BinomLong(gasnete_coll_op_t *op GASNETE_THREAD_FARG) {
  gasnete_coll_generic_data_t * const data = op->data;
  gasnete_coll_broadcast_args_t * const args = GASNETE_COLL_GENERIC_ARGS(data, broadcast);
  gasnete_coll_team_t const team = op->team;
  int result = 0;

  int rank = team->myrank - args->srcnode;
  if (rank < 0) rank += team->total_ranks;

  switch (data->state) {
    case 0:     /* thread barrier */
      if (!gasnete_coll_generic_all_threads(data)) {
        break;
      }
      data->state = 1;

    case 1:
      if (op->flags & GASNET_COLL_IN_ALLSYNC) {
        /* fold a half-barrier in to the address comms by stalling for all children here */
        int idx, size = gasnete_coll_binom_subtree_size(rank, team) - 1; /* excludes self */
        int done = 1;
        for (idx = 0; size != 0; ++idx, size >>= 1) {
          done &= data->p2p->state[idx+1];
        }
        if (! done) break;
      }
      if (rank) { /* send own address to parent (state[1] and up) */
        const gasnet_node_t parent = gasnete_coll_binom_parent(rank, team);
        const int offset = gasnete_coll_binom_child_rank(rank, team) + 1;
        gasnete_coll_p2p_eager_addr(op, parent, args->dst, offset, 1);
      }
      data->state = 2;

    case 2:     /* wait for data from parent (signaled in state[0]) */
      if (!rank) {
        GASNETE_FAST_UNALIGNED_MEMCPY(args->dst, args->src, args->nbytes);
      } else if (! data->p2p->state[0]) {
        break;
      }
      data->state = 3;

    case 3: {   /* signalling put to child addresses as they arrive (state[1] and up) */
      int idx = gasnete_coll_binom_children(rank, team);
      void * const * const addrs = (void **)data->p2p->data;
      int done = 1;
      while (idx--) {
        const int state = data->p2p->state[idx+1];
        if (0 == state) {
          done = 0; /* the child is not ready */
        } else if (1 == state) {
          data->p2p->state[idx+1] = 2; /* don't send more than once */
          gasnete_coll_p2p_signalling_putAsync(op, team->peers.fwd[idx], addrs[idx+1],
                                               args->dst, args->nbytes, 0, 1);
        }
      }
      if (! done) break; /* at least one child has not arrived */
      data->state = 4;
    }

    case 4:	/* OUT barrier (ALL) or upsync for completion of Async (NO|MY)*/
      if (op->flags & GASNET_COLL_OUT_ALLSYNC) {
        if (!gasnete_coll_generic_outsync(team, data)) break;
      } else {
        if (!gasnete_coll_binomial_upsync(op, rank, 0)) break;
      }
      data->state = 5;

    case 5:     /* done */
      gasnete_coll_generic_free(team, data GASNETE_THREAD_PASS);
      result = (GASNETE_COLL_OP_COMPLETE | GASNETE_COLL_OP_INACTIVE);
  }

  return result;
}
GASNETE_COLL_DECLARE_BCAST_ALG(BinomLong)
{
  int options =
  GASNETE_COLL_GENERIC_OPT_OUTSYNC_IF (flags & GASNET_COLL_OUT_ALLSYNC) |
  GASNETE_COLL_GENERIC_OPT_P2P;

  gasneti_assert(nbytes <= gasnet_AMMaxLongRequest());

  return gasnete_coll_generic_broadcast_nb(team, dst, srcimage, src, nbytes, flags,
                                           &gasnete_coll_pf_bcast_BinomLong, options,
                                           NULL, NULL, sequence, NULL, 0, NULL
                                           GASNETE_THREAD_PASS);
}


/* bcast BinomPut */
/* Requires GASNETE_COLL_GENERIC_OPT_P2P on all nodes */
/* Naturally IN_MYSYNC, OUT_MYSYNC */
/* size is unbounded */
static int gasnete_coll_pf_bcast_BinomPut(gasnete_coll_op_t *op GASNETE_THREAD_FARG) {
  gasnete_coll_generic_data_t * const data = op->data;
  gasnete_coll_broadcast_args_t * const args = GASNETE_COLL_GENERIC_ARGS(data, broadcast);
  gasnete_coll_team_t const team = op->team;
  int result = 0;

  int rank = team->myrank - args->srcnode;
  if (rank < 0) rank += team->total_ranks;

  switch (data->state) {
    case 0:     /* thread barrier */
      if (!gasnete_coll_generic_all_threads(data)) {
        break;
      }
      data->state = 1;

    case 1:
      if (op->flags & GASNET_COLL_IN_ALLSYNC) {
        /* fold a half-barrier in to the address comms by stalling for all children here */
        int idx, size = gasnete_coll_binom_subtree_size(rank, team) - 1; /* excludes self */
        int done = 1;
        for (idx = 0; size != 0; ++idx, size >>= 1) {
          done &= data->p2p->state[idx+1];
        }
        if (! done) break;
      }
      if (rank) { /* send own address to parent (state[1] and up) */
        const gasnet_node_t parent = gasnete_coll_binom_parent(rank, team);
        const int offset = gasnete_coll_binom_child_rank(rank, team) + 1;
        gasnete_coll_p2p_eager_addr(op, parent, args->dst, offset, 1);
      }
      data->state = 2;

    case 2:     /* wait for data from parent (signaled in state[0]) */
      if (!rank) {
        GASNETE_FAST_UNALIGNED_MEMCPY(args->dst, args->src, args->nbytes);
      } else if (! data->p2p->state[0]) {
        break;
      }
      data->state = 3;

    case 3: {   /* put and signal child as their addresses arrive (state[1] and up) */
      int idx = gasnete_coll_binom_children(rank, team);
      void ** const addrs = (void **)data->p2p->data;
      volatile uint32_t *state = data->p2p->state;
      int done = 1;
      while (idx--) {
        gasnet_handle_t *handle_p = (gasnet_handle_t *)&addrs[idx+1]; /* reduce, reuse, recycle */
        switch (state[idx+1]) {
        /*case 0: break; */
          case 1: /* received addr -> put data */
            *handle_p = gasnete_put_nb_bulk(team->peers.fwd[idx], addrs[idx+1],
                                            args->dst, args->nbytes GASNETE_THREAD_PASS);
            gasnete_coll_save_handle(handle_p GASNETE_THREAD_PASS);
            state[idx+1] = 2; /* advance and fall through */
          case 2: /* if put is complete -> update child's state */
            if (GASNET_INVALID_HANDLE != *handle_p) break;
            gasnete_coll_p2p_change_states(op, team->peers.fwd[idx], 1, 0, 1);
            state[idx+1] = 3; /* advance and fall through */
          case 3: /* nothing left to do */
            continue; /* next child */
        }
        done = 0;
      }
      if (! done) break; /* at least one child is not done */
      data->state = 4;
    }

    case 4:     /* Optional OUT barrier */
      if (!gasnete_coll_generic_outsync(team, data)) {
        break;
      }
      data->state = 5;

    case 5:     /* done */
      gasnete_coll_generic_free(team, data GASNETE_THREAD_PASS);
      result = (GASNETE_COLL_OP_COMPLETE | GASNETE_COLL_OP_INACTIVE);
  }

  return result;
}
GASNETE_COLL_DECLARE_BCAST_ALG(BinomPut)
{
  int options =
  GASNETE_COLL_GENERIC_OPT_OUTSYNC_IF (flags & GASNET_COLL_OUT_ALLSYNC) |
  GASNETE_COLL_GENERIC_OPT_P2P;

  gasneti_assert(sizeof(gasnet_handle_t) <= sizeof(void*));

  return gasnete_coll_generic_broadcast_nb(team, dst, srcimage, src, nbytes, flags,
                                           &gasnete_coll_pf_bcast_BinomPut, options,
                                           NULL, NULL, sequence, NULL, 0, NULL
                                           GASNETE_THREAD_PASS);
}


gasnet_coll_handle_t
gasnete_coll_broadcast_nb_log(gasnet_team_handle_t team,
                              void *dst,
                              gasnet_image_t srcimage, void *src,
                              size_t nbytes, int flags, uint32_t sequence
                              GASNETE_THREAD_FARG)
{
  const size_t eager_limit = gasnet_AMMaxMedium();
#if GASNET_DEBUG
  const size_t long_limit = 8192; /* lowered to get testing coverage of BinomPut */
#else
  const size_t long_limit = gasnet_AMMaxLongRequest();
#endif

  /* Choose algorithm based on size alone */
  if (nbytes <= eager_limit && nbytes <= gasneti_nodes * gasnete_coll_p2p_eager_scale) {
    return gasnete_coll_bcast_BinomEager(team, dst, srcimage, src, nbytes, flags, NULL, sequence GASNETE_THREAD_PASS);
  } else if (nbytes <= long_limit) {
    return gasnete_coll_bcast_BinomLong(team, dst, srcimage, src, nbytes, flags, NULL, sequence GASNETE_THREAD_PASS);
  } else {
    return gasnete_coll_bcast_BinomPut(team, dst, srcimage, src, nbytes, flags, NULL, sequence GASNETE_THREAD_PASS);
  }
}

/*---------------------------------------------------------------------------------*/
/* gasnete_coll_scatter_nb() */

/* scat BinomEager */
/* Requires GASNETE_COLL_GENERIC_OPT_P2P on all nodes */
/* Naturally IN_MYSYNC, OUT_MYSYNC */
/* Max size is the eager "scale" */
static int gasnete_coll_pf_scat_BinomEager(gasnete_coll_op_t *op GASNETE_THREAD_FARG) {
  gasnete_coll_generic_data_t * const data = op->data;
  gasnete_coll_scatter_args_t * const args = GASNETE_COLL_GENERIC_ARGS(data, scatter);
  gasnete_coll_team_t const team = op->team;
  int result = 0;

  int rank = team->myrank - args->srcnode;
  if (rank < 0) rank += team->total_ranks;

  switch (data->state) {
    case 0:     /* Optional IN barrier */
      if (!gasnete_coll_generic_all_threads(data) ||
          ((op->flags & GASNET_COLL_IN_ALLSYNC) &&
           !gasnete_coll_binomial_upsync(op, rank, 0))) {
        break;
      }
      data->state = 1;

    case 1: {	/* Data movement */
      /* Note that we send to children in reverse order (deepest first) */
      const size_t nbytes = args->nbytes;
      if (!rank) {
        const gasnet_node_t total_ranks = team->total_ranks;
        const gasnet_node_t myrank = team->myrank;
        const gasnet_node_t remain = total_ranks - myrank;
        const void * const src = args->src;
        int idx   = team->peers.num;
        int stride = 1 << (idx-1);
        /* Count can differ from 'stride' only for the highest-rank child */
        int count = MIN(stride, total_ranks - (rank + stride));
        while (idx--) {
          const gasnet_node_t child = (stride < remain) ? (stride + myrank) : (stride - remain);
          uint8_t *payload = gasnete_coll_scale_ptr(src, child, nbytes);
          if (child + count > total_ranks) { /* pack two contributions into eager data space */
            const size_t len1 = nbytes * (total_ranks - child);
            payload = memcpy(data->p2p->data, payload, len1);
            memcpy(payload + len1, src, nbytes * (child + count - total_ranks));
          }
          gasnete_coll_p2p_eager_put_tree(op, team->peers.fwd[idx], payload, count * nbytes);
          count = (stride >>= 1);
        }
        GASNETE_FAST_UNALIGNED_MEMCPY(args->dst, gasnete_coll_scale_ptr(src, myrank, nbytes), nbytes);
      } else if (data->p2p->state[0]) {
        const void * const src = data->p2p->data;
        int idx   = gasnete_coll_binom_children(rank, team);
        int stride = 1 << (idx-1);
        /* Count can differ from 'stride' only for the highest-rank child */
        int count = MIN(stride, team->total_ranks - (rank + stride));
        while (idx--) {
          uint8_t *payload = gasnete_coll_scale_ptr(src, stride, nbytes);
          gasnete_coll_p2p_eager_put_tree(op, team->peers.fwd[idx], payload, count * nbytes);
          count = (stride >>= 1);
        }
        GASNETE_FAST_UNALIGNED_MEMCPY(args->dst, src, nbytes);
      } else {
        break;	/* Stalled until data arrives */
      }
      data->state = 2;
    }

    case 2:   /* Optional OUT barrier */
      if (!gasnete_coll_generic_outsync(team, data)) {
        break;
      }
      data->state = 3;

    case 3: /*done*/
      gasnete_coll_generic_free(team, data GASNETE_THREAD_PASS);
      result = (GASNETE_COLL_OP_COMPLETE | GASNETE_COLL_OP_INACTIVE);
  }

  return result;
}
GASNETE_COLL_DECLARE_SCATTER_ALG(BinomEager)
{
  int options =
  GASNETE_COLL_GENERIC_OPT_OUTSYNC_IF(flags & GASNET_COLL_OUT_ALLSYNC) |
  GASNETE_COLL_GENERIC_OPT_P2P;

  gasneti_assert(nbytes <= 2 * gasnete_coll_p2p_eager_scale);
  gasneti_assert(dist == nbytes);

  return gasnete_coll_generic_scatter_nb(team, dst, srcimage, src, nbytes, dist, flags,
                                         &gasnete_coll_pf_scat_BinomEager, options,
                                         NULL, NULL, sequence, NULL, 0, NULL
                                         GASNETE_THREAD_PASS);
}


/* scatter BinomLong */
/* Requires GASNETE_COLL_GENERIC_OPT_P2P on all nodes */
/* Naturally IN_MYSYNC, OUT_MYSYNC */
/* size is <= MIN(team->smallest_scratch_seg, gasnet_AMMaxLongRequest()); */
static int gasnete_coll_pf_scat_BinomLong(gasnete_coll_op_t *op GASNETE_THREAD_FARG) {
  gasnete_coll_generic_data_t * const data = op->data;
  gasnete_coll_scatter_args_t * const args = GASNETE_COLL_GENERIC_ARGS(data, scatter);
  gasnete_coll_team_t const team = op->team;
  int result = 0;

  int rank = team->myrank - args->srcnode;
  if (rank < 0) rank += team->total_ranks;

  switch (data->state) {
    case 0:     /* alloc scratch */
      gasneti_assert(op->scratch_req);
      if (!gasnete_coll_scratch_alloc_nb(op GASNETE_THREAD_PASS)) {
        break;
      }
      data->state = 1;

    case 1:     /* Optional IN barrier */
      if (!gasnete_coll_generic_all_threads(data) ||
          ((op->flags & GASNET_COLL_IN_ALLSYNC) &&
           !gasnete_coll_binomial_upsync(op, rank, 0))) {
        break;
      }
      data->state = 2;

    case 2:     /* wait for data from parent (signaled in state[0] and possibly state[1]) */
      if (rank) {
        int state = data->p2p->state[0];
        if (state > 1) state = data->p2p->state[1]; /* two puts, follow "chain" */
        if (! state) break;
      }
      data->state = 3;

    case 3: {   /* signalling put to child scratch spaces */
      const size_t nbytes = args->nbytes;
      const gasnet_node_t total_ranks = team->total_ranks;
      const gasnet_node_t myrank = team->myrank;
      const gasnet_node_t remain = total_ranks - myrank;
      const gasnet_node_t * const peers = team->peers.fwd;
      if (!rank) {
        void *src = args->src;
        int idx    = team->peers.num;
        int stride = 1 << (idx-1);
        int count  = MIN(stride, total_ranks - stride);
        while (idx--) {
          const gasnet_node_t child = (stride < remain) ? (stride + myrank) : (stride - remain);
          int8_t *payload = gasnete_coll_scale_ptr(src, child, nbytes);
          int8_t *dst = (int8_t*)team->scratch_segs[child].addr + op->scratchpos[idx];
          if (child + count > total_ranks) { /* Two sends */
            /* TODO: is it ever beneficial to copy into our our scratch_seg to form a single Put? */
            const size_t len1 = nbytes * (total_ranks - child);
            const size_t len2 = nbytes * (child + count - total_ranks);
            gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst, payload, len1, 0, 2);
            gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst+len1, src, len2, 1, 1);
          } else {
            gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst, payload, count*nbytes, 0, 1);
          }
          count = (stride >>= 1);
        }
        GASNETE_FAST_UNALIGNED_MEMCPY(args->dst, gasnete_coll_scale_ptr(src, myrank, nbytes), nbytes);
      } else {
        int8_t *src = (int8_t*)team->scratch_segs[myrank].addr + op->myscratchpos;
        int idx    = gasnete_coll_binom_children(rank, team);
        int stride = 1 << (idx-1);
        int count  = MIN(stride, total_ranks - (rank + stride));
        while (idx--) {
          const gasnet_node_t child = (stride < remain) ? (stride + myrank) : (stride - remain);
          int8_t *dst = (int8_t*)team->scratch_segs[child].addr + op->scratchpos[idx];
          gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst,
                                               gasnete_coll_scale_ptr(src, stride, nbytes),
                                               count * nbytes, 0, 1);
          count = (stride >>= 1);
        }
        GASNETE_FAST_UNALIGNED_MEMCPY(args->dst, src, nbytes);
      }
      data->state = 4;
    }

    case 4:	/* OUT barrier (ALL) or upsync for completion of Async (NO|MY)*/
      if (op->flags & GASNET_COLL_OUT_ALLSYNC) {
        if (!gasnete_coll_generic_outsync(team, data)) break;
      } else {
        if (!gasnete_coll_binomial_upsync(op, rank, 1)) break;
      }
      data->state = 5;

    case 5:     /* done */
      gasnete_coll_free_scratch(op);
      gasnete_coll_generic_free(team, data GASNETE_THREAD_PASS);
      result = (GASNETE_COLL_OP_COMPLETE | GASNETE_COLL_OP_INACTIVE);
  }

  return result;
}
GASNETE_COLL_DECLARE_SCATTER_ALG(BinomLong)
{
  int options =
  GASNETE_COLL_GENERIC_OPT_OUTSYNC_IF (flags & GASNET_COLL_OUT_ALLSYNC) |
  GASNETE_COLL_GENERIC_OPT_P2P|
  GASNETE_COLL_USE_SCRATCH;

  gasnete_coll_scratch_req_t *scratch_req;
  static gasnete_coll_tree_type_t tree_type = NULL;

  gasnet_node_t srcnode = gasnete_coll_image_node(team, srcimage);
  const gasnet_node_t rank = (team->myrank >= srcnode)
                                ? (team->myrank - srcnode)
                                : (team->myrank + (team->total_ranks - srcnode));
  int i;

  if_pf (! tree_type) {
    tree_type = gasnete_coll_make_tree_type(GASNETE_COLL_TREE_CLASS_LOG_SCAT1, NULL, 0);
  }

  scratch_req = (gasnete_coll_scratch_req_t*) gasneti_calloc(1,sizeof(gasnete_coll_scratch_req_t));
  scratch_req->tree_type = tree_type;
  scratch_req->tree_dir = GASNETE_COLL_DOWN_TREE;
  scratch_req->op_type = GASNETE_COLL_TREE_OP;
  scratch_req->root = srcnode;
  scratch_req->team = team;

  scratch_req->in_peers_absolute = 1;
  if (!rank) {
    scratch_req->num_in_peers = 0;
    scratch_req->in_peers = NULL;
    scratch_req->incoming_size = 0;
  } else {
    scratch_req->num_in_peers = 1;
    scratch_req->in_peers = &(team->peers.bwd[gasnete_coll_ctz(rank)]); /* parent by reference */
    scratch_req->incoming_size = nbytes * gasnete_coll_binom_subtree_size(rank, team);
  }

  scratch_req->num_out_peers = gasnete_coll_binom_children(rank, team);
  scratch_req->out_peers = team->rel_peers.fwd;

  scratch_req->out_sizes = (uint64_t *)gasneti_malloc(sizeof(uint64_t) * scratch_req->num_out_peers);
  for (i=0; i<scratch_req->num_out_peers; ++i) {
    scratch_req->out_sizes[i] = nbytes * gasnete_coll_binom_subtree_size(rank+(1<<i), team);
  }

  gasneti_assert(nbytes <= gasnet_AMMaxLongRequest());
  gasneti_assert(dist == nbytes);

  return gasnete_coll_generic_scatter_nb(team, dst, srcimage, src, nbytes, dist, flags,
                                         &gasnete_coll_pf_scat_BinomLong, options,
                                         NULL, NULL, sequence, scratch_req, 0, NULL
                                         GASNETE_THREAD_PASS);
}

/* scatter BinomPut */
/* Requires GASNETE_COLL_GENERIC_OPT_P2P on all nodes */
/* Naturally IN_MYSYNC, OUT_MYSYNC */
/* size is <= MIN(team->smallest_scratch_seg, gasnet_AMMaxLongRequest()); */
static int gasnete_coll_pf_scat_BinomPut(gasnete_coll_op_t *op GASNETE_THREAD_FARG) {
  gasnete_coll_generic_data_t * const data = op->data;
  gasnete_coll_scatter_args_t * const args = GASNETE_COLL_GENERIC_ARGS(data, scatter);
  gasnete_coll_team_t const team = op->team;
  int result = 0;

  int rank = team->myrank - args->srcnode;
  if (rank < 0) rank += team->total_ranks;

  /*   p2p->state[0]   Counts received chunks in scratch space, or -1 on split-send
   *             [1]   In case of split-send replaces [0] as count of received chunks
   *             [2]   Saved "unsent" count
   *             [3]   Non-zero means own chunk has been received
   *             [3+i] Non-zero means waiting for ack from peers.fwd[i] for i > 0
   *
   * p2p->counter[0]   Used by final binomial_upsync(), if any
   *             [1]   Unused
   *             [3]   Unused
   *             [3+i] Non-zero means have address from peers.fwd[i]
   *
   * addrs = (void**)p2p->data
   *        addrs[3+i] Holds address received from peers.fwd[i]
   */

  switch (data->state) {
    case 0:     /* alloc scratch */
      gasneti_assert(op->scratch_req);
      if (!gasnete_coll_scratch_alloc_nb(op GASNETE_THREAD_PASS)) {
        break;
      }
      data->state = 1;

    case 1: {
      const int size = gasnete_coll_binom_subtree_size(rank, team);

      /* Thread barrier: */
      if (!gasnete_coll_generic_all_threads(data)) {
        break;
      }

      /* Optional IN barrier: */
      if (op->flags & GASNET_COLL_IN_ALLSYNC) {
        /* fold a half-barrier in to the address comms by stalling for all children here */
        int idx, s = size - 1; /* excludes self */
        int done = 1;
        for (idx = 0; s != 0; ++idx, s >>= 1) {
          done &= gasneti_weakatomic_read(&(data->p2p->counter[idx+3]), 0);
        }
        if (! done) break;
      }

      /* Send own address to parent, incrementing counter[idx+3] */
      if (rank) {
        const int offset = 3 + gasnete_coll_binom_child_rank(rank, team);
        gasnete_coll_p2p_counting_eager_put(op, gasnete_coll_binom_parent(rank, team), &(args->dst),
                                            sizeof(void*), sizeof(void*), offset, offset);
      }

      /* Advance state: */
      data->p2p->state[2] = size;  /* Our "unsent" counter */
      data->state = 2;
    }

    case 2: {   /* data movement */
      const size_t nbytes = args->nbytes;
      const gasnet_node_t total_ranks = team->total_ranks;
      const gasnet_node_t myrank = team->myrank;
      const gasnet_node_t remain = total_ranks - myrank;
      const gasnet_node_t * const peers = team->peers.fwd;
      int unsent = data->p2p->state[2];
      if (!rank) {
        void *src = args->src;
        while (unsent > 1) {
          const int idx = gasnete_coll_log2(unsent - 1);
          const gasnet_node_t stride = 1 << idx;
          const gasnet_node_t child = (stride < remain) ? (stride + myrank) : (stride - remain);
          int count = unsent - stride;
          gasneti_assert(count > 0);
          if (count == 1) { /* Direct put of child's own portion of data */
            if (0 == gasneti_weakatomic_read(&(data->p2p->counter[idx+3]), 0)) {
              return 0; /* waiting for an address */
            } else {
              void ** const addrs = (void **)data->p2p->data;
              void *payload = gasnete_coll_scale_ptr(src, child, nbytes);
              gasnete_coll_p2p_signalling_putAsync(op, peers[idx], addrs[idx+3], payload, nbytes, 3, 1);
              unsent -= 1;
            }
          } else { /* Send data to child's scratch space to be forwarded */
            gasneti_assert(idx > 0); /* idx=0 is always a leaf */
            if (data->p2p->state[idx+3]) {
              return 0; /* waiting for an ack */
            } else {
              /* TODO: avoid division here? idx-independent value? */
              const int max_count = op->scratch_req->out_sizes[idx-1] / nbytes;
              gasnet_node_t start;
              int8_t *payload, *dst = (int8_t*)team->scratch_segs[child].addr + op->scratchpos[idx-1];
              unsent -= (count = MIN(count-1, max_count));
              start = (unsent < remain) ? (unsent + myrank) : (unsent - remain);
              payload = gasnete_coll_scale_ptr(src, start, nbytes);
              data->p2p->state[idx+3] = 1; /* mark as pending ack */
              if (start + count > total_ranks) { /* Two sends */
                /* TODO: is it ever beneficial to copy into our our scratch_seg to form a single Put? */
                const size_t len1 = nbytes * (total_ranks - start);
                const size_t len2 = nbytes * (start + count - total_ranks);
                gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst, payload, len1, 0, -1);
                gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst+len1, src, len2, 1, count);
              } else {
                gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst, payload, count*nbytes, 0, count);
              }
            }
          }
          data->p2p->state[2] = unsent;
        }
        GASNETE_FAST_UNALIGNED_MEMCPY(args->dst, gasnete_coll_scale_ptr(src, myrank, nbytes), nbytes);
      } else if (unsent > 1) {
        int8_t *src = (int8_t*)team->scratch_segs[myrank].addr + op->myscratchpos;
        int rcvd = data->p2p->state[0];
        if (rcvd < 0) rcvd = data->p2p->state[1]; /* two puts, follow "chain" */
        if (! rcvd) break;
        do {
          const int idx = gasnete_coll_log2(unsent - 1);
          const gasnet_node_t stride = 1 << idx;
          uint8_t *payload;
          int count = MIN(unsent - stride, rcvd);
          unsent -= count;
          if (unsent == stride && count > 1) { /* split child's own data from their scratch */
            unsent += 1;
            count -= 1;
          }
          payload = gasnete_coll_scale_ptr(src, rcvd - count, nbytes);
          if (unsent == stride) {
            if (0 == gasneti_weakatomic_read(&(data->p2p->counter[idx+3]), 0)) {
              return 0; /* waiting for an address */
            } else {
              void ** const addrs = (void **)data->p2p->data;
              gasnete_coll_p2p_signalling_putAsync(op, peers[idx], addrs[idx+3], payload, nbytes, 3, 1);
            }
          } else {
            gasneti_assert(idx > 0); /* idx=0 is always a leaf */
            if (data->p2p->state[idx+3]) {
              return 0; /* waiting for an ack */
            } else {
              const gasnet_node_t child = (stride < remain) ? (stride + myrank) : (stride - remain);
              int8_t *dst = (int8_t*)team->scratch_segs[child].addr + op->scratchpos[idx-1];
              data->p2p->state[idx+3] = 1; /* mark as pending ack */
              gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst, payload, count*nbytes, 0, count);
            }
          }
          data->p2p->state[0] = (rcvd -= count);
          data->p2p->state[2] = unsent;
        } while (rcvd);
        if (unsent > 1) { /* ACK to parent if more data is expected */
          const gasnet_node_t parent = gasnete_coll_binom_parent(rank, team);
          const int offset = 3 + gasnete_coll_binom_child_rank(rank, team);
          gasnete_coll_p2p_change_states(op, parent, 1, offset, 0);
          break;
        }
      }
      data->state = 3;
    }

    case 3:	/* wait for own data to arrive */
      if (rank && ! data->p2p->state[3]) {
        break;
      }
      data->state = 4;

    case 4:	/* OUT barrier (ALL) or upsync for completion of Async (NO|MY)*/
      if (op->flags & GASNET_COLL_OUT_ALLSYNC) {
        if (!gasnete_coll_generic_outsync(team, data)) break;
      } else {
        if (!gasnete_coll_binomial_upsync(op, rank, 0)) break;
      }
      data->state = 5;

    case 5:     /* done */
      gasnete_coll_free_scratch(op);
      gasnete_coll_generic_free(team, data GASNETE_THREAD_PASS);
      result = (GASNETE_COLL_OP_COMPLETE | GASNETE_COLL_OP_INACTIVE);
  }

  return result;
}
GASNETE_COLL_DECLARE_SCATTER_ALG(BinomPut)
{
  int options =
  GASNETE_COLL_GENERIC_OPT_OUTSYNC_IF (flags & GASNET_COLL_OUT_ALLSYNC) |
  GASNETE_COLL_GENERIC_OPT_P2P|
  GASNETE_COLL_USE_SCRATCH;

  gasnete_coll_scratch_req_t *scratch_req;
  static gasnete_coll_tree_type_t tree_type = NULL;

  gasnet_node_t srcnode = gasnete_coll_image_node(team, srcimage);
  const gasnet_node_t rank = (team->myrank >= srcnode)
                                ? (team->myrank - srcnode)
                                : (team->myrank + (team->total_ranks - srcnode));
  const gasnet_node_t max_count = MIN(team->smallest_scratch_seg, gasnet_AMMaxLongRequest()) / nbytes;
  int i;

  if_pf (! tree_type) {
    tree_type = gasnete_coll_make_tree_type(GASNETE_COLL_TREE_CLASS_LOG_SCAT2, NULL, 0);
  }

  scratch_req = (gasnete_coll_scratch_req_t*) gasneti_calloc(1,sizeof(gasnete_coll_scratch_req_t));
  scratch_req->tree_type = tree_type;
  scratch_req->tree_dir = GASNETE_COLL_DOWN_TREE;
  scratch_req->op_type = GASNETE_COLL_TREE_OP;
  scratch_req->root = srcnode;
  scratch_req->team = team;

  /* TODO: are in/out sizes the "tightest" possible values?  */

  scratch_req->in_peers_absolute = 1;
  if (!rank) {
    scratch_req->num_in_peers = 0;
    scratch_req->in_peers = NULL;
    scratch_req->incoming_size = 0;
  } else if(! gasnete_coll_binom_leaf(rank, team)) {
    int size = gasnete_coll_binom_subtree_size(rank, team) - 1;
    scratch_req->num_in_peers = 1;
    scratch_req->in_peers = &(team->peers.bwd[gasnete_coll_ctz(rank)]); /* parent by reference */
    scratch_req->incoming_size = nbytes * MIN(max_count, size);
  }

  if (! gasnete_coll_binom_leaf(rank, team)) {
    /* First child is always a leaf and last could be, too */
    int count = gasnete_coll_binom_children(rank, team) - 1;
    count -= (count && gasnete_coll_binom_leaf(rank + (1<<count), team));
    if (count) {
      scratch_req->num_out_peers = count;
      scratch_req->out_peers = team->rel_peers.fwd + 1;
      scratch_req->out_sizes = (uint64_t *)gasneti_malloc(sizeof(uint64_t) * count);
      for (i=0; i<count; ++i) {
        int size = gasnete_coll_binom_subtree_size(rank + (2<<i), team) - 1;
        scratch_req->out_sizes[i] = nbytes * MIN(max_count, size);
      }
    }
  }

  gasneti_assert(nbytes <= gasnet_AMMaxLongRequest());
  gasneti_assert(dist == nbytes);

  return gasnete_coll_generic_scatter_nb(team, dst, srcimage, src, nbytes, dist, flags,
                                         &gasnete_coll_pf_scat_BinomPut, options,
                                         NULL, NULL, sequence, scratch_req, 0, NULL
                                         GASNETE_THREAD_PASS);
}

/* scatter BinomSegInner */
/* Requires GASNETE_COLL_GENERIC_OPT_P2P on all nodes */
/* Naturally IN_MYSYNC,OUT_MYSYNC */
/* size is unbounded */
static int gasnete_coll_pf_scat_BinomSegInner(gasnete_coll_op_t *op GASNETE_THREAD_FARG) {
  gasnete_coll_generic_data_t * const data = op->data;
  gasnete_coll_scatter_args_t * const args = GASNETE_COLL_GENERIC_ARGS(data, scatter);
  gasnete_coll_team_t const team = op->team;
  int result = 0;

  int rank = team->myrank - args->srcnode;
  if (rank < 0) rank += team->total_ranks;

  /* Note that the following follows the resource assignments in BinomPut.
   * This is because this code is a simplification of that code for
   * the case max_count==1, plus allowing for dist!=nbytes.
   *
   *   p2p->state[0]   Indicates a chunk is available in scratch space
   *             [1]   unused
   *             [2]   Saved "unsent" count
   *             [3]   Non-zero means own chunk has been received
   *             [3+i] Non-zero means waiting for ack from peers.fwd[i] for i > 0
   *
   * p2p->counter[0]   Used by final binomial_upsync()
   */

  /* Only intended for use as a subordinate */
  gasneti_assert(op->flags & GASNETE_COLL_SUBORDINATE);
  gasneti_assert(!(op->flags & (GASNET_COLL_IN_ALLSYNC | GASNET_COLL_IN_ALLSYNC)));

  switch (data->state) {
    case 0:     /* alloc scratch */
      gasneti_assert(op->scratch_req);
      if (!gasnete_coll_scratch_alloc_nb(op GASNETE_THREAD_PASS)) {
        break;
      }
      data->p2p->state[2] = gasnete_coll_binom_subtree_size(rank, team);  /* Our "unsent" counter */
      data->state = 1;

    case 1: {   /* data movement */
      const size_t nbytes = args->nbytes;
      const size_t dist = args->dist;
      const gasnet_node_t total_ranks = team->total_ranks;
      const gasnet_node_t myrank = team->myrank;
      const gasnet_node_t remain = total_ranks - myrank;
      const gasnet_node_t * const peers = team->peers.fwd;
      uint8_t ** const addrs = (uint8_t **)data->private_data;
      const ptrdiff_t offset = (uint8_t*)args->dst - addrs[0];
      int unsent = data->p2p->state[2];
      if (!rank) {
        void *src = args->src;
        while (unsent > 1) {
          const int idx = gasnete_coll_log2(--unsent);
          const gasnet_node_t stride = 1 << idx;
          const gasnet_node_t child = (stride < remain) ? (stride + myrank) : (stride - remain);
          if (unsent == stride) { /* Direct put of child's own portion of data */
            uint8_t *dst = addrs[idx+1] + offset;
            void *payload = gasnete_coll_scale_ptr(src, child, dist);
            gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst, payload, nbytes, 3, 1);
          } else { /* Send data to child's scratch space to be forwarded */
            gasneti_assert(idx > 0); /* idx=0 is always a leaf */
            if (data->p2p->state[idx+3]) {
              return 0; /* waiting for an ack */
            } else {
              int8_t *dst = (int8_t*)team->scratch_segs[child].addr + op->scratchpos[idx-1];
              gasnet_node_t start = (unsent < remain) ? (unsent + myrank) : (unsent - remain);
              int8_t *payload = gasnete_coll_scale_ptr(src, start, dist);
              data->p2p->state[idx+3] = 1; /* mark as pending ack */
              gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst, payload, nbytes, 0, 1);
            }
          }
          data->p2p->state[2] = unsent;
        }
        GASNETE_FAST_UNALIGNED_MEMCPY(args->dst, gasnete_coll_scale_ptr(src, myrank, dist), nbytes);
      } else if (unsent > 1) {
        if (! data->p2p->state[0]) {
          break;
        } else {
          int8_t *src = (int8_t*)team->scratch_segs[myrank].addr + op->myscratchpos;
          const int idx = gasnete_coll_log2(--unsent);
          const gasnet_node_t stride = 1 << idx;
          if (unsent == stride) {
            uint8_t *dst = addrs[idx+1] + offset;
            gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst, src, nbytes, 3, 1);
          } else {
            gasneti_assert(idx > 0); /* idx=0 is always a leaf */
            if (data->p2p->state[idx+3]) {
              return 0; /* waiting for an ack */
            } else {
              const gasnet_node_t child = (stride < remain) ? (stride + myrank) : (stride - remain);
              int8_t *dst = (int8_t*)team->scratch_segs[child].addr + op->scratchpos[idx-1];
              data->p2p->state[idx+3] = 1; /* mark as pending ack */
              gasnete_coll_p2p_signalling_putAsync(op, peers[idx], dst, src, nbytes, 0, 1);
            }
          }
          data->p2p->state[0] = 0;
          data->p2p->state[2] = unsent;
        }
        if (unsent > 1) { /* ACK to parent if more data is expected */
          const gasnet_node_t parent = gasnete_coll_binom_parent(rank, team);
          const int offset = 3 + gasnete_coll_binom_child_rank(rank, team);
          gasnete_coll_p2p_change_states(op, parent, 1, offset, 0);
          break;
        }
      }
      data->state = 3;
    }

    case 3:	/* wait for own data to arrive */
      if (rank && ! data->p2p->state[3]) {
        break;
      }
      data->state = 4;

    case 4:	/* upsync for completion of Async */
      if (!gasnete_coll_binomial_upsync(op, rank, 0)) {
        break;
      }
      data->state = 5;

    case 5:     /* done */
      gasnete_coll_free_scratch(op);
      gasnete_coll_generic_free(team, data GASNETE_THREAD_PASS);
      result = (GASNETE_COLL_OP_COMPLETE | GASNETE_COLL_OP_INACTIVE);
  }

  return result;
}
/* scatter BinomSeg */
/* Requires GASNETE_COLL_GENERIC_OPT_P2P on all nodes */
/* Naturally IN_MYSYNC,OUT_MYSYNC */
/* size is (almost) unbounded - may exhaust handles in the extream */
static int gasnete_coll_pf_scat_BinomSeg(gasnete_coll_op_t *op GASNETE_THREAD_FARG) {
  gasnete_coll_generic_data_t * const data = op->data;
  gasnete_coll_scatter_args_t * const args = GASNETE_COLL_GENERIC_ARGS(data, scatter);
  gasnete_coll_team_t const team = op->team;
  int result = 0;

  int rank = team->myrank - args->srcnode;
  if (rank < 0) rank += team->total_ranks;

  /* Note that the following follows the resource assignments in BinomPut.
   *
   *   p2p->state[0]   Unused
   *             [i+i] Non-zero means have address from peers.fwd[i]
   *
   * p2p->counter[0]   Used by final binomial_downsync(), if any
   *
   * addrs = (void**)p2p->data
   *        addrs[0]   Holds initial args->src;
   *        addrs[1+i] Holds address received from peers.fwd[i]
   */

  switch (data->state) {
    case 0: /* Optional IN barrier */
      if (!gasnete_coll_generic_all_threads(data)) {
        break;
      }
      if (op->flags & GASNET_COLL_IN_ALLSYNC) {
        /* fold a half-barrier in to the address comms by stalling for all children here */
        int idx, size = gasnete_coll_binom_subtree_size(rank, team) - 1; /* excludes self */
        int done = 1;
        for (idx = 0; size != 0; ++idx, size >>= 1) {
          done &= data->p2p->state[idx+1];
        }
        if (! done) break;
      }
      data->state = 1;

    case 1: /* Xmit dst addresses once */
      if (rank) { /* send own address to parent (state[1] and up) */
        const gasnet_node_t parent = gasnete_coll_binom_parent(rank, team);
        const int offset = gasnete_coll_binom_child_rank(rank, team) + 1;
        gasnete_coll_p2p_eager_addr(op, parent, args->dst, offset, 1);
      }
      data->state = 2;

    case 2:
      /* wait for all addresses unless we did so above */
      if (! (op->flags & GASNET_COLL_IN_ALLSYNC)) {
        int idx, size = gasnete_coll_binom_subtree_size(rank, team) - 1; /* excludes self */
        int done = 1;
        for (idx = 0; size != 0; ++idx, size >>= 1) {
          done &= data->p2p->state[idx+1];
        }
        if (! done) break;
      }
      /* initialize resources */
      ((void**)data->p2p->data)[0] = args->dst;
      data->state = 3;

    case 3: {   /* data movement */
      static gasnete_coll_tree_type_t tree_type = NULL;
      gasnete_coll_handle_vec_t *hvec = data->private_data;
      const int flags = GASNETE_COLL_FORWARD_FLAGS(op->flags);
      const int options = GASNETE_COLL_GENERIC_OPT_P2P | GASNETE_COLL_USE_SCRATCH;
      const size_t limit = MIN(team->smallest_scratch_seg, gasnet_AMMaxLongRequest());
      gasnete_coll_scratch_req_t scratch_master = {0};
      int i;

      size_t remain = args->nbytes;
      uint8_t *src = args->src;
      uint8_t *dst = args->dst;

      if_pf (! tree_type) {
        tree_type = gasnete_coll_make_tree_type(GASNETE_COLL_TREE_CLASS_LOG_SCAT3, NULL, 0);
      }

      scratch_master.tree_type = tree_type;
      scratch_master.tree_dir = GASNETE_COLL_DOWN_TREE;
      scratch_master.op_type = GASNETE_COLL_TREE_OP;
      scratch_master.root = args->srcnode;
      scratch_master.team = team;

      scratch_master.in_peers_absolute = 1;
      if (rank && ! gasnete_coll_binom_leaf(rank, team)) {
        scratch_master.num_in_peers = 1;
        scratch_master.in_peers = &(team->peers.bwd[gasnete_coll_ctz(rank)]); /* parent by reference */
        scratch_master.incoming_size = limit;
      }

      if (! gasnete_coll_binom_leaf(rank, team)) {
        /* First child is always a leaf and last could be, too */
        int count = gasnete_coll_binom_children(rank, team) - 1;
        count -= (count && gasnete_coll_binom_leaf(rank + (1<<count), team));
        scratch_master.num_out_peers = count;
        scratch_master.out_peers = team->rel_peers.fwd + 1;
        /* out_sizes allocated individually */
      }

      for (i=0; i<hvec->count; ++i) {
        size_t nbytes;

        gasnete_coll_scratch_req_t *scratch_req = gasneti_malloc(sizeof(gasnete_coll_scratch_req_t));
        *scratch_req = scratch_master;
        if (scratch_req->num_out_peers) {
          int j, count = scratch_req->num_out_peers;
          scratch_req->out_sizes = (uint64_t *)gasneti_malloc(sizeof(uint64_t) * count);
          for (j=0; j<count; ++j) {
            scratch_req->out_sizes[j] = limit;
          }
        }

        nbytes = MIN(limit, remain);

        hvec->handles[i] =
            gasnete_coll_generic_scatter_nb(team, dst, args->srcnode, src,
                                            nbytes, args->dist, flags,
                                            &gasnete_coll_pf_scat_BinomSegInner, options,
                                            data->p2p->data, NULL, op->sequence+1+i,
                                            scratch_req, 0, NULL GASNETE_THREAD_PASS);
        gasnete_coll_save_coll_handle(&hvec->handles[i] GASNETE_THREAD_PASS);

        src += nbytes;
        dst += nbytes;
        remain -= nbytes;
      }
      gasneti_assert(0 == remain);
      data->state = 4;
    }

    case 4: { /* Sync data movement */
      gasnete_coll_handle_vec_t *hvec = data->private_data;
      if (!gasnete_coll_generic_coll_sync(hvec->handles, hvec->count GASNETE_THREAD_PASS)) {
        break;
      }
      gasneti_free(hvec);
      data->state = 5;
    }

    case 5:   /* Optional OUT barrier */
      if ((op->flags & GASNET_COLL_OUT_ALLSYNC) &&
          ! gasnete_coll_binomial_downsync(op, rank, 0)) {
        break;
      }
      data->state = 6;

    case 6:     /* done */
      gasnete_coll_generic_free(team, data GASNETE_THREAD_PASS);
      result = (GASNETE_COLL_OP_COMPLETE | GASNETE_COLL_OP_INACTIVE);
  }

  return result;
}
GASNETE_COLL_DECLARE_SCATTER_ALG(BinomSeg)
{
  int options = GASNETE_COLL_GENERIC_OPT_P2P;

  const size_t limit = MIN(team->smallest_scratch_seg, gasnet_AMMaxLongRequest());
  size_t num_ops = (nbytes + limit - 1) / limit;
  gasnete_coll_handle_vec_t *data = gasnete_coll_alloc_handle_vec(num_ops);

  gasneti_assert(dist == nbytes);
  gasneti_assert(! (flags & GASNETE_COLL_SUBORDINATE));

  return gasnete_coll_generic_scatter_nb(team, dst, srcimage, src, nbytes, nbytes, flags,
                                         &gasnete_coll_pf_scat_BinomSeg, options,
                                         data, NULL, num_ops, NULL, 0, NULL
                                         GASNETE_THREAD_PASS);
}

gasnet_coll_handle_t
gasnete_coll_scatter_nb_log(gasnet_team_handle_t team,
                              void *dst,
                              gasnet_image_t srcimage, void *src,
                              size_t nbytes, int flags, uint32_t sequence
                              GASNETE_THREAD_FARG)
{
  const size_t eager_limit = gasnet_AMMaxMedium();
  const size_t long_limit  = MIN(team->smallest_scratch_seg, gasnet_AMMaxLongRequest());
  const size_t max_payload = nbytes * (team->total_ranks / 2);

  /* Choose algorithm based on size alone */
  if (nbytes <= 2*gasnete_coll_p2p_eager_scale && max_payload <= eager_limit) {
    return gasnete_coll_scat_BinomEager(team, dst, srcimage, src, nbytes, nbytes, flags, NULL, sequence GASNETE_THREAD_PASS);
  } else if (max_payload <= long_limit) {
    return gasnete_coll_scat_BinomLong(team, dst, srcimage, src, nbytes, nbytes, flags, NULL, sequence GASNETE_THREAD_PASS);
  } else if (nbytes <= long_limit) {
    return gasnete_coll_scat_BinomPut(team, dst, srcimage, src, nbytes, nbytes, flags, NULL, sequence GASNETE_THREAD_PASS);
  } else {
    return gasnete_coll_scat_BinomSeg(team, dst, srcimage, src, nbytes, nbytes, flags, NULL, sequence GASNETE_THREAD_PASS);
  }
}
