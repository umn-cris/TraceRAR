#ifndef WRITEGRAPH_H
#define WRITEGRAPH_H
#include "depAnalyser.h"


class GraphVectorNode {
public:
	unsigned dump_offset;
	double elapsed_usecs;
	unsigned short inflight_ios;
	unsigned seqid;
	unsigned short lun_ssid;
	unsigned short op;
	unsigned short phase;
	unsigned long long lba;
	unsigned nblks;
	unsigned short host_id;
	unsigned short host_lun;
	double latency_usecs;
// #DUMP_OFFSET.I,ELAPSED_USECS.D,ELAPSED_TICKS.I,CMD.S,INFLIGHT_IOS.I,TS.I,SEQID.I,LUN_SSID.I,OP.I,PHASE.I,LBA.I,NBLKS.I,LATENCY_TICKS.I,HOST_ID.I,HOST_LUN.I,LATENCY_USECS.D
	GraphVectorNode(){
		dump_offset=0;
		elapsed_usecs=0;
		inflight_ios=0;
		seqid=0;
		lun_ssid=0;
		op=0;
		phase=0;
		lba=0;
		nblks=0;
		host_id=0;
		host_lun=0;
		latency_usecs=0;
	}
	
	GraphVectorNode(DRecordData * rec){
		dump_offset=(*rec)["dump_offset"].i;
		elapsed_usecs=(*rec)["elapsed_usecs"].d;
		inflight_ios=(*rec)["inflight_ios"].i;
		seqid=(*rec)["seqid"].i;
		lun_ssid=(*rec)["lun_ssid"].i;
		op=(*rec)["op"].i;
		phase=(*rec)["phase"].i;
		lba=(*rec)["lba"].i;
		nblks=(*rec)["nblks"].i;
		host_id=(*rec)["host_id"].i;
		host_lun=(*rec)["host_lun"].i;
		latency_usecs=(*rec)["latency_usecs"].d;
	}
	
	bool operator==(const GraphVectorNode &other) const{
		if( this->seqid == other.seqid)
			return true;
		else
			return false;
	}
};
void writeGraphviz(std::vector<GraphVectorNode>& recordsVector,Graph & depGraph);
/*
bool GraphVectorNode::operator==(const GraphVectorNode& other) const
{
	if( this->seqid == other.seqid)
		return true;
	else
		return false;
}
*/
#endif
