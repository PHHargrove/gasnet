/*   $Source: bitbucket.org:berkeleylab/gasnet.git/tests/testhello.c $
 * Description: GASNet "Hello, World" test/example
 * Copyright 2010, The Regents of the University of California
 * Terms of use are as specified in license.txt
 */

#include <gasnetex.h>
#include <gasnet_vis.h>
#include <gasnet.h>
#include <stdio.h>

#include <stdlib.h>
#include <time.h>

/* Macro to check return codes and terminate with useful message. */
#define GASNET_SAFE(fncall) do {                                     \
    int _retval;                                                     \
    if ((_retval = fncall) != GASNET_OK) {                           \
      fprintf(stderr, "ERROR calling: %s\n"                          \
                      " at: %s:%i\n"                                 \
                      " error: %s (%s)\n",                           \
              #fncall, __FILE__, __LINE__,                           \
              gasnet_ErrorName(_retval), gasnet_ErrorDesc(_retval)); \
      fflush(stderr);                                                \
      gasnet_exit(_retval);                                          \
    }                                                                \
  } while(0)

int main(int argc, char **argv)
{
  gex_Rank_t rank, size;
  size_t segsz = GASNET_PAGESIZE;
  int argi;
  int test_case = -1;

  gex_Client_t      myclient;
  gex_EP_t    myep;
  gex_TM_t myteam;
  gex_Segment_t     mysegment;

  GASNET_SAFE(gex_Client_Init(&myclient, &myep, &myteam, "testhello", &argc, &argv, 0));
  rank = gex_TM_QueryRank(myteam);
  size = gex_TM_QuerySize(myteam);

  argi = 1;
  if (argi < argc) {
    test_case = atol(argv[argi]);
    ++argi;
  }

  if (test_case < 0) {
    srand(time(NULL));
    test_case = rand();
  }

  GASNET_SAFE(gex_Segment_Attach(&mysegment, myteam, segsz));

  gasnet_handle_t h;
  gasnet_register_value_t value;
  gasnet_valget_handle_t vgh;

  test_case %= 41;

  fprintf(stderr, "@ case %d\n", test_case);
  fflush(NULL);

  switch (test_case) {
    // AM Requests (0 arg only)
    case 0:	gasnet_AMRequestShort0(0, 0); break;
    case 1:	gasnet_AMRequestMedium0(0, 0, 0, 0); break;
    case 2:	gasnet_AMRequestLong0(0, 0, 0, 0, 0); break;
    // Contiguous-memory Put
    case 3:	gasnet_put(0,0,0,0); break;
    case 4:	gasnet_put_bulk(0,0,0,0); break;
    case 5:	h = gasnet_put_nb(0,0,0,0); break;
    case 6:	h = gasnet_put_nb_bulk(0,0,0,0); break;
    case 7:	gasnet_put_nbi(0,0,0,0); break;
    case 8:	gasnet_put_nbi_bulk(0,0,0,0); break;
    case 9:	gasnet_put_val(0,0,0,SIZEOF_GASNET_REGISTER_VALUE_T); break;
    case 10:	h = gasnet_put_nb_val(0,0,0,SIZEOF_GASNET_REGISTER_VALUE_T); break;
    case 11:	gasnet_put_nbi_val(0,0,0,SIZEOF_GASNET_REGISTER_VALUE_T); break;
    // Contiguous-memory Get
    case 12:	gasnet_get(0,0,0,0); break;
    case 13:	gasnet_get_bulk(0,0,0,0); break;
    case 14:	h = gasnet_get_nb(0,0,0,0); break;
    case 15:	h = gasnet_get_nb_bulk(0,0,0,0); break;
    case 16:	gasnet_get_nbi(0,0,0,0); break;
    case 17:	gasnet_get_nbi_bulk(0,0,0,0); break;
    case 18:	value = gasnet_get_val(0,0,SIZEOF_GASNET_REGISTER_VALUE_T); break;
    case 19:	vgh = gasnet_get_nb_val(0,0,SIZEOF_GASNET_REGISTER_VALUE_T); break;
    // Non-contiguous-memory Put
    case 20:	gasnet_putv_bulk(0,0,0,0,0); break;
    case 21:	h = gasnet_putv_nb_bulk(0,0,0,0,0); break;
    case 22:	gasnet_putv_nbi_bulk(0,0,0,0,0); break;
    case 23:	gasnet_puti_bulk(0,0,0,0,0,0,0); break;
    case 24:	h = gasnet_puti_nb_bulk(0,0,0,0,0,0,0); break;
    case 25:	gasnet_puti_nbi_bulk(0,0,0,0,0,0,0); break;
    case 26:	gasnet_puts_bulk(0,0,0,0,0,0,0); break;
    case 27:	h = gasnet_puts_nb_bulk(0,0,0,0,0,0,0); break;
    case 28:	gasnet_puts_nbi_bulk(0,0,0,0,0,0,0); break;
    // Non-contiguous-memory Get
    case 29:	gasnet_getv_bulk(0,0,0,0,0); break;
    case 30:	h = gasnet_getv_nb_bulk(0,0,0,0,0); break;
    case 31:	gasnet_getv_nbi_bulk(0,0,0,0,0); break;
    case 32:	gasnet_geti_bulk(0,0,0,0,0,0,0); break;
    case 33:	h = gasnet_geti_nb_bulk(0,0,0,0,0,0,0); break;
    case 34:	gasnet_geti_nbi_bulk(0,0,0,0,0,0,0); break;
    case 35:	gasnet_gets_bulk(0,0,0,0,0,0,0); break;
    case 36:	h = gasnet_gets_nb_bulk(0,0,0,0,0,0,0); break;
    case 37:	gasnet_gets_nbi_bulk(0,0,0,0,0,0,0); break;
    // memset
    case 38:    gasnet_memset(0,0,0,0); break;
    case 39:    h = gasnet_memset_nb(0,0,0,0); break;
    case 40:    gasnet_memset_nbi(0,0,0,0); break;
    //
    default:    fprintf(stderr, "@ WARNING: no test run\n");
  }

  /* Spec says client should include a barrier before gasnet_exit() */
  gasnet_barrier_notify(0,GASNET_BARRIERFLAG_ANONYMOUS);
  gasnet_barrier_wait(0,GASNET_BARRIERFLAG_ANONYMOUS);

  gasnet_exit(0);

  /* Not reached in most implementations */
  return 0;
}
