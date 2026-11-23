#! /usr/bin/bash

MODULE=""
PROCFILE=""
TESTS=""
FILETESTS=""
NOTIFIER=""
LOGDIR="./benchmarks/bench_logs/"
DATADIR="./datas_collected/"

function save_logs() {
	local log_name="$1.log"
	local timers="$2"

	rm -rf "$DATADIR/$log_name"

	for file in $(ls "$LOGDIR"); do
		local file_name=$(echo $file | cut -d '/' -f 2)
		local initial_name=$(echo $file_name | cut -d '_' -f 1)

		if [[ -d "$LOGDIR/$file" ]]; then
			cp -r "$LOGDIR/$file" "$DATADIR/$1"
			continue
		fi

		if [[ "$initial_name" == "initial" || "$initial_name" == "final" ]]; then
			cp "$LOGDIR/$file" "$DATADIR/$(echo $initial_name)_dump_$log_name"
			continue
		fi

		cat "$LOGDIR/$file" >>"$DATADIR/$log_name"
		echo "------------------------------------------------------------------------------------" >>"$DATADIR/$log_name"
		echo "------------------------------------------------------------------------------------" >>"$DATADIR/$log_name"
	done

	if [[ "$log_name" != "normal.log" ]]; then
		python ./events_converter.py
		mv "page_events.csv" "$DATADIR/events_$1.csv"
	fi

	echo "$timers" >>"$DATADIR/$log_name"
}

function run_bench() {
	local proc="$1"

	cd "benchmarks"

	sudo ./run_all_benchmarks.sh "$proc" >tmp_bench &
	bench_pid=$!

	sudo ./collect_proc.sh --output "bench_logs/memory" &
	collector_pid=$!

	wait "$bench_pid"
	kill -9 "$collector_pid"

	local timer=$(tail -n 1 <tmp_bench)
	rm tmp_bench

	cd ".."

	echo "$timer"
}

function run_test() {
	local execution="$1"
	local params="$2"

	data_name=$(echo "$params" | awk -F, '{ if ($1 ~ /^name=/) { sub(/^name=/, "", $1); print $1 } }')
	if [[ "$data_name" == "" ]]; then
		echo "[x] Must define test name (Ex: \"name=test1,p1=v1,p2=v2;name=test2,p1=v3,p2=v4\")"
		echo "[...] Passing test with params: $params"
		return
	fi

	if [[ "$data_name" != "normal" ]]; then
		sudo rm /tmp/page_window_rate_events.bin

		params=$(echo $params | sed 's/,/ /g')
		echo "[...] Installing $MODULE with params: $params"
		sudo insmod "$MODULE.ko" $params

		sleep 1
	fi

	echo "[...] Starting benchmark '$data_name' at $(date '+%Hh %Mm %Ss %Nns')"
	timer=$(run_bench "$PROCFILE")

	if [[ "$data_name" != "normal" ]]; then
		echo "[...] Removing $MODULE"
		sudo rmmod "$MODULE"
	fi

	echo "[...] Saving logs"
	save_logs "$data_name" "$timer"

	echo "[✓] $executionº test finished"

	sleep 3
}

function shout() {
	export XDG_RUNTIME_DIR="/run/user/$(sudo --user=$SUDO_USER id -u)"
	export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
	ffplay -nodisp -autoexit -loglevel error -volume 100 -nodisp "$NOTIFIER"
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
		"-ft" | "--file-tests")
			FILETESTS="$2"
			;;
		"-n" | "--notifier")
			NOTIFIER="$2"
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

	echo "[...] Starting configured tests"

	local ti=1
	if [[ "$FILETESTS" != "" ]]; then
		while IFS= read -r line || [[ -n "$line" ]]; do
			line="${line%%#*}"
			line="$(echo "$line" | tr -d '[:space:]')"

			[[ -z "$line" ]] && continue

			run_test "$ti" "$line"

			ti=$((ti + 1))
		done <"$FILETESTS"
	fi

	if [[ "$TESTS" != "" ]]; then
		for params in $TESTS; do
			run_test "$ti" "$params"
			ti=$((ti + 1))
		done
	fi

	echo "[✓] All tests finished"

	sudo chown -R adrian "$DATADIR"

	[[ "$NOTIFIER" != "" ]] && shout
}

main "$@"
