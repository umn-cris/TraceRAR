/*
© Regents of the University of Minnesota. 
   This software is licensed under GPL version 3.0 (https://www.gnu.org/licenses/gpl-3.0.en.html). 
*/
/*
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
//#include <errno.h>
#include <aio.h>
#include <time.h>
#include <list>
#include <sched.h>
//#include <xmmintrin.h> //used for software prefetching
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "HFPlayerUtils.h"
#include <signal.h>
#include <sys/processor.h>
#include <sys/errno.h>
#include <sys/thread.h>
#include <time.h>
#include <pthread.h>
//#include <iocp.h>
#include <memory>

#include <memory.h>
#include <string.h>
#include <sys/rset.h>
#define STDC_WANT_LIB_EXT1 1

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

#define SIG_AIO SIGRTMIN
//SIGRTMAX
//#define SIG_AIO 100
#define maxreq 1000

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
unsigned long long requestID;
unsigned tottest;
unsigned CountBundle;

//struct sigaction action;
//struct aiocb* ioo[1000];


static volatile sig_atomic_t gotSIGQUIT = 0;
static volatile sig_atomic_t sigflag = 1;

static void             
quitHandler(int sig)
{
    gotSIGQUIT = 1;
}

/*********************************************************************
  This function is used to read the processor tsc (time stamp counter).
  The timer core uses this to maintain overall time
  for the replay engine.
**********************************************************************/
/*
int rdtscp(void){
	clock_t time;
	//time = clock()*10;
	//return time;
	return 0;
}
*/

static __inline__ uint64_t rdtscp(void)
{
	unsigned int tbl, tbu0, tbu1;

     do {
	  __asm__ __volatile__ ("mftbu %0" : "=r"(tbu0));
	  __asm__ __volatile__ ("mftb %0" : "=r"(tbl));
	  __asm__ __volatile__ ("mftbu %0" : "=r"(tbu1));
     } while (tbu0 != tbu1);

     return (((unsigned long long)tbu1) << 32) | tbl;
  
}

/*
static __inline__ unsigned long long rdtscp(void)
{
  unsigned long long int result=0;
  unsigned long int upper, lower, tmp;
  __asm__ volatile(
                "0:                  \n"
                "\tmftbu   %0           \n"
                "\tmftb    %1           \n"
                "\tmftbu   %2           \n"
                "\tcmpw    %2,%0        \n"
                "\tbne     0b         \n"
                : "=r"(upper),"=r"(lower),"=r"(tmp)
                );
  result = upper;
  result = result<<32;
  result = result|lower;

  return(result);
}
*/

struct AIORequest
{
public:
    ULONG64 seqid;
	unsigned requestID;
    void* buffer;
    unsigned long bufsize;
	size_t sizeio;
    unsigned long long ts;
	aiocb64* ioo; //aiocb64


    AIORequest(unsigned long long iosize, unsigned long long usecs)
    {
        ts = usecs;
        buffer = 0;
        bufsize = iosize >> 32;
    }

    ~AIORequest()
    {
    }
};
struct iooq
{
//public:
	aiocb64* io;
	unsigned long long id;
	unsigned long long issueTime;
	//int pid;
	/*
	iooq(aiocb64* ioo, unsigned long long i, unsigned long long j)
	{
		io = ioo;
		id = i;
		issueTime = j;
		//pid = tid;
	}
	~iooq()
	{
	}
	*/
};

int* running; // thread status
int* warmdone; // warmup done status from worker threads
long* io_complete; // completed IO count
long* io_count; //expected aiocb requests per thread
long* warmup_count; //expected warmup aiocb reqeusts per worker
long* bundle_count;  //expected bundles per thread
long* warmup_bundle_count; //expected warmup bundles per thread
//io_context_t* context; // thread context
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
unsigned long long totnumreq;
int IOPrep_ready = 0;
int IOPrep_finish = 0;
unsigned long long totreq = 566407;//1000000; //32768 //150828  //566407 //32768
pthread_mutex_t* lock;
pthread_mutex_t* lock3;
pthread_mutex_t* lock1;
pthread_mutexattr_t mutex_shared_attr;
pthread_cond_t  SpaceAvailable, ItemAvailable;
int signalwait=0;
float delta=0;
float error_rate=0;

std::list<aiocb64*> iov;
std::list<unsigned long long> starttime;
std::vector<struct iooq *> finishreq;
//tbb::concurrent_vector<struct iooq *> finishreq;
std::vector<struct iooq *> finishreq1;
std::list<struct aiocb64 *> finishio;
std::vector<unsigned long long> issueTime;
/*
std::list<aiocb64*> iov;
std::list<unsigned long long> starttime;
std::list<struct iooq *> finishreq;
std::list<struct aiocb64 *> finishio;
std::list<unsigned long long> issueTime;
*/
const int printflag = 0;
volatile unsigned long long iter_worker=0;
volatile int signal_flag = 0;
float executiontime = 0;
unsigned long long iterissue = 0;
unsigned long long aiotimes = 0;
struct sigaction action, oaction;

#ifdef DEBUG_TIMING
//unsigned long long* issueTime[MAX_THREAD];
unsigned long long* issueTimex;
unsigned long long* wallTime;
		
