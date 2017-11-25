/* HFPlayer 3.0 Source Code
   © Regents of the University of Minnesota. 
   This software is licensed under GPL version 3.0 (https://www.gnu.org/licenses/gpl-3.0.en.html). 
*/
/*
 **    File:  hfcore.cc
 **    Authors:  Sai Susarla, Weiping He, Jerry Fredin, Ibra Fall,Nikhil Sharma
 **              Alireza Haghdoost
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

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <aio.h>
#include <signal.h>
#include <time.h>
#include <list>
#include <sched.h>
#include <xmmintrin.h> //used for software prefetching
#include <sys/wait.h>
#include "HFPlayerUtils.h"

#define SHARED_BUFFER_SIZE 4096 * 1024

#define errExit(msg) do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define errMsg(msg)  do { perror(msg); } while (0)
#define HE_ERROR 0x80
#define TE_ERROR 0x40

#define BADFD 0x01
#define UNKNOWN 0x02
#define OVERLOAD 0x03
#define ENDFILE 0x04
#define COMPERR 0x05
#define WARMUP 0x06

#define HE_ENDFILE (HE_ERROR | ENDFILE)
#define HE_COMPERR (HE_ERROR | COMPERR)
#define HE_UNKNOWN (HE_ERROR | UNKNOWN)
#define HE_WARMUP  (HE_ERROR | WARMUP)

#define TE_BADFD (TE_ERROR | BADFD)
#define TE_UNKNOWN (TE_ERROR | UNKNOWN)
#define TE_OVERLOAD (TE_ERROR | OVERLOAD)
#define TE_WARMUP (TE_ERROR | WARMUP)

extern char* cfgfile;
extern unsigned WT;
extern int max_inflight;
extern double max_speed;
extern int totalCores;
extern int numSockets;
extern int cores_per_socket;
extern int doCapture;
extern int stopOnError;
extern int stopOnOverload;
extern int debuglevel;
extern int shortIATnsec;
extern unsigned int warmupIO;

static volatile sig_atomic_t gotSIGQUIT = 0;
/* On delivery of SIGQUIT, we attempt to
   cancel all outstanding I/O requests.
   Processes must poll for the gotSIGQUIT variable
   to detect that the signal was received.*/

static void             /* Handler for SIGQUIT */
quitHandler(int sig)
{
    gotSIGQUIT = 1;
}

/*********************************************************************
  This function is used to read the processor tsc (time stamp counter).
  The timer core uses this to maintain overall time
  for the replay engine.
**********************************************************************/

__inline__ uint64_t rdtscp(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi) :: "ecx" );
    return (uint64_t)hi << 32 | lo;
}

struct AIORequest
{
public:
    ULONG64 seqid;
    void* buffer;
    size_t bufsize;
    unsigned long long ts;

    AIORequest(size_t iosize, unsigned long long usecs)
    {
        ts = usecs;
        buffer = 0;
        bufsize = iosize;
    }

    ~AIORequest()
    {
    }
};


int* running; // thread status
int* warmdone; // warmup done status from worker threads
long* io_complete; // completed IO count
long* io_count; //expected iocb requests per thread
long* warmup_count; //expected warmup iocb reqeusts per worker
long* bundle_count;  //expected bundles per thread
long* warmup_bundle_count; //expected warmup bundles per thread
io_context_t* context; // thread context
int* thread_err; //set if a thread detects an error
int start_threads; //"signal" all workers to go
int start_warmup = 0;
int warmup_harvest_complete = 0;
int stop_timer = 0;
int debug1, debug2, debug3, debug4; //global debug flags
unsigned long long debugarray1[8][8];
unsigned long long debugarray2[8][8];
int global_error = 0; //Only the harvest thread updates, all others check it
int timer_ready = 0;
unsigned long long core_time = 0;
unsigned long long base_time = 0;

#ifdef DEBUG_TIMING
unsigned long long* issueTime[MAX_THREAD];
unsigned long long* endTime[MAX_THREAD];
unsigned long long* depWaitTimeStart[MAX_THREAD];
#endif 

struct TraceReplayer;
/*
 * A Derivative of TraceReplayer class that does async IO
 */
struct TraceIOReplayer : public TraceReplayer
{
    const static unsigned long long minwait_usecs = 1000;

