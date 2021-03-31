#include <Distribution.hpp>
#include <algorithm>
#include <unordered_set>

Distribution* Distribution::createDistribution(Distribution::TYPE t,
                                               Buffer *buffer,
                                               int cacheline,
                                               size_t element_size,
                                               int seed,
                                               const size_t numSubpaths,
                                               const double zipfAlpha)
{
    Distribution *ptr = NULL;
    switch (t) {
        case UNIFORM:
            ptr = new UniformDistribution;
            break;;
        case ZIPF:
            ptr = new ZipfDistribution;
            break;;
        case LINEAR:
            ptr = new LinearDistribution;
            break;;
        case WUNI:
            ptr = new WeightedUniform;
            break;;
        case SUBPATH:
            ptr = new SubpathDistribution(numSubpaths, zipfAlpha);
            break;
        default:
            return NULL;
            break;;
    }
 
    ptr->setup(buffer,element_size,seed,cacheline);
    return ptr;
}

Distribution::Distribution() {
    this->buffer = NULL;
    this->entries = 0;
}

void Distribution::distribute() {
    this->doDistribute();
}

void Distribution::setup(Buffer *buffer, size_t esize, int seed, int cacheline) {
    this->buffer = buffer;
    //this->buffersize = size;
    this->element_size = esize;
    this->seed = seed;
    this->cacheline = cacheline;
    this->num_elements = buffer->Get_size()/ element_size;
    this->num_elements_in_cacheline = cacheline / element_size;
    this->entries = 0;
}

unsigned int Distribution::getEntries() {
    return this->entries;
}

unsigned int Distribution::getNumElements(){
    return this->num_elements;
}

double Distribution::getBufferUtilization() {
    return (double)entries / num_elements;
}

//Dump the contents of our buffer to fd
void Distribution::dumpBuffer(int fd) {
    if(write(fd,buffer->Get_buffer_pointer(),buffer->Get_allocated_size()) < 0) {
        perror("Dumping Buffer");
    }
}

void Distribution::dumpSequence(int fd) {
    std::vector<unsigned int>::iterator it = this->sequence.begin();
    for(;it!=sequence.end();it++) {
        if(write(fd,&(*it),sizeof(unsigned int)) < 0 ) {
            perror("Dumping Sequence");
        }
    }
}
///End Base Class

void ZipfDistribution::setParameters(double alpha, unsigned int items) {
    if(items == 0)
        return; //Need an error here
    this->alpha = alpha;
    this->num_items = items;
}

//Actual implementations of Distributions
//
void LinearDistribution::doDistribute() {
    int datalines = buffer->Get_size()/ cacheline;
    unsigned int offset = buffer->Get_start_address() - buffer->Get_buffer_pointer();
    this->sequence.clear();
    for ( int i = 0; i < datalines; i += 1 ) {
        *(int*)(buffer->Get_start_address() + i * cacheline) = ((i + 1) % datalines) * cacheline + offset;
        this->sequence.push_back((i+1)%datalines);
        entries += 1;
    }
}

void UniformDistribution::doDistribute() {
    // FIX: Pretty sure bufferlines doesn't account for start offset!
    int bufferlines = buffer->Get_size()/ cacheline;
    int num_line_elements = cacheline / element_size;
    int offset;
    int element = 0;
    int previdx = buffer->Get_start_address() - buffer->Get_buffer_pointer();
    bool done = false;
    this->sequence.clear();
    MTRand mr(seed);

    if(buffer->Is_large_buffer() && buffer->Get_slabs().size() > 0)
        return this->doHugeDistribution();

    /* New algorithm:
	 * Init the access array, such that we randomly sample a new place to go to,
	 * but each entry has `cacheline / sizeof(int)` entries. This is sort of
	 * analogous to buckets in hashing. When the front entry is full, find 
	 * one empty one in the next 15 entries. If all of them are full, 
	 * remember that this occured and draw a new number element.
	 * Then the actual memory traversal should simply follow the pointers again
	 * and all is swell
	 */
    // For as many elements can fit in the buffer:
    // 1) Imagine that buffer is split into sequential cache lines
    // 2) Pick a random cache line
    // 3) Find the first empty entry (i.e., contains zero) within cache line
    //   a) if empty entry found, use this entry as next in the sequence
    //   b) otherwise, end pointer chasing loop

    // Make sure to account for first element with index
    // `buffer->Get_start_address() - buffer->Get_buffer_pointer()`
    entries = 1;
    for ( int i = 1; i < num_elements; i += 1 ) {
        // Temporarily mark previdx location as taken in the instance that
        // same element is selected twice in row. This is guaranteed to be overwritten.
        *(element_size_t*)(buffer->Get_buffer_pointer() + previdx) = 1;

        int idx = mr.randInt() % bufferlines;
        offset = 0;
        while (true) {
            element = idx * cacheline + offset * element_size + (buffer->Get_start_address() - buffer->Get_buffer_pointer());
            if(0 != *(element_size_t*)(buffer->Get_buffer_pointer() + element)) {
                offset += 1;
            } else {
                break;
            }
            if(offset == num_line_elements - 1) {
                done=true;
                break;
            }
        }
        if(done) {
            break;
        } else {
            this->sequence.push_back(idx);
            *(element_size_t*)(buffer->Get_buffer_pointer() + previdx) = element;
            previdx = element;
            entries += 1;
        }
    }

    // Connect start to end
    *(element_size_t*)(buffer->Get_buffer_pointer() + previdx) = buffer->Get_start_address() - buffer->Get_buffer_pointer();
}

