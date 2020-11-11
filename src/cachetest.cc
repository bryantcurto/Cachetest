#include <iostream>
#include <string>
#include <sstream>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <unistd.h>
#include <iomanip>
#include <sched.h>
#include <sys/time.h>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <numeric>
#include <utility>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fstream>
#include <pthread.h>
#include <Options.hpp>
#include <Distribution.hpp>
#include <Buffer.hpp>
#include "zipf.h"
#include <Prototypes.hpp>
#include <config.h>

#include <pthread.h>
#include <sched.h>

#include <Perf.hpp>
//Defs

#define INITIAL_LOOP_ITERATIONS 10000000
//#define DEBUG
//#define DEBUG_CREATE
//#define DEBUG_RUN
//#define DEBUG_ALARM

#ifdef WRITEBACK
void * WRITEBACK_VAR;
#endif

#define START_CODE 4711u
#define EXIT_CODE 4710u

#define OPT_ARG_STRING "T:C:IM:shab:d:f:l:r:c:e:o:Aw"

//Global Variables
unsigned int*       barrier             = NULL;
//static cpu_set_t*   cpuset              = NULL;
std::ostream*       output              = NULL;
std::ostream*       error_out           = NULL;
Distribution*       distr               = NULL;
Perf*               perf                = NULL;
std::vector<Event>* Measured_events     = NULL;

Buffer*				buffer;
Result_vector_t     Results;
std::stringstream   ss, ss1;
Options             opt;

std::vector<size_t> subpathThreadCounts;
size_t numSubpaths;
std::vector<std::vector<std::unordered_set<int> > > cpuIdSets;
int mainThreadCPUId = -1;
std::vector<element_size_t> subpathStartIndices;

//We should add a sanity check to ensure that the dataset is a multiple of 
//the element size, otherwise we might have half sized elements, which could force
//a buggy situtation

void error(std::string code)
{
    std::cerr << code << std::endl;
    exit(1);
}

std::vector<std::string> split(const std::string& str, const std::string& del) {
	std::vector<std::string> tokens;

	size_t offset = 0;
	auto it = str.find(del);
	while (std::string::npos != it) {
		tokens.emplace_back(str.substr(offset, it - offset));
		offset += tokens.back().size() + del.size();
		it = str.find(del, offset);
	}
	tokens.emplace_back(str.substr(offset, it));

	return tokens;
}

/* This function alters the indices stored within the buffer so that the
 * pointer chasing path is split into subpaths. Each subpath is a new
 * pointer chasing path, each pair has a difference in lenth <= 1, and
 * the locations spanned by the subpath matches the locations spanned
 * by the original.
 * Returns the starting index of each subpath
 */
std::vector<element_size_t> splitPointerChasingPath(const element_size_t numPaths) {
	auto printPath = [](element_size_t idx) {
		std::cout << "Path:" << std::endl;
		const element_size_t firstIdx = idx;
		size_t length = 0;
		do {
			std::cout << idx << ", ";
			length += 1;
			idx = *(element_size_t *)(buffer->Get_buffer_pointer() + idx);
		} while (firstIdx != idx);

		std::cout << std::endl << "  length=" << length << " vs " << distr->getEntries() << std::endl;
	};

#ifdef DEBUG
	// Log input pointer chaising path
	std::cout << "Input ";
	printPath(buffer->Get_start_address() - buffer->Get_buffer_pointer());
#endif

	// Try to evenly split entries in original pointer chaising path
	// amongst the new paths. Assign extras to earlier paths.
	const element_size_t minPathLength = distr->getEntries() / numPaths;
	if (0 == minPathLength) {
		std::cerr << "Can't split path of length " << distr->getEntries() << " into "
				  << numPaths << " parts" << std::endl;
		exit(-1);
	}
	element_size_t extras = distr->getEntries() - minPathLength * numPaths;

	// Figure out the addresses of the first and last entries of each path
	std::vector<std::pair<element_size_t, element_size_t> > pathStartEndIndices;
	element_size_t unusedEntryIdx = buffer->Get_start_address() - buffer->Get_buffer_pointer();
	for (element_size_t i = 0; i < numPaths; i++) {
		const element_size_t startIdx = unusedEntryIdx;
		for (element_size_t j = 1 /* account for starting entry */; j < minPathLength; ++j) {
			unusedEntryIdx = *(element_size_t *)(buffer->Get_buffer_pointer() + unusedEntryIdx);
		}
		if (extras > 0) {
			unusedEntryIdx = *(element_size_t *)(buffer->Get_buffer_pointer() + unusedEntryIdx);
			--extras;
		}
		const element_size_t endIdx = unusedEntryIdx;
		unusedEntryIdx = *(element_size_t *)(buffer->Get_buffer_pointer() + unusedEntryIdx);
		pathStartEndIndices.emplace_back(startIdx, endIdx);
	}

	// Make the last entry in each path refer to the first
	for (auto& pathPair : pathStartEndIndices) {
		*(element_size_t*)(buffer->Get_buffer_pointer() + pathPair.second) = pathPair.first;
	}

	// Collect the staring indices to return
	std::vector<element_size_t> pathStartIndices;
	for (auto& pathPair : pathStartEndIndices) {
		pathStartIndices.emplace_back(pathPair.first);
	}

#ifdef DEBUG
	// Log output pointer chaising paths
	std::cout << "New paths:" << std::endl;
	for (element_size_t idx : pathStartIndices) {
		printPath(idx);
	}
#endif

	return pathStartIndices;
}