    TraceIOReplayer(
        TextDataSet* indump,
        TraceReplayConfig* cfg)
        : TraceReplayer(indump, cfg)
    {
    }
public:
    /*********************************************************************
      This is the start point for execution of the replay engine.

    **********************************************************************/
    int run()
    {
        /** Check for Incorrect Number of Cores **/
        printf("Cores required: %d, Cores available: %d\n", WT + 3, totalCores);

        if(totalCores < WT + 3)
        {
            printf(" ==============================================================================\n");
            printf(" Error!! Not enough cores available for the requested number of worker threads.\n");
            printf(" ==============================================================================\n");
            exit (EXIT_FAILURE);
        }

        replaySetup();
        /* construct IO queue(s) as needed */
        io_count = new long[WT];
        warmup_count = new long[WT];
        bundle_count = new long[WT];
        warmup_bundle_count = new long[WT];

        for(unsigned cnt = 0; cnt < WT; cnt++)
        {
            io_count[cnt] = warmup_count[cnt] = bundle_count[cnt] = warmup_bundle_count[cnt] = 0;
        }

        int prep_result = prepareIOs();

        if (prep_result < 0)
        {
            printf("No IO to execute, exiting\n");
            exit(1);
        }

        /* set start time point */
        /* this code is used for traces that don't start at time zero */
        // long quo;
        // quo = start_point / 1000000;
        // st.tv_sec = quo;
        // st.tv_nsec = (long)( (start_point - quo * 1000000) * 1000 );
        /* declare all worker threads */
        threads = new pthread_t[WT];
        timethread = new pthread_t;
        harvestthread = new pthread_t;
        debugthread = new pthread_t;
        running = new int[WT];
        warmdone = new int[WT];
        io_complete = new long[WT];
        context = new io_context_t[WT];
        thread_err = new int[WT + 2];
        int rets[WT], timeret, harvestret, debugret;

        for(unsigned i = 0; i < (WT + 2); i++)
            thread_err[i] = 0;

#ifdef DEBUG

        /* launch debug thread */
        if((debugret = pthread_create(debugthread, NULL, TraceIOReplayer::executeDebug, (void*)8)) < 0)
            errMsg("Debug thread creation failure!\n");

#endif
        /* launch harvester thread */

        if((harvestret = pthread_create(harvestthread, NULL, TraceIOReplayer::executeHarvester, (void*)8)) < 0)
            errMsg("Harvester thread creation failure!\n");

        /* launch timer thread */
        if((timeret = pthread_create(timethread, NULL, TraceIOReplayer::executeTimer, (void*)8)) < 0)
            errMsg("Time thread creation failure!\n");

        /* launch all worker threads */
        for(unsigned i = 0; i < WT; i++)
        {
            if((rets[i] = pthread_create(&threads[i], NULL, TraceIOReplayer::executeWorker, (void*)(params + i))) < 0)
                errMsg("Thread creation failure!\n");
        }

        sleep(1);
        printf("Waiting for the threads to start\n");
        // ensure all worker threads are running
        int stat = 1;

        while(stat)
        {
            for(unsigned i = 0; i < WT; i++)
            {
                if(!running[i])
                {
                    stat = 1;
                    break;
                }
                else
                {
                    stat = 0;
                }
            }
        }

        while (!timer_ready)
            ; //wait for the timer to warm up and calibrate

        printf("All threads running, starting replay.\n");

        if (warmupIO)
        {
            base_time = core_time;
            start_warmup = 1;
            printf("Waiting for the worker threads to finish warmup\n");
            int warmstat = 1;

            while(warmstat)
            {
                for(unsigned i = 0; i < WT; i++)
                {
                    if(!warmdone[i])
                    {
                        warmstat = 1;
                        break;
                    }
                    else
                    {
                        warmstat = 0;
                    }
                }
            }

            printf("Waiting for the harvester to finish warmup\n");

            while(!warmup_harvest_complete)
                ;
        }

        if (doCapture)
        {
            startCapture();
            //sleep(10);
        }

        base_time = core_time;
        start_threads = 1;

        // All there is left to do is wait for the threads to complete.

        for( unsigned i = 0; i < WT; i++)
            pthread_join(threads[i], NULL);

        printf ("Worker threads are done.\n");
        pthread_join(harvestthread[0], NULL);
        printf ("Harvester thread is done.\n");
        stop_timer = 1;  //Tell the timer we're done
        pthread_join(timethread[0], NULL);

        if (doCapture)
        {
            //sleep(10);
            stopCapture();  //May have to put in a delay - NOOO! The harvester has seen all completions
        }

        if(global_error != 0)
        {
            if(global_error == TE_BADFD)
                printf("Worker thread detected Bad File Descriptor, Error: %x\n", TE_BADFD);

            if(global_error == TE_UNKNOWN)
                printf("Worker thread detected an unknown error\n");

            if(global_error == TE_OVERLOAD)
                printf("Worker thread detected an overload condition\n");

            if(global_error == HE_ENDFILE)
                printf("Harvester thread detected an end of file error\n");

            if(global_error == HE_COMPERR)
                printf("Harvester thread detected a completion error\n");

            if(global_error == HE_UNKNOWN)
                printf("Harvester thread detected an unknown error\n");

            return global_error;
        }

        if(debuglevel)
        {
            for(unsigned i = 0; i < WT; i++)
            {
                printf("Thread %d debug arrays:\n", i);

                for(int j = 0; j < 8; j++)
                    printf ("Start time = %llu\tSubmission time = %llu\n", debugarray2[i][j], debugarray1[i][j]);
            }
        }
#ifdef DEBUG_TIMING        
		// Write down collected Debug logs for Replay Timings now
		//FILE *fp;
		//fp = fopen( "Timing-Log.csv", "w");
		//fprintf( fp , " THREAD_ID, issueTime\n" );
		printf(" THREAD_ID, issueTime\n" );
		for(unsigned i=0 ; i < WT; i++){
			for(unsigned j=0; allIOs[i][j].iocb_bunch != NULL ; j ++ ) {   
				//fprintf(fp, "%d,  %llu \n", i, 	issueTime[i][j]);
				printf("issue: %llu end: %llu runtime: %llu\n", issueTime[i][j], endTime[i][j], endTime[i][j]-issueTime[i][j]);
			}
		//fprintf(fp , " ================================================================================================\n" ); 
		}
#endif 
        return global_error;
    }

