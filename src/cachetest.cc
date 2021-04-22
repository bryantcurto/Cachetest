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
#include <cassert>

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
//#define FIBRE_YIELD_GLOBAL
//#define FIBRE_YIELD_FORCE

#define OPT_ARG_STRING "T:F:MC:Im:shab:d:f:l:r:c:e:o:AwZ:"

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

const size_t CACHE_LINE_SIZE = 64;

const size_t MAX_NUM_SUBPATHS = 10;

// 0 -> all threads/fibres start at first entry in subpath
// 1 -> threads/fibres get assigned first entries so that they're spread across subpath
const double THREAD_SUBPATH_DISTRIBUTION_DEC = 0.;

// Dirty trick to avoid performing a memory load to get base address of
// cluster array.
// Treat this as a chunk of untyped memory used to store the clusters.
struct alignas(CACHE_LINE_SIZE) PaddedCluster {
	Cluster cluster;
	int8_t _padding[sizeof(Cluster) % CACHE_LINE_SIZE];
};

/*
 * Structure for holding results of calling loop()
 * for a single thread/fibre
 */
struct LoopResult {
	unsigned long long accesscount;
	unsigned int index;
};

/*
 * Structure for holding values needed by fibreCallback.
 */
struct CallbackPack {
	LoopResult& res;
	size_t subpathIdx, fibreIdx;
	fibre_mutex_t *loggingMutex;
	fibre_barrier_t *testBarrier;
};

/*
 * Structure for storing main info about a subpath of
 * the pointer chasing path.
 */
struct SubpathInfo {
	SubpathInfo(element_size_t startIdx,
				element_size_t endIdx,
				size_t length)
	: startIdx(startIdx)
	, endIdx(endIdx)
	, length(length)
	{}

	element_size_t startIdx, endIdx;
	size_t length;
};

// Global variables for fibre migration microbenchmark
size_t numSubpaths;
std::vector<SubpathInfo> subpathInfo;
std::vector<size_t> subpathThreadCounts;
std::vector<std::vector<std::unordered_set<int> > > cpuIdSets;
int mainThreadCPUId = -1;
double zipfAlpha = 0.;
std::atomic<size_t> numAtBarrior(0);
volatile bool startExperiment = false;

std::vector<size_t> subpathFibreCounts;
bool migrate = false;
alignas(CACHE_LINE_SIZE) int8_t paddedClusters[MAX_NUM_SUBPATHS * sizeof(PaddedCluster)];

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
 * Read globally accessible pointer chasing path generated by SUBPATH
 * distribution and return starting index, ending index, and length
 * of each subpath in pointer chasing path.
 */
std::vector<SubpathInfo> collectPointerChasingSubpathInfo() {
	std::vector<SubpathInfo> subpathInfo;

	// We're assuming that it was generated using distribution SUBPATH.
	// This means that the buffer is evenly divided such that each
	// slice of the buffer contains a subpath. Further, the first entry
	// of each slice is the first entry of the subpath.
	const element_size_t bufferOffset = buffer->Get_start_address() - buffer->Get_buffer_pointer();
	const size_t subpathBufferLength = opt.dataset / numSubpaths;
	for (size_t i = 0; i < numSubpaths; i++) {
		const element_size_t startIdx = bufferOffset + i * subpathBufferLength;
		const element_size_t startIdxNextSubpath = bufferOffset + ((i + 1) % numSubpaths) * subpathBufferLength;

		element_size_t idx = startIdx, nextIdx;
		size_t length = 1;
		while (startIdxNextSubpath != (nextIdx = *(element_size_t *)(buffer->Get_buffer_pointer() + idx))) {
			length++;
			idx = nextIdx;
		}

		subpathInfo.emplace_back(startIdx, idx, length);
	}
	assert(subpathInfo.size() > 0);

	return subpathInfo;
}

/*
 * Print the path starting at the specified index.
 * Perform some additional sanity checks if subpath info
 * struct is specified.
 */
