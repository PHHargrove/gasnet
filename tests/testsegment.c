/* $Source: bitbucket.org:berkeleylab/gasnet.git/tests/testseg.c $
 * Copyright (c) 2020, The Regents of the University of California
 *
 * Description: GASNet Client Segment test
 *
 * This test check correctness of binding client-allocated memory as a segment
 * and perforiming RMA and Long operations which target it.
 */

#include <gasnetex.h>

// Unused
#ifndef TEST_SEGSZ
#define TEST_SEGSZ PAGESZ
#endif

#include <test.h>

//  ------------------------------------------------------------------------------------

static gex_Client_t  myclient;
static gex_EP_t      myep;
static gex_TM_t      myteam;
static gex_Rank_t    myrank, nranks;

//  ------------------------------------------------------------------------------------

// NOT fully general, but sufficient for this test
#if PLATFORM_ARCH_32
  #define PTR_NARGS       1
  #define PTR_ARGS        gex_AM_Arg_t arg0
  #define PTR_PACK(ptr)   ((gex_AM_Arg_t)(ptr))
  #define PTR_UNPACK()    ((void *)a0)
#elif PLATFORM_ARCH_64
  #define PTR_NARGS       2
  #define PTR_ARGS        gex_AM_Arg_t arg0, gex_AM_Arg_t arg1
  #define PTR_PACK(ptr)   ((gex_AM_Arg_t)TEST_HIWORD(ptr)), \
                          ((gex_AM_Arg_t)TEST_LOWORD(ptr))
  #define PTR_UNPACK()    ((void *)((((uint64_t)(arg0)) << 32) | \
                                    (((uint64_t)(arg1)) & 0xFFFFFFFF)))
#endif

//  ------------------------------------------------------------------------------------

static volatile int ping_rcvd = 0;
static volatile int pong_rcvd = 0;

#define hidx_ping       200
#define hidx_pong       201

static void ping_handler(gex_Token_t token, void *buf, size_t nbytes, PTR_ARGS) {
  assert_always(! ping_rcvd);
  ping_rcvd = 1;

  // Payload value is our jobrank, address is in the handler arg(s)
  gex_AM_ReplyLong0(token, hidx_pong, &myrank, sizeof(gex_Rank_t), PTR_UNPACK(), GEX_EVENT_NOW, 0);
}

static void pong_handler(gex_Token_t token, void *buf, size_t nbytes) {
  assert_always(! pong_rcvd);
  pong_rcvd = 1;
}

// handler table
gex_AM_Entry_t htable[] = {
  { hidx_ping, ping_handler,   GEX_FLAG_AM_REQUEST|GEX_FLAG_AM_LONG, PTR_NARGS },
  { hidx_pong, pong_handler,   GEX_FLAG_AM_REPLY  |GEX_FLAG_AM_LONG,  0 }
 };
#define HANDLER_TABLE_SIZE (sizeof(htable)/sizeof(gex_AM_Entry_t))

//  ------------------------------------------------------------------------------------

