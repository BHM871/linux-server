#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

PROCFILE="/proc/page_window_access"

echo "[+] Limpando logs antigos..."
rm -f $LOGDIR/*

echo "[+] Registrando estado inicial do módulo..."
cat $PROCFILE > "$LOGDIR/inicial_proc_dump.txt"

echo "[...] Rodando stress-ng..."
./run_stress_bench.sh

echo "[...] Rodando Redis benchmark..."
./run_redis_bench.sh

echo "[...] Rodando Memcached benchmark..."
./run_memcached_bench.sh

echo "[...] Rodando PostgreSQL benchmark..."
./run_postgres_bench.sh

echo "[...] Rodando Nginx benchmark..."
./run_nginx_bench.sh

echo "[+] Capturando logs finais do módulo..."
cat $PROCFILE > "$LOGDIR/final_proc_dump.txt"

echo "[✓] Todos benchmarks concluídos!"
echo "Resultados armazenados em $LOGDIR/"

