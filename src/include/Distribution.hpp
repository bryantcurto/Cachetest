#include <stdlib.h>
#include "MersenneTwister.h"
#include "zipf.h"
#include "Buffer.hpp"
#include <algorithm>
#include <vector>

typedef unsigned int element_size_t;

class Distribution {
    private:
    protected:
        int cacheline;
        size_t element_size;
        int seed;
        Buffer *buffer;
        std::vector<unsigned int> sequence;

        unsigned int entries, num_elements, num_elements_in_cacheline;
    public:
        enum TYPE {UNIFORM, ZIPF, LINEAR, WUNI, SUBPATH};
        
        //Factory method
        static Distribution* createDistribution(TYPE t, Buffer* buffer, int cacheline, size_t element_size, int seed,
                                                // This is dumb, but it's easy
                                                const size_t numSubpaths = 1, const double zipfAlpha = 0);

        void setup(Buffer*,size_t,int,int cacheline);
        void distribute();  //Public start point

        Distribution();
        virtual ~Distribution() {}

        virtual void doDistribute() = 0;    //Implement this
        unsigned int getEntries();  //Retrieve the # of entries created
        unsigned int getNumElements();  //Retrieve the # of entries created
        double getBufferUtilization();  //Retrieve the # of entries created
        void dumpBuffer(int dumpfd);    //Dump the data to the given file
        void dumpSequence(int dumpfd);  //Dump the generated sequence
};

class UniformDistribution : public Distribution {
    private:
        void doHugeDistribution();
    public:
        virtual void doDistribute();
};

class WeightedUniform : public Distribution {
    public:
        WeightedUniform() { weightFactor = 1;}
        virtual void doDistribute();
    private:
        unsigned int weightFactor; //Two connected items are 1 unit of probability apart
        std::vector<unsigned long long> cdf;
        unsigned long long maxValinCDF;

        void calculateCDF();    //Precomputes the distribution probabilities for WUNI
        unsigned int getNextIDX(MTRand &);
};

class ZipfDistribution: public Distribution {
    private:
        double alpha;
        unsigned int num_items;
    public:
        ZipfDistribution() { alpha = 0; num_items = 0;}
        void setParameters(double alpha, unsigned int num_items);
        virtual void doDistribute();
};

class LinearDistribution: public Distribution {
    public:
        virtual void doDistribute();
};

class SubpathDistribution : public Distribution {
    private:
        const size_t numSubpaths;
        const double zipfAlpha;
        // Extension of MTRand to satisfy requirements of UniformRandomBitGenerator.
        // The constants were taken from MTRand source.
        struct MTRandURBG : public MTRand {
            using MTRand::MTRand;
            using result_type = uint32;

            constexpr result_type min() {
                return 0;
            }

            constexpr result_type max() {
                return (uint32_t)-1;
            }

            result_type operator()() {
                return randInt(max());
            }
        };

        template <typename URBG>
        element_size_t uniformSubpath(const size_t offsetDatalines, const size_t sizeDatalines,
                                      URBG& rand);
        template <typename URBG>
        element_size_t zipfSubpath(const size_t offsetDatalines, const size_t sizeDatalines,
                                   URBG& rand);

    public:
        // Zipf alpha must be > 0 to use zipf
        SubpathDistribution(const size_t numSubpaths,
                            const double zipfAlpha = 0)
        : numSubpaths(numSubpaths)
        , zipfAlpha(zipfAlpha)
        {}

        virtual void doDistribute();
};

