/* Copyright (C) 2007-2015 Open Information Security Foundation
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


/**
 * \file
 *
 * \author Mat Oldham <mat.oldham@gmail.com>
 *
 * Output PCAP files when an alert fires
 */

#include "suricata-common.h"
#include "debug.h"
#include "flow.h"
#include "conf.h"

#include "threads.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "util-byte.h"
#include "util-path.h"
#include "util-print.h"
#include "util-proto-name.h"
#include "util-time.h"

#include "output.h"

#include "timemachine.h"

#define MODULE_NAME                     "AlertPcap"
#define DEFAULT_DIRECTORY_NAME          "alert"
#define DEFAULT_FILE_TIMEOUT            300

/**
 * Represents an individual PCAP file 
 **/
typedef struct AlertPcapFile_ {
    char                                *filename;
    pcap_t                              *pcap_file;
    pcap_dumper_t                       *pcap_writer;
    FILE                                *output_file;

    struct timeval                      updated;    
    TAILQ_ENTRY(AlertPcapFile_)         next;
} AlertPcapFile;

/**
 * Represents global data shared across all threads that
 * can output AlertPcap logs 
 **/
typedef struct AlertPcapLogData_ {
    char                                directory[PATH_MAX];
    uint32_t                            timeout;

    AlertPcapFile                       *current_file;
    uint32_t                            pcap_file_count;
    TAILQ_HEAD(, AlertPcapFile_)        pcap_files;    
    SCMutex                             apl_lock;      
} AlertPcapLogData;

/** 
 * AlertPcapThread specific variables 
 **/
typedef struct AlertPcapLogThreadData_ {
    AlertPcapLogData                    *apl_data;
} AlertPcapLogThreadData;

/**
 * Global storage for alert pcap log data 
 **/
static AlertPcapLogData *g_apl_data = NULL;

static TmEcode AlertPcapLogThreadInit(ThreadVars *, void *, void **);
static TmEcode AlertPcapLogThreadDeinit(ThreadVars *, void *);
static OutputCtx *AlertPcapLogInitCtx(ConfNode *);
static void AlertPcapLogDeInitCtx(OutputCtx *);

int AlertPcapLogCondition(ThreadVars*, const Packet*);
int AlertPcapLogProcess(ThreadVars *, void *, const Packet *);


void TmModuleAlertPcapLogRegister(void) 
{
    tmm_modules[TMM_ALERTPCAP].name = MODULE_NAME;
    tmm_modules[TMM_ALERTPCAP].ThreadInit = AlertPcapLogThreadInit;
    tmm_modules[TMM_ALERTPCAP].ThreadDeinit = AlertPcapLogThreadDeinit;
    tmm_modules[TMM_ALERTPCAP].RegisterTests = NULL;

    OutputRegisterPacketModule(MODULE_NAME, "alert-pcap", AlertPcapLogInitCtx,
        AlertPcapLogProcess, AlertPcapLogCondition);

    return;
}

/**
 *  \brief The AlertPcapLog module should only be executed when the packet is 
 *         associated with a flow, the packet contains at least one alert or there 
 *         was a prior packet within the flow that also contained an alert 
 *         (regardless of whether that packet was part of timemachine or not)
 *
 *  \param tv Thread Variable containing input/output queue, cpu affinity etc.
 *  \param p The packet the conditions are checked against.
 *
 *  \retval TM_ECODE_OK on succces
 *  \retval TM_ECODE_FAILED on failure
 **/
int AlertPcapLogCondition(ThreadVars *tv, const Packet *p) {
    if (p->flow && (p->flags & PKT_FLOW_CONTAINS_ALERTS)) {
        return TRUE;
    }

    return FALSE;
}

/** \brief Generates a new pcap output file that would be associated with a given  
 *         flow 
 *
 *  \param filename The filename (minus directory) of the file to create
 *  \param directory The directory the file will be generated in (directory will be 
 *         created if it doesn't exist)
 *   \param packet A packet the output fill (needed to set appropriate datalink info)
 *
 *  \retval AlertPcapFile struct containing the new file info and file handles
 *  \retval NULL on failure
 **/
