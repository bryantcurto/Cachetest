pushd output > /dev/null
dir_long=2
dir_short=1

for test_prefix in $(ls *.out | rev | cut -d '_' -f 3- | rev | sort -u); do
  log_short="$test_prefix"_duration_"$dir_short".out
  log_long="$test_prefix"_duration_"$dir_long".out

  access_short=$(cat "$log_short" | grep "total access count=" | cut -d '=' -f 2)
  access_long=$(cat "$log_long" | grep "total access count=" | cut -d '=' -f 2)

  loads_short=$(cat "$log_short" | grep L1-dcache-loads | cut -d 'L' -f 1 | sed 's/[ ,]//g')
  loads_long=$(cat "$log_long" | grep L1-dcache-loads | cut -d 'L' -f 1 | sed 's/[ ,]//g')

  misses_short=$(cat "$log_short" | grep L1-dcache-load-misses | cut -d 'L' -f 1 | sed 's/[ ,]//g')
  misses_long=$(cat "$log_long" | grep L1-dcache-load-misses | cut -d 'L' -f 1 | sed 's/[ ,]//g')

  loads=$(echo "$loads_long - $loads_short" | bc -l)
  misses=$(echo "$misses_long - $misses_short" | bc -l)

  echo "==== $test_prefix ===="
  printf "%s sec test: accesses = %'d\n" $dir_long $access_long
  printf "%s sec test: accesses = %'d\n" $dir_short $access_short
  printf "%s sec test: loads = %'d\n" $dir_long $loads_long
  printf "%s sec test: loads = %'d\n" $dir_short $loads_short
  printf "%s sec test: misses = %'d\n" $dir_long $misses_long
  printf "%s sec test: misses = %'d\n" $dir_short $misses_short
  printf "miss rate = %.2f%% / loads = %'d / misses = %'d\n" "$(echo "$misses / $loads * 100." | bc -l)" $loads $misses
done

#for test_prefix in $(ls | rev | cut -d '_' -f 3- | rev | sort -u); do
#  loads1=$(cat 
#  values="$(cat "$test_prefix"* | grep "results=" | rev | cut -d ' ' -f 1 | rev)"
#  mean=$(echo "$values" | awk 'BEGIN{s=0} {s=s+$1} END{printf "%f\n", s/NR}')
#  echo "$test_prefix $mean"
#done
#
#means="$(compute_means)"
#for i in $(seq 1 2 $(echo "$means" | wc -l)); do
#  hits=$(echo "$means" | head -n $i | tail -n 1)
#  if [[ "$(echo $hits | cut -d ' ' -f 1)" != *"hit"* ]]; then
#    echo "not hit"; exit -1
#  fi
#
#  misses=$(echo "$means" | head -n $((i + 1)) | tail -n 1)
#  if [[ "$(echo "$misses" | cut -d ' ' -f 1)" != *"miss"* ]]; then
#    echo "not miss"; exit -1
#  fi
#
#  if [[ "$(echo "$hits" | cut -d ':' -f 1)" != "$(echo "$misses" | cut -d ':' -f 1)" ]]; then
#    echo "bad match"; exit -2
#  fi
#
#  test_name=$(echo "$hits" | cut -d ':' -f 1)
#  hits=$(echo "$hits" | cut -d ' ' -f 2)
#  misses=$(echo "$misses" | cut -d ' ' -f 2)
#
#  echo "$test_name: hits=$hits misses=$misses hit_prob=$(bc -l  <<< "$hits / ($hits + $misses)")"
#done

popd > /dev/null
