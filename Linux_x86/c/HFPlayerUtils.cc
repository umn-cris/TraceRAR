/* HFPlayer 3.0 Source Code
   © Regents of the University of Minnesota. 
   This software is licensed under GPL version 3.0 (https://www.gnu.org/licenses/gpl-3.0.en.html). */
   /**
 **    File:  HFPlayerUtils.cc
 **    Authors:  Sai Susarla, Weiping He, Jerry Fredin, Ibra Fall
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

#include <sys/types.h>
#include <fcntl.h>
#include "HFPlayerUtils.h"
int debug = 0;
void split(vector<string> &result, string str, string delim )
{
    result.clear();
    size_t prevpos = 0, pos;

    while (prevpos != string::npos)
    {
        pos = str.find(delim, prevpos);
        result.push_back(str.substr(prevpos, pos - prevpos));

        if (pos == string::npos)
            break;

        prevpos = pos + delim.size();
    }
}

void mystrtoupper(string& str)
{
    for(size_t j = 0; j < str.size(); ++j)
        str[j] = toupper(str[j]);
}

void mystrtolower(string& str)
{
    for (size_t j = 0; j < str.size(); ++j)
        str[j] = tolower(str[j]);
}

map<char, ULONG64>& DRecordData::set_multfactor()
{
    map<char, ULONG64>* mf = new map<char, ULONG64>();
    (*mf)['k'] = 1024;
    (*mf)['m'] = 1024 * 1024;
    (*mf)['g'] = 1024 * 1024 * 1024;
    (*mf)['s'] = 1000000;
    (*mf)['h'] = 60 * 60 * (ULONG64) 1000000;
    return *mf;
}

map<char, ULONG64>& DRecordData::mult_factor =
    DRecordData::set_multfactor();



int TraceReplayConfig::import(char* cfgfile)
{
    TextDataSet cfg; // general text data set containing multiple lun record
    int ret;

    if ((ret = cfg.open(cfgfile)) != 0)
        return ret;

    if (cfg.start() == false)
        return 1;

    DRecordData* rec; // each data record
    lunCfgs.clear();  // clear map content, mapping int->LUNCfg

    // iterating through multiple lun record
    while ((rec = cfg.next()))
    {
        int lun = (*rec)[LUN_FIELD].i;   // get LUN number for this record line
        string* file = (*rec)["file"].s; // corresponding record file for this LUN
        int fd;

        if (file->empty())
        {
            fprintf(stderr, "WARN: no file supplied for lun %d; skipping\n", lun);
            continue;
        }

        if ((fd = open(file->c_str(), O_RDWR | O_DIRECT)) < 0)
        {
            // open record file for this LUN
            fprintf(stderr, "WARN: cannot open %s; skipping lun %d\n",
                    file->c_str(), lun);
            continue;
        }

        if(debug)
        {
            fprintf(stderr, "Using %s as lun %d: Replay Parameters:\n%s\n", file->c_str(), lun,
                    rec->str(cfg.delim).c_str());
        }

        // setup lun configuration according to each lun record
        lunCfgs[lun].filename = file;
        lunCfgs[lun].fd = fd;
        lunCfgs[lun].lba_start = (*rec)["start_offset"].i / DISK_BLOCK_SIZE;
        lunCfgs[lun].nblks = (*rec)["range_nbytes"].i / DISK_BLOCK_SIZE;
        lunCfgs[lun].lba_shift = (*rec)["offset_shift"].i / DISK_BLOCK_SIZE;
        lunCfgs[lun].lba_scale = (*rec)["offset_scale"].d;
        lunCfgs[lun].iosize_scale = (*rec)["iosize_scale"].d;
        lunCfgs[lun].start_usecs = (*rec)["start_usecs"].d; //changed to double, which it should be
        //lunCfgs[lun].start_usecs = (*rec)["start_usecs"].i;
        lunCfgs[lun].num_usecs = (*rec)["num_usecs"].i;
        lunCfgs[lun].await_reply = (*rec)["await_reply"].i;
        lunCfgs[lun].slack_scale = (*rec)["slack_scale"].d;
#if 0
        fprintf(stderr, FMT_ULONG64 " " FMT_ULONG64 " " FMT_ULONG64 " " FMT_ULONG64 "\n",
                lunCfgs[lun].lba_start,
                lunCfgs[lun].nblks,
                lunCfgs[lun].start_usecs, lunCfgs[lun].num_usecs);
#endif /* 0 */
    }

    cfg.cleanup();
    return 0;
}

