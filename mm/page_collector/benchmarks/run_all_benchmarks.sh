#!/bin/bash

log=1

if [ "$1" == "" ]; then
	echo "[x] Need specify module"
	exit 1
elif [[ "$1" == "none" ]]; then
	log=0
fi

PROCFILENAME="$1"
PROCFILE="/proc/$PROCFILENAME"

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Limpando logs antigos..."
rm -f $LOGDIR/* 2>/dev/null

if [[ $log -eq 1 ]]; then
	echo "[+] Registring initial module..."
	[[ -r "$PROCFILE" ]] && cat $PROCFILE | sort -n >"$LOGDIR/$PROCFILENAME.proc_before"
fi

sync && echo 3>"/proc/sys/vm/drop_caches"

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

wait "$spid" "$mpid" "$ppid" "$npid"

if [[ $log -eq 1 ]]; then
	echo "[+] Capturing final logs..."

	[[ -r "$PROCFILE" ]] && cat $PROCFILE >"$LOGDIR/$PROCFILENAME.proc_after"
fi

echo "[✓] All benchmarks finished!"

if [[ $log -eq 1 ]]; then
	echo "Results storaged at $LOGDIR/"
fi