std::tuple<element_size_t, element_size_t> printPath(element_size_t idx,
													 const SubpathInfo *info = nullptr)
{
	element_size_t minIdx = std::numeric_limits<element_size_t>::max();
	element_size_t maxIdx = 0;
	const element_size_t startIdx = idx;
	size_t length = 0;

	std::cout << "Path:" << std::endl;
	do {
		minIdx = (minIdx < idx ? minIdx : idx);
		maxIdx = (maxIdx > idx ? maxIdx : idx);
		std::cout << idx << ", ";
		length += 1;
		idx = *(element_size_t *)(buffer->Get_buffer_pointer() + idx);
	} while (startIdx != idx);

	std::cout << std::endl
			  << "  length=" << length << " vs " << distr->getEntries() << std::endl
			  << "  range=[" << minIdx << ", " << maxIdx << "]" << std::endl;
	if (nullptr != info) {
		assert(length == info->length);
	}

	return std::make_tuple(minIdx, maxIdx);
}

/*
 * Alter the indices stored within the buffer so that the pointer
 * chasing path is split into subpaths. Each subpath is a new pointer
 * chasing path where the locations spanned by all subpaths match the
 * locations spanned by the original.
 * Info about the subpaths is stored in the global subpaths info structure.
 */
void splitPointerChasingPath() {
	// Split pointer chasing path assuming that it was generated using
	// distribution SUBPATH. This means that the buffer is evenly divided
	// such that each slice of the buffer contains a subpath. Further,
	// the first entry of each slice is the first entry of the subpath.

	// Make the last entry in each subpath point to the first entry
	for (const SubpathInfo& info : subpathInfo) {
		*(element_size_t*)(buffer->Get_buffer_pointer() + info.endIdx) = info.startIdx;
	}

#ifdef DEBUG
	// Log output pointer chaising paths and perform some sanity checing
	std::cout << "New paths:" << std::endl;
	element_size_t minIdx, maxIdx;
	std::tie(minIdx, maxIdx) = printPath(subpathInfo[0].startIdx, &subpathInfo[0]);

	element_size_t prevMinIdx = minIdx, prevMaxIdx = maxIdx;
	for (size_t i = 1; i < subpathInfo.size(); i++) {
		std::tie(minIdx, maxIdx) = printPath(subpathInfo[i].startIdx, &subpathInfo[i]);
		printf("%u %u\n", prevMaxIdx, minIdx);
		assert(prevMaxIdx < minIdx);

		prevMinIdx = minIdx;
		prevMaxIdx = maxIdx;
	}
#endif
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
 * Perform that pointer chasing by a single thread/fibre
 */
LoopResult loop(const size_t subpathIdx, const size_t threadIdx, const element_size_t startIdx, size_t stepsOffset) {
    register unsigned long long accesscount = 0;
    unsigned int dummy=0;

    register element_size_t index = startIdx;
    unsigned char* startAddr = buffer->Get_buffer_pointer();  //This is new, need to get the correct version from the Buffer

#if defined(FIBRE_YIELD) || defined(FIBRE_YIELD_GLOBAL) || defined(FIBRE_YIELD_FORCE)
	const register size_t steps = subpathInfo[subpathIdx].length;
	register size_t curStep = stepsOffset;
#endif

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
		printf("subpath:%zu tid:%zu: (%p) %u -> %u\n", subpathIdx, threadIdx, (element_size_t *)(startAddr + index), index, next);
#endif

        index = next;
        accesscount += 1;

#if defined(FIBRE_YIELD) || defined(FIBRE_YIELD_GLOBAL) || defined(FIBRE_YIELD_FORCE)
		curStep += 1;
		if (curStep >= steps) {
			curStep = 0;

#if defined(FIBRE_YIELD)
			StackContext::yield();
#elif defined(FIBRE_YIELD_GLOBAL)
			StackContext::yieldGlobal();
#elif defined(FIBRE_YIELD_FORCE)
			StackContext::forceYield();
#else
		((void(*)())nullptr)(); // crash
#endif
		}
#endif

        asm("#Exit");
    }

	return LoopResult{.accesscount=accesscount, .index=index};
}

/*
 * Perform that pointer chasing by a single thread/fibre
 */
