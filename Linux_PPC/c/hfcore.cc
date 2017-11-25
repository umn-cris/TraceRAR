/*
© Regents of the University of Minnesota. 
   This software is licensed under GPL version 3.0 (https://www.gnu.org/licenses/gpl-3.0.en.html). 
*/
/*
 **    Authors:  Sai Susarla, Weiping He, Jerry Fredin, Ibra Fall,Nikhil Sharma
 **              Alireza Haghdoost, Bingzhe Li, Farnaz Toussi, Clark Anderson, David Lilja, David Du
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
#include <algorithm>    // std::find
#include <sys/wait.h>
#include "HFPlayerUtils.h"

#define SHARED_BUFFER_SIZE 4096 * 1024 * 32

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
#define granularity 32
#define granularity_fine 8
#define Mshift 50            // shift for max and min value
#define MaxProb 0.05            // MaxProb

#define HE_ENDFILE (HE_ERROR | ENDFILE)
#define HE_COMPERR (HE_ERROR | COMPERR)
#define HE_UNKNOWN (HE_ERROR | UNKNOWN)
#define HE_WARMUP  (HE_ERROR | WARMUP)

#define TE_BADFD (TE_ERROR | BADFD)
#define TE_UNKNOWN (TE_ERROR | UNKNOWN)
#define TE_OVERLOAD (TE_ERROR | OVERLOAD)
#define TE_WARMUP (TE_ERROR | WARMUP)

#define maxreq 1000
extern char* cfgfile;
extern unsigned WT;
extern unsigned NRR;
extern unsigned RAR;
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
unsigned requestID;
unsigned CountBundle;

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

static __inline__ uint64_t rdtscp(void)
{
	unsigned int tbl, tbu0, tbu1;

	timebasestruct_t present_time;  					// EXPERIMENT 	A001
	read_real_time(&present_time, TIMEBASE_SZ); 		// EXPERIMENT   A001
	time_base_to_time(&present_time, TIMEBASE_SZ);		// EXPERIMENT	A001
	tbu1 = present_time.tb_high;						// EXPERIMENT	A001
	tbl = present_time.tb_low;							// EXPERIMENT	A001

//    do {												// EXPERIMENT	D001
//	  __asm__ __volatile__ ("mftbu %0" : "=r"(tbu0));	// EXPERIMENT	D001
//	  __asm__ __volatile__ ("mftb %0" : "=r"(tbl));		// EXPERIMENT	D001
//	  __asm__ __volatile__ ("mftbu %0" : "=r"(tbu1));	// EXPERIMENT	D001
//     } while (tbu0 != tbu1);							// EXPERIMENT	D001

//   return (((unsigned long long)tbu1) << 32) | tbl;  				/* D001 */
     return (((unsigned long long)tbu1) * 1000000000) + tbl;  		/* A001
     	 	 	 	 	 	 	 	 	 Convert tbu from seconds to   A001
     	 	 	 	 	 	 	 	 	 nanoseconds & add to tbl.     A001 */
}

struct AIORequest
{
public:
    ULONG64 seqid;
	unsigned requestID;
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
struct probability{
	float P_offset[granularity];
	float P_size[granularity];
	float P_time[granularity];
	float P_WR;
	float P_seq;
	float P_lun[10];
	
	float C_offset[granularity];
	float C_size[granularity];
	float C_time[granularity];
	float C_lun[10];
	
	/*
	probability(){
		for(unsigned i = 0; i < granularity; i++){
			C_offset[i] = 0;
			C_size[i] = 0;
			C_time[i] = 0;
		}
		
	}
	*/
};
struct TraceAnalysis
{
public:
	struct RatioRW{
		int write;
		int read;
	};
	int time[granularity];
	int size[granularity];
	int offset[granularity];
	int lun[10];
	int seq;
	float P_WR;

	RatioRW RatioRW;
	probability prob;
	long totio;
	
	std::vector<unsigned long long> number_time;
	std::vector<unsigned long long> number_size;
	std::vector<unsigned long long> number_offset;
	
	std::vector<float> prob_time;
	std::vector<float> prob_size;
	std::vector<float> prob_offset;
	
	std::vector<float> cdf_time;
	std::vector<float> cdf_size;
	std::vector<float> cdf_offset;
	
