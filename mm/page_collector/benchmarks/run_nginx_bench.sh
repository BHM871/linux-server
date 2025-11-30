#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Iniciando nginx..."
sudo systemctl start nginx
sleep 1

echo "[+] Executando siege (HTTP load test)..."

sudo apt install -y siege

siege -c 50 -t 180s http://localhost \
    | tee "$LOGDIR/nginx_bench.log"

echo "[✓] Nginx benchmark finalizado!"

