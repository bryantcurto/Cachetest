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

TESTS= # stores the tests to run
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
	    options="$(echo "$test" | cut -d ' ' -f 2-)"
	    cmd="$CACHETEST $options -r $SEED -d $duration $WORKING_SET_SIZE_KB"
	    cmd="taskset -ac 0-15 sudo perf stat -e $EVENTS $cmd"

	    output_filepath="$OUTPUT_DIR/$test_prefix"_duration_"$duration".out

	    echo "Running $test_prefix ($duration sec) - $(date -u)"
	    echo $(git log -1 --pretty=format:"Commit Hash: %h - %s") > "$output_filepath"
		echo "$cmd" >> "$output_filepath"
		echo >> "$output_filepath"
	    eval "$cmd" &>> "$output_filepath"
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

set_option() {
	option="$1"
	action="$2"

	if [[ 0 == "$action" ]]; then
		start_pre=
		end_pre='//'
		str="disable"
	else
		start_pre='//'
		end_pre=
		str="enable"
	fi
	start_sed_pre=$(echo $start_pre | sed 's/\//\\\//g')
	end_sed_pre=$(echo $end_pre | sed 's/\//\\\//g')

	if [[ 1 != $(cat "$CACHETEST_SRC" | grep '^'"$start_pre"'#define '"$option"'$' | wc -l) ]]; then
		echo "Failed to $str $option"
		exit -1
	fi

	start_str="$start_sed_pre"'\(#define '"$option"'\)'
	end_str="$end_sed_pre"'\1'
	cat "$CACHETEST_SRC" | sed 's/^'"$start_str"'$/'"$end_str"'/' > "$CACHETEST"
	mv "$CACHETEST" "$CACHETEST_SRC"

	build_cachetest
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
TESTS="$TESTS""baseline -T 16\n"
TESTS="$TESTS""baseline_group_pinned -T 16 -C 0-15\n"
TESTS="$TESTS""baseline_indiv_pinned -T 16 -C 0-15 -I\n"

## Thread test
TESTS="$TESTS""threads -T 8,8\n"
TESTS="$TESTS""threads_group_pinned -T 8,8 -C 0-7.8-15\n"
TESTS="$TESTS""threads_indiv_pinned -T 8,8 -C 0-7.8-15 -I\n"

## Fibre test
TESTS="$TESTS""fibres32 -T 8,8 -F 32,32\n"
TESTS="$TESTS""fibres32_group_pinned -T 8,8 -C 0-7.8-15 -F 32,32\n"
TESTS="$TESTS""fibres32_indiv_pinned -T 8,8 -C 0-7.8-15 -I -F 32,32\n"

## Migration test
TESTS="$TESTS""migrate32 -T 8,8 -F 32,32 -M\n"
TESTS="$TESTS""migrate32_group_pinned -T 8,8 -C 0-7.8-15 -F 32,32 -M\n"
TESTS="$TESTS""migrate32_indiv_pinned -T 8,8 -C 0-7.8-15 -I -F 32,32 -M\n"


# Vary the number of fibres
for f in 8 16 64 128; do
	TESTS="$TESTS""fibres$f -T 8,8 -F $f,$f\n"
	TESTS="$TESTS""migrate$f -T 8,8 -F $f,$f -M\n"
done

### RUN THE TESTS
run_tests



### Enable yielding
set_option FIBRE_YIELD 1

### Yielding-Fibre-Code Tests
## Create 16 fibres per subpath, no migration just loop over subpath, fibres yield at the end of subpath
## Understand the cost of yielding fibres.
TESTS=

## Evaluate the cost of calling yield, but not actually switching to another fibre
TESTS="$TESTS""yield_fibres8_baseline -T 8,8 -F 8,8\n"
TESTS="$TESTS""yield_fibres8_baseline_group_pinned -T 8,8 -C 0-7.8-15 -F 8,8\n"
TESTS="$TESTS""yield_fibres8_baseline_indiv_pinned -T 8,8 -C 0-7.8-15 -I -F 8,8\n"

## Evaluate the cost of calling yield and switching to another fibre
TESTS="$TESTS""yield_fibres32 -T 8,8 -F 32,32\n"
TESTS="$TESTS""yield_fibres32_group_pinned -T 8,8 -C 0-7.8-15 -F 32,32\n"
TESTS="$TESTS""yield_fibres32_indiv_pinned -T 8,8 -C 0-7.8-15 -I -F 32,32\n"

### RUN THE TESTS
run_tests

### Disable yielding
set_option FIBRE_YIELD 0



## Enable global yielding
set_option FIBRE_YIELD_GLOBAL 1

## Evaluate the cost of calling yield and switching to another fibre
TESTS="yield_global_fibres32 -T 8,8 -F 32,32\n"

### RUN THE TESTS
run_tests

### Disable global yielding
set_option FIBRE_YIELD_GLOBAL 0


### Enable global yielding
set_option FIBRE_YIELD_FORCE 1

## Evaluate the cost of calling yield and switching to another fibre
TESTS="yield_force_fibres32 -T 8,8 -F 32,32\n"

### RUN THE TESTS
run_tests

### Disable global yielding
set_option FIBRE_YIELD_FORCE 0


#sudo perf record -e cycles,L1-dcache-load-misses,LLC-load-misses,cache-misses -g ./src/cachetest -T 8,8 -F 32,32 -r 2 -d 1 49 > tests/output/{yield_,}fibres_duration_1_report.txt
#sudo perf report -g graph -i tests/output/yield_fibres_duration_1_report.data

#sudo perf mem -t load record -e 'cpu/mem-loads,ldlat=30,freq=2000/P' ./src/cachetest -T 8,8 -F 32,32 -r 2 -d 1 49
#sudo perf mem report
