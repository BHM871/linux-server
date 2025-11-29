#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Iniciando Redis..."
sudo systemctl start redis-server

sleep 2

echo "[+] Executando redis-benchmark..."
redis-benchmark -n 200000 -t get,set -q | tee "$LOGDIR/redis_bench.log"

echo "[✓] Redis benchmark finalizado!"

