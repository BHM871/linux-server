#!/bin/bash

log=1

if [ "$1" == "" ]; then
	echo "[x] Need specify module"
	exit 1
elif [[ "$1" == "none" ]]; then
	log=0
fi

PROCFILE="/proc/$1"

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Limpando logs antigos..."
rm -f $LOGDIR/* 2>/dev/null

if [[ $log -eq 1 ]]; then
	echo "[+] Registring initial module..."
	[[ -r "$PROCFILE" ]] && cat $PROCFILE | sort -n >"$LOGDIR/initial_proc_dump.csv"
fi

sync && echo 3>"/proc/sys/vm/drop_caches"

start=$(date +%s%N)

echo "[...] Running Workload..."
./run_workload.sh &
wpid="$!"

echo "[...] Running stress-ng..."
./run_stress_bench.sh &
spid="$!"

echo "[...] Running Memcached benchmark..."
./run_memcached_bench.sh &
mpid="$!"

echo "[...] Running PostgreSQL benchmark..."
./run_postgres_bench.sh &
ppid="$!"

echo "[...] Running Nginx benchmark..."
./run_nginx_bench.sh &
npid="$!"

wait "$wpid" "$spid" "$mpid" "$ppid" "$npid"

end=$(date +%s%N)
duration=$((end - start))

if [[ $log -eq 1 ]]; then
	echo "[+] Capturing final logs..."

	[[ -r "$PROCFILE" ]] && cat $PROCFILE >"$LOGDIR/final_proc_dump.csv"
	echo "# timers start: $start end: $end duration: $duration" >>"$LOGDIR/final_proc_dump.csv"
fi

echo "[✓] All benchmarks finished!"

if [[ $log -eq 1 ]]; then
	echo "Results storaged at $LOGDIR/"
fi

echo "[✓] start: $start end: $end duration: $duration"
