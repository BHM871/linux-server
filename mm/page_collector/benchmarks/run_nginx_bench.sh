#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Iniciando nginx..."
sudo systemctl start nginx
sleep 1

echo "[+] Executando siege (HTTP load test)..."
siege -c 50 -t 3m http://127.0.0.1 |
	tee "$LOGDIR/nginx_bench.log"

sudo systemctl stop nginx

echo "[✓] Nginx benchmark finalizado!"
