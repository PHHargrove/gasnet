/*   $Source: bitbucket.org:berkeleylab/gasnet.git/ofi-conduit/gasnet_extended.c $
 * Description: GASNet Extended API Reference Implementation
 * Copyright 2002, Dan Bonachea <bonachea@cs.berkeley.edu>
 * Copyright 2015, Intel Corporation
 * Terms of use are as specified in license.txt
 */

#include <gasnet_coll_internal.h> // for refbarrier.c
#include <gasnet_internal.h>
#include <gasnet_extended_internal.h>
#include <gasnet_core_internal.h>
#include <gasnet_ofi.h>

/* ------------------------------------------------------------------------------------ */
/*
  Extended API Common Code
  ========================
  Factored bits of extended API code common to most conduits, overridable when necessary
*/

#include "gasnet_extended_common.c"

/* ------------------------------------------------------------------------------------ */
/*
  Initialization
  ==============
*/
/* called at startup to check configuration sanity */
static void gasnete_check_config(void) {
  gasneti_check_config_postattach();
  gasnete_check_config_amref();

  gasneti_assert(sizeof(gasnete_eop_t) >= sizeof(void*));
}

extern void gasnete_init(void) {
  static int firstcall = 1;
  GASNETI_TRACE_PRINTF(C,("gasnete_init()"));
  gasneti_assert(firstcall); /*  make sure we haven't been called before */
  firstcall = 0;

  gasnete_check_config(); /*  check for sanity */

  gasneti_assert(gasneti_nodes >= 1 && gasneti_mynode < gasneti_nodes);

  { gasneti_threaddata_t *threaddata = NULL;
    gasnete_eop_t *eop = NULL;
    #if GASNETI_MAX_THREADS > 1
      /* register first thread (optimization) */
      threaddata = _gasneti_mythread_slow(); 
    #else
      /* register only thread (required) */
      threaddata = gasnete_new_threaddata();
    #endif

    /* cause the first pool of eops to be allocated (optimization) */
    eop = gasnete_eop_new(threaddata);
    GASNETE_EOP_MARKDONE(eop);
    gasnete_eop_free(eop);
  }

  /* Initialize barrier resources */
  gasnete_barrier_init();

  /* Initialize VIS subsystem */
  gasnete_vis_init();
}

/* ------------------------------------------------------------------------------------ */
/* GASNET-Internal OP Interface */
gasneti_eop_t *gasneti_eop_create(GASNETI_THREAD_FARG_ALONE) {
  gasnete_eop_t *op = gasnete_eop_new(GASNETI_MYTHREAD);
  return (gasneti_eop_t *)op;
}
gasneti_iop_t *gasneti_iop_register(unsigned int noperations, int isget GASNETI_THREAD_FARG) {
  gasneti_threaddata_t * const mythread = GASNETI_MYTHREAD;
  gasnete_iop_t * const op = mythread->current_iop;
  gasnete_iop_check(op);
  if (isget) op->initiated_get_cnt += noperations;
  else       op->initiated_put_cnt += noperations;
  gasnete_iop_check(op);
  return (gasneti_iop_t *)op;
}
void gasneti_eop_markdone(gasneti_eop_t *eop) {
  gasnete_eop_t *op = (gasnete_eop_t *)eop;
  gasnete_eop_check(op);
  GASNETE_EOP_MARKDONE(op);
}
void gasneti_iop_markdone(gasneti_iop_t *iop, unsigned int noperations, int isget) {
  gasnete_iop_t *op = (gasnete_iop_t *)iop;
  gasneti_weakatomic_t * const pctr = (isget ? &(op->completed_get_cnt) : &(op->completed_put_cnt));
  gasnete_iop_check(op);
  if (gasneti_constant_p(noperations) && (noperations == 1))
      gasneti_weakatomic_increment(pctr, 0);
  else {
    #if defined(GASNETI_HAVE_WEAKATOMIC_ADD_SUB)
      gasneti_weakatomic_add(pctr, noperations, 0);
    #else /* yuk */
      while (noperations) {
        gasneti_weakatomic_increment(pctr, 0);
        noperations--;
      }
    #endif
  }
  gasnete_iop_check(op);
}

