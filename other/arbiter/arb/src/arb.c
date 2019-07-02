/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 * See COPYRIGHT in top-level directory.
 */

#include <pthread.h>
#include <string.h>
#include <arb.h>

#define ARBI_MAX_PROGRESS_HOOKS  (16)

struct {
    arb_info_s info;
    int valid;
} arbi_info_list[ARBI_MAX_PROGRESS_HOOKS] = { 0 };

int arbi_numclients = 0;
int arbi_info_idx = 0;
static pthread_mutex_t arbi_mutex = PTHREAD_MUTEX_INITIALIZER;

int arb_register(arb_info_s *info, int *handle)
{
    pthread_mutex_lock(&arbi_mutex);
    memcpy(&arbi_info_list[arbi_info_idx].info, info, sizeof(arb_info_s));
    arbi_info_list[arbi_info_idx].info.myname = strdup(info->myname);
    arbi_info_list[arbi_info_idx].valid = 1;
    *handle = arbi_info_idx++;
    pthread_mutex_unlock(&arbi_mutex);

    return ARB_SUCCESS;
}

int arb_deregister(int handle)
{
    arbi_info_list[handle].valid = 0;

    return ARB_SUCCESS;
}

int arb_progress(int handle)
{
    for (int i = 0; i < arbi_info_idx; i++) {
        if ((i != handle) && arbi_info_list[i].valid)
            arbi_info_list[i].info.progress_fn();
    }

    return ARB_SUCCESS;
}