LoopResult migratingLoop(const size_t subpathIdx, const size_t threadIdx, const element_size_t startIdx, size_t stepsOffset) {
	// Register variables have underscore (_) prefix
    register unsigned long long _accesscount = 0;
    unsigned int dummy=0;

    register element_size_t _index = startIdx;
    register unsigned char* _startAddr = buffer->Get_buffer_pointer();  //This is new, need to get the correct version from the Buffer

	// == v == migration code == v ==
	const register size_t _numSubpaths = numSubpaths;
	register size_t _subpathIdx = subpathIdx;
	register size_t _numSubpathSteps = subpathInfo[subpathIdx].length;
	register size_t _curStep = stepsOffset;
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
		printf("subpath:%zu tid:%zu: (%p) %u -> %u\n",
			   _subpathIdx, threadIdx, (element_size_t *)(_startAddr + _index), _index, next);
#endif

        _index = next;
        _accesscount += 1;
		_curStep += 1;

		// == v == migration code == v ==
		if (_curStep >= _numSubpathSteps) {
#ifdef DEBUG_RUN
			const size_t oldSubpathIdx = _subpathIdx;
#endif
			_curStep = 0;

			_subpathIdx += 1;
			if (_subpathIdx >= _numSubpaths) {
				_subpathIdx = 0;
			}
			_numSubpathSteps = subpathInfo[_subpathIdx].length;

#ifdef DEBUG_RUN
			printf("subpath:%zu tid:%zu migrating to subpath %zu!\n",
				   oldSubpathIdx, threadIdx, _subpathIdx);
#endif

			Cluster *cluster = &((PaddedCluster *)paddedClusters + _subpathIdx)->cluster;
			fibre_migrate(cluster);
			//printf("Fibre start=%u migrate to cluster %llu, index %llu\n", startIndex, subpathIdx, index);
		}
		// == ^ == migration code == ^ ==
        asm("#Exit");
    }

	return LoopResult{.accesscount=_accesscount, .index=_index};
}

