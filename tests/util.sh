#!/bin/bash

# General Cachetest constants
CACHETEST_DIR=../
CACHETEST="$CACHETEST_DIR"/src/cachetest
CACHETEST_SRC="$CACHETEST_DIR"/src/cachetest.cc

# More specific Cachetest constants
# (may need to generalize code more)
DURATIONS="20 10"
SEED=2
EVENTS="L1-dcache-loads,L1-dcache-load-misses"


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
		return -1
	fi

	# Remove suffix and convert to bytes
	cache_size=$(( $(echo $cache_size | sed 's/K$/ * 1024/') ))

	# Make sure it's a number
	if [[ $(echo $cache_size | sed 's/^[0-9][0-9]*$//') != "" ]]; then
		echo "Could not parse cache size of \"$cache_size\""
		return -1
	fi

	echo $cache_size
}
L1D_SIZE_B=$(get_cache_size 1 Data)
L2_SIZE_B=$(get_cache_size 2)
L3_SIZE_B=$(get_cache_size 3)

function build_cachetest() {
	pushd "$CACHETEST_DIR"
	make clean
	make -j $@; tmp=$?
	popd
	return $tmp;
}

build_cachetest_with_flags() {
	build_cachetest CXXFLAGS="$@"
}

# Accpts inputs:
#  - output directory
#  - List of tests to executed (stored as array)
function run_tests() {
	output_dir="$1"
	shift
	tests=( "$@" )

	echo "run_tests:"
	echo "'$output_dir'"
	echo "'$tests'"
	for test_line in "${tests[@]}"; do
		echo "  '$test_line'"
	done
	exit -1

	mkdir -p "$output_dir"


	# Before test(s) begin:
	sudo cpupower frequency-set -g performance > /dev/null


	# Loop over tests executing each
	# Each line has format: TEST_IDENTIFIER COMMAND...
	for test_line in "${tests[@]}"; do
		if [[ "" == "$test_line" ]]; then
			continue
		fi

		# Parse input line
		test_identifier=$(echo "$test_line" | cut -d ' ' -f 1)
		cmd="$(echo "$test_line" | cut -d ' ' -f 2-)"

		output_filepath="$output_dir/$test_identifier".out
		timestamp="$(date -u)"

		# Log info to output file
		echo $(git log -1 --pretty=format:"Commit Hash: %h - %s") > "$output_filepath"
		echo "$cmd" >> "$output_filepath"
		echo "Timestamp: $timestamp" >> "$output_filepath"
		echo >> "$output_filepath"

		# Log info to terminal
		echo "Running Test \"$test_identifier\" - $timestamp"

		# Run test
		eval "$cmd" &>> "$output_filepath"
	done


	#After test(s) end:
	sudo cpupower frequency-set -g powersave > /dev/null
}

function run_cachetests() {
	output_dir="$1"
	shift
	args=( "$@" )
	tests=()

	# Execute each test in a random order
	for duration in $DURATIONS; do
		for test_args_line in "${args[@]}"; do
			test_identifier=$(echo "$test_args_line" | cut -d ' ' -f 1)
			test_identifier="$test_identifier"_duration_"$duration"

			test_args="$(echo "$test_args_line" | cut -d ' ' -f 2-)"
			cmd="taskset -ac 0-15 sudo perf stat -e $EVENTS $CACHETEST -r $SEED -d $duration $test_args"

			tests+=("$test_identifier $cmd")
		done
	done

	run_tests "${output_dir}" "${tests[@]}"
}

function check_output_dir() {
	if [[ 1 != $# ]] || [[ '-h' == "$1" ]] || [[ '--help' == "$1" ]]; then
		echo "USAGE: $0 OUTPUT_DIRECTORY"
		exit -1
	fi
	echo "$1"
}

function quit_on_sigint() {
	trap exit SIGINT
}