//Mainly duplicate from above.
//For now we assume that a huge page can only be
//2MB (1 << 21). This assumption is also hardcoded 
//inside Buffer.cc
//The main difference is that we want to distribute across multiple pages, 
//but treat them as though they were contiguous. So we need to do some fancy 
//address translation
void UniformDistribution::doHugeDistribution() {
    unsigned int bufferlines = buffer->Get_size()/ cacheline;
    unsigned int num_line_elements = cacheline / element_size;
    unsigned int num_lines_on_page = ((1 << 21) / (cacheline));
    int offset;
    int element = 0;
    unsigned long long  previdx = 0;
    bool done = false;
    MTRand mr(seed);
    this->sequence.clear();

    previdx = buffer->Get_start_address() - buffer->Get_buffer_pointer();

    assert((buffer->Get_slabs()).size() > 0);    //Ensure that the slabs have been setup

    // Make sure to account for first element with index
    // `buffer->Get_start_address() - buffer->Get_buffer_pointer()`
    entries = 1;
    for ( int i = 1; i < num_elements; i += 1 ) {
        // Temporarily mark previdx location as taken in the instance that
        // same element is selected twice in row. This is guaranteed to be overwritten.
        *(element_size_t*)(buffer->Get_buffer_pointer() + previdx) = 1;

        int idx = mr.randInt() % bufferlines;
        this->sequence.push_back(idx);
        unsigned int page = idx / num_lines_on_page; //Find the logical page idx mod (2 MB/64 b)
        unsigned int pageIDX = idx - page*num_lines_on_page; //Offset into page (line)
        unsigned long long virtPageStart = buffer->Get_slabs()[page];
        unsigned long long virtBufferStart = (unsigned long long)buffer->Get_buffer_pointer();

        offset = 0;
        while (true) {
            element = (virtPageStart-virtBufferStart) + pageIDX*cacheline + offset*element_size;
            if(0 != *(element_size_t*)(buffer->Get_buffer_pointer() + element)) {
                offset += 1;
            }else{
                break;
            }
            if(offset == num_line_elements - 1) {
                done=true;
                break;
            }
        }
        if(done) {
            break;
        } else {
            *(element_size_t*)(buffer->Get_buffer_pointer() + previdx) = element;
            previdx = element;
            entries += 1;
        }
    }
    *(element_size_t*)(buffer->Get_buffer_pointer() + previdx) = (buffer->Get_start_address()-buffer->Get_buffer_pointer());
}

/*
 * Zipf Distribution
 * Algorithm: Seed Zipf.
 * Distribute memory slots uniform random (create ranks)
 * Then apply Zipf distribution amongst the slots and create
 * linked list of access pattern. 
 */
