/* 
 * Description: reproducer for cxi provider RMA performance issue with non-trivial working set size
 * Copyright (c) 2023, The Regents of the University of California
 * Terms of use are as specified in GASNet-EX's license.txt
 * Author: Paul H. Hargrove <PHHargrove@lbl.gov>
 *
 * To compile (assuming a bash shell):
      ml libfabric cray-pmi
      cc testcxirma.c -o testcxirma $(pkg-config --cflags --libs libfabric)
 * To run in a two-node allocation (assuming a bash shell):
      srun -n2 -N2 ./testcxirma 0 0 0            # default case with small segment
      srun -n2 -N2 ./testcxirma 0 0 $((2**31))   # problematic case with 2GiB segment
 *
 * This test measures uni-directional flood bandwidth of `fi_write()`.
 * The target memory is (as required by libfabric) registered with the provider
 * using either `fi_mr_reg()` or `fi_mr_regattr()`.
 * The source memory is not registered.
 * Command line options provide control over the number of iterations, the size
 * of the `fi_write()` payload, and the size of the registered target memory
 * (aka "segment").
 *
 * The issue this test is meant to reproduce is that when holding the
 * iterations and payload size constant, increasing the segment size beyond
 * some threshold greatly reduces the flood bandwidth.  The example `srun`
 * commands above have been found to be sufficient to reproduce on multiple
 * Slingshot-11 based systems.
 *
 * See code comments and usage message for various details.
 * In partcular, "NOTE:" comments identify places where small modifications to
 * the code have already been attempted.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

//======================================================================
//  START OF UTILITY MACROS TO IMPROVE READABILITY
//======================================================================

// Binary units
#define KiB 1024
#define MiB (1024*KiB)
#define GiB (1024*MiB)

// Print an error message, prefixed with our rank, and abort().
#define FATAL(format, ...) do { \
   fprintf(stderr, "%d:\t Error: " format "\n", rank,  __VA_ARGS__); \
   abort(); \
 } while (0)

// Simple error-handling macro
// Prints a fatal error message if `fncall` doesn't return the `success` value
#define SAFE_CALL_VAL(success, fncall) do { \
   int rc = (fncall); \
   if (rc != success) FATAL("%s\n%d:\t    returned %i", #fncall, rank, rc); \
 } while (0)

// Error-handling macro specialized for the `success == 0` case
#define SAFE_CALL(fncall) SAFE_CALL_VAL(0, fncall)

//======================================================================
//  START OF PMI SPAWNING / OOB COMMS
//======================================================================

#include <pmi_cray.h>
#define max_name_len 1024
#define max_key_len 32
#define max_val_len 64
static char kvs_name[max_name_len+1];
static int size = -1;
static int rank = -1;
static int active = -1;

void kvs_put(const char *key, int src, void *value, size_t sz) {
  char tmp_key[max_key_len+1];
  char tmp_val[max_val_len+1];
  uint8_t *bytes = value;
  snprintf(tmp_key, sizeof(tmp_key), "%s:%d", key, src);
  for (size_t i = 0; i < sz; ++i) {
    snprintf(tmp_val+i*2, 3, "%02x", bytes[i]);
  }
  SAFE_CALL( PMI2_KVS_Put(tmp_key, tmp_val) );
  SAFE_CALL( PMI2_KVS_Fence() );
}
 
void kvs_get(const char *key, int src, void *value, size_t sz) {
  char tmp_key[max_key_len+1];
  char tmp_val[max_val_len+1];
  uint8_t *bytes = value;
  int len;
  snprintf(tmp_key, sizeof(tmp_key), "%s:%d", key, src);
  SAFE_CALL( PMI2_KVS_Get(kvs_name, rank, tmp_key, tmp_val, max_val_len, &len) );
  for (size_t i = 0; i < sz; ++i) {
    char *next = tmp_val + 2*(i+1);
    char save = *next; *next = '\0';
    unsigned int tmp;
    sscanf(tmp_val+i*2, "%02x", &tmp);
    bytes[i] = tmp;
    *next = save;
  }
}

void do_init(void) {
  static int spawned, appnum;
  SAFE_CALL( PMI2_Init(&spawned, &size, &rank, &appnum) );
  SAFE_CALL( PMI2_Job_GetId(kvs_name, max_name_len) );
  SAFE_CALL( PMI_Barrier() );
  active = !(rank & 1); // evens active, odds passive
}

//======================================================================
//  (HYPOTHETICALLY) NANOSECOND-RESOLUTION TIMER
//======================================================================

uint64_t now(void) { // timer in ns
  struct timespec tm;
  static clockid_t clockid = CLOCK_MONOTONIC;
  if (clock_gettime(clockid,&tm)) {
    clockid = CLOCK_REALTIME; // fail over
    clock_gettime(CLOCK_REALTIME,&tm);
  }
  return tm.tv_sec*((uint64_t)1E9)+tm.tv_nsec;
}

//======================================================================
//  START OF ACTUAL TEST CODE
//======================================================================

#include <rdma/fabric.h>
#include <rdma/fabric.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>

// NOTE: Use of a key of 0 requests an "optimized MR", but using
// values like 1111 do not impact the performance issue.
#define MY_MR_KEY 0

static size_t iters;
static size_t nbytes;
static size_t segsize;
static void *my_buffer;
static void *my_segment;

static struct fid_fabric*    my_fabric;
static struct fid_domain*    my_domain;
static struct fid_ep*        my_ep;
static struct fid_cq*        my_cq;
static struct fid_av*        my_av;
static struct fid_mr*        my_mr;

// Allocate/initialize resources used in the test
void do_wireup(void) {
  struct fi_info *hints = fi_allocinfo();
  hints->caps = FI_RMA;
  hints->addr_format = FI_FORMAT_UNSPEC;
  // NOTE: `FI_DELIVERY_COMPLETE` is used in GASNet-EX, but changes to
  // this test to use `FI_TRANSMIT_COMPLETE` or `FI_INJECT_COMPLETE`
  // do not impact the performance issue.
  hints->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
  hints->ep_attr->type = FI_EP_RDM;
  hints->fabric_attr->prov_name = strdup("cxi");
  hints->domain_attr->name = strdup("cxi0");
  hints->domain_attr->threading = FI_THREAD_DOMAIN;
  hints->domain_attr->control_progress  = FI_PROGRESS_MANUAL;
  hints->domain_attr->resource_mgmt     = FI_RM_ENABLED;
  hints->domain_attr->av_type = FI_AV_TABLE;
  hints->domain_attr->mr_mode = FI_MR_ENDPOINT | FI_MR_ALLOCATED;

  // Get fi_info for the cxi provider with appropriate capabilities and settings
  struct fi_info *info = NULL;
  SAFE_CALL( fi_getinfo(FI_VERSION(1, 9), NULL, NULL, 0ULL, hints, &info) );

  // Create fabric and domain
  SAFE_CALL( fi_fabric(info->fabric_attr, &my_fabric, NULL) );
  SAFE_CALL( fi_domain(my_fabric, info, &my_domain, NULL) );

  // Create an RMA endpoint
  info->caps = FI_RMA;
  SAFE_CALL( fi_endpoint(my_domain, info, &my_ep, NULL) );

  // Done with both fi_info structs
  fi_freeinfo(hints);
  fi_freeinfo(info);

  // Create and bind a CQ to the endpoint
  struct fi_cq_attr cq_attr;
  memset(&cq_attr, 0, sizeof(cq_attr));
  cq_attr.format    = FI_CQ_FORMAT_DATA;
  cq_attr.size      = 0;
  cq_attr.wait_obj  = FI_WAIT_NONE;
  SAFE_CALL( fi_cq_open(my_domain, &cq_attr, &my_cq, NULL) );
  SAFE_CALL( fi_ep_bind(my_ep, &my_cq->fid, FI_TRANSMIT | FI_RECV) );

  // Create and bind an AV to the endpoint
  struct fi_av_attr av_attr;
  memset(&av_attr, 0, sizeof(av_attr));
  av_attr.type  = FI_AV_TABLE;
  av_attr.count = size;
  SAFE_CALL( fi_av_open(my_domain, &av_attr, &my_av, NULL) );
  SAFE_CALL( fi_ep_bind(my_ep, &my_av->fid, 0) );
  SAFE_CALL( fi_enable(my_ep) );

  // Populate the AV, exchanging endpoint addresses via PMI
  {
    // Must first query the namelen
    size_t namelen = 0;
    SAFE_CALL_VAL(-FI_ETOOSMALL, fi_getname(&my_ep->fid, NULL, &namelen) );
    void *my_addr = malloc(namelen);
    void *tmp_addr = malloc(namelen);
    // Publish own address into PMI key-value-store
    fi_getname(&my_ep->fid, my_addr, &namelen);
    kvs_put("av", rank, my_addr, namelen);
    // Populate the AV
    for (int i = 0; i < size; ++i) {
      if (i == rank) {
        SAFE_CALL_VAL(1, fi_av_insert(my_av, my_addr, 1, NULL, 0 , NULL) );
      } else {
        kvs_get("av", i, tmp_addr, namelen);
        SAFE_CALL_VAL(1, fi_av_insert(my_av, tmp_addr, 1, NULL, 0 , NULL) );
      }
    }
    free(tmp_addr);
    free(my_addr);
  }

  // Allocate a heap buffer, sized for the xfer
  SAFE_CALL( posix_memalign(&my_buffer, 4096, nbytes) );
  *(uint64_t *)my_buffer = 0; // zero-initialize first eight bytes

  // Allocate and register a segment w/ permissions for RMA access
#if 1
  // Option 1: Allocate segment using posix_memalign()
  // With this logic enabled, use of a `craype-hugepages*` environment
  // has been seen to eliminate the specific performance issue which
  // this reduced test case is meant to reproduce.  It it unknown if
  // the same is true of any other cases.
  if (!rank) fprintf(stderr, "Using segment allocation option 1\n");
  SAFE_CALL( posix_memalign(&my_segment, 4096, segsize) );
#elif 0
  // Option 2: Allocate 2MiB followed by segment, both using posix_memalign()
  // NOTE: With this logic enabled, use of a `craype-hugepages*` environment
  // does *NOT* eliminate the performance issue.  In fact, the result with a
  // large segment is *slower* than seen with GASNet-EX's testlarge benchmark.
  // This is despite differing from Option 1 by a single 2MiB allocation!
  if (!rank) fprintf(stderr, "Using segment allocation option 2\n");
  void *unused_ptr;
  SAFE_CALL( posix_memalign(&unused_ptr, 2*MiB, 2*MiB) );
  SAFE_CALL( posix_memalign(&my_segment, 4096, segsize) );
  free(unused_ptr);
#elif 0
  // Option 3: malloc(2MiB), followed by segment via posix_memalign()
  // NOTE: With this logic enabled, use of a `craype-hugepages*` environment
  // does *NOT* eliminate the performance issue.  In fact, the result with a
  // large segment is *slower* than seen with GASNet-EX's testlarge benchmark.
  // This is despite differing from Option 1 by a single 2MiB allocation!
  if (!rank) fprintf(stderr, "Using segment allocation option 3\n");
  void *unused_ptr = malloc(2*MiB);
  SAFE_CALL( posix_memalign(&my_segment, 4096, segsize) );
  free(unused_ptr);
#else
  // Option 4: Allocate segment using mmap(MAP_HUGETLB | MAP_ANONYMOUS)
  // NOTE: With this logic enabled, use of a `craype-hugepages*` environment
  // does *NOT* eliminate the performance issue.  In fact, the result with a
  // large segment is *slower* than seen with GASNet-EX's testlarge benchmark.
  if (!rank) fprintf(stderr, "Using segment allocation option 4\n");
  {
    // We need to align the allocation size to at least a multiple of 4096 to
    // keep mmap() happy, but use a multiple of 2MiB to be hugepage-friendly.
    size_t align = 2*MiB;
    size_t aligned_segsz = (segsize + align - 1) & ~align;
    // NOTE: performance issue remains regardless of MAP_SHARED vs MAP_PRIVATE
    const int mmap_flags = MAP_HUGETLB | MAP_ANONYMOUS | MAP_SHARED;
    my_segment = mmap(NULL, aligned_segsz, (PROT_READ|PROT_WRITE), mmap_flags, -1, 0);
    if (my_segment == MAP_FAILED) {
      FATAL("mmap() failed %d (%s)", errno, strerror(errno));
    }
  }
#endif
  *(uint64_t *)my_segment = 0; // zero-initialize first eight bytes
  {
    struct iovec iov = { my_segment, segsize };
    struct fi_mr_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mr_iov        = &iov;
    attr.iov_count     = 1;
    attr.access        = FI_REMOTE_READ | FI_REMOTE_WRITE;
    attr.requested_key = MY_MR_KEY;
    attr.context       = NULL;
    // NOTE: performance issue remains independent of which is chosen below
#if 1
    SAFE_CALL( fi_mr_reg(my_domain,
                         attr.mr_iov->iov_base, attr.mr_iov->iov_len,
                         attr.access, attr.offset, attr.requested_key,
                         0, &my_mr, attr.context) );
#else
    SAFE_CALL( fi_mr_regattr(my_domain, &attr, 0, &my_mr) );
#endif
    SAFE_CALL( fi_mr_bind(my_mr, &my_ep->fid, 0) );
    SAFE_CALL( fi_mr_enable(my_mr) );
  }

  SAFE_CALL( PMI_Barrier() );
}

// Poll the CQ, simply counting completions in this reproducer
static size_t cq_counter = 0;
void do_poll(void) {
 // NOTE: performance issue remains independent of which is chosen below
#if 0
  #define MAX_NUM_COMPLETIONS 1
#else
  #define MAX_NUM_COMPLETIONS 64
#endif
  struct fi_cq_data_entry re[MAX_NUM_COMPLETIONS];
  int rc = fi_cq_read(my_cq, (void *)&re, MAX_NUM_COMPLETIONS);
  if ((rc == -FI_EAGAIN) || !rc) {
     // cq is empty
  } else if (rc > 0) {
    cq_counter += rc;
  } else {
    FATAL("fi_cq_read() returned %d (%s)", rc, fi_strerror(-rc));
  }
}

// The actual test:
//
// Timed region on an active process includes:
//   Loops `iters` times, calling `fi_write()` with a length of
//   `nbytes` and targeting the passive peer's segment at an
//   offset of zero bytes.
//   Calls to poll the CQ are made if/when `fi_write()` returns
//   an "EAGAIN" indication, to ensure progress.
//   After all `fi_write()` calls have been issued, spin-poll the
//   CQ until completions have been processed for all writes.
// Meanwhile, the passive process spin-polls to ensure progress.
//
// Outside the timed region, the active process performs one more
// `fi_write()` to set the first byte of the remote segment to a
// non-zero value, which the passive peer observes as the indication
// that the test is done.  Note that in GASNet-EX, this "trick" is
// not necessary since there is significant more infrastructure such
// as `FI_MSG` endpoints and a barrier implementation.
//
void do_test(void) {
  if (active) {
    fi_addr_t peer = (fi_addr_t)(rank ^ 1);
    uint64_t t0 = now();
    // START of timed region
      // Issue `iters` non-blocking RMA Puts using `fi_write()`
      for (size_t i = 0; i < iters; ++i) {
        int rc = fi_write(my_ep, my_buffer, nbytes, NULL, peer, 0, MY_MR_KEY, NULL);
        while (rc == -FI_EAGAIN) {
          do_poll();
          rc = fi_write(my_ep, my_buffer, nbytes, NULL, peer, 0, MY_MR_KEY, NULL);
        };
        if (rc) {
          FATAL("fi_write() returned %d (%s)", rc, fi_strerror(-rc));
        }
      }
      // Stall for completion of all `fi_write()` operations
      while (cq_counter != iters) do_poll();
    // END of timed region
    uint64_t t1 = now();

    // Signal end of test to passive peer by writing anything non-zero to start of segment
    {
      static char one = '1';
      do {
        do_poll();
      } while (-FI_EAGAIN == fi_write(my_ep, &one, sizeof(one), NULL, peer, 0, MY_MR_KEY, NULL));
      // Error handling elided, since the actual test is already complete
    }

    // Report the flood bandwidth
    double how_long = (t1 - t0) * 1.e-9;
    double how_much = nbytes * iters * (1./GiB);
    printf("%d:\t%g GiB /  %g sec:  %.4g GiB/s\n", rank, how_much, how_long, how_much / how_long);
    fflush(stdout);
  } else {
    // A "trick" to detect termination w/o any additional comms resources
    do {
      do_poll();
    } while ( 0 == *(volatile uint64_t *)my_segment );
  }
}

int main(int argc, char **argv) {
  // Bootstrap the parallel job (not libfabric)
  do_init();

  // Argument processing
  const size_t dflt_iters = 100000;
  const size_t dflt_nbytes = 16*KiB;
  if (argc != 4) {
    if (!rank) {
      fprintf(stderr,
              "usage: %s iters nbytes segsize\n"
              "        iters: number of fi_write operations (default %zd)\n"
              "       nbytes: size of each fi_write() (default %zd)\n"
              "      segsize: size of the registered segment (defaults to nbytes)\n"
              "   All arguments are required, but zeros request the above defaults.\n",
              argv[0], dflt_iters, dflt_nbytes);
    }
    goto out;
  }

  // Require that process count is even
  if (size & 1) {
    if (!rank) {
      fprintf(stderr, "Error: this test must be run with an even number of processes.\n");
    }
    goto out;
  }

  iters = atol(argv[1]);
  if (!iters) iters = dflt_iters;

  nbytes = atol(argv[2]);
  if (!nbytes) nbytes = dflt_nbytes;

  segsize = atol(argv[3]);
  if (segsize < nbytes) segsize = nbytes;

  if (!rank) {
    printf("Running %zd iterations of fi_write() with len=%zd into a %zd byte remote segment\n",
           iters, nbytes, segsize);
    fflush(stdout);
  }

  // Allocate/intialize resources (libfabric and heap memory)
  do_wireup();

  // Run the test
  do_test();

out:
  // Have elided libfabric shutdown and free of application memory
  SAFE_CALL( PMI_Barrier() );
  PMI2_Finalize();
  return 0;
}