//unsigned long long* wallTime[MAX_THREAD];
unsigned long long* beginTime[MAX_THREAD];
unsigned long long* reachTime[MAX_THREAD];
unsigned long long* excuteTime[MAX_THREAD];
unsigned long long* endTime[MAX_THREAD];
unsigned long* glbsize[MAX_THREAD];
unsigned long long* depWaitTimeStart[MAX_THREAD];
#endif 


static void aio_handler(int signal, siginfo_t *info, void *uap)
{
	//pthread_mutex_lock ( &lock);
	//if(printflag == 1) aiotimes++;
	//signal_flag = 0;
	//printf("singlal %d\n", signal);
	if(info != NULL) {
		if(info->si_value.sival_ptr != NULL){
			//printf("1\n");
			struct iooq* req = (struct iooq *)info->si_value.sival_ptr;
			//printf("2\n");
			if(req != NULL){
				//std::vector<unsigned long long>::iterator it = issueTime.begin();
				struct aiocb64* io = req->io;
				//struct aiocb64* io = *(it+req->id);
				//printf("3\n");
				//int pid = req->pid;
				int pid = 0;
				//printf("4\n");
				int cbNumber = req->id;
				unsigned long long issueTimex = req->issueTime;
				
				if (aio_error64(io) !=0) {
					printf("AIO error!\n");
					if (aio_error64(io) == (EINPROG | EAGAIN | EINVAL | EINTR | EIO | EINPROG | ECANCELED) ) printf("ERRNO=%d STR=%s\n", aio_error64(io), strerror(aio_error64(io)));
				}
				//printf("8\n");
				if (aio_return64(io) != io->aio_nbytes)  
					printf("lio_list did read/write %d bytes, expecting %d bytes at #%d\n", aio_return64(io), io->aio_nbytes, cbNumber);
				io_complete[pid]=io_complete[pid]+1;
				//pthread_mutex_lock ( &lock3);
				executiontime = float(core_time-base_time)/1000000000 - float(issueTimex)/1000000000 + executiontime;
				//printf("Complete Req No#%d tot#: %f issutime %f exe %f\n", cbNumber, float(core_time-base_time)/1000000000, float(issueTimex)/1000000000, executiontime);
				//pthread_mutex_unlock ( &lock3);
				//free(req);
				//pthread_mutex_lock ( &lock);
				//delete io;
				//free(io);
				//delete req;
				
				//delete info->si_value.sival_ptr;
				//pthread_mutex_unlock ( &lock);
				//printf("10\n");
				
				//printf("Complete Req No#%d\n", cbNumber);
				//assert(info->si_value.sival_ptr != NULL);	
				//free(io);
				//free(req);
				
				//finishio.push_back(io);
				//iov.erase(it+cbNumber);
				//starttime.erase(starttime.begin()+cbNumber);
				//iter_worker--;
				//if (signalwait == 1) while(signalwait);
				/*
				while(sigflag ==1) {
					finishreq.push_back(req);
					sigflag = 0;
				}
				*/
				finishreq.push_back(req);
				sigflag++;
				iter_worker++;

			}
			//if(req->io != NULL) free(req->io);
			//delete req;
			
		}
		
	}
	//signal_flag = 1;
	
	//pthread_mutex_lock ( &lock);
	//free(req);
	//req = NULL;
	//pthread_mutex_unlock ( &lock3);
	//if(printflag == 1) aiotimes--;
	//if(printflag == 1) printf("---------Leave aio_handler %llu\n", aiotimes);

}

struct TraceReplayer;
struct TraceIOReplayer;
TraceIOReplayer *oneObj=NULL;
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
		oneObj=this;
    }
	~TraceIOReplayer()
    {
		oneObj=NULL;
    }