void ZipfDistribution::doDistribute() {
    int offset;
    MTRand mr(seed);

    if(init_zipf(this->seed) < 0){
        std::cerr << "Seeding Zipf failed" << std::endl;
        exit(1);
    }

    //Randomize the slots
    //Doing Sattolo's Alrgorithm for an in-place shuffle of an array
    //This is the Fisher-Yates shuffler
    int *slots = new int[num_elements]; //This is the rank array
    int i=0;
    for(i=0;i<num_elements;i++) slots[i]=0;
    i = num_elements;
    while (i > 0) {
        i -= 1;
        int j = mr.randInt(i-1);
        int tmp = slots[j];
        slots[j]=slots[i];
        slots[i]=tmp;
    }

    //The element at idx in rank array is the bufferslot of rank idx
    //
    //Now insert the zipf pattern

    /*for ( int i = 0; i < num_elements; i += 1 ) {
        int idx = mr.randInt() % bufferlines;
        offset = 0;
        while (true) {
            element = idx * cacheline + offset*element_size;
            if(*(element_size_t*)(buffer + element)) {
                offset += 1;
            }else{
                break;
            }
            if(offset == num_line_elements - 1) { done=true; break; }
        }
        if(done) break;
        *(element_size_t*)(buffer + previdx) = element;
        previdx = element;
        entries += 1;
    }
    *(int*)(buffer + previdx) = 0;*/
}

void WeightedUniform::calculateCDF() {
    unsigned long long running_total = 0;

    for(int i=0;i<buffer->Get_size()/cacheline;i++){
        running_total += i;
        cdf.push_back(running_total);
    }

    std::sort(cdf.begin(),cdf.end());
    maxValinCDF = running_total;
}

/*
 * Draw a number from the weighted distribution
 * via binary search on the slots
 */
unsigned int WeightedUniform::getNextIDX(MTRand &mr) {
    unsigned res = 0;
    int imax = cdf.size();
    int imin = 0;
    unsigned long long key = mr.randExc()*maxValinCDF;

    //Binary search for slot
    int max,min,mid;
    max = cdf.size();
    min = 0;
    while(min<max) {
        mid = ((max + min)/2);
        if(key < cdf[mid])
            max = mid;
        else
            min = mid+1;
    }
    return mid;
}

void WeightedUniform::doDistribute() {
    int bufferlines = buffer->Get_size()/ cacheline;
    int num_line_elements = cacheline / element_size;
    int offset;
    int element = 0;
    int previdx = buffer->Get_start_address() - buffer->Get_buffer_pointer();
    bool done = false;
    this->sequence.clear();
    MTRand mr(seed);

    if(buffer->Is_large_buffer() && buffer->Get_slabs().size() > 0)
        assert(false);

    calculateCDF();

    /* New algorithm:
	 * Init the access array, such that we randomly sample a new place to go to,
	 * but each entry has cacheline / sizeof(int) entries. This is sort of 
	 * analogous to buckets in hashing. When the front entry is full, find 
	 * one empty one in the next 15 entries. If all of them are full, 
	 * remember that this occured and draw a new number element.
	 * Then the actual memory traversal should simply follow the pointers again
	 * and all is swell
	 */
    for ( int i = 0; i < num_elements; i += 1 ) {
        unsigned idx = getNextIDX(mr);
        assert(idx < bufferlines);
        offset = 0;
        while (true) {
            element = idx * cacheline + offset*element_size + (buffer->Get_start_address() - buffer->Get_buffer_pointer());
            if(*(element_size_t*)(buffer->Get_buffer_pointer() + element)) {
                offset += 1;
            }else{
                break;
            }
            if(offset == num_line_elements - 1) { done=true; break; }
        }
        if(done) break;
        this->sequence.push_back(idx);
        *(element_size_t*)(buffer->Get_buffer_pointer() + previdx) = element;
        previdx = element;
        entries += 1;
    }
    *(int*)(buffer->Get_buffer_pointer() + previdx) = buffer->Get_start_address() - buffer->Get_buffer_pointer();
}