AlertPcapFile* AlertPcapFileNew(const char *filename, const char* directory, const Packet *p) {
       
    AlertPcapFile *output = SCMalloc(sizeof(AlertPcapFile));
    if (unlikely(output == NULL)) {
        SCLogError(SC_ERR_FATAL, "Error could not create AlertPcapFile output.");
        return NULL;            
    }

    output->filename = SCCalloc(1, strlen(filename) + 1);
    if (unlikely(output->filename == NULL)) {
        SCLogError(SC_ERR_FATAL, "Error could not create output file for AlertPcapFile.");
        return NULL;
    }

    snprintf(output->filename, strlen(filename) + 1, "%s", filename);

    struct stat stat_buf;
    if (stat(directory, &stat_buf) != 0) {
        int ret;
        ret = MakePath(filename, S_IRWXU|S_IXGRP|S_IRGRP);
        if (ret != 0) {
            int err = errno;
            if (err != EEXIST) {
                SCLogError(SC_ERR_LOGDIR_CONFIG,
                           "Cannot create file drop directory %s: %s",
                           directory, strerror(err));
                return NULL;
            }
        } else {
            SCLogInfo("Created alert log pcap directory %s",
                      directory);
        }
    }    

    output->pcap_file = pcap_open_dead(p->datalink, 65535);
    if (output->pcap_file == NULL) {
        SCLogError(SC_ERR_FATAL, "Error, could not create TimeMachine pcap output.");
        return NULL;
    }

    output->pcap_writer = pcap_dump_open(output->pcap_file, filename);
    if (output->pcap_writer == NULL) {
        SCLogError(SC_ERR_LOGDIR_CONFIG, 
                   "Cannot create alert pcap log output file %s: %s",
                   filename, pcap_geterr(output->pcap_file));
        return NULL;
    }    

    output->output_file=pcap_dump_file(output->pcap_writer);
    return output;
}

/** \brief Close and flush an AlertPcapFile
 *
 *  \param output The output file to close
 **/
void AlertPcapFileClose(AlertPcapFile* output) {

    if (output->pcap_writer) {
        pcap_dump_flush(output->pcap_writer);
        pcap_dump_close(output->pcap_writer);
    }

    if (output->pcap_file) {
        pcap_close(output->pcap_file);
    }
}

/** \brief Generate a new pcap for each unique flow assuming the flow has at 
 *         least one alert associated with it.  This output module will continue
 *         to log data for the rest of the stream until a timeout occurs.  If
 *         TimeMachine is enabled, all data prior to the packet containing the
 *         alert but still associated with the flow will also be output.
 *
 *  \param t Thread Variable containing input/output queue, cpu affinity etc.
 *  \param thread_data AlertPcapLog module thread data.
 *  \param p The current packet that was sent to the output module 
 *
 *  \retval TM_ECODE_OK on succces
 *  \retval TM_ECODE_FAILED on failure
 **/
