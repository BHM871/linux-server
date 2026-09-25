#! /usr/bin/bash

MODULE=""
PROCFILE=""
TESTS=""
VALUES=""
LOGDIR="./benchmarks/bench_logs/"
DATADIR="./datas_collected/"

function save_logs() {
	local log_name="$1.log"
	local timers="$2"

	rm -rf "$DATADIR/$log_name"

	for file in $(ls "$LOGDIR"); do
		local file_name=$(echo $file | cut -d '/' -f 2)
		local initial_name=$(echo $file_name | cut -d '_' -f 1)

		if [[ "$initial_name" != "initial" && "$initial_name" != "final" ]]; then
			cat "$LOGDIR/$file" >>"$DATADIR/$log_name"
			echo "------------------------------------------------------------------------------------" >>"$DATADIR/$log_name"
			echo "------------------------------------------------------------------------------------" >>"$DATADIR/$log_name"
		else
			cp "$LOGDIR/$file" "$DATADIR/$(echo $initial_name)_dump_$log_name"
		fi
	done

	echo "$timers" >>"$DATADIR/$log_name"
}

function run_bench() {
	local proc="$1"

	cd "benchmarks"
	local time=$(sudo ./run_all_benchmarks.sh "$proc" | tail -n 1)
	cd ".."

	echo "$time"
}

function main() {
	while [[ "$1" != "" ]]; do
		case "$1" in
		"-m" | "--module")
			MODULE="$2"
			;;
		"-f" | "--file")
			PROCFILE="$2"
			;;
		"-t" | "--tests")
			TESTS=$(echo "$2" | sed "s/;/ /g")
			;;
		esac

		shift
		shift
	done

	if [ "$MODULE" == "" ]; then
		echo "[x] Must define module"
		exit 1
	elif [ "$PROCFILE" == "" ]; then
		echo "[x] Must define /proc filename"
		exit 1
	fi

	mkdir -p "$DATADIR"

	local timer=$(run_bench "none")
	save_logs "normal" "$timer"

	for params in $TESTS; do

		data_name=$(echo "$params" | awk -F, '{ if ($1 ~ /^name=/) { sub(/^name=/, "", $1); print $1 } }')
		if [[ "$data_name" == "" ]]; then
			echo "[x] Must define test name (Ex: \"name=test1,p1=v1,p2=v2;name=test2,p1=v3,p2=v4\")"
			echo "[...] Passing test with params: $params"
			continue
		fi

		sudo insmod "$MODULE.ko" "$params"

		sleep 1

		timer=$(run_bench "$PROCFILE")
		save_logs "$data_name" "$timer"

		sudo rmmod "$MODULE"
	done

	sudo chown -R adrian "$DATADIR"
}

main "$@"
