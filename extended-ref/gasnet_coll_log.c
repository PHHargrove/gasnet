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
