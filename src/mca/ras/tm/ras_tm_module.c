/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#include <unistd.h>
#include <string.h>

#include "src/util/show_help.h"
#include "src/util/os_path.h"
#include "src/util/net.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/mca/ras/base/ras_private.h"
#include "ras_tm.h"


/*
 * Local functions
 */
static int allocate(prte_job_t *jdata, prte_list_t *nodes);
static int finalize(void);

static int discover(prte_list_t* nodelist, char *pbs_jobid);
static char *tm_getline(FILE *fp);

#define TM_FILE_MAX_LINE_LENGTH 512

static char *filename;

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_tm_module = {
    NULL,
    allocate,
    NULL,
    finalize
};


/**
 * Discover available (pre-allocated) nodes and report
 * them back to the caller.
 *
 */
static int allocate(prte_job_t *jdata, prte_list_t *nodes)
{
    int ret;
    char *pbs_jobid;

    /* get our PBS jobid from the environment */
    if (NULL == (pbs_jobid = getenv("PBS_JOBID"))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    /* save that value in the global job ident string for
     * later use in any error reporting
     */
    prte_job_ident = strdup(pbs_jobid);

    if (PRTE_SUCCESS != (ret = discover(nodes, pbs_jobid))) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }

    /* in the TM world, if we didn't find anything, then this
     * is an unrecoverable error - report it
     */
    if (prte_list_is_empty(nodes)) {
        prte_show_help("help-ras-tm.txt", "no-nodes-found", true, filename);
        return PRTE_ERR_NOT_FOUND;
    }

    /* All done */
    return PRTE_SUCCESS;
}

/*
 * There's really nothing to do here
 */
static int finalize(void)
{
    PRTE_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                         "%s ras:tm:finalize: success (nothing to do)",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    return PRTE_SUCCESS;
}


/**
 * Discover the available resources.  Obtain directly from TM (and
 * therefore have no need to validate) -- ignore hostfile or any other
 * user-specified parameters.
 *
 *  - validate any nodes specified via hostfile/commandline
 *  - check for additional nodes that have already been allocated
 */

static int discover(prte_list_t* nodelist, char *pbs_jobid)
{
    int32_t nodeid;
    prte_node_t *node;
    prte_list_item_t* item;
    FILE *fp;
    char *hostname, *cppn;
    int ppn;
    char *ptr;

    /* Ignore anything that the user already specified -- we're
       getting nodes only from TM. */

    /* TM "nodes" may actually correspond to PBS "VCPUs", which means
       there may be multiple "TM nodes" that correspond to the same
       physical node.  This doesn't really affect what we're doing
       here (we actually ignore the fact that they're duplicates --
       slightly inefficient, but no big deal); just mentioned for
       completeness... */

    /* if we are in SMP mode, then read the environment to get the
     * number of cpus for each node read in the file
     */
    if (prte_ras_tm_component.smp_mode) {
        if (NULL == (cppn = getenv("PBS_PPN"))) {
            prte_show_help("help-ras-tm.txt", "smp-error", true);
            return PRTE_ERR_NOT_FOUND;
        }
        ppn = strtol(cppn, NULL, 10);
    } else {
        ppn = 1;
    }

    /* setup the full path to the PBS file */
    filename = prte_os_path(false, prte_ras_tm_component.nodefile_dir,
                            pbs_jobid, NULL);
    fp = fopen(filename, "r");
    if (NULL == fp) {
        PRTE_ERROR_LOG(PRTE_ERR_FILE_OPEN_FAILURE);
        free(filename);
        return PRTE_ERR_FILE_OPEN_FAILURE;
    }

    /* Iterate through all the nodes and make an entry for each.  TM
       node ID's will never be duplicated, but they may end up
       resolving to the same hostname (i.e., vcpu's on a single
       host). */

    nodeid=0;
    while (NULL != (hostname = tm_getline(fp))) {
        if( !prte_keep_fqdn_hostnames && !prte_net_isaddr(hostname) ) {
            if (NULL != (ptr = strchr(hostname, '.'))) {
                *ptr = '\0';
            }
        }

        PRTE_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                             "%s ras:tm:allocate:discover: got hostname %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hostname));

        /* Remember that TM may list the same node more than once.  So
           we have to check for duplicates. */

        for (item = prte_list_get_first(nodelist);
             prte_list_get_end(nodelist) != item;
             item = prte_list_get_next(item)) {
            node = (prte_node_t*) item;
            if (0 == strcmp(node->name, hostname)) {
                if (prte_ras_tm_component.smp_mode) {
                    /* this cannot happen in smp mode */
                    prte_show_help("help-ras-tm.txt", "smp-multi", true);
                    return PRTE_ERR_BAD_PARAM;
                }
                ++node->slots;

                PRTE_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                                     "%s ras:tm:allocate:discover: found -- bumped slots to %d",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->slots));

                break;
            }
        }

        /* Did we find it? */

        if (prte_list_get_end(nodelist) == item) {

            /* Nope -- didn't find it, so add a new item to the list */

            PRTE_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                                 "%s ras:tm:allocate:discover: not found -- added to list",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

            node = PRTE_NEW(prte_node_t);
            node->name = hostname;
            prte_set_attribute(&node->attributes, PRTE_NODE_LAUNCH_ID, PRTE_ATTR_LOCAL, &nodeid, PMIX_INT32);
            node->slots_inuse = 0;
            node->slots_max = 0;
            node->slots = ppn;
            node->state = PRTE_NODE_STATE_UP;
            prte_list_append(nodelist, &node->super);
        } else {

            /* Yes, so we need to free the hostname that came back */
            free(hostname);
        }

        /* up the nodeid */
        nodeid++;
    }
    fclose(fp);

    return PRTE_SUCCESS;
}

static char *tm_getline(FILE *fp)
{
    char *ret, *buff;
    char input[TM_FILE_MAX_LINE_LENGTH];

    ret = fgets(input, TM_FILE_MAX_LINE_LENGTH, fp);
    if (NULL != ret) {
        input[strlen(input)-1] = '\0';  /* remove newline */
        buff = strdup(input);
        return buff;
    }

    return NULL;
}