/* ------------------------------------------------------------------------------------ */
/*
  Get/Put/Memset:
  ===============
*/

/*
 * Configuration appears in gasnet_extended_fwd.h
 */
#include "gasnet_extended_amref.c"

/* ------------------------------------------------------------------------------------ */
/*
  Non-blocking memory-to-memory transfers (explicit handle)
  ==========================================================
*/
/* ------------------------------------------------------------------------------------ */

/* Conduits not using the gasnete_amref_ versions should implement at least the following:
     gasnete_get_nb_bulk
     gasnete_put_nb
     gasnete_put_nb_bulk
     gasnete_memset_nb
*/

extern gex_Event_t gasnete_get_nb_bulk (void *dest, gex_Rank_t node, void *src, size_t nbytes GASNETI_THREAD_FARG) 
{
	GASNETI_CHECKPSHM_GET(UNALIGNED,H,dest,node,src,nbytes);
	{
		gasnete_eop_t *op = _gasnete_eop_new(GASNETI_MYTHREAD);
		op->ofi.type = OFI_TYPE_EGET;
		gasnetc_rdma_get(dest, node, src, nbytes, &op->ofi);
		return (gex_Event_t)op;
	}
}

extern gex_Event_t gasnete_put_nb      (gex_Rank_t node, void *dest, void *src, size_t nbytes GASNETI_THREAD_FARG) 
{
	GASNETI_CHECKPSHM_PUT(UNALIGNED,H,node,dest,src,nbytes);
	{
		gasnete_eop_t *op = _gasnete_eop_new(GASNETI_MYTHREAD);
		op->ofi.type = OFI_TYPE_EPUT;
        /* Try to submit this in a non-blocking way. If we can't, block for 
         * it for correctness */
		if (gasnetc_rdma_put_non_bulk(node, dest, src, nbytes, &op->ofi)) {
		    gasnetc_rdma_put_wait((gex_Event_t) op);
            gasnete_eop_free (op);
            return GEX_EVENT_INVALID;
        }
        return (gex_Event_t)op;
	}
}

extern gex_Event_t gasnete_put_nb_bulk (gex_Rank_t node, void *dest, void *src, size_t nbytes GASNETI_THREAD_FARG) 
{
	GASNETI_CHECKPSHM_PUT(UNALIGNED,H,node,dest,src,nbytes);
	{
		gasnete_eop_t *op = _gasnete_eop_new(GASNETI_MYTHREAD);
		op->ofi.type = OFI_TYPE_EPUT;
		gasnetc_rdma_put(node, dest, src, nbytes, &op->ofi);
		return (gex_Event_t)op;
	}
}

/* ------------------------------------------------------------------------------------ */
/*
  Non-blocking memory-to-memory transfers (implicit handle)
  ==========================================================
*/
/* ------------------------------------------------------------------------------------ */

extern void gasnete_get_nbi_bulk (void *dest, gex_Rank_t node, void *src, size_t nbytes GASNETI_THREAD_FARG) 
{
	GASNETI_CHECKPSHM_GET(UNALIGNED,V,dest,node,src,nbytes);
	{
		gasneti_threaddata_t * const mythread = GASNETI_MYTHREAD;
		gasnete_iop_t *op = mythread->current_iop;
		op->initiated_get_cnt++;
		op->get_ofi.type = OFI_TYPE_IGET;
		gasnetc_rdma_get(dest, node, src, nbytes, (void *) &op->get_ofi);
	}
}

