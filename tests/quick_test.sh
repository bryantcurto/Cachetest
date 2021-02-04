#!/bin/bash

# NOTES:
# hypterthreading disabled for all experiments
# cores 0-7 on one socket and 8-15 on another

CACHETEST_DIR=../
CACHETEST="$CACHETEST_DIR"/src/cachetest
CACHETEST_SRC="$CACHETEST_DIR"/src/cachetest.cc
OUTPUT_DIR=./output
SEED=2
DURATIONS="20 10"
EVENTS="L1-dcache-loads,L1-dcache-load-misses"

trap exit SIGINT

run_tests() {
	# Before test(s) begin:
	sudo cpupower frequency-set -g performance > /dev/null

	# Execute each test in a random order
	for duration in $DURATIONS; do
	  mkdir -p "$OUTPUT_DIR"
	  while read test; do
	    if [[ "" == "$test" ]]; then
	      continue
	    fi

	    test_prefix=$(echo "$test" | cut -d ' ' -f 1)
	    command="$(echo "$test" | cut -d ' ' -f 2-)"
	    command="$command -r $SEED -d $duration $WORKING_SET_SIZE_KB"
	    output_filepath="$OUTPUT_DIR/$test_prefix"_duration_"$duration".out

	    echo "Running $test_prefix ($duration sec) - $(date -u)"
	    git log -1 --pretty=format:"Commit Hash: %h - %s" > "$output_filepath"
	    echo >> "$output_filepath"
	    eval "taskset -ac 0-15 sudo perf stat -e $EVENTS $command" &>> "$output_filepath"
	  done <<< "$(echo -e "$TESTS")"
	done

	#After test(s) end:
	sudo cpupower frequency-set -g powersave > /dev/null
}

get_cache_size() {
	level=$1
	dtype=${2:-"Unified"}

	cache_size=0
	for dir in $(ls -d /sys/devices/system/cpu/cpu0/cache/index*); do
		if [[ -e "$dir/level" ]] && [[ $(cat "$dir/level") == "$level" ]] &&
		   [[ -e "$dir/type" ]] && [[ $(cat "$dir/type") == "$dtype" ]]; then
			cache_size=$(cat "$dir"/size)
			break
		fi
	done
	if [[ $cache_size == "0" ]]; then
		echo "Couldn't find size of cache described by level=\"$level\" and dtype=\"$dtype\""
		exit -1
	fi

	# Remove suffix and convert to bytes
	cache_size=$(( $(echo $cache_size | sed 's/K$/ * 1024/') ))

	# Make sure it's a number
	if [[ $(echo $cache_size | sed 's/^[0-9][0-9]*$//') != "" ]]; then
		echo "Could not parse cache size of \"$cache_size\""
		exit -1
	fi

	echo $cache_size
}

build_cachetest() {
	pushd "$CACHETEST_DIR"
	make clean
	make -j all
	popd
}

# Run the test with working set size that is ~1.5 * size of L1.


# TODO: Vary the sizes, checking hits/misses for L2 and L3
L1_SIZE_SCALAR=1.5
L1_SIZE_B=$(get_cache_size 1 Data)
WORKING_SET_SIZE_KB=$(python3 -c "import math; print(int(math.ceil($L1_SIZE_B * $L1_SIZE_SCALAR / 1000.)))")

echo "L1 DCache Size (B): $L1_SIZE_B"
echo "Target Working Set Size (KB): $WORKING_SET_SIZE_KB"


### Build Cachetest initially
build_cachetest
TESTS=

## Baseline test
TESTS="$TESTS""baseline $CACHETEST -T 16\n"
TESTS="$TESTS""baseline_group_pinned $CACHETEST -T 16 -C 0-15\n"
TESTS="$TESTS""baseline_indiv_pinned $CACHETEST -T 16 -C 0-15 -I\n"

## Thread test
TESTS="$TESTS""threads $CACHETEST -T 8,8\n"
TESTS="$TESTS""threads_group_pinned $CACHETEST -T 8,8 -C 0-7.8-15\n"
TESTS="$TESTS""threads_indiv_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -I\n"

## Fibre test
TESTS="$TESTS""fibres $CACHETEST -T 8,8 -F 32,32\n"
TESTS="$TESTS""fibres_group_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -F 32,32\n"
TESTS="$TESTS""fibres_indiv_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -I -F 32,32\n"

## Migration test
TESTS="$TESTS""migrate $CACHETEST -T 8,8 -F 32,32 -M\n"
TESTS="$TESTS""migrate_group_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -F 32,32 -M\n"
TESTS="$TESTS""migrate_indiv_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -I -F 32,32 -M\n"

### RUN THE TESTS
run_tests


### Enable yielding
if [[ 1 != $(cat "$CACHETEST_SRC" | grep '^//#define FIBRE_YIELD$' | wc -l) ]]; then
	echo "Failed to enable yielding"
	exit -1
fi

cat "$CACHETEST_SRC" | sed 's/^\/\/\(#define FIBRE_YIELD\)$/\1/' > "$CACHETEST"
mv "$CACHETEST" "$CACHETEST_SRC"
build_cachetest

### Yielding-Fibre-Code Tests
## Create 16 fibres per subpath, no migration just loop over subpath, fibres yield at the end of subpath
## Understand the cost of yielding fibres.
TESTS=

## Evaluate the cost of calling yield, but not actually switching to another fibre
TESTS="$TESTS""yield_fibres_baseline $CACHETEST -T 8,8 -F 8,8\n"
TESTS="$TESTS""yield_fibres_baseline_group_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -F 8,8\n"
TESTS="$TESTS""yield_fibres_baseline_indiv_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -I -F 8,8\n"

## Evaluate the cost of calling yield and switching to another fibre
TESTS="$TESTS""yield_fibres $CACHETEST -T 8,8 -F 32,32\n"
TESTS="$TESTS""yield_fibres_group_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -F 32,32\n"
TESTS="$TESTS""yield_fibres_indiv_pinned $CACHETEST -T 8,8 -C 0-7.8-15 -I -F 32,32\n"

### RUN THE TESTS
run_tests

### Disable yielding
cat "$CACHETEST_SRC" | sed 's/^\(#define FIBRE_YIELD\)$/\/\/\1/' > "$CACHETEST"
mv "$CACHETEST" "$CACHETEST_SRC"
build_cachetest


#sudo perf record -e cycles,L1-dcache-load-misses,LLC-load-misses,cache-misses -g ./src/cachetest -T 8,8 -F 32,32 -r 2 -d 1 49 > tests/output/{yield_,}fibres_duration_1_report.txt
#sudo perf report -g graph -i tests/output/yield_fibres_duration_1_report.data

#sudo perf mem -t load record -e 'cpu/mem-loads,ldlat=30,freq=2000/P' ./src/cachetest -T 8,8 -F 32,32 -r 2 -d 1 49
#sudo perf mem report
