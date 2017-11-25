/*
© Regents of the University of Minnesota. 
   This software is licensed under GPL version 3.0 (https://www.gnu.org/licenses/gpl-3.0.en.html). 
*/
/**
 **    File:  HFPlayerUtils.h
 **    Authors:  Sai Susarla, Weiping He, Jerry Fredin, Ibra Fall, Alireza Haghdoost
 **
 ******************************************************************************
 **
 **    Copyright 2012 NetApp, Inc.
 **
 **    Licensed under the Apache License, Version 2.0 (the "License");
 **    you may not use this file except in compliance with the License.
 **    You may obtain a copy of the License at
 **
 **    http://www.apache.org/licenses/LICENSE-2.0
 **
 **    Unless required by applicable law or agreed to in writing, software
 **    distributed under the License is distributed on an "AS IS" BASIS,
 **    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 **    See the License for the specific language governing permissions and
 **    limitations under the License.
 **
 ******************************************************************************
 **/


#ifndef __IOLogUtils_H__
#define __IOLogUtils_H__
//#define DEBUG
#include <math.h>
#include <fstream>
#include "HFParsed.h"
#include "TextDataSet.h"
//#include <libaio.h>
#include <sys/types.h>
//#include <sys/stat.h>
//#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <aio.h>
#include <unistd.h>

#ifdef __GNUC__
#define likely(x)   __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif

const unsigned MAX_THREAD = 32; /* max number of worker threads */
extern unsigned WT; //number of worker threads

/* Returns the speed of the system's CPU in GHz, as reported by
   /proc/cpuinfo. On a multiprocessor machine, returns the speed of
   the first CPU. On error returns zero.  */
inline double get_cpu_speed()
{
    FILE* ptr;
    char buffer[1024];
    char* match;
    char* temp;
    double clock_speed;
    /* Read the entire contents of /proc/cpuinfo into the buffer.  */
    ptr = fopen("cfg.txt", "r");
    fread(buffer, 1, sizeof(buffer), ptr);
    fclose(ptr);
    /* Locate the line that contains the frequency  */
    match = strstr(buffer, "Processor Clock Speed");

    if(match == NULL)
        return 0;

    temp = strtok(match, ":");
    temp = strtok(NULL, "\n");

    //ARH bug fix. add default value (1GHz) for processor clock speed.
    if(temp)
    {
        sscanf(temp, "%lf", &clock_speed);
        printf("CPU clock speed: %.3lf GHz\n", clock_speed/1000);
    }
    else
        clock_speed = 1 ;

    return clock_speed/1000;
}
/** Setup the interrupt affinity **/
inline int set_interrupt_affinity()
{
    FILE* procint;
    FILE* chgFile;
    char tmpline[1024];
    char* match;
    char affinityFile[80];
    int intNumber;
    int qlaint;
    int megasasint;
    int mptsasint;
    int qlafound = 0;
    int megasasfound = 0;
    int mptsasfound = 0;
    int int_core = 16;
    /* Read the entire contents of /proc/interrupts into the buffer.  */
    procint = fopen("/proc/interrupts", "r");

    while(!feof(procint))
    {
        fgets(tmpline, sizeof(tmpline), procint);
        sscanf(tmpline, "%d:", &intNumber);   //save interrupt number in case this is the line we want

        if(strstr(tmpline, "qla") != 0)
        {
            qlafound = 1;
            qlaint = intNumber;
            continue;
        }

        if(strstr(tmpline, "megasas") != 0)
        {
            megasasfound = 1;
            megasasint = intNumber;
            continue;
        }

        if(strstr(tmpline, "mpt2sas") != 0)
        {
            mptsasfound = 1;
            mptsasint = intNumber;
            continue;
        }
    }

    if (qlafound || megasasfound || mptsasfound)
    {
        printf ("IRQ affinity set to Core 0 for:\n\t");
    }

    if (qlafound)
    {
        printf ("qla(%d)  ", qlaint);
        sprintf(affinityFile, "/proc/irq/%d/smp_affinity", qlaint);
        chgFile = fopen(affinityFile, "r+");
        fprintf(chgFile, "1");
        fclose(chgFile);
        qlaint++;
        sprintf(affinityFile, "/proc/irq/%d/smp_affinity", qlaint);
        chgFile = fopen(affinityFile, "r+");
        fprintf(chgFile, "1");
        fclose(chgFile);
    }

    if (megasasfound)
    {
        printf ("megasas(%d)  ", megasasint);
        sprintf(affinityFile, "/proc/irq/%d/smp_affinity", megasasint);
        chgFile = fopen(affinityFile, "r+");
        fprintf(chgFile, "1");
        fclose(chgFile);
    }

    if (mptsasfound)
    {
        printf ("mpt2sas(%d)  ", mptsasint);
        sprintf(affinityFile, "/proc/irq/%d/smp_affinity", mptsasint);
        chgFile = fopen(affinityFile, "r+");
        fprintf(chgFile, "1");
        fclose(chgFile);
    }

    printf("\n");
    fclose(procint);
    return(0);
}

