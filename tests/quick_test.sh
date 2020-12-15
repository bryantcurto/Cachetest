#!/bin/bash

CACHETEST=../src/cachetest
OUTPUT_DIR=./output
SEED=2
#DURATIONS="2 1"
DURATIONS="20 10"
EVENTS="L1-dcache-loads,L1-dcache-load-misses"

# Run the test with working set size that is ~1.5 * size of L1.

# Vary the sizes, checking hits/misses for L2 and L3
L1_SIZE_SCALAR=1.5
L1_SIZE_B=$(getconf -a | grep LEVEL1_DCACHE_SIZE | rev | cut -d ' ' -f 1 | rev) # bytes
WORKING_SET_SIZE_KB=$(bc -l <<< "$L1_SIZE_B * $L1_SIZE_SCALAR / 1000. + 0.5" | cut -d "." -f 1)

echo "L1 DCache Size (B): $L1_SIZE_B"
echo "Target Working Set Size (KB): $WORKING_SET_SIZE_KB"

TESTS=

# Use perf to get information about cache misses
# Are these the cores we want to pin to, or something like 0-3,8-11

# Base case using my own code to 


# Thread test
TESTS="$TESTS""threads $CACHETEST -T 4,4\n"
TESTS="$TESTS""threads_group_pinned $CACHETEST -T 4,4 -C 0-3.4-7\n"
TESTS="$TESTS""threads_indiv_pinned $CACHETEST -T 4,4 -C 0-3.4-7 -I\n"

# Fibre test
TESTS="$TESTS""fibres $CACHETEST -T 4,4 -F 4,4\n"
TESTS="$TESTS""fibres_group_pinned $CACHETEST -T 4,4 -C 0-3.4-7 -F 4,4\n"
TESTS="$TESTS""fibres_indiv_pinned $CACHETEST -T 4,4 -C 0-3.4-7 -I -F 4,4\n"

# Migration test
TESTS="$TESTS""migrate $CACHETEST -T 4,4 -F 16,16 -M\n"
TESTS="$TESTS""migrate_group_pinned $CACHETEST -T 4,4 -F 16,16 -M -C 0-3.4-7\n"
TESTS="$TESTS""migrate_indiv_pinned $CACHETEST -T 4,4 -F 16,16 -M -C 0-3.4-7 -I\n"

# Create 16 fibres per subpath, no migration, fibres yield at the end of subpath
# Understand the cost of yielding fibres

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
