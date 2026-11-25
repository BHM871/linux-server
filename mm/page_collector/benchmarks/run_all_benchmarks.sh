#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Limpando logs antigos..."
rm -f $LOGDIR/* 2>/dev/null

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

echo "[✓] All benchmarks finished!"
