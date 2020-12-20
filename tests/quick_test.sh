#!/bin/bash

# NOTES:
# hypterthreading disabled for all experiments
# cores 0-7 on one socket and 8-15 on another

CACHETEST=../src/cachetest
OUTPUT_DIR=./output
SEED=2
#DURATIONS="2 1"
DURATIONS="2 1"
EVENTS="L1-dcache-loads,L1-dcache-load-misses"

# Run the test with working set size that is ~1.5 * size of L1.

# TODO: Vary the sizes, checking hits/misses for L2 and L3
L1_SIZE_SCALAR=1.5
L1_SIZE_B=$(getconf -a | grep LEVEL1_DCACHE_SIZE | rev | cut -d ' ' -f 1 | rev) # bytes
WORKING_SET_SIZE_KB=$(bc -l <<< "$L1_SIZE_B * $L1_SIZE_SCALAR / 1000. + 0.5" | cut -d "." -f 1)

echo "L1 DCache Size (B): $L1_SIZE_B"
echo "Target Working Set Size (KB): $WORKING_SET_SIZE_KB"
exit 1

TESTS=

# Thread test
TESTS="$TESTS""threads $CACHETEST -T 8,8\n"
TESTS="$TESTS""threads_group_pinned $CACHETEST -T 8,8 -C 0-7.8-15\n"
TESTS="$TESTS""threads_indiv_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -I\n"

# Fibre test
TESTS="$TESTS""fibres $CACHETEST -T 8,8 -F 32,32\n"
TESTS="$TESTS""fibres_group_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -F 32,32\n"
TESTS="$TESTS""fibres_indiv_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -I -F 32,32\n"

# Migration test
TESTS="$TESTS""migrate $CACHETEST -T 8,8 -F 32,32 -M\n"
TESTS="$TESTS""migrate_group_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -F 32,32 -M\n"
TESTS="$TESTS""migrate_indiv_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -I -F 32,32 -M\n"

# Yielding-Fibre-Code test
# Create 16 fibres per subpath, no migration just loop over subpath, fibres yield at the end of subpath
# Understand the cost of yielding fibres.
#TESTS="$TESTS""yield_fibres $CACHETEST -T 8,8 -F 32,32\n"
#TESTS="$TESTS""yield_fibres_group_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -F 32,32\n"
#TESTS="$TESTS""yield_fibres_indiv_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -I -F 32,32\n"


# Execute each test in a random order
for duration in $DURATIONS; do
  mkdir -p "$OUTPUT_DIR"
  while read test; do
    if [[ "" == "$test" ]]; then
      continue
    fi

    test_prefix=$(echo "$test" | cut -d ' ' -f 1)
    command="$(echo "$test" | cut -d ' ' -f 2-)"
    eval "sudo perf stat -e $EVENTS $command -r $SEED -d $duration $WORKING_SET_SIZE_KB" &> "$OUTPUT_DIR/$test_prefix"_duration_"$duration".out
    #echo "eval perf stat -e $EVENTS $command -r $SEED -d $duration $WORKING_SET_SIZE_KB &> $OUTPUT_DIR/$test_prefix"_duration_"$duration".out
  done <<< "$(echo -e "$TESTS")"
done
