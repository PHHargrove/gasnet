/*   $Source: bitbucket.org:berkeleylab/gasnet.git/tests/testcontendAM.c $
 *
 * Description: GASNet threaded AM injection contention tester.
 *   The test initializes GASNet and forks off up to 256 threads.  
 *  The test measures the level of inter-thread contention for local 
 *  network resources with various different usage patterns.
 *
 * Portions Copyright 2019, The Regents of the University of California
 * Based on testcontent.c: Copyright 2004, Dan Bonachea <bonachea@cs.berkeley.edu>
 * Terms of use are as specified in license.txt
 */

#include "test.h"

#ifndef GASNET_PAR
#error This test can only be built for GASNet PAR configuration
#endif

static gex_Client_t      myclient;
static gex_EP_t    myep;
static gex_TM_t myteam;
static gex_Segment_t     mysegment;

static gex_Rank_t myrank;
static gex_Rank_t numranks;

typedef struct {
  int activecnt;
  int passivecnt;
} threadcnt_t;

typedef gex_AM_Arg_t harg_t;

/* configurable parameters */
#define DEFAULT_ITERS 50
int	iters = DEFAULT_ITERS;
int amactive;
int peer = -1;
char *peerseg = NULL;
int threads;
gasnett_atomic_t pong[TEST_MAXTHREADS];
volatile int signal_done = 0;
#define thread_barrier() PTHREAD_BARRIER(threads)

int revthreads = 0;
#define ARG2THREAD(arg) (revthreads?(threads-1)-(int)(intptr_t)args:(int)(intptr_t)args)

typedef void * (*threadmain_t)(void *args);

/* AM Handlers */
void	ping_shorthandler(gex_Token_t token, gex_AM_Arg_t tid);
void 	pong_shorthandler(gex_Token_t token, gex_AM_Arg_t tid);

void	markdone_shorthandler(gex_Token_t token);

#define hidx_ping_shorthandler        201
#define hidx_pong_shorthandler        202
#define hidx_markdone_shorthandler    203

gex_AM_Entry_t htable[] = { 
	{ hidx_ping_shorthandler,     ping_shorthandler,     GEX_FLAG_AM_REQUEST|GEX_FLAG_AM_SHORT, 1 },
	{ hidx_pong_shorthandler,     pong_shorthandler,     GEX_FLAG_AM_REPLY|GEX_FLAG_AM_SHORT, 1 },
	{ hidx_markdone_shorthandler, markdone_shorthandler, GEX_FLAG_AM_REQUEST|GEX_FLAG_AM_SHORT, 0 },
};
#define HANDLER_TABLE_SIZE (sizeof(htable)/sizeof(gex_AM_Entry_t))

#define SPINPOLL_UNTIL(cond) do { while (!(cond)) gasnet_AMPoll(); } while (0)

int _havereport = 0;
char _reportstr[644];
const char *getreport(void) {
  if (_havereport) {
    _havereport = 0;
    return _reportstr;
  } else return NULL;
}
void report(gasnett_tick_t ticks) {
  double timeus = (double)gasnett_ticks_to_us(ticks);
  snprintf(_reportstr, sizeof(_reportstr),
     "%7.3f us\t%5.3f sec", 
     timeus/iters, timeus/1000000);
  _havereport = 1;
}

/* testing functions */

void * ampingpong_poll_active(void *args) {
    int mythread = ARG2THREAD(args);
    static int nonzero_present = 0;
    gasnett_tick_t start, end;
    signal_done = 0;
    if (mythread != 0) nonzero_present = 1;
    gasnett_atomic_set(&pong[mythread],0,0);
    thread_barrier();
      start = gasnett_ticks_now();
      for (int i = 0; i < iters; i++) {
        gex_AM_RequestShort1(myteam, peer, hidx_ping_shorthandler, 0, mythread);
        SPINPOLL_UNTIL(gasnett_atomic_read(&pong[mythread],0) > i);
      }
    thread_barrier();
    end = gasnett_ticks_now();
    if (mythread == 0 ) {
      gex_AM_RequestShort0(myteam, peer, hidx_markdone_shorthandler, 0);
      gex_AM_RequestShort0(myteam, myrank, hidx_markdone_shorthandler, 0);
      if (!nonzero_present) {
        mythread = 1; /* ensure it runs once, impersonating thread1 */
        SPINPOLL_UNTIL(signal_done);
        mythread = 0;
      }
    }
    thread_barrier();
    nonzero_present = 0;
    if (mythread == 0 && amactive) report(end-start);
    return NULL;
}

void * poll_passive(void *args) {
  int mythread = ARG2THREAD(args);
  signal_done = 0;
  thread_barrier();
  thread_barrier();
  while (!signal_done) gasnet_AMPoll();
  thread_barrier();
  return NULL;
}

typedef struct {
  const char *desc;
  threadmain_t activefunc;
  threadmain_t passivefunc;
} fntable_t;

fntable_t fntable[] = {
  { "AM Ping-pong", ampingpong_poll_active, poll_passive },
};
#define NUM_FUNC (sizeof(fntable)/sizeof(fntable_t))
int tcountentries;
threadcnt_t *tcount;

