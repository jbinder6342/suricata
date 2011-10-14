/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "suricata-common.h"
#include "tm-threads.h"
#include "conf.h"
#include "runmodes.h"
#include "runmode-pfring.h"
#include "source-pfring.h"
#include "log-httplog.h"
#include "output.h"
#include "cuda-packet-batcher.h"
#include "source-pfring.h"

#include "alert-fastlog.h"
#include "alert-prelude.h"
#include "alert-unified-log.h"
#include "alert-unified-alert.h"
#include "alert-unified2-alert.h"
#include "alert-debuglog.h"

#include "util-debug.h"
#include "util-time.h"
#include "util-cpu.h"
#include "util-affinity.h"
#include "util-runmodes.h"
#include "util-device.h"

static const char *default_mode_auto = NULL;
static const char *default_mode_autofp = NULL;


#define PFRING_CONF_V1 1
#define PFRING_CONF_V2 2

const char *RunModeIdsPfringGetDefaultMode(void)
{
#ifdef HAVE_PFRING
    return default_mode_autofp;
#else
    return NULL;
#endif
}

void RunModeIdsPfringRegister(void)
{
    default_mode_auto = "auto";
    RunModeRegisterNewRunMode(RUNMODE_PFRING, "auto",
                              "Multi threaded pfring mode",
                              RunModeIdsPfringAuto);
    default_mode_autofp = "autofp";
    RunModeRegisterNewRunMode(RUNMODE_PFRING, "autofp",
                              "Multi threaded pfring mode.  Packets from "
                              "each flow are assigned to a single detect "
                              "thread, unlike \"pfring_auto\" where packets "
                              "from the same flow can be processed by any "
                              "detect thread",
                              RunModeIdsPfringAutoFp);
    RunModeRegisterNewRunMode(RUNMODE_PFRING, "single",
                              "Single threaded pfring mode",
                              RunModeIdsPfringSingle);
    RunModeRegisterNewRunMode(RUNMODE_PFRING, "workers",
                              "Workers pfring mode, each thread does all"
                              " tasks from acquisition to logging",
                              RunModeIdsPfringWorkers);
    return;
}

void PfringDerefConfig(void *conf)
{
    PfringIfaceConfig *pfp = (PfringIfaceConfig *)conf;
    if (SC_ATOMIC_SUB(pfp->ref, 1) == 0) {
        SCFree(pfp);
    }
}

/**
 * \brief extract information from config file
 *
 * The returned structure will be freed by the thread init function.
 * This is thus necessary to or copy the structure before giving it
 * to thread or to reparse the file for each thread (and thus have
 * new structure.
 *
 * If old config system is used, then return the smae parameters
 * value for each interface.
 *
 * \return a PfringIfaceConfig corresponding to the interface name
 */
