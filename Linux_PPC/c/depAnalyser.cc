/* HFPlayer 3.0 Source Code
   © Regents of the University of Minnesota. 
   This software is licensed under GPL version 3.0 (https://www.gnu.org/licenses/gpl-3.0.en.html). 
*/
#include <iostream>                  // for std::cout
#include <utility>                   // for std::pair
#include <algorithm>                 // for std::for_each
#include <graphviz/gvc.h>
#include "depAnalyser.h"
#include "writeGraphviz.h"

using namespace std; 


// create a typedef for the Graph type
typedef map<double, Vertex, greater<double>> VertexSortedList;
// typedef map<double, Vertex > InFlightList;
typedef pair<double,Vertex> VSortedListPair;

void usage(){
    cout<< "Usage: dep_analyser <iotrace_dump1>" <<endl;
    exit(1);
}

bool is_reachable( Graph & depGraph, size_t parent, size_t child){
	
	vector<int> distances(boost::num_vertices(depGraph), 0);
	Vertex start = boost::vertex(parent,depGraph);
	Vertex target = boost::vertex(child,depGraph);
	Graph::adjacency_iterator neighbourIt, neighbourEnd, childIt, childEnd;
	boost::tie(neighbourIt, neighbourEnd) = boost::adjacent_vertices( start, depGraph );
	for(;neighbourIt != neighbourEnd ; neighbourIt++ ){
		Vertex neighbour = * neighbourIt;
		boost::tie(childIt, childEnd) = boost::adjacent_vertices(neighbour,depGraph);
		for(; childIt != childEnd ; childIt++){
			Vertex targetCandidate = *childIt;
			if(  targetCandidate == target ){
				return true;
			}
		}
	}
// 	boost::Visitor vis = boost::make_dfs_visitor(boost::record_distances(&distance[0], boost::on_tree_edge() ) );
	//boost::depth_first_search(depGraph,   boost::visitor(boost::make_dfs_visitor(boost::record_distances(&distances[0], boost::on_tree_edge()))).root_vertex( start) ); 
	
	/*
	
	boost::breadth_first_search(depGraph, start , 
								boost::visitor(boost::make_bfs_visitor(boost::record_distances(&distances[0], boost::on_tree_edge())))); 
	
	if(distances[child] != 0){
		// it is reachable, do NOT add the edge
// 		cout << "Cycle between "<< parent <<"->"<< child << endl;
		return true;
	}*/
	return false;
}

int main(int argc, char* argv[])
{
	if( argc < 2 )
		usage(); 
	
	DRecordData* rec;
    TextDataSet* trace = new TextDataSet();
	trace->open(argv[1]);
	assert( trace->start() );
	

	VertexSortedList vSortedList;
	pair<VertexSortedList::iterator,bool> mapRet;
	
	Graph depGraph; //Main dependency graph 
	Graph shadowGraph; //shadow dependency graph 
	vector<GraphVectorNode> recordsVector; //keep track of trace records in the memory
	double currCompTime = 0; 
	double currArrivTime = 0; 
	Vertex lastCompletedVertex = boost::graph_traits<Graph>::null_vertex();
	unsigned maxQueueDepth = 0 ;
	// main loop to detect dependency and build graph
	while( ( rec = trace->next() ) ){
		// creat and add new vertex in the dependency graph
		GraphVectorNode currRecord(rec); 
		size_t currVertexIndexinVector = recordsVector.size();
		recordsVector.push_back(currRecord);
		Vertex currVertex = boost::add_vertex(depGraph);
		depGraph[currVertex].name = to_string(currRecord.dump_offset);
		Vertex currShadowVertex = boost::add_vertex(shadowGraph);
		currArrivTime = (*rec)["elapsed_usecs"].d ;
		// establish edges for the added vertex
		auto mapItr = vSortedList.begin(); // read first inFlight Vertex with largest completion time
		unsigned inEdges = 0;
		if( currRecord.inflight_ios  > maxQueueDepth )  
			maxQueueDepth = currRecord.inflight_ios   ;
		for(; mapItr != vSortedList.end(); mapItr++){
			if( mapItr->first < currArrivTime){
				lastCompletedVertex =  mapItr->second;
				//add dependency edge if currVertex is not reachable from lastCompletedVertex
				if( ! is_reachable(shadowGraph,lastCompletedVertex,currShadowVertex) ){
					boost::add_edge(lastCompletedVertex,currVertex,depGraph);
					inEdges++;
				}
				boost::add_edge(lastCompletedVertex,currShadowVertex,shadowGraph);
				if( inEdges == maxQueueDepth + 1 )
					break; // we have find all possible parents. 
			}
		}
		cout<< currVertex <<","<< currRecord.inflight_ios + 1 <<","<<inEdges<<endl;
		// insert current vertex in the inflight requests list
		currCompTime = currArrivTime + (*rec)["latency_usecs"].d ;
		mapRet = vSortedList.insert( VSortedListPair( currCompTime ,currVertexIndexinVector ) );
		assert ( mapRet.second ); // check no duplicated node with the same completion time is there
	}
// 	writeGraphviz(recordsVector, depGraph );
	Graph_writer graph_writer; 
	Vertex_Writer<Graph> vertex_writer(depGraph); 
	
	/*
	// write unstructured graph to console
	cout << "\n-- graphviz output START --" << endl;
	boost::write_graphviz(cout, depGraph, vertex_writer, boost::default_writer(), graph_writer );
	cout << "\n-- graphviz output END --" << endl;
	*/
	
	ofstream outf("net.gv");
	
	boost::write_graphviz(outf, depGraph, vertex_writer, boost::default_writer(), graph_writer );
	outf.close();
	FILE * dotFile = fopen("net.gv", "r" ); 
	
	static GVC_t *gvc;
	/* set up a graphviz context - but only once even for multiple graphs */
	if (!gvc)
		gvc = gvContext();
	
	Agraph_t *graphViz;	
	graphViz = agopen("Dependency Graph", AGDIGRAPH);
	graphViz = agread(dotFile);
	fclose(dotFile);
	gvLayout(gvc, graphViz, "dot");
	/* Output in .dot format ( or "png" for .png, etc) */
	string outFileName(argv[1]);
	outFileName.append(".png");
	FILE *outPng = fopen( outFileName.c_str() , "w"); 
	gvRender(gvc, graphViz, "png", outPng);
	fclose(outPng); 
	gvFreeLayout(gvc, graphViz);
	agclose(graphViz);
}
