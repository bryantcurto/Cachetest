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

#include <libfibre/fibre.h>
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

// Have fibres yield at the end of each subpath
//#define FIBRE_YIELD

#define OPT_ARG_STRING "T:F:MC:Im:shab:d:f:l:r:c:e:o:Aw"

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

const size_t MAX_NUM_SUBPATHS = 10;

size_t numSubpaths;
std::vector<size_t> subpathThreadCounts;
std::vector<std::vector<std::unordered_set<int> > > cpuIdSets;
int mainThreadCPUId = -1;
std::vector<element_size_t> subpathStartIndices;
std::atomic<size_t> numAtBarrior(0);
volatile bool startExperiment = false;

std::vector<size_t> subpathFibreCounts;
bool migrate = false;

// Dirty trick to avoid performing a memory load to get base address of
// cluster array.
// Treat this as a chunk of untyped memory used to store the clusters.
uint8_t clusters[MAX_NUM_SUBPATHS * sizeof(Cluster)];

//We should add a sanity check to ensure that the dataset is a multiple of 
//the element size, otherwise we might have half sized elements, which could force
//a buggy situtation

void error(std::string code)
{
    std::cerr << code << std::endl;
    exit(1);
}

/*
 * Split string by a delimiter very similar to Python2's string split().
 * e.g.: "1,2,3,4" --> vector{"1", "2", "3", "4"}
 */
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

/*
 * This function alters the indices stored within the buffer so that the
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
cpu_set_t getCPUSet(size_t subpathIdx, size_t threadIdx) {
	// Create set of CPUs to which this thread is pinned
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	for (int cpu : cpuIdSets[subpathIdx][threadIdx]) {
		CPU_SET(cpu, &cpuset);
	}
	return cpuset;
}

void logCPUAffinity(pthread_t tid, size_t subpathIdx) {
//#ifdef DEBUG
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	assert(0 == pthread_getaffinity_np(tid, sizeof(cpu_set_t), &cpuset));

	std::string cpuIds = "";
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &cpuset)) {
			cpuIds += std::to_string(i) + ",";
		}
	}
	std::cout << "Thread " << tid << " (pinned to CPUs: " << (cpuIds.size() > 0 ? cpuIds : "n/a")
			  << ") looping over subpath " << subpathIdx << std::endl;
//#endif
}

void blockAlarmSignal() {
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	assert(0 == pthread_sigmask(SIG_BLOCK, &set, NULL));
}

/*
 * Structure for holding results of calling loop()
 * for a single thread/fibre
 */
struct LoopResult {
	unsigned long long accesscount;
	unsigned int index;
};

/*
 * Perform that pointer chasing by a single thread/fibre
 */