int AlertPcapLogProcess(ThreadVars *t, void *thread_data, const Packet *p) {
    char proto[16] = "", timebuf[64] = "";
    char srcip[46] = "", dstip[46] = "", directory[PATH_MAX], filename[PATH_MAX];

    AlertPcapLogThreadData *td = (AlertPcapLogThreadData *)thread_data;

    FLOWLOCK_WRLOCK(p->flow);
    CreateIsoTimeString(&p->flow->startts, timebuf, sizeof(timebuf));

    if (FLOW_IS_IPV4(p->flow)) {
        PrintInet(AF_INET, (const void*)&p->flow->src.addr_data32[0], srcip, sizeof(srcip));
        PrintInet(AF_INET, (const void*)&p->flow->dst.addr_data32[0], dstip, sizeof(dstip));
    } else if (FLOW_IS_IPV6(p->flow)) {
        PrintInet(AF_INET6, (const void*)&p->flow->src.addr_data32, srcip, sizeof(srcip));
        PrintInet(AF_INET6, (const void*)&p->flow->dst.addr_data32, dstip, sizeof(dstip));
    }

    if (SCProtoNameValid(p->flow->proto) == TRUE) {
        strlcpy(proto, known_proto[p->flow->proto], sizeof(proto));
    } else {
        snprintf(proto, sizeof(proto), "%03" PRIu32, p->flow->proto);
    }

    snprintf(directory, sizeof(directory), "%s/%.10s/%s-%s", 
             td->apl_data->directory, timebuf, srcip, dstip);

    if (p->flow->proto == IPPROTO_ICMP) {
        snprintf(filename, sizeof(filename), "%s/%s-%s-%s.ICMP.pcap",
                 directory, srcip, dstip, timebuf);
    } else {
        snprintf(filename, sizeof(filename), "%s/%s:%hu-%s:%hu-%s.%s.pcap",
                 directory, srcip, p->flow->sp, dstip, p->flow->dp, timebuf, proto);
    }    

    SCMutexLock(&td->apl_data->apl_lock);

    AlertPcapFile* current_file = td->apl_data->current_file;

    /* see if we can find an output file */    
    if (!(current_file) || strcmp(current_file->filename, filename) != 0) {
        TAILQ_FOREACH(current_file, &td->apl_data->pcap_files, next) {
            if (strcmp(current_file->filename, filename) == 0) {
                TAILQ_REMOVE(&td->apl_data->pcap_files, current_file, next);
                TAILQ_INSERT_TAIL(&td->apl_data->pcap_files, current_file, next);
                td->apl_data->current_file = current_file;
                break;
            }
        }
    }

    /* no output file exists, so we need to create a new one */
    if (current_file == NULL) {
        current_file = AlertPcapFileNew(filename, directory, p);

        if (current_file == NULL) {
            SCMutexUnlock(&td->apl_data->apl_lock);
            FLOWLOCK_UNLOCK(p->flow);
            return TM_ECODE_FAILED;
        }

        TAILQ_INSERT_TAIL(&td->apl_data->pcap_files, current_file, next);
        td->apl_data->current_file = current_file;
        td->apl_data->pcap_file_count += 1;
    }

    SCMutexLock(&p->flow->tm_m);
    /* If the flow still has packets, we need to flush them */        
    while (p->flow->tm_pkt_cnt > 0) {
        TimeMachinePacket* packet = TAILQ_FIRST(&p->flow->tm_pkts);
        pcap_dump((u_char*)current_file->output_file, &packet->header, packet->data);
        TAILQ_REMOVE(&p->flow->tm_pkts, packet, next);
        p->flow->tm_pkt_cnt--;    
        packet->flow = NULL;
    }
    SCMutexUnlock(&p->flow->tm_m);

    FLOWLOCK_UNLOCK(p->flow);  

    /* time for this packet to be dumped */
    struct pcap_pkthdr pkthdr;
    pkthdr.ts.tv_sec = p->ts.tv_sec;
    pkthdr.ts.tv_usec = p->ts.tv_usec;
    pkthdr.caplen = GET_PKT_LEN(p);
    pkthdr.len = GET_PKT_LEN(p);

    struct timeval current_time;
    TimeGet(&current_time);

    pcap_dump((u_char*)current_file->output_file, &pkthdr, (void*)GET_PKT_DATA(p));
    fflush(current_file->output_file);
 
    memcpy(&current_file->updated, &current_time, sizeof(struct timeval));

    /* clean up any remaining output files that may be open */
    AlertPcapFile* output = TAILQ_FIRST(&td->apl_data->pcap_files);
    TAILQ_FOREACH(output, &td->apl_data->pcap_files, next) {

        if(current_time.tv_sec - output->updated.tv_sec < td->apl_data->timeout) {
            break;
        }

        TAILQ_REMOVE(&td->apl_data->pcap_files, output, next);
        AlertPcapFileClose(output);
        SCFree(output);

        td->apl_data->current_file = NULL;
        td->apl_data->pcap_file_count--;
    }

    SCMutexUnlock(&td->apl_data->apl_lock);   
    return TM_ECODE_OK;
}