std::vector<std::vector<LoopResult> > threadTest() {
	splitPointerChasingPath();

	// Create container for results
	std::vector<std::vector<LoopResult> > loopResults;
	for (size_t numThreads : subpathThreadCounts) {
		loopResults.emplace_back(std::vector<LoopResult>(numThreads));
	}

	std::vector<std::thread> threads;

	// Spawn threads and have them call callbacks
	std::mutex loggingMutex;
	for(size_t i = 0; i < numSubpaths; i++) {
		for(size_t j = 0; j < subpathThreadCounts[i]; j++) {
			threads.emplace_back([&loggingMutex](LoopResult& res, size_t subpathIdx, size_t threadIdx) {
				// Figure out index of subpath to start on
				double subpathDecimalOffset = (double)threadIdx / subpathThreadCounts[subpathIdx];
				subpathDecimalOffset *= THREAD_SUBPATH_DISTRIBUTION_DEC;
				size_t stepsOffset = (size_t)std::round(subpathInfo[subpathIdx].length * subpathDecimalOffset);
				element_size_t startIdx = subpathInfo[subpathIdx].startIdx;
				for (size_t k = 0; k < stepsOffset; k++) {
					startIdx = *(element_size_t*)(buffer->Get_buffer_pointer() + startIdx);
				}

				// Set cpu affinity
				if (cpuIdSets.size() > 0) {
					cpu_set_t cpuset = getCPUSet(subpathIdx, threadIdx);
					assert(0 == pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset));
				}

				loggingMutex.lock();
				printf("subpath %zu tid %zu starting on idx %d (offset=%zu)\n",
					   subpathIdx, threadIdx, startIdx, stepsOffset);
				loggingMutex.unlock();

				// Don't handle alarm signal when it goes off
				blockAlarmSignal();

				// Have all threads wait at barrior until main thread starts test
				numAtBarrior.fetch_add(1);
				while (!startExperiment);

				res = loop(subpathIdx, threadIdx, startIdx, stepsOffset);
			}, std::ref(loopResults[i][j]), i, j);
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

void *fibreCallback(void *p) {
	CallbackPack& pack = *(CallbackPack *)p;

	// Figure out index of subpath to start on
	double subpathDecimalOffset = (double)pack.fibreIdx / subpathFibreCounts[pack.subpathIdx];
	subpathDecimalOffset *= THREAD_SUBPATH_DISTRIBUTION_DEC;
	size_t stepsOffset = (size_t)std::round(subpathInfo[pack.subpathIdx].length * subpathDecimalOffset);
	element_size_t startIdx = subpathInfo[pack.subpathIdx].startIdx;
	for (size_t k = 0; k < stepsOffset; k++) {
		startIdx = *(element_size_t*)(buffer->Get_buffer_pointer() + startIdx);
	}

	// Don't handle alarm signal when it goes off.
	// This is done for the underlying worker thread since we can't
	// have one thread block signals for another thread.
	// TODO FIX: This doesn't guarantee that we get all of the worker fibres.
	blockAlarmSignal();

	assert(0 == fibre_mutex_lock(pack.loggingMutex));
	printf("cluster/subpath %zu fid %zu starting on idx %d (offset=%zu)\n",
		   pack.subpathIdx, pack.fibreIdx, startIdx, stepsOffset);
	assert(0 == fibre_mutex_unlock(pack.loggingMutex));

	// Have all fibres wait at barrior until main thread starts test
	numAtBarrior.fetch_add(1);
	assert(0 == fibre_barrier_wait(pack.testBarrier));

	if (migrate) {
		pack.res = migratingLoop(pack.subpathIdx, pack.fibreIdx, startIdx, stepsOffset);
	} else {
		pack.res = loop(pack.subpathIdx, pack.fibreIdx, startIdx, stepsOffset);
	}

	delete (CallbackPack *)p;
	return NULL;
}

std::vector<std::vector<LoopResult> > fibreTest() {
	// Create container for results
	std::vector<std::vector<LoopResult> > loopResults;
	for (size_t numFibres : subpathFibreCounts) {
		loopResults.emplace_back(std::vector<LoopResult>(numFibres));
	}

	// Acutally split the path as long as we're not performing migration.
	// If we're performing migration, fibre jumps to another cluster
	// after stepping through all of the entries of a subpath.
	if (!migrate) {
		splitPointerChasingPath();
	}
	FibreInit();

	// Create a cluster per subpath and assign worker
	// threads desired characteristics
	assert(numSubpaths <= MAX_NUM_SUBPATHS);
	new ((void*)&paddedClusters) PaddedCluster[numSubpaths];
	for (size_t i = 0; i < numSubpaths; i++) {
		const size_t numThreads = subpathThreadCounts[i];

		printf("Cluster %zu:\n", i);
		((PaddedCluster*)&paddedClusters + i)->cluster.addWorkers(numThreads);

		// Set cpu affinity for worker threads
		if (cpuIdSets.size() > 0) {
			// Get thread ids
			pthread_t* tids = new pthread_t[numThreads];
			assert(((PaddedCluster*)&paddedClusters + i)->cluster.getWorkerSysIDs(tids, numThreads) == numThreads);

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
		attr.cluster = &((PaddedCluster*)&paddedClusters + i)->cluster;

		// Spawn each fiber for this cluster
		for (size_t j = 0; j < numSubpathFibres; j++) {
			CallbackPack *pack = new CallbackPack{
				.res=loopResults[i][j],
				.subpathIdx=i,
				.fibreIdx=j,
				.loggingMutex=&loggingMutex,
				.testBarrier=&testBarrier
			};

			// Spawn the fiber and have it execute lambda
			assert(0 == fibre_create(&fibres[curFibreIdx], &attr, fibreCallback, (void *)pack));
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

    // Make sure working set size isn't larger than expected
    if (opt.huge && opt.dataset > (1 << 30)) {
        fprintf(stderr, "ERROR: Dataset size > 1GB not supported for use with hugepages.\n");
        exit(-1);
    }

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
	for (const SubpathInfo& info : subpathInfo) {
		*(element_size_t*)(buffer->Get_buffer_pointer() + info.startIdx) = -1;
	}
}

static void usage( const char* program ) {
    std::cerr << "usage: " << program << " [options] <dataset in KB> [output file] [error output file]" << std::endl;
    std::cerr << "options:" << std::endl;
	std::cerr << "-T CSV of threads per subpath (default: 1 thread on 1 subpath)" << std::endl;
	std::cerr << "-F CSV of fibres per subpath (disabled by default)" << std::endl;
	std::cerr << "-M flag indicating to perform thread migration across subpaths (disabled by default; specify with -F)" << std::endl;
	std::cerr << "-C CSV *or* range (M-N) of CPU ids to which all threads are pinned" << std::endl;
	std::cerr << "   Specify CPUs per threads of each subpath by specifying CSVs/ranges separated by a period (\".\")" << std::endl;
	std::cerr << "   Specify CPUs per thread of a subpath by specifying CSVs/ranges separated by an tilde (\"~\")" << std::endl;
	/*
	 * Some examples:
	 *   0-7: pin all cores to cores 0-7
	 *   0,2,4,6: pin all cores to cores 0,2,4,6
	 *   0-3.4-7: pin threads of first subpath to cores 0,1,2,3 and second subpath to 4,5,6,7
	 *   0-1~2,3.4-7: pin first thread of first subpath to 0,1, second thread of first subpath to 2,3
	 *                and second subpath to 4,5,6,7
	 */
	std::cerr << "   (default: no pinning)" << std::endl;
	std::cerr << "-I Enable individual pinning i.e., pin 1 thread to 1 CPU" << std::endl;
	std::cerr << "   This option overridden by specifying CPUs per thread of a subpath" << std::endl;
	std::cerr << "-m Pin main thread to specified core (default: not pinned)" << std::endl;
	std::cerr << "-Z Use Zipf distribution and specify alpha value" << std::endl;
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
            case 'Z': zipfAlpha = atof(optarg); break;
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
		auto parseCSVOrRange = [](const std::string& cpuIdsStr) -> std::vector<int> {
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

			return cpuIds;
		};

		// SubpathCPUs is a dirty way of storing a list of CPUs to which all threads of a subpath
		// are pinned, or a list of list to which each thread of each subpath is pinned.
		// If only we were using C++17, then we could just use std::variant
		using SubpathCPUs = std::pair<std::vector<int>, std::vector<std::vector<int> > >;
		std::vector<SubpathCPUs> cpuIdsList;

		// Parse input, getting CPUs to which threads of a subpath are pinned
		for (const std::string& cpuIdsStr : split(cpuIdsInput, ".")) {
			std::vector<int> cpuIdsOfSubpath;
			std::vector<std::vector<int> > cpuIdsOfSubpathThreads;

			if (std::string::npos != cpuIdsStr.find("~")) {
				// CPUs were specified per thread of the subpath, so collect CPUs
				// to which each thread should be pinned
				for (const std::string& str : split(cpuIdsStr, "~")) {
					cpuIdsOfSubpathThreads.emplace_back(parseCSVOrRange(str));
				}
			} else {
				// CPUs were specified for all threads in the subpath
				cpuIdsOfSubpath = parseCSVOrRange(cpuIdsStr);
			}

			// Perform some sanity checks
			assert((cpuIdsOfSubpath.size() > 0) ^ (cpuIdsOfSubpathThreads.size() > 0));
			assert((0 == cpuIdsOfSubpath.size()) ||
				   (std::accumulate(cpuIdsOfSubpathThreads.begin(), cpuIdsOfSubpathThreads.end(), (size_t)-1,
									[](size_t min, const std::vector<int>& set) -> size_t {
										return std::min(min, set.size());
									}) > 0));

			cpuIdsList.emplace_back(cpuIdsOfSubpath, cpuIdsOfSubpathThreads);
		}

		// Make sure that largest CPU index is < CPU_SETSIZE
		{
			int minCPUId, maxCPUId;
			std::tie(minCPUId, maxCPUId) =
					std::accumulate(cpuIdsList.begin(), cpuIdsList.end(), std::pair<int,int>(INT_MAX, INT_MIN),
					[](std::pair<int,int> pair, const SubpathCPUs& subpathCPUIds) -> std::pair<int,int> {
						const std::vector<int>& cpuIdsOfSubpath = subpathCPUIds.first;
						const std::vector<std::vector<int> >& cpuIdsOfSubpathThreads = subpathCPUIds.second;

						if (cpuIdsOfSubpath.size() > 0) {
							pair.first = std::min(pair.first, *std::min_element(cpuIdsOfSubpath.begin(), cpuIdsOfSubpath.end()));
							pair.second = std::max(pair.second, *std::max_element(cpuIdsOfSubpath.begin(), cpuIdsOfSubpath.end()));
						} else {
							pair.first =
									std::accumulate(cpuIdsOfSubpathThreads.begin(), cpuIdsOfSubpathThreads.end(), pair.first,
									[](int min, const std::vector<int>& cpuIds) {
										return std::accumulate(cpuIds.begin(), cpuIds.end(), min,
															   static_cast<const int&(*)(const int&, const int&)>(std::min));
									});
							pair.second =
									std::accumulate(cpuIdsOfSubpathThreads.begin(), cpuIdsOfSubpathThreads.end(), pair.second,
									[](int max, const std::vector<int>& cpuIds) {
										return std::accumulate(cpuIds.begin(), cpuIds.end(), max,
															   static_cast<const int&(*)(const int&, const int&)>(std::max));
									});
						}

						return pair;
					});
			if (mainThreadCPUId >= 0) {
				minCPUId = std::min(minCPUId, mainThreadCPUId);
				maxCPUId = std::max(maxCPUId, mainThreadCPUId);
			}
			assert(0 <= minCPUId && minCPUId <= maxCPUId && maxCPUId < CPU_SETSIZE);
		}

		// Make sure either one set of CPUs is specified, or
		// one set is specified per subpath
		assert(1 == cpuIdsList.size() || (numSubpaths == cpuIdsList.size()));

		// Duplicate the single set of specified CPUs for each subpath if only 1 specified
		if (1 == cpuIdsList.size() && numSubpaths > 1) {
			for (size_t i = 0; i < numSubpaths - 1; i++) {
				cpuIdsList.emplace_back(cpuIdsList[0]);
			}
		}

		// Create mapping from thread to CPU set
		for (size_t i = 0; i < numSubpaths; i++) {
			std::vector<std::unordered_set<int> > subpathThreadCPUIds;

			const std::vector<int>& cpuIdsOfSubpath = cpuIdsList[i].first;
			const std::vector<std::vector<int> >& cpuIdsOfSubpathThreads = cpuIdsList[i].second;
			const size_t numThreads = subpathThreadCounts[i];

			if (cpuIdsOfSubpathThreads.size() > 0) {
				// CPUs were specified per thread of the subpath
				assert(numThreads == cpuIdsOfSubpathThreads.size());
				for (const std::vector<int>& cpuIdsOfThread : cpuIdsOfSubpathThreads) {
					subpathThreadCPUIds.emplace_back(cpuIdsOfThread.begin(), cpuIdsOfThread.end());
					assert(cpuIdsOfThread.size() == subpathThreadCPUIds.back().size()); // otherwise, user specified duplicate cpuId id!
				}
			} else {
				// CPUs were specified for all threads in the subpath
				if (groupPinning) {
					// Each thread in subpath gets pinned to same set of CPUIds
					std::unordered_set<int> cpuIds(cpuIdsOfSubpath.begin(), cpuIdsOfSubpath.end());
					assert(cpuIds.size() == cpuIdsOfSubpath.size()); // Otherwise, they specified duplicate cpuId id!

					for (size_t i = 0; i < numThreads; i++) {
						subpathThreadCPUIds.emplace_back(cpuIds);
					}
				} else {
					// Each thread in each subpath gets pinned to specified core
					assert(numThreads == cpuIdsOfSubpath.size());
					for (int cpuId : cpuIdsOfSubpath) {
						subpathThreadCPUIds.emplace_back(std::unordered_set<int>({cpuId}));
					}
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
    std::cout << "Buffer at: "
              << std::hex << (element_size_t*)buffer->Get_buffer_pointer()
              << " - "
              << (element_size_t*)(buffer->Get_buffer_pointer() + buffersize)
              << std::endl << std::dec;
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
	if (zipfAlpha <= 0) {
		distr = Distribution::createDistribution(Distribution::SUBPATH,buffer,opt.cacheline,sizeof(element_size_t),opt.seed,
												 numSubpaths);
	} else {
		distr = Distribution::createDistribution(Distribution::SUBPATH,buffer,opt.cacheline,sizeof(element_size_t),opt.seed,
												 numSubpaths, zipfAlpha);
	}
    distr->distribute();

	subpathInfo = collectPointerChasingSubpathInfo();

#ifdef DEBUG
	// Log input pointer chaising path
	std::cout << "Base Path:" << std::endl;
	element_size_t pathMinIdx, pathMaxIdx;
	std::tie(pathMinIdx, pathMaxIdx) =
			printPath(buffer->Get_start_address() - buffer->Get_buffer_pointer());
#endif

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
