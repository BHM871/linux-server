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
rm -f $LOGDIR/*

if [[ $log -eq 1 ]]; then
	echo "[+] Registring initial module..."
	cat $PROCFILE | sort -n >"$LOGDIR/initial_proc_dump.csv"
fi

start=$(date +%s%N)

echo "[...] Running Workload..."
./run_workload.sh

# echo "[...] Running stress-ng..."
# ./run_stress_bench.sh
#
# echo "[...] Running Memcached benchmark..."
# ./run_memcached_bench.sh
#
# echo "[...] Running PostgreSQL benchmark..."
# ./run_postgres_bench.sh
#
# echo "[...] Running Nginx benchmark..."
# ./run_nginx_bench.sh

end=$(date +%s%N)
duration=$((end - start))

if [[ $log -eq 1 ]]; then
	echo "[+] Capturing final logs..."
	cat $PROCFILE >"$LOGDIR/final_proc_dump.csv"
	echo "# timers start: $start end: $end duration: $duration" >>"$LOGDIR/final_proc_dump.csv"
	sort -n "$LOGDIR/final_proc_dump.csv" >"$LOGDIR/final_proc_dump.csv"
fi

echo "[✓] All benchmarks finished!"

if [[ $log -eq 1 ]]; then
	echo "Results storaged at $LOGDIR/"
fi

echo "[✓] start: $start end: $end duration: $duration"
