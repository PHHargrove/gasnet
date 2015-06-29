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
#define gasnete_coll_broadcast_nb gasnete_coll_broadcast_nb_log
#define gasnete_coll_scatter_nb gasnete_coll_scatter_nb_log

#endif

/* Add tree types: */
#define GASNETE_COLL_TREE_CLASS_EXTRA \
        GASNETE_COLL_TREE_CLASS_LOG_SCAT1, \
        GASNETE_COLL_TREE_CLASS_LOG_SCAT2, \
        GASNETE_COLL_TREE_CLASS_LOG_SCAT3


#endif