/** Return the number of physical cores **/
inline int get_number_cores()
{
    FILE* ptr;
    char buffer[1024];
    char* match;
    int cores_per_socket;
    int numSockets;
    int total_phys_cores = 0;
    int total_logical_cores = 0;
	//execl("/usr/sbin/prtconf", "prtconf > cfg.txt");
    /* Read the entire contents of /proc/cpuinfo into the buffer.  */
	
    ptr = fopen("cfg.txt", "r"); // /usr/lbz/hf/bin/
    fread(buffer, 1, sizeof(buffer), ptr);
    fclose(ptr);
    match = strstr(buffer, "Number Of Processors");

    if(match == NULL)
        return 0;

    sscanf(match, "Number Of Processors: %d", &cores_per_socket);
    total_phys_cores = sysconf(_SC_NPROCESSORS_ONLN);  
    total_logical_cores = sysconf(_SC_NPROCESSORS_CONF);  
    numSockets = total_phys_cores / cores_per_socket ;
	
	
    //printf("Cores per CPU: %d\n", cores_per_socket);
    //printf("Number of CPUs: %d\n", numSockets);
    //printf("Total physical cores available : %d\n", total_phys_cores);
	
	
    return (total_phys_cores);
}
/** Check if hyperthreading is enabled.  We want it off for replay **/

inline int check_hyperthreading()
{
    FILE* ptr;
    char buffer[1024];
    char* match;
    int sibling;
    int cores_per_socket;
    ptr = fopen("cfg.txt", "r"); //  /usr/lbz/hf/bin/
    fread(buffer, 1, sizeof(buffer), ptr);
    fclose(ptr);
    match = strstr(buffer, "Number Of Processors");

    if(match == NULL)
        return 0;

    sscanf(match, "Number Of Processors: %d", &cores_per_socket);
    /*match = strstr (buffer, "siblings");
    sscanf(match, " siblings     : %d", &sibling);
	
    if (cores_per_socket != sibling)
    {
        printf("Error:  Hyperthreading is enabled, exiting\n");
        return(1);
    }
	*/
    return(0);
}

/** Return the number of processor sockets (physical CPUs) **/
inline int get_number_sockets()
{
    FILE* ptr;
    char buffer[1024];
    char* match;
    int cores_per_socket;
    int numSockets;
    int total_phys_cores = 0;
    /* Read the entire contents of /proc/cpuinfo into the buffer.  */
    ptr = fopen("cfg.txt", "r"); ///usr/lbz/hf/bin/
    fread(buffer, 1, sizeof(buffer), ptr);
    fclose(ptr);
    /* Locate the line that contains the information needed  */
    match = strstr(buffer, "Number Of Processors");

    if(match == NULL)
        return 0;

    sscanf(match, "Number Of Processors: %d", &cores_per_socket);
    total_phys_cores = sysconf(_SC_NPROCESSORS_ONLN);    /** Online Cores **/
    numSockets = total_phys_cores / cores_per_socket;
    return (numSockets);
}

class Bundle
{
public:
    Bundle(unsigned short bundleSize)
    {
        allocateIocbBunch(bundleSize);
    }
    Bundle()
    {
        iocb_bunch = NULL;
        size_ios = 0;
        startTimeNano = 0;
        startLoad = 0;
    }
    Bundle(const Bundle& cp) : size_ios(cp.size_ios)
    {
        iocb_bunch = NULL;
    }
    ~Bundle()
    {
        delete[] iocb_bunch;
    }
    void allocateIocbBunch(unsigned short bundleSize)
    {
        size_ios = bundleSize;
        iocb_bunch = new struct aiocb64* [bundleSize];

        if(!iocb_bunch)  /* make sure allocation was successful */
        {
            fprintf(stderr, "cannot allocate %lu bytes in memory\n", size_ios * sizeof(struct aiocb64*));
            exit(1);
        }

        startTimeNano = 0;
        startLoad = 0;
    }
    unsigned short size_ios; //16-bit
    struct aiocb64 **iocb_bunch;  //64-bit
    unsigned long long startTimeNano;
    int startLoad; // Target controller load, retrieved from trace file
};
/* can be used with
 * __attribute__((aligned(CACHELINE_SIZE)));
 * for cache-lign alignmet but memory overhead is huge , about 75% (96-bit used out of CACHELINE_SIZE*8=512-bit) */