extern void gasnete_put_nbi      (gex_Rank_t node, void *dest, void *src, size_t nbytes GASNETI_THREAD_FARG) 
{
	GASNETI_CHECKPSHM_PUT(ALIGNED,V,node,dest,src,nbytes);
	{
        /* If we know we will definitely submit this non-blocking op as
         * a blocking one, simply call the put function to avoid messing
         * with eops and iops. Note that if this branch is not taken, that
         * doesn't mean that gasnetc_rdma_put_non_bulk will not still block.
         * See below. */
        if (gasnetc_rdma_put_will_block(nbytes)) {
            gasnete_put(node, dest, src, nbytes GASNETI_THREAD_PASS);
            return;
        }

		gasneti_threaddata_t * const mythread = GASNETI_MYTHREAD;
		gasnete_iop_t *op = mythread->current_iop;
		op->initiated_put_cnt++;
		op->put_ofi.type = OFI_TYPE_IPUT;
        /* Try to submit this in a non-blocking way. If we can't, block for 
         * it for correctness. Note that the blocking case will only be hit
         * if there are not enough available boucne buffers. In that case,
         * we may oversynchronize by finishing all iops prior to this one. */
		if_pf (gasnetc_rdma_put_non_bulk(node, dest, src, nbytes, &op->put_ofi))
		    gasnetc_rdma_put_wait((gex_Event_t) op);
	}
}

extern void gasnete_put_nbi_bulk (gex_Rank_t node, void *dest, void *src, size_t nbytes GASNETI_THREAD_FARG) 
{
	GASNETI_CHECKPSHM_PUT(UNALIGNED,V,node,dest,src,nbytes);
	{
		gasneti_threaddata_t * const mythread = GASNETI_MYTHREAD;
		gasnete_iop_t *op = mythread->current_iop;
		op->initiated_put_cnt++;
		op->put_ofi.type = OFI_TYPE_IPUT;
		gasnetc_rdma_put(node, dest, src, nbytes, &op->put_ofi);
	}
}

/* ------------------------------------------------------------------------------------ */
/*
  Barriers:
  =========
*/

/* use reference implementation of barrier */
#define GASNETI_GASNET_EXTENDED_REFBARRIER_C 1
#include "gasnet_extended_refbarrier.c"
#undef GASNETI_GASNET_EXTENDED_REFBARRIER_C

/* ------------------------------------------------------------------------------------ */
/*
  Vector, Indexed & Strided:
  =========================
*/

/* use reference implementation of scatter/gather and strided */
#include "gasnet_refvis.h"

/* ------------------------------------------------------------------------------------ */
/*
  Collectives:
  ============
*/

/* use reference implementation of collectives */
#include "gasnet_refcoll.h"

/* ------------------------------------------------------------------------------------ */
/*
  Handlers:
  =========
*/
static gasnet_handlerentry_t const gasnete_handlers[] = {
  #ifdef GASNETE_REFBARRIER_HANDLERS
    GASNETE_REFBARRIER_HANDLERS(),
  #endif
  #ifdef GASNETE_REFVIS_HANDLERS
    GASNETE_REFVIS_HANDLERS()
  #endif
  #ifdef GASNETE_REFCOLL_HANDLERS
    GASNETE_REFCOLL_HANDLERS()
  #endif

  /* ptr-width independent handlers */

  /* ptr-width dependent handlers */
#if GASNETE_BUILD_AMREF_GET_HANDLERS
  gasneti_handler_tableentry_with_bits(gasnete_amref_get_reqh),
  gasneti_handler_tableentry_with_bits(gasnete_amref_get_reph),
  gasneti_handler_tableentry_with_bits(gasnete_amref_getlong_reqh),
  gasneti_handler_tableentry_with_bits(gasnete_amref_getlong_reph),
#endif
#if GASNETE_BUILD_AMREF_PUT_HANDLERS
  gasneti_handler_tableentry_with_bits(gasnete_amref_put_reqh),
  gasneti_handler_tableentry_with_bits(gasnete_amref_putlong_reqh),
#endif
#if GASNETE_BUILD_AMREF_MEMSET_HANDLERS
  gasneti_handler_tableentry_with_bits(gasnete_amref_memset_reqh),
#endif
#if GASNETE_BUILD_AMREF_PUT_HANDLERS || GASNETE_BUILD_AMREF_MEMSET_HANDLERS
  gasneti_handler_tableentry_with_bits(gasnete_amref_markdone_reph),
#endif

  { 0, NULL }
};

extern gasnet_handlerentry_t const *gasnete_get_handlertable(void) {
  return gasnete_handlers;
}
/* ------------------------------------------------------------------------------------ */

