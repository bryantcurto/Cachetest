#!/bin/bash

source util.sh

L1_SIZE_SCALAR=1.5

quit_on_sigint

# NOTES:
# hypterthreading disabled for all experiments
# cores 0-7 on one socket and 8-15 on another
check_output_dir "$@"
output_dir="$1"

# Run the test with working set size that is ~1.5 * size of L1.
# TODO: Vary the sizes, checking hits/misses for L2 and L3
WORKING_SET_SIZE_KB=$(python3 -c "import math; print(int(math.ceil($L1D_SIZE_B * $L1_SIZE_SCALAR / 1000.)))")

echo "L1 DCache Size (B): $L1D_SIZE_B"
echo "Target Working Set Size (KB): $WORKING_SET_SIZE_KB"


# Build Cachetest initially
build_cachetest

# Run basec cachetests
TESTS=()

## Baseline test
TESTS+=("baseline -T 2")
TESTS+=("baseline_group_pinned -T 2 -C 0-1")
TESTS+=("baseline_indiv_pinned -T 2 -C 0-1 -I")

## Thread test
TESTS+=("threads -T 1,1")
TESTS+=("threads_group_pinned -T 1,1 -C 0.8")
TESTS+=("threads_indiv_pinned -T 1,1 -C 0.8 -I")

## Fibre test
TESTS+=("fibres8 -T 1,1 -F 8,8")
TESTS+=("fibres8_group_pinned -T 1,1 -C 0.8 -F 8,8")
TESTS+=("fibres8_indiv_pinned -T 1,1 -C 0.8 -I -F 8,8")

## Migration test
TESTS+=("migrate8 -T 1,1 -F 8,8 -M")
TESTS+=("migrate8_group_pinned -T 1,1 -C 0.8 -F 8,8 -M")
TESTS+=("migrate8_indiv_pinned -T 1,1 -C 0.8 -I -F 8,8 -M")


## Vary the number of fibres
for f in 16 32; do
	TESTS+=("migrate$f -T 1,1 -F $f,$f -M")
done

# RUN THE TESTS
run_cachetests "$output_dir"/basic "${TESTS[@]}"
exit



# Enable yielding
build_cachetest_with_flags -DFIBRE_YIELD

# Yielding-Fibre-Code Tests
## Create 16 fibres per subpath, no migration just loop over subpath, fibres yield at the end of subpath
## Understand the cost of yielding fibres.
tests=()

## Evaluate the cost of calling yield, but not actually switching to another fibre
#tests+=("yield_fibres8_baseline -T 1,1 -F 8,8")
#tests+=("yield_fibres8_baseline_group_pinned -T 1,1 -C 0.8 -F 8,8")
#tests+=("yield_fibres8_baseline_indiv_pinned -T 1,1 -C 0.8 -I -F 8,8")

## Evaluate the cost of calling yield and switching to another fibre
tests+=("yield_fibres8 -T 1,1 -F 8,8")
tests+=("yield_fibres8_group_pinned -T 1,1 -C 0.8 -F 8,8")
#tests+=("yield_fibres8_indiv_pinned -T 1,1 -C 0.8 -I -F 8,8")

### RUN THE TESTS
run_tests "$output_dir"/yield "${tests[@]}"



## Enable global yielding
build_cachetest_with_flags -DFIBRE_YIELD_GLOBAL

tests=()

## Evaluate the cost of calling yield and switching to another fibre
tests+=("yield_global_fibres8 -T 1,1 -F 8,8")
tests+=("yield_global_fibres8_group_pinned -T 1,1 -C 0.8 -F 8,8")
#tests+=("yield_global_fibres8_indiv_pinned -T 1,1 -C 0.8 -I -F 8,8")

### RUN THE TESTS
run_cachetests "$output_dir"/yield_global "${tests[@]}"



### Enable global yielding
build_cachetest_with_flags -DFIBRE_YIELD_FORCE

## Evaluate the cost of calling yield and switching to another fibre
TESTS="yield_force_fibres8 -T 1,1 -F 8,8\n"
TESTS="yield_force_fibres8_group_pinned -T 1,1 -C 0.8 -F 8,8\n"
#TESTS="yield_force_fibres8_indiv_pinned -T 1,1 -C 0.8 -I -F 8,8\n"

### RUN THE TESTS
run_tests


# Don't leave compiled cachetest being some weird variant
build_cachetest "$output_dir"/yield_force "${tests[@]}"




#sudo perf record -e cycles,L1-dcache-load-misses,LLC-load-misses,cache-misses -g ./src/cachetest -T 1,1 -F 8,8 -r 2 -d 1 49 > tests/output/{yield_,}fibres_duration_1_report.txt
#sudo perf report -g graph -i tests/output/yield_fibres_duration_1_report.data

#sudo perf mem -t load record -e 'cpu/mem-loads,ldlat=30,freq=2000/P' ./src/cachetest -T 1,1 -F 8,8 -r 2 -d 1 49
#sudo perf mem report
