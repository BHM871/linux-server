#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Iniciando PostgreSQL..."
sudo systemctl start postgresql
sleep 2

echo "[+] Preparando banco pgbench..."
sudo -u postgres pgbench -i -s 10 postgres

echo "[+] Executando pgbench..."
sudo -u postgres pgbench -c 10 -t 100000 postgres \
    | tee "$LOGDIR/postgres_bench.log"

echo "[✓] PostgreSQL benchmark finalizado!"