void *workerthread(void *args) {
  int fnidx;
  int mythread = ARG2THREAD(args);
  for (fnidx = 0; fnidx < NUM_FUNC; fnidx++) {
    int tcountpos;

    if (mythread == 0) TEST_SECTION_BEGIN();
    thread_barrier();
    if (!TEST_SECTION_ENABLED()) {
      thread_barrier();
      continue;
    }

    if (mythread == 0 && myrank == 0) {
        MSG("%c: --------------------------------------------------------------------------",
            TEST_SECTION_NAME());
        MSG("%c: Running test %s", TEST_SECTION_NAME(), fntable[fnidx].desc);
        MSG("%c: --------------------------------------------------------------------------",
            TEST_SECTION_NAME());
        MSG("%c: Injecting threads\t  Polling threads\t  IterTime\tTotalTime",
            TEST_SECTION_NAME());
        MSG("%c: --------------------------------------------------------------------------",
            TEST_SECTION_NAME());
    }

    for (tcountpos = 0; tcountpos < tcountentries; tcountpos++) {
      threadmain_t mainfn = amactive ? fntable[fnidx].activefunc : fntable[fnidx].passivefunc;
      int participating_threads = amactive ? tcount[tcountpos].activecnt : tcount[tcountpos].passivecnt;
      thread_barrier();
      if (mythread < participating_threads) mainfn(args);
      else { /* match barriers */
        thread_barrier();
        thread_barrier();
        thread_barrier();
      }
      thread_barrier();
      if (mythread == 0 && amactive) { 
        const char *rpt = getreport();
        if (rpt) MSG("%c:\t   %d\t\t\t  %d\t\t%s", TEST_SECTION_NAME(),
          tcount[tcountpos].activecnt, tcount[tcountpos].passivecnt, rpt);
      }
    }
  }
  return NULL;
}
int main(int argc, char **argv) {
	int maxthreads = 4;
	int i;
	int arg;
	int help = 0;
        threadcnt_t *ptcount;

	GASNET_Safe(gex_Client_Init(&myclient, &myep, &myteam, "testcontendAM", &argc, &argv, 0));
        GASNET_Safe(gex_Segment_Attach(&mysegment, myteam, TEST_SEGSZ_REQUEST));
        GASNET_Safe(gex_EP_RegisterHandlers(myep, htable, HANDLER_TABLE_SIZE));

        myrank = gex_TM_QueryRank(myteam);
        numranks = gex_TM_QuerySize(myteam);

	test_init("testcontendAM",1,"[options] (maxthreads) (iters) (test_sections)\n"
                  "  The -rev option reverses thread numbering");

        arg = 1;
        while (argc > arg) {
          if (!strcmp(argv[arg], "-rev")) {
            revthreads = 1;
            ++arg;
          } else if (argv[arg][0] == '-') {
            help = 1;
            ++arg;
          } else break;
        }
        if (argc > arg) { maxthreads = atoi(argv[arg]); ++arg; }
        if (argc > arg) { iters = atoi(argv[arg]); ++arg; }
        if (argc > arg) { TEST_SECTION_PARSE(argv[arg]); ++arg; }
        if (argc > arg || help) test_usage();

	if (maxthreads > TEST_MAXTHREADS || maxthreads < 1) {
	  printf("Threads must be between 1 and %i\n", TEST_MAXTHREADS);
	  gasnet_exit(-1);
	}
	maxthreads = test_thread_limit(maxthreads);
        if (numranks % 2 != 0) {
    	  MSG0("WARNING: This test requires an even number of nodes. Test skipped.\n");
    	  gasnet_exit(0); /* exit 0 to prevent false negatives in test harnesses for smp-conduit */
        }
        if (myrank == 0) {
          MSG("Running testcontendAM with 1..%i threads and %i iterations", maxthreads, iters);
        }
        tcountentries = 3 * maxthreads;
        tcount = test_malloc(tcountentries * sizeof(threadcnt_t));
        ptcount = tcount;
        for (i = 1; i <= maxthreads; i++) { ptcount->activecnt = i; ptcount->passivecnt = 1; ptcount++; }
        for (i = 1; i <= maxthreads; i++) { ptcount->activecnt = i; ptcount->passivecnt = i; ptcount++; }
        peer = (myrank + 1) % numranks;
        amactive = (myrank % 2 == 0);

        peerseg = TEST_SEG(peer);

        /* create all worker threads */
        threads = maxthreads;
        test_createandjoin_pthreads(maxthreads, &workerthread, NULL, 0);

        BARRIER();
	if (myrank == 0) MSG("Tests complete");
        BARRIER();

	gasnet_exit(0);

	return 0;
}

/****************************************************************/
/* AM Handlers */
void ping_shorthandler(gex_Token_t token, gex_AM_Arg_t tid) {
  gex_AM_ReplyShort1(token, hidx_pong_shorthandler, 0, tid);
}

void pong_shorthandler(gex_Token_t token, gex_AM_Arg_t tid) {
  gasnett_atomic_increment(&pong[tid],0);
}

void markdone_shorthandler(gex_Token_t token) {
  signal_done = 1;
}