struct LoopResult {
	long long accesscount;
	unsigned int index;
};

LoopResult loop(const unsigned int startIndex) {
    register long long accesscount = 0;
    register unsigned int stop = START_CODE;
    unsigned int dummy=0;

    register unsigned int index = startIndex;
    unsigned char* startAddr = buffer->Get_buffer_pointer();  //This is new, need to get the correct version from the Buffer

    if(Measured_events != NULL && !perf->start())
        error("ERROR: Can't start counters");

    asm ("#//Loop Starts here");
    for (;;) {
		if (index < 0) {
			// The test is done!
			// When the alarm goes off, subloops are intentionally broken
			// to notify the threads that the test is done. This is done
			// to avoid loading anything new into cache each loop to
			// check if test is done.
			break;
		}

        element_size_t next = *(element_size_t*)(startAddr + index);

        //for ( int j = 0; j < loopfactor; j += 1 ) dummy *= next;
#ifdef WRITEBACK
        *(element_size_t*)(startAddr + index + sizeof(int)) = dummy;
#endif
#ifdef DEBUG_RUN
        std::cout << index << ' ' << next << std::endl;
        std::cout << std::hex << (element_size_t*)(startAddr+index) << std::dec << std::endl;
#endif

        index = next;
        accesscount += 1;
        asm("#Exit");
    }

	return LoopResult{.accesscount=accesscount, .index=index};
}

