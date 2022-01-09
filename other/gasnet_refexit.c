/*   $Source: bitbucket.org:berkeleylab/gasnet.git/other/gasnet_refexit.c $
 * Description: Reference implementation of gasnet_exit()
 * Copyright 2022, The Regents of the University of California
 * Terms of use are as specified in license.txt
 */

/*
 * Guidance for conduit writers.
 *
 * This file contains a reference implementation of exit handling, suitable
 * for use in most conduits, with minimal additional effort.
 *
 * Overview
 * ========
 *
 * The expected use is the following lines in gasnet_core.c:
     // use reference implementation of exit handling
     #define ... // any necessary settings described below
     #define GASNETI_GASNET_REFEXIT_C 1
     #include "gasnet_refexit.c"
     #undef GASNETI_GASNET_REFEXIT_C
 * and that its `gasnetc_handlers[]` contains `GASNETC_REFEXIT_HANDLERS`.
 *
 * This model allows for all conduit-specific exit logic to be file-scoped,
 * including conduit-specific "hooks" and any associated state.
 *
 * All identifiers defined in this file use a prefix of `gasnetc_refexit_`  or
 * `GASNETC_REFEXIT_`.
 *
 * Since there is no header to declare these identifiers, any conduit-specific
 * functions referencing such identifiers will need to be defined after the
 * `#include` of this file.  If these are used as hooks, then the probably need
 * to be declared prior to the `#include`.
 *
 * Conduit-facing identifiers, with the hooks and controls.
 * ========================================================
 *
 * NOTE: This is a WORK IN PROGRESS
 *
 */

#ifndef GASNETI_GASNET_REFEXIT_C
  #error This file not meant to be compiled directly - it should be included by gasnet_core.c
#endif