	std::vector<int> number_lun;
	std::vector<float> prob_lun;
	std::vector<float> cdf_lun;
	
	
	TraceAnalysis(){
		RatioRW.read = 0;
		RatioRW.write = 0;
		
		totio = 0;
		seq = 0;
		for(unsigned i = 0; i < 10; i++){
			lun[i] = 0;
		}
		for(unsigned i = 0; i < granularity; i++){
			time[i] = 0;
			size[i] = 0;
			offset[i] = 0;
			
			prob.C_offset[i] = 0;
			prob.C_size[i] = 0;
			prob.C_time[i] = 0;
		}
		
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
int IOPrep_ready = 0;
int IOPrep_finish = 0;
unsigned long long totnumreq;
pthread_mutex_t* lock;
pthread_mutex_t* lock3;
pthread_mutex_t* lock1;
pthread_mutexattr_t mutex_shared_attr;
unsigned long long IntervalSize[granularity];
unsigned long long IntervalOffset[granularity];
unsigned long long IntervalTime[granularity];
unsigned long long totreq = 14000000;
float executiontime = 0;
//unsigned long long totreq = 1000;

int printflag = 0;
std::list<iocb*> iov;
std::list<unsigned long long> starttime;

unsigned long long* wallTime[MAX_THREAD];
unsigned long long* beginTime[MAX_THREAD];
unsigned long long* reachTime[MAX_THREAD];
unsigned long long* excuteTime[MAX_THREAD];
unsigned long long* endTime[MAX_THREAD];
unsigned long long* depWaitTimeStart[MAX_THREAD];


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
		
		lock = (pthread_mutex_t*) calloc(1, sizeof(pthread_mutex_t));
		lock1 = (pthread_mutex_t*) calloc(1, sizeof(pthread_mutex_t));
		lock3 = (pthread_mutex_t*) calloc(1, sizeof(pthread_mutex_t));
		//pthread_mutexattr_init(&mutex_shared_attr);
		//pthread_mutexattr_setpshared(&mutex_shared_attr, PTHREAD_MUTEX_NORMAL);
		
		pthread_mutex_init( lock, NULL);
		pthread_mutex_init( lock1, NULL);
		pthread_mutex_init( lock3, NULL);
		
        /** Check for Incorrect Number of Cores **/
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
		
		if(dump->start() == false){
			printf("Read trace file error!\n");
			 exit(1);
		}
		
        /* set start time point */
        /* this code is used for traces that don't start at time zero */
        // long quo;
        // quo = start_point / 1000000;
        // st.tv_sec = quo;
        // st.tv_nsec = (long)( (start_point - quo * 1000000) * 1000 );
        /* declare all worker threads */
		if(RAR ==1 | RAR == 3 | RAR == 5){
			threads = new pthread_t[WT];
			ioprepthread = new pthread_t;
			timethread = new pthread_t;
			harvestthread = new pthread_t;

			running = new int[WT];
			warmdone = new int[WT];
			io_complete = new long[WT];
			context = new io_context_t[WT];
			thread_err = new int[WT + 2];
			int rets[WT], timeret, harvestret, debugret, ioprep;;
			void* shared_buffer;
			if(posix_memalign(&shared_buffer, 4096, SHARED_BUFFER_SIZE) != 0)
				
			for(unsigned i = 0; i < (WT + 2); i++)
				thread_err[i] = 0;
			if((ioprep = pthread_create(ioprepthread, NULL, TraceIOReplayer::prepareIOs,  (void*)shared_buffer)) < 0) errMsg("prepareIOs thread creation failure!\n");

			/* launch harvester thread */

			if((harvestret = pthread_create(harvestthread, NULL, TraceIOReplayer::executeHarvester, (void*)8)) < 0) errMsg("Harvester thread creation failure!\n");

			/* launch timer thread */
			if((timeret = pthread_create(timethread, NULL, TraceIOReplayer::executeTimer, (void*)8)) < 0)
				errMsg("Time thread creation failure!\n");

			/* launch all worker threads */
			for(unsigned i = 0; i < WT; i++)
			{
			   if((rets[i] = pthread_create(&threads[i], NULL, TraceIOReplayer::executeWorker, (void*)(params + i))) < 0)  errMsg("Thread creation failure!\n");
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

			if (doCapture)
			{
				startCapture();
				//sleep(10);
			}

			base_time = core_time;
			start_threads = 1;

			// All there is left to do is wait for the threads to complete.
			pthread_join(ioprepthread[0], NULL);
			for( unsigned i = 0; i < WT; i++)
				pthread_join(threads[i], NULL);

			printf ("Worker threads are done.\n");
			pthread_join(harvestthread[0], NULL);
			printf ("Harvester thread is done.\n");
			stop_timer = 1;  //Tell the timer we're done
			pthread_join(timethread[0], NULL);

	#ifdef nonanalysis
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
	#endif		
			int active_WT=0;
			for(unsigned i = 0; i < WT; i++)
			{
				if(io_count[i] != 0)
					active_WT++;
			}
						
			//double ratio = analysis.RatioRW->read/(analysis.RatioRW->read + analysis.RatioRW->write);

		
			printf("Total time #: %f \n",executiontime);
			printf("Ave excution time: %f \n", executiontime/totnumreq);
		}
		if( RAR == 2 | RAR == 3 | RAR == 4 | RAR == 5){
			struct TraceAnalysis analysis;
			doAnalysis(&analysis);
			
			printf("*************************Trace Characterization*************\n");
			printf("Total: %d, The read#: %d, Write#: %d	Ratio of R: %f\n",analysis.RatioRW.read+analysis.RatioRW.write, analysis.RatioRW.read, analysis.RatioRW.write, float(analysis.RatioRW.read)/float(analysis.RatioRW.read+analysis.RatioRW.write));
			
			FILE * pFile1;
			pFile1 = fopen ("result/size.csv","w");
			fprintf(pFile1, "#Size	value1\n");
			for (int i = 0; i < analysis.prob_size.size(); i++) 
				fprintf(pFile1, "%llu	%.5f\n", analysis.number_size[i] / 1024, analysis.cdf_size[i]);
			fclose (pFile1);
			
			FILE * pFile2;
			pFile2 = fopen ("result/offset.csv","w");
			fprintf(pFile2, "#Locality	value2\n");
			for (int i = 0; i < analysis.prob_offset.size(); i++) 
				fprintf(pFile2, "%llu	%.5f\n", analysis.number_offset[i]/1024/1024, analysis.cdf_offset[i]);
			fclose (pFile2);
			
			FILE * pFile3;
			pFile3 = fopen ("result/time.csv","w");
			fprintf(pFile3, "#Time	value3\n");
			for (int i = 0; i < analysis.prob_time.size(); i++) 
				fprintf(pFile3, "%llu	%.5f\n", analysis.number_time[i]/1000, analysis.cdf_time[i]);
			fclose (pFile3);
			
			printf("Sequentiality: %.2f \n",  float(analysis.seq)/(analysis.RatioRW.read+analysis.RatioRW.write -1));
			
			//struct probability prob;
			//prob = analysis.prob;
			if(RAR == 5 | RAR == 4) {
				if(NRR != 0) regenerator(&analysis);
			}
		}	

		return global_error;
    }
	
	int doAnalysis(TraceAnalysis* analysis)
    {
		
        unsigned long long now; //Current request time (nanoseconds)
        bool isFirst = true;
        unsigned op;
		unsigned long long pretime;
		unsigned long long newtime;
		unsigned long long presize=0;
		unsigned long long preoffset=0;
		
		unsigned long long MaxOffset[Mshift];
		unsigned long long MinOffset[Mshift];
		unsigned long long MaxSize[Mshift];
		unsigned long long MinSize[Mshift];
		unsigned long long MaxIotime[Mshift];
		unsigned long long MinIotime[Mshift];
		
		unsigned long long MaxNumbOffset[Mshift];
		unsigned long long MinNumbOffset[Mshift];
		unsigned long long MaxNumbSize[Mshift];
		unsigned long long MinNumbSize[Mshift];
		unsigned long long MaxNumbIotime[Mshift];
		unsigned long long MinNumbIotime[Mshift];
		
		unsigned long long MaxSize1 = 0;
		unsigned long long MaxIotime1 = 0;
		unsigned long long MaxOffset1 = 0;
		
		unsigned long long MinOffset1 = 10000000000000;
		unsigned long long MinSize1 = 10000000000000;
		unsigned long long MinIotime1 = 10000000000000;
		
		dump->restart();
        DRecordData* rec;
		// find the trace max value for different metrics
		pretime=0;
		newtime=0;
		
		for (int i = 0; i < Mshift; i++)	{
			MaxOffset[i] = 0;
			MinOffset[i] = 10000000000000;
			
			MaxSize[i] = 0;
			MinSize[i] = 10000000000000;
			
			MaxIotime[i] = 0;
			MinIotime[i] = 10000000000000;
			
			MaxNumbOffset[i] = 0;
			MinNumbOffset[i] = 0;
			MaxNumbSize[i] = 0;
			MinNumbSize[i] = 0;
			MaxNumbIotime[i] = 0;
			MinNumbIotime[i] = 0;
		}
		
		
		/************ Find the Max & Min values ********************/
		//while((rec = dump->next()))
        //{
		long long int count = 0;		
		while(count <=totreq &  bool(rec = dump->next()) ){
			//rec = dump->next();
			count++;
			
			int fd;
			int lun;
                unsigned long long offset, size, slack_scale, iotime;
                int cmp = replaycfg->rescale(rec, fd, slack_scale, offset, size, iotime, lun);  //iotime is issue time
				newtime = iotime;
				if(cmp == 0)  // <0 means skip this IO, out of range
                {
					/*
					if ( offset > MaxOffset) MaxOffset = offset;
					if ( size > MaxSize) MaxSize = size;
					if ( newtime - pretime > MaxIotime & pretime !=0 ) MaxIotime = newtime - pretime;
					
					if ( offset < MinOffset) MinOffset = offset;
					if ( size < MinSize) MinSize = size;
					if ( newtime - pretime < MinIotime & pretime !=0) MinIotime = newtime - pretime;
					*/
					analysis->totio++;
					//printf("### Req ID %d, size %d\n",analysis->totio, size);
					for (int i = 0; i < Mshift-1 ; i++)	{
						
						/****************** OFFSET Max&Min find *********************/
						if (offset > MaxOffset[i]){
							if (offset < MaxOffset[i+1]){
								MaxOffset[i] = offset;
								break;	
							}
							if (i == Mshift-2 && offset > MaxOffset[Mshift-1]) MaxOffset[Mshift-1] = offset;
						}
				
						if (offset < MinOffset[i]){
							if (offset > MinOffset[i+1]){
								MinOffset[i] = offset;
								break;	
							} 
							if (i == Mshift-2 && offset < MinOffset[Mshift-1]) MinOffset[Mshift-1] = offset;
						}
						
						/****************** Size Max&Min find *********************/
						if (size > MaxSize[i]){
							if (size < MaxSize[i+1]){
								MaxSize[i] = size;
								break;	
							} 
							if (i == Mshift-2 && size > MaxSize[Mshift-1]) MaxSize[Mshift-1] = size;
						}
				
						if (size < MinSize[i]){
							if (size > MinSize[i+1]){
								MinSize[i] = size;
								break;	
							} 
							if (i == Mshift-2 && size < MinSize[Mshift-1]) MinSize[Mshift-1] = size;
						}
						
						/****************** Size Max&Min find *********************/
						if ( newtime - pretime > MaxIotime[i]){
							if (newtime - pretime < MaxIotime[i+1]){
								MaxIotime[i] = newtime - pretime;
								break;	
							} 
							if (i == Mshift-2 && newtime-pretime > MaxIotime[Mshift-1]) MaxIotime[Mshift-1] = newtime - pretime;
						}
				
						if (newtime - pretime < MinIotime[i]){
							if (newtime - pretime > MinIotime[i+1]){
								MinIotime[i] = newtime - pretime;
								break;	
							} 
							if (i == Mshift-2 && newtime - pretime < MinIotime[Mshift-1]) MinIotime[Mshift-1] = newtime - pretime;
						}				
					}
					
				}
				pretime = newtime;
		}
		
		// find number of requests for each maximum value 
		dump->restart();
		pretime=0;
		newtime=0;
		
		//while((rec = dump->next()))
        //{
		count = 0;
		while(count <=totreq &  bool(rec = dump->next()) ){
			//rec = dump->next();
			count++;
		
			int fd;
			int lun;
                unsigned long long offset, size, slack_scale, iotime;
                int cmp = replaycfg->rescale(rec, fd, slack_scale, offset, size, iotime, lun);  //iotime is issue time
				newtime = iotime;
				if(cmp == 0)  // <0 means skip this IO, out of range
                {
					for (int i = 0; i < Mshift ; i++)	{
						
						/****************** OFFSET Max&Min find *********************/
						if (offset == MaxOffset[i]){
							MaxNumbOffset[i]++;
						}
				
						if (offset == MinOffset[i]){
							MinNumbOffset[i]++;
						}
						
						/****************** Size Max&Min find *********************/
						if (size == MaxSize[i]){
							MaxNumbSize[i]++;
						}
				
						if (size == MinSize[i]){
							MinNumbSize[i]++;
						}
						
						/****************** Size Max&Min find *********************/
						if ( newtime - pretime == MaxIotime[i]){
							MaxNumbIotime[i]++;
						}
				
						if (newtime - pretime == MinIotime[i]){
							MinNumbIotime[i]++;
						}				
					}
					
				}
				pretime = newtime;
		}
		
		int flag1=0;
		int flag2=0;
		int flag3=0;
		int flag4=0;
		int flag5=0;
		int flag6=0;
		for (int i = Mshift-1; i >= 0 ; i--){
			if(i != Mshift-1){
				MaxNumbSize[i]+=MaxNumbSize[i+1];
				MaxNumbOffset[i]+=MaxNumbOffset[i+1];
				MaxNumbIotime[i]+=MaxNumbIotime[i+1];
				
				MinNumbIotime[i]+=MinNumbIotime[i+1];
				MinNumbSize[i]+=MinNumbSize[i+1];
				MinNumbOffset[i]+=MinNumbOffset[i+1];
			}
			
			
			if(flag1 == 0){
				if( float(MaxNumbOffset[i])/analysis->totio >= 0.005) flag1 = i;
			}
			if(flag2 ==0){
				if( float(MinNumbOffset[i])/analysis->totio >= 0.005) flag2 = i;
			}
			if(flag3 == 0){
				if( float(MaxNumbSize[i])/analysis->totio >= 0.005) flag3 = i;
			}
			if(flag4 == 0){
				if( float(MinNumbSize[i])/analysis->totio >= 0.005) flag4 = i;
			}
			if(flag5 == 0){
				if( float(MaxNumbIotime[i])/analysis->totio >= 0.005) flag5 = i;
			}
			if(flag6 == 0){
				if( float(MinNumbIotime[i])/analysis->totio >= 0.005) flag6 = i;
			}
			
			
		}
		MaxOffset1 = MaxOffset[flag1];
		MinOffset1 = MinOffset[flag2];
		
		MaxSize1 = MaxSize[flag3];
		MinSize1 = MinSize[flag4];
		
		MaxIotime1 = MaxIotime[flag5];
		MinIotime1 = MinIotime[flag6];
		
		
		
		
		unsigned long long interval1 = (MaxSize1 -  MinSize1)/(granularity-1);
		unsigned long long interval2 = (MaxOffset1 -  MinOffset1)/(granularity-1);
		unsigned long long interval3 = (MaxIotime1 -  MinIotime1)/(granularity-1);
		
		IntervalSize[0] = MinSize1;
		IntervalOffset[0] = MinOffset1;
		IntervalTime[0] = MinIotime1;
		IntervalSize[granularity-1] = MaxSize1;
		IntervalOffset[granularity-1] = MaxOffset1;
		IntervalTime[granularity-1] = MaxIotime1;
		
		for (unsigned i = 0; i < granularity-1; i++){
			IntervalSize[i] = MinSize1 + interval1 * i;
			IntervalOffset[i] = MinOffset1 + interval2 * i;
			IntervalTime[i] = MinIotime1 + interval3 * i;
		}
		
		// Start to analyze the trace
		dump->restart();
		pretime=0;
		newtime=0;
		/************coarse grained analysis ********************/
        
		//while((rec = dump->next()))
        //{
		count = 0;
		while(count <=totreq &  bool(rec = dump->next()) ){
			//rec = dump->next();
			count++;
			
            //printf("doBundleDump:  records processed = %d\n",rec_count++);
            op = (*rec)["op"].i;			
            if(op < IOLogTrace::max_optypes)
            {
                int fd;
				int lun;
                unsigned long long offset, size;
                unsigned long long slack_scale;
                unsigned long long iotime; //io submission time after scale in nanoseconds
                int cmp = replaycfg->rescale(rec, fd, slack_scale, offset, size, iotime, lun);  //iotime is issue time

                if(cmp == 0)  // <0 means skip this IO, out of range
                {
                    now = iotime; //nano second time
                    now += 100000000; // shift all io by .1 second to allow for ramp-up phase
                    //*********** Read Write Ratio
                    if(op == 1)  // write op
                    {
						analysis->RatioRW.write++;
                    }
                    else  // read op
                    {
						analysis->RatioRW.read++; 
                    }
					//************* sequentiality **********************
					if ( offset <= presize + preoffset + DISK_BLOCK_SIZE & offset >= presize + preoffset - DISK_BLOCK_SIZE) analysis->seq++;
					presize = size;
					preoffset = offset;
					
					//************* size **********************
					//analysis->size.average = analysis->size.average + size;
					if (size <= MinSize1) analysis->size[0]++;
					else if(size > IntervalSize[granularity-1]) analysis->size[granularity-1]++;
					else
					{
						for (unsigned i = 1; i < granularity ; i++){
							if(size <= IntervalSize[i] & size > IntervalSize[i-1]){
								analysis->size[i]++;
								break;
							}
						}
					}
					
					//************* local **********************  unit is number of blocks
					if (offset <= MinOffset1) analysis->offset[0]++;
					else if(offset > IntervalOffset[granularity-1]) analysis->offset[granularity-1]++;
					else
					{
						for (unsigned i = 1; i < granularity ; i++){
							if(offset <= IntervalOffset[i] & offset > IntervalOffset[i-1]){
								analysis->offset[i]++;
								break;
							}
							
						}
					}
					
					//************* lun number **********************
					//analysis->lun[lun]++;
					std::vector<int>::iterator it;
					it = find (analysis->number_lun.begin(), analysis->number_lun.end(), lun);
					if (it == analysis->number_lun.end()) analysis->number_lun.push_back(lun);
					//************* arrival rate ********************
					newtime = iotime;
					if (count != 1){
						double time = newtime - pretime;  //unit is micron second(us)
						if (time <= MinIotime1) analysis->time[0]++;
						else if(time > IntervalTime[granularity-1]) analysis->time[granularity-1]++;
						else
						{
							for (unsigned i = 1; i < granularity ; i++){
								if(time <= IntervalTime[i] & time > IntervalTime[i-1]){
									analysis->time[i]++;
									break;
								}
							}
						}
					}
					pretime = newtime;
				}
			}
		}
		
		int checktotal1=0;
		int checktotal2=0;
		int checktotal3=0;
			
		//analysis->totio = analysis->RatioRW.write + analysis->RatioRW.read;
		
		std::vector<int> Ntime;
		std::vector<int> Nsize;
		std::vector<int> Noffset;
		
		/**** compute prob of each metric ******/
		for (unsigned i = 0; i < granularity ; i++){
			analysis->prob.P_time[i] = float( analysis->time[i] )/(analysis->totio-1);
			analysis->prob.P_size[i] = float( analysis->size[i] )/analysis->totio;
			analysis->prob.P_offset[i] = float( analysis->offset[i] )/analysis->totio;
			
			checktotal1 += analysis->time[i];
			checktotal2 += analysis->offset[i];
			checktotal3 += analysis->size[i];
			
			/******* CDF of each metrics **********/
			for (unsigned j = 0; j <=i  ; j++){
				analysis->prob.C_time[i] += analysis->prob.P_time[j];
				analysis->prob.C_size[i] += analysis->prob.P_size[j];
				analysis->prob.C_offset[i] += analysis->prob.P_offset[j];
			}
			if(i != 0){
				if(analysis->prob.P_time[i] >= MaxProb & IntervalTime[i] - IntervalTime[i-1] >= granularity_fine * 4) Ntime.push_back(i);
				if(analysis->prob.P_size[i] >= MaxProb & IntervalSize[i] - IntervalSize[i-1] >= granularity_fine * 4) Nsize.push_back(i);
				if(analysis->prob.P_offset[i] >= 0.05) {
					Noffset.push_back(i);
				}
			}
			
		}
		
		//for (unsigned i = 0; i < 10 ; i++) analysis->prob.P_lun[i] = float( analysis->lun[i] )/analysis->totio;

		if (checktotal1 != analysis->totio-1) printf("Error for time: total req should be %d, current is %d\n",analysis->totio-1, checktotal1);
		if (checktotal2 != analysis->totio) printf("Error for offset: total req should be %d, current is %d\n",analysis->totio, checktotal2);
		if (checktotal3 != analysis->totio) printf("Error for size: total req should be %d, current is %d\n",analysis->totio, checktotal3);
		
		analysis->prob.P_WR = float(analysis->RatioRW.write) / analysis->totio;
		analysis->P_WR = float(analysis->RatioRW.write) / analysis->totio;
		
		dump->restart();
		
		
		/************Fine grained analysis ********************/
		int fine_time[Ntime.size()][granularity_fine];
		int fine_offset[Noffset.size()][granularity_fine];
		int fine_size[Nsize.size()][granularity_fine];
		
		for (unsigned i = 0; i < granularity_fine ; i++){
			for (int j = 0; j < Ntime.size(); j++){
				fine_time[j][i] = 0;
			}
			for (int j = 0; j < Noffset.size(); j++){
				fine_offset[j][i] = 0;
			}
			for (int j = 0; j < Nsize.size(); j++){
				fine_size[j][i] = 0;
			}
		}
		pretime=0;
		newtime=0;
		int temp_lun[analysis->number_lun.size()];
		for (int j = 0; j < analysis->number_lun.size(); j++) temp_lun[j] = 0;
        
		//while((rec = dump->next()))
        //{
		count = 0;
		while(count <=totreq &  bool(rec = dump->next()) ){
			//rec = dump->next();
			count++;
		
            //printf("doBundleDump:  records processed = %d\n",rec_count++);
            op = (*rec)["op"].i;			
            if(op < IOLogTrace::max_optypes)
            {
                int fd;
				int lun;
                unsigned long long offset, size;
                unsigned long long slack_scale;
                unsigned long long iotime; //io submission time after scale in nanoseconds
                int cmp = replaycfg->rescale(rec, fd, slack_scale, offset, size, iotime, lun);  //iotime is issue time
				// **************** lun
				
				for (int j = 0; j < analysis->number_lun.size(); j++) 
				{
					if(analysis->number_lun[j] == lun){
						temp_lun[j]++;
						break;
					}
				}
				
				/*
				for (std::vector<int>::iterator it = Ntime.begin() ; it != Ntime.end(); ++it){
					float time_interval = float(IntervalTime[*it] - IntervalTime[*it-1]) / granularity_fine;
					if (time > IntervalTime[*it-1] & time <= IntervalTime[*it]){
						for (unsigned i = 1; i < granularity_fine ; i++){
							if(time > IntervalTime[*it-1]+i*time_interval & size <= IntervalTime[*it-1]+(i+1)*time_interval) {
								fine_time[i]++;
								break;
							}								
						}
					}
					
				}
				
				for (std::vector<int>::iterator it = Nsize.begin() ; it != Nsize.end(); ++it){
					float size_interval = float(IntervalSize[*it] - IntervalSize[*it-1]) / granularity_fine;
					if (size > IntervalSize[*it-1] & size <= IntervalSize[*it]){
						for (unsigned i = 1; i < granularity_fine ; i++){
							if(size > IntervalSize[*it-1]+i*size_interval & size <= IntervalSize[*it-1]+(i+1)*size_interval) {
								fine_size[i]++;
								break;	
							}
						}
					}
					
				}
				*/
				for (int j = 0; j < Noffset.size(); j++){
				//for (std::vector<int>::iterator it = Noffset.begin() ; it != Noffset.end(); ++it){
					float offset_interval = float(IntervalOffset[Noffset[j]] - IntervalOffset[Noffset[j]-1]) / granularity_fine;
					if (offset > IntervalOffset[Noffset[j]-1] & offset <= IntervalOffset[Noffset[j]]){
						for (unsigned i = 0; i < granularity_fine ; i++){
							if(offset > IntervalOffset[Noffset[j]-1]+i*offset_interval & offset <= IntervalOffset[Noffset[j]-1]+(i+1)*offset_interval) {
								fine_offset[j][i]++;
								break;	
							}
						}
					}
				}
				
				for (int j = 0; j < Nsize.size(); j++){
				//for (std::vector<int>::iterator it = Noffset.begin() ; it != Noffset.end(); ++it){
					float size_interval = float(IntervalSize[Nsize[j]] - IntervalSize[Nsize[j]-1]) / granularity_fine;
					if (size > IntervalSize[Nsize[j]-1] & size <= IntervalSize[Nsize[j]]){
						for (unsigned i = 0; i < granularity_fine ; i++){
							if(size > IntervalSize[Nsize[j]-1]+i*size_interval & size <= IntervalSize[Nsize[j]-1]+(i+1)*size_interval) {
								fine_size[j][i]++;
								break;	
							}
						}
					}
				}
				
				newtime = iotime;
				double time = newtime - pretime;
				for (int j = 0; j < Ntime.size(); j++){
				//for (std::vector<int>::iterator it = Noffset.begin() ; it != Noffset.end(); ++it){
					float time_interval = float(IntervalTime[Ntime[j]] - IntervalTime[Ntime[j]-1]) / granularity_fine;
					if (time > IntervalTime[Ntime[j]-1] & time <= IntervalTime[Ntime[j]]){
						for (unsigned i = 0; i < granularity_fine ; i++){
							if(time > IntervalTime[Ntime[j]-1]+i*time_interval & time <= IntervalTime[Ntime[j]-1]+(i+1)*time_interval) {
								fine_time[j][i]++;
								break;	
							}
						}
					}
				}
				pretime = newtime;

			}
		}
		/*
		for (int j = 0; j < Noffset.size(); j++){
			int totoo = 0;
			for (unsigned i = 0; i < granularity_fine ; i++){
				totoo += fine_offset[j][i];
			}
			printf( "%d	%d\n", analysis->offset[Noffset[j]], totoo);
		}
		*/
		int tempp = 0;
		for (int j = 0; j < analysis->number_lun.size(); j++) 
		{
			analysis->prob_lun.push_back(float(temp_lun[j])/analysis->totio);
			analysis->cdf_lun.push_back(tempp+float(temp_lun[j])/analysis->totio);
			tempp += float(temp_lun[j])/analysis->totio;
			printf( "LUN: %d	Prob: %.5f\n", analysis->number_lun[j], analysis->prob_lun[j]);
		}
		
		/************** Push all intervals to the vector (including coarse grained and fine grained) ********************/
		for (unsigned i = 0; i < granularity ; i++){
			int temp_flag1 = 0;
			int temp_flag2 = 0;
			int temp_flag3 = 0;
			/*
			for (std::vector<int>::iterator it = Nsize.begin() ; it != Nsize.end(); ++it){
			
				if(i == *it){
					temp_flag1 = 1;
					float size_interval = float(IntervalSize[*it] - IntervalSize[*it-1]) / granularity_fine;
					for (unsigned j = 0; j < granularity_fine ; j++){
						analysis->number_size.push_back(IntervalSize[*it-1]+j*size_interval);
						analysis->prob_size.push_back(fine_size[i]/analysis->totio);
					}
				}
			
			}
			
			if(temp_flag1 == 0){
				analysis->number_size.push_back(IntervalSize[i]);
				analysis->prob_size.push_back(analysis->prob.P_size[i]);
			}
			*/
/******************* OFFSET **************************/
			for (int m = 0; m < Noffset.size(); m++){
			//for (std::vector<int>::iterator it = Noffset.begin() ; it != Noffset.end(); ++it){
				/*** fine grained push_back ******/
				if(i == Noffset[m]){
					temp_flag1 = 1;
					float offset_interval = float(IntervalOffset[Noffset[m]] - IntervalOffset[Noffset[m]-1]) / granularity_fine;
					for (unsigned j = 0; j < granularity_fine ; j++){
						analysis->number_offset.push_back(IntervalOffset[Noffset[m]-1]+(j+1)*offset_interval);
						analysis->prob_offset.push_back(float(fine_offset[m][j])/analysis->totio);
					}
				}
			
			}
			/*** Normal push_back ******/
			if(temp_flag1 == 0){
				analysis->number_offset.push_back(IntervalOffset[i]);
				analysis->prob_offset.push_back(analysis->prob.P_offset[i]);
			}
/********************** SIZE ***********************/
			for (int m = 0; m < Nsize.size(); m++){
			//for (std::vector<int>::iterator it = Noffset.begin() ; it != Noffset.end(); ++it){
				/*** fine grained push_back ******/
				if(i == Nsize[m]){
					temp_flag2 = 1;
					float size_interval = float(IntervalSize[Noffset[m]] - IntervalSize[Noffset[m]-1]) / granularity_fine;
					for (unsigned j = 0; j < granularity_fine ; j++){
						analysis->number_size.push_back(IntervalSize[Nsize[m]-1]+(j+1)*size_interval);
						analysis->prob_size.push_back(float(fine_size[m][j])/analysis->totio);
					}
				}
			
			}
			/*** Normal push_back ******/
			if(temp_flag2 == 0){
				analysis->number_size.push_back(IntervalSize[i]);
				analysis->prob_size.push_back(analysis->prob.P_size[i]);
			}
/********************** Time ***********************/
			for (int m = 0; m < Ntime.size(); m++){
			//for (std::vector<int>::iterator it = Noffset.begin() ; it != Noffset.end(); ++it){
				/*** fine grained push_back ******/
				if(i == Ntime[m]){
					temp_flag2 = 1;
					float time_interval = float(IntervalTime[Ntime[m]] - IntervalTime[Ntime[m]-1]) / granularity_fine;
					for (unsigned j = 0; j < granularity_fine ; j++){
						analysis->number_time.push_back(IntervalTime[Ntime[m]-1]+(j+1)*time_interval);
						analysis->prob_time.push_back(float(fine_time[m][j])/analysis->totio);
					}
				}
			
			}
			/*** Normal push_back ******/
			if(temp_flag2 == 0){
				analysis->number_time.push_back(IntervalTime[i]);
				analysis->prob_time.push_back(analysis->prob.P_time[i]);
			}
		}
/**********************Compute CDF ***********************/
		float temp_cdf = 0;
		for (int i = 0; i < analysis->prob_size.size(); i++){
			temp_cdf +=  analysis->prob_size[i];
			analysis->cdf_size.push_back(temp_cdf);
			//printf( "SIZE: %llu	%.5f\n", analysis->number_size[i]/1024, analysis->prob_size[i]);
		}
		temp_cdf = 0;
		for (int i = 0; i < analysis->prob_offset.size(); i++){
			temp_cdf +=  analysis->prob_offset[i];
			analysis->cdf_offset.push_back(temp_cdf);
			//printf( "OFFSET: %llu	%.5f\n", analysis->number_offset[i]/1024/1024, analysis->prob_offset[i]);
		}
		temp_cdf = 0;
		for (int i = 0; i < analysis->prob_time.size(); i++){
			temp_cdf +=  analysis->prob_time[i];
			analysis->cdf_time.push_back(temp_cdf);
			//printf( "TIME: %llu	%.5f\n", analysis->number_time[i]/1000, analysis->prob_time[i]);
		}
		
		
		/*
		//analysis->prob.P_time[0] = float( analysis->arrival.time1 )/analysis->totio;	
		float toooo=0;
		for (unsigned i = 0; i <= 28 ; i++){
			toooo+=analysis->prob.P_time[i];
			printf("%f ", analysis->prob.P_time[0]);
		}
		printf("tooo%f\n", toooo);
		*/
		return(0);
   
   
    }

	
	int regenerator(TraceAnalysis* analysis)
	{
		FILE * pFile;
		pFile = fopen ("newtrace.csv","w");
		srand (time(NULL));
		if (pFile==NULL)
		{ 
			fprintf(stderr, "can't open new trace file\n");
		}else fprintf (pFile, "#DUMP_OFFSET.I,ELAPSED_USECS.D,ELAPSED_TICKS.I,CMD.S,INFLIGHT_IOS.I,TS.I,SEQID.I,LUN_SSID.I,OP.I,PHASE.I,LBA.I,NBLKS.I,LATENCY_TICKS.I,HOST_ID.I,HOST_LUN.I,LATENCY_USECS.D\n");
		int number;
		number = NRR;
		unsigned long long iotime = 0;
		while(number--)
		{	
			
			float random1, random2, random3, random4;
			unsigned long long offset, size;
			unsigned op;
			
			
			// offset generate
			/*
			float Poffset = 0;
			random1 = float(rand() % 1000000)/1000000;
			for (unsigned i = 0; i < granularity ; i++){
				if(random1 <= Poffset) {
					offset = IntervalOffset[i] / DISK_BLOCK_SIZE - 52736;
					break;
				}
				Poffset += prob->P_offset[i];
				
			}
			// size generate
			float Psize = 0;
			random2 = float(rand() % 1000000)/1000000;
			for (unsigned i = 0; i < granularity ; i++){
				if(random2 <= Psize) {
					size = IntervalSize[i] / DISK_BLOCK_SIZE; // number of blocks
					break;
				}
				Psize += prob->P_size[i];
			}
			
			// time generate
			float Ptime = 0;
			random3 = float(rand() % 1000000)/1000000;
			for (unsigned i = 0; i < granularity ; i++){
				if(random3 <= Ptime) {
					iotime += IntervalTime[i]/1000;
					break;
				}
				Ptime += prob->P_time[i];
			}
			*/
			// offset generate
			random1 = float(rand() % 1000000)/1000000;
			for (int i = 0; i < analysis->cdf_offset.size(); i++){
				if(random1 <= analysis->cdf_offset[i]) {
					offset = analysis->number_offset[i] / DISK_BLOCK_SIZE - 52736;
					break;
				}
			}
			
			// size generate
			random2 = float(rand() % 1000000)/1000000;
			for (int i = 0; i < analysis->cdf_size.size(); i++){
				if(random2 <= analysis->cdf_size[i]) {
					size = analysis->number_size[i] / DISK_BLOCK_SIZE; // number of blocks
					break;
				}
			}
			
			// time generate
			random3 = float(rand() % 1000000)/1000000;
			for (int i = 0; i < analysis->cdf_time.size(); i++){
				if(random3 <= analysis->cdf_time[i]) {
					iotime += analysis->number_time[i] / 1000; // number of blocks
					break;
				}
			}
			
			// op generate
			float PWR = analysis->P_WR;
			random1 = float(rand() % 1000000)/1000000;
			if(random4 <= PWR) {
				op = 1;
			}else op = 0;
		
		fprintf (pFile, "0,%llu,0,0,0,0,0,6,%u,0,%llu,%llu,0,0,0,0\n", iotime, op, offset, size);
		//fprintf (pFile, "0	%llu	0	0	0	0	0	6	%u	0	%llu	%llu	0	0	0	0\n", time, op, offset, size);
		}
		fclose (pFile);
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
		/*
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(2, &cpuset);

        if(pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0)
        {
            errMsg("Failed to set Thread Affinity\n");
        }
		*/
		if(printflag == 1) printf("++++++++++Enter lock\n"); //By Bingzhe
		/*
        if(oneObj->dump->start() == false)
		{
            printf("No IO to execute, exiting\n");
            exit(1);
		}
		*/
        vector<Bundle*> allBundles;
        //doBundleDump(allBundles, shared_buffer);
		
		/*doBundles!!!!!!!!*/
		vector<struct iocb> currentBundle;
        DRecordData* rec;
        unsigned long long now; //Current request time (nanoseconds)
        bool isFirst = true;
        unsigned op;
		perThreadBundles = maxreq;   
		
		
       //while((rec = dump->next()))
        //{
		unsigned long long count = 0;
		while(count <=totreq & bool(rec = oneObj->dump->next()) ){
			//rec = dump->next();
			pthread_mutex_lock ( lock);
			struct timespec tim, tim2;
			tim.tv_sec = 1;
			tim.tv_nsec = 500;
			
			if(iov.size()>500000) {
				printf("Nano sleep system call failed \n");
				if(nanosleep(&tim , &tim2) < 0 )   
				{
					  printf("Nano sleep system call failed \n");
				}
				 //pthread_mutex_unlock (lock);
			}
			
			if(printflag == 1) printf("IOOOOOOOOOO lock\n"); //By Bingzhe	
			op = (*rec)["op"].i;
            int currLoad = ((*rec)["inflight_ios"].i) / WT;
			//printf("-----------IOLogTrace::max_optypes: %d op: %d count: %llu  %d\n", IOLogTrace::max_optypes, op, count, op < IOLogTrace::max_optypes); //By Bingzhe
			if(op < IOLogTrace::max_optypes)
			{
				
                int fd, lun;
                unsigned long long offset, size;
                unsigned long long slack_scale;
                unsigned long long iotime; //io submission time after scale in nanoseconds
                int cmp = oneObj->replaycfg->rescale(rec, fd, slack_scale, offset, size, iotime, lun);  //iotime is issue time
				CountBundle++;
				count++;
                if(cmp == 0)  // <0 means skip this IO, out of range
                {
                    now = iotime; //nano second time
                    now += 100000000; // shift all io by .1 second to allow for ramp-up phase
					
					requestID++;

					if(printflag == 1) printf("IOOOOOOOOOOz1\n");
					if(requestID == 50000) IOPrep_ready = 1;
					struct iocb* io = (struct iocb*) calloc(1, sizeof(iocb));
					//struct aiocb64* io = new aiocb64;
					io->aio_fildes = fd;
					
					io->u.c.buf = shared_buffer;
					io->u.c.nbytes = size;
					io->u.c.offset = offset;
					
					//req->ioo = &io;
					//printf("IOOOOOO # %d size %d\n", sizeof(ULONG64), iov.size());//iox->aio_nbytes);
					//printf("IOOOOOO # %llu totreq: %llu count:%llu\n", requestID, totreq, count);//iox->aio_nbytes);
					//io.aio_sigevent.sigev_value.sival_ptr = req;
                   if(op == 1)  // write op
                    {
                        io_prep_pwrite(io, fd, shared_buffer, size, offset);
                    }
                    else  // read op
                    {
                         io_prep_pwrite(io, fd, shared_buffer, size, offset);
                    }
		
                    //io->data =  req;
					
					
					
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
					//pthread_mutex_lock ( lock);
					if(printflag == 1) printf("IOOOOOOOOOO1\n");
					iov.push_back(io);
					if(printflag == 1) printf("IOOOOOOOOOO2\n");
					starttime.push_back(now);
					//pthread_mutex_unlock (lock);
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
		
		totnumreq = requestID;
		IOPrep_finish = 1;
		IOPrep_ready = 2;
       // assert(allBundles.size());
        //assert(currentBundle.size() == 0);
        printf("Done prepareIOs %llu requests.\n", totnumreq);
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
        int queuelen = 65536; //TODO:  Make this a global variable or constant
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

        for (unsigned cnt = 0; cnt < WT; cnt++)
            io_complete[cnt] = 0;

        printf("Harvester thread running on Core %d\n", sched_getcpu());

       
        for (unsigned cnt = 0; cnt < WT; cnt++)
            io_complete[cnt] = 0;

        while(!start_threads)
            ;

        for(unsigned i = 0; i < WT; i++)
        {
            if(io_count[i] != 0)
                active_WT++;
        }

        printf("There are %d Active Worker Threads.\n", active_WT);
		

       while (1)
        {

            for(unsigned i = 0; i < 1; i++)
            {
                numevents = io_getevents(context[i], 0, queuelen, eventlist, NULL);
                if(numevents > 0)
                {	
					io_complete[i] = io_complete[i] + numevents;
                    for(int j = 0; j < numevents; j++)
                    {
                        struct io_event event = eventlist[j];
                        AIORequest* req = static_cast<AIORequest*>(event.data);
						executiontime = float(core_time-base_time)/1000000000 - float(req->ts)/1000000000 + executiontime;
						//printf("Finish NO#%d io_complete %d\n", req->requestID, io_complete[i]);
                        if(event.res != req->bufsize)
                        {
                            if(event.res == 0)
                            {
                                eoferr++;
								printf("Error returned on aio completion:  %lx,  %d\n", event.res,req->bufsize);
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
						delete (event.data);
						//delete event;
                    }

                    
					//printf("Finish tot: %d totnumreq %d\n", io_complete[i], totnumreq);
                    numevents = 0;
                } 
            }
			 if (io_complete[0] >= totnumreq & IOPrep_finish == 1) break;
        }

        for (j = 0; j < WT ; j++)
        {
           // totalBundles = totalBundles + bundle_count[j];
            totalIOs = totalIOs + io_complete[j];
        }

        printf("Completed %d IO Bundles in %Lf seconds, %Lf Bundle/S\n", totalIOs, ((long double)(core_time - base_time) / (long double) 1000000000), (long double) totalIOs / ((long double)(core_time - base_time) / (long double)1000000000) );
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

        while ((cycle_count - warm_start) / (1000 * max_speed) < 1000000)
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
		qlen=65536;
        unsigned long long wtime;  //Used to carry integer iotime in nanoseconds
        /* create async io context */
        if(io_setup(qlen, &ctx) != 0)
            errMsg("Context setup failed \n");

        /* "sync point" */
        running[tid] = 1;
        context[tid] = ctx;

        counter = 0;
        warmdone[tid] = 1;

        while(!start_threads)
            ;
		printf("worker start %d\n", start_threads);
		
        /* submit async IO requests in the private queue */
        int flag = 1;
		unsigned long long count =0;
        while(flag){

			if(count >= requestID | iov.empty() | starttime.empty()){
				if(printflag == 1) printf("empty\n");
			//if(*ioq[i].iocb_bunch== NULL){
				//while (pthread_cond_wait( &ItemAvailable, &lock) != 0) ; 
			}  
			else{
				pthread_mutex_lock ( lock);
				if(printflag == 1) printf("-------lock start \n");
				std::list<iocb*>::iterator it = iov.begin();
				std::list<unsigned long long>::iterator itt = starttime.begin();
				wtime = (*itt);
					
				AIORequest* req = new AIORequest((*it)->u.c.nbytes, (*itt));
                req->buffer = (*it)->u.c.buf;
				req->requestID = count; // use for request ID --Bingzhe
				
				while(likely(wtime > (core_time - base_time)))
				{
				}
				//printf("IO request#%llu	size: %d  offset %llu wtime %f itime %f iov %d Complete: %d \n", count, (*it)->u.c.nbytes, (*it)->u.c.offset, float(core_time - base_time)/1000000000, float(wtime)/1000000000, iov.size(), io_complete[0]);
				req->ts = core_time - base_time;
				(*it)->data = req; 
				if((submit_err = io_submit(ctx, 1, &(*it))) >= 0) {
					
				}
				else{
				//if((submit_err = syscall(__NR_io_submit, ctx, bundleSize, ioq[i].iocb_bunch)) < 0) {
						printf("Failed with counter = %ld on core %ld\n", counter, tid);
						printf("Returned value was: %d\n", submit_err);

						if(submit_err == -11) {
							fprintf(stderr, " Submit failed due to [EAGAIN]:lack resource\n");
							continue;
						}

						if(submit_err == -9) {
							fprintf(stderr, "Submit failed due to bad file descriptor.\n");
							thread_err[tid] = TE_BADFD;
							continue;
						}

						fprintf(stderr, "Submit failed, error = %d\n", submit_err);
						thread_err[tid] = TE_UNKNOWN;
						continue;
				}
				free(*it);
				iov.erase(it);
				
				count++;
				starttime.erase(itt);
				
				if(IOPrep_finish == 1 & count >= totnumreq) flag =0;
				pthread_mutex_unlock ( lock);
				counter += submit_err;

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

       
	    //while((rec = dump->next()))
        //{
		long long int count = 0;
		while(count <=totreq &  bool(rec = dump->next()) ){
			//rec = dump->next();
			count++;
	   
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

            int fd, lun;
            unsigned long long offset, size;
            unsigned long long slack_scale;
            unsigned long long iotime; //io submission time after scale in nanoseconds
            int cmp = replaycfg->rescale(rec, fd, slack_scale, offset, size, iotime, lun);

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