template <typename URBG>
element_size_t SubpathDistribution::zipfSubpath(const size_t offsetDatalines, const size_t sizeDatalines,
                                                URBG& rand)
{
    // To avoid needing to alter zipf implementation to use rand, let's just
    // re-seed using number generated by rand (seed must be in range [1, INT_MAX]).
    const int seed = rand.randInt(INT_MAX - 1) + 1;
    assert(1 <= seed && seed <= INT_MAX);
    if (0 != init_zipf(seed)) {
        fprintf(stderr, "ERROR: zipf initialization failed\n");
        exit(-1);
    }

    std::vector<size_t> datalineCounts(sizeDatalines, 0); // for debugging
    const element_size_t bufferOffset = buffer->Get_start_address() - buffer->Get_buffer_pointer();
    const size_t numEntries = (sizeDatalines * cacheline) / element_size;
    const size_t numEntriesPerDataline = cacheline / element_size;
    element_size_t previdx = bufferOffset + offsetDatalines * cacheline;
    bool done = false;
    int offset;

    // Select a random rank for each of the datalines.
    // Ranking starts at 0!
    std::vector<size_t> rankToDatalineMap(sizeDatalines);
    for (size_t i = 0; i < sizeDatalines; i++) {
        rankToDatalineMap[i] = i;
    }
    std::shuffle(rankToDatalineMap.begin(), rankToDatalineMap.end(), rand);

    // This code is more or less copied from UniformDistribution::doDistribute()
    /* New algorithm:
     * Init the access array, such that we randomly sample a new place to go to,
     * but each entry has `cacheline / sizeof(int)` entries. This is sort of
     * analogous to buckets in hashing. When the front entry is full, find
     * one empty one in the next 15 entries. If all of them are full,
     * remember that this occured and draw a new number element.
     * Then the actual memory traversal should simply follow the pointers again
     * and all is swell
     */
    // For as many elements can fit in the buffer:
    // 1) Imagine that buffer is split into sequential cache lines
    // 2) Pick a random cache line
    // 3) Find the first empty entry (i.e., contains zero) within cache line
    //   a) if empty entry found, use this entry as next in the sequence
    //   b) otherwise, end pointer chasing loop

    // Make sure to account for first element with index
    // `buffer->Get_start_address() - buffer->Get_buffer_pointer()`
    entries += 1;
    for (size_t i = 1; i < num_elements; i += 1) {
        // Temporarily mark previdx location as taken in the instance that
        // same element is selected twice in row. This is guaranteed to be overwritten.
        *(element_size_t *)(buffer->Get_buffer_pointer() + previdx) = 1;

        // Choose dataline index using zipf distribution.
        // zipf() returns rank in range [1, size], so map that rank back to the
        // associated dataline.
        // NOTE: may want to pull out zipf() implementation. Doesn't seem to allow
        // changes to alpha and size after first call because of static vars.
        size_t idx = rankToDatalineMap[zipf(zipfAlpha, sizeDatalines) - 1];
        assert(0 <= idx && idx < sizeDatalines);
        datalineCounts[idx]++;
        idx += offsetDatalines;

        element_size_t element = 0;
        offset = 0;
        while (true) {
            element = bufferOffset + (idx * cacheline) + (offset * element_size);
            if (0 != *(element_size_t *)(buffer->Get_buffer_pointer() + element)) {
                offset += 1;
            } else {
                break;
            }
            if (offset == numEntriesPerDataline) {
                done = true;
                break;
            }
        }
        if (done) {
            break;
        } else {
            this->sequence.push_back(idx);
            *(element_size_t *)(buffer->Get_buffer_pointer() + previdx) = element;
            previdx = element;
            entries += 1;
        }
    }

    /*
    size_t pathCount = 0;
    for (auto v : datalineCounts) {
        pathCount += v;
    }
    printf("Zipf Subpath Distribution\n");
    printf(">> %u - %zu (%zu)\n", 0u, datalineCounts.size() - 1, pathCount);
    for (size_t i = 0; i < datalineCounts.size(); i++) {
        if (0 != datalineCounts[i]) {
            printf("%4zu: ", i);
            for (size_t j = 0; j < datalineCounts[i]; j++) {
                printf("*");
            }
            printf("\n");
        } else {
            printf("...\n");
            while (++i < datalineCounts.size() && 0 == datalineCounts[i]) {}
            if (i != datalineCounts.size()) {
                i -= 1;
            }
        }
    }
    fflush(stdout);
    */

    return previdx;
}

