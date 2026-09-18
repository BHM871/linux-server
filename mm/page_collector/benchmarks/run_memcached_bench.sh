#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Iniciando memcached..."
sudo systemctl start memcached
sleep 1

echo "[+] Executando memtier_benchmark..."
sudo -u root memcached -p 11211 -s 127.0.0.1 \
	--protocol=binary \
	--threads=2 |
	tee "$LOGDIR/memcached_bench.log"

sudo systemctl stop memcached

echo "[✓] Memcached benchmark finalizado!"