    /**********************************************************************
     **  Stop capturing a trace of the replay by executing an external
     **  script.  This first example will execute a specific script.  Later
     **  it should be updated to run a generic script.
     **********************************************************************/
    int stopCapture()
    {
        char argv1[] = "hfplayerCapturePlugin";
        char argv2[] = "-stop";
        char* argv[] = {argv1, argv2, NULL};
        int  pidstatus = 0;
        pid_t  pid, pidresult;
        pid = fork();

        if (pid == -1)
        {
            /* Error encountered during fork process */
            printf("Unable to fork process to stop trace capture. \n");
            exit(EXIT_FAILURE);
        }

        if (pid == 0)
        {
            /*  This is the child process where we will run the external program */
            execv(argv[0], argv);
            printf("Error issuing execv command\n");
            _exit(0);
        }
        else
        {
            /* This is the parent process, it needs to wait for the child */
            pidresult = waitpid(pid, &pidstatus, 0);

            if (pidresult != pid)
            {
                printf("stopCapture:  waitpid returned unexpected result\n");
            }
        }

        return(0);
    }
    /**********************************************************************
     **  Start capturing a trace of the replay by executing an external
     **  script.  This first example will execute a specific script.  Later
     **  it should be updated to run a generic script.
     **********************************************************************/
    int startCapture()
    {
        char argv1[] = "hfplayerCapturePlugin";
        char argv2[] = "-start";
        char* argv[] = {argv1, argv2, NULL};
        int  pidstatus = 0;
        pid_t  pid, pidresult;
        pid = fork();

        if (pid == -1)
        {
            /* Error encountered during fork process */
            printf("Unable to fork process to start trace capture. \n");
            exit(EXIT_FAILURE);
        }

        if (pid == 0)
        {
            /*  This is the child process where we will run the external program */
            execv(argv[0], argv);
            printf("Error issuing execv command\n");
            _exit(0);
        }
        else
        {
            /* This is the parent process, it needs to wait for the child */
            pidresult = waitpid(pid, &pidstatus, 0);

            if (pidresult != pid)
            {
                printf("startCapture:  waitpid returned unexpected result\n");
            }
        }

        return(0);
    }

    /**********************************************************************
     **  Prototype code for executing support programs external to
     **  hfplayer.  This first example will execute our replaySetup.sh
     **  support script.
     **********************************************************************/
    int replaySetup()
    {
        extern char* cfgfile;
        char argv1[] = "replaySetup.sh";
        char argv2[] = "-y";
        char argv3[] = "-s";
        char* argv[] = {argv1, argv2, argv3, cfgfile, NULL};
        int  pidstatus = 0;
        pid_t  pid, pidresult;
        pid = fork();

        if (pid == -1)
        {
            /* Error encountered during fork process */
            printf("Unable to fork process to run replaySetup.sh\n");
            exit(EXIT_FAILURE);
        }

        if (pid == 0)
        {
            /*  This is the child process where we will run the external program */
            execv(argv[0], argv);
            printf("Error issuing execv command\n");
            _exit(0);
        }
        else
        {
            /* This is the parent process, it needs to wait for the child */
            pidresult = waitpid(pid, &pidstatus, 0);

            if (pidresult != pid)
            {
                printf("replaySetup:  waitpid returned unexpected result\n");
            }
        }

        return(0);
    }