LoopResult loop(const element_size_t startIndex) {
    register unsigned long long accesscount = 0;
    unsigned int dummy=0;

    register element_size_t index = startIndex;
    unsigned char* startAddr = buffer->Get_buffer_pointer();  //This is new, need to get the correct version from the Buffer

#ifdef FIBRE_YIELD
	register unsigned int steps = distr->getEntries() / numSubpaths;
	register unsigned int curStep = 0;
#endif // FIBRE_YIELD

    asm ("#//Loop Starts here");
    for (;;) {
		if (index == (element_size_t)-1) {
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

#ifdef FIBRE_YIELD
		curStep += 1;
		if (curStep == steps) {
			curStep = 0;
			// We make the assumption that the number of elements in the walk is divisible by the number of subpaths
			// We enforece it
			fibre_yield();
		}
#endif // FIBRE_YIELD

        asm("#Exit");
    }

	return LoopResult{.accesscount=accesscount, .index=index};
}

/*
 * Perform that pointer chasing by a single thread/fibre
 */
LoopResult migratingLoop(const element_size_t startIndex, const unsigned int subpathIdx) {
	// Register variables have underscore (_) prefix
    register unsigned long long _accesscount = 0;
    unsigned int dummy=0;

    register element_size_t _index = startIndex;
    register unsigned char* _startAddr = buffer->Get_buffer_pointer();  //This is new, need to get the correct version from the Buffer

	// == v == migration code == v ==
	const register size_t _numSubpaths = numSubpaths;
	register unsigned int _subpathIdx = subpathIdx;
	const register unsigned int _numSubpathSteps = distr->getEntries() / numSubpaths;
	register unsigned int _curStep = 0;
	// == ^ == migration code == ^ ==

    asm ("#//Loop Starts here");
    for (;;) {
		if (_index == (element_size_t)-1) {
			// The test is done!
			// When the alarm goes off, subloops are intentionally broken
			// to notify the threads that the test is done. This is done
			// to avoid loading anything new into cache each loop to
			// check if test is done.
			break;
		}

        element_size_t next = *(element_size_t*)(_startAddr + _index);

        //for ( int j = 0; j < loopfactor; j += 1 ) dummy *= next;
#ifdef WRITEBACK
        *(element_size_t*)(_startAddr + _index + sizeof(int)) = dummy;
#endif
#ifdef DEBUG_RUN
        std::cout << _index << ' ' << next << std::endl;
        std::cout << std::hex << (element_size_t*)(_startAddr + _index) << std::dec << std::endl;
#endif

        _index = next;
        _accesscount += 1;
		_curStep += 1;

		// == v == migration code == v ==
		if (_curStep >= _numSubpathSteps) {
			_curStep = 0;

			// We make the assumption that the number of elements in the walk is divisible by the number of subpaths
			// We enforece it
			_subpathIdx += 1;
			if (_subpathIdx >= _numSubpaths) {
				_subpathIdx = 0;
			}
			Cluster *cluster = (Cluster*)&clusters + _subpathIdx;
			fibre_migrate(cluster);
			//printf("Fibre start=%u migrate to cluster %llu, index %llu\n", startIndex, subpathIdx, index);
		}
		// == ^ == migration code == ^ ==
        asm("#Exit");
    }

	return LoopResult{.accesscount=_accesscount, .index=_index};
}

std::vector<std::vector<LoopResult> > threadTest() {
	// Create container for results
	std::vector<std::vector<LoopResult> > loopResults;
	for (size_t numThreads : subpathThreadCounts) {
		loopResults.emplace_back(std::vector<LoopResult>(numThreads));
	}

	subpathStartIndices = splitPointerChasingPath(numSubpaths);

	std::vector<std::thread> threads;

	// Spawn threads and have them call callbacks
	std::mutex loggingMutex;
	for(size_t i = 0; i < numSubpaths; i++) {
		for(size_t j = 0; j < subpathThreadCounts[i]; j++) {
			threads.emplace_back([&loggingMutex](LoopResult& res, size_t subpathIdx, size_t threadIdx, element_size_t startIdx) {
				// Set cpu affinity
				if (cpuIdSets.size() > 0) {
					cpu_set_t cpuset = getCPUSet(subpathIdx, threadIdx);
					assert(0 == pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset));
				}

				loggingMutex.lock();
				std::cout << "Thread idx=" << threadIdx << " on subpath " << subpathIdx << " starting index " << startIdx << std::endl;
				loggingMutex.unlock();

				// Don't handle alarm signal when it goes off
				blockAlarmSignal();


				// Have all threads wait at barrior until main thread starts test
				numAtBarrior.fetch_add(1);
				while (!startExperiment);

				res = loop(startIdx);
			}, std::ref(loopResults[i][j]), i, j, subpathStartIndices[i]);
		}
	}

	// Wait for all threads to be ready!
	while (numAtBarrior.load() != threads.size());

	if (cpuIdSets.size() > 0) {
		for (size_t i = 0, k = 0; i < numSubpaths; i++) {
			for (size_t j = 0; j < subpathThreadCounts[i]; j++, k++) {
				logCPUAffinity(threads[k].native_handle(), i);
			}
		}
	}

	// Officially start test timer!
	setupTimeout();

	// Have perf start recording events
	if (Measured_events != NULL && !perf->start()) {
		error("ERROR: Can't start counters");
	}

	// Let threads loose!
	startExperiment = true;

	// Wait for timer to go off and threads to finish
	for (std::thread& t : threads) {
		t.join();
	}

	// Have perf stop recording events
	if (Measured_events != NULL != 0 && !perf->stop()) {
		error("ERROR: Stopping Counters failed");
	}

	return loopResults;
}

