#! /usr/bin/bash

MODULE=""
PROCFILE=""
FILETESTS=""
NOTIFIER=""
LOGDIR="./benchmarks/bench_logs/"
DATADIR="datas_collected"

function save_logs() {
	local path="$1"
	local timers="$2"

	rm -rf "$DATADIR/$path"

	for file in $(ls "$LOGDIR"); do
		cp "$LOGDIR/$file" "$DATADIR/$path/"
	done

	if [[ "$path" != "normal" ]]; then
		python ./events_converter.py
		mv "page_events.csv" "$DATADIR/$path/events.csv"
	fi

	echo "$timers" >>"$DATADIR/$path/bench_logs.log"
}

function vm_value() {
	awk -v k="$1" '$1 == k { print $2 }' /proc/vmstat
}

function mm_value() {
	local type="$1"

	local lines=$(cat "/proc/meminfo" |
		grep -E "MemTotal|MemAvaliable|MemFree|Cached|SwapFree")

	if [[ $type == "head" ]]; then
		echo "$lines" | awk 'BEGIN { ORS="," } { sub(/:/,""); print $1 }'
	elif [[ $type == "data" ]]; then
		echo "$lines" | awk 'BEGIN { ORS="," } { print $2 }'
	fi
	echo ""
}

function collect_mm() {
	local dir="$1"

	rm -f "$dir/meminfo.csv"

	mm_value "head" >>"$dir/meminfo.csv"

	while [[ true ]]; do
		mm_value "data" >>"$dir/meminfo.csv"

		sleep 1
	done
}

function load_module() {
	echo "[...] Installing $MODULE with params: $1"
	insmod "$MODULE.ko" $1
}

function unload_module() {
	echo "[...] Removing $MODULE"
	rmmod "$MODULE"
}

function run_bench() {
	local path="$1"
	local proc="$2"

	rm -rf "$DATADIR/$path"
	mkdir -p "$DATADIR/$path"

	local maj0 min0
	maj0=$(vm_value pgmajfault)
	min0=$(vm_value pgfault)

	local start=$(date +%s%N)

	cd "benchmarks"

	sudo ./run_all_benchmarks.sh "$proc" >"../$DATADIR/$path/bench_logs.log" &
	bench_pid=$!

	cd ".."

	collect_mm "$DATADIR/$path" &
	collector_pid=$!

	wait "$bench_pid"
	kill -9 "$collector_pid"

	local end=$(date +%s%N)

	local maj1 min1
	maj1=$(vm_value pgmajfault)
	min1=$(vm_value pgfault)

	{
		echo "pgmajfault $((maj1 - maj0))"
		echo "pgfault $((min1 - min0))"
	} >"$DATADIR/$path/fault.vm"

	echo "# timers start: $start end: $end duration: $((end - start))"
}

function run_test() {
	local execution="$1"
	local params="$2"

	sudo rm /tmp/page_window_rate_events.bin

	data_name=$(echo "$params" | awk -F, '{ if ($1 ~ /^name=/) { sub(/^name=/, "", $1); print $1 } }')
	params=$(echo $params | sed 's/,/ /g')

	if [[ "$data_name" == "" ]]; then
		echo "[x] Must define test name (Ex: \"name=test1,p1=v1,p2=v2;name=test2,p1=v3,p2=v4\")"
		echo "[...] Passing test with params: $params"
		return
	fi

	if [[ "$data_name" != "normal" ]]; then
		load_module $params
		sleep 1
	fi

	echo "[...] Starting benchmark '$data_name' at $(date '+%Hh %Mm %Ss %Nns')"
	timer=$(run_bench "$data_name" "$PROCFILE"
	run_bench "$data_name" "$PROCFILE"

	[[ -r "/proc/$PROCFILE" ]] && cat "/proc/$PROCFILE" >"$DATADIR/$data_name/final.proc"

	if [[ "$data_name" != "normal" ]]; then
		unload_module
		dmesg | tail -n 50 >"$DATADIR/$data_name/dmesg_tail.txt"
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
		"-m" | "--module") MODULE="$2" ;;
		"-f" | "--file") PROCFILE="$2" ;;
		"-ft" | "--file-tests") FILETESTS="$2" ;;
		"-n" | "--notifier") NOTIFIER="$2" ;;
		esac

		shift
		shift
	done

	if [[ $EUID -ne 0 ]]; then
		echo "[x] run as root (inmod, rmmod, /proc/sys/vm/drop_caches)"
		exit 1
	fi

	if [[ ! -f "$FILETESTS" ]]; then
		echo "[x] tests file not found: $FILETESTS"
		exit 1
	fi

	if [[ "$MODULE" == "" ]]; then
		echo "[x] Must define module"
		exit 1
	elif [ "$PROCFILE" == "" ]; then
		echo "[x] Must define /proc filename"
		exit 1
	fi

	if [[ ! -f "$MODULE.ko" && "$(grep -c '^name=normal' "$FILETESTS")" -ne "$(grep -vc '^\s*\(#\|$\)' "$FILETESTS")" ]]; then
		echo "[x] module not found: $MODULE (run make in mm/page_collector)"
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

	echo "[✓] All tests finished"

	sudo chown -R adrian "$DATADIR"

	[[ "$NOTIFIER" != "" ]] && shout
}

main "$@"