    /**********************************************************************
     **  Read the input file and create the internal IO records based on
     **  the input and configuration files.  Put the internal IO records on
     **  individulal queues for each worker thread to execute.
     **********************************************************************/
    int prepareIOs()
    {
        extern unsigned int warmupIO;
        unsigned long int totalBundles;
        unsigned long int perThreadBundles;
        unsigned long int i ; //loop counter
        unsigned long int totalWarmupBundles;
        unsigned long int perThreadWarmups;
        void* shared_buffer;

        if(dump->start() == false)
            return -1;

        /* Allocate the shared buffer for use by warmup and trace IO */

        if(posix_memalign(&shared_buffer, 4096, SHARED_BUFFER_SIZE) != 0)
        {
            errMsg("posix_memalign error \n");
            exit(1);
        }

        if(warmupIO != 0)
        {
            vector<Bundle*> allWarmupBundles;
            doWarmupBundles(allWarmupBundles, shared_buffer);
            /* Warmup bundles are ready for round robin allocation to threads */
            totalWarmupBundles = allWarmupBundles.size();
            perThreadWarmups = (totalWarmupBundles / WT) + 2;

            for(i = 0; i < WT; i++)
            {
                allWarmupIOs[i] = new  Bundle [perThreadWarmups]; // call default constructor , iocb_bunch is not allocated yet
                allWarmupIOs[i][(perThreadWarmups - 1)].iocb_bunch = NULL; /* make sure all [*][6] are null terminated, although it
                                                                      * was initialized by NULL in the default constructor */
            }

            for(i = 0 ; i < totalWarmupBundles ; i++)
            {
                /* Round robin */
                allWarmupIOs[i % WT][i / WT].allocateIocbBunch(allWarmupBundles[i]->size_ios);
                allWarmupIOs[i % WT][i / WT] = *(allWarmupBundles[i]); //[0][5] is valid
                warmup_count[i % WT] += allWarmupBundles[i]->size_ios;
                warmup_bundle_count[i % WT] ++;
                // deallocated memory used for allBundles
            }

            while(i % WT)
            {
                /* it is ok if this peace of code does not execute,
                * unallocated iocb_bunch are already nulled in default constructure */
                allWarmupIOs[i % WT][i / WT].iocb_bunch = NULL; //null terminate [1][21/4],[2][5],[3][5]
                ++i;
            }

            for(i = 0; i < WT ; i++)
            {
                params[i].warmup_ioq = allWarmupIOs[i];
            }

            /*  Need to re-init trace file pointer to the beginning */
            dump->restart();
        }

        vector<Bundle*> allBundles;
        doBundleDump(allBundles, shared_buffer);
        /* Bundles are ready for round robin job now */
        /* allocate per thread data structures to carry bundles */
        totalBundles = allBundles.size();
        perThreadBundles = (totalBundles / WT) + 2; //example 21/4 + 2 = 7

        for(i = 0; i < WT; i++)
        {
            allIOs[i] = new  Bundle [perThreadBundles]; // call default constructor , iocb_bunch is not allocated yet
            allIOs[i][(perThreadBundles - 1)].iocb_bunch = NULL; /* make sure all [*][6] are null terminated, although it
                                                                  * was initialized by NULL in the default constructor */
        }

        for(i = 0 ; i < totalBundles ; i++)
        {
            /* Round robin */
            allIOs[i % WT][i / WT].allocateIocbBunch(allBundles[i]->size_ios);
            allIOs[i % WT][i / WT] = *(allBundles[i]); //[0][5] is valid
            io_count[i % WT] += allBundles[i]->size_ios;
            bundle_count[i % WT] ++;
            // deallocated memory used for allBoundles
        }

        while(i % WT)
        {
            /* it is ok if this peace of code does not execute,
            * unallocated iocb_bunch are already nulled in default constructure */
            allIOs[i % WT][i / WT].iocb_bunch = NULL; //null terminate [1][21/4],[2][5],[3][5]
            ++i;
        }

        for(i = 0; i < WT ; i++)
        {
            params[i].ioq = allIOs[i];
#ifdef DEBUG_TIMING
			depWaitTimeStart[i] = new unsigned long long [perThreadBundles];
			issueTime[i] = new unsigned long long [perThreadBundles]; 
			endTime[i] = new unsigned long long [perThreadBundles]; 
#endif 
        }

        /*
         * we don't deallocated allBoundles[] because iocb_bunches in allBoundles[] are
         * reused in allIOs[]. allIOs[] will be deallocated by OS when process exit */
        return 0;
    }
    /*************************************************************************
    **  This is the entry function for debug thread
    **
    *************************************************************************/
#ifdef DEBUG
    static void* executeDebug(void* threadid)
    {
        while (1)
        {
            printf("Enter a value to put in debug1:  ");
            scanf ("%d", &debug1);
            printf("You entered %d.\n", debug1);
        }
    }
#endif
    /*************************************************************************
     **  This is the entry function for harvester thread
     **
     *************************************************************************/

