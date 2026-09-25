#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Starting nginx..."
sudo systemctl start nginx
sleep 1

echo "[+] Executing siege (HTTP load test)..."
siege -c 50 -t 3m http://127.0.0.1:80 |
	tee "$LOGDIR/nginx_bench.log"

sudo systemctl stop nginx

echo "[✓] Nginx benchmark finished!"