int 
main( int argc, char* const argv[] ) {

	Initialize();

    Parse_options(argc, argv, opt);

    // Perform some setup
    if(!Configure_experiment())
        error("ERROR: Configuring");

    if(!Setup_buffer())
        error("ERROR: Setup_buffer");

    if(!Setup_perf())
        error("ERROR: Setup Perf");

    if(!Setup_distribution())
        error("ERROR: Distribution");

    if(!Synchronize_instances())
        error("ERROR: Synchronizing");

	// Spawn threads
	subpathStartIndices = splitPointerChasingPath(numSubpaths);
	std::vector<std::thread> threads;
	std::vector<std::vector<LoopResult> > loopResults;
	for (size_t threadCount : subpathThreadCounts) {
		loopResults.emplace_back(std::vector<LoopResult>(threadCount));
	}
	std::mutex mutex;

	std::atomic<bool> startExperiment(false);
	assert(startExperiment.is_lock_free());
	std::atomic<size_t> numAtBarrior(0);
	assert(numAtBarrior.is_lock_free());
	for(size_t i = 0; i < subpathThreadCounts.size(); i++) {
		for(size_t j = 0; j < subpathThreadCounts[i]; j++) {
			threads.emplace_back([i,j,&numAtBarrior,&startExperiment,&loopResults,&mutex]() {
				if (cpuIdSets.size() > 0) {
					// Create set of CPUs to which this thread is pinned
					cpu_set_t cpuset;
					CPU_ZERO(&cpuset);
					for (int cpu : cpuIdSets[i][j]) {
						CPU_SET(cpu, &cpuset);
					}

					// Pin thread to CPUs
					assert(0 == pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset));

					// Don't handle alarm signal when it goes off
					sigset_t set;
					sigemptyset(&set);
					sigaddset(&set, SIGALRM);
					assert(0 == pthread_sigmask(SIG_BLOCK, &set, NULL));
				}

#ifdef DEBUG
				std::string cpuIds = "";
				if (cpuIdSets.size() > 0) {
					cpuIds = std::accumulate(cpuIdSets[i][j].begin(), cpuIdSets[i][j].end(), std::string(""),
							[](std::string s, int cpuId){ return s + std::to_string(cpuId) + ","; });
					cpuIds = " (pinned to CPUs: " + cpuIds + ") ";
				}
				std::cout << "Thread " << pthread_self() << cpuIds
						  << " looping over path starting at " << subpathStartIndices[i] << std::endl;
#endif

				// Have all threads wait at barrior until main thread starts test
				numAtBarrior.fetch_add(1);
				while (!startExperiment.load());

				LoopResult res = loop(subpathStartIndices[i]);

				// I don't think we need to use a mutex here, but let's just be safe...
				const std::lock_guard<std::mutex> lock(mutex);
				loopResults[i][j] = res;
			});
		}
	}

	// Pin main thread to specified core
	if (mainThreadCPUId >= 0) {
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(mainThreadCPUId, &cpuset);
		assert(0 == pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset));
	}

	// Wait for all threads to be ready!
	while (numAtBarrior.load() != threads.size());

	// Officially start test timer!
    if(!Setup_timeout())
        error("ERROR: Alarm");

	// Let threads loose!
	startExperiment.store(true);

	// Wit for timer to go off and threads to finish
	for (std::thread& t : threads) {
		t.join();
	}

    //Get the Perf data
	std::vector<Result_t> results;
    if( Measured_events != NULL != 0 && !perf->stop() )
		error("ERROR: Stopping Counters failed");
	if( !perf->read_results(results) )
        error("ERROR: Reading results");

    //Result output
    *output
        << "dataset="  << (opt.dataset >> 10) << std::endl
        << "duration="  << opt.duration << std::endl
        << "cacheline="  << opt.cacheline << std::endl
        << "buffer factor="  << opt.bufferfactor << std::endl
        << "loop factor="  << opt.loopfactor << std::endl
        //<< ' ' << dummy
        << "entries/elems="  << distr->getEntries() << '/' << distr->getNumElements() << std::endl
        << "buffer util="  << std::setprecision(3) << ((double)distr->getBufferUtilization()) << std::endl;
	*output << std::endl;

	// Log per-thread access counts
	*output << "Per-thread results" << std::endl;
	for (size_t i = 0; i < loopResults.size(); i++) {
		for (size_t j = 0; j < loopResults[i].size(); j++) {
			*output << "Subpath " << i << "/Thread " << j << " access count="
					<< loopResults[i][j].accesscount << std::endl;
		}
	}
	*output << std::endl;

	// Log per-subpath access counts
	*output << "Per-subpath results" << std::endl;
	std::vector<size_t> subpathAccessCounts;
	for (size_t i = 0; i < loopResults.size(); i++) {
		subpathAccessCounts.emplace_back(
				std::accumulate(loopResults[i].begin(), loopResults[i].end(), 0,
								[](size_t count, LoopResult res) { return count + res.accesscount; }));
		*output << "subpath " << i << " access count=" << subpathAccessCounts.back() << std::endl;
	}
	*output << std::endl;

	// Log total access count
	*output << "total access count="
			<< std::accumulate(subpathAccessCounts.begin(), subpathAccessCounts.end(), 0)
			<< std::endl << std::endl;

    *output << "Perf" << std::endl
        << "  dataset="  << (opt.dataset >> 10) << std::endl
        << "  results=";
	for(std::vector<Result_t>::iterator it = results.begin(); it != results.end(); it++)
		*output << " " << *it;
    *output << std::endl;

    buffer->Dump_frames(ss1.str());

    return 0;
}

//Functions/Methods