    static void* executeHarvester(void* threadid)
    {
        long numevents = 0;
        int debug = 0;
        int unkerr = 0;
        int eoferr = 0;
        int reterr = 0;
        int warmerr = 0;
        int totalBundles = 0;
        int totalWarmupBundles = 0;
        int totalIOs = 0;
        int j = 0;
        int active_WT = 0;
#ifdef DEBUG
        debug = 1;
#endif
        unsigned long long doneflag;
        int queuelen = 2000; //TODO:  Make this a global variable or constant
        /*  Set harvest thread affinity to core 2 in all cases
         *  Core allocation:  0 for interrupts, 1 for timer,2 for harvester
         *  See worker thread for description of worker allocation
         */
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(2, &cpuset);

        if(pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0)
        {
            errMsg("Failed to set Thread Affinity\n");
        }

        struct io_event* eventlist = new io_event[queuelen];
		struct io_event* eventlist1 = new io_event[50000];

        for (unsigned cnt = 0; cnt < WT; cnt++)
            io_complete[cnt] = 0;

        printf("Harvester thread running on Core %d\n", sched_getcpu());

        if (warmupIO)
        {
            warmup_harvest_complete = 0;

            while(!start_warmup)
                ;

            for(unsigned i = 0; i < WT; i++)
            {
                while(warmup_count[i] == 0)
                    ;

                printf("Warmup IO count for worker %d is %ld\n", i, warmup_count[i]);
            }

            while (1)
            {
                doneflag = 0;

                for(unsigned i = 0; i < WT; i++)
                {
                    numevents = io_getevents(context[i], 0, queuelen, eventlist, NULL);

                    if(numevents > 0)
                    {
                        for(int j = 0; j < numevents; j++)
                        {
                            struct io_event event = eventlist[j];
                            AIORequest* req = static_cast<AIORequest*>(event.data);

                            if(event.res != req->bufsize)
                            {
                                printf("Harvester detected error during warmup\n");
                                warmerr++;
                            }
                        }

                        io_complete[i] = io_complete[i] + numevents;
                        numevents = 0;
                    }

                    if (io_complete[i] == warmup_count[i])
                        doneflag++;

                    if (thread_err[i] || warmerr)
                    {
                        global_error = TE_WARMUP;
                    }

                    if (warmerr)
                    {
                        global_error = HE_WARMUP;
                    }
                }

                if((doneflag == WT) || (global_error))
                    break;
            }

            warmup_harvest_complete = 1;
        }  //end if warmupIO

        for (unsigned cnt = 0; cnt < WT; cnt++)
            io_complete[cnt] = 0;

        while(!start_threads)
            ;

        for(unsigned i = 0; i < WT; i++)
        {
            if(io_count[i] != 0)
                active_WT++;

            printf("IO count for worker %d is %ld\n", i, io_count[i]);
        }

        printf("There are %d Active Worker Threads.\n", active_WT);
		
		unsigned tot_numberevents=0;
        while (1)
        {	
			numevents = io_getevents(context[0], 0, queuelen, eventlist, NULL);
			if(numevents > 0)
                {
					endTime[0][tot_numberevents]=core_time - base_time;
					//printf("------------------------numevents %d %d %d \n", numevents, tot_numberevents, io_count[0]);
					eventlist1[tot_numberevents]=eventlist[0];
					tot_numberevents=tot_numberevents+numevents;
					numevents=0;
				}
			 if (tot_numberevents == io_count[0]) break;
		}
		
		
            if(debug1)
            {
                for (unsigned i = 0; i < WT; i++)
                {
                    printf("Thread %d has completed %ld bundles out of %ld.\n", i, io_complete[i], io_count[i]);
                }
            }

            doneflag = 0;

            for(unsigned i = 0; i < active_WT; i++)
            {
                    for(int j = 0; j < numevents; j++)
                    {
                        struct io_event event = eventlist1[j];
                        AIORequest* req = static_cast<AIORequest*>(event.data);

                        if(event.res != req->bufsize)
                        {
                            if(event.res == 0)
                            {
                                eoferr++;
                            }

                            if(event.res < 0)
                            {
                                reterr++;
                                printf("Error returned on aio completion:  %lx\n", event.res);
                            }

                            if(event.res > 0)
                            {
                                unkerr++;
                                printf("Unknown error returned on aio completion:  %lx\n", event.res);
                            }
                        }
                    }

                

               

                if (thread_err[i] || eoferr || reterr || unkerr)
                {
                    if ((thread_err[i] == TE_OVERLOAD) && (stopOnOverload))
                        global_error = thread_err[i];

                    if (eoferr && stopOnError)
                        global_error = HE_ENDFILE;

                    if (reterr && stopOnError)
                        global_error = HE_COMPERR;

                    if (unkerr && stopOnError)
                        global_error = HE_UNKNOWN;
                }
            
			}
				

        for (j = 0; j < WT ; j++)
        {
            totalBundles = totalBundles + bundle_count[j];
            totalIOs = totalIOs + tot_numberevents;
        }

        printf("Harvester completed with %d errors\n", (eoferr + reterr + unkerr));
        printf("EOF errors:  %d\n", eoferr);
        printf("Defined errors (code negative):  %d\n", reterr);
        printf("Unknown errors (code positive): %d\n", unkerr);
        printf("Completed %d IO Bundles in %Lf seconds, %Lf Bundle/S\n", totalBundles, ((long double)(core_time - base_time) / (long double) 1000000000), (long double) totalBundles / ((long double)(core_time - base_time) / (long double)1000000000) );
        printf("Completed %d IO in %Lf seconds, %LF IOPS\n", totalIOs, ((long double)(core_time - base_time) / (long double)1000000000), (long double)totalIOs / ((long double)(core_time - base_time) / (long double)1000000000 ) );
        return 0;
    }
    /*************************************************************************
     **  This is the entry function for timer thread
     **
     *************************************************************************/

    static void* executeTimer(void* threadid)
    {
        int delayy;
        unsigned long long cycle_count;
        unsigned long long warm_start;
        /*  Set timer thread affinity to core 1 in all cases
         *  Core allocation:  0 for interrupts, 1 for timer, 2 for harvester
         *  See worker thread for description of worker allocation
         */
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(1, &cpuset);

        if(pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0)
        {
            errMsg("Failed to set Thread Affinity\n");
        }

        printf("Timer thread running on Core %d. CPU speed is %.2lf GHz.\n", sched_getcpu(), max_speed);
        warm_start = rdtscp();
        cycle_count = warm_start;

        while ((cycle_count - warm_start) / (1000 * max_speed) < 10000000)
        {
            cycle_count = rdtscp();
        }

        printf("Timer warmup complete\n");
        cycle_count = rdtscp();  //Make sure core_time is initialized before timer_ready is set
        core_time = cycle_count / max_speed; //NANO change
        timer_ready = 1;

        while(1)
        {
            cycle_count = rdtscp();
            core_time = cycle_count / max_speed; //NANO change

            if (stop_timer)
                break;
        }
    }

    /*************************************************************************
     **  This is the entry function for worker threads
     **
     *************************************************************************/

