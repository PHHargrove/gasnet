/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 * See COPYRIGHT in top-level directory.
 */

#ifndef ARB_H_INCLUDED
#define ARB_H_INCLUDED

enum {
    ARB_SUCCESS = 0,
    ARB_ERR_OTHER,
};

typedef struct {
    int requested_version;
    int provided_version;
    void (*progress_fn)(void);
    void (*registration_notification)(void);
    const char *myname;
} arb_info_s;

int arb_register(arb_info_s *info, int *handle);
int arb_deregister(int handle);
int arb_progress(int handle);

extern int arbi_numclients;
static inline int arb_numclients(void)
{
    return arbi_numclients;
}

#endif /* ARB_H_INCLUDED */
