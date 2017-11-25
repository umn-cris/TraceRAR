For PowerPC:
1. Install necessary libaio tool:
libaio-devel install
yum install libaio-devel
2. Running hfreplayer:
1. Check disk information:
sudo fdisk -l
2. Find a applicable disk partition, e.g. /dev/sda1
3. Change the sampleConf-sda8.cvs file in /bin using the selected partition
4. In terminal, go to \hfplayer2.3_POSIX_AIX\, compile the tool: make or gmake
5. \hfplayer2.3_POSIX_AIX\bin, type¡±./run.sh¡± to run the tool.

You can change the replayed trace name in /bin/run.sh

The trace file "total20f" has 200,000 requests. 