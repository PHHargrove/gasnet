/*   $Source: bitbucket.org:berkeleylab/gasnet.git/other/kinds/gasnet_ze.c $
 * Description: GASNet Memory Kinds Implementation for oneAPI Level Zero
 * Copyright (c) 2022, The Regents of the University of California
 * Terms of use are as specified in license.txt
 */

#define GASNETI_NEED_GASNET_MK_H 1
#include <gasnet_internal.h>
#include <gasnet_kinds_internal.h>

#if GASNET_HAVE_MK_CLASS_ZE // Else empty

#include <level_zero/ze_api.h>

GASNETI_IDENT(gasneti_IdentString_MKClassZE, "$GASNetMKClassZE: 1 $");

//
// Class-specific MK type and functions
//

typedef struct my_MK_s {
  GASNETI_MK_COMMON // Class-indep prefix

  // WIP: which of these do we need?
  ze_device_handle_t        device;
  ze_context_handle_t       context;
  uint32_t                  ordinal;
} *my_MK_t;

//
// Error checking/reporting wrapper
// TODO: strerror() like capability?
//
#define gasneti_check_zecall(op) do {                \
   ze_result_t _result = (op);                       \
    if_pf (_result != ZE_RESULT_SUCCESS ) {          \
      gasneti_fatalerror("%s returned %d", #op, _result); \
    }                                                \
  } while (0)

static const char *gasneti_formatmk_ze(gasneti_MK_t i_mk)
{
  my_MK_t kind = (my_MK_t) i_mk;
  return gasneti_dynsprintf("ZE(gex_zeDevice=%p, gex_zeContext=%p, gex_zeMemoryOrdinal=%u)",
                            (void*)kind->device, (void*)kind->context, (unsigned int)kind->ordinal);
}

static gasneti_mk_impl_t *get_impl(void);

//
// Class-specific MK_Create
//
int gasneti_MK_Create_ze(
            gasneti_MK_t                     *i_memkind_p,
            gasneti_Client_t                 client,
            const gex_MK_Create_args_t       *args,
            gex_Flags_t                      flags)
{
  gasneti_static_assert(sizeof(ze_device_handle_t) == sizeof(void*));
  gasneti_static_assert(sizeof(ze_context_handle_t) ==  sizeof(void*));
  
  // TODO: validation
  // is Device valid?
  // is Context valid?
  // is are Device and Context from the same Driver?
  // is MemoryOrdinal valid for this Device?

  my_MK_t result = (my_MK_t) gasneti_alloc_mk(client, get_impl(), flags);
  result->device = args->gex_args.gex_class_ze.gex_zeDevice;
  result->context = args->gex_args.gex_class_ze.gex_zeContext;
  result->ordinal = args->gex_args.gex_class_ze.gex_zeMemoryOrdinal;

  *i_memkind_p = (gasneti_MK_t) result;
  return GASNET_OK;
}

//
// Class-specific Segment_Create
//
static int gasneti_MK_Segment_Create_ze(
            gasneti_Segment_t                *i_segment_p,
            gasneti_MK_t                     i_mk,
            void *                           addr,
            uintptr_t                        size,
            gex_Flags_t                      flags)
{
  my_MK_t kind = (my_MK_t) i_mk;
  ze_result_t result;
  void * to_free = NULL;
  const char * failure = NULL;

  if (addr) { // Client-allocated
    // check that addr properties match the kind
    ze_memory_allocation_properties_t props = {0,};
    ze_device_handle_t device = NULL;
    gasneti_check_zecall( zeMemGetAllocProperties(kind->context, addr, &props, &device) );
    if (props.type == ZE_MEMORY_TYPE_UNKNOWN) {
      failure = "address was not allocated with the gex_zeContext passed to gex_MK_Create()";
    } else if (props.type != ZE_MEMORY_TYPE_DEVICE) {
      failure = "address is not device memory";
    } else if (device != kind->device) {
      failure = "address was not allocated from the gex_zeDevice passed to gex_MK_Create()";
    }
  } else { // GASNet-allocated
    // WIP - error handling, including OOM and device's maxium allocation size
    ze_device_mem_alloc_desc_t allocDesc = {0,};
    allocDesc.ordinal = kind->ordinal;
    result = zeMemAllocDevice( kind->context, &allocDesc, size, 0, kind->device, &addr );
    switch (result) {
      case ZE_RESULT_SUCCESS:
        break;

      case ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY:
        failure = "ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY";
        break;

      case ZE_RESULT_ERROR_UNSUPPORTED_SIZE:
        failure = "ZE_RESULT_ERROR_UNSUPPORTED_SIZE";
        break;

      default:
        failure = "unknown failure of zeMemAllocDevice"; // TODO: anything like strerror?
        break;
    }
    to_free = addr;
  }

  if (failure) {
    const char *msg = gasneti_dynsprintf("GEX_MK_CLASS_ZE: %s", failure);
    GASNETI_RETURN_ERRR(BAD_ARG, msg);
  }

  gasneti_Client_t client = i_mk->_client;
  gex_MK_t e_mk = gasneti_export_mk(i_mk);
  gasneti_Segment_t i_segment = gasneti_alloc_segment(client, addr, size, e_mk, !to_free, flags);
  i_segment->_opaque_mk_use = to_free;

  *i_segment_p = i_segment;

  return GASNET_OK;
}