static void start_handler( int )
{
}

static void alarm_handler( int ) {
	// Notify looping threads that test is done by breaking
	// the subloops (i.e., setting invalid indices)
	for (const element_size_t index : subpathStartIndices) {
		*(element_size_t*)(buffer->Get_buffer_pointer() + index) = -1;
	}
}

static void usage( const char* program ) {
    std::cerr << "usage: " << program << " [options] <dataset in KB> [output file] [error output file]" << std::endl;
    std::cerr << "options:" << std::endl;
	std::cerr << "-T CSV of threads per subpath (default: 1 thread on 1 subpath)" << std::endl;
	std::cerr << "-C CSV *or* range (M-N) of CPU ids to which all threads are pinned" << std::endl;
	std::cerr << "   Secify CPUs per subpath threads by specifying CSVs/ranges separated by a period (\".\") (default: no pinning)" << std::endl;
	std::cerr << "-I Enable individual pinning i.e., pin 1 thread pinned to 1 CPU (default: group pinning)" << std::endl;
	std::cerr << "-M Pin main thread to specified core (default: not pinned)" << std::endl;
	std::cerr << std::endl;
    std::cerr << "-a require physically contigous memory (default: no)" << std::endl;
    std::cerr << "-b buffer factor (default: 1)" << std::endl;
    std::cerr << "-d duration in seconds (default: 1)" << std::endl;
    std::cerr << "-f loop factor (default: 1)" << std::endl;
    std::cerr << "-l cache line size in bytes (default: 64)" << std::endl;
    std::cerr << "-r random seed (default: sequential access)" << std::endl;
    std::cerr << "-c CPU ID to pin the experiment to (default: 1, off main)" << std::endl;
    std::cerr << "-e Event list (rx0000yyxx or xx,uyy format)" << std::endl;
    std::cerr << "-h Use Huge pages (default: no) " << std::endl;
    std::cerr << "-o Offset into the start of the buffer" << std::endl;
    std::cerr << "-A Append output when using files" << std::endl;
    std::cerr << "-w Wait for signal before executing" << std::endl;
    std::cerr << "-s Dump the sequence pattern" << std::endl;
    exit(1);
}

/*
void pinExperiment(int cpu) {
    size_t size;

    //FIXME this shouldn't be hardcoded here?
    cpuset = CPU_ALLOC(100);
    if(cpuset == NULL) {
        perror("CPU_ALLOC");
        exit(EXIT_FAILURE);
    }

    size = CPU_ALLOC_SIZE(100);

    CPU_ZERO_S(size, cpuset);
    CPU_SET(cpu, cpuset);

    sched_setaffinity(getpid(),size,cpuset);
}
*/