int main(int argc, char **argv)
{
  int client_segment = 1;

  GASNET_Safe(gex_Client_Init(&myclient, &myep, &myteam, "testsegment", &argc, &argv, 0));

  int help = 0;
  int argi = 1;
  while (argc > argi) {
    if (!strcmp(argv[argi], "-cb")) {
      client_segment = 1;
      ++argi;
    } else if (!strcmp(argv[argi], "-gb")) {
      client_segment = 0;
      ++argi;
    } else if (argv[argi][0] == '-') {
      help = 1;
      ++argi;
    } else break;
  }

  test_init("testsegment", 0, "[options]\n"
               "  -gb:  Test with a GASNet-allocated buffer\n"
               "  -cb:  Test with a client-allocated buffer (default)\n");
  if (argc > argi || help) test_usage();

  myrank = gex_TM_QueryRank(myteam);
  nranks = gex_TM_QuerySize(myteam);

  GASNET_Safe(gex_EP_RegisterHandlers(myep, htable, sizeof(htable)/sizeof(gex_AM_Entry_t)));

  #if GASNET_SEGMENT_EVERYTHING
    // TBD: which calls are valid/useful?
  #else
  {
    // TODO: fix PSHM support and remove this:
    gex_Rank_t nbrhd_set_size;
    gex_System_QueryMyPosition(&nbrhd_set_size, NULL, NULL, NULL);
    int run_test= (nbrhd_set_size == nranks); // All neighborhoods are singletons
    if (!run_test) {
      MSG0("WARNING: Skipping the communication portion of this test.");
      MSG0("WARNING: To enable communication, set GASNET_SUPERNODE_MAXSIZE=1.");
    }

    int success = 1;
    unsigned int offset = 16;

    // Test creation of a GASNet-allocated segment w/ non-page length and alignment
    gex_Segment_t g_segment = GEX_SEGMENT_INVALID;
    GASNET_Safe(gex_Segment_Create(&g_segment, myclient, NULL, GASNET_PAGESIZE-offset, GEX_MEMKIND_HOST, 0));
    if ((g_segment == GEX_SEGMENT_INVALID) ||
        (gex_Segment_QueryAddr(g_segment) == NULL) ||
        (gex_Segment_QuerySize(g_segment) != GASNET_PAGESIZE-offset)) {
      MSG("*** ERROR - FAILED GASNET-ALLOCATED SEGMENT CREATE TEST!!!!!");
      success = 0;
    }

    // Test creation of a client-allocated segment w/ non-page length and alignment
    gex_Segment_t c_segment = GEX_SEGMENT_INVALID;
    uint8_t *c_segment_mem = (uint8_t *) test_malloc(GASNET_PAGESIZE);
    uint8_t *c_segment_addr = c_segment_mem + offset;
    size_t c_segment_size = GASNET_PAGESIZE - 2*offset;
    GASNET_Safe(gex_Segment_Create(&c_segment, myclient, c_segment_addr, c_segment_size, GEX_MEMKIND_HOST, 0));
    if ((c_segment == GEX_SEGMENT_INVALID) ||
        (gex_Segment_QueryAddr(c_segment) != c_segment_addr) ||
        (gex_Segment_QuerySize(c_segment) != c_segment_size)) {
      MSG("*** ERROR - FAILED CLIENT-ALLOCATED SEGMENT CREATE TEST!!!!!");
      success = 0;
    }

    // Test pre-bind (no segments yet) Publish
    // Should not fail, nor interfere with the post-Bind use of Publish
    if (GASNET_OK != gex_Segment_PublishM(&myteam,  1, 0)) {
      MSG("*** ERROR - FAILED EARLY SEGMENT PUBLISH TEST!!!!!");
      success = 0;
    }

    // Pick a segment to test and (TODO:) destroy the other
    gex_Segment_t seg;
    void *    seg_addr;
    uintptr_t seg_size;
    if (client_segment) {
      seg      = c_segment;
      seg_addr = c_segment_addr;
      seg_size = c_segment_size;
      // GASNET_Safe(gex_Segment_Destroy(g_segment, 0));
    } else {
      seg      = g_segment;
      seg_addr = gex_Segment_QueryAddr(g_segment);
      seg_size = gex_Segment_QuerySize(g_segment);
      // GASNET_Safe(gex_Segment_Destroy(c_segment, 0));
    }

    // Bind the client-allocated segments and validate
    gex_Segment_EP_Bind(seg, myep, 0);
    {
      void *tmp_addr;
      size_t tmp_size;
      GASNET_Safe(gex_Segment_QueryBound(myteam, myrank, &tmp_addr, NULL, &tmp_size));
      if ((seg != gex_EP_QuerySegment(myep)) ||
          (tmp_addr != seg_addr) ||
          (tmp_size != seg_size)) {
        MSG("*** ERROR - FAILED SEGMENT EP BIND TEST!!!!!");
        success = 0;
      }
    }

    // Publish the segment over a permuted temporary team,
    // consisting all odds in reverse order followed by evens in reverse order
    {
      gex_TM_t tmp_tm = GEX_TM_INVALID;
      int key = (myrank & 1 ? 0 : nranks) + (nranks - myrank);
      gex_TM_Split(&tmp_tm, myteam, 0, key, NULL, 0, GEX_FLAG_TM_NO_SCRATCH);
      assert_always(tmp_tm != GEX_TM_INVALID);
      assert_always(nranks == gex_TM_QuerySize(tmp_tm));
      if (GASNET_OK != gex_Segment_PublishM(&tmp_tm,  1, 0)) {
        MSG("*** ERROR - FAILED PERMUTED SEGMENT PUBLISH TEST!!!!!");
        success = 0;
      }
      GASNET_Safe(gex_TM_Destroy(tmp_tm, NULL, 0));
    }

    // Prepare for comms
    gex_Rank_t peer = (myrank + 1) % nranks;
    void *loc_base, *rem_base;
    GASNET_Safe(gex_Segment_QueryBound(myteam, peer, &rem_base, NULL, NULL));
    loc_base = seg_addr;

    // Put, Get and AMLong to exercise the segment
    // TODO: Collective scratch space carved out of client-allocated segment?
    // TODO: Can Long payloads be made large to prevent packed-long optimizations?
    if (run_test) {
      gex_Rank_t rank_val;
      gex_Rank_t *loc_array = (gex_Rank_t *)loc_base;
      gex_Rank_t *rem_array = (gex_Rank_t *)rem_base;
      loc_array[0] = myrank;            // Source of Gets
      loc_array[1] = GEX_RANK_INVALID;  // Destination of Put
      loc_array[2] = GEX_RANK_INVALID;  // Destination of RequestLong
      loc_array[3] = GEX_RANK_INVALID;  // Destination of ReplyLong
      loc_array[4] = GEX_RANK_INVALID;  // Destination of loopback Put

      BARRIER();

      gex_Event_t ev[2] = {
            gex_RMA_GetNB(myteam, &rank_val, peer, rem_array, sizeof(gex_Rank_t), 0),
            gex_RMA_PutNB(myteam, peer, rem_array + 1, &peer, sizeof(gex_Rank_t), GEX_EVENT_DEFER, 0),
          };
      gex_Event_WaitAll(ev, 2, 0);

      gex_AM_RequestLong(myteam, peer, hidx_ping, &peer, sizeof(gex_Rank_t), rem_array + 2,
                         GEX_EVENT_NOW, 0, PTR_PACK(loc_array + 3));

      // Ping follows completion of Put to same peer, providing point-to-point sync.
      // So a full barrier is not required prior to examining results.
      GASNET_BLOCKUNTIL(ping_rcvd && pong_rcvd);

      assert_always(rank_val     == peer);   // Get
      assert_always(loc_array[1] == myrank); // Put
      assert_always(loc_array[2] == myrank); // RequestLong payload
      assert_always(loc_array[3] == peer);   // ReplyLong payload

      // Try loopback too
      rank_val = gex_RMA_GetBlockingVal(myteam, myrank, loc_array, sizeof(gex_Rank_t), 0);
      assert_always(rank_val == myrank);
      gex_RMA_PutBlockingVal(myteam, myrank, loc_array + 4, myrank, sizeof(gex_Rank_t), 0);
      assert_always(loc_array[4] == myrank);
    }

    // Test redundant Publish
    if (GASNET_OK != gex_Segment_PublishM(&myteam,  1, 0)) {
      MSG("*** ERROR - FAILED NO-OP SEGMENT PUBLISH TEST!!!!!");
      success = 0;
    }
  }
  #endif

  BARRIER();
  MSG("done.");

  gasnet_exit(0);
  return 0;
}