    static void* executeWorker(void* param_p)
    {

		
        Param* param = (Param*)param_p;
        io_context_t ctx = *(param->ctx);
        Bundle* ioq = param->ioq;
        Bundle* warmup_ioq = param->warmup_ioq;
        long tid = param->id;
        int submit_err;
        int first_worker;
        /* Assign the worker threads to cores.  Try to keep equal distance from
         * the timer and harvester to the workers if possible.
         */
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        /* If only one socket, put all workers on it.  We verified there were
         * enough cores before we started. Just assign cores by tid after
         * skipping the interrupt, timer and harvester cores (3).  Also put
         * everything on one socket if WT+3 will fit.  This keeps all threads close.
         */
        if ((numSockets == 1) || ((WT + 3) <= cores_per_socket))
        {
            first_worker = 3;
        }
        else
            /* We have more than one socket, but if all workers don't fit on one core,
             * there is no way to keep equal distance, so just assign the workers by tid
             * after skipping the first 3.
             */
        {
            if(WT > cores_per_socket)
            {
                first_worker = 3;
            }
            else
                /* We have more than one socket, and there are enough cores in one socket to
                 * put all the workers there.  So assign workers to cores starting at the
                 * offset of the first core on the second socket (cores_per_socket).
                 */
            {
                first_worker = cores_per_socket;
            }
        }

        CPU_SET(tid + first_worker, &cpuset);

        if(pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0)
        {
            errMsg("Failed to set Thread Affinity\n");
        }

        printf("Worker thread %ld running on Core %d\n", tid, sched_getcpu());
        long counter = 0;
        int qmax = max_inflight / WT;
        int qlen = qmax + 8;
        unsigned long long wtime;  //Used to carry integer iotime in nanoseconds

        /* create async io context */
        if(io_setup(qlen, &ctx) != 0)
            errMsg("Context setup failed \n");

        /* "sync point" */
        running[tid] = 1;
        context[tid] = ctx;

        if(ioq == NULL)
            errMsg("Trace file is empty or no request for requested LUN in trace file. \n");

        if (warmupIO)
        {
            while(!start_warmup)
                ;

            /* submit warmup IO as async IO requests in the private queue */

            for(int i = 0; warmup_ioq[i].iocb_bunch != NULL ; i++)
            {
                unsigned short bundleSize = warmup_ioq[i].size_ios;
                wtime = warmup_ioq[i].startTimeNano;
                /* inter-arrival time control */
                while(wtime > (core_time - base_time))
                    ;

                /* actual submission */
                if((submit_err = io_submit(ctx, bundleSize, warmup_ioq[i].iocb_bunch)) < 0)
                {
                    printf("Failed during warmup with counter = %ld on core %ld\n", counter, tid);
                    printf("Returned value was: %d\n", submit_err);
                    thread_err[tid] = TE_WARMUP;
                    continue;
                }

                /* submit Bundle succesfuly , check for partial failure */
                if(submit_err != bundleSize)
                {
                    fprintf(stderr, " Warmup bundle submission failed partially, bundleSize = %d\n", bundleSize);
                    thread_err[tid] = TE_WARMUP;
                    continue;
                }

                counter += submit_err;

                if (global_error != 0)
                    pthread_exit;

                if (counter - io_complete[tid] > qmax)
                {
                    thread_err[tid] = TE_OVERLOAD;

                    if (!stopOnOverload)
                    {
                        while ((counter - io_complete[tid] >= qmax) && (!global_error))
                            ;
                    }
                }
            }
        }

        counter = 0;
        warmdone[tid] = 1;

        while(!start_threads)
            ;

        /* submit async IO requests in the private queue */
        for(int i = 0; ioq[i].iocb_bunch != NULL ; i++)
        {
            unsigned short prefetchDone = 0;
            unsigned short bundleSize = ioq[i].size_ios;
            wtime = ioq[i].startTimeNano;
            //printf("In thread %ld, wtime = %llu, base_time = %llu, core_time = %llu, curr_time = %llu\n",tid,wtime, base_time, core_time,(core_time - base_time));
			 //printf("%llu\n",core_time - base_time);

#ifndef AFAP
            /* inter-arrival controller load control */

           // while( ioq[i].startLoad + bundleSize < counter  - io_complete[tid] )
            //    ;

            /* inter-arrival time control */
            while(likely(wtime > (core_time - base_time)))
            {
#ifdef PREFETCH

                if(likely(prefetchDone < bundleSize))
                {
                    //_mm_prefetch(((const void*) ioq[i].iocb_bunch[prefetchDone]) , _MM_HINT_NTA);    //Alireza: Prefetch this address of memory, we have to do (void*) pointer arithmatic
                    //++ prefetchDone;
                }

#endif
            }

            /* Trying to avoid partial bundle execution */
#ifdef PARBUNDLE

            while ((bundleSize > (qmax - counter + io_complete[tid])) && (!global_error));

#endif
#endif

#ifdef DEBUG_TIMING
				issueTime[tid][i]=core_time - base_time;
#endif 
            /* actual submission */
            if((submit_err = io_submit(ctx, bundleSize, ioq[i].iocb_bunch)) < 0)
            {
                printf("Failed with counter = %ld on core %ld\n", counter, tid);
                printf("Returned value was: %d\n", submit_err);

                if(submit_err == -11)
                {
                    fprintf(stderr, " Submit failed due to [EAGAIN]:lack resource\n");
                    continue;
                }

                if(submit_err == -9)
                {
                    fprintf(stderr, "Submit failed due to bad file descriptor.\n");
                    thread_err[tid] = TE_BADFD;
                    continue;
                }

                fprintf(stderr, "Submit failed, error = %d\n", submit_err);
                thread_err[tid] = TE_UNKNOWN;
                continue;
            }

            //printf("Return from submit: In thread %ld, wtime = %llu, base_time = %llu, core_time = %llu, curr_time = %llu\n",tid,wtime, base_time, core_time,(core_time - base_time));
            /* submit Bundle succesfuly , check for partial failure */
            if(submit_err != bundleSize)
            {
                fprintf(stderr, " Bundle submission failed partially\n");
                //FIXME: error handling
            }

            counter += submit_err;

            if (global_error != 0)
                pthread_exit;
			
            // if((counter < 8) && debuglevel)
            //{
            //   debugarray1[tid][counter] = core_time-base_time-curr_time;
            //   debugarray2[tid][counter] = curr_time;
            // }

            if (counter - io_complete[tid] > qmax)
            {
                thread_err[tid] = TE_OVERLOAD;

                if (!stopOnOverload)
                {
                    while ((counter - io_complete[tid] >= qmax) && (!global_error))
                        ;
                }
            }
        }

        printf("Worker %ld complete.\n", tid);
        pthread_exit(NULL);
    }

private:
    /*************************************************************************
     **  This is the private function that performs IOCB bundling
     **
     *************************************************************************/

