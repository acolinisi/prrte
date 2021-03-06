/*
 * Copyright (c) 2018-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <pmix.h>

static pmix_proc_t myproc;
struct timeval start,end;
static bool completed;
double sec;

static void notification_fn(size_t evhdlr_registration_id,
                            pmix_status_t status,
                            const pmix_proc_t *source,
                            pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc,
                            void *cbdata)
{
    gettimeofday(&end,NULL);
    int i;
    char name[255];
    gethostname(name, 255);
    if( (info[0].value.data.proc!=NULL) && strcmp(info[0].value.data.proc->nspace, myproc.nspace)==0)
    {
        for(i=0; i<ninfo; i++)
        {
            fprintf(stderr, "%s Client %s:%d NOTIFIED with status %d and error proc %s:%d key %s \n",
                    name, myproc.nspace, myproc.rank, status,
                    info[i].value.data.proc->nspace, info[i].value.data.proc->rank,info[i].key);
        }
        completed = true;
        if (NULL != cbfunc) {
            cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
        }
    }
    else
        fprintf(stderr, "Not from my namespace");
}

static void op_callbk(pmix_status_t status,
                      void *cbdata)
{
    fprintf(stderr, "Client %s:%d OP CALLBACK CALLED WITH STATUS %d\n", myproc.nspace, myproc.rank, status);
}

static void errhandler_reg_callbk(pmix_status_t status,
                                  size_t errhandler_ref,
                                  void *cbdata)
{
    fprintf(stderr, "Client %s:%d ERRHANDLER REGISTRATION CALLBACK CALLED WITH STATUS %d, ref=%lu\n",
               myproc.nspace, myproc.rank, status, (unsigned long)errhandler_ref);
}

int main(int argc, char **argv)
{
    int rc;
    pmix_value_t value;
    pmix_value_t *val = &value;
    pmix_proc_t proc;
    uint32_t nprocs;
    pid_t pid;

    char name[255];

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n", myproc.nspace, myproc.rank, rc);
        exit(0);
    }
    gethostname(name, 255);
    fprintf(stderr, "%s Client ns %s rank %d: Running\n", name, myproc.nspace, myproc.rank);

    PMIX_PROC_CONSTRUCT(&proc);
    (void)strncpy(proc.nspace, myproc.nspace, PMIX_MAX_NSLEN);
    proc.rank = PMIX_RANK_WILDCARD;

    /* get our universe size */
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_UNIV_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get universe size failed: %d\n", myproc.nspace, myproc.rank, rc);
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    //fprintf(stderr, "Client %s:%d universe size %d\n", myproc.nspace, myproc.rank, nprocs);
    completed = false;

    pmix_status_t status;
    status = PMIX_ERR_PROC_ABORTED;
    /* register our errhandler */
    PMIx_Register_event_handler(&status, 1, NULL, 0,
            notification_fn, errhandler_reg_callbk, NULL);

    /* call fence to sync */
    PMIX_PROC_CONSTRUCT(&proc);
    (void)strncpy(proc.nspace, myproc.nspace, PMIX_MAX_NSLEN);
    proc.rank = PMIX_RANK_WILDCARD;
    sleep(3);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %d\n", myproc.nspace, myproc.rank, rc);
        goto done;
    }

    if ( myproc.rank == 3 ) {
        fprintf(stderr, "\nClient ns %s:%d kill its host \n", myproc.nspace, myproc.rank);
        completed = true;
        pid = getppid();
    }
    gettimeofday(&start, NULL);
    if ( myproc.rank == 3 ) {
        kill(pid, 9);
    }
    while (!completed) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100000;
        nanosleep(&ts, NULL);
    }
done:
    sec = end.tv_sec + (double)end.tv_usec/1000000.0 - start.tv_sec - (double)start.tv_usec/1000000.0;
    fprintf(stderr, "Client ns %s rank %d takes %f Finalizing\n", myproc.nspace, myproc.rank, sec);
    /* finalize us */
    PMIx_Deregister_event_handler(1, op_callbk, NULL);
    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %d\n", myproc.nspace, myproc.rank, rc);
    } else {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize successfully completed\n", myproc.nspace, myproc.rank);
    }
    fflush(stderr);
    return(0);
}
