#! /usr/bin/bash

# Module infos
MODULE=""
PROCFILE=""

# Tests infos
WORKLOAD=0
FILETESTS=""
DATAFILE="pattern_data.bin"
SIZE_MB=512
PATTERNS="chain stride random seq"
GROUPS_N=64
MEMBERS=8
STEADY=10
WARM_ROUNDS=5
WARM_SECONDS=20
THINK_US=500
EVICT="dontneed"
DROP_CACHES=0
RANDOM_FLAG=""

# Logs infos
LOGDIR="./benchmarks/bench_logs/"
DATADIR="datas_collected"

# Notifier
NOTIFIER=""

function save_logs() {
	local path="$1"
	local timers="$2"

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
	echo "[...] Installing $MODULE with params: $@"
	insmod "$MODULE.ko" $@
}

function unload_module() {
	echo "[...] Removing $MODULE"
	rmmod "$MODULE"
}

function run_patterns() {
	local dir="$1"

	if [[ ! -x "pattern_workload" ]]; then
		echo "[...] Compilling pattern_workload.c"
		gcc -O2 -o pattern_workload pattern_workload.c || exit 1
	fi

	for pattern in $PATTERNS; do
		echo "        Running $pattern"

		[[ $DROP_CACHES -eq 1 ]] && sync && echo 3>"/proc/sys/vm/drop_caches"

		local maj0 min0
		maj0=$(vm_value pgmajfault)
		min0=$(vm_value pgfault)

		./pattern_workload -f "$dir/$DATAFILE" -s "$SIZE_MB" -p "$pattern" -g "$GROUPS_N" \
			-m "$MEMBERS" -r "$STEADY" -w "$WARM_ROUNDS" -T "$WARM_SECONDS" \
			-t "$THINK_US" -e "$EVICT" $RANDOM_FLAG >"$dir/$pattern.csv"

		local maj1 min1
		maj1=$(vm_value pgmajfault)
		min1=$(vm_value pgfault)

		{
			echo "pgmajfault $((maj1 - maj0))"
			echo "pgfault $((min1 - min0))"
		} >"$dir/$pattern.vm"

		sleep 2
	done
}

function run_bench() {
	local name="$1"

	rm -rf "$DATADIR/$name"
	mkdir -p "$DATADIR/$name"

	[[ -f "/proc/$PROCFILE" ]] && cat "/proc/$PROCFILE" >"$DATADIR/$name/$name.proc_before"

	local maj0 min0
	maj0=$(vm_value pgmajfault)
	min0=$(vm_value pgfault)

	local start=$(date +%s%N)

	test_pid=0
	cd "benchmarks"
	if [[ "$WORKLOAD" -eq 0 ]]; then
		[[ $DROP_CACHES -eq 1 ]] && sync && echo 3>"/proc/sys/vm/drop_caches"

		sudo ./run_all_benchmarks.sh >"../$DATADIR/$name/bench_logs.log" &
		test_pid=$!
	else
		run_patterns "../$DATADIR/$name" &
		test_pid=$!
	fi
	cd ".."

	collect_mm "$DATADIR/$name" &
	collector_pid=$!

	wait "$test_pid"
	kill -9 "$collector_pid"

	local end=$(date +%s%N)

	local maj1 min1
	maj1=$(vm_value pgmajfault)
	min1=$(vm_value pgfault)

	[[ -f "/proc/$PROCFILE" ]] && cat "/proc/$PROCFILE" >"$DATADIR/$name/$name.proc_after"

	{
		echo "pgmajfault $((maj1 - maj0))"
		echo "pgfault $((min1 - min0))"
	} >"$DATADIR/$name/fault.vm"

	echo "# timers start: $start end: $end duration: $((end - start))"
}

function run_test() {
	local execution="$1"
	local params="$2"

	[[ -f /tmp/page_window_rate_events.bin ]] && rm /tmp/page_window_rate_events.bin

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
	run_bench "$data_name"
	timer=""
	# timer=$(run_bench "$data_name")

	[[ -f "/proc/$PROCFILE" ]] && cat "/proc/$PROCFILE" >"$DATADIR/$data_name/final.proc"

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
		"-n" | "--notifier") NOTIFIER="$2" ;;
		"-m" | "--module") MODULE="$2" ;;
		"-f" | "--file") PROCFILE="$2" ;;
		"-dd" | "--data-dir") DATADIR="$2" ;;
		"-ft" | "--file-tests") FILETESTS="$2" ;;
		"-F" | "--data-file") DATAFILE="$2" ;;
		"-S" | "--size-mb") SIZE_MB="$2" ;;
		"-p" | "--pattern") PATTERNS="$2" ;;
		"-g" | "--group") GROUPS_N="$2" ;;
		"-M" | "--members") MEMBERS="$2" ;;
		"-r" | "--steady") STEADY="$2" ;;
		"-W" | "--warm-rounds") WARM_ROUNDS="$2" ;;
		"-T" | "--warm-seconds") WARM_SECONDS="$2" ;;
		"-t" | "--think-us") THINK_US="$2" ;;
		"-e" | "--evict") EVICT="$2" ;;
		"-D" | "--drop")
			DROP_CACHES=1
			shift
			continue
			;;
		"-R" | "--random-flag")
			RANDOM_FLAG="-R"
			shift
			continue
			;;
		"-w" | "--workload")
			WORKLOAD=1
			shift
			continue
			;;
		*) exit 1 ;;
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

	[[ "$WORKLOAD" -eq 1 ]] && python ./collect_patterns_results.py --dir "$DATADIR"

	sudo chown -R adrian "$DATADIR"

	[[ "$NOTIFIER" != "" ]] && shout
}

main "$@"
