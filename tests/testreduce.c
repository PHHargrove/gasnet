/*   $Source: bitbucket.org:berkeleylab/gasnet.git/tests/testreduce.c $
 * Description: test of user-defined reductions
 * Copyright 2018, The Regents of the University of California
 * Terms of use are as specified in license.txt
 */

#include <gasnetex.h>
#include <gasnet_coll.h>
#include <test.h>

#include <string.h>
#include <stdint.h>
#include <float.h>
#include <math.h>

// Size of vector for built-in types
#define Nelem 64

// Size of vector for string-concat types
#define Nstr 4

// Polymorphic operator for a commutative operation: addition
void op_ADD(const void * arg1,
            void *       arg2_and_out,
            size_t       count,
            const void * cdata)
{
  gex_DT_t dt = (gex_DT_t)(uintptr_t)cdata;
  switch (dt) {
    case GEX_DT_I32:
    case GEX_DT_U32: {
      const uint32_t * restrict x = arg1;
            uint32_t * restrict y = arg2_and_out;
      for (size_t i = 0; i < count; ++i) y[i] += x[i];
      break;
    }
    case GEX_DT_I64:
    case GEX_DT_U64: {
      const uint64_t * restrict x = arg1;
            uint64_t * restrict y = arg2_and_out;
      for (size_t i = 0; i < count; ++i) y[i] += x[i];
      break;
    }
    case GEX_DT_FLT: {
      const float * restrict x = arg1;
            float * restrict y = arg2_and_out;
      for (size_t i = 0; i < count; ++i) y[i] += x[i];
      break;
    }
    case GEX_DT_DBL: {
      const double * restrict x = arg1;
            double * restrict y = arg2_and_out;
      for (size_t i = 0; i < count; ++i) y[i] += x[i];
      break;
    }
  }
}

// Type and operator for a non-commutative operation: strcat
// "Post-concatenates" two null-terminated strings (e.g. "arg2 .= arg1" in perl)
static size_t myDT_sz;
void op_concat(const void * arg1,
               void *       arg2_and_out,
               size_t       count,
               const void * cdata)
{
  const size_t dt_sz = (size_t)(uintptr_t)cdata;
  const char * restrict x = arg1;
        char * restrict y = arg2_and_out;
  for (size_t i = 0; i < count; ++i, x+=dt_sz, y+=dt_sz) {
    strcat(y,x);
  }
}

// Convenience to avoid calling TEST_RAND with lo==hi
#define RAND_ROOT(size) ((size == 1) ? 0 : TEST_RAND(0, size-1))