bool
Parse_options( int argc, char * const *argv, Options &opt)
{

	std::string threadsInput = "";
	std::string cpuIdsInput = "";
	bool groupPinning = true;
    for (;;) {
        int option = getopt( argc, argv, OPT_ARG_STRING );
        if ( option < 0 ) break;
        switch(option) {
			case 'T': threadsInput = optarg; break;
			case 'C': cpuIdsInput = optarg; break;
			case 'I': groupPinning = false; break;
			case 'M': mainThreadCPUId = atoi(optarg); break;

            case 'b': opt.bufferfactor = atoi( optarg ); break;
            case 'd': opt.duration = atoi( optarg ); break;
            case 'f': opt.loopfactor = atoi( optarg ); break;
            case 'l': opt.cacheline = atoi( optarg ); break;
            case 'r': opt.seed = atoi( optarg ); break;
            case 'c': opt.cpu = atoi( optarg ); break;
            case 'e': Measured_events = perf->parseEvents( optarg ) ; break;
            case 'a': opt.cont = 1; break;
            case 'h': opt.huge = 1; break;
            case 'o': opt.bufferoffset = atoi( optarg ); break;
            case 'A': opt.append = true; break;
            case 'w': opt.wait = true; break;
            case 's': opt.dumpSeq = true; break;
            default: usage( argv[0] ); break;
        }
    }

    if ( argc < optind + 1 ) usage( argv[0] );
    opt.dataset = atoi(argv[optind]) << 10;

    std::stringstream oss;
    if ( argc >= optind + 2 ){
        oss << argv[optind+1] << "_" << opt.cpu << ".dat";
        if(opt.append)
            output = new std::ofstream(oss.str().c_str(),std::ios_base::out | std::ios_base::app);
        else
            output = new std::ofstream(oss.str().c_str());
    }//if
    else
        output = &std::cout;

    if ( argc >= optind + 3 ){
        oss << argv[optind+2] << "_" << opt.cpu << ".dat";
        if(opt.append)
            error_out = new std::ofstream(oss.str().c_str(),std::ios_base::out | std::ios_base::app);
        else
            error_out = new std::ofstream(oss.str().c_str());
    }//if
    else
        error_out = &std::cout;

	// Get the number of threads per subpath (and inherantly how many subpaths to create)
	if (0 == threadsInput.size()) {
		subpathThreadCounts.emplace_back(1);
	} else {
		auto tmp = split(threadsInput, ",");
		std::transform(tmp.begin(), tmp.end(), std::back_inserter(subpathThreadCounts),
					  [](const std::string& s) { return (size_t)std::stoull(s); });
	}
	numSubpaths = subpathThreadCounts.size();

	// Parse input string to determine which CPUs to pin to which threads
	if (cpuIdsInput.size() > 0) {
		std::vector<std::vector<int> > cpuIdsList;

		// Parse input
		for (const std::string& cpuIdsStr : split(cpuIdsInput, ".")) {
			std::vector<int> cpuIds;

			if (std::string::npos != cpuIdsStr.find("-")) {
				auto range = split(cpuIdsStr, "-");
				assert(range.size() == 2);
				for (int id = std::stoi(range[0]); id <= std::stoi(range[1]); id++) {
					cpuIds.emplace_back(id);
				}
			} else {
				for (const auto& id : split(cpuIdsStr, ",")) {
					cpuIds.emplace_back(std::stoi(id));
				}
			}
			assert(cpuIds.size() > 0);
			cpuIdsList.emplace_back(cpuIds);
		}

		// Make sure that largest CPU index is < CPU_SETSIZE
		{
			int maxCPUId = std::accumulate(cpuIdsList.begin(), cpuIdsList.end(), 0,
					[](int max, std::vector<int> cpuIds) {
						return std::max(max, *std::max_element(cpuIds.begin(), cpuIds.end()));
					});
			maxCPUId = std::max(maxCPUId, mainThreadCPUId);
			assert(maxCPUId < CPU_SETSIZE);
		}

		// Make sure either one set of CPUs is specified, or
		// one set is specified per subpath
		assert(1 == cpuIdsList.size() || (cpuIdsList.size() == cpuIdsList.size()));

		// Duplicate the single set of specified CPUs for each subpath
		if (1 == cpuIdsList.size() && numSubpaths > 1) {
			for (size_t i = 0; i < numSubpaths - 1; i++) {
				cpuIdsList.emplace_back(cpuIdsList[0]);
			}
		}

		// Create mapping from thread to CPU set
		for (size_t i = 0; i < numSubpaths; i++) {
			const std::vector<int>& subpathCPUIds = cpuIdsList[i];
			const size_t numThreads = subpathThreadCounts[i];
			std::vector<std::unordered_set<int> > subpathThreadCPUIds;

			if (groupPinning) {
				// Each thread in subpath gets pinned to same set of CPUIds
				std::unordered_set<int> cpuIds(subpathCPUIds.begin(), subpathCPUIds.end());
				assert(cpuIds.size() == subpathCPUIds.size()); // Otherwise, they specified duplicate cpuId id!

				for (size_t i = 0; i < numThreads; i++) {
					subpathThreadCPUIds.emplace_back(cpuIds);
				}
			} else {
				// Each thread in each subpath gets pinned to specified core
				assert(numThreads == subpathCPUIds.size());
				for (int cpuId : subpathCPUIds) {
					subpathThreadCPUIds.emplace_back(std::unordered_set<int>({cpuId}));
				}
			}
			cpuIdSets.emplace_back(subpathThreadCPUIds);
		}

#ifdef DEBUG
		// Log correspondence between threads and cores
		std::cout << "Specified CPU Pins:" << std::endl;
		for (auto subpathCPUIds : cpuIdSets) {
			std::cout << "> Subpath:" << std::endl;
			for (auto thrCPUIds : subpathCPUIds) {
				std::cout << ">> Thread CPU Ids: ";
				for (int cpuId : thrCPUIds) {
					std::cout << cpuId << " ";
				}
				std::cout << std::endl;
			}
		}
#endif
	}

    return true;
}

