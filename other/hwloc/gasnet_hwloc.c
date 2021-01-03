/*   $Source: bitbucket.org:berkeleylab/gasnet.git/other/hwloc/gasnet_hwloc.c $
 * Description: GASNet conduit-independent hwloc utilities
 * Copyright 2020, The Regents of the University of California
 * Terms of use are as specified in license.txt
 */

#include <gasnet_internal.h>
#include <gasnet_hwloc_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if GASNETI_HAVE_HWLOC_LIB
  #include "hwloc.h"
  #ifndef HWLOC_API_VERSION
    #error hwloc.h did not define HWLOC_API_VERSION
  #endif
  #define USE_HWLOC_LIB 1
  #undef USE_HWLOC_UTILS
  typedef hwloc_obj_type_t gasneti_hwloc_obj_type_t;
  typedef hwloc_cpuset_t   gasneti_hwloc_cpuset_t;
#elif GASNETI_HAVE_HWLOC_UTILS
  #undef HWLOC_API_VERSION
  #undef USE_HWLOC_LIB
  #define USE_HWLOC_UTILS 1
  typedef const char *gasneti_hwloc_obj_type_t;
  typedef const char *gasneti_hwloc_cpuset_t;
#else
  #error Attempting to compile gasnet_hwloc.c without necessary support
#endif

// Convert string to gasneti_hwloc_obj_type_t
// Returns 0 on success, negative on error
//
// This wrapper hides API changes over the life of hwloc
// WIP - test 2.0.0 and OLD api
static int string_to_obj_type(const char *string, gasneti_hwloc_obj_type_t *result)
{
  #if USE_HWLOC_UTILS
    *result = gasneti_strdup(string);
    return 0;
  #elif HWLOC_API_VERSION >= 0x020000 // 2.0.0
    #warning GASNet untested with HWLOC API 2.0
    return hwloc_type_sscanf(string, result, NULL, 0);
  #elif HWLOC_API_VERSION >= 0x010900 // 1.9.0
    return hwloc_obj_type_sscanf(string, result, NULL, NULL, 0);
  #else
    #warning GASNet untested with HWLOC API < 1.9
    *result = hwloc_obj_type_of_string(string);
    return ((int)(*result) < 0) ? -1 : 0;
  #endif
}

// Given the keyname, yield (in *result) the hwloc object type (or the given
// default) of environment variable "[keyname]_TYPE".
// Returns 0 on success, negative on error.
static int get_selector_type(gasneti_hwloc_obj_type_t *result, const char *keyname, const char *dflt_type)
{
  const char suffix[] = "_TYPE";
  size_t len = strlen(keyname) + sizeof(suffix); // sizeof includes the terminator
  char *envvar = gasneti_malloc(len);
  strcpy(envvar, keyname); gasneti_assert_uint(strlen(envvar) ,<, len);
  strcat(envvar, suffix);  gasneti_assert_uint(strlen(envvar) ,==, len-1);

  char *envval = gasneti_getenv_withdefault(envvar, dflt_type);

  int rc = string_to_obj_type(envval, result);
  if (rc < 0) {
    gasneti_console_message("WARNING",
                            "%s = '%s' is invalid.  Using default '%s' instead.",
                            envvar, envval, dflt_type);
    rc = string_to_obj_type(dflt_type, result);
    gasneti_assert_int(rc ,==, 0);
  }

  gasneti_free(envvar);

  // Currently non-zero only if dflt_type is invalid
  return rc;
}

