#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Iniciando memcached..."
sudo systemctl start memcached
sleep 1

echo "[+] Executando memtier_benchmark..."
memtier_benchmark -p 11211 -s 127.0.0.1 \
	--protocol=memcache_text \
	--requests=200000 \
	--ratio=1:1 \
	--threads=2 --clients=50 |
	tee "$LOGDIR/memcached_bench.log"

sudo systemctl stop memcached

echo "[✓] Memcached benchmark finalizado!"