std::vector<std::vector<LoopResult> > fibreTest() {
	// Create container for results
	std::vector<std::vector<LoopResult> > loopResults;
	for (size_t numFibres : subpathFibreCounts) {
		loopResults.emplace_back(std::vector<LoopResult>(numFibres));
	}

	// Acutally split the path as long as we're not performing migration.
	// If we're performing migration, fibre jumps to another cluster
	// after N steps where each N steps represents a subpath.
	// Calling function with subpath of 1 doesn't change path.
	if (migrate) {
		subpathStartIndices = splitPointerChasingPath(1);

		assert(0 == distr->getEntries() % numSubpaths);
		size_t subpathLength = distr->getEntries() / numSubpaths;
		//printf(">> Subpath length %zu\n", subpathLength);
		for (size_t i = 1; i < numSubpaths; i++) {
			// Compute starting location of next subpath by walking through path
			element_size_t start = subpathStartIndices.back();
			for (size_t j = 0; j < subpathLength; j++) {
				//printf(">> Skipping over %llu\n", start);
				start = *(element_size_t*)(buffer->Get_buffer_pointer() + start);
			}
			subpathStartIndices.emplace_back(start);
		}
	} else {
		subpathStartIndices = splitPointerChasingPath(numSubpaths);
	}

	FibreInit();

	// Create a cluster per subpath and assign worker
	// threads desired characteristics
	assert(numSubpaths <= MAX_NUM_SUBPATHS);
	new ((void*)&clusters) Cluster[numSubpaths];
	for (size_t i = 0; i < numSubpaths; i++) {
		const size_t numThreads = subpathThreadCounts[i];

		((Cluster*)&clusters + i)->addWorkers(numThreads);

		// Set cpu affinity for worker threads
		if (cpuIdSets.size() > 0) {
			// Get thread ids
			pthread_t* tids = new pthread_t[numThreads];
			assert(((Cluster*)&clusters + i)->getWorkerSysIDs(tids, numThreads) == numThreads);

			// Set affinity for each thread
			for (size_t j = 0; j < numThreads; j++) {
				cpu_set_t cpuset = getCPUSet(i, j);
				assert(0 == pthread_setaffinity_np(tids[j], sizeof(cpu_set_t), &cpuset));

				logCPUAffinity(tids[j], i);
			}

			delete[] tids;
		}
	}

	// Figure out how many fibres we'll be spawning
	size_t numTotalFibres = std::accumulate(subpathFibreCounts.begin(),
											subpathFibreCounts.end(),
											(size_t)0);
	fibre_t *fibres = new fibre_t[numTotalFibres];

	// Synchronize logging
	fibre_mutex_t loggingMutex;
	fibre_mutexattr_t loggingMutexAttr;
	assert(0 == fibre_mutex_init(&loggingMutex, &loggingMutexAttr));

	// Create barrior for fibres to wait on before test starts
	fibre_barrier_t testBarrier;
	assert(0 == fibre_barrier_init(&testBarrier, NULL,
								   numTotalFibres + 1 /* for main fibre to release the others */));

	// For each subpath, get the associated cluster and spawn
	// the associated fibers for the cluster
	size_t curFibreIdx = 0;
	for (size_t i = 0; i < numSubpaths; i++) {
		const size_t numSubpathFibres = subpathFibreCounts[i];
		if (0 == numSubpathFibres) {
			continue;
		}

		fibre_attr_t attr;
		attr.init();
		attr.cluster = ((Cluster*)&clusters + i);

		// Spawn each fiber for this cluster
		for (size_t j = 0; j < numSubpathFibres; j++) {
			struct CallbackPack {
				LoopResult& res;
				size_t subpathIdx, fibreIdx;
				size_t startIdx;
				fibre_mutex_t *loggingMutex;
				fibre_barrier_t *testBarrier;
			};
			CallbackPack *pack = new CallbackPack{
				.res=loopResults[i][j],
				.subpathIdx=i,
				.fibreIdx=j,
				.startIdx=subpathStartIndices[i],
				.loggingMutex=&loggingMutex,
				.testBarrier=&testBarrier
			};

			// Spawn the fiber and have it execute lambda
			assert(0 == fibre_create(&fibres[curFibreIdx], &attr,
				[](void *p) -> void * {
					CallbackPack& pack = *(CallbackPack *)p;

					// Don't handle alarm signal when it goes off.
					// This is done for the underlying worker thread since we can't
					// have one thread block signals for another thread.
					// TODO FIX: This doesn't guarantee that we get all of the worker fibres.
					blockAlarmSignal();

					assert(0 == fibre_mutex_lock(pack.loggingMutex));
					std::cout << "Fibre " << fibre_self() << " idx=" << pack.fibreIdx << " on cluster " << pack.subpathIdx << " starting index " << pack.startIdx << std::endl;
					assert(0 == fibre_mutex_unlock(pack.loggingMutex));

					// Have all fibres wait at barrior until main thread starts test
					numAtBarrior.fetch_add(1);
					assert(0 == fibre_barrier_wait(pack.testBarrier));

					if (migrate) {
						pack.res = migratingLoop(pack.startIdx, pack.subpathIdx);
					} else {
						pack.res = loop(pack.startIdx);
					}

					delete (CallbackPack *)p;
					return NULL;
				}, (void *)pack));
			curFibreIdx++;
		}
	}

	// Wait for all threads to be ready!
	while (numAtBarrior.load() != numTotalFibres);

	// Officially start test timer!
	setupTimeout();

	// Have perf start recording events
	if (Measured_events != NULL && !perf->start()) {
		error("ERROR: Can't start counters");
	}

	// Let threads loose!
	assert(PTHREAD_BARRIER_SERIAL_THREAD == fibre_barrier_wait(&testBarrier));

	// Wait until eveything is finished
	for (size_t i = 0; i < numTotalFibres; i++) {
		assert(0 == fibre_join(fibres[i], NULL /*non-null retval not implemented*/));
	}

	// Have perf stop recording events
	if (Measured_events != NULL != 0 && !perf->stop()) {
		error("ERROR: Stopping Counters failed");
	}

	// Cleanup
	assert(0 == fibre_barrier_destroy(&testBarrier));
	assert(0 == fibre_mutex_destroy(&loggingMutex));
	assert(0 == fibre_mutexattr_destroy(&loggingMutexAttr));

	delete[] fibres;
	//delete[] clusters;

	return loopResults;
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

	// Some sanity checks
	assert(0 == distr->getEntries() % numSubpaths);
	assert(numAtBarrior.is_lock_free());

	// Pin main thread to specified core
	if (mainThreadCPUId >= 0) {
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(mainThreadCPUId, &cpuset);
		assert(0 == pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset));
	}

	std::vector<std::vector<LoopResult> > loopResults;
	if (subpathFibreCounts.size() > 0) {
		loopResults = fibreTest();
	} else {
		loopResults = threadTest();
	}

    //Get the Perf data
	std::vector<Result_t> results;
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
	std::vector<unsigned long long> subpathAccessCounts;
	for (size_t i = 0; i < loopResults.size(); i++) {
		subpathAccessCounts.emplace_back(
				std::accumulate(loopResults[i].begin(), loopResults[i].end(), (unsigned long long)0,
								[](unsigned long long count, LoopResult res) { return count + res.accesscount; }));
		*output << "subpath " << i << " access count=" << subpathAccessCounts.back() << std::endl;
	}
	*output << std::endl;

	// Log total access count
	*output << "total access count="
			<< std::accumulate(subpathAccessCounts.begin(), subpathAccessCounts.end(), (unsigned long long)0)
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
	std::cerr << "-F CSV of fibres per subpath (disabled by default)" << std::endl;
	std::cerr << "-M flag indicating to perform thread migration across subpaths (disabled by default; specify with -F)" << std::endl;
	std::cerr << "-C CSV *or* range (M-N) of CPU ids to which all threads are pinned" << std::endl;
	std::cerr << "   Secify CPUs per subpath threads by specifying CSVs/ranges separated by a period (\".\") (default: no pinning)" << std::endl;
	std::cerr << "-I Enable individual pinning i.e., pin 1 thread pinned to 1 CPU (default: group pinning)" << std::endl;
	std::cerr << "-m Pin main thread to specified core (default: not pinned)" << std::endl;
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