    void doBundleDump(vector<Bundle*> & allBundles, void* shared_buffer)
    {
        vector<struct iocb> currentBundle;
        DRecordData* rec;
        unsigned long long now; //Current request time (nanoseconds)
        bool isFirst = true;
        unsigned op;
        int rec_count = 0;
        int startLoad = 0; //the controller target load before issue a bundle to kernel

        while((rec = dump->next()))
        {
            //printf("doBundleDump:  records processed = %d\n",rec_count++);
            struct iocb io;
            op = (*rec)["op"].i;
            int currLoad = ((*rec)["inflight_ios"].i) / WT;

            if(op < IOLogTrace::max_optypes)
            {
                int fd;
                unsigned long long offset, size;
                unsigned long long slack_scale;
                unsigned long long iotime; //io submission time after scale in nanoseconds
                int cmp = replaycfg->rescale(rec, fd, slack_scale, offset, size, iotime);

                if(cmp == 0)  // <0 means skip this IO, out of range
                {
                    now = iotime; //nano second time
                    now += 100000000; // shift all io by .1 second to allow for ramp-up phase
                    AIORequest* req = new AIORequest(size, now);
                    req->buffer = shared_buffer;

                    if(op == 1)  // write op
                    {
                        io_prep_pwrite(&io, fd, req->buffer, req->bufsize, offset);
                    }
                    else  // read op
                    {
                        io_prep_pread(&io, fd, req->buffer, req->bufsize, offset);
                    }

                    io.data =  req;
                    /* iocb is ready now, check if it fits to the previous bundle */

                    if(isFirst)
                    {
                        isFirst = false;
                        start_point = now;
                        currentBundle.clear();
                        currentBundle.push_back(io);
                        startLoad = currLoad;
                        continue;
                    }

                    if(currentBundle.size() == 0)
                    {
                        /* this is the first request in the bundle */
                        ///FIXME: from Alireza: This peace of code does not execute with current logic, clean it up
                        currentBundle.push_back(io);
                        startLoad = currLoad;
                    }
                    else
                    {
                        struct iocb lastIocb = currentBundle.back();
                        struct AIORequest* lastReqP = (AIORequest*) lastIocb.data;

                        if((now -  lastReqP->ts < shortIATnsec))
                        {
                            /* Pad to the current Bundle */
                            currentBundle.push_back(io);
                        }
                        else
                        {
                            /* this is going to start new bundle */
                            Bundle* newBundleP =  new Bundle(currentBundle.size());  //dynamic allocate new bundle with the right size
                            struct iocb firstIocb = currentBundle.front();
                            struct AIORequest* firstReqP = (AIORequest*) firstIocb.data;
                            newBundleP->startTimeNano = firstReqP->ts ;
                            newBundleP->startLoad = startLoad ;

                            for(unsigned i = 0; i < currentBundle.size() ; i++)
                            {
                                newBundleP->iocb_bunch[i] = new struct iocb;
                                *(newBundleP->iocb_bunch[i]) = currentBundle[i] ;
                            }

                            allBundles.push_back(newBundleP);
                            currentBundle.clear();
                            currentBundle.push_back(io);
                            startLoad = currLoad ;
                        } // end else
                    } // end
                }
            }
            else
            {
                errMsg("Skip invalid Operation in trace file");
            }
        } // end while()

        /* check the last bundle in the currentBundle */
        if(currentBundle.size())
        {
            Bundle* lastBundleP = new Bundle(currentBundle.size());
            struct iocb firstIocb = currentBundle.front();
            struct AIORequest firstAIOreq = *(AIORequest*)firstIocb.data;
            lastBundleP->startTimeNano = firstAIOreq.ts ;

            for(unsigned i = 0; i < currentBundle.size() ; i++)
            {
                lastBundleP->iocb_bunch[i] = new struct iocb;
                *(lastBundleP->iocb_bunch[i]) = currentBundle[i];
            }

            allBundles.push_back(lastBundleP);
            currentBundle.clear();
        }

        assert(allBundles.size());
        assert(currentBundle.size() == 0);
        printf("Done bundling and preparing %lu bundles.\n", allBundles.size());
//      return allBundles by reference
    }
//};

