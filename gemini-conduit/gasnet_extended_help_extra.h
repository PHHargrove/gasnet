/*   $Source: bitbucket.org:berkeleylab/gasnet.git/gemini-conduit/gasnet_extended_help_extra.h $
 * Description: GASNet Extended gemini-specific Header
 * Terms of use are as specified in license.txt
 */

#ifndef _IN_GASNET_H
  #error This file is not meant to be included directly- clients should include gasnet.h
#endif

#ifndef _GASNET_EXTENDED_HELP_EXTRA_H
#define _GASNET_EXTENDED_HELP_EXTRA_H

/*
  Extensions:
  ==============================
*/

#if GASNETC_GNI_FETCHOP
/* Proof-of-concept GNI uint{32,64}_t fetch-and-op.
 * Not supported, and subject to change or removal.
 */

#define GASNETX_FETCHOP_DECLS(_name,_type)                        \
    extern void _gasnetX_fetch##_name(                            \
                _type *dest, gasnet_node_t node, _type *src,      \
                _type operand GASNETE_THREAD_FARG);               \
    extern gasnet_handle_t _gasnetX_fetch##_name##_nb(            \
                _type *dest, gasnet_node_t node, _type *src,      \
                _type operand GASNETE_THREAD_FARG)                \
                GASNETI_WARN_UNUSED_RESULT;                       \
    extern void _gasnetX_fetch##_name##_nbi(                      \
                _type *dest, gasnet_node_t node, _type *src,      \
                _type operand GASNETE_THREAD_FARG);               \
    extern _type _gasnetX_fetch##_name##_val(                     \
                gasnet_node_t node, _type *src,                   \
                _type operand GASNETE_THREAD_FARG);               \
    extern gasnet_valget_handle_t _gasnetX_fetch##_name##_nb_val( \
                gasnet_node_t node, _type *src,                   \
                _type operand GASNETE_THREAD_FARG)                \
                GASNETI_WARN_UNUSED_RESULT;