public:
    /*********************************************************************
      This is the start point for execution of the replay engine.
 
    **********************************************************************/
    int run()
    {
        /** Check for Incorrect Number of Cores **/
		lock = (pthread_mutex_t*) calloc(1, sizeof(pthread_mutex_t));
		lock1 = (pthread_mutex_t*) calloc(1, sizeof(pthread_mutex_t));
		pthread_mutexattr_init(&mutex_shared_attr);
		pthread_mutexattr_setpshared(&mutex_shared_attr, PTHREAD_MUTEX_NORMAL);
		pthread_mutex_init( lock, NULL);
		pthread_mutex_init( lock1, NULL);
		pthread_mutex_init( lock3, NULL);
		pthread_cond_init( &ItemAvailable, NULL);
		pthread_mutexattr_destroy(&mutex_shared_attr);
		
		
		
		
		sigset_t sigset, oldMask, newMask;
		//sigemptyset(&action.sa_mask); //new add
		//sigaddset(&action.sa_mask, SIG_AIO);
		//sigemptyset(&oldMask);
		
		sigemptyset(&sigset);
		sigaddset(&sigset, SIG_AIO);

		if (pthread_sigmask(SIG_BLOCK, &sigset, &oldMask) !=0) {
				printf("BLOCK sucessfully! \n");
		}


		
		
		
		
		issueTimex = new unsigned long long[totreq];
		wallTime= new unsigned long long[totreq];
		
        printf("Cores required: %d, Cores available: %d\n", WT + 3, totalCores);
		requestID=0;
		CountBundle=0;
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
        //int prep_result = prepareIOs();
		

        /* set start time point */
        /* this code is used for traces that don't start at time zero */
        // long quo;
        // quo = start_point / 1000000;
        // st.tv_sec = quo;
        // st.tv_nsec = (long)( (start_point - quo * 1000000) * 1000 );
        /* declare all worker threads */
		//memset(&action, 0, sizeof(action));
		/*
		action.sa_sigaction = &aio_handler;	//action.sa_flags =SA_NODEFER | SA_SIGINFO; //SA_NODEFER
		action.sa_flags = SA_SIGINFO;
		sigset_t sigset, oldMask, newMask;
		//sigemptyset(&action.sa_mask); //new add
		//sigaddset(&action.sa_mask, SIG_AIO);
		sigemptyset(&oldMask);
		if (sigaction(SIG_AIO, &action, NULL) != 0){
					perror(" sigaction");
					exit(-1);
		}
		sigemptyset(&sigset);
		sigaddset(&sigset, SIG_AIO);
		*/
		//if(pthread_sigmask(SIG_BLOCK, &sigset, NULL) < 0) printf("Block signal unsuccessfully h\n");
		
		
		ioprepthread = new pthread_t;
        threads = new pthread_t[WT];
        timethread = new pthread_t;
        harvestthread = new pthread_t;
        debugthread = new pthread_t;
        running = new int[WT];
        warmdone = new int[WT];
        io_complete = new long[WT];
        //context = new io_context_t[WT];
        thread_err = new int[WT + 2];
        int rets[WT], timeret, harvestret, debugret, ioprep;
		
		for (unsigned cnt = 0; cnt < WT; cnt++) io_complete[cnt] = 0;
        for(unsigned i = 0; i < (WT + 2); i++)
            thread_err[i] = 0;
		
		void* shared_buffer;
        if(posix_memalign(&shared_buffer, 4096, SHARED_BUFFER_SIZE) != 0)
		//if(posix_memalign(&shared_buffer, 4096, SHARED_BUFFER_SIZE) != 0)	
        {
            errMsg("posix_memalign error \n");
            exit(1);
        }
		
		if((ioprep = pthread_create(ioprepthread, NULL, TraceIOReplayer::prepareIOs,  (void*)shared_buffer)) < 0) errMsg("prepareIOs thread creation failure!\n");

		//while(IOPrep_ready==0);
		

#ifdef DEBUG

        /* launch debug thread */
        if((debugret = pthread_create(debugthread, NULL, TraceIOReplayer::executeDebug, (void*)8)) < 0)
            errMsg("Debug thread creation failure!\n");

#endif
        /* launch harvester thread */

		//if((harvestret = pthread_create(harvestthread, NULL, TraceIOReplayer::executeHarvester, (void*)8)) < 0)
		//if((harvestret = thread_create(harvestthread, NULL, TraceIOReplayer::executeHarvester, (void*)8)) < 0)
			//errMsg("Harvester thread creation failure!\n");
		
		//tid_t harvestthread1 = thread_create(NULL, NULL);
		//if(harvestthread1==-1) errMsg("Harvester thread creation failure!\n");
		//kthread_start(harvestthread1, TraceIOReplayer::executeHarvester, NULL, NULL, NULL, NULL);
        /* launch timer thread */
		
        //if((timeret = pthread_create(timethread, NULL, TraceIOReplayer::executeTimer, (void*)8)) < 0)
			
		
		
		if((timeret = pthread_create(timethread, NULL, TraceIOReplayer::executeTimer, (void*)8)) < 0)
            errMsg("Time thread creation failure!\n");

        /* launch all worker threads */
        for(unsigned i = 0; i < WT; i++)
        {
            //if((rets[i] = pthread_create(&threads[i], NULL, TraceIOReplayer::executeWorker, (void*)(params + i))) < 0)
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
		//if(pthread_sigmask(SIG_BLOCK, &sigset, NULL) < 0) printf("Block signal unsuccessfully h\n");
		/*
		while(!IOPrep_finish){
			if(signalwait == 1){
				if(pthread_sigmask(SIG_BLOCK, &sigset, NULL) < 0) printf("Block signal unsuccessfully h\n");
				if(signalwait == 0){
					if(pthread_sigmask(SIG_UNBLOCK, &sigset, NULL) < 0) printf("unblock signal unsuccessfully h\n");
				}
			}
		}
		*/
        // All there is left to do is wait for the threads to complete.
		
		pthread_join(ioprepthread[0], NULL);
        for( unsigned i = 0; i < WT; i++)
            pthread_join(threads[i], NULL);
	
        printf ("Worker threads are done.\n");
        //pthread_join(harvestthread[0], NULL);
       // printf ("Harvester thread is done.\n");
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
		//while (io_complete[0] != io_count[0]);
		//printf("Completed %d IO Bundles in %Lf seconds, %Lf Bundle/S\n", io_complete[0], ((long double)(core_time - base_time) / (long double) 1000000000), (long double) io_complete[0] / ((long double)(core_time - base_time) / (long double)1000000000) );
#ifdef DEBUG_TIMING        
		// Write down collected Debug logs for Replay Timings now
		//FILE *fp;
		//fp = fopen( "Timing-Log.csv", "w");
		//fprintf( fp , " THREAD_ID, issueTime\n" );
		unsigned long long Totaltime=0;
		unsigned long long Extime=0;
		long numberreq=0;
		long overlap=0;
		
		for(unsigned i=0 ; i < WT; i++){
			for(unsigned j=0; j <=totnumreq ; j ++ ) {   
				//excuteTime[i][j]=endTime[i][j]-issueTime[i][j];
				//Totaltime=beginTime[i][j]-issueTime[i][j];
				//printf("No#%d issue: %f record time: %f\n ", j, float(issueTime[i][j])/1000000000, float(beginTime[i][j])/1000000000);
				//printf("No#%d issue: %llu end: %llu excution time: %llu\n ", j, issueTime[i][j], wallTime[i][j], issueTime[i][j]-wallTime[i][j]);
				//printf("%d	Wtime: %f  Begin: %f  Issue %f  endTime %f size %d\n ", j, float(wallTime[i][j])/1000000000, float(beginTime[i][j])/1000000000, float(issueTime[i][j])/1000000000, float(endTime[i][j])/1000000000), glbsize[i][j];
			}
		//fprintf(fp , " ================================================================================================\n" ); 
		}
		for(unsigned i=0 ; i < WT; i++){
			for(unsigned j=0; j <=totnumreq ; j ++ ) {   
				//Totaltime+=endTime[i][j]-issueTime[i][j];
				//Extime+=issueTime[i][j]-beginTime[i][j];
			}
			numberreq=numberreq+io_count[i];
		//fprintf(fp , " ================================================================================================\n" ); 
		}

		printf("Total time #: %f \n",executiontime);
		printf("Total %llu reqs Mean: %fs\n", totnumreq, executiontime/totnumreq);
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
  static void* prepareIOs(void* shared_buffer)
    {
        extern unsigned int warmupIO;
        unsigned long int totalBundles;
        unsigned long int perThreadBundles;
        unsigned long int i ; //loop counter
        unsigned long int totalWarmupBundles;
        unsigned long int perThreadWarmups;
        

        if(oneObj->dump->start() == false)
		{
            printf("No IO to execute, exiting\n");
            exit(1);
		}
        /* Allocate the shared buffer for use by warmup and trace IO */
		/*
		void* shared_buffer;
        if(posix_memalign(&shared_buffer, 4096, SHARED_BUFFER_SIZE) != 0)
		//if(posix_memalign(&shared_buffer, 4096, SHARED_BUFFER_SIZE) != 0)	
        {
            errMsg("posix_memalign error \n");
            exit(1);
        }
*/
        if(warmupIO != 0)
        {
            vector<Bundle*> allWarmupBundles;
            //doWarmupBundles(allWarmupBundles, shared_buffer);
            /* Warmup bundles are ready for round robin allocation to threads */
            totalWarmupBundles = allWarmupBundles.size();
            perThreadWarmups = (totalWarmupBundles / WT) + 2;

            for(i = 0; i < WT; i++)
            {
                oneObj->allWarmupIOs[i] = new  Bundle [perThreadWarmups]; // call default constructor , iocb_bunch is not allocated yet
                oneObj->allWarmupIOs[i][(perThreadWarmups - 1)].iocb_bunch = NULL; /* make sure all [*][6] are null terminated, although it
                                                                      * was initialized by NULL in the default constructor */
            }

            for(i = 0 ; i < totalWarmupBundles ; i++)
            {
                /* Round robin */
                oneObj->allWarmupIOs[i % WT][i / WT].allocateIocbBunch(allWarmupBundles[i]->size_ios);
                oneObj->allWarmupIOs[i % WT][i / WT] = *(allWarmupBundles[i]); //[0][5] is valid
                warmup_count[i % WT] += allWarmupBundles[i]->size_ios;
                warmup_bundle_count[i % WT] ++;
                // deallocated memory used for allBundles
            }

            while(i % WT)
            {
                /* it is ok if this peace of code does not execute,
                * unallocated iocb_bunch are already nulled in default constructure */
                oneObj->allWarmupIOs[i % WT][i / WT].iocb_bunch = NULL; //null terminate [1][21/4],[2][5],[3][5]
                ++i;
            }

            for(i = 0; i < WT ; i++)
            {
                oneObj->params[i].warmup_ioq = oneObj->allWarmupIOs[i];
            }

            /*  Need to re-init trace file pointer to the beginning */
            oneObj->dump->restart();
        }
		 
        vector<Bundle*> allBundles;
        //doBundleDump(allBundles, shared_buffer);
		
		/*doBundles!!!!!!!!*/
		vector<struct aiocb64> currentBundle;
        DRecordData* rec;
        unsigned long long now; //Current request time (nanoseconds)
        bool isFirst = true;
        unsigned op;
        int rec_count = 0;
        int startLoad = 0; //the controller target load before issue a bundle to kernel
		perThreadBundles = maxreq;   
		for(i = 0; i < WT ; i++)
        {
           // oneObj->params[i].ioq = oneObj->allIOs[i];

			depWaitTimeStart[i] = new unsigned long long [perThreadBundles];
			//issueTime[i] = new unsigned long long [perThreadBundles]; 
			beginTime[i] = new unsigned long long [perThreadBundles];
			reachTime[i] = new unsigned long long [perThreadBundles];
			excuteTime[i] = new unsigned long long [perThreadBundles];
			endTime[i] = new unsigned long long [perThreadBundles]; 
			//wallTime[i] = new unsigned long long [perThreadBundles]; 
        }
		
       //while((rec = dump->next()))
        //{
		unsigned long long count = 0;
		while(count <=totreq & bool(rec = oneObj->dump->next()) ){
			//rec = dump->next();
			pthread_mutex_lock ( lock); 
			if(printflag == 1) printf("IOOOOOOOOOO lock\n"); //By Bingzhe	
			//op = 0;
            //printf("doBundleDump:  records processed = %d\n",rec_count++);
            //struct aiocb64* io;
            //op = (*rec)["op"].i;
			op = (*rec)["op"].i;
            int currLoad = ((*rec)["inflight_ios"].i) / WT;
			//printf("-----------IOLogTrace::max_optypes: %d op: %d count: %llu  %d\n", IOLogTrace::max_optypes, op, count, op < IOLogTrace::max_optypes); //By Bingzhe
			if(op < IOLogTrace::max_optypes)
			{
                int fd, lun;
                unsigned long long offset, size;
                unsigned long long slack_scale;
                unsigned long long iotime; //io submission time after scale in nanoseconds
                int cmp = oneObj->replaycfg->rescale(rec, fd, slack_scale, offset, size, iotime);  //iotime is issue time
				CountBundle++;
				count++;
                if(cmp == 0)  // <0 means skip this IO, out of range
                {
                    now = iotime; //nano second time
                    now += 100000000; // shift all io by .1 second to allow for ramp-up phase

					requestID++;
					if(printflag == 1) printf("IOOOOOOOOOOz1\n");
					if(requestID == 50000) IOPrep_ready = 1;
					struct aiocb64* io = (struct aiocb64*) calloc(1, sizeof(aiocb64));
					//struct aiocb64* io = new aiocb64;
					io->aio_fildes = fd;
					io->aio_offset = offset;
					io->aio_nbytes = size;
					io->aio_buf = shared_buffer;
					
					//req->ioo = &io;
					//printf("IOOOOOO # %d size %d\n", sizeof(ULONG64), io.aio_nbytes);//iox->aio_nbytes);
					//printf("IOOOOOO # %llu totreq: %llu count:%llu\n", requestID, totreq, count);//iox->aio_nbytes);
					//io.aio_sigevent.sigev_value.sival_ptr = req;
                    if(op == 1)  // write op
                    {
						io->aio_lio_opcode = LIO_WRITE;
                        //io_prep_pwrite(&io, fd, req->buffer, req->bufsize, offset);
                    } else  // read op
                    {
						io->aio_lio_opcode = LIO_READ;
                        //io_prep_pread(&io, fd, req->buffer, req->bufsize, offset);
                    }
		
                    //io.data =  req;
					
					
					
					if(printflag == 1) printf("IOOOOOOOOOO requestID: %llu, size %llu, offseet %llu op %d time %llu io %p\n", requestID, size, offset, op, now, io); //By Bingzhe
					//printf("IOOOOOOOOOO requestID: %llu, size %llu, offseet %llu ita %p\n", requestID, size, offset, io); //By Bingzhe						
					/*
					Bundle* newBundleP =  new Bundle(1);
					newBundleP->startTimeNano = now ;
					newBundleP->iocb_bunch[0] = new struct aiocb64;
                    newBundleP->iocb_bunch[0] = io;
					allBundles.push_back(newBundleP);
					oneObj->params[0].ioqvector.push_back(newBundleP);
					*/
					if(printflag == 1) printf("IOOOOOOOOOO1\n");
					iov.push_back(io);
					if(printflag == 1) printf("IOOOOOOOOOO2\n");
					starttime.push_back(now);
					//free(io);
					/*
					if(finishio.size()>=100){
						std::vector<struct aiocb64*>::iterator ita = finishio.begin();
						for(ita = finishio.begin();ita != finishio.end(); ++ita){
							free(*ita);
						}
						finishio.clear();
					}
					*/
                }
            }
            else
            {
                errMsg("Skip invalid Operation in trace file");
            }
			if(printflag == 1) printf("IOOOOOOOOOO unlock\n"); //By Bingzhe
			pthread_mutex_unlock (lock);
				
			//pthread_cond_signal( &ItemAvailable ); 
			//printf("-----------%llu \n", CountBundle); //By Bingzhe
        } // end while()
		//totalBundles = maxreq;
		//totalBundles = allBundles.size();
        //perThreadBundles = (totalBundles / WT) + 2; //example 21/4 + 2 = 7
		
		
		/*
        for(i = 0; i < WT; i++)
        {
           oneObj->allIOs[i] = new  Bundle [perThreadBundles]; // call default constructor , iocb_bunch is not allocated yet
           oneObj->allIOs[i][(perThreadBundles - 1)].iocb_bunch = NULL; 
        }

        for(i = 0 ; i < totalBundles ; i++)
        {
            oneObj->allIOs[i % WT][i / WT].allocateIocbBunch(allBundles[i]->size_ios);
            oneObj->allIOs[i % WT][i / WT] = *(allBundles[i]); //[0][5] is valid
            io_count[i % WT] += allBundles[i]->size_ios;
            bundle_count[i % WT] ++;
            // deallocated memory used for allBoundles
        }	
		
		for(i = 0; i < WT ; i++) oneObj->params[i].ioq = oneObj->allIOs[i];
		
		for (i = 0; i < totalBundles; i++){
			printf("IO request#%d	size: %d  offset %d\n", i,oneObj-> params[0].ioq[i].iocb_bunch[0]->aio_nbytes, oneObj->params[0].ioq[i].iocb_bunch[0]->aio_offset);

		}
		*/
		
		totnumreq = requestID;
		IOPrep_finish = 1;
		IOPrep_ready = 2;
       // assert(allBundles.size());
        //assert(currentBundle.size() == 0);
        printf("Done prepareIOs %llu requests, count %llu.\n", totnumreq, count);
		/*
		int j=0;
		for (std::vector<Bundle*>::iterator it = oneObj->params[0].ioqvector.begin() ; it != oneObj->params[0].ioqvector.end(); ++it){
			j++;			
			printf("Prep Submit IO request#%d	size: %d  time %f\n", j, (*it)->iocb_bunch[0]->aio_nbytes, float(core_time - base_time)/1000000000);
		}
		*/
        /* Bundles are ready for round robin job now */
        /* allocate per thread data structures to carry bundles */
       

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
		 /*
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(1, &cpuset);

        if(pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0)
        {
            errMsg("Failed to set Thread Affinity\n");
        }
		*/
		/*
		pid_t  pid;
        pid = getpid();
		int retbind=0;
		cpu_t bindparameter = 1;
		retbind = bindprocessor(BINDPROCESS, pthread_self(), bindparameter );
		printf("Selfthread %d %d\n", pthread_self(), pid);
		if (retbind == -1) 
		{
			printf("Timer    Failed to set Thread Affinity  %d\n", errno);
			if (errno == EINVAL) printf("The What parameter is invalid\n");
			if (errno == ESRCH) printf("The specified process or thread does not exist\n");
			if (errno == EPERM) printf("The caller does not have root user authority\n");
		}else if ( retbind == 0) printf("Timer thread running on Core %d\n", bindparameter);
		*/
	

        warm_start = rdtscp();
		//sleep(10);
		//printf("sleep time %f %llu\n", float(rdtscp()-warm_start)/max_speed/1000000000, warm_start);
		
        cycle_count = warm_start;
        while ((cycle_count - warm_start) / (max_speed) < 10000)
        {
            cycle_count = rdtscp();
			//printf("Timer warmup complete %f\n", max_speed);
        }

        printf("Timer warmup complete\n");
        cycle_count = rdtscp();  //Make sure core_time is initialized before timer_ready is set
        core_time = cycle_count / max_speed; //NANO change
        timer_ready = 1;

        while(1)
        {
            cycle_count = rdtscp();
            core_time = cycle_count / max_speed; //NANO change
			//printf("Timer warmup complete %llu %llu\n", cycle_count, core_time);
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
        //io_context_t ctx = *(param->ctx);
        Bundle* ioq = param->ioq;
        //Bundle* warmup_ioq = param->warmup_ioq;
        long tid = param->id;
        int submit_err;
        int first_worker;
		
        if ((numSockets == 1) || ((WT + 3) <= cores_per_socket))
        {
            first_worker = 12;
        }
        else
        {
            if(WT > cores_per_socket)
            {
                first_worker = 3;
            }
            else
        
            {
                first_worker = cores_per_socket;
            }
        }
		
		//struct sigaction action, oaction;
		
		
		
		
        long counter = 0;
		tottest = 0;
        int qmax = max_inflight / WT;
        int qlen = qmax + 8;
		int size;
        unsigned long long wtime;  //Used to carry integer iotime in nanoseconds

        running[tid] = 1;
		
        while(!start_threads)
            ;
		printf("worker start %d\n", start_threads);
        /* submit async IO requests in the private queue */
        //for(int i = 0; ioq[i].iocb_bunch != NULL ; i++)
		
		unsigned long long count =0;
		unsigned long long i=0;

		int flag = 1;
		//iter_worker = 0;
		
		sigset_t sigset, oldMask, newMask;
		//sigemptyset(&action.sa_mask); //new add
		//sigaddset(&action.sa_mask, SIG_AIO);
		//sigemptyset(&oldMask);
		
		sigemptyset(&sigset);
		sigaddset(&sigset, SIG_AIO);
		
        while(flag){
			if(printflag == 1) printf("------start %llu %llu\n", iter_worker, requestID);
			
			
			int rc;
			
			if (pthread_sigmask(SIG_BLOCK, &sigset, &oldMask) !=0) {
				perror ("sigprocmask");
				exit(1);
			}
			
			if( finishreq.size() >= 10000 ){
			//if( count >= 10000 ){
				
				std::vector<struct iooq*>::iterator ita = finishreq.begin();
					//sigprocmask(SIG_BLOCK, &sigset, &oldMask);
					
				while(finishreq.size() >= 1){
							
					if((*ita)->io != NULL) free((*ita)->io);
					free (*ita);
					finishreq.erase(ita);
					ita = finishreq.begin();
				}
			}
			
			if (pthread_sigmask(SIG_SETMASK, &oldMask, NULL) < 0) {
				perror ("sigprocmask");
				exit(1);
			}
			
			//rc = pthread_sigmask (SIG_UNBLOCK, &sigset, NULL);
			//if (rc != 0) printf("UNBlock error! \n");
			
			
			memset (&action, 0, sizeof(action));
			action.sa_sigaction = &aio_handler;	//action.sa_flags =SA_NODEFER | SA_SIGINFO; //SA_NODEFER
			action.sa_flags = SA_SIGINFO;
			if (sigaction(SIG_AIO, &action, NULL) != 0){
					perror(" sigaction");
					exit(-1);
			}
			if(printflag == 1) printf("** finish\n");
			
			if(count >= requestID | iov.empty() | starttime.empty()){
				if(printflag == 1) printf("empty\n");
			//if(*ioq[i].iocb_bunch== NULL){
				//while (pthread_cond_wait( &ItemAvailable, &lock) != 0) ; 
			}  
			else{
				
				
				
				pthread_mutex_lock ( lock);
				if(printflag == 1) printf("-------lock start \n");
				std::list<aiocb64*>::iterator it = iov.begin();  //aiocb64
				std::list<unsigned long long>::iterator itt = starttime.begin();
				//std::list<aiocb64*>::iterator it = iov.begin();
				//std::list<unsigned long long>::iterator itt = starttime.begin();
				//it = it+iter_worker;
				if(printflag == 1) printf("------start 2 \n");

				
				
				//printf("New1 \n");
				//pthread_mutex_lock ( &lock1);
				//iooq* req = new(sizeof(aiocb64), nothrow) iooq(i);
				
				iooq* req = (iooq*) calloc(1, sizeof(iooq));

				liocb* ioqn = (aiocb64*) calloc(1, sizeof(liocb)); //aiocb64

				if(printflag == 1) printf("------start 2.5 \n");
				ioqn->aio_fildes = (*it)->aio_fildes;
				ioqn->aio_offset = (*it)->aio_offset;
				ioqn->aio_nbytes = (*it)->aio_nbytes;
				ioqn->aio_lio_opcode = (*it)->aio_lio_opcode;
				ioqn->aio_buf = (*it)->aio_buf;					
				req->id = count;
				req->io = ioqn;
				wtime = (*itt);
				printf("IO request#%llu	size: %d  offset %llu ID %llu it %p\n", count, (*(it))->aio_nbytes, (*(it))->aio_offset, req->id, it);
				free(*it);
				iov.erase(it);
				starttime.erase(itt);
				pthread_mutex_unlock ( lock);
				
				
				
				//
				if(printflag == 1) printf("------start 3 \n");
				//printf("------core_time - base_time %llu i=%llu\n", core_time - base_time, wtime);
				
				wallTime[count] = wtime;
				while(likely(wtime > (core_time - base_time))){
					//printf("------wtime %llu\n", wtime);
					//printf("------core_time - base_time %llu i=%llu\n", core_time - base_time, i);
				}
				req->issueTime = core_time - base_time;
				issueTimex[count] =  core_time - base_time;
				delta = delta + (float)(issueTimex[count]-wallTime[count])/1000000000;
				error_rate = error_rate + (float)(issueTimex[count]-wallTime[count])/(float)wallTime[count];
				
				
				
				
				
				
				struct sigevent sig;
				sig.sigev_notify = SIGEV_SIGNAL;
				sig.sigev_signo = SIG_AIO;				
				sig.sigev_value.sival_ptr = req;
				if(req == NULL) printf("IRequest error \n");
				//printf("New3 \n");
				//iooq* req = new iooq(ioq[i].iocb_bunch[0], i, tid);				
				//printf("------start 3 %p\n", sig.sigev_value.sival_ptr);
				//wtime = (*it)->startTimeNano;
				//printf("------start 3 %llu\n",ioq[i].startTimeNano);
				//wtime = ioq[i].startTimeNano;
				
				//printf("New4 \n");
				if(printflag == 1) printf("------start 3 %llu\n", wtime);
				//reachTime[tid][i]=core_time - base_time;
				//wallTime[tid][i]=wtime;
				
				
				//printf("IO request#%llu	size: %d  offset %llu wtime %f itime %f capacity %d ioqn %d \n", count, ioqn->aio_nbytes, ioqn->aio_offset, float(core_time - base_time)/1000000000, float(wtime)/1000000000, finishreq.size(), iov.size());
				int aiosubmit = lio_listio64(LIO_NOWAIT, &ioqn, 1, &sig);
				//if( lio_listio64(LIO_NOWAIT, &ioqn, 1, &sig) !=-1){ 
				if(aiosubmit == 0){
						//issueTime.push_back(core_time - base_time);
						//req->issueTime = core_time - base_time;
						tottest++;
						//io_complete[0]++;
						if(printflag == 1) printf("Submit IO request#%llu	size: %d  time %f\n", count, ioqn->aio_nbytes, float(core_time - base_time)/1000000000);
						//printf("Sucessfully submit req %d\n", i);
				}else{
					//if((submit_err = syscall(__NR_io_submit, ctx, bundleSize, ioq[i].iocb_bunch)) < 0) {
					if(aiosubmit == EAGAIN ) printf("Out of resources\n");
					else if(aiosubmit == EINVAL  ) printf("mode is invalid\n");
					else if(aiosubmit == EINTR    ) printf("EINTR  \n");
					else if(aiosubmit == EIO    ) printf("EIO  \n");
					else if(aiosubmit == ECANCELED    ) printf("ECANCELED  \n");
					else if(aiosubmit == EFBIG    ) printf("ECANCELED  \n");
					else if(aiosubmit == EINPROGRESS    ) printf("EINPROGRESS  \n");
					else if(aiosubmit == EOVERFLOW    ) printf("EOVERFLOW  \n");
					else printf("Failed with counter = %llu on error no %d\n", count, errno);
					//thread_err[tid] = TE_UNKNOWN;
					//continue;
				}
				count++;
				
				///itt++;
				//free(req);
				//printf("IO request2#%llu	size: %d  offset %llu op %d it %p, req: %p\n", i, (*(it))->aio_nbytes, (*(it))->aio_offset, (*(it))->aio_lio_opcode, it, req);
				
				//it++;
				if(IOPrep_finish == 1 & count >= totnumreq) flag =0;
				if(printflag == 1) printf("-------unlock end \n");
					
			}
			
		}
        printf("Worker %ld complete. Total IO count %llu\n", tid, count);
		/*
		while (io_complete[0] < io_count[0]){
			int transfer_count, completion_key
			LPOVERLAPPED overlapped;
			c = GetQueuedCompletionStatus (34, &transfer_count, &completion_key, &overlapped, 1000);
		}	
		*/
		//printf("Harvester completed with %d errors\n", (eoferr + reterr + unkerr));
        //printf("EOF errors:  %d\n", eoferr);
        //printf("Defined errors (code negative):  %d\n", reterr);
        //printf("Unknown errors (code positive): %d\n", unkerr);
		
		while (io_complete[0] < io_count[0] & (io_complete[0] < io_count[0]-1) & IOPrep_finish == 1){		}
		//printf("Completed %d \n", io_complete[0]);
        printf("Completed %d IO Bundles in %Lf seconds, %Lf Bundle/S\n", io_complete[0], ((long double)(core_time - base_time) / (long double) 1000000000), (long double) io_complete[0] / ((long double)(core_time - base_time) / (long double)1000000000) );
        printf("Completed %d IO in %Lf seconds, %LF IOPS\n", io_complete[0], ((long double)(core_time - base_time) / (long double)1000000000), (long double)io_complete[0] / ((long double)(core_time - base_time) / (long double)1000000000 ) );
        printf("HF replaying tot delta: %f, tot error rate: %f \n", delta, error_rate);
		/*
		for (int i = 0; i < count; i++){ //150828  //566407 //32768
			delta = delta + (float)(issueTimex[i]-wallTime[i])/1000000000;
			error_rate = error_rate + (float)(issueTimex[i]-wallTime[i])/(float)wallTime[i];
		}
		*/
		/*
		FILE * pFile2;
		pFile2 = fopen ("timestamp","w");
		for (int i = 0; i < count; i++) //150828  //566407 //32768
				fprintf(pFile2, "%d	%f	%f	%f	%f\n",i, (float)wallTime[i]/1000000000, (float)issueTimex[i]/1000000000, (float)(issueTimex[i]-wallTime[i])/1000000000, (float)(issueTimex[i]-wallTime[i])/(float)wallTime[i]);
				
		fclose (pFile2);
		*/
		return 0;
        pthread_exit(NULL);
    }

private:
    /*************************************************************************
     **  This is the private function that performs IOCB bundling
     **
     *************************************************************************/

   

    /*************************************************************************
     **  This is the private function that creates warmup bundles
     **
     *************************************************************************/

    void doWarmupBundles(vector<Bundle*> & allWarmupBundles, void* shared_buffer )
    {
        vector<struct aiocb64> currentBundle;
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

            struct aiocb64 io;

            op = (*rec)["op"].i;

            if(op >= IOLogTrace::max_optypes)
            {
                errMsg("Skip invalid Operation in trace file\n");
                continue;
            }

            int fd;
            unsigned long long offset;//, size;
			unsigned long long size;
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
				io.aio_lio_opcode = LIO_WRITE;
                //io_prep_pwrite(&io, fd, req->buffer, req->bufsize, offset);
            }

            if(op == 0) // read op
            {
                if(readcnt == warmReadMax)
                {
                    continue;
                }
				io.aio_lio_opcode = LIO_READ;
                readcnt++;
                //io_prep_pread(&io, fd, req->buffer, req->bufsize, offset);
            }

            warmupcnt++;
            //io.data =  req;
			//io.aio_sigevent.sigev_value.sival_ptr = req;
            /* aiocb is ready now, check if it fits to the previous bundle */

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
                struct aiocb64 lastIocb = currentBundle.back();
                struct AIORequest* lastReqP = (AIORequest*) lastIocb.aio_sigevent.sigev_value.sival_ptr;
                /* for warmup, we always start a new bundle */
                Bundle* newBundleP =  new Bundle(currentBundle.size());  //dynamic allocate new bundle with the right size
                struct aiocb64 firstIocb = currentBundle.front();
                struct AIORequest* firstReqP = (AIORequest*) firstIocb.aio_sigevent.sigev_value.sival_ptr;
                newBundleP->startTimeNano = firstReqP->ts ;

                for(unsigned i = 0; i < currentBundle.size() ; i++)
                {
                    newBundleP->iocb_bunch[i] = new struct aiocb64;
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
            struct aiocb64 firstIocb = currentBundle.front();
            struct AIORequest firstAIOreq = *(AIORequest*)firstIocb.aio_sigevent.sigev_value.sival_ptr;
            lastBundleP->startTimeNano = firstAIOreq.ts ;

            for(unsigned i = 0; i < currentBundle.size() ; i++)
            {
                lastBundleP->iocb_bunch[i] = new struct aiocb64;
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