void *OldParsePfringConfig(const char *iface)
{
    char *threadsstr = NULL;
    PfringIfaceConfig *pfconf = SCMalloc(sizeof(*pfconf));
    char *tmpclusterid;
#ifdef HAVE_PFRING_CLUSTER_TYPE
    char *tmpctype = NULL;
    char * default_ctype = SCStrdup("cluster_round_robin");
#endif

    if (iface == NULL) {
        return NULL;
    }

    if (pfconf == NULL) {
        return NULL;
    }
    strlcpy(pfconf->iface, iface, sizeof(pfconf->iface));
    pfconf->threads = 1;
    pfconf->cluster_id = 1;
#ifdef HAVE_PFRING_CLUSTER_TYPE
    pfconf->ctype = (cluster_type)default_ctype;
#endif
    pfconf->DerefFunc = PfringDerefConfig;
    SC_ATOMIC_INIT(pfconf->ref);
    SC_ATOMIC_ADD(pfconf->ref, 1);

    /* Find initial node */
    if (ConfGet("pfring.threads", &threadsstr) != 1) {
        pfconf->threads = 1;
    } else {
        if (threadsstr != NULL) {
            pfconf->threads = (uint8_t)atoi(threadsstr);
        }
    }
    if (pfconf->threads == 0) {
        pfconf->threads = 1;
    }

    SC_ATOMIC_RESET(pfconf->ref);
    SC_ATOMIC_ADD(pfconf->ref, pfconf->threads);

    if (ConfGet("pfring.cluster-id", &tmpclusterid) != 1) {
        SCLogError(SC_ERR_INVALID_ARGUMENT,"Could not get cluster-id from config");
    } else {
        pfconf->cluster_id = (uint16_t)atoi(tmpclusterid);
        SCLogDebug("Going to use cluster-id %" PRId32, pfconf->cluster_id);
    }

#ifdef HAVE_PFRING_CLUSTER_TYPE
    if (ConfGet("pfring.cluster-type", &tmpctype) != 1) {
        SCLogError(SC_ERR_GET_CLUSTER_TYPE_FAILED,"Could not get cluster-type fron config");
    } else if (strcmp(tmpctype, "cluster_round_robin") == 0) {
        SCLogInfo("Using round-robin cluster mode for PF_RING (iface %s)",
                pfconf->iface);
        pfconf->ctype = (cluster_type)tmpctype;
    } else if (strcmp(tmpctype, "cluster_flow") == 0) {
        SCLogInfo("Using flow cluster mode for PF_RING (iface %s)",
                pfconf->iface);
        pfconf->ctype = (cluster_type)tmpctype;
    } else {
        SCLogError(SC_ERR_INVALID_CLUSTER_TYPE,"invalid cluster-type %s",tmpctype);
        return NULL;
    }
#endif

    return pfconf;
}

/**
 * \brief extract information from config file
 *
 * The returned structure will be freed by the thread init function.
 * This is thus necessary to or copy the structure before giving it
 * to thread or to reparse the file for each thread (and thus have
 * new structure.
 *
 * If old config system is used, then return the smae parameters
 * value for each interface.
 *
 * \return a PfringIfaceConfig corresponding to the interface name
 */