// For a given keyname:
// 1. Look for a hwloc object type in env var "[kename]_TYPE", or dflt_type if none.
// 2. Find the current procs binding(s) for the given type
// 3. Return the value of env var "[keyname]_[binding]", or of "[keyname]", or dflt_val
char *gasneti_hwloc_getenv_withdefault(const char *keyname, const char *dflt_val, const char *dflt_type)
{
  char *result = gasneti_getenv_withdefault(keyname, dflt_val);

  char *suffix = NULL;
  gasneti_hwloc_obj_type_t type = (gasneti_hwloc_obj_type_t)0;
  gasneti_hwloc_cpuset_t cpuset = NULL;

  // Step 1 - hwloc object type
  gasneti_assert_zeroret( get_selector_type(&type, keyname, dflt_type) );

  // Step 2a - current procs cpu binding
#if USE_HWLOC_LIB
  int topo_is_init = 0;
  hwloc_topology_t topology;
  if (hwloc_topology_init(&topology) < 0) {
    // WIP - better error handling
    goto out;
  }
  topo_is_init = 1;
  // Enable "whole system" mode for uniform counting/naming
  #if HWLOC_API_VERSION >= 0x020100 // 2.1.0
    (void)hwloc_topology_set_flags(topology, HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED);
  #else
    (void)hwloc_topology_set_flags(topology, HWLOC_TOPOLOGY_FLAG_WHOLE_SYSTEM);
  #endif
  cpuset = hwloc_bitmap_alloc();
  if (!cpuset ||
      (hwloc_topology_load(topology) < 0) ||
      (hwloc_get_cpubind(topology, cpuset, HWLOC_CPUBIND_PROCESS) < 0 )) {
    // WIP - better error handling
    goto out;
  }
#else
  { 
    FILE *stream = popen(GASNETI_HWLOC_BIND_PATH " --get", "r");
    if (!stream) {
      // WIP - error handling
      goto out;
    }
    // Note: gasneti_hwloc_cpuset_t is "char *"
    char *buf = NULL;
    size_t n = 0;
    ssize_t line_len = gasneti_getline(&buf, &n, stream);
    if (line_len < 1) {
      // WIP - alternative error handling??
      gasneti_free(buf);
      cpuset = "all"; // accepted by hwloc-calc
    } else {
      if (buf[line_len-1] == '\n') { // strip trailing newline
        buf[line_len-1] = '\0';
        line_len -= 1;
      }
      cpuset = buf;
    }
    pclose(stream);
  }
#endif

  // Step 2b - map cpu binding to requested object type
#if USE_HWLOC_LIB
  {
    int count = hwloc_get_nbobjs_by_type(topology, type);
    size_t suffix_len = 0;
    size_t alloc_len = 1 + 2*count; // long enough for all common cases
    suffix = gasneti_malloc(alloc_len); suffix[0] = '\0';
    for (int i = 0; i < count; ++i) {
      hwloc_obj_t obj = hwloc_get_obj_by_type(topology, type, i);
      gasneti_assert(obj);
      if (hwloc_bitmap_intersects(cpuset, obj->cpuset)) {
        char tmp[16];
        size_t tmplen = snprintf(tmp, sizeof(tmp), "_%d", i);
        gasneti_assert_uint(tmplen ,>, 1);
        suffix_len += tmplen;
        if (suffix_len >= alloc_len) {
          alloc_len = GASNETI_ALIGNUP(1 + suffix_len, 8);
          suffix = gasneti_realloc(suffix, alloc_len);
        }
        strcat(suffix, tmp);
        gasneti_assert_uint(strlen(suffix) ,<, alloc_len);
      }
    }
  }
#else
  { 
    char base_cmd[] = GASNETI_HWLOC_CALC_PATH " --sep _ --intersect ";
    size_t len = sizeof(base_cmd) + strlen(type) + 1 + strlen(cpuset); // sizeof() includes '\0'
    char *cmd = gasneti_malloc(len);
    size_t len2 = snprintf(cmd, len, "%s%s %s", base_cmd, type, cpuset);
    gasneti_assert_uint(strlen(cmd) ,==, len-1);
    FILE *stream = popen(cmd, "r");
    if (!stream) {
      // WIP - error handling
      goto out;
    }
    char *buf = NULL;
    size_t n = 0;
    ssize_t line_len = gasneti_getline(&buf, &n, stream);
    pclose(stream);
    if (line_len < 1) {
      // WIP - alternative error handling??
      goto out;
    }
    if (buf[line_len-1] == '\n') { // strip trailing newline
      buf[line_len-1] = '\0';
      line_len -= 1;
    }
    if (line_len) {
      suffix = gasneti_malloc(line_len + 2);
      suffix[0] = '_';
      strcpy(suffix+1, buf);
      gasneti_assert_uint(strlen(suffix) ,==, line_len + 1);
    }
    gasneti_free(buf);
  }
#endif
    
  // Step 3 - query the environment first with suffix, and w/o if needed
  if (suffix && suffix[0]) {
    size_t fulllen = strlen(keyname) + strlen(suffix) + 1;
    char *fullkey = gasneti_malloc(fulllen);
    strcpy(fullkey, keyname);
    strcat(fullkey, suffix);
    gasneti_assert_uint(strlen(fullkey) ,==, fulllen-1);
    char *tmp = gasneti_getenv_withdefault(fullkey, NULL);
    gasneti_free(fullkey);
    if (tmp) result = tmp;
  }

out:
#if USE_HWLOC_LIB
  if (cpuset) hwloc_bitmap_free(cpuset);
  if (topo_is_init) hwloc_topology_destroy(topology);
#else
  gasneti_free((void *)type);
  gasneti_free((void *)cpuset);
#endif

  return result;
}