/** Need to use that function either here or in the HFPlayerUtils.h */
/** double TraceReplayConfig::frequence_speed()*/

int TraceReplayConfig::rescale(
    DRecordData* iorec,
    int& fd, /* out */
    unsigned long long& slack_scale, /* out */
    unsigned long long& offset, /* out */
    unsigned long long& size, /* out */
    unsigned long long& iotime, /* out in nanosecond */
	int& lun)
{
    lun = (*iorec)[LUN_FIELD].i;
    map<int, LUNCfg>::iterator it;
    it = lunCfgs.find(lun);

    if (it == lunCfgs.end())
        return -1;          //The lun is not specified in the cfg file, skip this recored

    LUNCfg& cfg = (*it).second;
    LBA lba = (*iorec)["lba"].i;
    ULONG64 nblks = (*iorec)["nblks"].i;
    double usecs = (*iorec)["elapsed_usecs"].d; // read useconds in double
    //ULONG64 usecs = (ULONG64) (*iorec)["elapsed_usecs"].d;

    //printf("usecs=%f,lba=%lld,nblks=%lld,start_usecs=%f,num_usecs=%f,lba_start=%lld,cfg.nblks=%lld\n", usecs, lba, nblks, cfg.start_usecs, cfg.num_usecs, cfg.lba_start, cfg.nblks); //AIX data type size

    // Set offset   (Moved to before filter tests on 6/30/2016 by CA)
    //offset = (((ULONG64) (lba * cfg.lba_scale) + cfg.lba_shift) * DISK_BLOCK_SIZE;
    //offset = (lba+31248000) * DISK_BLOCK_SIZE;69095424
    //offset = lba * DISK_BLOCK_SIZE;
    //else if (cfg.filename == '/dev/sda5') offset = (lba+40264960) * DISK_BLOCK_SIZE;
    //offset = lba/32;						// Commented this out 6/30/16 by CA D001  what is /32 for ?
    lba = (ULONG64)((lba / cfg.lba_scale) + cfg.lba_shift); // DO offset scaling ! (units are bytes) CA A001
    offset = lba * DISK_BLOCK_SIZE; // Convert from blocks to bytes
    
    // Set size     (Moved to before filter tests on 6/30/2016 by CA)
    //size = nblks * DISK_BLOCK_SIZE;				// Commented this out 6/30/16 by CA D001
    size = nblks * cfg.iosize_scale * DISK_BLOCK_SIZE; 		// DO size scaling ! (assuming units are bytes) CA A001
 
    //printf("op = lun: %d lba: %lld offset[bytes]: %lld nblks: %lld size: %lld dev1stLBA: %lld devblks: %lld iosizescaler: %f blksize: %d\n", lun, lba, offset, nblks, size, cfg.lba_start, cfg.nblks, cfg.iosize_scale, DISK_BLOCK_SIZE); //By CA
 
    // This is the first filter, check if trace record is in the time window specified in the cfg file.
    if ((usecs < cfg.start_usecs) || (usecs >= (cfg.start_usecs + cfg.num_usecs))){
		printf("-----------filter #1 num: %f %f cfg.start_usecs + cfg.num_usecs:%f\n", usecs, cfg.start_usecs, cfg.start_usecs + cfg.num_usecs); //By Bingzhe
        return -1;
	}
    
    // This is the second filter, check if the trace record is in the LBA range specified in the cfg file.
    if ((lba < cfg.lba_start) || (lba >= (cfg.lba_start + cfg.nblks))){
        printf("------------filter #2 lba: %lld %lld %lld \n", lba, cfg.lba_start, cfg.nblks); //By Bingzhe
        return -1;
	}

    fd = cfg.fd;
    // Now we know the trace record should be executed, scale it as needed (actually it was scaled prior to the validity test)
    // Always shift the start time by the config file value, so the replay starts at zero
    usecs = usecs - cfg.start_usecs;
    usecs = usecs * cfg.slack_scale;  //TODO need to change cfg file to have time_scale value
    iotime = (unsigned long long)(usecs * 1000) ; // convert to nanoseconds
    
    //printf("---------size %llu\n", size);
    //size = nblks;
    //printf("IOOOOOO # %d %d %d %d %d\n", size, DISK_BLOCK_SIZE, nblks);//iox->aio_nbytes);

    if (cfg.await_reply)
        slack_scale = cfg.slack_scale;
    else
        slack_scale = -1; /* send IO requests at full speed without waiting for replies */

    return 0;
}