static void gasneti_MK_Segment_Destroy_ze(
           gasneti_Segment_t                i_segment)
{
  void *to_free = i_segment->_opaque_mk_use;
  if (to_free) {
    my_MK_t kind = (my_MK_t) gasneti_import_mk_nonhost(i_segment->_kind);
    gasneti_check_zecall( zeMemFree(kind->context, to_free) );
  }
}

//
// Class-specific "impl(ementation)": constants and function pointers.
//
// Due to lack of designated initializers in GASNet's required C99 subset, we
// address the fragility as the structure grows or changes by lazy explicit
// initialization.
static gasneti_mk_impl_t *get_impl(void) {
  // Static storage duration ensures these are zero-initialized
  static gasneti_mk_impl_t the_impl;
  static gasneti_mk_impl_t *result;

  if (!result) {
    static gasneti_mutex_t lock = GASNETI_MUTEX_INITIALIZER;
    gasneti_mutex_lock(&lock);
    if (!result) {
      the_impl.mk_class     = GEX_MK_CLASS_ZE;
      the_impl.mk_name      = "ZE";
      the_impl.mk_sizeof    = sizeof(struct my_MK_s);

      the_impl.mk_format    = &gasneti_formatmk_ze;
      the_impl.mk_destroy   = NULL; // No class-specific MK_Destroy needed
      the_impl.mk_segment_create
                            = gasneti_MK_Segment_Create_ze;
      the_impl.mk_segment_destroy
                            = gasneti_MK_Segment_Destroy_ze;

      gasneti_sync_writes();
      result = &the_impl;
    }
    gasneti_mutex_unlock(&lock);
  } else {
    gasneti_sync_reads();
  }

  gasneti_assert(result);
  return result;
}

// Determine the kind's device ordinal, used by at least libfabric.
// This is basic device enumeration, terminated when a matching device is found.
// This works because enumeration calls are guaranteed to return the same
// handle for each device every time.
int gasneti_mk_ze_device_ordinal(void *device_handle_arg)
{
  ze_device_handle_t theDevice = (ze_device_handle_t) device_handle_arg;

  uint32_t driverCount = 0;
  gasneti_check_zecall( zeDriverGet(&driverCount, NULL) );

  ze_driver_handle_t *driverArray = gasneti_malloc(driverCount * sizeof(ze_driver_handle_t));
  gasneti_check_zecall( zeDriverGet(&driverCount, driverArray) );

  ze_device_handle_t *deviceArray = NULL;

  int count = 0;
  int result = -1;
  for (uint32_t i = 0; i < driverCount; ++i) {
    ze_driver_handle_t driver = driverArray[i];
    uint32_t deviceCount = 0;
    gasneti_check_zecall( zeDeviceGet(driver, &deviceCount, NULL) );
    deviceArray = gasneti_realloc(deviceArray, deviceCount * sizeof(ze_device_handle_t));
    gasneti_check_zecall( zeDeviceGet(driver, &deviceCount, deviceArray) );

    for (uint32_t j = 0; j < deviceCount; ++j) {
      ze_device_handle_t currDevice = deviceArray[j];

      // Only count GPU devices in our enumeration
      // This is needed because, in general, we don't control the zeInit() arguments
      ze_device_properties_t deviceProperties = {0,};
      gasneti_check_zecall( zeDeviceGetProperties(currDevice, &deviceProperties) );
      if (deviceProperties.type != ZE_DEVICE_TYPE_GPU) continue;

      if (currDevice == theDevice) {
        result = count;
        goto done;
      }

      ++count;
    }
  }

done:
  gasneti_free(deviceArray);
  gasneti_free(driverArray);

  return result;
}


void gasneti_mk_ze_dmabuf(gasneti_Segment_t i_segment, int *dmabuf_fd_p, uintptr_t *offset_p)
{
  my_MK_t kind = (my_MK_t) gasneti_import_mk_nonhost(i_segment->_kind);
  ze_context_handle_t context = kind->context;
  void *addr = i_segment->_addr;

  ze_ipc_mem_handle_t handle;
  gasneti_check_zecall( zeMemGetIpcHandle(context, addr, &handle) );
  *dmabuf_fd_p = (int) *(uintptr_t*) handle.data;

  void *base;
  gasneti_check_zecall( zeMemGetAddressRange(context, addr, &base, NULL) );
  *offset_p = (uintptr_t)addr - (uintptr_t)base;
}

#endif