void *ParsePfringConfig(const char *iface)
{
    char *threadsstr = NULL;
    ConfNode *if_root;
    ConfNode *pf_ring_node;
    PfringIfaceConfig *pfconf = SCMalloc(sizeof(*pfconf));
    char *tmpclusterid;
#ifdef HAVE_PFRING_CLUSTER_TYPE
    char *tmpctype = NULL;
    /* TODO free me */
    char * default_ctype = SCStrdup("cluster_round_robin");
    int getctype = 0;
#endif

    if (iface == NULL) {
        return NULL;
    }

    if (pfconf == NULL) {
        return NULL;
    }
    strlcpy(pfconf->iface, iface, sizeof(pfconf->iface));
    pfconf->threads = 1;
    pfconf->cluster_id = 1;
#ifdef HAVE_PFRING_CLUSTER_TYPE
    pfconf->ctype = (cluster_type)default_ctype;
#endif
    pfconf->DerefFunc = PfringDerefConfig;
    SC_ATOMIC_INIT(pfconf->ref);
    SC_ATOMIC_ADD(pfconf->ref, 1);

    /* Find initial node */
    pf_ring_node = ConfGetNode("pfring");
    if (pf_ring_node == NULL) {
        SCLogInfo("Unable to find pfring config using default value");
        return pfconf;
    }

    if_root = ConfNodeLookupKeyValue(pf_ring_node, "interface", iface);
    if (if_root == NULL) {
        /* Switch to old mode */
        if_root = pf_ring_node;
        SCLogInfo("Unable to find pfring config for "
                  "interface %s, using default value or 1.0 "
                  "configuration system. ",
                  iface);
        return pfconf;
    }

    if (ConfGetChildValue(if_root, "threads", &threadsstr) != 1) {
        pfconf->threads = 1;
    } else {
        if (threadsstr != NULL) {
            pfconf->threads = (uint8_t)atoi(threadsstr);
        }
    }
    if (pfconf->threads == 0) {
        pfconf->threads = 1;
    }

    SC_ATOMIC_RESET(pfconf->ref);
    SC_ATOMIC_ADD(pfconf->ref, pfconf->threads);

    /* command line value has precedence */
    if (ConfGet("pfring.cluster-id", &tmpclusterid) == 1) {
        pfconf->cluster_id = (uint16_t)atoi(tmpclusterid);
        SCLogDebug("Going to use command-line provided cluster-id %" PRId32,
                   pfconf->cluster_id);
    } else {
        if (ConfGetChildValue(if_root, "cluster-id", &tmpclusterid) != 1) {
            SCLogError(SC_ERR_INVALID_ARGUMENT,
                       "Could not get cluster-id from config");
        } else {
            pfconf->cluster_id = (uint16_t)atoi(tmpclusterid);
            SCLogDebug("Going to use cluster-id %" PRId32, pfconf->cluster_id);
        }
    }

#ifdef HAVE_PFRING_CLUSTER_TYPE
    if (ConfGet("pfring.cluster-type", &tmpctype) == 1) {
        SCLogDebug("Going to use command-line provided cluster-type");
        getctype = 1;
    } else {
        if (ConfGetChildValue(if_root, "cluster-type", &tmpctype) != 1) {
            SCLogError(SC_ERR_GET_CLUSTER_TYPE_FAILED,
                       "Could not get cluster-type fron config");
        } else {
            getctype = 1;
        }
    }

    if (getctype) {
        if (strcmp(tmpctype, "cluster_round_robin") == 0) {
            SCLogInfo("Using round-robin cluster mode for PF_RING (iface %s)",
                    pfconf->iface);
            pfconf->ctype = (cluster_type)tmpctype;
        } else if (strcmp(tmpctype, "cluster_flow") == 0) {
            SCLogInfo("Using flow cluster mode for PF_RING (iface %s)",
                    pfconf->iface);
            pfconf->ctype = (cluster_type)tmpctype;
        } else {
            SCLogError(SC_ERR_INVALID_CLUSTER_TYPE,
                       "invalid cluster-type %s",
                       tmpctype);
            return NULL;
        }
    }

#endif

    return pfconf;
}

int PfringConfigGeThreadsCount(void *conf)
{
    PfringIfaceConfig *pfp = (PfringIfaceConfig *)conf;
    return pfp->threads;
}

int PfringConfLevel()
{
    char *def_dev;
    /* 1.0 config should return a string */
    if (ConfGet("pfring.interface", &def_dev) != 1) {
        return PFRING_CONF_V2;
    } else {
        return PFRING_CONF_V1;
    }
    return PFRING_CONF_V2;
}

#ifdef HAVE_PFRING
static int GetDevAndParser(char **live_dev, ConfigIfaceParserFunc *parser)
{
     ConfGet("pfring.live-interface", live_dev);

    /* determine which config type we have */
    if (PfringConfLevel() > PFRING_CONF_V1) {
        *parser = ParsePfringConfig;
    } else {
        SCLogInfo("Using 1.0 style configuration for pfring");
        *parser = OldParsePfringConfig;
        /* In v1: try to get interface name from config */
        if (*live_dev == NULL) {
            if (ConfGet("pfring.interface", live_dev) == 1) {
                SCLogInfo("Using interface %s", *live_dev);
                LiveRegisterDevice(*live_dev);
            } else {
                SCLogInfo("No interface found, problem incoming");
                *live_dev = NULL;
            }
        }
    }

    return 0;
}
#endif

