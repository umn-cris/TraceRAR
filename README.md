TraceRAR
========

Performance evaluation tool by replaying I/O block traces

Authors: Bingzhe Li, Farnaz Toussi, Clark Anderson, David Lilja, David Du, and Alireza Haghdoost

Center for Research in Intelligent Storage (CRIS)
University of Minnesota
http://cris.cs.umn.edu

About
========

The goal of this study is to develop a scalable, timing accurate, replay engine that can faithfully preserve important characteristics of the original block workload trace. Developing a high fidelity replay tool is quite challenging for high performance storage systems. Because actual workload that hit the storage system is a mixed IO operation of multiple hosts which might have millions of IO operation per second and thousands of IO operation in-flight at a certain time. Therefore, faithfully replay a workload from as few as possible hosts is quite difficult which requires well understanding of IO subsystem operations and latency.  Although it is prevalent to replay traces at the level which the traces were captured, replay the trace at the controller level is not feasible because of the hardware limitation of storage system controller. On the other hand, replay the trace from the kernel space is not appropriate because of the portability issue. Therefore, we have developed a tool to replay a workload trace from user space and monitor kernel IO stack to maintain the replay fidelitys. 

This project has been sponsered by Center for Research in Intelligent Storage (CRIS) and made open-source by CRIS member committee. 


Setup and Run:
========
Setup involves different architectures and operating systems.

For PowerPC:
1.	Install necessary libaio tool:
libaio-devel install
	yum install libaio-devel
2.	Running hfreplayer:
      1.	Check disk information:
      sudo fdisk –l -u
      2.	Find a applicable disk partition, e.g. /dev/sda1
      3.	Change the sampleConf-sda8.cvs file in /bin using the selected partition
      4.	Change the value in line 181 in file /xxx_PPC/c/HFPlayerUtils.cc to match the disk configuration
      5.	Change the value of field “RANGE_NBYTES.I” in /bin/ sampleConf-sda8.cvs to match selected partition range referring to slides page 11 in 20150714.pptx
      6.	In terminal, go to /xxx_PPC/, type “make”
      7.	In terminal, go to /xxx_PPC/bin, type”./run.sh” to run the tool.

For x86:
1. Check disk information: sudo fdisk -l
2. Find an applicable disk partition, e.g. /dev/sda8. Note that your disk partition should not have data since hfplayer might overwrite AND distroy filesystem on your partition.
3. Change the sampleConf-sda8.cvs file in bin/ using the selected partition
4. Change to suitable lun numbers in the sampleConf-sda8.cvs
[optional] Change the limitations of request sizes, timestamps and offset in the configuration file sampleConf-sda8.cvs
5. Go to bin/, type ./run.sh to run the tool.


Other things:
1.	Change the block layer queue depths(default value is 128)
sudo sh
echo 4096 >  /sys/block/sda/queue/nr_requests
2.	Change the device queue depths
sudo sh
echo 64 > /sys/block/sda/device/queue_depth
3.	I have commented controller load control in line 1070 in /c/hfcore.cc. If you want to open it, just uncomment it and make file again
4.	Line 394 in /c/hfcore.cc is to print issue time details for all requests. If want to check details, just uncomment it.
5.	Using # to comment other lines in file “sampleConf-sda8.cvs” if just want to test only one LUN
6.	In run.sh, the trace file names:.
      1.	total.csv is real trace provide by Farnaz.
      2.	total_small2.csv is first 32769 requests of total.csv




Support
=======
Please post your question in the github Issues page. 
https://github.com/umn-cris/TraceRAR/issues


Citation and related work
=========
This work is published in the 12th International Conference on Networking, Architecture, and Storage (NAS'2017) with title "TraceRAR: An I/O Performance Evaluation Tool for Replaying, Analyzing, and Regenerating Traces". (http://ieeexplore.ieee.org/abstract/document/8026880/)

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
Please also find the related work, hfplay (https://github.com/umn-cris/hfplayer) and the corresponding paper in the 15th USENIX Conference on File and Storage Technologies (FAST'17). https://www.usenix.org/conference/fast17/technical-sessions/presentation/haghdoost

License
=======
© Regents of the University of Minnesota. This software is licensed under GPL version 3.0 (https://www.gnu.org/licenses/gpl-3.0.en.html).
Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the specific language governing permissions and limitations under the License.