GASNETX_FETCHOP_DECLS(add_u64, uint64_t)
#define gasnetX_fetchadd_u64(dest,node,src,operand) \
        _gasnetX_fetchadd_u64(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchadd_u64_nb(dest,node,src,operand) \
        _gasnetX_fetchadd_u64_nb(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchadd_u64_nbi(dest,node,src,operand) \
        _gasnetX_fetchadd_u64_nbi(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchadd_u64_val(node,src,operand) \
        _gasnetX_fetchadd_u64_val(node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchadd_u64_nb_val(node,src,operand) \
        _gasnetX_fetchadd_u64_nb_val(node,src,operand GASNETE_THREAD_GET)

GASNETX_FETCHOP_DECLS(and_u64, uint64_t)
#define gasnetX_fetchand_u64(dest,node,src,operand) \
        _gasnetX_fetchand_u64(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchand_u64_nb(dest,node,src,operand) \
        _gasnetX_fetchand_u64_nb(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchand_u64_nbi(dest,node,src,operand) \
        _gasnetX_fetchand_u64_nbi(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchand_u64_val(node,src,operand) \
        _gasnetX_fetchand_u64_val(node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchand_u64_nb_val(node,src,operand) \
        _gasnetX_fetchand_u64_nb_val(node,src,operand GASNETE_THREAD_GET)

GASNETX_FETCHOP_DECLS(or_u64, uint64_t)
#define gasnetX_fetchor_u64(dest,node,src,operand) \
        _gasnetX_fetchor_u64(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchor_u64_nb(dest,node,src,operand) \
        _gasnetX_fetchor_u64_nb(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchor_u64_nbi(dest,node,src,operand) \
        _gasnetX_fetchor_u64_nbi(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchor_u64_val(node,src,operand) \
        _gasnetX_fetchor_u64_val(node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchor_u64_nb_val(node,src,operand) \
        _gasnetX_fetchor_u64_nb_val(node,src,operand GASNETE_THREAD_GET)

GASNETX_FETCHOP_DECLS(xor_u64, uint64_t)
#define gasnetX_fetchxor_u64(dest,node,src,operand) \
        _gasnetX_fetchxor_u64(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchxor_u64_nb(dest,node,src,operand) \
        _gasnetX_fetchxor_u64_nb(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchxor_u64_nbi(dest,node,src,operand) \
        _gasnetX_fetchxor_u64_nbi(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchxor_u64_val(node,src,operand) \
        _gasnetX_fetchxor_u64_val(node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchxor_u64_nb_val(node,src,operand) \
        _gasnetX_fetchxor_u64_nb_val(node,src,operand GASNETE_THREAD_GET)

GASNETX_FETCHOP_DECLS(add_u32, uint32_t)
#define gasnetX_fetchadd_u32(dest,node,src,operand) \
        _gasnetX_fetchadd_u32(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchadd_u32_nb(dest,node,src,operand) \
        _gasnetX_fetchadd_u32_nb(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchadd_u32_nbi(dest,node,src,operand) \
        _gasnetX_fetchadd_u32_nbi(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchadd_u32_val(node,src,operand) \
        _gasnetX_fetchadd_u32_val(node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchadd_u32_nb_val(node,src,operand) \
        _gasnetX_fetchadd_u32_nb_val(node,src,operand GASNETE_THREAD_GET)

GASNETX_FETCHOP_DECLS(and_u32, uint32_t)
#define gasnetX_fetchand_u32(dest,node,src,operand) \
        _gasnetX_fetchand_u32(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchand_u32_nb(dest,node,src,operand) \
        _gasnetX_fetchand_u32_nb(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchand_u32_nbi(dest,node,src,operand) \
        _gasnetX_fetchand_u32_nbi(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchand_u32_val(node,src,operand) \
        _gasnetX_fetchand_u32_val(node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchand_u32_nb_val(node,src,operand) \
        _gasnetX_fetchand_u32_nb_val(node,src,operand GASNETE_THREAD_GET)

GASNETX_FETCHOP_DECLS(or_u32, uint32_t)
#define gasnetX_fetchor_u32(dest,node,src,operand) \
        _gasnetX_fetchor_u32(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchor_u32_nb(dest,node,src,operand) \
        _gasnetX_fetchor_u32_nb(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchor_u32_nbi(dest,node,src,operand) \
        _gasnetX_fetchor_u32_nbi(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchor_u32_val(node,src,operand) \
        _gasnetX_fetchor_u32_val(node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchor_u32_nb_val(node,src,operand) \
        _gasnetX_fetchor_u32_nb_val(node,src,operand GASNETE_THREAD_GET)

GASNETX_FETCHOP_DECLS(xor_u32, uint32_t)
#define gasnetX_fetchxor_u32(dest,node,src,operand) \
        _gasnetX_fetchxor_u32(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchxor_u32_nb(dest,node,src,operand) \
        _gasnetX_fetchxor_u32_nb(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchxor_u32_nbi(dest,node,src,operand) \
        _gasnetX_fetchxor_u32_nbi(dest,node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchxor_u32_val(node,src,operand) \
        _gasnetX_fetchxor_u32_val(node,src,operand GASNETE_THREAD_GET)
#define gasnetX_fetchxor_u32_nb_val(node,src,operand) \
        _gasnetX_fetchxor_u32_nb_val(node,src,operand GASNETE_THREAD_GET)

extern uint32_t gasnetX_read_u32_val(gasnet_node_t node, void *src);
extern uint64_t gasnetX_read_u64_val(gasnet_node_t node, void *src);
extern void gasnetX_set_u32_val(gasnet_node_t node, void *dest, uint64_t value);
extern void gasnetX_set_u64_val(gasnet_node_t node, void *dest, uint64_t value);

extern uint64_t _gasnetX_cswap_u64_val(
            gasnet_node_t node, void *src,
            uint64_t oldval, uint64_t newval GASNETE_THREAD_FARG);
#define gasnetX_cswap_u64_val(node,src,oldval,newval) \
        _gasnetX_cswap_u64_val(node,src,oldval,newval GASNETE_THREAD_GET)

extern uint32_t _gasnetX_cswap_u32_val(
            gasnet_node_t node, void *src,
            uint32_t oldval, uint32_t newval GASNETE_THREAD_FARG);
#define gasnetX_cswap_u32_val(node,src,oldval,newval) \
        _gasnetX_cswap_u32_val(node,src,oldval,newval GASNETE_THREAD_GET)

#endif

/* ------------------------------------------------------------------------------------ */
 

#endif