bool
Configure_experiment()
{
    if ( opt.duration < 1 ) {
        std::cerr << "ERROR: duration must be at least 1" << std::endl;
        exit(1);
    }
    if ( opt.loopfactor < 0 ) {
        std::cerr << "ERROR: loop factor must be at least 0" << std::endl;
        exit(1);
    }
    if ( opt.cacheline < 2 * sizeof(int) ) {
        std::cerr << "ERROR: cacheline must be at least " << 2 * sizeof(int) << std::endl;
        exit(1);
    }

	/*
    //Pin this process
    if(opt.cpu >= 0)
        pinExperiment(opt.cpu);
	*/
    return true;
}

bool
Setup_buffer()
{   
    //Allocate the buffer array
    //Need to be 100% sure that this is contiguous
    int buffersize = opt.dataset * opt.bufferfactor;
    if(opt.huge)
    	buffer = new Large_buffer( buffersize, opt.cont );
    else
    	buffer = new Small_buffer( buffersize, opt.cont );

    buffer->Set_buffer_offset(opt.bufferoffset);

    //unsigned char* buffer = new unsigned char[buffersize];
    //memset( buffer, 0, buffersize );
#ifdef DEBUG
    std::cout << "Buffer at: " << std::hex << (element_size_t*)buffer->getPtr() << " - " << (element_size_t*)(buffer->getPtr() + buffersize) << std::endl;
#endif

    return true;
}

bool
Setup_perf()
{
    if(Measured_events == NULL)
        return true;

    for(std::vector<Event>::iterator it = Measured_events->begin(); it != Measured_events->end(); it++)
    {
        if(!perf->addEvent( *it ))
            return false;
    }

    return true;
}

bool
Setup_distribution()
{

    // the code below sets up a list of cacheline-aligned and -sized blocks,
    // such that the first (sizeof int) bytes are used as a pointer to the
    // next index, while the next (sizeof int) bytes can be used for a dummy
    // write operation in the runtime loop below
    distr = NULL;
    if ( opt.seed >= 0 ) {
		std::cout << "Uniform distribution used" << std::endl;
        distr = Distribution::createDistribution(Distribution::UNIFORM,buffer,opt.cacheline,sizeof(element_size_t),opt.seed);
    }else{
		std::cerr << "LINEAR DISTRIBUTION should NOT be used for EXPERIMENT, specify non-negative seed!" << std::endl;
        distr = Distribution::createDistribution(Distribution::LINEAR,buffer,opt.cacheline,sizeof(element_size_t),opt.seed);
    }
    distr->distribute();

#ifdef DEBUG_CREATE
    int dumpfd = open("bufferdump.dat",O_CREAT | O_RDWR, S_IRWXU);
    distr->dumpBuffer(dumpfd);
#endif
    ss1 << "frameDump_" << opt.cpu << ".dat";
    ss << "sequencedump_" << opt.cpu << ".dat";
    unlink(ss.str().c_str());

    int seqdumpfd = open(ss.str().c_str(),O_CREAT | O_RDWR, S_IRWXU);
    if(opt.dumpSeq)
        distr->dumpSequence(seqdumpfd); 
    return true;
}

bool
Synchronize_instances()
{
    if(opt.wait) {
        //At this point I want to wait for the signal
        signal(SIGUSR1, start_handler);
        pause();
    }
    return true;
}

bool
Setup_timeout()
{
    //Setup the alarm signal handler
    if ( (signal( SIGALRM, alarm_handler ) < 0) || (alarm( opt.duration ) < 0) ) {
        std::cerr << "ERROR: signal or alarm" << std::endl;
        return false;
    }
    return true;
}

bool
Perform_experiment()
{
    return true;
}

bool
Dump_results()
{
    return true;
}

void
Initialize()
{
	perf = Perf::get_instance();
}
