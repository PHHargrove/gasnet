/*   $Source: bitbucket.org:berkeleylab/gasnet.git/extended-ref/gasnet_coll_log.h $
 * Description: PROOF-OF-CONCEPT implementation of collectives using only logrithmic team storage
 * Copyright 2015, Lawrence Berkeley National Laboratory
 * Terms of use are as specified in license.txt
 */

#ifndef _IN_GASNET_H
  #error This file is not meant to be included directly- clients should include gasnet.h
#endif

#ifndef _GASNET_COLL_LOG_H
#define _GASNET_COLL_LOG_H

#if GASNET_SEQ  /* No threads support in this proof-of-concept */

/* Register the replacement dispatchers: */
/* NONE YET */

#endif

#endif
