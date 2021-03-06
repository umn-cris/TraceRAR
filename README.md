TraceRAR
========

Performance evaluation tool about Replaying, Analyzing and Regenerating I/O block traces

Authors: Bingzhe Li, Farnaz Toussi, Clark Anderson, David Lilja, David Du, and Alireza Haghdoost

Center for Research in Intelligent Storage (CRIS)
University of Minnesota
http://cris.cs.umn.edu

About
========

The purpose of this project is using I/O workload analyzer/replayer tells customer how the application's storage performance will change on new system without running Customer’s actual application. The implementation of replayer tool is capable of replaying block I/O traces across different operating systems and architectures such as AIX and linux OSes and x86 and ppc architectures. In addition. the tool provides the performance reports after finishing replaying. The performance metrics include total replaying execution time, average I/O execution time, issue error, etc. Each operating system contains different functions, libraries and may also cause fidelity variance. The regenerator can build a longer trace according to the characterisitic of the original trace. 


Setup:
========
1. Copy, or git, directory structure to system under test.
 
Remaining steps are different for various processor architecture and operating system combinations.

For Linux on Power or x86:

2. Install necessary libaio tool:
```
libaio-devel install
yum install libaio-devel
```

3. Build code, if needed:
	A. Change the "DISK_BLOCK_SIZE" constant value in file ../c/IOLogDumpSchema.h to match the block length of the devices to be tested. The units are bytes. 
	B. From the directory containing the 'Makefile' file, /LINUX_PPC/ or /LINUX_X86/, run 'make'.   


For AIX on Power:

2. Install necessary Open Source Linux application packages for AIX
	A. They can be found here --> http://www-03.ibm.com/systems/power/software/aix/linux/
	B. They can be installed with the "rpm -ihv nnnnnn.rpm" command or with smit software installation menu options.
```
The list of required packages for AIX version 7.1 or later are:
	bash-4.3-17.aix5.1.ppc.rpm
	binutils-2.14-4.aix6.1.ppc.rpm
	gcc-4.8.3-1.aix7.1.ppc.rpm
	gcc-c++-4.8.3-1.aix7.1.ppc.rpm
	gcc-cpp-4.8.3-1.aix7.1.ppc.rpm
	gettext-0.10.40-8.aix5.2.ppc.rpm
	gmp-6.0.0a-1.aix5.1.ppc.rpm
	info-5.1-2.aix5.1.ppc.rpm
	libgcc-4.8.3-1.aix7.1.ppc.rpm
	libmpc-1.0.3-1.aix5.1.ppc.rpm
	libstdc++-4.8.3-1.aix7.1.ppc.rpm
	libstdc++-devel-4.8.3-1.aix7.1.ppc.rpm
	make_64-4.1-1.aix5.3.ppc.rpm
	mpfr-3.1.3-1.aix5.1.ppc.rpm
	zlib-1.2.4-2.aix5.1.ppc.rpm
	Use later versions if they exist.)
```
    
	
3. Build code, if needed:	
   	A. Change the "DISK_BLOCK_SIZE" constant value in file ../c/IOLogDumpSchema.h to match the block length of the devices to be tested. The units are bytes. 
	B. From the directory containing the 'Makefile' file, /AIX_PPC/, run 'gmake'.
		   

Run:
========
Running hfreplayer: 
1. Check disk information:
```
On Linux you can use: sudo fdisk -l -u
On AIX you can use: lsdev 
```	
2. Choose names of applicable disk partition(s) to excerise, e.g. /dev/sda1 for Linux or /dev/rhdisk12 for AIX.

   Note: The partitions under test should not have data that you need to keep. TraceRAR might overwrite AND destroy filesystems on your partition.
	
3. Edit the FILE.S parameter in last column of config file in /bin directory, such as sampleConf-sda8.cvs, to use the selected partition(s).
      
4. If desired, the following config file parameters can be altered as well: 
```
(Spatial Filtering) 
	A.	START_OFFSET.I (units  = bytes) prohibits execution of commands with LBAs < START_OFFSET/DISK_BLOCK_SIZE    
	B.	RANGE_NBYTES.I (units = bytes) prohibits exeecution of commands with LBAs >= (RANGE_NBYTES/DISK_BLOCK_SIZE + START_OFFSET/DISK_BLOCK_SIZE)
(Queue limiting)
	C.	NREQS.I is the maximum # of outstanding commands that can be sent to that LUN. (Errata: not always honored)
(Spatial Alterations) 
	D.	OFFSET_SHIFT.I (units = bytes) shifts up LBA values to avoid accessing LBAs < OFFSET_SHIFT/DISK_BLOCK_SIZE, yet still execute commands if they are within other filter limits.
	E.	OFFSET_SCALE.D reduces command LBAs by dividing the trace LBA by this decimal factor.   
	F.	IOSIZE_SCALE.D increases number of blocks transfered per request by multiplying the trace command op length by this decimal factor.
(Temporal Filtering)   
	G.	START_USECS.I (units  = microseconds) prohibits execution of commands with timestamps < START_USECS 
	H.	NUM_USECS.I (units  = microseconds) prohibits execution of commands with timestamps >= (START_USECS + NUM_USECS)
(Temporal Alterations)   
	I.	SLACK_SCALE.D alters the command inter-arrival time by multiplying the timestamps by this scalar. For example, 
    	a. to try to make a trace run faster than the rate at which it was created, use a value < 1.0. 
    	b. If trying to run the trace slower than at what it was created, use a value greater than 1.0. 
    	c. To try to run the trace at the same speed it was created, set the value to 1.0. 
    	Note: The word 'try' is used because the system being tested may not be capable of running the trace at the desired rate.    
	J.	AWAIT_REPLY.I, when set = 0, sends IO requests at full speed without waiting for replies. (Errata: not currently honored)
 ```
5. In terminal, go to /bin, type ./run.sh to run the tool.


Analyzer and regenerator options:
```
-rar <Turn on replayer, analyser and regenerator>
			1: Turn on replayer only
			2: Turn on analyser only
			3: Turn on replayer + analyser
			4: Turn on analyser + regnerator
			5: All on(replayer + analyser + regenerator)
```

Citation and related work
=========
This work is published in the 12th International Conference on Networking, Architecture, and Storage (NAS'2017) with the title "TraceRAR: An I/O Performance Evaluation Tool for Replaying, Analyzing, and Regenerating Traces". (http://ieeexplore.ieee.org/abstract/document/8026880/)

Please cite the work if you use the tool:
```
@inproceedings{li2017tracerar,
  title={TraceRAR: An I/O Performance Evaluation Tool for Replaying, Analyzing, and Regenerating Traces},
  author={Li, Bingzhe and Toussi, Farnaz and Anderson, Clark and Lilja, David J and Du, David HC},
  booktitle={Networking, Architecture, and Storage (NAS), 2017 International Conference on},
  pages={1--10},
  year={2017},
  organization={IEEE}
}
```
Please also find the related work, hfplayer (https://github.com/umn-cris/hfplayer) and the corresponding paper in the 15th USENIX Conference on File and Storage Technologies (FAST'17). https://www.usenix.org/conference/fast17/technical-sessions/presentation/haghdoost

Issues
=======
Please post your question in the github Issues page. 
https://github.com/umn-cris/TraceRAR/issues

License
=======
© Regents of the University of Minnesota. This software is licensed under GPL version 3.0 (https://www.gnu.org/licenses/gpl-3.0.en.html).
Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the specific language governing permissions and limitations under the License.