bool
Parse_options( int argc, char * const *argv, Options &opt)
{

	std::string threadsInput = "";
	std::string cpuIdsInput = "";
	std::string fibresInput = "";
	bool groupPinning = true;
    for (;;) {
        int option = getopt( argc, argv, OPT_ARG_STRING );
        if ( option < 0 ) break;
        switch(option) {
			case 'T': threadsInput = optarg; break;
			case 'F': fibresInput = optarg; break;
			case 'M': migrate = true; break;
			case 'C': cpuIdsInput = optarg; break;
			case 'I': groupPinning = false; break;
			case 'm': mainThreadCPUId = atoi(optarg); break;

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

	// Get the number of fibres per subpath (this must agree with above
	if (fibresInput.size() > 0) {
		auto tmp = split(fibresInput, ",");
		std::transform(tmp.begin(), tmp.end(), std::back_inserter(subpathFibreCounts),
					  [](const std::string& s) { return (size_t)std::stoull(s); });
		assert(subpathFibreCounts.size() == numSubpaths);
	}

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

//#ifdef DEBUG
		// Log correspondence between threads of a subpath and cores
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
		if (subpathFibreCounts.size() > 0) {
			std::cout << "Subpath Fibre Counts (migrate=" << (migrate ? "Y" : "N") << "):" << std::endl;
			for (auto fibreCount : subpathFibreCounts) {
				std::cout << "> " << fibreCount << " fibres" << std::endl;
			}
		}
//#endif
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

void setupTimeout() {
    //Setup the alarm signal handler
	assert(SIG_ERR != signal(SIGALRM, alarm_handler));
	assert(alarm(opt.duration) >= 0);
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