static TmEcode AlertPcapLogThreadInit(ThreadVars *t, void *initdata, void **data)
{
    if (initdata == NULL) {
        SCLogDebug("Error getting context for AlertPcapLog.  \"initdata\" argument NULL");
        return TM_ECODE_FAILED;
    }

    AlertPcapLogData *apl = ((OutputCtx *)initdata)->data;

    AlertPcapLogThreadData *td = SCCalloc(1, sizeof(AlertPcapLogThreadData));
    if (unlikely(td == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Failed to allocate Memory for AlertPcapLogThreadData");
        exit(EXIT_FAILURE);
    }

    td->apl_data = apl;
    *data = (void *)td;
    return TM_ECODE_OK;
}

/**
 *  \brief Thread DeInit function.
 *
 *  \param t Thread Variable containing input/output queue, cpu affinity etc.
 *  \param thread_data AlertPcap thread data.
 *
 *  \retval TM_ECODE_OK on succces
 *  \retval TM_ECODE_FAILED on failure
 **/
static TmEcode AlertPcapLogThreadDeinit(ThreadVars *t, void *thread_data)
{
    return TM_ECODE_OK;   
}

/** \brief Fill in the alert pcap logging struct from the provided ConfNode
 *
 *  \param conf The configuration node for this output.
 *
 *  \retval output_ctx
 **/
static OutputCtx *AlertPcapLogInitCtx(ConfNode *conf) 
{
    AlertPcapLogData *apl = SCMalloc(sizeof(AlertPcapLogData));
    if (unlikely(apl == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Failed to allocate memory for AlertPcapLogData");
        exit(EXIT_FAILURE);
    }

    memset(apl, 0, sizeof(AlertPcapLogData));

    SCMutexInit(&apl->apl_lock, NULL);

    if (conf != NULL) {
        const char *s_dir = NULL;
        s_dir = ConfNodeLookupChildValue(conf, "directory");
        if (s_dir == NULL) {
            s_dir = DEFAULT_DIRECTORY_NAME;
        }

        if (PathIsAbsolute(s_dir)) {
            snprintf(apl->directory, strlen(apl->directory), "%s", s_dir);
        }
        else {
            char *log_dir = NULL;
            log_dir = ConfigGetLogDirectory();
            
            snprintf(apl->directory, sizeof(apl->directory), "%s/%s",
                log_dir, s_dir);
        }

        uint32_t file_timeout = DEFAULT_FILE_TIMEOUT;
        const char* file_timeout_s = NULL;
        file_timeout_s = ConfNodeLookupChildValue(conf, "timeout");

        if (file_timeout_s != NULL) {
            if (ByteExtractStringUint32(&file_timeout, 10, 0, 
                                        file_timeout_s) == -1) {
              SCLogError(SC_ERR_INVALID_ARGUMENT, "Failed to initialize "
                         "alert pcap, invalid timeout period: %s",
                         file_timeout_s);
              exit(EXIT_FAILURE);
            } else if (file_timeout < 1) {
                SCLogError(SC_ERR_INVALID_ARGUMENT,
                    "Failed to initialize alert-pcap output, limit less than "
                    "allowed minimum.");
                exit(EXIT_FAILURE);
            } else {
                apl->timeout = file_timeout;
            }
        }
        else {
            apl->timeout = DEFAULT_FILE_TIMEOUT;
        }
    }

    TAILQ_INIT(&apl->pcap_files);
    apl->pcap_file_count = 0;
    apl->current_file = NULL;

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Failed to allocate memory for OutputCtx.");
        exit(EXIT_FAILURE);
    }

    output_ctx->data = apl;
    output_ctx->DeInit = AlertPcapLogDeInitCtx;

    g_apl_data = apl;
    return output_ctx;
}

/** \brief Deinitialize the output context
 *
 *  \param output_ctx The output context generated from AlertPcapLogInitCtx
 **/
static void AlertPcapLogDeInitCtx(OutputCtx *output_ctx)
{
    if (output_ctx == NULL) 
        return;

    AlertPcapLogData *apl = output_ctx->data;

    while(apl->pcap_file_count > 0) {
        apl->current_file = TAILQ_FIRST(&apl->pcap_files);
        TAILQ_REMOVE(&apl->pcap_files, apl->current_file, next);       
        AlertPcapFileClose(apl->current_file);
        SCFree(apl->current_file);
        apl->pcap_file_count--;
    }

    SCFree(apl);
    SCFree(output_ctx);
}
