TraceRAR
========

Performance evaluation tool by replaying I/O block traces

Authors: Bingzhe Li, Farnaz Toussi, Clark Anderson, David Lilja, David Du, and Alireza Haghdoost

Center for Research in Intelligent Storage (CRIS)
University of Minnesota
http://cris.cs.umn.edu

About
========

The purpose of this project is using I/O workload analyzer/replayer tells customer how the application's storage performance will change on new system without running Customer’s actual application. The implementation of replayer tool is capable of replaying block I/O traces across different operating systems and architectures such as AIX and linux OSes and x86 and ppc architectures. In addition. the tool provides the performance reports after finishing replaying. The performance metrics include total replaying execution time, average I/O execution time, issue error, etc. Each operating system contains different functions, libraries and may also cause fidelity variance. 


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
      5.	Change the value of field “RANGE_NBYTES.I” in /bin/ sampleConf-sda8.cvs to match selected partition range
      6.	In terminal, go to /xxx_PPC/, type “make”
      7.	In terminal, go to /xxx_PPC/bin, type”./run.sh” to run the tool.

For x86:
1. Check disk information: sudo fdisk -l
2. Find an applicable disk partition, e.g. /dev/sda8. Note that your disk partition should not have data since hfplayer might overwrite AND distroy filesystem on your partition.
3. Change the sampleConf-sda8.cvs file in bin/ using the selected partition
4. Change to suitable lun numbers in the sampleConf-sda8.cvs
[optional] Change the limitations of request sizes, timestamps and offset in the configuration file sampleConf-sda8.cvs
5. Go to bin/, type ./run.sh to run the tool.


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