template <typename URBG>
element_size_t SubpathDistribution::uniformSubpath(const size_t offsetDatalines, const size_t sizeDatalines,
                                                   URBG& rand)
{
    const element_size_t bufferOffset = buffer->Get_start_address() - buffer->Get_buffer_pointer();
    const element_size_t offsetBytes = offsetDatalines * cacheline;

    // Fill container with invalid value to trigger later assertion if something went wrong
    for (size_t i = 0; i < sizeDatalines * cacheline; i++) {
        *(element_size_t*)(buffer->Get_buffer_pointer() + bufferOffset + offsetBytes + i) = (element_size_t)-1;
    }

    if (64 != cacheline) {
        fprintf(stderr, "Unexpected cacheline size: %d\n", cacheline);
        exit(-1);
    }

    // Write indices into unused region of datalines of the chunk. These indices
    // get shuffled and subsequently used to construct the path.
    // The objective is to make this memory efficient for when using large chunks.
    struct CacheLine {
        element_size_t pathIndex;
        // indexToShuffle corresponds to index of element in cachelineArray and
        // datalines starting from base of current chunk.
        element_size_t indexToShuffle;
        int8_t padding[64 - 2 * sizeof(element_size_t)];
    };
    CacheLine *cachelineArray = (CacheLine *)(buffer->Get_buffer_pointer() + bufferOffset + offsetBytes);

    // Write index N+1 to dataline N.
    // Reserve the index corresponding to the first data line in the chunk so we can
    // close loop or link to previous and next chunks.
    for (size_t i = 0; i < sizeDatalines - 1; i++) {
        cachelineArray[i].indexToShuffle = i + 1;
    }

    // Shuffle datalines in chunk
    std::shuffle(&cachelineArray[0], &cachelineArray[sizeDatalines - 1], rand);

    /*
    printf("Uniform Subpath Distribution\n");
    printf("Pre-Path Indexing\n");
    for (int i = 0; i < sizeDatalines; i++) {
        printf("path_idx=%d:shuffle_idx=%d\n", cachelineArray[i].pathIndex, cachelineArray[i].indexToShuffle);
    }
    */

    // Construct pointer chasing path in chunk from shuffled indices.
    // prevIdx corresponds to index of element in cachelineArray and datalines
    // starting from base of current chunk.
    element_size_t prevIdx = 0;
    this->sequence.emplace_back(offsetDatalines + prevIdx);
    entries += 1;

    for (size_t i = 0; i < sizeDatalines - 1; i++) {
        const element_size_t nextIdx = cachelineArray[i].indexToShuffle;
        assert(0 < nextIdx && nextIdx < sizeDatalines);
        cachelineArray[i].indexToShuffle = -1; // sanity check this shouldn't break anything

        assert((element_size_t)-1 == cachelineArray[prevIdx].pathIndex);
        cachelineArray[prevIdx].pathIndex = bufferOffset + offsetBytes + nextIdx * cacheline;
        //printf("%lu: %u <- %u\n", i, prevIdx, nextIdx);

        prevIdx = nextIdx;
        this->sequence.emplace_back(offsetDatalines + prevIdx);
        entries += 1;
    }

	/*
    printf("Post-Path Indexing\n");
    for (int i = 0; i < sizeDatalines; i++) {
        printf("%d:%d\n", cachelineArray[i].pathIndex, cachelineArray[i].indexToShuffle);
    }
	*/

    // Return buffer index of last, unwritten entry in path.
    // Caller should store next index to go at this location.
    return bufferOffset + offsetBytes + prevIdx * cacheline;
}

void SubpathDistribution::doDistribute() {
    if (buffer->Is_large_buffer() && buffer->Get_slabs().size() > 0) {
        fprintf(stderr, "ERROR: doHugeDistribution() not implemtned\n");
        exit(-1);
    }
    this->sequence.clear();
    entries = 0;
    MTRandURBG mr(seed);

    // Dataline is a "cacheline in the buffer
    const int numDatalines = buffer->Get_size() / cacheline;
    const int numDatalinesPerSubpath = numDatalines / this->numSubpaths;
    const element_size_t bufferOffset = buffer->Get_start_address() - buffer->Get_buffer_pointer();
    assert(numDatalines % this->numSubpaths == 0);

    // Split buffer into chunks where each chunk is associated with a subpath.
    // Create a path in each chunk and link chunk paths together using first entries.
    for (size_t subpathIdx = 0; subpathIdx < this->numSubpaths; subpathIdx++) {
        const size_t chunkOffsetDatalines = subpathIdx * numDatalinesPerSubpath;
        element_size_t lastEntryIndex;
        if (zipfAlpha <= 0) {
            lastEntryIndex = uniformSubpath(chunkOffsetDatalines, numDatalinesPerSubpath, mr);
        } else {
            lastEntryIndex = zipfSubpath(chunkOffsetDatalines, numDatalinesPerSubpath, mr);
        }

        // Connect last entry in path of this chunk to first entry in path of "next" chunk.
        // If current chunk is last chunk, then next chunk is the first chunk.
        const size_t nextChunkOffsetDatalines = ((subpathIdx + 1) % this->numSubpaths) * numDatalinesPerSubpath;
        *(element_size_t *)(buffer->Get_buffer_pointer() + lastEntryIndex) =
                bufferOffset + nextChunkOffsetDatalines * cacheline;
    }
}

