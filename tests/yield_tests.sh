#!/bin/bash

source util.sh

L1_SIZE_SCALAR=1.5

quit_on_sigint

# NOTES:
# hypterthreading disabled for all experiments
# cores 0-7 on one socket and 8-15 on another
output_dir="$(parse_output_dir "$@")"


# Run the test with working set size that is ~1.5 * size of L1.
# TODO: Vary the sizes, checking hits/misses for L2 and L3
WORKING_SET_SIZE_KB=$(python3 -c "import math; print(int(math.ceil($L1D_SIZE_B * $L1_SIZE_SCALAR / 1000.)))")

echo "L1 DCache Size (B): $L1D_SIZE_B"
echo "Target Working Set Size (KB): $WORKING_SET_SIZE_KB"


# Enable yielding
build_cachetest_with_flags -DFIBRE_YIELD


# Yielding-Fibre-Code Tests
## Create 16 fibres per subpath, no migration just loop over subpath, fibres yield at the end of subpath
## Understand the cost of yielding fibres.
tests=()

### Evaluate the cost of calling yield, but not actually switching to another fibre
#tests="$tests""yield_fibres8_baseline -T 1,1 -F 8,8")
#tests="$tests""yield_fibres8_baseline_group_pinned -T 1,1 -C 0.8 -F 8,8")
#tests="$tests""yield_fibres8_baseline_indiv_pinned -T 1,1 -C 0.8 -I -F 8,8")

## Evaluate the cost of calling yield and switching to another fibre
tests=("yield_fibres8 -T 1,1 -F 8,8")
tests=("yield_fibres8_group_pinned -T 1,1 -C 0.8 -F 8,8")
#tests=("yield_fibres8_indiv_pinned -T 1,1 -C 0.8 -I -F 8,8")

### RUN THE tests
run_cachetests "$output_dir" "${tests[@]}"



## Enable global yielding
build_cachetest_with_flags -DFIBRE_YIELD_GLOBAL

## Evaluate the cost of calling yield and switching to another fibre
TESTS="yield_global_fibres8 -T 1,1 -F 8,8\n"
TESTS="yield_global_fibres8_group_pinned -T 1,1 -C 0.8 -F 8,8\n"
#TESTS="yield_global_fibres8_indiv_pinned -T 1,1 -C 0.8 -I -F 8,8\n"

### RUN THE TESTS
run_tests



### Enable global yielding
build_cachetest_with_flags -DFIBRE_YIELD_FORCE

## Evaluate the cost of calling yield and switching to another fibre
TESTS="yield_force_fibres8 -T 1,1 -F 8,8\n"
TESTS="yield_force_fibres8_group_pinned -T 1,1 -C 0.8 -F 8,8\n"
#TESTS="yield_force_fibres8_indiv_pinned -T 1,1 -C 0.8 -I -F 8,8\n"

### RUN THE TESTS
run_tests


# Don't leave compiled cachetest being some weird variant
build_cachetest

