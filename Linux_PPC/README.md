hfreplay 2.3
========

High Fidelity Workload Replay Engine

Authors: Alireza Haghdoost, Jerry Fredin, Ibra Fall,  Sai Susarla, Weiping He

Center for Research in Intelligent Storage (CRIS)
University of Minnesota
http://cris.cs.umn.edu

About
========

The goal of this study is to develop a scalable, timing accurate, replay engine that can faithfully preserve important characteristics of the original block workload trace. Developing a high fidelity replay tool is quite challenging for high performance storage systems. Because actual workload that hit the storage system is a mixed IO operation of multiple hosts which might have millions of IO operation per second and thousands of IO operation in-flight at a certain time. Therefore, faithfully replay a workload from as few as possible hosts is quite difficult which requires well understanding of IO subsystem operations and latency.  Although it is prevalent to replay traces at the level which the traces were captured, replay the trace at the controller level is not feasible because of the hardware limitation of storage system controller. On the other hand, replay the trace from the kernel space is not appropriate because of the portability issue. Therefore, we have developed a tool to replay a workload trace from user space and monitor kernel IO stack to maintain the replay fidelitys. 

This project has been sponsered by Center for Research in Intelligent Storage (CRIS) and is open for CRIS member companies. 


Setup
========
Setup involves regural linux make procedure. libaio library is required for builing the tool.


Run
========
A sample run shell script is provided in bin directory


Support
=======
Please contact with following persorns to get support
Alireza Haghdoost : alireza@cs.umn.edu
Jerry Fredin : jerry.fredin@netapp.com


License
=======
Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the specific language governing permissions and limitations under the License.


Private Source Code Repository :
https://github.com/gfredin/hfplayer