// Test ADD (both built-in and user-defined for a built-in DT
#define TEST_ADD(TYPE) do {                                                   \
  int failures = 0;                                                           \
  for (int i = 0; i < iters; ++i) {                                           \
    gex_Rank_t root = RAND_ROOT(size);                                        \
    gex_Event_Wait(                                                           \
      gex_Coll_ReduceToOneNB(myteam, root, TYPE##_ans, TYPE##_val,            \
                             GEX_DT_##TYPE, sizeof(TYPE##_ans[0]), Nelem,     \
                             GEX_OP_ADD, NULL, NULL, 0));                     \
    if (root == rank) {                                                       \
      for (int j = 0; j < Nelem; ++j) {                                       \
        uint64_t got = TYPE##_ans[j];                                         \
        if (got != correct[j]) {                                              \
          MSG("Mismatch GEX_OP_ADD(GEX_DT_" #TYPE "), iter=%i, elem=%i"       \
              " (got=%" PRIu64 ", want=%" PRIu64 ")", i, j, got, correct[j]); \
          ++failures;                                                         \
        }                                                                     \
      }                                                                       \
    }                                                                         \
    gex_Event_Wait(                                                           \
      gex_Coll_ReduceToOneNB(myteam, root, TYPE##_ans, TYPE##_val,            \
                             GEX_DT_##TYPE, sizeof(TYPE##_ans[0]), Nelem,     \
                             GEX_OP_USER, &op_ADD,                            \
                             (void*)(uintptr_t)GEX_DT_##TYPE, 0));            \
    if (root == rank) {                                                       \
      for (int j = 0; j < Nelem; ++j) {                                       \
        uint64_t got = TYPE##_ans[j];                                         \
        if (got != correct[j]) {                                              \
          MSG("Mismatch GEX_OP_USER(GEX_DT_" #TYPE "), iter=%i, elem=%i"      \
              " (got=%" PRIu64 ", want=%" PRIu64 ")", i, j, got, correct[j]); \
          ++failures;                                                         \
        }                                                                     \
      }                                                                       \
    }                                                                         \
  }                                                                           \
  if (failures) ERR("GEX_DT_" #TYPE ": %d failures", failures);               \
  gex_Event_Wait(gex_Coll_BarrierNB(myteam,0));                               \
} while (0)

static int iters = 0;

int main(int argc, char **argv)
{
  gex_Client_t      myclient;
  gex_EP_t          myep;
  gex_TM_t          myteam;

  gex_Client_Init(&myclient, &myep, &myteam, "testreduce", &argc, &argv, 0);

  int arg = 1;
  if (argc > arg) { iters = atoi(argv[arg]); ++arg; }
  if (!iters) iters = 1000;

  unsigned int seed = 0;
  if (argc > arg) { seed = atoi(argv[arg]); ++arg; }

  test_init("testreduce",0,"(iters) (seed)");

  if (seed == 0) {
    seed = (((unsigned int)TIME()) & 0xFFFF);
    gex_Event_Wait(gex_Coll_BroadcastNB(myteam, 0, &seed, &seed, sizeof(seed), 0));
  }
  TEST_SRAND(seed); // SAME seed

  MSG0("Running %i iterations of user-defined reduction tests (seed = %u).", iters, seed);

  gex_Rank_t rank = gex_TM_QueryRank(myteam);
  gex_Rank_t size = gex_TM_QuerySize(myteam);

  //
  // GEX_OP ADD
  // Test for every type which can exactly represent Sum(ranks).
  // This tests both built-in and user-defines ADD operations
  // TODO: split to make a team small enough to test instead of skipping?
  // TODO: args to limit which tests?
  //
  {
    int32_t   I32_ans[Nelem], I32_val[Nelem];
    uint32_t  U32_ans[Nelem], U32_val[Nelem];
    int64_t   I64_ans[Nelem], I64_val[Nelem];
    uint64_t  U64_ans[Nelem], U64_val[Nelem];
    float     FLT_ans[Nelem], FLT_val[Nelem];
    double    DBL_ans[Nelem], DBL_val[Nelem];
    uint64_t  correct[Nelem];

    assert_always(size <= INT32_MAX - Nelem);
    for (int i = 0; i < Nelem; ++i) {
      I32_val[i] = U32_val[i] = (rank + i);
      I64_val[i] = U64_val[i] = (rank + i);
      FLT_val[i] = DBL_val[i] = (rank + i);
      correct[i] = ((size + i) * (size + i - 1) - i * (i - 1)) / 2;
    }

    uint64_t biggest = correct[Nelem-1];
    if (biggest <= (uint64_t) INT32_MAX) {
      MSG0("Running ADD:GEX_DT_I32 test...");
      TEST_ADD(I32);
    } else {
      MSG0("WARNING: skipping ADD:GEX_DT_I32 test (would overflow)");
    }
    if (biggest <= (uint64_t)UINT32_MAX) {
      MSG0("Running ADD:GEX_DT_U32 test...");
      TEST_ADD(U32);
    } else {
      MSG0("WARNING: skipping ADD:GEX_DT_U32 test (would overflow)");
    }
    MSG0("Running ADD:GEX_DT_I64 test...");
    TEST_ADD(I64);
    MSG0("Running ADD:GEX_DT_U64 test...");
    TEST_ADD(U64);
    if (biggest < (uint64_t)pow(FLT_RADIX, FLT_MANT_DIG)) {
      MSG0("Running ADD:GEX_DT_FLT test...");
      TEST_ADD(FLT);
    } else {
      MSG0("WARNING: skipping ADD:GEX_DT_FLT test (exceeds mantissa bits)");
    }
    if (biggest < (uint64_t)pow(FLT_RADIX, DBL_MANT_DIG)) {
      MSG0("Running ADD:GEX_DT_DBL test...");
      TEST_ADD(DBL);
    } else {
      MSG0("WARNING: skipping ADD:GEX_DT_DBL test (exceeds mantissa bits)");
    }
  }

  //
  // TODO: test the remaining built-in operators.
  //
  // Validation:
  // Option 1: Assuming the user-defined ADD passed above, use a user-defined
  //           reduction as the reference for validation of each built-in
  //           operator.
  // Option 2: Follow the pattern of the ADD test above, and chose inputs that
  //           provide a locally computable reference for validation.
  //
  // GEX_OP_MULT:
  // There is concern over FP Mult and exact reproducibility.
  // If (and only if) we assume a "high-quality" implementation with full
  // reproducability of the order of evaliuation it *might* be reasonable to
  // exect bit-wise idenitcal results from the built-in and user-defined
  // versions of the same operator (use Option 1, above).  However, that sounds
  // risky.
  // An alternative to use Option 2, picking the input with care such that the
  // product will never (even at an intermediate value in the worst-case
  // application of commutativity) require more than the available mantissa
  // digits (24 for IEEE float).
  // Another alternative is to test for equality within "epsilon".  However,
  // without help from a skilled numerical analyst (or a text by one), it is
  // not obvious how that would need to scale with the number of ranks.
  //
  // GEX_OP_{MIN,MAX}:
  // No issues are anticipated, as long as we do not include Nan, Inf or
  // negative-0 among the FP inputs.
  //
  // GEX_OP_{AND,OR}:
  // Care is needed to avoid inputs which "saturate" the output.  Otherwise the
  // values 0 and ~0 could be arrived at "accidentally" and still pass
  // validation.
  //
  // GEX_OP_XOR:
  // No issues are anticipated.
  //

  //
  // Test "concat" operator on user-defined data type (a fixed-len char[])
  //
  MSG0("Running GEX_DT_USER tests...");
  {
    // Use just 'A' - 'Z' for ease of debugging by humans
    #define BASE_CHAR 'A'
    #define NUM_CHAR  26
    size_t dt_sz = size + 1; // One for '\0' terminator
    char InStrings[Nstr * dt_sz], OutStrings[Nstr * dt_sz];
    for (size_t i = 0; i < Nstr; ++i) {
      char *p = InStrings + i * dt_sz;
      p[0] = BASE_CHAR + ((rank + i) % NUM_CHAR);
      p[1] = '\0';
    }

    // GEX_OP_USER(GEX_DT_USER): OP(x,y) := strcat(y,x)
    // Due to permitted assumption of commutativity, order is unspecified.
    // However, the reduction must preserve number of instances of each char.
    int failures = 0;
    for (int i = 0; i < iters; ++i) {
      gex_Rank_t root = RAND_ROOT(size);
      gex_Event_Wait(
        gex_Coll_ReduceToOneNB(myteam, root, OutStrings, InStrings,
                               GEX_DT_USER, dt_sz, Nstr,
                               GEX_OP_USER, &op_concat,
                               (void*)(uintptr_t)dt_sz, 0));
      if (rank==root) {
        for (size_t j = 0; j < Nstr; ++j) {
          const char *str = OutStrings + j * dt_sz;
          // Count the number of occurances of each char
          gex_Rank_t tally[NUM_CHAR];
          memset(tally, 0, sizeof(tally));
          for (size_t k = 0; k < (dt_sz-1); ++k) {
            uint8_t idx = str[k] - BASE_CHAR;
            if (idx > 25) {
              ++failures;
            } else {
              ++tally[idx];
            }
          }
          // Check counts against the expected ones
          gex_Rank_t numer = size / NUM_CHAR;
          gex_Rank_t denom = size % NUM_CHAR;
          for (int k = 0; k < NUM_CHAR; ++k) {
            size_t expect = (numer + (k < denom));
            failures += (tally[(j + k) % NUM_CHAR] != expect);
          }
        }
      }
    }
    if (failures) ERR("GEX_OP_USER(GEX_DT_USER): %d failures", failures);
    gex_Event_Wait(gex_Coll_BarrierNB(myteam,0));

    // GEX_OP_USER_NC(GEX_DT_USER): OP(x,y) := strcat(y,x)
    // Due to non-commutativity, order must be preserved.
  #if 0 // TODO: enabled once GEX_OP_USER_NC is implemented
    failures = 0;
    for (int i = 0; i < iters; ++i) {
      gex_Rank_t root = RAND_ROOT(size);
      gex_Event_Wait(
        gex_Coll_ReduceToOneNB(myteam, root, OutStrings, InStrings,
                               GEX_DT_USER, dt_sz, Nstr,
                               GEX_OP_USER_NC, &op_concat,
                               (void*)(uintptr_t)dt_sz, 0));
      if (rank==root) {
        for (size_t j = 0; j < Nstr; ++j) {
          // validate result against properly ordered (lexically reversed) result
          const char *str = OutStrings + j * dt_sz;
          int idx = (j + (size - 1)) % NUM_CHAR;
          for (size_t k = 0; k < (dt_sz-1); ++k) {
            failures += (str[k] != BASE_CHAR + idx);
             // not using % since (-1 % x) is -1, not (x-1)
            idx = idx ? (idx - 1) : (NUM_CHAR - 1);
          }
        }
      }
    }
    if (failures) ERR("GEX_OP_USER_NC(GEX_DT_USER): %d failures", failures);
    gex_Event_Wait(gex_Coll_BarrierNB(myteam,0));
  #endif
  }

  MSG0("done.");
  gasnet_exit(0);

  /* Not reached in most implementations */
  return 0;
}