/**
 * \brief RunModeIdsPfringAuto set up the following thread packet handlers:
 *        - Receive thread (from pfring)
 *        - Decode thread
 *        - Stream thread
 *        - Detect: If we have only 1 cpu, it will setup one Detect thread
 *                  If we have more than one, it will setup num_cpus - 1
 *                  starting from the second cpu available.
 *        - Respond/Reject thread
 *        - Outputs thread
 *        By default the threads will use the first cpu available
 *        except the Detection threads if we have more than one cpu.
 *
 * \param de_ctx Pointer to the Detection Engine.
 *
 * \retval 0 If all goes well. (If any problem is detected the engine will
 *           exit()).
 */
int RunModeIdsPfringAuto(DetectEngineCtx *de_ctx)
{
    SCEnter();
/* We include only if pfring is enabled */
#ifdef HAVE_PFRING
    int ret;
    char *live_dev = NULL;
    ConfigIfaceParserFunc tparser;

    RunModeInitialize();

    TimeModeSetLive();

    ret = GetDevAndParser(&live_dev, &tparser);
    if (ret != 0) {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM,
                "Unable to get parser and interface params");
        exit(EXIT_FAILURE);
    }

    ret = RunModeSetLiveCaptureAuto(de_ctx, tparser, "ReceivePfring", "DecodePfring",
                                    "RxPFR", live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Runmode start failed");
        exit(EXIT_FAILURE);
    }
#endif /* HAVE_PFRING */
    return 0;
}

int RunModeIdsPfringAutoFp(DetectEngineCtx *de_ctx)
{
    SCEnter();

/* We include only if pfring is enabled */
#ifdef HAVE_PFRING
    int ret;
    char *live_dev = NULL;
    ConfigIfaceParserFunc tparser;

    RunModeInitialize();

    TimeModeSetLive();

    ret = GetDevAndParser(&live_dev, &tparser);
    if (ret != 0) {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM,
                "Unable to get parser and interface params");
        exit(EXIT_FAILURE);
    }

    ret = RunModeSetLiveCaptureAutoFp(de_ctx,
                              tparser,
                              PfringConfigGeThreadsCount,
                              "ReceivePfring",
                              "DecodePfring", "RxPFR",
                              live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Runmode start failed");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsPfringAutoFp initialised");
#endif /* HAVE_PFRING */

    return 0;
}

int RunModeIdsPfringSingle(DetectEngineCtx *de_ctx)
{
    SCEnter();

/* We include only if pfring is enabled */
#ifdef HAVE_PFRING
    int ret;
    char *live_dev = NULL;
    ConfigIfaceParserFunc tparser;

    RunModeInitialize();

    TimeModeSetLive();

    ret = GetDevAndParser(&live_dev, &tparser);
    if (ret != 0) {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM,
                "Unable to get parser and interface params");
        exit(EXIT_FAILURE);
    }

    ret = RunModeSetLiveCaptureSingle(de_ctx,
                              tparser,
                              PfringConfigGeThreadsCount,
                              "ReceivePfring",
                              "DecodePfring", "RxPFR",
                              live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Runmode start failed");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsPfringSingle initialised");
#endif /* HAVE_PFRING */

    return 0;
}

int RunModeIdsPfringWorkers(DetectEngineCtx *de_ctx)
{
    SCEnter();

/* We include only if pfring is enabled */
#ifdef HAVE_PFRING
    int ret;
    char *live_dev = NULL;
    ConfigIfaceParserFunc tparser;

    RunModeInitialize();

    TimeModeSetLive();

    ret = GetDevAndParser(&live_dev, &tparser);
    if (ret != 0) {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM,
                "Unable to get parser and interface params");
        exit(EXIT_FAILURE);
    }

    ret = RunModeSetLiveCaptureWorkers(de_ctx,
                              tparser,
                              PfringConfigGeThreadsCount,
                              "ReceivePfring",
                              "DecodePfring", "RxPFR",
                              live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Runmode start failed");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsPfringWorkers initialised");
#endif /* HAVE_PFRING */

    return 0;
}