struct Param
{
    //io_context_t* ctx;
    Bundle* ioq;
    Bundle* warmup_ioq;
    int id;
	std::vector<Bundle*> ioqvector;
    Param()
    {
       // ctx = NULL;
        ioq = NULL;
        id = 0;
    }
};

static inline ofstream& myopen(const char* fname)
{
    ofstream* of = new ofstream(fname);
    fprintf(stderr, "creating %s\n", fname);

    if (! of->is_open())
    {
        fprintf(stderr, "cannot open %s to write\n", fname);
        exit(1);
    }

    return *of;
}


struct TraceReplayConfig
{
    struct LUNCfg
    {
        LBA lba_start; /* replay IOs in range [lba_start, lba_start+nblks] */
        LBA nblks;
        LBA lba_shift; /* shift lba by lba_shift */
        double lba_scale; /* then scale the lba by lba_scale */
        double iosize_scale; /* scale iosize in #bytes */
        double start_usecs; /* replay IOs in range [start_usecs, start+num] */ //changed to double
        //ULONG64 start_usecs; /* replay IOs in range [start_usecs, start+num] */
        double num_usecs; //changed to double
        //ULONG64 num_usecs;
        double slack_scale; /* scale think time by slack_scale */
        int await_reply; /* Obey request-reply causality in the input trace */

        int fd;
        string* filename;
    };
    map<int, LUNCfg> lunCfgs;

    int import(char* cfgfile);

    int rescale(
        DRecordData* iorec,
        int& fd, /* out */
        unsigned long long& slack_scale, /* out */
        unsigned long long& offset, /* out */
        unsigned long long& size, /* out */
		//size_t& size,
        unsigned long long& iotime); /* out */

    //double frequence_speed();

};

struct TraceReplayer
{
    TraceReplayConfig* replaycfg;
protected:
    map<ULONG64, ULONG64> reply_seqids; /* TS -> SEQID */
    map<ULONG64, ULONG64> reply_ts; /* SEQID -> TS */
private:
    DRecordSchema replay_schema;
    DRecordData replayrec;
    //TextDataSet *dump; // moved to public
    ULONG64 ts_per_usec;
    ULONG64 cur_ts;
    ofstream* replayout;
public:
    TextDataSet* dump;
    /* added by hwp */
    double start_point; // start point for global timer, get from trace file
    struct timespec st; // real start point
    Bundle* allIOs[MAX_THREAD]; // array of private queues
    Bundle* allWarmupIOs[MAX_THREAD]; // array fo private queues for warmup
    //io_context_t* ctxs; // array of private aio context
    pthread_t* threads, *timethread, *harvestthread, *debugthread, *ioprepthread;
    Param* params;

public:
    TraceReplayer(
        TextDataSet* indump,
        TraceReplayConfig* cfg)
        : dump(indump), replaycfg(cfg)
    {
        ts_per_usec = 1;
        replay_schema.add("is_req");
        replay_schema.add("ts");
        replay_schema.add("elapsed_usecs", DRecordSchema::MyDouble);
        replay_schema.add("seqid");
        replay_schema.add("think_usecs");
        replay_schema.add("op");
        replay_schema.add(LUN_FIELD);
        replay_schema.add("offset");
        replay_schema.add("nbytes");
        replayrec.set_schema(&replay_schema);
        start_point = 0;
        //memset(&ctx, 0, sizeof(ctx)); // recommended
        //ctxs = new io_context_t[WT];
        params = new Param[WT];

        for(unsigned i = 0; i < MAX_THREAD; i++)
        {
            allIOs[i] = NULL;
        }

        for(unsigned i = 0; i < WT; i++)
        {
           // memset(ctxs + i, 0, sizeof(*ctxs));
           // params[i].ctx = ctxs + i;
            params[i].id = i;
            /* params[i].ioq will be assigned later in the prepareIO */
        }
    }
};
extern int
do_ioreplay(
    TextDataSet* indump,
    TraceReplayConfig* cfg);

#endif /* __IOLogUtils_H__ */