    /*************************************************************************
     **  This is the private function that creates warmup bundles
     **
     *************************************************************************/

    void doWarmupBundles(vector<Bundle*> & allWarmupBundles, void* shared_buffer )
    {
        vector<struct iocb> currentBundle;
        DRecordData* rec;
        unsigned long long now; //Current request time (nanoseconds)
        bool isFirst = true;
        unsigned op;
        unsigned long long warmup_startnano = 10000000; //start the first warmup IO at 10msec
        unsigned long long warmup_interval = 100000; //100usec IAT in nanoseconds
        unsigned warmReadMax;
        unsigned warmWriteMax;
        unsigned readcnt;
        unsigned writecnt;
        unsigned warmupcnt = 0;
        readcnt = 0;
        writecnt = 0;
        warmReadMax = (warmupIO / 2) * WT;
        warmWriteMax = warmReadMax;

        while((rec = dump->next()))
        {
            if ((readcnt == warmReadMax) && (writecnt == warmWriteMax))
            {
                break;    /* could scan the whole file, but we have hit our limit */
            }

            struct iocb io;

            op = (*rec)["op"].i;

            if(op >= IOLogTrace::max_optypes)
            {
                errMsg("Skip invalid Operation in trace file\n");
                continue;
            }

            int fd;
            unsigned long long offset, size;
            unsigned long long slack_scale;
            unsigned long long iotime; //io submission time after scale in nanoseconds
            int cmp = replaycfg->rescale(rec, fd, slack_scale, offset, size, iotime);

            if(cmp != 0)  // <0 means skip this IO, out of range
            {
                continue;
            }

            now = warmup_startnano + (warmupcnt * warmup_interval);
            AIORequest* req = new AIORequest(size, now);
            req->buffer = shared_buffer;

            if(op == 1)  // write op
            {
                if(writecnt == warmWriteMax)
                {
                    continue;
                }

                writecnt++;
                io_prep_pwrite(&io, fd, req->buffer, req->bufsize, offset);
            }

            if(op == 0) // read op
            {
                if(readcnt == warmReadMax)
                {
                    continue;
                }

                readcnt++;
                io_prep_pread(&io, fd, req->buffer, req->bufsize, offset);
            }

            warmupcnt++;
            io.data =  req;
            /* iocb is ready now, check if it fits to the previous bundle */

            if(isFirst)
            {
                isFirst = false;
                start_point = now;
                currentBundle.clear();
                currentBundle.push_back(io);
                continue;
            }

            if(currentBundle.size() == 0)
            {
                currentBundle.push_back(io);
            }
            else
            {
                struct iocb lastIocb = currentBundle.back();
                struct AIORequest* lastReqP = (AIORequest*) lastIocb.data;
                /* for warmup, we always start a new bundle */
                Bundle* newBundleP =  new Bundle(currentBundle.size());  //dynamic allocate new bundle with the right size
                struct iocb firstIocb = currentBundle.front();
                struct AIORequest* firstReqP = (AIORequest*) firstIocb.data;
                newBundleP->startTimeNano = firstReqP->ts ;

                for(unsigned i = 0; i < currentBundle.size() ; i++)
                {
                    newBundleP->iocb_bunch[i] = new struct iocb;
                    *(newBundleP->iocb_bunch[i]) = currentBundle[i] ;
                }

                allWarmupBundles.push_back(newBundleP);
                currentBundle.clear();
                currentBundle.push_back(io);
            } // end else
        } // end while()

        /* check the last bundle in the currentBundle */
        if(currentBundle.size())
        {
            Bundle* lastBundleP = new Bundle(currentBundle.size());
            struct iocb firstIocb = currentBundle.front();
            struct AIORequest firstAIOreq = *(AIORequest*)firstIocb.data;
            lastBundleP->startTimeNano = firstAIOreq.ts ;

            for(unsigned i = 0; i < currentBundle.size() ; i++)
            {
                lastBundleP->iocb_bunch[i] = new struct iocb;
                *(lastBundleP->iocb_bunch[i]) = currentBundle[i];
            }

            allWarmupBundles.push_back(lastBundleP);
            currentBundle.clear();
        }

        assert(allWarmupBundles.size());
        assert(currentBundle.size() == 0);
        printf("Done preparing %lu warmup bundles.\n", allWarmupBundles.size());
//      return allWarmupBundles by reference
    }
};

/***************************************************************************************
 * Here's where hfplayer calls into the core functions
 ***************************************************************************************/
int  do_ioreplay(TextDataSet* trace, TraceReplayConfig* cfg)
{
    if (cfg->lunCfgs.size() == 0)
    {
        fprintf(stderr, "No mapped LUNs available for IO replay; exiting.\n");
        return 1;
    }

    TraceIOReplayer ioreplayer(trace, cfg);
    int ret = ioreplayer.run();
    return ret;
}

